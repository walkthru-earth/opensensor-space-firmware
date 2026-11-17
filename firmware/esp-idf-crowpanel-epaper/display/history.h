#pragma once

// Fixed rings of recent readings for the dashboard sparklines and the 24 h
// charts. One slot per 10 s for the last hour and one per minute for the last
// day, for each metric. Slots are keyed by monotonic seconds, never UTC, so
// the charts work before the clock is set. A slot with no sample is NaN and
// draws as a gap, nothing is interpolated.
//
// The object is about 50 KB. The board allocates it in PSRAM, it does no
// allocation of its own and has no locks, so one task owns it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "series.h"
#include "ui_model.h"

namespace opensensor::ui {

enum class Window : std::uint8_t { Hour, Day };

class History {
public:
  static constexpr std::size_t kHourSlots = 360; // 10 s each.
  static constexpr std::size_t kDaySlots = 1440; // 60 s each.
  static constexpr std::uint32_t kHourStep = 10;
  static constexpr std::uint32_t kDayStep = 60;

  History() { reset(); }

  void reset();

  // Records one sample taken at `mono_s`. NaN marks a missing value. Slots
  // skipped since the previous push become NaN. A sample in the same slot as
  // the previous one replaces it in the hour ring and joins its mean in the
  // day ring. A time before the previous push is ignored.
  void push(std::uint32_t mono_s, std::span<const float, kMetricCount> values);

  // Moves "now" to `mono_s` without a sample, so a stopped sensor shows as a
  // growing gap instead of a frozen chart.
  void advance(std::uint32_t mono_s);

  [[nodiscard]] Series series(Metric m, Window w) const;
  [[nodiscard]] bool empty() const { return !started_; }

private:
  void advance_hour(std::uint32_t slot);
  void advance_day(std::uint32_t slot);

  std::array<std::array<float, kHourSlots>, kMetricCount> hour_{};
  std::array<std::array<float, kDaySlots>, kMetricCount> day_{};
  std::array<float, kMetricCount> day_sum_{};
  std::array<std::uint16_t, kMetricCount> day_count_{};
  std::uint32_t hour_slot_ = 0; // Absolute slot of the newest hour entry.
  std::uint32_t day_slot_ = 0;  // Absolute minute of the newest day entry.
  bool started_ = false;
};

} // namespace opensensor::ui
