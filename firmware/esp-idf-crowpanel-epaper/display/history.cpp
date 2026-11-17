#include "history.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace opensensor::ui {
namespace {
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
} // namespace

void History::reset() {
  for (auto &ring : recent_)
    ring.fill(kNaN);
  for (auto &ring : day_)
    ring.fill(kNaN);
  day_sum_.fill(0.0F);
  day_count_.fill(0);
  recent_slot_ = 0;
  day_slot_ = 0;
  started_ = false;
}

void History::advance_recent(std::uint32_t slot) {
  if (slot <= recent_slot_)
    return;
  const std::uint32_t gap =
      std::min<std::uint32_t>(slot - recent_slot_, kRecentSlots);
  for (std::uint32_t i = 0; i < gap; ++i) {
    const std::size_t pos = (slot - i) % kRecentSlots;
    for (auto &ring : recent_)
      ring[pos] = kNaN;
  }
  recent_slot_ = slot;
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
  const std::uint32_t recent = mono_s / kRecentStep;
  const std::uint32_t day = mono_s / kDayStep;
  if (!started_) {
    recent_slot_ = recent;
    day_slot_ = day;
    started_ = true;
    return;
  }
  advance_recent(recent);
  advance_day(day);
}

void History::push(std::uint32_t mono_s,
                   std::span<const float, kMetricCount> values) {
  const std::uint32_t recent = mono_s / kRecentStep;
  const std::uint32_t day = mono_s / kDayStep;
  if (started_ && (recent < recent_slot_ || day < day_slot_))
    return;
  advance(mono_s);
  const std::size_t recent_pos = recent % kRecentSlots;
  const std::size_t day_pos = day % kDaySlots;
  for (std::size_t m = 0; m < kMetricCount; ++m) {
    const float v = values[m];
    recent_[m][recent_pos] = std::isfinite(v) ? v : kNaN;
    if (!std::isfinite(v))
      continue;
    day_sum_[m] += v;
    ++day_count_[m];
    day_[m][day_pos] = day_sum_[m] / static_cast<float>(day_count_[m]);
  }
}

Series History::series(Metric m, Window w) const {
  if (w == Window::Recent)
    return {.ring = recent_[index(m)],
            .start = (recent_slot_ + 1) % kRecentSlots};
  return {.ring = day_[index(m)], .start = (day_slot_ + 1) % kDaySlots};
}

} // namespace opensensor::ui
