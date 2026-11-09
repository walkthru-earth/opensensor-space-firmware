// Host tests for the LAN OTA rules in firmware/common/src/ota_transfer.h.

#include "ota_transfer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace ota = opensensor::ota;

namespace {
int failures = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "%s:%d CHECK failed: %s\n", __FILE__, __LINE__,     \
                   #condition);                                                \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

constexpr std::string_view kProject = "opensensor_waveshare";
constexpr std::uint16_t kChip = 0x0009;
constexpr std::uint32_t kSlot = 0x600000;

struct FakeFlash {
  std::vector<std::uint8_t> written;
  ota::Digest digest{};
  bool fail_begin = false;
  bool fail_write = false;
  bool fail_digest = false;
  bool fail_finish = false;
  bool fail_activate = false;
  bool fail_revert = false;
  int begins = 0;
  int writes = 0;
  int finishes = 0;
  int digests = 0;
  int activates = 0;
  int reverts = 0;
  int aborts = 0;
  std::uint32_t begun_size = 0;
};

FakeFlash &flash_of(void *context) {
  return *static_cast<FakeFlash *>(context);
}

std::expected<void, ota::SinkError> fail_if(bool fail) {
  if (fail)
    return std::unexpected(ota::SinkError::Failed);
  return {};
}

ota::Sink sink_for(FakeFlash &flash) {
  return {
      .context = &flash,
      .begin =
          [](void *context, std::uint32_t size) {
            auto &f = flash_of(context);
            ++f.begins;
            f.begun_size = size;
            f.written.clear();
            return fail_if(f.fail_begin);
          },
      .write =
          [](void *context, std::span<const std::uint8_t> data) {
            auto &f = flash_of(context);
            ++f.writes;
            if (f.fail_write)
              return fail_if(true);
            f.written.insert(f.written.end(), data.begin(), data.end());
            return fail_if(false);
          },
      .digest = [](void *context, std::uint32_t size)
          -> std::expected<ota::Digest, ota::SinkError> {
        auto &f = flash_of(context);
        ++f.digests;
        if (f.fail_digest || size != f.written.size())
          return std::unexpected(ota::SinkError::Failed);
        return f.digest;
      },
      .finish =
          [](void *context) {
            auto &f = flash_of(context);
            ++f.finishes;
            return fail_if(f.fail_finish);
          },
      .activate =
          [](void *context) {
            auto &f = flash_of(context);
            ++f.activates;
            return fail_if(f.fail_activate);
          },
      .revert =
          [](void *context) {
            auto &f = flash_of(context);
            ++f.reverts;
            return fail_if(f.fail_revert);
          },
      .abort = [](void *context) { ++flash_of(context).aborts; },
  };
}

ota::Target target() {
  return {.slot_bytes = kSlot,
          .chip_id = kChip,
          .project_name = {kProject.data(), kProject.size()}};
}

// A fake app image. The digest is a stand in, the state machine only compares
// what the sink reads back with what OTA_BEGIN declared.
std::vector<std::uint8_t> image(std::size_t size,
                                std::string_view project = kProject,
                                std::uint16_t chip = kChip) {
  std::vector<std::uint8_t> bytes(size);
  for (std::size_t i = 0; i < size; ++i)
    bytes[i] = static_cast<std::uint8_t>(i * 7 + 3);
  bytes[0] = ota::kImageMagic;
  bytes[ota::kChipIdOffset] = static_cast<std::uint8_t>(chip & 0xff);
  bytes[ota::kChipIdOffset + 1] = static_cast<std::uint8_t>(chip >> 8);
  const std::uint32_t magic = ota::kAppDescMagic;
  for (unsigned i = 0; i < 4; ++i)
    bytes[ota::kAppDescOffset + i] =
        static_cast<std::uint8_t>((magic >> (8 * i)) & 0xff);
  std::memset(bytes.data() + ota::kProjectNameOffset, 0,
              ota::kProjectNameBytes);
  std::memcpy(bytes.data() + ota::kProjectNameOffset, project.data(),
              project.size());
  return bytes;
}

ota::Digest digest_a() {
  ota::Digest d{};
  for (std::size_t i = 0; i < d.size(); ++i)
    d[i] = static_cast<std::uint8_t>(0xa0 + i);
  return d;
}

std::vector<std::uint8_t> begin_request(std::uint32_t size,
                                        const ota::Digest &digest) {
  std::vector<std::uint8_t> r{ota::kOpBegin};
  for (unsigned i = 0; i < 4; ++i)
    r.push_back(static_cast<std::uint8_t>((size >> (8 * i)) & 0xff));
  r.insert(r.end(), digest.begin(), digest.end());
  return r;
}

std::vector<std::uint8_t> write_request(std::uint32_t offset,
                                        std::span<const std::uint8_t> data) {
  std::vector<std::uint8_t> r{ota::kOpWrite};
  for (unsigned i = 0; i < 4; ++i)
    r.push_back(static_cast<std::uint8_t>((offset >> (8 * i)) & 0xff));
  r.insert(r.end(), data.begin(), data.end());
  return r;
}

const std::vector<std::uint8_t> kEnd{ota::kOpEnd};
const std::vector<std::uint8_t> kCancel{ota::kOpCancel};
constexpr ota::Owner kOwner = (ota::Owner{1} << 32) | 7;
constexpr ota::Owner kOther = (ota::Owner{2} << 32) | 9;

bool is_error(const std::expected<ota::Progress, ota::Error> &result,
              ota::Error error) {
  return !result && result.error() == error;
}

// Sends the whole image in kMaxChunk slices. Returns the last result.
std::expected<ota::Progress, ota::Error>
send_all(ota::Transfer &transfer, const std::vector<std::uint8_t> &bytes,
         ota::Owner owner = kOwner) {
  std::expected<ota::Progress, ota::Error> last = transfer.progress();
  for (std::size_t offset = 0; offset < bytes.size();
       offset += ota::kMaxChunk) {
    const auto count = std::min(ota::kMaxChunk, bytes.size() - offset);
    last = transfer.handle(write_request(static_cast<std::uint32_t>(offset),
                                         {bytes.data() + offset, count}),
                           owner);
    if (!last)
      return last;
  }
  return last;
}

void test_happy_path() {
  FakeFlash flash;
  flash.digest = digest_a();
  ota::Transfer transfer(target(), sink_for(flash));
  const auto bytes = image(3 * ota::kMaxChunk + 17);
  const auto begun = transfer.handle(
      begin_request(static_cast<std::uint32_t>(bytes.size()), digest_a()),
      kOwner);
  CHECK(begun && begun->phase == ota::Phase::Receiving &&
        begun->next_offset == 0 && begun->size == bytes.size() &&
        begun->slot_bytes == kSlot);
  CHECK(flash.begins == 1 && flash.begun_size == bytes.size());
  const auto sent = send_all(transfer, bytes);
  CHECK(sent && sent->next_offset == bytes.size());
  CHECK(flash.written == bytes);
  const auto done = transfer.handle(kEnd, kOwner);
  CHECK(done && done->phase == ota::Phase::Ready);
  CHECK(flash.finishes == 1 && flash.digests == 1 && flash.activates == 1);
  CHECK(flash.aborts == 0);
  // OTA_END again is idempotent and does not activate twice.
  const auto again = transfer.handle(kEnd, kOwner);
  CHECK(again && again->phase == ota::Phase::Ready && flash.activates == 1);
}

void test_begin_rules() {
  FakeFlash flash;
  ota::Transfer transfer(target(), sink_for(flash));
  CHECK(is_error(transfer.handle(begin_request(0, digest_a()), kOwner),
                 ota::Error::Size));
  CHECK(is_error(
      transfer.handle(begin_request(ota::kHeaderBytes - 1, digest_a()), kOwner),
      ota::Error::Size));
  CHECK(is_error(transfer.handle(begin_request(kSlot + 1, digest_a()), kOwner),
                 ota::Error::Size));
  CHECK(flash.begins == 0);
  // The slot size itself is allowed.
  CHECK(transfer.handle(begin_request(kSlot, digest_a()), kOwner).has_value());
  CHECK(flash.begins == 1);
  CHECK(transfer.handle(kCancel, kOwner).has_value());
  CHECK(!transfer.active() && flash.aborts == 1);
  // Missing digest bytes.
  auto short_begin = begin_request(1000, digest_a());
  short_begin.pop_back();
  CHECK(is_error(transfer.handle(short_begin, kOwner), ota::Error::Malformed));
  // No inactive slot.
  FakeFlash none;
  ota::Target no_slot = target();
  no_slot.slot_bytes = 0;
  ota::Transfer empty(no_slot, sink_for(none));
  CHECK(is_error(empty.handle(begin_request(1000, digest_a()), kOwner),
                 ota::Error::NoTarget));
  // esp_ota_begin failing leaves the machine idle.
  FakeFlash broken;
  broken.fail_begin = true;
  ota::Transfer refused(target(), sink_for(broken));
  CHECK(is_error(refused.handle(begin_request(1000, digest_a()), kOwner),
                 ota::Error::Flash));
  CHECK(!refused.active());
}

// Starts an upload of `bytes` and returns the transfer ready for chunks.
void start(ota::Transfer &transfer, const std::vector<std::uint8_t> &bytes) {
  CHECK(transfer
            .handle(begin_request(static_cast<std::uint32_t>(bytes.size()),
                                  digest_a()),
                    kOwner)
            .has_value());
}

void test_order_rules() {
  const auto bytes = image(2 * ota::kMaxChunk);
  {
    FakeFlash flash;
    ota::Transfer transfer(target(), sink_for(flash));
    CHECK(is_error(transfer.handle(write_request(0, bytes), kOwner),
                   ota::Error::NotActive));
    CHECK(is_error(transfer.handle(kEnd, kOwner), ota::Error::NotActive));
  }
  {
    // A gap aborts.
    FakeFlash flash;
    ota::Transfer transfer(target(), sink_for(flash));
    start(transfer, bytes);
    CHECK(transfer
              .handle(write_request(0, {bytes.data(), ota::kMaxChunk}), kOwner)
              .has_value());
    CHECK(is_error(
        transfer.handle(
            write_request(ota::kMaxChunk + 1, {bytes.data() + 1, 10}), kOwner),
        ota::Error::Offset));
    CHECK(!transfer.active() && flash.aborts == 1);
    CHECK(is_error(transfer.handle(kEnd, kOwner), ota::Error::NotActive));
  }
  {
    // An overlap, the same chunk sent twice, aborts.
    FakeFlash flash;
    ota::Transfer transfer(target(), sink_for(flash));
    start(transfer, bytes);
    const auto first = write_request(0, {bytes.data(), ota::kMaxChunk});
    CHECK(transfer.handle(first, kOwner).has_value());
    CHECK(is_error(transfer.handle(first, kOwner), ota::Error::Offset));
    CHECK(!transfer.active() && flash.writes == 1);
  }
  {
    // A chunk past the declared size aborts and never reaches flash.
    FakeFlash flash;
    const auto small = image(ota::kHeaderBytes + 10);
    ota::Transfer transfer(target(), sink_for(flash));
    start(transfer, small);
    auto longer = small;
    longer.push_back(0);
    CHECK(is_error(transfer.handle(write_request(0, longer), kOwner),
                   ota::Error::Overrun));
    CHECK(!transfer.active() && flash.writes == 0);
  }
  {
    // An empty write is malformed.
    FakeFlash flash;
    ota::Transfer transfer(target(), sink_for(flash));
    start(transfer, bytes);
    CHECK(is_error(transfer.handle(write_request(0, {}), kOwner),
                   ota::Error::Malformed));
    CHECK(!transfer.active());
  }
  {
    // OTA_END before the last byte aborts.
    FakeFlash flash;
    ota::Transfer transfer(target(), sink_for(flash));
    start(transfer, bytes);
    CHECK(transfer
              .handle(write_request(0, {bytes.data(), ota::kMaxChunk}), kOwner)
              .has_value());
    CHECK(is_error(transfer.handle(kEnd, kOwner), ota::Error::Incomplete));
    CHECK(!transfer.active() && flash.finishes == 0 && flash.activates == 0);
  }
}

void expect_header_rejected(const std::vector<std::uint8_t> &bytes,
                            ota::Error error) {
  FakeFlash flash;
  ota::Transfer transfer(target(), sink_for(flash));
  start(transfer, bytes);
  const auto count = std::min(ota::kMaxChunk, bytes.size());
  CHECK(is_error(
      transfer.handle(write_request(0, {bytes.data(), count}), kOwner), error));
  CHECK(!transfer.active() && flash.writes == 0 && flash.aborts == 1);
}

void test_header_rules() {
  const std::size_t size = 2 * ota::kMaxChunk;
  // A CoreS3 image never lands on a Waveshare board.
  expect_header_rejected(image(size, "opensensor_cores3"), ota::Error::Project);
  // A prefix or a longer name is another project.
  expect_header_rejected(image(size, "opensensor_wave"), ota::Error::Project);
  expect_header_rejected(image(size, "opensensor_waveshare2"),
                         ota::Error::Project);
  // An ESP32 (chip id 0) image.
  expect_header_rejected(image(size, kProject, 0x0000), ota::Error::Chip);
  auto bad_magic = image(size);
  bad_magic[0] = 0xE8;
  expect_header_rejected(bad_magic, ota::Error::Header);
  auto bad_desc = image(size);
  bad_desc[ota::kAppDescOffset] ^= 1;
  expect_header_rejected(bad_desc, ota::Error::Header);
  {
    // A first chunk too short to hold the header.
    FakeFlash flash;
    const auto bytes = image(size);
    ota::Transfer transfer(target(), sink_for(flash));
    start(transfer, bytes);
    CHECK(is_error(
        transfer.handle(write_request(0, {bytes.data(), ota::kHeaderBytes - 1}),
                        kOwner),
        ota::Error::Header));
    CHECK(!transfer.active() && flash.writes == 0);
  }
  {
    // The header is checked once. Later chunks may hold any bytes.
    FakeFlash flash;
    flash.digest = digest_a();
    auto bytes = image(size);
    bytes[ota::kMaxChunk] = 0;
    ota::Transfer transfer(target(), sink_for(flash));
    start(transfer, bytes);
    CHECK(send_all(transfer, bytes).has_value());
  }
}

void test_finish_rules() {
  const auto bytes = image(ota::kMaxChunk + 40);
  {
    FakeFlash flash;
    flash.digest = digest_a();
    flash.digest[31] ^= 1;
    ota::Transfer transfer(target(), sink_for(flash));
    start(transfer, bytes);
    CHECK(send_all(transfer, bytes).has_value());
    CHECK(is_error(transfer.handle(kEnd, kOwner), ota::Error::Digest));
    CHECK(flash.activates == 0 && !transfer.active() && flash.aborts == 1);
  }
  {
    FakeFlash flash;
    flash.digest = digest_a();
    flash.fail_finish = true;
    ota::Transfer transfer(target(), sink_for(flash));
    start(transfer, bytes);
    CHECK(send_all(transfer, bytes).has_value());
    CHECK(is_error(transfer.handle(kEnd, kOwner), ota::Error::Verify));
    CHECK(flash.digests == 0 && flash.activates == 0 && !transfer.active());
  }
  {
    FakeFlash flash;
    flash.digest = digest_a();
    flash.fail_digest = true;
    ota::Transfer transfer(target(), sink_for(flash));
    start(transfer, bytes);
    CHECK(send_all(transfer, bytes).has_value());
    CHECK(is_error(transfer.handle(kEnd, kOwner), ota::Error::Flash));
    CHECK(flash.activates == 0 && !transfer.active());
  }
  {
    FakeFlash flash;
    flash.digest = digest_a();
    flash.fail_activate = true;
    ota::Transfer transfer(target(), sink_for(flash));
    start(transfer, bytes);
    CHECK(send_all(transfer, bytes).has_value());
    CHECK(is_error(transfer.handle(kEnd, kOwner), ota::Error::Activate));
    CHECK(!transfer.active());
  }
  {
    FakeFlash flash;
    flash.fail_write = true;
    ota::Transfer transfer(target(), sink_for(flash));
    start(transfer, bytes);
    CHECK(is_error(send_all(transfer, bytes), ota::Error::Flash));
    CHECK(!transfer.active() && flash.aborts == 1);
  }
  {
    // OTA_END carries no body.
    FakeFlash flash;
    flash.digest = digest_a();
    ota::Transfer transfer(target(), sink_for(flash));
    start(transfer, bytes);
    CHECK(send_all(transfer, bytes).has_value());
    CHECK(is_error(
        transfer.handle(std::vector<std::uint8_t>{ota::kOpEnd, 0}, kOwner),
        ota::Error::Malformed));
    CHECK(!transfer.active() && flash.activates == 0);
  }
}

void test_ownership() {
  const auto bytes = image(2 * ota::kMaxChunk);
  FakeFlash flash;
  flash.digest = digest_a();
  ota::Transfer transfer(target(), sink_for(flash));
  start(transfer, bytes);
  const auto first = write_request(0, {bytes.data(), ota::kMaxChunk});
  // Another session can neither write, end, cancel nor restart it.
  CHECK(is_error(transfer.handle(first, kOther), ota::Error::Busy));
  CHECK(is_error(transfer.handle(kEnd, kOther), ota::Error::Busy));
  CHECK(is_error(transfer.handle(kCancel, kOther), ota::Error::Busy));
  CHECK(is_error(transfer.handle(begin_request(100000, digest_a()), kOther),
                 ota::Error::Busy));
  CHECK(transfer.active() && transfer.owner() == kOwner && flash.aborts == 0);
  CHECK(transfer.handle(first, kOwner).has_value());
  // A dropped owner frees the slot for a new session.
  CHECK(!transfer.drop_owner(kOther));
  CHECK(transfer.drop_owner(kOwner));
  CHECK(!transfer.active() && flash.aborts == 1);
  start(transfer, bytes);
  // The owner may restart, which closes the old handle first.
  CHECK(transfer.handle(first, kOwner).has_value());
  start(transfer, bytes);
  CHECK(flash.aborts == 2 && transfer.progress().next_offset == 0);
  CHECK(send_all(transfer, bytes).has_value());
  CHECK(transfer.handle(kEnd, kOwner).has_value());
  // A staged image blocks a new upload until REBOOT or OTA_CANCEL.
  CHECK(is_error(transfer.handle(begin_request(1000, digest_a()), kOwner),
                 ota::Error::RebootPending));
  CHECK(is_error(transfer.handle(first, kOwner), ota::Error::RebootPending));
  CHECK(!transfer.drop_owner(kOwner));
  // Any session may cancel a staged image. It restores the running slot.
  const auto cancelled = transfer.handle(kCancel, kOther);
  CHECK(cancelled && cancelled->phase == ota::Phase::Idle &&
        flash.reverts == 1);
  // Cancel when idle is a no-op.
  CHECK(transfer.handle(kCancel, kOther).has_value() && flash.aborts == 2);
  // A failed revert keeps the image staged.
  start(transfer, bytes);
  CHECK(send_all(transfer, bytes).has_value());
  CHECK(transfer.handle(kEnd, kOwner).has_value());
  flash.fail_revert = true;
  CHECK(is_error(transfer.handle(kCancel, kOwner), ota::Error::Activate));
  CHECK(transfer.progress().phase == ota::Phase::Ready);
}

void test_wire() {
  FakeFlash flash;
  ota::Transfer transfer(target(), sink_for(flash));
  CHECK(is_error(transfer.handle(std::vector<std::uint8_t>{0x11}, kOwner),
                 ota::Error::UnknownOp));
  CHECK(is_error(transfer.handle(std::vector<std::uint8_t>{}, kOwner),
                 ota::Error::Malformed));
  std::array<std::uint8_t, ota::kStateBytes> frame{};
  const ota::Progress progress{.phase = ota::Phase::Receiving,
                               .next_offset = 0x01020304,
                               .size = 0x0a0b0c0d,
                               .slot_bytes = kSlot};
  CHECK(ota::encode_state(progress, frame) == 16);
  const std::array<std::uint8_t, 16> expected{
      0x50, 1,    0x04, 0x03, 0x02, 0x01, 0x0d, 0x0c,
      0x0b, 0x0a, 0x00, 0x00, 0x60, 0x00, 0xfb, 0x01};
  CHECK(frame == expected);
  CHECK(ota::kMaxChunk == 507 && ota::kHeaderBytes == 112);
  CHECK(ota::wire_error(ota::Error::Project).code == 18);
  CHECK(std::string_view(ota::wire_error(ota::Error::Project).detail) ==
        "project");
  CHECK(ota::wire_error(ota::Error::Offset).code == 6);
  CHECK(ota::wire_error(ota::Error::Busy).code == 7);
  CHECK(ota::wire_error(ota::Error::NotActive).code == 5);
  CHECK(ota::wire_error(ota::Error::Flash).code == 8);
  CHECK(ota::wire_error(ota::Error::UnknownOp).code == 11);
}

void test_routing() {
  // Every OTA op is LAN only. BLE never reaches the transfer.
  for (const std::uint8_t op :
       {ota::kOpBegin, ota::kOpWrite, ota::kOpEnd, ota::kOpCancel}) {
    CHECK(ota::route(op, true) == ota::Route::Handle);
    CHECK(ota::route(op, false) == ota::Route::LanOnly);
  }
  // Neighbours stay with the common control handler on both links.
  for (const std::uint8_t op : {0x11, 0x16, 0x0B, 0x50}) {
    CHECK(ota::route(op, true) == ota::Route::NotOta);
    CHECK(ota::route(op, false) == ota::Route::NotOta);
  }
}

void test_stall() {
  CHECK(ota::keep_owner(true, 0) == ota::Keep::Keep);
  CHECK(ota::keep_owner(true, ota::kStallMs - 1) == ota::Keep::Keep);
  CHECK(ota::keep_owner(true, ota::kStallMs) == ota::Keep::Stalled);
  // A closed session loses the upload at once, however recent.
  CHECK(ota::keep_owner(false, 0) == ota::Keep::SessionClosed);
  CHECK(ota::keep_owner(false, ota::kStallMs) == ota::Keep::SessionClosed);
  // The stall bound stays under the 300 s LAN idle close.
  static_assert(ota::kStallMs < 300000);
  // Dropping a stalled owner aborts and frees the slot for another session.
  FakeFlash flash;
  ota::Transfer transfer(target(), sink_for(flash));
  CHECK(transfer.handle(begin_request(4096, digest_a()), kOwner).has_value());
  CHECK(transfer.drop_owner(kOwner));
  CHECK(!transfer.active() && flash.aborts == 1);
  CHECK(
      transfer.handle(begin_request(4096, digest_a()), kOwner + 1).has_value());
}
// The drop_stale() and handle() glue in ota_lan.cpp, decided by stale_action()
// and touch_seen().
void test_stale_owner() {
  constexpr std::uint8_t kPeer = 2;
  constexpr std::uint32_t kGen = 9;
  constexpr ota::Owner owner = ota::lan_owner(kPeer, kGen);
  static_assert(owner == ((ota::Owner{kPeer} << 32) | kGen));
  static_assert(ota::owner_peer(owner) == kPeer);
  static_assert(ota::owner_peer(ota::lan_owner(255, 0xFFFFFFFF)) == 255);
  constexpr ota::Progress receiving{.phase = ota::Phase::Receiving};
  constexpr std::int64_t seen = 5'000'000;
  constexpr std::int64_t stall_us = ota::kStallMs * 1000;

  // Open session, recent request, keeps the upload.
  CHECK(ota::stale_action(receiving, owner, kGen, seen, seen) ==
        ota::Keep::Keep);
  CHECK(ota::stale_action(receiving, owner, kGen, seen + stall_us - 1, seen) ==
        ota::Keep::Keep);
  // Idle time is now minus the owner's last request, not now alone.
  CHECK(ota::stale_action(receiving, owner, kGen, seen + stall_us, seen) ==
        ota::Keep::Stalled);
  CHECK(ota::stale_action(receiving, owner, kGen, stall_us + 1, 0) ==
        ota::Keep::Stalled);
  // A new generation on the slot means the owner's session closed, even when
  // the owner was seen a moment ago.
  CHECK(ota::stale_action(receiving, owner, kGen + 1, seen, seen) ==
        ota::Keep::SessionClosed);
  CHECK(ota::stale_action(receiving, owner, kPeer, seen, seen) ==
        ota::Keep::SessionClosed);
  CHECK(ota::stale_action(receiving, owner, std::nullopt, seen, seen) ==
        ota::Keep::SessionClosed);
  // Idle and a staged image are never dropped as stale.
  for (const auto phase : {ota::Phase::Idle, ota::Phase::Ready}) {
    const ota::Progress still{.phase = phase};
    CHECK(ota::stale_action(still, owner, std::nullopt, seen + stall_us, 0) ==
          ota::Keep::Keep);
  }

  // owner_generation() is the slot lookup drop_stale() runs. It asks for the
  // owner's own slot and returns what the slot holds now, so a reconnect on
  // that slot closes the owner's session. A slot past the table is never
  // looked up.
  constexpr std::uint8_t kSlots = 3;
  int lookups = 0;
  std::uint8_t asked = 0xFF;
  const auto slot_now = [&](std::uint32_t now_gen) {
    return [&lookups, &asked, now_gen](std::uint8_t peer) {
      ++lookups;
      asked = peer;
      return now_gen;
    };
  };
  CHECK(ota::owner_generation(owner, kSlots, slot_now(kGen)) ==
        std::optional<std::uint32_t>(kGen));
  CHECK(lookups == 1 && asked == kPeer);
  CHECK(ota::owner_generation(owner, kSlots, slot_now(kGen + 1)) ==
        std::optional<std::uint32_t>(kGen + 1));
  CHECK(ota::stale_action(receiving, owner,
                          ota::owner_generation(owner, kSlots, slot_now(kGen)),
                          seen, seen) == ota::Keep::Keep);
  CHECK(ota::stale_action(
            receiving, owner,
            ota::owner_generation(owner, kSlots, slot_now(kGen + 1)), seen,
            seen) == ota::Keep::SessionClosed);
  lookups = 0;
  CHECK(!ota::owner_generation(ota::lan_owner(kSlots, kGen), kSlots,
                               slot_now(kGen))
             .has_value());
  CHECK(
      !ota::owner_generation(ota::lan_owner(255, kGen), kSlots, slot_now(kGen))
           .has_value());
  CHECK(lookups == 0);
  CHECK(ota::owner_generation(ota::lan_owner(kSlots - 1, kGen), kSlots,
                              slot_now(kGen + 2)) ==
        std::optional<std::uint32_t>(kGen + 2));
  CHECK(lookups == 1 && asked == kSlots - 1);

  // Only the requester that owns a Receiving upload moves the seen time.
  constexpr ota::Owner other = ota::lan_owner(kPeer + 1, kGen);
  CHECK(ota::touch_seen(receiving, owner, owner, seen + 7, seen) == seen + 7);
  CHECK(ota::touch_seen(receiving, owner, other, seen + 7, seen) == seen);
  CHECK(ota::touch_seen({.phase = ota::Phase::Idle}, owner, owner, seen + 7,
                        seen) == seen);
  CHECK(ota::touch_seen({.phase = ota::Phase::Ready}, owner, owner, seen + 7,
                        seen) == seen);

  // Through a Transfer, as handle() runs it. BEGIN takes the upload and
  // touches. A busy second session does not touch, so the owner still stalls
  // on time. Each owner request pushes the stall out again.
  FakeFlash flash;
  ota::Transfer transfer(target(), sink_for(flash));
  std::int64_t seen_us = 0;
  std::int64_t now_us = 1'000'000;
  CHECK(transfer.handle(begin_request(4096, digest_a()), owner).has_value());
  seen_us = ota::touch_seen(transfer.progress(), transfer.owner(), owner,
                            now_us, seen_us);
  CHECK(seen_us == now_us);
  now_us += stall_us - 1;
  CHECK(!transfer.handle(begin_request(4096, digest_a()), other).has_value());
  seen_us = ota::touch_seen(transfer.progress(), transfer.owner(), other,
                            now_us, seen_us);
  CHECK(seen_us == 1'000'000);
  CHECK(ota::stale_action(transfer.progress(), transfer.owner(), kGen,
                          now_us + 1, seen_us) == ota::Keep::Stalled);
  CHECK(transfer.handle(begin_request(4096, digest_a()), owner).has_value());
  seen_us = ota::touch_seen(transfer.progress(), transfer.owner(), owner,
                            now_us, seen_us);
  CHECK(ota::stale_action(transfer.progress(), transfer.owner(), kGen,
                          now_us + 1, seen_us) == ota::Keep::Keep);
}
} // namespace

int main() {
  test_happy_path();
  test_begin_rules();
  test_order_rules();
  test_header_rules();
  test_finish_rules();
  test_ownership();
  test_wire();
  test_routing();
  test_stall();
  test_stale_owner();
  if (failures) {
    std::fprintf(stderr, "%d OTA transfer check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
