#include "epaper_ui/history.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace opensensor::ui {
namespace {
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

void set_bit(std::span<std::uint8_t> bits, std::size_t pos, bool on) {
  const auto mask = static_cast<std::uint8_t>(1U << (pos % 8));
  if (on)
    bits[pos / 8] |= mask;
  else
    bits[pos / 8] &= static_cast<std::uint8_t>(~mask);
}
} // namespace

void History::reset() {
  for (auto &ring : recent_)
    ring.fill(kNaN);
  for (auto &ring : day_)
    ring.fill(kNaN);
  recent_restarts_.fill(0);
  day_restarts_.fill(0);
  day_sum_.fill(0.0F);
  day_count_.fill(0);
  anchor_ = {};
  recent_slot_ = 0;
  day_slot_ = 0;
  last_push_s_.reset();
  started_ = false;
  sealed_ = false;
  boot_opened_ = true;
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
    set_bit(recent_restarts_, pos, false);
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
    set_bit(day_restarts_, pos, false);
  }
  day_slot_ = slot;
  day_sum_.fill(0.0F);
  day_count_.fill(0);
}

void History::move_to(std::uint32_t recent, std::uint32_t day) {
  if (!started_) {
    recent_slot_ = recent;
    day_slot_ = day;
    started_ = true;
    return;
  }
  advance_recent(recent);
  advance_day(day);
}

void History::restart(std::uint32_t first_s) {
  const std::uint32_t recent = recent_slot_ + 1;
  const std::uint32_t day = day_slot_ + 1;
  advance_recent(recent);
  advance_day(day);
  set_bit(recent_restarts_, recent % kRecentSlots, true);
  set_bit(day_restarts_, day % kDaySlots, true);
  anchor_ = {.first_s = first_s, .recent = recent + 1, .day = day + 1};
}

void History::seed_boot() {
  if (!sealed_)
    boot_opened_ = true;
}

void History::seed(std::uint32_t mono_s,
                   std::span<const float, kMetricCount> values) {
  if (sealed_)
    return;
  if (boot_opened_) {
    // The boot's first row anchors its segment, so a boot with no rows
    // leaves no trace.
    boot_opened_ = false;
    last_push_s_.reset();
    if (started_)
      restart(mono_s);
    else
      anchor_ = {.first_s = mono_s, .recent = 0, .day = 0};
  }
  record(mono_s, values);
}

void History::finish_seed() {
  if (sealed_)
    return;
  sealed_ = true;
  if (!started_)
    return; // Nothing seeded, the plain slots stay.
  last_push_s_.reset();
  restart(0);
}

void History::advance(std::uint32_t mono_s) {
  if (!sealed_)
    finish_seed();
  move_to(anchor_.recent +
              (mono_s / kRecentStep - anchor_.first_s / kRecentStep),
          anchor_.day + (mono_s / kDayStep - anchor_.first_s / kDayStep));
}

void History::push(std::uint32_t mono_s,
                   std::span<const float, kMetricCount> values) {
  if (!sealed_)
    finish_seed();
  record(mono_s, values);
}

void History::record(std::uint32_t mono_s,
                     std::span<const float, kMetricCount> values) {
  // Slots alone miss an older sample in the same slot, 10 s after 19 s,
  // which would replace the newer value and join the minute mean.
  if (last_push_s_ && mono_s < *last_push_s_)
    return;
  if (mono_s < anchor_.first_s)
    return;
  const std::uint32_t recent =
      anchor_.recent + (mono_s / kRecentStep - anchor_.first_s / kRecentStep);
  const std::uint32_t day =
      anchor_.day + (mono_s / kDayStep - anchor_.first_s / kDayStep);
  if (started_ && (recent < recent_slot_ || day < day_slot_))
    return;
  last_push_s_ = mono_s;
  move_to(recent, day);
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
            .start = (recent_slot_ + 1) % kRecentSlots,
            .restarts = recent_restarts_};
  return {.ring = day_[index(m)],
          .start = (day_slot_ + 1) % kDaySlots,
          .restarts = day_restarts_};
}

} // namespace opensensor::ui
