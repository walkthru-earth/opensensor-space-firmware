#include "ota_lan.h"

#include "debug_log.h"
#include "wifi_link.h"

#include <ota_transfer.h>

#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <psa/crypto.h>
#include <sdkconfig.h>

#include <cstring>
#include <optional>

namespace ossync::ota {
namespace {
namespace rules = opensensor::ota;
namespace st = opensensor::selftest;

static_assert(rules::kOpBegin == ble::kOpOtaBegin &&
              rules::kOpWrite == ble::kOpOtaWrite &&
              rules::kOpEnd == ble::kOpOtaEnd &&
              rules::kOpCancel == ble::kOpOtaCancel);
static_assert(rules::kFrameState == ble::kFrameOtaState);
static_assert(rules::kMaxRequestBytes == ble::kMaxControlBytes);
// wire_error() codes, see ota_transfer.cpp.
static_assert(ble::kErrMalformed == 1 && ble::kErrBadHandle == 5 &&
              ble::kErrRange == 6 && ble::kErrBusy == 7 &&
              ble::kErrStorage == 8 && ble::kErrUnknownOp == 11 &&
              ble::kErrImageRejected == 18);

// Command worker only. One upload at a time.
struct Device {
  const esp_partition_t *slot = nullptr;
  esp_ota_handle_t handle = 0;
  bool open = false;
};
Device device;
// Read back buffer for the SHA-256, off the worker stack.
std::uint8_t readback[1024];

Device &of(void *context) { return *static_cast<Device *>(context); }

std::expected<void, rules::SinkError> check(esp_err_t err) {
  if (err != ESP_OK)
    return std::unexpected(rules::SinkError::Failed);
  return {};
}

const rules::Sink kSink{
    .context = &device,
    .begin =
        [](void *context, std::uint32_t) {
          auto &d = of(context);
          d.slot = esp_ota_get_next_update_partition(nullptr);
          if (!d.slot)
            return check(ESP_ERR_NOT_FOUND);
          // Sequential writes erase one sector at a time inside esp_ota_write,
          // so OTA_BEGIN never blocks the worker on a 6 MiB erase.
          const esp_err_t err =
              esp_ota_begin(d.slot, OTA_WITH_SEQUENTIAL_WRITES, &d.handle);
          d.open = err == ESP_OK;
          return check(err);
        },
    .write =
        [](void *context, std::span<const std::uint8_t> data) {
          auto &d = of(context);
          return check(d.open
                           ? esp_ota_write(d.handle, data.data(), data.size())
                           : ESP_ERR_INVALID_STATE);
        },
    .digest = [](void *context, std::uint32_t size)
        -> std::expected<rules::Digest, rules::SinkError> {
      auto &d = of(context);
      if (!d.slot || size > d.slot->size)
        return std::unexpected(rules::SinkError::Failed);
      psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
      if (psa_hash_setup(&operation, PSA_ALG_SHA_256) != PSA_SUCCESS)
        return std::unexpected(rules::SinkError::Failed);
      for (std::uint32_t offset = 0; offset < size;) {
        const std::uint32_t count =
            size - offset < sizeof(readback) ? size - offset : sizeof(readback);
        if (esp_partition_read(d.slot, offset, readback, count) != ESP_OK ||
            psa_hash_update(&operation, readback, count) != PSA_SUCCESS) {
          psa_hash_abort(&operation);
          return std::unexpected(rules::SinkError::Failed);
        }
        offset += count;
      }
      rules::Digest digest{};
      std::size_t length = 0;
      if (psa_hash_finish(&operation, digest.data(), digest.size(), &length) !=
              PSA_SUCCESS ||
          length != digest.size()) {
        psa_hash_abort(&operation);
        return std::unexpected(rules::SinkError::Failed);
      }
      return digest;
    },
    .finish =
        [](void *context) {
          auto &d = of(context);
          if (!d.open)
            return check(ESP_ERR_INVALID_STATE);
          // esp_ota_end frees the handle whatever it returns.
          d.open = false;
          return check(esp_ota_end(d.handle));
        },
    .activate =
        [](void *context) {
          auto &d = of(context);
          return check(d.slot ? esp_ota_set_boot_partition(d.slot)
                              : ESP_ERR_NOT_FOUND);
        },
    .revert =
        [](void *) {
          const esp_partition_t *running = esp_ota_get_running_partition();
          return check(running ? esp_ota_set_boot_partition(running)
                               : ESP_ERR_NOT_FOUND);
        },
    .abort =
        [](void *context) {
          auto &d = of(context);
          if (d.open)
            static_cast<void>(esp_ota_abort(d.handle));
          d.open = false;
        },
};

rules::Transfer &transfer() {
  static char project[sizeof(esp_app_desc_t::project_name)]{};
  static std::optional<rules::Transfer> instance;
  if (!instance) {
    const esp_app_desc_t *app = esp_app_get_description();
    std::size_t length = 0;
    if (app) {
      length = strnlen(app->project_name, sizeof(app->project_name));
      std::memcpy(project, app->project_name, length);
    }
    const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
    instance.emplace(
        rules::Target{.slot_bytes = slot ? std::uint32_t(slot->size) : 0U,
                      .chip_id = std::uint16_t(CONFIG_IDF_FIRMWARE_CHIP_ID),
                      .project_name = {project, length}},
        kSink);
  }
  return *instance;
}

rules::Owner owner_of(const ble::ControlRequest &request) {
  return rules::lan_owner(request.peer, request.link_generation);
}

// esp_timer time of the Receiving owner's last OTA request.
std::int64_t owner_seen_us = 0;

// A LAN session that went away or stalled mid upload frees the slot for the
// next one.
void drop_stale(rules::Transfer &upload, std::int64_t now_us) {
  const auto progress = upload.progress();
  if (progress.phase != rules::Phase::Receiving)
    return;
  const auto owner = upload.owner();
  const auto peer = rules::owner_peer(owner);
  const std::optional<std::uint32_t> generation =
      peer < ble::kMaxLanClients
          ? std::optional<std::uint32_t>(lan::connection_generation(peer))
          : std::nullopt;
  const auto keep =
      rules::stale_action(progress, owner, generation, now_us, owner_seen_us);
  if (keep == rules::Keep::Keep || !upload.drop_owner(owner))
    return;
  oslog.printf("OTA ABORT reason=%s\n",
               keep == rules::Keep::Stalled ? "stalled" : "session-closed");
}

const char *slot_name() { return device.slot ? device.slot->label : "none"; }
} // namespace

bool handle(const ble::ControlRequest &request, const ControlReplies &replies) {
  if (!request.length)
    return false;
  const auto op = static_cast<ble::Op>(request.bytes[0]);
  const auto route = rules::route(op, request.link == ble::Link::Lan);
  if (route == rules::Route::NotOta)
    return false;
  if (route == rules::Route::LanOnly) {
    replies.error(replies.context, request.link, op, ble::kErrNotOnThisLink,
                  "lan-only");
    return true;
  }
  auto &upload = transfer();
  const std::int64_t now_us = esp_timer_get_time();
  drop_stale(upload, now_us);
  const auto before = upload.progress();
  const auto owner = owner_of(request);
  const auto result = upload.handle({request.bytes, request.length}, owner);
  owner_seen_us = rules::touch_seen(upload.progress(), upload.owner(), owner,
                                    now_us, owner_seen_us);
  if (!result) {
    const auto wire = rules::wire_error(result.error());
    replies.error(replies.context, request.link, op,
                  static_cast<ble::Error>(wire.code), wire.detail);
    oslog.printf("OTA ERROR op=0x%02x reason=%s next=%lu size=%lu "
                 "active=%u\n",
                 unsigned(op), wire.detail ? wire.detail : "unknown-op",
                 static_cast<unsigned long>(before.next_offset),
                 static_cast<unsigned long>(before.size),
                 upload.active() ? 1U : 0U);
    return true;
  }
  std::uint8_t frame[rules::kStateBytes];
  replies.respond(replies.context, request.link, frame,
                  rules::encode_state(*result, frame));
  // One line per phase change. Chunks are not logged.
  if (op == ble::kOpOtaBegin)
    oslog.printf("OTA BEGIN size=%lu slot=%s slot_bytes=%lu\n",
                 static_cast<unsigned long>(result->size), slot_name(),
                 static_cast<unsigned long>(result->slot_bytes));
  else if (op == ble::kOpOtaEnd && before.phase != rules::Phase::Ready)
    oslog.printf("OTA READY size=%lu slot=%s sha256=match reboot=required\n",
                 static_cast<unsigned long>(result->size), slot_name());
  else if (op == ble::kOpOtaCancel && before.phase != rules::Phase::Idle)
    oslog.printf("OTA CANCEL from=%s\n",
                 before.phase == rules::Phase::Ready ? "ready" : "receiving");
  return true;
}

st::OtaState running_state() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  // ESP_ERR_NOT_FOUND when otadata holds no entry for this slot, as after a
  // USB flash that erased or never wrote otadata.
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK)
    return st::OtaState::Unknown;
  switch (state) {
  case ESP_OTA_IMG_NEW:
    return st::OtaState::New;
  case ESP_OTA_IMG_PENDING_VERIFY:
    return st::OtaState::PendingVerify;
  case ESP_OTA_IMG_VALID:
    return st::OtaState::Valid;
  case ESP_OTA_IMG_INVALID:
    return st::OtaState::Invalid;
  case ESP_OTA_IMG_ABORTED:
    return st::OtaState::Aborted;
  case ESP_OTA_IMG_UNDEFINED:
    return st::OtaState::Undefined;
  }
  return st::OtaState::Unknown;
}

st::Mark confirm_running_app(st::Verdict verdict, st::OtaState state) {
  if (!st::should_mark_valid(verdict, state))
    return st::Mark::Skipped;
  // Safe with rollback off. esp_ota_ops.c only rewrites the running slot's
  // otadata entry to ESP_OTA_IMG_VALID, which the bootloader never skips.
  return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK ? st::Mark::Marked
                                                            : st::Mark::Failed;
}

} // namespace ossync::ota
