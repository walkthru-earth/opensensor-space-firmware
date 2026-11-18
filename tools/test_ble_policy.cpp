// Exercise the real BLE link policy (notify credits, the response queue, slow
// peer close, advert flag flips, advert window, interval mode, parameter
// validation and the link authentication gates) on the host. Run with
// tools/test_ble_policy.py.
#include <ble_link_auth.h>
#include <ble_policy.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

using ble::AdvertWindow;
using ble::CloseStart;
using ble::ConnParams;
using ble::FlagWatch;
using ble::IntervalPolicy;
using ble::NotifyCredits;
using ble::NotifyStatus;
using ble::Options;
using ble::ResponseQueue;
using ble::StallClose;
using ble::StreamChange;
using ble::TxSlot;
using ossync::EnqueueResult;

namespace {
constexpr std::int64_t kMs = 1000;
constexpr std::int64_t kSec = 1000 * kMs;

static_assert(ble::valid_options(Options{}), "default options are valid");
static_assert(ble::valid_conn_params(Options{}.active), "default active");
static_assert(ble::valid_conn_params(Options{}.relaxed), "default relaxed");

NotifyCredits opened(std::uint32_t generation, std::int64_t now_us) {
  NotifyCredits credits;
  credits.configure(4, 15000);
  credits.open(generation, now_us);
  return credits;
}

void exhaust(NotifyCredits &credits, std::uint32_t generation,
             std::int64_t now_us) {
  while (credits.available())
    assert(credits.take(generation, now_us) == EnqueueResult::Ok);
  assert(credits.take(generation, now_us) == EnqueueResult::Full);
}

void test_credit_exhaustion() {
  NotifyCredits credits = opened(7, 0);
  assert(credits.capacity() == 4 && credits.available() == 4);
  assert(credits.idle() && credits.open_for(7));
  for (int i = 0; i < 4; ++i)
    assert(credits.take(7, 0) == EnqueueResult::Ok);
  assert(credits.take(7, 0) == EnqueueResult::Full);
  assert(credits.available() == 0 && !credits.idle());
  assert(credits.on_notify_tx(7, NotifyStatus::Sent, 1 * kMs));
  assert(credits.available() == 1);
  assert(credits.take(7, 2 * kMs) == EnqueueResult::Ok);
  assert(credits.take(7, 2 * kMs) == EnqueueResult::Full);
  // Every credit returning makes the link idle again.
  for (int i = 0; i < 4; ++i)
    assert(credits.on_notify_tx(7, NotifyStatus::Sent, 3 * kMs));
  assert(credits.idle());
  // An extra Sent never pushes past capacity.
  assert(credits.on_notify_tx(7, NotifyStatus::Sent, 4 * kMs));
  assert(credits.available() == 4);
}

void test_refused_and_failed() {
  NotifyCredits credits = opened(3, 0);
  const std::int64_t t0 = 5 * kSec;
  exhaust(credits, 3, t0);
  assert(credits.on_notify_tx(3, NotifyStatus::Refused, t0 + 10 * kSec));
  assert(credits.available() == 0);
  assert(credits.take(3, t0 + 10 * kSec) == EnqueueResult::Full);
  // Refused is no progress, so the stall still fires 15 s after exhaustion.
  assert(!credits.stalled(t0 + 15 * kSec - 1));
  assert(credits.stalled(t0 + 15 * kSec));

  NotifyCredits failing = opened(4, 0);
  assert(failing.take(4, 0) == EnqueueResult::Ok);
  assert(failing.on_notify_tx(4, NotifyStatus::Failed, 1 * kMs));
  assert(!failing.open_for(4));
  assert(failing.take(4, 2 * kMs) == EnqueueResult::Closed);
  assert(!failing.stalled(100 * kSec));
  // Events after the close belong to no open generation.
  assert(!failing.on_notify_tx(4, NotifyStatus::Sent, 3 * kMs));
}

void test_stale_generation() {
  NotifyCredits credits = opened(1, 0);
  assert(credits.take(1, 0) == EnqueueResult::Ok);
  assert(credits.available() == 3);
  assert(!credits.on_notify_tx(2, NotifyStatus::Sent, 1 * kMs));
  assert(!credits.on_notify_tx(2, NotifyStatus::Failed, 1 * kMs));
  assert(credits.available() == 3 && credits.open_for(1));
  assert(credits.take(2, 1 * kMs) == EnqueueResult::Closed);

  credits.open(2, 2 * kMs);
  assert(credits.available() == 4 && credits.open_for(2));
  assert(!credits.open_for(1));
  assert(credits.take(1, 3 * kMs) == EnqueueResult::Closed);
  assert(!credits.on_notify_tx(1, NotifyStatus::Sent, 3 * kMs));
  assert(credits.take(2, 3 * kMs) == EnqueueResult::Ok);
  assert(credits.available() == 3);
}

void test_disconnect_while_waiting() {
  NotifyCredits credits = opened(9, 0);
  exhaust(credits, 9, 1 * kSec);
  credits.close();
  assert(!credits.open_for(9));
  assert(credits.take(9, 2 * kSec) == EnqueueResult::Closed);
  assert(!credits.stalled(100 * kSec));
  assert(!credits.on_notify_tx(9, NotifyStatus::Sent, 3 * kSec));
}

void test_progress_timeout() {
  NotifyCredits credits = opened(5, 0);
  const std::int64_t t0 = 1 * kSec;
  exhaust(credits, 5, t0);
  assert(!credits.stalled(t0 + 14999 * kMs));
  assert(credits.stalled(t0 + 15 * kSec));

  // A return at t0 + 10 s moves the clock, and a take while busy does not.
  NotifyCredits reset = opened(5, 0);
  exhaust(reset, 5, t0);
  assert(reset.on_notify_tx(5, NotifyStatus::Sent, t0 + 10 * kSec));
  assert(reset.available() == 1);
  assert(!reset.stalled(t0 + 15 * kSec));
  assert(reset.take(5, t0 + 12 * kSec) == EnqueueResult::Ok);
  assert(!reset.stalled(t0 + 25 * kSec - 1));
  assert(reset.stalled(t0 + 25 * kSec));

  // One frame refused forever stalls even with credits to spare.
  NotifyCredits single = opened(8, 0);
  assert(single.take(8, 2 * kSec) == EnqueueResult::Ok);
  assert(single.on_notify_tx(8, NotifyStatus::Refused, 3 * kSec));
  assert(!single.stalled(17 * kSec - 1));
  assert(single.stalled(17 * kSec));

  // A long idle spell before a burst never counts against the host.
  NotifyCredits idle = opened(6, 0);
  assert(!idle.stalled(60 * kSec));
  exhaust(idle, 6, 60 * kSec);
  assert(!idle.stalled(60 * kSec + 1 * kMs));
  assert(!idle.stalled(75 * kSec - 1));
  assert(idle.stalled(75 * kSec));

  // Every credit home means no stall however long ago progress was.
  NotifyCredits home = opened(8, 0);
  assert(home.take(8, 0) == EnqueueResult::Ok);
  assert(home.on_notify_tx(8, NotifyStatus::Sent, 1 * kSec));
  assert(home.idle() && !home.stalled(1000 * kSec));
  // The next burst starts a fresh clock.
  assert(home.take(8, 1000 * kSec) == EnqueueResult::Ok);
  assert(!home.stalled(1015 * kSec - 1));
  assert(home.stalled(1015 * kSec));
}

void test_cancel() {
  NotifyCredits credits = opened(2, 0);
  assert(credits.take(2, 0) == EnqueueResult::Ok);
  assert(credits.available() == 3);
  credits.cancel(1);
  assert(credits.available() == 3);
  credits.cancel(2);
  assert(credits.available() == 4 && credits.idle());
  credits.cancel(2);
  assert(credits.available() == 4);
  exhaust(credits, 2, 1 * kSec);
  credits.cancel(2);
  assert(credits.available() == 1);
  // Cancelling every credit makes the link idle, so it cannot stall.
  credits.cancel(2);
  credits.cancel(2);
  credits.cancel(2);
  assert(credits.idle() && !credits.stalled(100 * kSec));
}

void test_configure_clamps() {
  NotifyCredits credits;
  credits.configure(0, 15000);
  assert(credits.capacity() == 1);
  assert(!credits.open_for(0));
  credits.open(1, 0);
  assert(credits.take(1, 0) == EnqueueResult::Ok);
  assert(credits.take(1, 0) == EnqueueResult::Full);
  assert(credits.on_notify_tx(1, NotifyStatus::Sent, 1 * kMs));
  assert(credits.idle());
}

void test_advert_window() {
  AdvertWindow window;
  assert(!window.fast(0) && window.remaining_ms(0) == 0);
  const std::int64_t now = 100 * kSec;
  window.extend(now, 30000);
  assert(window.fast(now));
  assert(window.remaining_ms(now) == 30000);
  assert(window.fast(now + 30 * kSec - 1));
  assert(!window.fast(now + 30 * kSec));
  assert(window.remaining_ms(now + 30 * kSec) == 0);
  // 0.5 ms left rounds up to 1 ms.
  assert(window.remaining_ms(now + 29999 * kMs + 500) == 1);
  assert(window.remaining_ms(now + 1) == 30000);
  // A shorter window never shortens the fast spell.
  window.extend(now + 1 * kSec, 5000);
  assert(window.fast(now + 29 * kSec));
  assert(window.remaining_ms(now + 10 * kSec) == 20000);
  // A longer one extends it.
  window.extend(now + 10 * kSec, 60000);
  assert(window.remaining_ms(now + 10 * kSec) == 60000);
  window.clear();
  assert(!window.fast(now + 10 * kSec));
  assert(window.remaining_ms(now + 10 * kSec) == 0);
}

void test_interval_policy() {
  using Action = IntervalPolicy::Action;
  IntervalPolicy policy;
  std::uint32_t rearm = 99;
  const std::int64_t t = 10 * kSec;
  policy.connect(t);
  assert(!policy.relaxed());
  assert(policy.on_timer(t + 20 * kSec, 60000, rearm) == Action::None);
  assert(rearm == 40000);
  assert(policy.on_timer(t + 60 * kSec - 1, 60000, rearm) == Action::None);
  assert(rearm == 1);
  assert(policy.on_timer(t + 60 * kSec, 60000, rearm) ==
         Action::RequestRelaxed);
  assert(rearm == 0 && policy.relaxed());
  rearm = 99;
  assert(policy.on_timer(t + 61 * kSec, 60000, rearm) == Action::None);
  assert(rearm == 0 && policy.relaxed());
  // A response frame never wakes a relaxed link.
  policy.on_activity(t + 62 * kSec);
  assert(policy.relaxed());

  assert(policy.on_request(t + 70 * kSec) == Action::RequestActive);
  assert(!policy.relaxed());
  assert(policy.on_request(t + 71 * kSec) == Action::None);
  // Activity pushes the due time.
  policy.on_activity(t + 100 * kSec);
  assert(policy.on_timer(t + 131 * kSec, 60000, rearm) == Action::None);
  assert(rearm == 29000);
  assert(policy.on_timer(t + 160 * kSec, 60000, rearm) ==
         Action::RequestRelaxed);
  // A refused relax request keeps the link active, and the timer asks again.
  policy.request_failed();
  assert(!policy.relaxed());
  assert(policy.on_timer(t + 161 * kSec, 60000, rearm) ==
         Action::RequestRelaxed);
  // A refused active request leaves it relaxed, so the next write retries.
  assert(policy.on_request(t + 162 * kSec) == Action::RequestActive);
  policy.request_failed();
  assert(policy.relaxed());
  assert(policy.on_request(t + 163 * kSec) == Action::RequestActive);

  // A reconnect starts active again.
  policy.connect(t + 200 * kSec);
  assert(!policy.relaxed());
  assert(policy.on_timer(t + 200 * kSec, 60000, rearm) == Action::None);
  assert(rearm == 60000);
}

void test_conn_params() {
  using ble::valid_conn_params;
  assert(valid_conn_params({12, 24, 0, 400}));
  assert(valid_conn_params({80, 160, 4, 400}));
  assert(valid_conn_params({12, 12, 0, 400}));
  assert(!valid_conn_params({}));
  assert(!valid_conn_params({6, 24, 0, 400}));   // below 15 ms
  assert(!valid_conn_params({11, 24, 0, 400}));  // below 15 ms
  assert(!valid_conn_params({24, 12, 0, 400}));  // max below min
  assert(!valid_conn_params({12, 20, 0, 400}));  // max not 15 ms above min
  assert(!valid_conn_params({12, 23, 0, 400}));  // one unit short
  assert(!valid_conn_params({16, 16, 0, 400}));  // equal only at 15 ms
  assert(valid_conn_params({12, 24, 30, 400}));  // latency limit
  assert(!valid_conn_params({12, 24, 31, 400})); // latency over 30
  assert(valid_conn_params({12, 24, 0, 200}));   // 2 s timeout
  assert(valid_conn_params({12, 24, 0, 600}));   // 6 s timeout
  assert(!valid_conn_params({12, 24, 0, 100}));  // 1 s timeout
  assert(!valid_conn_params({12, 24, 0, 199}));
  assert(!valid_conn_params({12, 24, 0, 601}));
  assert(!valid_conn_params({12, 24, 0, 700})); // 7 s timeout
  // 200 ms times 10 is exactly 2 s effective, but a 6 s timeout is not over
  // three effective intervals.
  assert(!valid_conn_params({80, 160, 9, 600}));
  // 200 ms times 11 is a 2.2 s effective interval. Any effective interval
  // above 2 s also fails the timeout rule, since the timeout is at most 6 s.
  assert(!valid_conn_params({80, 160, 10, 600}));
  assert(!valid_conn_params({800, 1600, 0, 600}));  // 2 s, 6 s not over 3x
  assert(!valid_conn_params({1600, 3200, 0, 600})); // 4 s interval
  // 200 ms times 5 is 1 s effective, so the timeout must exceed 3 s.
  assert(!valid_conn_params({80, 160, 4, 300}));
  assert(valid_conn_params({80, 160, 4, 301}));
  // 200 ms times 8 is 1.6 s effective, so 4.8 s is not enough.
  assert(!valid_conn_params({80, 160, 7, 480}));
  assert(valid_conn_params({80, 160, 7, 481}));
}

void test_options() {
  assert(ble::valid_options(Options{}));
  Options o;
  o.notify_credits = 0;
  assert(!ble::valid_options(o));
  o = Options{};
  o.wait_step_ms = o.progress_timeout_ms;
  assert(!ble::valid_options(o));
  o.wait_step_ms = o.progress_timeout_ms + 1;
  assert(!ble::valid_options(o));
  o.wait_step_ms = o.progress_timeout_ms - 1;
  assert(ble::valid_options(o));
  o = Options{};
  o.wait_step_ms = 0;
  assert(!ble::valid_options(o));
  o = Options{};
  o.refused_retry_ms = 0;
  assert(!ble::valid_options(o));
  o.refused_retry_ms = o.progress_timeout_ms;
  assert(!ble::valid_options(o));
  o.refused_retry_ms = o.progress_timeout_ms - 1;
  assert(ble::valid_options(o));
  o = Options{};
  o.progress_timeout_ms = 0;
  assert(!ble::valid_options(o));
  o = Options{};
  o.data_len_octets = 252;
  assert(!ble::valid_options(o));
  o.data_len_octets = 27;
  assert(ble::valid_options(o));
  o.data_len_octets = 26;
  assert(!ble::valid_options(o));
  o = Options{};
  o.active = {12, 20, 0, 400};
  assert(!ble::valid_options(o));
  o = Options{};
  o.relaxed = {80, 160, 10, 600};
  assert(!ble::valid_options(o));
  o = Options{};
  o.fast_adv_min = 31;
  assert(!ble::valid_options(o));
  o = Options{};
  o.slow_adv_max = 16385;
  assert(!ble::valid_options(o));
  o = Options{};
  o.notify_credits = ble::kMaxNotifyCredits;
  assert(ble::valid_options(o));
  o.notify_credits = ble::kMaxNotifyCredits + 1;
  assert(!ble::valid_options(o));
}

ResponseQueue queue_for(std::uint32_t generation, std::uint8_t slots = 4) {
  ResponseQueue queue;
  queue.configure(slots, 15000);
  queue.open(generation, 0);
  return queue;
}

// Reserves and publishes one frame whose length tags it.
void push(ResponseQueue &queue, std::uint32_t generation, std::uint16_t tag) {
  const auto r = queue.reserve(generation, 0);
  assert(r.result == EnqueueResult::Ok);
  assert(queue.publish(r, generation, tag, 7));
}

std::uint16_t pop(ResponseQueue &queue, std::uint32_t generation) {
  TxSlot slot;
  std::uint8_t index = 0;
  assert(queue.front(slot, index));
  assert(slot.session == generation && slot.handle == 7);
  const auto outcome = queue.on_notify_tx(generation, NotifyStatus::Sent, 0);
  assert(outcome.current && !outcome.stalled);
  return slot.length;
}

void test_queue_order_and_wrap() {
  auto queue = queue_for(1, 3);
  TxSlot slot;
  std::uint8_t index = 0;
  assert(!queue.front(slot, index) && queue.idle());
  std::uint16_t next = 1, expect = 1;
  // Ten frames through three slots, never more than three in flight, come out
  // in order across every wrap.
  for (int round = 0; round < 4; ++round) {
    while (queue.queued() < 3 && next <= 10)
      push(queue, 1, next++);
    if (next <= 10)
      assert(queue.reserve(1, 0).result == EnqueueResult::Full);
    while (queue.queued())
      assert(pop(queue, 1) == expect++);
  }
  assert(expect == 11 && queue.idle());
}

// The review's race: a frame is reserved while two are queued, the stream
// closes and reopens for the same generation while the bytes are copied, and
// the late publish must not land in the new ring.
void test_reopen_same_generation_rejects_late_publish() {
  auto queue = queue_for(5, 3);
  push(queue, 5, 1);
  push(queue, 5, 2);
  const auto late = queue.reserve(5, 0);
  assert(late.result == EnqueueResult::Ok && late.slot == 2);
  const auto before = queue.epoch();
  queue.close();
  assert(queue.epoch() != before);
  queue.open(5, 0);
  assert(queue.open_for(5) && queue.idle());
  assert(!queue.publish(late, 5, 3, 7));
  TxSlot slot;
  std::uint8_t index = 0;
  assert(!queue.front(slot, index));
  // The new stream starts at slot 0 and keeps its own order.
  push(queue, 5, 10);
  assert(queue.front(slot, index) && index == 0 && slot.length == 10);
  assert(pop(queue, 5) == 10 && queue.idle());
}

void test_close_rejects_publish() {
  auto queue = queue_for(2);
  const auto r = queue.reserve(2, 0);
  queue.close();
  assert(!queue.publish(r, 2, 1, 7));
  assert(queue.reserve(2, 0).result == EnqueueResult::Closed);
  // A reservation for one generation never publishes into another.
  queue.open(3, 0);
  const auto other = queue.reserve(3, 0);
  assert(!queue.publish(other, 2, 1, 7));
  assert(queue.publish(other, 3, 1, 7));
}

void test_queue_refused_failed_and_foreign() {
  auto queue = queue_for(4);
  push(queue, 4, 1);
  push(queue, 4, 2);
  auto outcome = queue.on_notify_tx(4, NotifyStatus::Refused, 1 * kSec);
  assert(outcome.current && !outcome.stalled && queue.queued() == 2);
  // Refused until the progress timeout marks the stream stalled.
  outcome = queue.on_notify_tx(4, NotifyStatus::Refused, 15 * kSec);
  assert(outcome.current && outcome.stalled && queue.queued() == 2);
  // A frame of an older generation clears without touching credits.
  const auto epoch = queue.epoch();
  outcome = queue.on_notify_tx(3, NotifyStatus::Sent, 15 * kSec);
  assert(!outcome.current && queue.queued() == 0 && queue.epoch() != epoch);
  queue.open(4, 16 * kSec);
  push(queue, 4, 1);
  outcome = queue.on_notify_tx(4, NotifyStatus::Failed, 16 * kSec);
  assert(outcome.current && !outcome.stalled && queue.queued() == 0);
  assert(!queue.open_for(4));
  assert(queue.reserve(4, 0).result == EnqueueResult::Closed);
}

void test_configure_caps_slots() {
  ResponseQueue queue;
  queue.configure(200, 15000);
  assert(queue.capacity() == ble::kMaxNotifyCredits);
  queue.open(1, 0);
  for (int i = 0; i < ble::kMaxNotifyCredits; ++i)
    push(queue, 1, static_cast<std::uint16_t>(i + 1));
  assert(queue.reserve(1, 0).result == EnqueueResult::Full);
}

void test_response_subscribe() {
  using ble::response_subscribe;
  // The first subscribe after connect finds the stream already open.
  static_assert(response_subscribe(true, false, true) == StreamChange::None,
                "first subscribe");
  static_assert(response_subscribe(false, false, true) == StreamChange::Close,
                "unsubscribe closes at once");
  static_assert(response_subscribe(false, false, false) == StreamChange::Close,
                "a second unsubscribe is harmless");
  static_assert(response_subscribe(true, false, false) == StreamChange::Reopen,
                "resubscribe opens a new generation");
  static_assert(response_subscribe(false, true, true) == StreamChange::None,
                "disconnect leaves it to DISCONNECT");
  static_assert(response_subscribe(true, true, false) == StreamChange::None,
                "disconnect never reopens");
  // Unsubscribe, resubscribe: the late frame of the old stream cannot land.
  std::uint32_t generation = 8;
  auto queue = queue_for(generation);
  push(queue, generation, 1);
  const auto late = queue.reserve(generation, 0);
  assert(response_subscribe(false, false, queue.open_for(generation)) ==
         StreamChange::Close);
  queue.close();
  assert(response_subscribe(true, false, queue.open_for(generation)) ==
         StreamChange::Reopen);
  queue.open(++generation, 0);
  assert(!queue.publish(late, generation - 1, 2, 7));
  assert(!queue.publish(late, generation, 2, 7));
  assert(queue.reserve(generation - 1, 0).result == EnqueueResult::Closed);
  assert(queue.idle());
}

void test_classify_close() {
  constexpr int kAlready = 2; // BLE_HS_EALREADY
  static_assert(ble::classify_close(0, kAlready) == CloseStart::Started, "");
  static_assert(ble::classify_close(kAlready, kAlready) ==
                    CloseStart::AlreadyClosing,
                "");
  static_assert(ble::classify_close(7, kAlready) == CloseStart::Failed, "");
}

void test_stall_close() {
  StallClose stall;
  // Not stalled: retry, nothing logged.
  assert(stall.on_refused(3, false) == StallClose::Action::Retry);
  // Stalled while a reboot terminate is pending: EALREADY ends the retries
  // and the stall is logged a single time.
  assert(stall.on_refused(3, true) == StallClose::Action::Close);
  assert(stall.log(3));
  assert(stall.on_close(3, CloseStart::AlreadyClosing) ==
         StallClose::Action::Wait);
  for (int i = 0; i < 100; ++i) {
    assert(stall.on_refused(3, true) == StallClose::Action::Wait);
    assert(!stall.log(3));
  }
  // A failed terminate keeps retrying but never logs twice.
  assert(stall.on_refused(4, true) == StallClose::Action::Close);
  assert(stall.log(4));
  assert(stall.on_close(4, CloseStart::Failed) == StallClose::Action::Retry);
  assert(stall.on_refused(4, true) == StallClose::Action::Close);
  assert(!stall.log(4));
  assert(stall.on_close(4, CloseStart::Started) == StallClose::Action::Wait);
  assert(stall.on_refused(4, true) == StallClose::Action::Wait);
  // A new session is judged afresh.
  assert(stall.on_refused(5, true) == StallClose::Action::Close);
  assert(stall.log(5));
}

void test_flag_watch() {
  FlagWatch watch;
  // Boot: the placeholder was applied at sync without a publish; the first
  // publish (SD present, no UTC) only sets the baseline.
  assert(!watch.on_publish(0x05));
  assert(!watch.on_publish(0x05));
  // A real flip, then back.
  assert(watch.on_publish(0x04));
  assert(watch.on_publish(0x05));
  assert(!watch.on_publish(0x05));
  // A first publish equal to the placeholder is no flip either.
  FlagWatch same;
  assert(!same.on_publish(0x01));
  assert(same.on_publish(0x03));
}
// Link authentication (issue #51). The transport answers what these decide;
// the frames below are built the way ble_sync.cpp encodes them.
std::array<std::uint8_t, 7> error_frame(ble::Op op, ble::Error code) {
  return {ble::kFrameError, op, code, 'a', 'u', 't', 'h'};
}

// What one ble_sync.cpp gate does for a frame: the cached flag, the ERROR 14
// exception, then NimBLE's live state, counting live lookups.
struct Gate {
  bool cached = false;
  bool live_secure = false;
  int lookups = 0;
  bool send(std::span<const std::uint8_t> frame) {
    return ble::may_send(cached, frame, [this] {
      ++lookups;
      return live_secure;
    });
  }
};

static_assert(ble::should_latch(true, false, true), "first proof latches");
static_assert(!ble::should_latch(true, true, true), "latched once");
static_assert(!ble::should_latch(false, false, true), "another handle");
static_assert(!ble::should_latch(true, false, false), "insecure never");
static_assert(ble::control_refusal(false).code == ble::kErrAuth,
              "auth refusal is 14");
static_assert(ble::control_refusal(true).code == ble::kErrBusy,
              "queue refusal is 7");
// The full send or refuse table. A cached-only gate (the issue #51 bug)
// fails the live-secure row.
static_assert(ble::gate_allows(true, false, false), "cached link sends");
static_assert(ble::gate_allows(false, false, true), "stale flag, secure link");
static_assert(ble::gate_allows(false, true, false), "ERROR 14 always out");
static_assert(!ble::gate_allows(false, false, false), "insecure refused");
// A late ENC_CHANGE after a via= latch announces nothing.
static_assert(ble::enc_change_announces(false, true), "first ENC_CHANGE");
static_assert(!ble::enc_change_announces(true, true), "already latched");
static_assert(!ble::enc_change_announces(false, false), "insecure");

void test_stale_flag_secure_link_is_served() {
  // The failing bench connect: CONNECT before encryption leaves the flag
  // false and ENC_CHANGE never arrives, yet NimBLE says the link is secure.
  Gate gate{.cached = false, .live_secure = true, .lookups = 0};
  const std::uint8_t file_frame[] = {ble::kFrameFile, 0, 0, 0, 0, 'a'};
  const std::uint8_t list_end[] = {ble::kFrameListEnd, 1, 0, 0, 0};
  assert(gate.send(file_frame));
  assert(gate.send(list_end));
  assert(gate.lookups == 2);
  // A busy ERROR on that link still goes out, so a real 7 stays visible.
  const auto busy = error_frame(ble::kOpList, ble::kErrBusy);
  assert(gate.send(busy));

  // The first GATT access then latches the flag, and the gates stop asking
  // NimBLE for every frame.
  const bool current_link = true;
  assert(ble::should_latch(current_link, gate.cached, gate.live_secure));
  gate.cached = true;
  gate.lookups = 0;
  assert(gate.send(file_frame) && gate.send(list_end) && gate.lookups == 0);
  assert(!ble::should_latch(current_link, gate.cached, gate.live_secure));
}

void test_insecure_link_gets_visible_error_14() {
  Gate gate{.cached = false, .live_secure = false, .lookups = 0};
  // NimBLE's ATT check refuses an insecure Control write first. If it ever
  // lets one through, access() queues this refusal as the fallback.
  const auto refusal = ble::control_refusal(false);
  assert(refusal.code == ble::kErrAuth &&
         std::strcmp(refusal.detail, "auth") == 0);
  const auto frame = error_frame(ble::kOpList, refusal.code);
  assert(ble::is_auth_refusal(frame));
  // The gate lets it out without asking NimBLE, so the client sees it.
  assert(gate.send(frame) && gate.lookups == 0);
  // Nothing else reaches an insecure link: no data, no snapshot, no other
  // ERROR, and no latch.
  const std::uint8_t file_frame[] = {ble::kFrameFile, 0, 0, 0, 0, 'a'};
  const std::uint8_t status_json[] = {'{', '}'};
  assert(!gate.send(file_frame));
  assert(!gate.send(status_json));
  assert(!gate.send(error_frame(ble::kOpList, ble::kErrBusy)));
  assert(!ble::should_latch(true, gate.cached, gate.live_secure));
}

void test_no_auth_refusal_answers_busy() {
  // An auth reason is never code 7, and code 7 is only the full queue.
  assert(ble::control_refusal(false).code != ble::kErrBusy);
  const auto queue = ble::control_refusal(true);
  assert(queue.code == ble::kErrBusy &&
         std::strcmp(queue.detail, "queue-full") == 0);
  // Only a whole ERROR 14 frame counts as the auth refusal.
  assert(!ble::is_auth_refusal(error_frame(ble::kOpList, ble::kErrBusy)));
  const std::uint8_t short_frame[] = {ble::kFrameError, ble::kOpList};
  assert(!ble::is_auth_refusal(short_frame));
  const std::uint8_t not_error[] = {ble::kFrameFile, ble::kOpList,
                                    ble::kErrAuth};
  assert(!ble::is_auth_refusal(not_error));
  assert(!ble::is_auth_refusal(std::span<const std::uint8_t>{}));
}
// The bonded reconnect from the 2026-10-18 bench. A model of ble_sync.cpp's
// gap_event built from the same decisions, in NimBLE's event order.
// test_ble_policy.py checks that ble_sync.cpp wires them the same way.
constexpr unsigned kResponse = 4; // ble_sync.cpp Characteristic::Response
struct Transport {
  bool connected = false;
  bool authenticated = false;
  bool live_secure = false;
  std::uint16_t handle = ble::PendingCccd::kNone;
  std::uint32_t generation = 0;
  std::array<bool, 5> subscribed{};
  ble::PendingCccd early;
  ResponseQueue queue = queue_for(0);
  int fault_logs = 0;
  std::uint32_t refusal_logged = ~0U;

  void apply(unsigned which, bool notify, bool closing) {
    subscribed[which] = notify;
    if (which != kResponse || !connected)
      return;
    const auto change =
        ble::response_subscribe(notify, closing, queue.open_for(generation));
    if (change == StreamChange::Close)
      queue.close();
    else if (change == StreamChange::Reopen)
      queue.open(++generation, 0);
  }
  void on_subscribe(std::uint16_t event_handle, unsigned which, bool notify,
                    bool closing = false) {
    switch (ble::route_subscribe(connected, event_handle == handle)) {
    case ble::SubscribeRoute::Drop:
      return;
    case ble::SubscribeRoute::Hold:
      early.hold(event_handle, which, notify);
      return;
    case ble::SubscribeRoute::Apply:
      if (!closing && ble::should_latch(true, authenticated, live_secure))
        authenticated = true;
      apply(which, notify, closing);
    }
  }
  // ENC_CHANGE before CONNECT goes nowhere, the handle is not known yet.
  void on_enc_change(std::uint16_t event_handle) {
    live_secure = true;
    if (connected && event_handle == handle)
      authenticated = true;
  }
  void on_connect(std::uint16_t event_handle) {
    ++generation;
    handle = event_handle;
    authenticated = false;
    connected = true;
    subscribed.fill(false);
    queue.open(generation, 0);
    early.replay(event_handle, [this](unsigned which, bool notify) {
      apply(which, notify, false);
    });
    if (ble::should_latch(true, authenticated, live_secure))
      authenticated = true;
  }
  void on_disconnect() {
    connected = false;
    authenticated = false;
    handle = ble::PendingCccd::kNone;
    ++generation;
    subscribed.fill(false);
    early.clear();
    queue.close();
  }
  // enqueue_response(): the gate, then a credit.
  EnqueueResult send(std::span<const std::uint8_t> frame) {
    Gate gate{
        .cached = authenticated, .live_secure = live_secure, .lookups = 0};
    const auto refusal =
        ble::response_refusal(connected, true, subscribed[kResponse],
                              [&] { return gate.send(frame); });
    if (ble::refusal_is_fault(refusal) &&
        std::exchange(refusal_logged, generation) != generation)
      ++fault_logs;
    if (refusal != ble::GateRefusal::None)
      return EnqueueResult::Closed;
    const auto r = queue.reserve(generation, 0);
    if (r.result != EnqueueResult::Ok)
      return r.result;
    if (!queue.publish(r, generation, static_cast<std::uint16_t>(frame.size()),
                       handle))
      return EnqueueResult::Closed;
    TxSlot slot;
    std::uint8_t index = 0;
    assert(queue.front(slot, index));
    assert(queue.on_notify_tx(generation, NotifyStatus::Sent, 0).current);
    return EnqueueResult::Ok;
  }
  // The first LIST: two FILE frames and LIST_END all reach the client.
  bool first_list_served() {
    const std::uint8_t file_a[] = {ble::kFrameFile, 1, 0, 0, 0, 'a'};
    const std::uint8_t file_b[] = {ble::kFrameFile, 2, 0, 0, 0, 'b'};
    const std::uint8_t list_end[] = {ble::kFrameListEnd, 2, 0, 0, 0};
    return send(file_a) == EnqueueResult::Ok &&
           send(file_b) == EnqueueResult::Ok &&
           send(list_end) == EnqueueResult::Ok;
  }
};

void test_restore_before_connect_serves_first_list() {
  // NimBLE holds CONNECT back for the remote version and feature reads. The
  // Mac encrypts with its bond meanwhile, so ENC_CHANGE and the RESTORE
  // subscribe (Response and Status) arrive first, for an unknown handle.
  Transport link;
  link.on_disconnect();
  link.on_enc_change(1);
  link.on_subscribe(1, kResponse, true);
  link.on_subscribe(1, 1, true);
  link.on_connect(1); // logs via=connect, the link is already secure
  assert(link.authenticated);
  assert(link.subscribed[kResponse] && link.subscribed[1]);
  // The held restore opened nothing new, it is the stream CONNECT opened.
  const auto generation = link.generation;
  assert(link.first_list_served() && link.fault_logs == 0);
  assert(link.generation == generation);
  // The Mac never writes the CCCD again on this link, yet a later LIST and an
  // unsubscribe and resubscribe behave like after any CCCD write.
  assert(link.first_list_served());
  link.on_subscribe(1, kResponse, false);
  link.on_subscribe(1, kResponse, true);
  assert(link.generation == generation + 1 && link.first_list_served());
}

void test_secure_at_connect_restore_after_serves_first_list() {
  // The CoreS3 phone order. CONNECT first, then ENC_CHANGE and the restore.
  Transport link;
  link.on_connect(3);
  assert(!link.authenticated && !link.subscribed[kResponse]);
  link.on_enc_change(3);
  link.on_subscribe(3, kResponse, true);
  assert(link.authenticated && link.first_list_served());
  assert(link.fault_logs == 0 && link.early.held == ble::PendingCccd::kNone);
  // And a link already secure at CONNECT whose client writes the CCCD after.
  Transport later;
  later.on_enc_change(4);
  later.on_connect(4);
  assert(later.authenticated && !later.subscribed[kResponse]);
  later.on_subscribe(4, kResponse, true);
  assert(later.first_list_served() && later.fault_logs == 0);
}

void test_unsubscribed_refusal_is_logged() {
  // No restore and no CCCD write. Every frame is refused as unsubscribed, the
  // log says so once per session, and it is never a silent code 7.
  Transport link;
  link.on_enc_change(2);
  link.on_connect(2);
  assert(link.authenticated && !link.first_list_served());
  const auto busy = error_frame(ble::kOpList, ble::kErrBusy);
  assert(link.send(busy) == EnqueueResult::Closed);
  assert(link.fault_logs == 1);
  static_assert(ble::refusal_is_fault(ble::GateRefusal::Unsubscribed));
  static_assert(ble::refusal_is_fault(ble::GateRefusal::Insecure));
  static_assert(!ble::refusal_is_fault(ble::GateRefusal::Stale));
  static_assert(!ble::refusal_is_fault(ble::GateRefusal::Disconnected));
  static_assert(std::string_view(ble::refusal_name(
                    ble::GateRefusal::Unsubscribed)) == "unsubscribed");
  // A new session logs again.
  link.on_disconnect();
  link.on_connect(2);
  assert(link.send(busy) == EnqueueResult::Closed && link.fault_logs == 2);
  // The order of refusals. A dead or stale session is not a fault, and the
  // live lookup runs last.
  int lookups = 0;
  const auto live = [&] { return ++lookups, false; };
  assert(ble::response_refusal(false, true, true, live) ==
         ble::GateRefusal::Disconnected);
  assert(ble::response_refusal(true, false, true, live) ==
         ble::GateRefusal::Stale);
  assert(ble::response_refusal(true, true, false, live) ==
         ble::GateRefusal::Unsubscribed);
  assert(lookups == 0);
  assert(ble::response_refusal(true, true, true, live) ==
         ble::GateRefusal::Insecure);
  assert(lookups == 1);
  // The os-lan gate never asks the live state (#178). An unlatched link is
  // Deferred, which the transport returns as Full, never refused (#51). A
  // latched link and the ERROR 14 refusal go out, and the session refusals
  // still end the reply.
  const auto auth = error_frame(ble::kOpList, ble::kErrAuth);
  assert(ble::response_refusal_now(true, true, true, false, busy) ==
         ble::GateRefusal::Deferred);
  assert(ble::response_refusal_now(true, true, true, true, busy) ==
         ble::GateRefusal::None);
  assert(ble::response_refusal_now(true, true, true, false, auth) ==
         ble::GateRefusal::None);
  assert(ble::response_refusal_now(false, true, true, false, busy) ==
         ble::GateRefusal::Disconnected);
  assert(ble::response_refusal_now(true, false, true, true, busy) ==
         ble::GateRefusal::Stale);
  assert(ble::response_refusal_now(true, true, false, true, busy) ==
         ble::GateRefusal::Unsubscribed);
  static_assert(!ble::refusal_is_fault(ble::GateRefusal::Deferred));
  static_assert(std::string_view(ble::refusal_name(
                    ble::GateRefusal::Deferred)) == "deferred");
}

void test_pending_cccd() {
  using ble::route_subscribe;
  using ble::SubscribeRoute;
  static_assert(route_subscribe(false, false) == SubscribeRoute::Hold);
  static_assert(route_subscribe(true, true) == SubscribeRoute::Apply);
  static_assert(route_subscribe(true, false) == SubscribeRoute::Drop);
  ble::PendingCccd early;
  int calls = 0;
  const auto count = [&](unsigned, bool) { ++calls; };
  // Nothing held, nothing applied.
  assert(early.replay(1, count) == 0 && calls == 0);
  // The last state per characteristic wins.
  early.hold(1, kResponse, true);
  early.hold(1, 2, true);
  early.hold(1, 2, false);
  std::array<int, 8> state{-1, -1, -1, -1, -1, -1, -1, -1};
  assert(early.replay(1, [&](unsigned i, bool on) { state[i] = on; }) == 2);
  assert(state[kResponse] == 1 && state[2] == 0 && state[0] == -1);
  // Replay forgets, so a second CONNECT sees nothing.
  assert(early.replay(1, count) == 0);
  // A held state of another handle is never applied, and is forgotten.
  early.hold(5, kResponse, true);
  assert(early.replay(6, count) == 0 && calls == 0);
  assert(early.replay(5, count) == 0);
  // A new handle replaces an older pending one.
  early.hold(5, kResponse, true);
  early.hold(6, 1, true);
  assert(early.replay(6,
                      [&](unsigned i, bool on) {
                        assert(i == 1 && on);
                        ++calls;
                      }) == 1 &&
         calls == 1);
  // An index past the bit set is ignored.
  early.hold(7, ble::PendingCccd::kMax, true);
  assert(early.replay(7, count) == 0);
  early.hold(7, kResponse, true);
  early.clear();
  assert(early.replay(7, count) == 0 && calls == 1);
}

// link_on_host()'s Dle and PHY steps on a callout, in milliseconds. Central
// events and the callout run in time order on the host task, as in
// ble_sync.cpp. CONNECT itself sends nothing.
struct LinkSequence {
  static constexpr std::int64_t kSettleMs = 2000;   // kLinkSettleMs
  static constexpr std::uint16_t kWantOctets = 251; // data_len_octets
  ble::LinkStep step = ble::LinkStep::Idle;
  std::int64_t due_ms = -1;
  std::int64_t dle_ms = -1;
  std::int64_t requested_ms = -1;
  std::uint16_t central_tx_octets = 0; // the last DATA_LEN_CHG
  std::uint8_t tx = 1, rx = 1;         // 1M until a PHY update

  // The latch, via CONNECT or ENC_CHANGE. Dle runs on the next queue turn.
  void start(std::int64_t now) {
    step = ble::LinkStep::Dle;
    due_ms = now;
  }
  // Runs the callout if it fell due before `now`.
  void advance(std::int64_t now) {
    if (due_ms < 0 || due_ms > now)
      return;
    if (step == ble::LinkStep::Dle) {
      step = ble::LinkStep::Phy;
      if (ble::dle_request_needed(central_tx_octets, kWantOctets))
        dle_ms = due_ms;
      due_ms += kSettleMs;
      if (due_ms > now)
        return;
    }
    if (step != ble::LinkStep::Phy)
      return;
    step = ble::LinkStep::Interval;
    if (ble::phy_request_needed(true, tx, rx))
      requested_ms = due_ms;
  }
  void central(std::int64_t now) {
    advance(now);
    if (ble::central_procedure_rearms(step))
      due_ms = now + kSettleMs;
  }
};

// The CoreS3 Android bench (2026-10-18). The link is secure at CONNECT and
// the phone's MTU exchange and connection update land around 2 s, where the
// fixed wait sent the PHY request into the phone's own update.
void test_phy_request_waits_for_central_procedures() {
  using ble::LinkStep;
  static_assert(ble::central_procedure_rearms(LinkStep::Phy));
  static_assert(!ble::central_procedure_rearms(LinkStep::Idle));
  static_assert(!ble::central_procedure_rearms(LinkStep::Dle));
  static_assert(!ble::central_procedure_rearms(LinkStep::Interval));
  static_assert(!ble::central_procedure_rearms(LinkStep::Done));
  LinkSequence link;
  link.start(0);
  link.central(1900); // MTU 517
  link.central(2200); // the phone's connection update
  link.advance(10000);
  assert(link.step == LinkStep::Interval);
  assert(link.requested_ms == 2200 + LinkSequence::kSettleMs);
  // Once the PHY step ran, a central event changes nothing.
  link.central(4300);
  assert(link.requested_ms == 4200 && link.step == LinkStep::Interval);
  // A quiet central still gets the request after one settle.
  LinkSequence quiet;
  quiet.start(100);
  quiet.advance(10000);
  assert(quiet.requested_ms == 2100);
}

// A central that moved the link to 2M itself sees no local PHY procedure.
void test_phy_request_skipped_on_2m() {
  static_assert(!ble::phy_request_needed(true, ble::kPhy2M, ble::kPhy2M));
  static_assert(ble::phy_request_needed(true, ble::kPhy2M, 1));
  static_assert(ble::phy_request_needed(true, 1, ble::kPhy2M));
  static_assert(ble::phy_request_needed(false, ble::kPhy2M, ble::kPhy2M));
  LinkSequence link;
  link.start(0);
  link.tx = link.rx = ble::kPhy2M; // the phone's PHY update
  link.central(800);
  link.advance(10000);
  assert(link.step == ble::LinkStep::Interval && link.requested_ms == -1);
  // Only one direction on 2M still asks.
  LinkSequence half;
  half.start(0);
  half.tx = ble::kPhy2M;
  half.advance(10000);
  assert(half.requested_ms == 2000);
}

// The CoreS3 core dumps (2026-10-18). Data length at CONNECT timed out while
// the bonded phone started encryption. It now waits for the latch and goes
// out before the PHY request.
void test_dle_waits_for_latch_and_precedes_phy() {
  LinkSequence link;
  link.advance(5000); // connected, not yet secure
  assert(link.step == ble::LinkStep::Idle && link.dle_ms == -1);
  link.start(5100);
  link.advance(5100);
  assert(link.dle_ms == 5100 && link.step == ble::LinkStep::Phy);
  assert(link.requested_ms == -1);
  link.advance(10000);
  assert(link.requested_ms == 5100 + LinkSequence::kSettleMs);
  assert(link.dle_ms < link.requested_ms);
}

// A central that set the data length itself sees no local request.
void test_dle_skipped_when_central_set_it() {
  static_assert(ble::dle_request_needed(0, 251));
  static_assert(ble::dle_request_needed(27, 251));
  static_assert(!ble::dle_request_needed(251, 251));
  static_assert(!ble::dle_request_needed(251, 200));
  LinkSequence link;
  link.central_tx_octets = 251;
  link.start(0);
  link.advance(10000);
  assert(link.dle_ms == -1 && link.requested_ms == 2000);
  // A shorter length from the central still asks.
  LinkSequence short_central;
  short_central.central_tx_octets = 27;
  short_central.start(0);
  short_central.advance(10000);
  assert(short_central.dle_ms == 0);
}

// A connection update refused while the central's procedure runs is retried
// every second. Only the first refusal of a run logs BLE HCI fail.
void test_conn_fail_logged_once_per_run() {
  constexpr int kBusy = 15, kAlready = 2, kTimeout = 19;
  ble::HciFailLog log;
  assert(!log.on_result(0));
  assert(log.on_result(kBusy));
  assert(!log.on_result(kBusy) && !log.on_result(kBusy));
  // A different failure is news, the same one after it again is not.
  assert(log.on_result(kAlready) && !log.on_result(kAlready));
  assert(log.on_result(kBusy));
  // A success ends the run.
  assert(!log.on_result(0) && log.on_result(kBusy));
  // A new connection or a host reset ends it too.
  log.reset();
  assert(log.on_result(kBusy));
  assert(log.on_result(kTimeout) && !log.on_result(kTimeout));
}
} // namespace

int main() {
  test_credit_exhaustion();
  test_refused_and_failed();
  test_stale_generation();
  test_disconnect_while_waiting();
  test_progress_timeout();
  test_cancel();
  test_configure_clamps();
  test_advert_window();
  test_interval_policy();
  test_conn_params();
  test_options();
  test_queue_order_and_wrap();
  test_reopen_same_generation_rejects_late_publish();
  test_close_rejects_publish();
  test_queue_refused_failed_and_foreign();
  test_configure_caps_slots();
  test_response_subscribe();
  test_classify_close();
  test_stall_close();
  test_flag_watch();
  test_stale_flag_secure_link_is_served();
  test_insecure_link_gets_visible_error_14();
  test_no_auth_refusal_answers_busy();
  test_restore_before_connect_serves_first_list();
  test_secure_at_connect_restore_after_serves_first_list();
  test_unsubscribed_refusal_is_logged();
  test_pending_cccd();
  test_phy_request_waits_for_central_procedures();
  test_phy_request_skipped_on_2m();
  test_dle_waits_for_latch_and_precedes_phy();
  test_dle_skipped_when_central_set_it();
  test_conn_fail_logged_once_per_run();
  std::printf("ble policy passed\n");
  return 0;
}
