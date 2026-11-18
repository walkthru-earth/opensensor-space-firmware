#pragma once

#include <cstdint>
#include <limits>

// Pure SD space governor. It only classifies free space and schedules
// retries; the storage worker owns every file operation. No state here ever
// deletes, renames or rewrites archive data. A full card raises an alarm and
// logging resumes once the owner frees space.
namespace oslogger::storage {
enum class State : std::uint8_t {
  Ready = 0,
  Degraded = 1,
  Full = 2,
  Retry = 3
};

constexpr const char *state_name(State state) {
  switch (state) {
  case State::Ready:
    return "ready";
  case State::Degraded:
    return "degraded";
  case State::Full:
    return "full";
  case State::Retry:
    return "retry";
  }
  return "retry";
}

// Full and Retry stop new rows; Degraded still writes but warns.
constexpr bool alarm(State state) {
  return state == State::Full || state == State::Retry;
}

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kHardReserve = 32 * kMiB;
constexpr std::uint64_t kHysteresis = 16 * kMiB;
constexpr std::uint64_t kHighCap = 256 * kMiB;
constexpr std::int64_t kRecheckSeconds = 60;
constexpr std::uint32_t kBackoffCapSeconds = 600;

struct Thresholds {
  std::uint64_t reserve = 0;    // below this, stop writing new rows
  std::uint64_t high = 0;       // below this, warn
  std::uint64_t hysteresis = 0; // margin required before stepping back up
};

constexpr std::uint64_t smaller(std::uint64_t a, std::uint64_t b) {
  return a < b ? a : b;
}

// High mark is 5% of the card capped at 256 MiB. Small cards scale the
// reserve and hysteresis down so the bands still fit inside the card.
constexpr Thresholds thresholds(std::uint64_t total_bytes) {
  Thresholds out;
  out.reserve = smaller(kHardReserve, total_bytes / 8);
  out.hysteresis = smaller(kHysteresis, total_bytes / 16);
  out.high = smaller(total_bytes / 20, kHighCap);
  if (out.high < out.reserve + out.hysteresis)
    out.high = out.reserve + out.hysteresis;
  return out;
}

// Space measurements never leave Retry. Only a successful probe does.
constexpr State next_space_state(State current, std::uint64_t free_bytes,
                                 const Thresholds &t) {
  switch (current) {
  case State::Retry:
    return State::Retry;
  case State::Ready:
    if (free_bytes < t.reserve)
      return State::Full;
    return free_bytes < t.high ? State::Degraded : State::Ready;
  case State::Degraded:
    if (free_bytes < t.reserve)
      return State::Full;
    return free_bytes >= t.high + t.hysteresis ? State::Ready : State::Degraded;
  case State::Full:
    if (free_bytes >= t.high + t.hysteresis)
      return State::Ready;
    return free_bytes >= t.reserve + t.hysteresis ? State::Degraded
                                                  : State::Full;
  }
  return State::Retry;
}

// A remount may expose another card, so its active tree is scanned for
// partials before any file can be created there. A Full recheck remount only
// finds files this device closed before it, so it rescans only when the probe
// may write again. A card that stays below the reserve is never rescanned.
constexpr bool remount_scans_partials(State previous, std::uint64_t free_bytes,
                                      const Thresholds &t) {
  return previous != State::Full || free_bytes >= t.reserve;
}

// While the card may take rows, the worker asks it for its status about
// every kPresenceSeconds. A pulled card then leaves Ready before the next
// batch commit, so the screen and STATUS stop saying it logs. Full and Retry
// never ask, since their recheck and probe already touch the card.
constexpr std::int64_t kPresenceSeconds = 5;
constexpr bool presence_due(State state, bool storage_ready, std::int64_t now,
                            std::int64_t last) {
  return storage_ready && (state == State::Ready || state == State::Degraded) &&
         now - last >= kPresenceSeconds;
}

// 10 s, 30 s, 60 s, then doubling to a 600 s cap.
constexpr std::uint32_t backoff_seconds(std::uint32_t attempt) {
  if (attempt == 0)
    return 10;
  if (attempt == 1)
    return 30;
  if (attempt >= 6)
    return kBackoffCapSeconds;
  const std::uint32_t value = 60U << (attempt - 2);
  return value < kBackoffCapSeconds ? value : kBackoffCapSeconds;
}

class Governor {
public:
  // Starts in Retry until a mounted card reports capacity.
  void configure(std::uint64_t total_bytes) {
    limits_ = thresholds(total_bytes);
    if (total_bytes != 0 && state_ == State::Retry && !failure_)
      state_ = State::Ready;
  }

  // Fresh filesystem measurement. Returns true when the state changed.
  bool observe_free(std::uint64_t measured_free, std::int64_t now_seconds) {
    free_ = measured_free;
    checked_ = now_seconds;
    return settle();
  }

  // A write, close, rename or mount failed. Repeated failures back off.
  void write_failed(std::int64_t now_seconds) {
    if (state_ == State::Retry && failure_) {
      back_off(now_seconds);
      return;
    }
    state_ = State::Retry;
    failure_ = true;
    attempts_ = 0;
    next_probe_ = now_seconds + backoff_seconds(0);
  }

  // A failed probe from Full or Degraded is a card fault, so it moves to
  // Retry and its backoff instead of being retried on every recheck.
  void probe_failed(std::int64_t now_seconds) {
    if (state_ != State::Retry) {
      write_failed(now_seconds);
      return;
    }
    back_off(now_seconds);
  }

  void probe_succeeded(std::uint64_t measured_free, std::int64_t now_seconds) {
    failure_ = false;
    attempts_ = 0;
    free_ = measured_free;
    checked_ = now_seconds;
    // A Full or Degraded card keeps its hysteresis across a remount.
    state_ = next_space_state(state_ == State::Retry ? State::Ready : state_,
                              free_, limits_);
  }

  // Explicit FLUSH or codec/interval changes retry immediately.
  void request_probe() {
    next_probe_ = std::numeric_limits<std::int64_t>::min();
  }

  bool probe_due(std::int64_t now_seconds) const {
    return state_ == State::Retry && now_seconds >= next_probe_;
  }
  bool recheck_due(std::int64_t now_seconds) const {
    return (state_ == State::Degraded || state_ == State::Full) &&
           now_seconds - checked_ >= kRecheckSeconds;
  }
  bool writable() const {
    return state_ == State::Ready || state_ == State::Degraded;
  }
  State state() const { return state_; }
  std::uint64_t free_bytes() const { return free_; }
  std::uint32_t retry_attempts() const { return attempts_; }
  const Thresholds &limits() const { return limits_; }

private:
  // One more failed attempt while in Retry. Both failure paths share it, so
  // neither calls back into the other.
  void back_off(std::int64_t now_seconds) {
    if (attempts_ < std::numeric_limits<std::uint32_t>::max())
      ++attempts_;
    next_probe_ = now_seconds + backoff_seconds(attempts_);
  }

  bool settle() {
    const State next = next_space_state(state_, free_, limits_);
    const bool changed = next != state_;
    state_ = next;
    return changed;
  }

  Thresholds limits_{};
  State state_ = State::Retry;
  bool failure_ = false;
  std::uint32_t attempts_ = 0;
  std::uint64_t free_ = 0;
  std::int64_t checked_ = 0;
  std::int64_t next_probe_ = 0;
};
} // namespace oslogger::storage
