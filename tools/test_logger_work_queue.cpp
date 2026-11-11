// Deterministic interleavings for the actual worker queue/wakeup helper.
#include "os_logger_watchdog.h"
#include "os_logger_work_queue.h"
#include <cassert>

int main() {
  FakeQueue samples, commands;
  FakeSemaphore wakeup;
  const int item = 1;

  // A command arriving between idle queue inspection and the wait wakes it.
  before_idle_wait = [&] {
    assert(oslogger::work::enqueue(&commands, &item, &wakeup));
  };
  oslogger::work::wait_if_idle(&samples, &wakeup);
  assert(commands.depth == 1 && wakeup.blocked == 0 && !wakeup.pending);
  commands.depth = 0;

  // A producer can enqueue before the task exists; its signal is retained.
  assert(oslogger::work::enqueue(&commands, &item, &wakeup));
  oslogger::work::wait_if_idle(&samples, &wakeup);
  assert(commands.depth == 1 && wakeup.blocked == 0);
  commands.depth = 0;

  // Binary signals coalesce, while every successfully copied item survives.
  for (unsigned i = 0; i < commands.capacity; ++i)
    assert(oslogger::work::enqueue(&commands, &item, &wakeup));
  assert(!oslogger::work::enqueue(&commands, &item, &wakeup));
  assert(commands.depth == commands.capacity && wakeup.pending);
  oslogger::work::wait_if_idle(&samples, &wakeup);
  assert(commands.depth == commands.capacity && wakeup.blocked == 0);
  commands.depth = 0;

  // Samples queued behind a processed command keep their priority next pass.
  assert(oslogger::work::enqueue(&samples, &item, &wakeup));
  const auto waits = wakeup.waits;
  oslogger::work::wait_if_idle(&samples, &wakeup);
  assert(wakeup.waits == waits && samples.depth == 1);
  samples.depth = 0;
  oslogger::work::wait_if_idle(&samples,
                               &wakeup); // consume the retained signal
  assert(wakeup.blocked == 0);
  oslogger::work::wait_if_idle(&samples, &wakeup); // truly idle, bounded wait
  assert(wakeup.blocked == 1);

  // A rejected enqueue must not create a wakeup or a phantom item.
  commands.capacity = 0;
  assert(!oslogger::work::enqueue(&commands, &item, &wakeup));
  assert(!wakeup.pending && commands.depth == 0);

  // Worker stall liveness: passive warnings after 5 s with 30 s cooldown.
  using oslogger::work::check_stall;
  using oslogger::work::StallAction;
  std::int64_t last_warn = 0;
  assert(check_stall(10000000LL, 0, last_warn) == StallAction::None);
  assert(check_stall(5000000LL, 1000000LL, last_warn) == StallAction::None);
  assert(check_stall(6000001LL, 1000000LL, last_warn) == StallAction::Warn);
  assert(last_warn == 6000001LL);
  assert(check_stall(20000000LL, 1000000LL, last_warn) == StallAction::None);
  assert(check_stall(36000002LL, 1000000LL, last_warn) == StallAction::Warn);
  assert(last_warn == 36000002LL);
  // begin() seeds the heartbeat before the worker starts. A worker that never
  // stamps warns 5 s after that seed, instead of never, as an unseeded zero
  // heartbeat would.
  {
    constexpr std::int64_t seeded = 300000LL;
    std::int64_t first_warn = 0;
    assert(check_stall(seeded + 5000000LL, seeded, first_warn) ==
           StallAction::None);
    assert(check_stall(seeded + 5000001LL, seeded, first_warn) ==
           StallAction::Warn);
    assert(check_stall(seeded + 5000001LL, 0, first_warn) == StallAction::None);
  }
  // Supervisor expiry: none before the first heartbeat or with timeout 0,
  // strictly after the (>= 5 s) timeout, independent of the warn cooldown.
  using oslogger::work::stall_expired;
  assert(!stall_expired(100000000LL, 0, 60));
  assert(!stall_expired(100000000LL, 1000000LL, 0));
  assert(!stall_expired(61000000LL, 1000000LL, 60));
  assert(stall_expired(61000001LL, 1000000LL, 60));
  assert(!stall_expired(6000000LL, 1000000LL, 1));
  assert(stall_expired(6000001LL, 1000000LL, 1));
  // The poll warning path only warns, however long the stall. The reset
  // decision is the supervisor's.
  std::int64_t warn_only = 0;
  assert(check_stall(4000000000LL, 1000000LL, warn_only) == StallAction::Warn);

  // Boot-loop guard kept in RTC no-init memory.
  namespace wd = oslogger::watchdog;
  wd::Guard guard{0xdeadbeef, 7, 9, 1}; // power-on garbage
  assert(!wd::valid(guard));
  assert(wd::on_boot(guard, true) == wd::Mode::Armed);
  assert(wd::valid(guard) && guard.consecutive == 0 && guard.pending == 0);
  // A task-watchdog reset not armed by the supervisor does not count.
  assert(wd::on_boot(guard, true) == wd::Mode::Armed);
  assert(guard.consecutive == 0);
  // An armed stall followed by another reset reason does not count either.
  wd::mark_pending(guard);
  assert(wd::on_boot(guard, false) == wd::Mode::Armed);
  assert(guard.consecutive == 0 && guard.pending == 0);
  // Three stall resets in a row fall back to warn-only and stay there.
  for (std::uint32_t n = 1; n <= 3; ++n) {
    wd::mark_pending(guard);
    assert(guard.pending == 1);
    const auto mode = wd::on_boot(guard, true);
    assert(guard.consecutive == n && guard.pending == 0);
    assert(mode == (n < 3 ? wd::Mode::Armed : wd::Mode::WarnOnly));
  }
  wd::mark_pending(guard);
  assert(wd::on_boot(guard, true) == wd::Mode::WarnOnly);
  assert(guard.consecutive == wd::kMaxConsecutive);
  // A plain reboot keeps the count, it is not a healthy period.
  assert(wd::on_boot(guard, false) == wd::Mode::WarnOnly);
  // A corrupted count is rejected and starts over.
  wd::Guard flipped = guard;
  flipped.consecutive = 1;
  assert(!wd::valid(flipped));
  // Healthy period clears the count only after healthy_s of continuous
  // liveness, measured from the last expired heartbeat.
  assert(!wd::clear_if_healthy(guard, 3599999999LL, 0, 3600));
  assert(wd::mode(guard) == wd::Mode::WarnOnly);
  assert(!wd::clear_if_healthy(guard, 3700000000LL, 200000000LL, 3600));
  assert(wd::clear_if_healthy(guard, 3600000000LL, 0, 3600));
  assert(guard.consecutive == 0 && wd::mode(guard) == wd::Mode::Armed);
  assert(!wd::clear_if_healthy(guard, 7200000000LL, 0, 3600)); // already 0
  // Two stalls, a healthy hour, then two more: never reaches warn-only.
  for (int round = 0; round < 2; ++round) {
    for (int n = 0; n < 2; ++n) {
      wd::mark_pending(guard);
      assert(wd::on_boot(guard, true) == wd::Mode::Armed);
    }
    assert(wd::clear_if_healthy(guard, 3600000000LL, 0, 3600));
  }
}
