#include "ble_sync.h"
#include "abort_signal.h"
#include "ble_advert.h"
#include "ble_link_auth.h"
#include "ble_policy.h"
#include "debug_log.h"
#include "disconnect.h"

#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

extern "C" {
#include <host/ble_hs.h>
#include <host/ble_store.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>
void ble_store_config_init(void);
}

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>

namespace ble {
namespace {
static_assert(BLE_HS_ERR_HCI_BASE == kHciStatusBase, "NimBLE HCI status base");
constexpr const char *kServiceUuid = "255f595e-0001-47ed-8e0d-59994199b680";
constexpr std::int64_t kPairingWindowUs = 60LL * 1000000LL;
// send_response() from tasks other than the worker keeps the prior 4 s budget.
constexpr std::int64_t kSendBudgetUs = 4LL * 1000000LL;
// Native NimBLE UUID bytes use the same Bluetooth wire order as ADV.
ble_uuid128_t native_uuid(std::uint16_t selector) {
  ble_uuid128_t result{};
  result.u.type = BLE_UUID_TYPE_128;
  const auto bytes = wire::uuid(selector);
  std::memcpy(result.value, bytes.data(), bytes.size());
  return result;
}
const ble_uuid128_t uuids[] = {native_uuid(1), native_uuid(2), native_uuid(3),
                               native_uuid(4), native_uuid(5), native_uuid(6)};
enum Characteristic : unsigned { Info, Status, Live, Control, Response, Count };
static_assert(Count <= PendingCccd::kMax, "every CCCD restore must fit");
char name_text[16]{};
char info_text[kMaxInfo]{};
char status_text[kMaxJson + 1] = "{}", live_text[kMaxJson + 1] = "{}";
std::size_t status_length = 2, live_length = 2;
portMUX_TYPE snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
ble_gatt_chr_def characteristics[Count + 1]{};
ble_gatt_svc_def services[2]{};
std::uint16_t value_handles[Count]{};
std::atomic<bool> subscribed[Count]{};
std::atomic<bool> enabled{false}, advertising{false}, connected{false},
    authenticated{false}, pairing_active{false};
std::atomic<std::uint16_t> conn_handle{BLE_HS_CONN_HANDLE_NONE}, mtu{0};
std::atomic<std::uint32_t> passkey{0}, bonds{0}, generation{0}, ui{0};
std::atomic<std::int64_t> pairing_deadline{0};
// SUBSCRIBE events for a handle whose CONNECT has not run yet, applied at
// CONNECT (ble_link_auth.h). Host task only.
PendingCccd early_cccd;
// The last session a fault refusal was logged for, so it logs once.
std::atomic<std::uint32_t> refusal_logged{~0U};
// Why this side asked the controller to terminate. The controller reports a
// local termination as HCI 0x16, which carries no reason of its own.
std::atomic<DisconnectReason> pending_reason{DisconnectReason::Unknown};
CloseClaim close_claim;

// A terminate that did not start puts the previous reason back, so neither a
// failure nor a second request while one is pending (BLE_HS_EALREADY, say a
// slow peer close during a reboot close) relabels the close. The restore only
// happens if DISCONNECT has not consumed the reason meanwhile.
CloseStart terminate(std::uint16_t handle, DisconnectReason reason) {
  const auto previous = pending_reason.exchange(reason);
  const auto start = classify_close(
      ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM), BLE_HS_EALREADY);
  if (start != CloseStart::Started) {
    auto expected = reason;
    pending_reason.compare_exchange_strong(expected, previous);
  }
  return start;
}
config::PairMode mode = config::PairMode::Random;
std::uint32_t fixed_passkey = 0;
// Only the host writes applied_advert. Publishers update one atomic word so
// flags/counter cannot tear, and the host event coalesces concurrent updates.
std::atomic<std::uint64_t> desired_advert{0}, applied_advert{~0ULL};
std::uint16_t boot16 = 0;
RequestHandler request_handler = nullptr;
TaskHandle_t host_task_handle = nullptr;
struct ImmediateError {
  Op op;
  Error code;
  const char *detail; // Only constant, secret-free protocol identifiers.
  std::uint32_t generation;
};
QueueHandle_t immediate_errors = nullptr;
SemaphoreHandle_t startup_done = nullptr, response_mutex = nullptr,
                  clear_mutex = nullptr, clear_done = nullptr,
                  space_event = nullptr;
ble_npl_event advert_event{}, clear_event{}, response_event{}, snapshot_event{},
    wake_event{};
Options options;

// Response transmit queue ("Link parameters and notify flow" in the protocol
// doc). One slot per notify credit. A producer, serialised by response_mutex,
// reserves a slot, fills its bytes outside the lock and then publishes it. The
// host task alone sends and pops the head. tx_lock guards `responses`.
portMUX_TYPE tx_lock = portMUX_INITIALIZER_UNLOCKED;
ResponseQueue responses;
std::uint8_t *tx_frames = nullptr; // capacity x kMaxFrame
// Host task only: the NOTIFY_TX status for the frame being submitted.
bool awaiting_tx = false, notify_tx_seen = false;
int notify_tx_status = 0;
StallClose stall;     // host task only
FlagWatch flag_watch; // host task only
std::atomic<bool> advert_published{false};
ble_npl_callout retry_callout{}, relax_callout{}, advertise_callout{},
    link_callout{};
static_assert(kPhy2M == BLE_GAP_LE_PHY_2M);
// Where the post-connect link sequence is, see link_on_host().
LinkStep link_step = LinkStep::Idle; // host task only
// The last DATA_LEN_CHG max_tx_octets on this link, 0 for none. Host task.
std::uint16_t dle_tx_octets = 0;
HciFailLog conn_fail_log; // host task only
AdvertWindow adv_window;  // host task only
IntervalPolicy interval;  // host task only
std::atomic<bool> adv_fast{false};
std::atomic<unsigned> pending_snapshots{0};
std::atomic<bool> startup_ok{false}, host_requested{false};

void touch_ui() { ++ui; }

std::uint64_t pack_advert(const AdvertState &state) {
  return static_cast<std::uint64_t>(state.flags) |
         (static_cast<std::uint64_t>(state.finalized) << 8);
}

void update_bonds() {
  ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
  int count = 0;
  if (ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS) ==
      0)
    bonds = static_cast<std::uint32_t>(count);
}

bool secure(std::uint16_t handle) {
  ble_gap_conn_desc desc{};
  return ble_gap_conn_find(handle, &desc) == 0 && desc.sec_state.encrypted &&
         (mode == config::PairMode::None ||
          (desc.sec_state.authenticated && desc.sec_state.key_size == 16));
}

// The response gates (ble_link_auth.h). The cached flag, else the ERROR 14
// refusal, else NimBLE's live state, so a missed ENC_CHANGE never refuses a
// secure link. Safe from any task: ble_gap_conn_find() takes the host lock.
bool may_send_frame(std::uint16_t handle, const std::uint8_t *data,
                    std::size_t length) {
  return may_send(authenticated.load(),
                  std::span<const std::uint8_t>(data, length),
                  [handle] { return secure(handle); });
}

// The response stream gate for one frame of `session` on `handle`. A fault
// refusal (not subscribed, not secure) logs BLE REFUSE once per session, so
// the bench sees why before the worker's ERROR and LIST ABORT lines.
[[nodiscard]] GateRefusal response_gate(std::uint32_t session,
                                        std::uint16_t handle,
                                        const std::uint8_t *data,
                                        std::size_t length) {
  const auto refusal = response_refusal(
      connected.load(),
      generation.load() == session && conn_handle.load() == handle,
      subscribed[Response].load(),
      [&] { return may_send_frame(handle, data, length); });
  if (refusal_is_fault(refusal) && refusal_logged.exchange(session) != session)
    oslog.printf("BLE REFUSE reason=%s\n", refusal_name(refusal));
  return refusal;
}

bool apply_advert_data() {
  const std::uint64_t word = desired_advert.load();
  const auto data =
      wire::legacy_advert(static_cast<std::uint8_t>(word),
                          static_cast<std::uint32_t>(word >> 8), boot16);
  if (ble_gap_adv_set_data(data.data(), data.size()) != 0)
    return false;
  applied_advert = word;
  return true;
}

int gap_event(ble_gap_event *event, void *);
bool start_advertising();
void speed_up_advertising();

// Runs for every publish_advert() that changed the word, and for the first
// one, which sets the flag baseline even when it matches the placeholder.
void refresh_advert(ble_npl_event *) {
  if (desired_advert.load() != applied_advert.load()) {
    if (!apply_advert_data()) {
      oslog.println("BLE ERROR operation=advert-refresh");
      return;
    }
    touch_ui();
    const auto word = applied_advert.load();
    oslog.printf("BLE ADV flags=0x%02x fin=%lu\n", unsigned(word & 0xffU),
                 static_cast<unsigned long>(word >> 8));
  }
  // A scanner filtering on the flag byte must see a flip within a minute, so
  // a flag change advertises fast for flag_window_ms. The counter alone does
  // not. The sync placeholder is never published, so the logger's first
  // flags are no flip and the boot keeps its fast_window_ms.
  if (flag_watch.on_publish(static_cast<std::uint8_t>(applied_advert.load()))) {
    adv_window.extend(esp_timer_get_time(), options.flag_window_ms);
    speed_up_advertising();
  }
}

// Two stage advertising. Fast for the rest of the window, then slow with no
// end. The controller ends the fast stage itself (BLE_GAP_EVENT_ADV_COMPLETE
// with BLE_HS_ETIMEOUT), and that event restarts advertising slow.
bool start_advertising() {
  const auto now = esp_timer_get_time();
  const bool fast = adv_window.fast(now);
  ble_gap_adv_params params{};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  params.itvl_min = fast ? options.fast_adv_min : options.slow_adv_min;
  params.itvl_max = fast ? options.fast_adv_max : options.slow_adv_max;
  const std::int32_t duration =
      fast ? static_cast<std::int32_t>(adv_window.remaining_ms(now))
           : BLE_HS_FOREVER;
  const int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, nullptr, duration,
                                   &params, gap_event, nullptr);
  advertising = rc == 0;
  touch_ui();
  if (rc != 0) {
    oslog.printf("BLE ERROR operation=advertise rc=%d\n", rc);
    // A transient failure must not leave the device invisible until the next
    // disconnect: try again in a second.
    ble_npl_callout_reset(&advertise_callout, ble_npl_time_ms_to_ticks32(1000));
    return false;
  }
  if (adv_fast.exchange(fast) != fast || fast)
    oslog.record_only().printf(
        "BLE ADV mode=%s itvl=%u-%u ms=%ld\n", fast ? "fast" : "slow",
        unsigned(params.itvl_min), unsigned(params.itvl_max),
        static_cast<long>(fast ? duration : 0));
  return true;
}

// Restart a slow advertiser fast after the window grew. Host task only.
void speed_up_advertising() {
  if (!advertising.load() || adv_fast.load() || connected.load() ||
      !enabled.load() || !host_requested.load())
    return;
  if (ble_gap_adv_stop() == 0)
    start_advertising();
}

void advertise_on_host(ble_npl_event *) {
  if (enabled.load() && host_requested.load() && !connected.load() &&
      !advertising.load())
    start_advertising();
}

void wake_on_host(ble_npl_event *) {
  adv_window.extend(esp_timer_get_time(), options.fast_window_ms);
  speed_up_advertising();
}

// All notification submissions execute on the host task. Its GAP callbacks
// cannot change generation/authentication between the checks and submission,
// even when the controller reuses a connection handle after disconnection.
int notify_on_host(Characteristic which, const std::uint8_t *data,
                   std::size_t length, std::uint32_t session,
                   std::uint16_t handle) {
  if (!data || length == 0 || length > kMaxFrame || length > payload_max())
    return BLE_HS_EMSGSIZE;
  if (!connected.load() || !subscribed[which].load() ||
      generation.load() != session || conn_handle.load() != handle ||
      !may_send_frame(handle, data, length))
    return BLE_HS_ENOTCONN;
  // Native custom notifications consume the mbuf on success AND failure.
  // Each retry must allocate a fresh buffer containing the same whole frame.
  os_mbuf *buffer = ble_hs_mbuf_from_flat(data, length);
  return buffer ? ble_gatts_notify_custom(handle, value_handles[which], buffer)
                : BLE_HS_ENOMEM;
}

NotifyStatus classify_notify(int rc) {
  if (rc == 0)
    return NotifyStatus::Sent;
  // Out of mbufs or the controller queue is full: the same frame goes again.
  if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY || rc == BLE_HS_EAGAIN)
    return NotifyStatus::Refused;
  return NotifyStatus::Failed;
}

// Ends the response stream and wakes a waiting producer, which then sees
// Closed.
void close_responses() {
  portENTER_CRITICAL(&tx_lock);
  responses.close();
  portEXIT_CRITICAL(&tx_lock);
  if (space_event)
    xSemaphoreGive(space_event);
  ble_npl_callout_stop(&retry_callout);
}

bool responses_idle() {
  portENTER_CRITICAL(&tx_lock);
  const bool idle = responses.idle();
  portEXIT_CRITICAL(&tx_lock);
  return idle;
}

// Scratch for the NimBLE host task only (snapshot notifications and GATT
// access callbacks both run there, one at a time). Kept off its 8 KB stack,
// which the ATT, SM and NVS bond paths below these callbacks already use.
char host_json[kMaxJson];
ControlRequest host_request;

void notify_snapshots(ble_npl_event *) {
  // Snapshots never slip between the frames of a response stream and never
  // compete with it for host buffers; the pending bits wait for idle credits.
  if (!responses_idle())
    return;
  const unsigned pending = pending_snapshots.exchange(0);
  for (const auto which : {Status, Live}) {
    if (!(pending & (1U << which)))
      continue;
    char *const value = host_json;
    portENTER_CRITICAL(&snapshot_lock);
    const auto length = which == Status ? status_length : live_length;
    std::memcpy(value, which == Status ? status_text : live_text, length);
    portEXIT_CRITICAL(&snapshot_lock);
    if (snapshot_notification_fits(length, mtu.load()))
      notify_on_host(which, reinterpret_cast<const std::uint8_t *>(value),
                     length, generation.load(), conn_handle.load());
  }
}

// Host task. Submits queued frames in order until the queue empties or the
// host refuses one. NimBLE reports a notification through NOTIFY_TX inside
// ble_gatts_notify_custom() once the host has taken or dropped it, which is
// when its credit returns; the frame may still wait in the controller.
void drain_responses(ble_npl_event *) {
  for (;;) {
    TxSlot slot;
    std::uint8_t head = 0;
    portENTER_CRITICAL(&tx_lock);
    const bool any = responses.front(slot, head);
    portEXIT_CRITICAL(&tx_lock);
    if (!any)
      break;
    if (response_gate(slot.session, slot.handle,
                      tx_frames + std::size_t(head) * kMaxFrame,
                      slot.length) != GateRefusal::None) {
      // The phone unsubscribed or the session ended. The stream is over for
      // this session, but the link itself did nothing wrong.
      close_responses();
      return;
    }
    notify_tx_seen = false;
    awaiting_tx = true;
    const int rc =
        notify_on_host(Response, tx_frames + std::size_t(head) * kMaxFrame,
                       slot.length, slot.session, slot.handle);
    awaiting_tx = false;
    const auto status = classify_notify(notify_tx_seen ? notify_tx_status : rc);
    const auto now = esp_timer_get_time();
    portENTER_CRITICAL(&tx_lock);
    const auto outcome = responses.on_notify_tx(slot.session, status, now);
    portEXIT_CRITICAL(&tx_lock);
    const bool current = outcome.current;
    if (status == NotifyStatus::Sent && current) {
      interval.on_activity(now);
      xSemaphoreGive(space_event);
      continue;
    }
    if (!current) {
      // A frame from a closed session; the queue was cleared with it.
      xSemaphoreGive(space_event);
      return;
    }
    if (status == NotifyStatus::Failed) {
      xSemaphoreGive(space_event);
      oslog.printf("BLE ERROR operation=notify rc=%d\n", rc);
      if (connected.load() && conn_handle.load() == slot.handle)
        terminate(slot.handle, DisconnectReason::SendError);
      return;
    }
    // Refused. The host frees buffers without telling the application, so the
    // head frame is offered again shortly, until the progress timeout. A
    // stalled stream closes once; a terminate already pending for another
    // reason counts, and only a failed one keeps the retries going.
    auto action = stall.on_refused(slot.session, outcome.stalled);
    if (action == StallClose::Action::Close) {
      if (stall.log(slot.session))
        oslog.printf("BLE SLOW timeout_ms=%lu\n",
                     static_cast<unsigned long>(options.progress_timeout_ms));
      action = stall.on_close(
          slot.session, terminate(slot.handle, DisconnectReason::SlowPeer));
    }
    if (action == StallClose::Action::Wait)
      return; // DISCONNECT clears the queue.
    ble_npl_callout_reset(&retry_callout,
                          ble_npl_time_ms_to_ticks32(options.refused_retry_ms));
    return;
  }
  if (pending_snapshots.load())
    notify_snapshots(nullptr);
}

// Producer side, serialised by response_mutex. Ok queued the frame, Full has
// no credit now, Closed never will for this session.
ossync::EnqueueResult enqueue_response(const std::uint8_t *data,
                                       std::size_t length,
                                       std::uint32_t session) {
  if (!data || !length || length > kMaxFrame || length > payload_max())
    return ossync::EnqueueResult::Closed;
  const auto handle = conn_handle.load();
  if (response_gate(session, handle, data, length) != GateRefusal::None)
    return ossync::EnqueueResult::Closed;
  portENTER_CRITICAL(&tx_lock);
  const auto reservation = responses.reserve(session, esp_timer_get_time());
  portEXIT_CRITICAL(&tx_lock);
  if (reservation.result != ossync::EnqueueResult::Ok)
    return reservation.result;
  // The credit keeps this slot outside the queued range until it is published,
  // so the host never reads it while it is filled. A stream that closed or
  // reopened meanwhile rejects the publish by epoch.
  std::memcpy(tx_frames + std::size_t(reservation.slot) * kMaxFrame, data,
              length);
  portENTER_CRITICAL(&tx_lock);
  const bool open = responses.publish(
      reservation, session, static_cast<std::uint16_t>(length), handle);
  portEXIT_CRITICAL(&tx_lock);
  if (!open)
    return ossync::EnqueueResult::Closed;
  ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &response_event);
  return ossync::EnqueueResult::Ok;
}

// A link command NimBLE refused or the controller failed. rc 19
// (BLE_HS_ETIMEOUT_HCI) means no ack within 2 s, and NimBLE resets the host
// right after, so this line names the command that started the reset.
void log_hci_fail(const char *command, std::uint16_t opcode, int rc) {
  if (rc == 0)
    return;
  oslog.printf("BLE HCI fail cmd=%s op=0x%04x rc=%d timeout=%u\n", command,
               unsigned(opcode), rc, unsigned(rc == BLE_HS_ETIMEOUT_HCI));
}

int request_conn_params(std::uint16_t handle, const ConnParams &p,
                        const char *label) {
  ble_gap_upd_params params{};
  params.itvl_min = p.itvl_min;
  params.itvl_max = p.itvl_max;
  params.latency = p.latency;
  params.supervision_timeout = p.supervision_timeout;
  const int rc = ble_gap_update_params(handle, &params);
  oslog.record_only().printf(
      "BLE CONN request=%s itvl=%u-%u lat=%u to=%u rc=%d\n", label,
      unsigned(p.itvl_min), unsigned(p.itvl_max), unsigned(p.latency),
      unsigned(p.supervision_timeout), rc);
  if (conn_fail_log.on_result(rc))
    log_hci_fail("conn", BLE_HCI_OP(BLE_HCI_OGF_LE, BLE_HCI_OCF_LE_CONN_UPDATE),
                 rc);
  return rc;
}

// The relax timer: the link goes to the relaxed interval once no control
// write arrived and no response frame left for relax_after_ms.
void relax_on_host(ble_npl_event *) {
  if (!connected.load())
    return;
  if (link_step != LinkStep::Done) {
    ble_npl_callout_reset(&relax_callout, ble_npl_time_ms_to_ticks32(1000));
    return;
  }
  std::uint32_t rearm_ms = 0;
  const auto action =
      interval.on_timer(esp_timer_get_time(), options.relax_after_ms, rearm_ms);
  if (action == IntervalPolicy::Action::RequestRelaxed) {
    if (request_conn_params(conn_handle.load(), options.relaxed, "relaxed") !=
        0) {
      // A procedure was in progress. Stay active and try again later.
      interval.request_failed();
      ble_npl_callout_reset(&relax_callout, ble_npl_time_ms_to_ticks32(1000));
    }
  } else if (rearm_ms)
    ble_npl_callout_reset(&relax_callout, ble_npl_time_ms_to_ticks32(rearm_ms));
}

// Link procedures after connect run one at a time. PHY update and
// connection update both carry an instant, and the ESP32-S3 controller
// asserted in its local connection update (lld_con.c 3275) when an iPad's
// own CONNECTION_UPDATE_IND crossed ours during the bonded reconnect
// (bench 2026-10-12). No link request is sent from CONNECT. A data length
// request there timed out while a bonded phone started encryption, and the
// host reset that followed crashed on the stale ack (core dumps 2026-10-18).
// Data length goes once the link is secure, PHY waits for the central to
// settle, the interval waits for the PHY.
constexpr std::uint32_t kLinkSettleMs = 2000;
constexpr std::uint32_t kPhyWaitMs = 2000;

bool interval_already_active(std::uint16_t handle) {
  ble_gap_conn_desc desc{};
  return ble_gap_conn_find(handle, &desc) == 0 &&
         desc.conn_itvl >= options.active.itvl_min &&
         desc.conn_itvl <= options.active.itvl_max &&
         desc.conn_latency == options.active.latency;
}

void link_on_host(ble_npl_event *) {
  if (!connected.load() || !authenticated.load())
    return;
  const auto handle = conn_handle.load();
  if (link_step == LinkStep::Dle) {
    link_step = LinkStep::Phy;
    if (dle_request_needed(dle_tx_octets, options.data_len_octets)) {
      const int rc = ble_gap_set_data_len(handle, options.data_len_octets,
                                          options.data_len_time_us);
      oslog.record_only().printf("BLE LINK request dle=%u/%u rc=%d\n",
                                 unsigned(options.data_len_octets),
                                 unsigned(options.data_len_time_us), rc);
      log_hci_fail("dle",
                   BLE_HCI_OP(BLE_HCI_OGF_LE, BLE_HCI_OCF_LE_SET_DATA_LEN), rc);
    } else
      oslog.record_only().printf("BLE LINK dle=%u by central\n",
                                 unsigned(dle_tx_octets));
    ble_npl_callout_reset(&link_callout,
                          ble_npl_time_ms_to_ticks32(kLinkSettleMs));
    return;
  }
  if (link_step == LinkStep::Phy) {
    link_step = LinkStep::Interval;
    std::uint8_t tx = 0, rx = 0;
    const int read =
        options.prefer_2m_phy ? ble_gap_read_le_phy(handle, &tx, &rx) : 0;
    log_hci_fail("read_phy", BLE_HCI_OP(BLE_HCI_OGF_LE, BLE_HCI_OCF_LE_RD_PHY),
                 read);
    if (options.prefer_2m_phy && !phy_request_needed(read == 0, tx, rx)) {
      oslog.record_only().println("BLE LINK phy=2m by central");
    } else if (options.prefer_2m_phy) {
      const int rc = ble_gap_set_prefered_le_phy(handle, BLE_GAP_LE_PHY_2M_MASK,
                                                 BLE_GAP_LE_PHY_2M_MASK,
                                                 BLE_GAP_LE_PHY_CODED_ANY);
      oslog.record_only().printf("BLE LINK request phy=2m rc=%d\n", rc);
      log_hci_fail("phy", BLE_HCI_OP(BLE_HCI_OGF_LE, BLE_HCI_OCF_LE_SET_PHY),
                   rc);
      if (rc == 0) {
        // PHY_UPDATE_COMPLETE moves on sooner; this is the fallback.
        ble_npl_callout_reset(&link_callout,
                              ble_npl_time_ms_to_ticks32(kPhyWaitMs));
        return;
      }
    }
  }
  if (link_step != LinkStep::Interval)
    return;
  // The central often picks an interval in range on its own. Asking anyway
  // only starts a procedure that can cross the central's next update.
  if (interval.relaxed() || interval_already_active(handle)) {
    link_step = LinkStep::Done;
    return;
  }
  // A procedure in progress refuses it, so it is tried again every second.
  if (request_conn_params(handle, options.active, "active") != 0)
    ble_npl_callout_reset(&link_callout, ble_npl_time_ms_to_ticks32(1000));
  else
    link_step = LinkStep::Done;
}

// Called once the link is encrypted and authenticated, from a GAP event. The
// first command goes out on the next turn of the host's event queue.
void start_link_sequence() {
  if (link_step != LinkStep::Idle)
    return;
  link_step = LinkStep::Dle;
  ble_npl_callout_reset(&link_callout, 0);
}

// The central ran a procedure of its own on `handle`. While the PHY request
// still waits, the wait starts over (ble_policy.h). Host task only.
void defer_link_step(std::uint16_t handle, const char *after) {
  if (handle != conn_handle.load() || !central_procedure_rearms(link_step))
    return;
  ble_npl_callout_reset(&link_callout,
                        ble_npl_time_ms_to_ticks32(kLinkSettleMs));
  oslog.record_only().printf("BLE LINK wait phy after=%s\n", after);
}

// Host task only. Reads NimBLE's security state for `handle` and, the first
// time it proves the current link secure, latches `authenticated` the way
// ENC_CHANGE does. `via` names where the proof came from for the bench log.
// Returns the live state.
bool confirm_secure(std::uint16_t handle, const char *via) {
  const bool secure_now = secure(handle);
  if (!should_latch(connected.load() && handle == conn_handle.load(),
                    authenticated.load(), secure_now))
    return secure_now;
  authenticated = true;
  pairing_active = false;
  passkey = 0;
  update_bonds();
  touch_ui();
  oslog.printf("BLE BONDED authenticated=true bonds=%lu via=%s\n",
               static_cast<unsigned long>(bonds.load()), via);
  start_link_sequence();
  return true;
}

bool defer_error(Op op, Error code, const char *detail, std::uint32_t session) {
  const ImmediateError error{op, code, detail, session};
  return immediate_errors && xQueueSend(immediate_errors, &error, 0) == pdTRUE;
}

void error_task(void *) {
  ImmediateError error{};
  for (;;) {
    if (xQueueReceive(immediate_errors, &error, portMAX_DELAY) == pdTRUE)
      send_error(error.op, error.code, error.detail, error.generation);
  }
}

// Native NimBLE requires a mutable void* in the callback signature.
int access(std::uint16_t handle, std::uint16_t, ble_gatt_access_ctxt *ctxt,
           // cppcheck-suppress constParameterCallback
           void *arg) {
  const auto which =
      static_cast<Characteristic>(reinterpret_cast<std::uintptr_t>(arg));
  if (!confirm_secure(handle, "access")) {
    // Not reached in this configuration. NimBLE's ATT permission check
    // applies the same rule as secure() (ble_att_svr_check_perms with the
    // ENC flags, plus AUTHEN and sm_sc_only in the passkey modes) and its
    // ATT error is what makes a phone pair. If the checks ever diverge, a
    // Control write is refused by ERROR 14 on `response` alone. It is
    // acknowledged and not dispatched, because an ATT error would fail the
    // client's write before it could read the ERROR.
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && which == Control) {
      std::uint8_t op = 0;
      if (OS_MBUF_PKTLEN(ctxt->om) > 0)
        os_mbuf_copydata(ctxt->om, 0, 1, &op);
      const auto refusal = control_refusal(false);
      // If the error queue is full the ATT error below still refuses it.
      if (defer_error(static_cast<Op>(op), refusal.code, refusal.detail,
                      generation.load()))
        return 0;
    }
    return mode == config::PairMode::None ? BLE_ATT_ERR_INSUFFICIENT_ENC
                                          : BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
  }
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    if (which == Info)
      return os_mbuf_append(ctxt->om, info_text, std::strlen(info_text)) == 0
                 ? 0
                 : BLE_ATT_ERR_INSUFFICIENT_RES;
    if (which == Status || which == Live) {
      // ATT performs Read Blob offset slicing after the callback. Append the
      // full cached snapshot, including values too large for notifications.
      char *const value = host_json;
      portENTER_CRITICAL(&snapshot_lock);
      const auto length = which == Status ? status_length : live_length;
      std::memcpy(value, which == Status ? status_text : live_text, length);
      portEXIT_CRITICAL(&snapshot_lock);
      return os_mbuf_append(ctxt->om, value, length) == 0
                 ? 0
                 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return which == Response ? 0 : BLE_ATT_ERR_READ_NOT_PERMITTED;
  }
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || which != Control)
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  ControlRequest &request = host_request;
  request = ControlRequest{};
  request.received_mono_us = esp_timer_get_time();
  request.conn_handle = handle;
  request.link_generation = generation.load();
  const auto length = OS_MBUF_PKTLEN(ctxt->om);
  if (length > kMaxControlBytes)
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  request.length = static_cast<std::uint16_t>(length);
  if (length == 0) {
    return defer_error(kOpList, kErrMalformed, "empty", request.link_generation)
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (os_mbuf_copydata(ctxt->om, 0, length, request.bytes) != 0)
    return BLE_ATT_ERR_UNLIKELY;
  oslog.record_only().printf("BLE CMD op=0x%02x bytes=%u\n",
                             unsigned(request.bytes[0]),
                             unsigned(request.length));
  // Any request brings a relaxed link back to the active interval.
  if (interval.on_request(request.received_mono_us) ==
      IntervalPolicy::Action::RequestActive) {
    // A refused request leaves the mode relaxed so the next write retries.
    if (request_conn_params(handle, options.active, "active") != 0)
      interval.request_failed();
    ble_npl_callout_reset(&relax_callout,
                          ble_npl_time_ms_to_ticks32(options.relax_after_ms));
  }
  ossync::abort::stamp(request);
  if (request_handler(request)) {
    ossync::abort::commit(request);
    return 0;
  }
  // The link is secure here, so the only refusal left is the full queue.
  const auto refusal = control_refusal(true);
  if (!defer_error(static_cast<Op>(request.bytes[0]), refusal.code,
                   refusal.detail, request.link_generation))
    return BLE_ATT_ERR_INSUFFICIENT_RES;
  return 0;
}

// One characteristic's subscribe state on the current link, from a live
// CCCD write or from a restore held until CONNECT. Host task only.
void apply_subscribe(Characteristic which, bool notify, bool closing) {
  subscribed[which] = notify;
  if (which != Response || !connected.load())
    return;
  // Turning response notifications off ends the stream at once. On again
  // starts a new generation, so requests and handles of the old stream end
  // Closed or ERROR 5 instead of resuming mid stream.
  portENTER_CRITICAL(&tx_lock);
  const bool open = responses.open_for(generation.load());
  portEXIT_CRITICAL(&tx_lock);
  const auto change = response_subscribe(notify, closing, open);
  if (change == StreamChange::Close) {
    close_responses();
  } else if (change == StreamChange::Reopen) {
    const auto session = ++generation;
    portENTER_CRITICAL(&tx_lock);
    responses.open(session, esp_timer_get_time());
    portEXIT_CRITICAL(&tx_lock);
    oslog.record_only().println("BLE STREAM reopen");
  }
}

int gap_event(ble_gap_event *event, void *) {
  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    if (event->connect.status != 0) {
      early_cccd.clear();
      if (enabled.load() && host_requested.load())
        start_advertising();
      return 0;
    }
    if (!host_requested.load()) {
      early_cccd.clear();
      terminate(event->connect.conn_handle, DisconnectReason::ServerStop);
      return 0;
    }
    ++generation; // Invalidate previous worker replies before exposing handle.
    conn_handle = event->connect.conn_handle;
    mtu = ble_att_mtu(event->connect.conn_handle);
    advertising = false;
    authenticated = false;
    connected = true;
    std::fill(std::begin(subscribed), std::end(subscribed), false);
    link_step = LinkStep::Idle;
    dle_tx_octets = 0;
    conn_fail_log.reset();
    {
      const auto now = esp_timer_get_time();
      portENTER_CRITICAL(&tx_lock);
      responses.open(generation.load(), now);
      portEXIT_CRITICAL(&tx_lock);
      adv_fast = false;
      interval.connect(now);
      ble_npl_callout_reset(&relax_callout,
                            ble_npl_time_ms_to_ticks32(options.relax_after_ms));
    }
    touch_ui();
    oslog.printf("BLE CONNECT mtu=%u\n", unsigned(mtu.load()));
    // Subscriptions NimBLE restored or the client wrote before this event
    // count exactly like a CCCD write after it.
    if (const auto held = early_cccd.replay(
            event->connect.conn_handle, [](unsigned i, bool notify) {
              apply_subscribe(static_cast<Characteristic>(i), notify, false);
            }))
      oslog.printf("BLE SUBSCRIBE early=%u response=%u\n", held,
                   unsigned(subscribed[Response].load()));
    // Already secure at connect, which ENC_CHANGE would never report.
    confirm_secure(event->connect.conn_handle, "connect");
    return 0;
  case BLE_GAP_EVENT_DISCONNECT: {
    const auto local = pending_reason.exchange(DisconnectReason::Unknown);
    const std::uint32_t closed = generation.load();
    std::int16_t err = 0;
    const auto reason =
        map_ble_disconnect(event->disconnect.reason, local, err);
    const int hci = event->disconnect.reason - BLE_HS_ERR_HCI_BASE;
    // Record before clearing `connected`: disconnect() callers wait on it.
    if (close_claim.claim(closed))
      record_disconnect(reason, kEventLinkBle, closed, err, 0,
                        esp_timer_get_time());
    connected = false;
    authenticated = false;
    pairing_active = false;
    passkey = 0;
    conn_handle = BLE_HS_CONN_HANDLE_NONE;
    mtu = 0;
    ++generation;
    std::fill(std::begin(subscribed), std::end(subscribed), false);
    early_cccd.clear();
    dle_tx_octets = 0;
    close_responses();
    ble_npl_callout_stop(&relax_callout);
    ble_npl_callout_stop(&link_callout);
    oslog.printf("BLE DISCONNECT reason=%s hci=0x%02x\n",
                 disconnect_reason_name(reason), unsigned(hci & 0xff));
    // A phone that just lost the link reconnects fastest from a fast advert.
    adv_window.extend(esp_timer_get_time(), options.fast_window_ms);
    if (enabled.load() && host_requested.load())
      start_advertising();
    else {
      advertising = false;
      touch_ui();
    }
    return 0;
  }
  case BLE_GAP_EVENT_MTU:
    defer_link_step(event->mtu.conn_handle, "mtu");
    if (event->mtu.conn_handle == conn_handle.load()) {
      mtu = event->mtu.value;
      touch_ui();
      oslog.printf("BLE MTU mtu=%u\n", unsigned(event->mtu.value));
    }
    return 0;
  case BLE_GAP_EVENT_SUBSCRIBE: {
    const auto route = route_subscribe(
        connected.load(), event->subscribe.conn_handle == conn_handle.load());
    if (route == SubscribeRoute::Drop)
      return 0;
    const bool closing =
        event->subscribe.reason == BLE_GAP_SUBSCRIBE_REASON_TERM;
    const bool notify = event->subscribe.cur_notify != 0;
    if (route == SubscribeRoute::Hold) {
      // CONNECT has not run yet, typically a bonded CCCD that NimBLE restored
      // when the central encrypted early. CONNECT applies it.
      for (unsigned i = 0; i < Count; ++i)
        if (event->subscribe.attr_handle == value_handles[i])
          early_cccd.hold(event->subscribe.conn_handle, i, notify);
      return 0;
    }
    // A CCCD write proves nothing by itself, but it is the first event a
    // client sends after a reconnect, so a missed ENC_CHANGE latches here.
    // Not during teardown, which reports every CCCD as turned off.
    if (!closing)
      confirm_secure(event->subscribe.conn_handle, "subscribe");
    for (unsigned i = 0; i < Count; ++i)
      if (event->subscribe.attr_handle == value_handles[i])
        apply_subscribe(static_cast<Characteristic>(i), notify, closing);
    return 0;
  }
  case BLE_GAP_EVENT_ENC_CHANGE: {
    if (event->enc_change.conn_handle != conn_handle.load())
      return 0;
    pairing_active = false;
    passkey = 0;
    const bool was = authenticated.load();
    authenticated =
        event->enc_change.status == 0 && secure(event->enc_change.conn_handle);
    update_bonds();
    touch_ui();
    if (!authenticated.load()) {
      oslog.println("BLE AUTH result=failed action=disconnect");
      terminate(event->enc_change.conn_handle, DisconnectReason::AuthFailed);
    } else if (enc_change_announces(was, true)) {
      oslog.printf("BLE BONDED authenticated=true bonds=%lu\n",
                   static_cast<unsigned long>(bonds.load()));
      start_link_sequence();
    }
    return 0;
  }
  case BLE_GAP_EVENT_PASSKEY_ACTION: {
    if (mode == config::PairMode::None ||
        event->passkey.params.action != BLE_SM_IOACT_DISP)
      return BLE_HS_ENOTSUP;
    ble_sm_io io{};
    io.action = BLE_SM_IOACT_DISP;
    io.passkey = mode == config::PairMode::Fixed ? fixed_passkey
                                                 : esp_random() % 1000000U;
    passkey = io.passkey;
    pairing_deadline = esp_timer_get_time() + kPairingWindowUs;
    pairing_active = true;
    touch_ui();
    if (mode == config::PairMode::Fixed)
      oslog.println("BLE PAIR passkey=fixed");
    else
      oslog.record_only().println("BLE PAIR passkey=random");
    return ble_sm_inject_io(event->passkey.conn_handle, &io);
  }
  case BLE_GAP_EVENT_REPEAT_PAIRING: {
    // A phone can delete its own bond; a new authenticated pairing may replace
    // that obsolete record without clearing bonds belonging to other phones.
    ble_gap_conn_desc desc{};
    if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0 &&
        ble_store_util_delete_peer(&desc.peer_id_addr) == 0)
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    return BLE_GAP_REPEAT_PAIRING_IGNORE;
  }
  case BLE_GAP_EVENT_ADV_COMPLETE:
    // BLE_HS_ETIMEOUT ends the fast stage; the restart below is slow.
    advertising = false;
    if (enabled.load() && host_requested.load() && !connected.load())
      start_advertising();
    return 0;
  case BLE_GAP_EVENT_NOTIFY_TX:
    if (awaiting_tx && !event->notify_tx.indication &&
        event->notify_tx.conn_handle == conn_handle.load() &&
        event->notify_tx.attr_handle == value_handles[Response]) {
      notify_tx_status = event->notify_tx.status;
      notify_tx_seen = true;
    }
    return 0;
  case BLE_GAP_EVENT_CONN_UPDATE: {
    defer_link_step(event->conn_update.conn_handle, "conn");
    ble_gap_conn_desc desc{};
    if (event->conn_update.conn_handle == conn_handle.load() &&
        ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0)
      oslog.record_only().printf(
          "BLE CONN itvl=%u lat=%u to=%u status=%d\n", unsigned(desc.conn_itvl),
          unsigned(desc.conn_latency), unsigned(desc.supervision_timeout),
          event->conn_update.status);
    return 0;
  }
  case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
    // A PHY update before our request is the central's own.
    defer_link_step(event->phy_updated.conn_handle, "phy");
    if (event->phy_updated.conn_handle == conn_handle.load() &&
        link_step == LinkStep::Interval)
      ble_npl_callout_reset(&link_callout, ble_npl_time_ms_to_ticks32(200));
    if (event->phy_updated.conn_handle == conn_handle.load())
      oslog.record_only().printf("BLE PHY tx=%u rx=%u status=%d\n",
                                 unsigned(event->phy_updated.tx_phy),
                                 unsigned(event->phy_updated.rx_phy),
                                 event->phy_updated.status);
    return 0;
  case BLE_GAP_EVENT_DATA_LEN_CHG:
    if (event->data_len_chg.conn_handle != conn_handle.load())
      return 0;
    // The Dle step skips a length the central already set.
    dle_tx_octets = event->data_len_chg.max_tx_octets;
    oslog.record_only().printf("BLE DLE tx=%u/%u rx=%u/%u\n",
                               unsigned(event->data_len_chg.max_tx_octets),
                               unsigned(event->data_len_chg.max_tx_time),
                               unsigned(event->data_len_chg.max_rx_octets),
                               unsigned(event->data_len_chg.max_rx_time));
    return 0;
  default:
    return 0;
  }
}

void clear_bonds_on_host(ble_npl_event *) {
  const int rc = ble_store_clear();
  update_bonds();
  touch_ui();
  oslog.printf("BLE BONDS CLEARED bonds=%lu rc=%d\n",
               static_cast<unsigned long>(bonds.load()), rc);
  xSemaphoreGive(clear_done);
}

void on_reset(int reason) {
  connected = false;
  authenticated = false;
  advertising = false;
  pairing_active = false;
  passkey = 0;
  conn_handle = BLE_HS_CONN_HANDLE_NONE;
  mtu = 0;
  ++generation;
  std::fill(std::begin(subscribed), std::end(subscribed), false);
  early_cccd.clear();
  dle_tx_octets = 0;
  conn_fail_log.reset();
  close_responses();
  ble_npl_callout_stop(&relax_callout);
  ble_npl_callout_stop(&link_callout);
  touch_ui();
  oslog.printf("BLE RESET reason=%d\n", reason);
}

void on_sync() {
  if (!host_requested.load()) {
    startup_ok = false;
    xSemaphoreGive(startup_done);
    return;
  }
  // Deliberately require the factory public address, never infer a random/RPA
  // fallback: Android Companion Device Manager keys identity on this address.
  std::uint8_t public_address[6];
  bool ok = ble_hs_util_ensure_addr(0) == 0 &&
            ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, public_address, nullptr) == 0;
  wire::ScanResponse scan_response;
  ok = ok && wire::legacy_scan_response(name_text, scan_response) &&
       apply_advert_data() &&
       ble_gap_adv_rsp_set_data(scan_response.bytes.data(),
                                scan_response.length) == 0;
  update_bonds();
  if (ok) {
    enabled = true;
    adv_window.extend(esp_timer_get_time(), options.fast_window_ms);
    ok = start_advertising();
  }
  // A late controller synchronization must not enable the service after
  // begin() has returned failure to the application.
  if (!host_requested.load()) {
    ble_gap_adv_stop();
    enabled = false;
    advertising = false;
    ok = false;
  }
  startup_ok = ok;
  if (!ok)
    oslog.println("BLE ERROR operation=sync");
  xSemaphoreGive(startup_done);
}

void host_task(void *) {
  host_task_handle = xTaskGetCurrentTaskHandle();
  nimble_port_run();
  nimble_port_freertos_deinit();
}
} // namespace

bool begin(const Identity &identity, config::PairMode pairing_mode,
           std::uint32_t fixed_pin, RequestHandler handler,
           const Options &link_options) {
  if (!handler || startup_done || !identity.thing || !identity.device ||
      !identity.boot || !identity.schema || !identity.dictionary_sha256 ||
      !identity.firmware || !identity.descriptor_sha256 ||
      !valid_options(link_options))
    return false;
  options = link_options;
  request_handler = handler;
  mode = pairing_mode;
  fixed_passkey = fixed_pin % 1000000U;
  // Leave enough legacy scan-response space for the complete service UUID.
  if (!wire::local_name(name_text, identity.name_prefix, identity.device))
    return false;
  if (!wire::build_info(info_text, identity))
    return false;
  const auto boot_length = std::strlen(identity.boot);
  boot16 = static_cast<std::uint16_t>(std::strtoul(
      boot_length >= 4 ? identity.boot + boot_length - 4 : identity.boot,
      nullptr, 16));
  AdvertState initial;
  initial.flags = kAdvNoUtc;
  desired_advert = pack_advert(initial);
  startup_done = xSemaphoreCreateBinary();
  response_mutex = xSemaphoreCreateMutex();
  clear_mutex = xSemaphoreCreateMutex();
  clear_done = xSemaphoreCreateBinary();
  space_event = xSemaphoreCreateBinary();
  immediate_errors = xQueueCreate(8, sizeof(ImmediateError));
  if (!startup_done || !response_mutex || !clear_mutex || !clear_done ||
      !immediate_errors || !space_event)
    return false;
  responses.configure(options.notify_credits, options.progress_timeout_ms);
  const std::size_t frame_bytes = std::size_t(responses.capacity()) * kMaxFrame;
  tx_frames = static_cast<std::uint8_t *>(
      heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!tx_frames)
    tx_frames = static_cast<std::uint8_t *>(
        heap_caps_malloc(frame_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!tx_frames)
    return false;
  // config::load() owns NVS initialization. Do not erase settings on failure.
  const auto rc = nimble_port_init();
  if (rc != ESP_OK) {
    oslog.printf("BLE ERROR operation=init rc=%d\n", int(rc));
    return false;
  }
  ble_hs_cfg.reset_cb = on_reset;
  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr; // Evict oldest at 3.
  const bool mitm = mode != config::PairMode::None;
  ble_hs_cfg.sm_io_cap =
      mitm ? BLE_HS_IO_DISPLAY_ONLY : BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = mitm;
  ble_hs_cfg.sm_sc = 1;
  // Native SC-only also forces authenticated ATT access, which excludes
  // encrypted Just Works. Legacy pairing stays disabled in sdkconfig, so
  // `none` still negotiates Secure Connections without requiring MITM.
  ble_hs_cfg.sm_sc_only = mitm ? 1 : 0;
  ble_hs_cfg.sm_our_key_dist =
      BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist =
      BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_svc_gap_device_name_set(name_text);
  if (ble_att_set_preferred_mtu(kMaxMtu) != 0)
    return false;
  const auto read_flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                          (mitm ? BLE_GATT_CHR_F_READ_AUTHEN : 0);
  const auto write_flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                           (mitm ? BLE_GATT_CHR_F_WRITE_AUTHEN : 0);
  for (unsigned i = 0; i < Count; ++i) {
    auto &chr = characteristics[i];
    chr.uuid = &uuids[i + 1].u;
    chr.access_cb = access;
    chr.arg = reinterpret_cast<void *>(static_cast<std::uintptr_t>(i));
    chr.val_handle = &value_handles[i];
    chr.flags = i == Control ? write_flags : read_flags;
    if (i == Status || i == Live || i == Response)
      chr.flags |= BLE_GATT_CHR_F_NOTIFY;
  }
  services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
  services[0].uuid = &uuids[0].u;
  services[0].characteristics = characteristics;
  if (ble_gatts_count_cfg(services) != 0 || ble_gatts_add_svcs(services) != 0)
    return false;
  // Native IDF persists NimBLE keys via its NVS storage port. Retention of
  // existing Arduino NimBLE bonds is not qualified until board readback;
  // owner re-pairing may be required. Do not erase the namespace at startup.
  ble_store_config_init();
  ble_npl_event_init(&advert_event, refresh_advert, nullptr);
  ble_npl_event_init(&clear_event, clear_bonds_on_host, nullptr);
  ble_npl_event_init(&response_event, drain_responses, nullptr);
  ble_npl_event_init(&snapshot_event, notify_snapshots, nullptr);
  ble_npl_event_init(&wake_event, wake_on_host, nullptr);
  ble_npl_callout_init(&retry_callout, nimble_port_get_dflt_eventq(),
                       drain_responses, nullptr);
  ble_npl_callout_init(&relax_callout, nimble_port_get_dflt_eventq(),
                       relax_on_host, nullptr);
  ble_npl_callout_init(&advertise_callout, nimble_port_get_dflt_eventq(),
                       advertise_on_host, nullptr);
  ble_npl_callout_init(&link_callout, nimble_port_get_dflt_eventq(),
                       link_on_host, nullptr);
  if (xTaskCreate(error_task, "os_ble_error", 4096, nullptr, 4, nullptr) !=
      pdPASS)
    return false;
  host_requested = true;
  nimble_port_freertos_init(host_task);
  if (xSemaphoreTake(startup_done, pdMS_TO_TICKS(5000)) != pdTRUE ||
      !startup_ok.load()) {
    host_requested = false;
    enabled = false;
    ble_gap_adv_stop();
    if (connected.load()) {
      terminate(conn_handle.load(), DisconnectReason::ServerStop);
    }
    advertising = false;
    oslog.println("BLE ERROR operation=host-start");
    return false;
  }
  oslog.printf("BLE BEGIN name=%s service=%s mtu_max=%u bonds=%lu pair=%s "
               "adv_ver=%u boot16=%04x credits=%u\n",
               name_text, kServiceUuid, unsigned(kMaxMtu),
               static_cast<unsigned long>(bonds.load()),
               config::pair_name(mode), unsigned(kAdvertVersion),
               unsigned(boot16), unsigned(responses.capacity()));
  return true;
}

const char *local_name() { return name_text; }
const char *info_json() { return info_text; }
config::PairMode pair_mode() { return mode; }

void disconnect(DisconnectReason reason) {
  const auto handle = conn_handle.load();
  if (connected.load() && handle != BLE_HS_CONN_HANDLE_NONE)
    terminate(handle, reason);
}

void disconnect_and_wait(DisconnectReason reason, std::uint32_t timeout_ms) {
  disconnect(reason);
  for (std::uint32_t waited = 0; waited < timeout_ms && connected.load();
       waited += 10)
    vTaskDelay(pdMS_TO_TICKS(10));
  // The callback clears `connected` before the generation moves on, so a link
  // still up here is the one that was asked to close.
  const std::uint32_t closed = generation.load();
  if (connected.load() && close_claim.claim(closed))
    record_disconnect(reason, kEventLinkBle, closed, 0, 0,
                      esp_timer_get_time());
}

void clear_bonds() {
  if (!enabled.load() || !clear_mutex)
    return;
  if (xTaskGetCurrentTaskHandle() == host_task_handle) {
    clear_bonds_on_host(nullptr);
    return;
  }
  xSemaphoreTake(clear_mutex, portMAX_DELAY);
  // Drain a late completion before scheduling a new host operation.
  xSemaphoreTake(clear_done, 0);
  ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &clear_event);
  xSemaphoreTake(clear_done, portMAX_DELAY);
  xSemaphoreGive(clear_mutex);
}

void publish_snapshot(Characteristic which, const char *json,
                      std::size_t length) {
  if (!enabled.load() || !json || length > kMaxJson)
    return;
  portENTER_CRITICAL(&snapshot_lock);
  char *value = which == Status ? status_text : live_text;
  std::memcpy(value, json, length);
  value[length] = '\0';
  (which == Status ? status_length : live_length) = length;
  portEXIT_CRITICAL(&snapshot_lock);
  pending_snapshots.fetch_or(1U << which);
  ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &snapshot_event);
}

void publish_live(const char *json, std::size_t length) {
  publish_snapshot(Live, json, length);
}
void publish_status(const char *json, std::size_t length) {
  publish_snapshot(Status, json, length);
}

void publish_advert(const AdvertState &state) {
  if (!enabled.load())
    return;
  const auto next = pack_advert(state);
  // The first publish always reaches the host, which takes its flags as the
  // baseline for flips even when they match the sync placeholder.
  const bool first = !advert_published.exchange(true);
  if (desired_advert.exchange(next) != next || applied_advert.load() != next ||
      first)
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &advert_event);
}

namespace {
// Queues one frame, waiting for a credit in steps. `budget_us` bounds a wait
// with no credit returned; the host closes a stalled link sooner.
bool deliver_frame(const std::uint8_t *frame, std::size_t length,
                   std::uint32_t expected_generation, WaitStep between_steps,
                   void *context, std::int64_t budget_us) {
  if (!enabled.load() || !response_mutex)
    return false;
  const auto session =
      expected_generation ? expected_generation : generation.load();
  if (xTaskGetCurrentTaskHandle() == host_task_handle) {
    // The host cannot wait for itself: one attempt, never blocking.
    if (xSemaphoreTake(response_mutex, 0) != pdTRUE)
      return false;
    const bool ok =
        enqueue_response(frame, length, session) == ossync::EnqueueResult::Ok;
    xSemaphoreGive(response_mutex);
    return ok;
  }
  const TickType_t step = pdMS_TO_TICKS(options.wait_step_ms);
  std::int64_t waited_from = esp_timer_get_time();
  while (xSemaphoreTake(response_mutex, step) != pdTRUE) {
    if ((between_steps && !between_steps(context)) ||
        esp_timer_get_time() - waited_from >= budget_us)
      return false;
  }
  bool ok = false;
  waited_from = esp_timer_get_time();
  for (;;) {
    // Drop a stale wake before looking, so a credit returned after the look
    // still ends the wait below.
    xSemaphoreTake(space_event, 0);
    const auto result = enqueue_response(frame, length, session);
    if (result != ossync::EnqueueResult::Full) {
      ok = result == ossync::EnqueueResult::Ok;
      break;
    }
    if (xSemaphoreTake(space_event, step) == pdTRUE)
      waited_from = esp_timer_get_time();
    if (between_steps && !between_steps(context))
      break;
    if (esp_timer_get_time() - waited_from >= budget_us)
      break;
  }
  xSemaphoreGive(response_mutex);
  return ok;
}

std::size_t encode_error(std::uint8_t (&frame)[3 + 64], Op op, Error code,
                         const char *detail) {
  frame[0] = kFrameError;
  frame[1] = static_cast<std::uint8_t>(op);
  frame[2] = static_cast<std::uint8_t>(code);
  std::size_t length = 3;
  if (detail) {
    const auto detail_length = std::strlen(detail);
    const auto copy = detail_length > 64 ? 64 : detail_length;
    std::memcpy(frame + 3, detail, copy);
    length += copy;
  }
  oslog.printf("BLE ERROR op=0x%02x code=%u\n", unsigned(op), unsigned(code));
  return length;
}
} // namespace

bool deliver(const std::uint8_t *frame, std::size_t length,
             std::uint32_t expected_generation, WaitStep between_steps,
             void *context) {
  // Twice the progress timeout: the host normally closes a stalled link first.
  return deliver_frame(frame, length, expected_generation, between_steps,
                       context, 2LL * options.progress_timeout_ms * 1000LL);
}

bool deliver_error(Op op, Error code, const char *detail,
                   std::uint32_t expected_generation, WaitStep between_steps,
                   void *context) {
  std::uint8_t frame[3 + 64]{};
  const auto length = encode_error(frame, op, code, detail);
  return deliver(frame, length, expected_generation, between_steps, context);
}

bool send_response(const std::uint8_t *frame, std::size_t length,
                   std::uint32_t expected_generation) {
  return deliver_frame(frame, length, expected_generation, nullptr, nullptr,
                       kSendBudgetUs);
}

bool send_error(Op op, Error code, const char *detail,
                std::uint32_t expected_generation) {
  std::uint8_t frame[3 + 64]{};
  const auto length = encode_error(frame, op, code, detail);
  return send_response(frame, length, expected_generation);
}

ossync::EnqueueResult try_send_response(const std::uint8_t *frame,
                                        std::size_t length,
                                        std::uint32_t expected_generation) {
  if (!enabled.load() || !response_mutex)
    return ossync::EnqueueResult::Closed;
  const auto session =
      expected_generation ? expected_generation : generation.load();
  // The storage worker holds the lock across a whole credit wait. Never wait
  // for it here.
  if (xSemaphoreTake(response_mutex, 0) != pdTRUE)
    return ossync::EnqueueResult::Full;
  const auto result = enqueue_response(frame, length, session);
  xSemaphoreGive(response_mutex);
  return result;
}

// Future hook for a board wake gesture, no caller yet (see ble_sync.h).
void wake_advertising() {
  if (enabled.load())
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &wake_event);
}

std::uint16_t payload_max() { return wire::response_capacity(mtu.load()); }
std::uint32_t connection_generation() { return generation.load(); }

PairingState pairing() {
  PairingState state;
  state.active = pairing_active.load();
  state.passkey = passkey.load();
  state.deadline_us = pairing_deadline.load();
  if (state.active && esp_timer_get_time() > state.deadline_us)
    state.active = false;
  return state;
}

LinkState link() {
  LinkState state;
  state.enabled = enabled.load();
  state.advertising = advertising.load();
  state.connected = connected.load();
  state.authenticated = authenticated.load();
  state.mtu = mtu.load();
  state.bonds = bonds.load();
  state.advert_flags = static_cast<std::uint8_t>(applied_advert.load() & 0xffU);
  return state;
}
std::uint32_t ui_generation() { return ui.load(); }
} // namespace ble
