#include <storage_governor.h>

#include <cassert>
#include <cstdint>
#include <cstring>

namespace {
using namespace oslogger::storage;
constexpr std::uint64_t MiB = 1024ULL * 1024ULL;
constexpr std::uint64_t GiB = 1024ULL * MiB;

void thresholds_scale_with_card() {
  const auto large = thresholds(64 * GiB);
  assert(large.reserve == 32 * MiB && large.hysteresis == 16 * MiB);
  assert(large.high == 256 * MiB); // 5% capped
  const auto medium = thresholds(2 * GiB);
  assert(medium.high == 2 * GiB / 20);
  // High mark never sits inside the reserve band.
  const auto small = thresholds(512 * MiB);
  assert(small.high >= small.reserve + small.hysteresis);
  const auto tiny = thresholds(64 * MiB);
  assert(tiny.reserve < 64 * MiB &&
         tiny.high >= tiny.reserve + tiny.hysteresis);
  assert(tiny.high < 64 * MiB);
  const auto empty = thresholds(0);
  assert(empty.reserve == 0 && empty.high == 0);
}

void space_transitions_have_hysteresis() {
  const auto t = thresholds(64 * GiB); // reserve 32, high 256, hyst 16
  assert(next_space_state(State::Ready, 300 * MiB, t) == State::Ready);
  assert(next_space_state(State::Ready, 255 * MiB, t) == State::Degraded);
  assert(next_space_state(State::Ready, 31 * MiB, t) == State::Full);
  // Degraded leaves only above high + hysteresis.
  assert(next_space_state(State::Degraded, 260 * MiB, t) == State::Degraded);
  assert(next_space_state(State::Degraded, 272 * MiB, t) == State::Ready);
  assert(next_space_state(State::Degraded, 31 * MiB, t) == State::Full);
  // Full leaves only above reserve + hysteresis.
  assert(next_space_state(State::Full, 40 * MiB, t) == State::Full);
  assert(next_space_state(State::Full, 48 * MiB, t) == State::Degraded);
  assert(next_space_state(State::Full, 1 * GiB, t) == State::Ready);
  // Retry is never left by a space measurement alone.
  assert(next_space_state(State::Retry, 1 * GiB, t) == State::Retry);
  // A probe clears Retry, and Full keeps its hysteresis, so a Full card in
  // the band above the reserve still cannot take files (#219).
  assert(probe_state(State::Retry, 1 * GiB, t) == State::Ready);
  assert(probe_state(State::Retry, 33 * MiB, t) == State::Degraded);
  assert(probe_state(State::Retry, 31 * MiB, t) == State::Full);
  assert(probe_state(State::Full, 40 * MiB, t) == State::Full);
  assert(probe_state(State::Full, 48 * MiB, t) == State::Degraded);
}

void backoff_sequence() {
  const std::uint32_t expected[] = {10, 30, 60, 120, 240, 480, 600, 600, 600};
  for (std::uint32_t i = 0; i < sizeof expected / sizeof *expected; ++i)
    assert(backoff_seconds(i) == expected[i]);
  assert(backoff_seconds(1000000) == 600);
}

void governor_lifecycle() {
  Governor g;
  assert(g.state() == State::Retry && !g.writable()); // unconfigured
  g.configure(64 * GiB);
  g.observe_free(10 * GiB, 0);
  assert(g.state() == State::Ready && g.writable());
  assert(g.free_bytes() == 10 * GiB);
  assert(!g.recheck_due(59) && !g.recheck_due(1000)); // Ready never polls

  // Crossing the high mark degrades on the next measurement.
  g.observe_free(257 * MiB, 10);
  assert(g.state() == State::Ready);
  assert(g.observe_free(255 * MiB, 10) && g.state() == State::Degraded);
  assert(g.writable());
  assert(!g.recheck_due(69) && g.recheck_due(70)); // 60 s while not Ready
  g.observe_free(200 * MiB, 70);
  assert(!g.recheck_due(129) && g.recheck_due(130));

  // Crossing the reserve reports a transition to Full once.
  assert(g.observe_free(30 * MiB, 130) && g.state() == State::Full);
  assert(!g.writable());
  assert(!g.observe_free(0, 140)); // no transition
  assert(g.free_bytes() == 0 && g.state() == State::Full);

  // Owner frees space elsewhere, a later measurement resumes logging.
  g.observe_free(40 * MiB, 200);
  assert(g.state() == State::Full);
  g.observe_free(300 * MiB, 260);
  assert(g.state() == State::Ready && g.writable());
}

void retry_backoff_and_recovery() {
  Governor g;
  g.configure(64 * GiB);
  g.observe_free(10 * GiB, 0);
  g.write_failed(100);
  assert(g.state() == State::Retry && !g.writable());
  assert(g.retry_attempts() == 0);
  assert(!g.probe_due(109) && g.probe_due(110));
  // Measurements during Retry never clear it.
  g.observe_free(10 * GiB, 105);
  assert(g.state() == State::Retry);
  g.probe_failed(110);
  assert(g.retry_attempts() == 1 && !g.probe_due(139) && g.probe_due(140));
  g.probe_failed(140);
  assert(!g.probe_due(199) && g.probe_due(200));
  for (int i = 0; i < 20; ++i)
    g.probe_failed(1000);
  assert(!g.probe_due(1599) && g.probe_due(1600));

  // Explicit FLUSH or reconfiguration requests an immediate probe.
  g.request_probe();
  assert(g.probe_due(1001));

  // Success re-derives space state from the fresh measurement.
  g.probe_succeeded(10 * MiB, 1001);
  assert(g.state() == State::Full && g.retry_attempts() == 0);
  g.write_failed(1100);
  g.probe_succeeded(100 * MiB, 1110);
  assert(g.state() == State::Degraded);
  g.write_failed(1200);
  g.probe_succeeded(10 * GiB, 1210);
  assert(g.state() == State::Ready);
  // A fresh failure starts the backoff over at 10 s.
  g.write_failed(1300);
  assert(!g.probe_due(1309) && g.probe_due(1310));
}

void unmounted_boot_starts_in_retry() {
  Governor g;
  g.configure(0);
  g.write_failed(0);
  assert(g.state() == State::Retry && g.probe_due(10));
  g.configure(64 * GiB);
  g.probe_succeeded(10 * GiB, 10);
  assert(g.state() == State::Ready);
}

// A Full card probed by remount keeps its hysteresis, and a failed probe
// from Full backs off in Retry instead of rechecking on every wake.
void full_card_probe() {
  Governor g;
  g.configure(64 * GiB);
  g.observe_free(30 * MiB, 0);
  assert(g.state() == State::Full);
  g.probe_succeeded(40 * MiB, 60);
  assert(g.state() == State::Full && g.retry_attempts() == 0);
  g.probe_succeeded(48 * MiB, 120);
  assert(g.state() == State::Degraded);
  g.observe_free(30 * MiB, 130);
  assert(g.state() == State::Full && g.recheck_due(190));
  g.probe_failed(190);
  assert(g.state() == State::Retry && !g.recheck_due(191));
  assert(!g.probe_due(199) && g.probe_due(200));
  g.probe_failed(200);
  assert(g.retry_attempts() == 1 && !g.probe_due(229) && g.probe_due(230));
}

// Retry and Ready remounts always scan. A Full recheck remount scans only
// when the probe may write, so a card that stays full is not rescanned.
void remount_partial_scan() {
  const auto t = thresholds(64 * GiB);
  assert(remount_scans_partials(State::Retry, 0, t));
  assert(remount_scans_partials(State::Ready, 10 * GiB, t));
  assert(remount_scans_partials(State::Degraded, 100 * MiB, t));
  assert(!remount_scans_partials(State::Full, 0, t));
  assert(!remount_scans_partials(State::Full, 31 * MiB, t));
  assert(remount_scans_partials(State::Full, 32 * MiB, t));
  assert(remount_scans_partials(State::Full, 1 * GiB, t));
}

// A recheck alone makes a Full card writable, which lets the worker commit a
// waiting full batch. A failed commit leaves writable() so it cannot spin.
void recheck_resume_then_failed_commit() {
  Governor g;
  g.configure(64 * GiB);
  g.observe_free(30 * MiB, 0);
  assert(g.state() == State::Full && !g.writable());
  assert(g.recheck_due(60));
  g.observe_free(1 * GiB, 60);
  assert(g.state() == State::Ready && g.writable());
  g.write_failed(61);
  assert(g.state() == State::Retry && !g.writable());
  assert(!g.recheck_due(1000) && !g.probe_due(70) && g.probe_due(71));
}

void presence_check_schedule() {
  static_assert(kPresenceSeconds == 5);
  // Writable states ask once the interval has passed, and not before.
  assert(presence_due(State::Ready, true, 5, 0));
  assert(presence_due(State::Degraded, true, 12, 7));
  assert(!presence_due(State::Ready, true, 4, 0));
  assert(!presence_due(State::Degraded, true, 11, 7));
  // An unmounted card, Full and Retry never ask.
  assert(!presence_due(State::Ready, false, 100, 0));
  assert(!presence_due(State::Full, true, 100, 0));
  assert(!presence_due(State::Retry, true, 100, 0));
  // A failed answer is a write failure. Ready then stops writing and the
  // Retry probe owns recovery.
  Governor g;
  g.configure(64 * GiB);
  g.observe_free(1 * GiB, 0);
  assert(g.state() == State::Ready && g.writable());
  assert(presence_due(g.state(), true, 5, 0));
  g.write_failed(5);
  assert(g.state() == State::Retry && !g.writable() && alarm(g.state()));
  assert(!presence_due(g.state(), true, 10, 5));
  assert(!g.probe_due(14) && g.probe_due(15));
}

void names_are_stable() {
  assert(std::strcmp(state_name(State::Ready), "ready") == 0);
  assert(std::strcmp(state_name(State::Degraded), "degraded") == 0);
  assert(std::strcmp(state_name(State::Full), "full") == 0);
  assert(std::strcmp(state_name(State::Retry), "retry") == 0);
  assert(static_cast<int>(State::Ready) == 0 &&
         static_cast<int>(State::Degraded) == 1 &&
         static_cast<int>(State::Full) == 2 &&
         static_cast<int>(State::Retry) == 3);
  assert(alarm(State::Full) && alarm(State::Retry));
  assert(!alarm(State::Ready) && !alarm(State::Degraded));
}
} // namespace

int main() {
  thresholds_scale_with_card();
  space_transitions_have_hysteresis();
  backoff_sequence();
  governor_lifecycle();
  retry_backoff_and_recovery();
  unmounted_boot_starts_in_retry();
  full_card_probe();
  remount_partial_scan();
  recheck_resume_then_failed_commit();
  presence_check_schedule();
  names_are_stable();
  return 0;
}
