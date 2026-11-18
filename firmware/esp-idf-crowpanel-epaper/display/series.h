#pragma once

// A read only view of a ring buffer in time order, oldest first. Charts read
// history through it, so rendering a 24 h page copies nothing. NaN is a
// missing sample and draws as a gap. A restart point is a NaN slot that
// stands for a reboot of unknown length, the chart draws it as a dashed line.

#include <cstddef>
#include <cstdint>
#include <span>

namespace opensensor::ui {

struct Series {
  std::span<const float> ring;
  std::size_t start = 0; // Ring index of the oldest point.
  // One bit per ring slot, the least significant bit of byte 0 is slot 0.
  // Empty means no restart points.
  std::span<const std::uint8_t> restarts;

  [[nodiscard]] std::size_t size() const { return ring.size(); }
  [[nodiscard]] float operator[](std::size_t i) const {
    return ring[(start + i) % ring.size()];
  }
  // True when point i, oldest first, is a restart point.
  [[nodiscard]] bool restart(std::size_t i) const {
    if (restarts.empty())
      return false;
    const std::size_t pos = (start + i) % ring.size();
    return pos / 8 < restarts.size() &&
           ((restarts[pos / 8] >> (pos % 8)) & 1U) != 0;
  }
};

} // namespace opensensor::ui
