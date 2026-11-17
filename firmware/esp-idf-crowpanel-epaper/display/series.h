#pragma once

// A read only view of a ring buffer in time order, oldest first. Charts read
// history through it, so rendering a 24 h page copies nothing. NaN is a
// missing sample and draws as a gap.

#include <cstddef>
#include <span>

namespace opensensor::ui {

struct Series {
  std::span<const float> ring;
  std::size_t start = 0; // Ring index of the oldest point.

  [[nodiscard]] std::size_t size() const { return ring.size(); }
  [[nodiscard]] float operator[](std::size_t i) const {
    return ring[(start + i) % ring.size()];
  }
};

} // namespace opensensor::ui
