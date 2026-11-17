#include "history.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace opensensor::ui {
namespace {
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
} // namespace

void History::reset() {
  for (auto &ring : hour_)
    ring.fill(kNaN);
  for (auto &ring : day_)
    ring.fill(kNaN);
  day_sum_.fill(0.0F);
  day_count_.fill(0);
  hour_slot_ = 0;
  day_slot_ = 0;
  started_ = false;
}

void History::advance_hour(std::uint32_t slot) {
  if (slot <= hour_slot_)
    return;
  const std::uint32_t gap =
      std::min<std::uint32_t>(slot - hour_slot_, kHourSlots);
  for (std::uint32_t i = 0; i < gap; ++i) {
    const std::size_t pos = (slot - i) % kHourSlots;
    for (auto &ring : hour_)
      ring[pos] = kNaN;
  }
  hour_slot_ = slot;
}

void History::advance_day(std::uint32_t slot) {
  if (slot <= day_slot_)
    return;
  const std::uint32_t gap =
      std::min<std::uint32_t>(slot - day_slot_, kDaySlots);
  for (std::uint32_t i = 0; i < gap; ++i) {
    const std::size_t pos = (slot - i) % kDaySlots;
    for (auto &ring : day_)
      ring[pos] = kNaN;
  }
  day_slot_ = slot;
  day_sum_.fill(0.0F);
  day_count_.fill(0);
}

void History::advance(std::uint32_t mono_s) {
  const std::uint32_t hour = mono_s / kHourStep;
  const std::uint32_t day = mono_s / kDayStep;
  if (!started_) {
    hour_slot_ = hour;
    day_slot_ = day;
    started_ = true;
    return;
  }
  advance_hour(hour);
  advance_day(day);
}

void History::push(std::uint32_t mono_s,
                   std::span<const float, kMetricCount> values) {
  const std::uint32_t hour = mono_s / kHourStep;
  const std::uint32_t day = mono_s / kDayStep;
  if (started_ && (hour < hour_slot_ || day < day_slot_))
    return;
  advance(mono_s);
  const std::size_t hour_pos = hour % kHourSlots;
  const std::size_t day_pos = day % kDaySlots;
  for (std::size_t m = 0; m < kMetricCount; ++m) {
    const float v = values[m];
    hour_[m][hour_pos] = std::isfinite(v) ? v : kNaN;
    if (!std::isfinite(v))
      continue;
    day_sum_[m] += v;
    ++day_count_[m];
    day_[m][day_pos] = day_sum_[m] / static_cast<float>(day_count_[m]);
  }
}

Series History::series(Metric m, Window w) const {
  if (w == Window::Hour)
    return {.ring = hour_[index(m)], .start = (hour_slot_ + 1) % kHourSlots};
  return {.ring = day_[index(m)], .start = (day_slot_ + 1) % kDaySlots};
}

} // namespace opensensor::ui
