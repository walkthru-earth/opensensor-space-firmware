#include "epaper_ui/orientation.h"

#include <cmath>

namespace opensensor::ui {
namespace {

float component(const Gravity &g, Axis a) {
  switch (a) {
  case Axis::PosX:
    return g.x;
  case Axis::NegX:
    return -g.x;
  case Axis::PosY:
    return g.y;
  case Axis::NegY:
    return -g.y;
  case Axis::PosZ:
    return g.z;
  case Axis::NegZ:
    return -g.z;
  }
  return 0.0F;
}

constexpr float kDegPerRad = 57.29578F;

} // namespace

OrientationTracker::OrientationTracker(SensorAxes axes, Rotation initial,
                                       OrientationTuning tuning)
    : axes_(axes), tuning_(tuning), current_(initial) {}

Rotation OrientationTracker::update(std::optional<Gravity> reading,
                                    std::uint32_t now_ms) {
  if (!reading) {
    pending_.reset();
    return current_;
  }
  const Gravity &g = *reading;
  // The reading points up, so the pull is its negative. In panel axes.
  const float pull_right = -component(g, axes_.right);
  const float pull_down = -component(g, axes_.down);
  const float in_plane = pull_right * pull_right + pull_down * pull_down;
  const float total = g.x * g.x + g.y * g.y + g.z * g.z;
  const float share2 = tuning_.flat_share * tuning_.flat_share;
  if (!std::isfinite(in_plane) || !std::isfinite(total) || total <= 0.0F ||
      in_plane < share2 * total) {
    pending_.reset();
    return current_;
  }
  // 0 degrees when the pull is along the panel's down edge (R0), 90 when it
  // is along its left edge (R90), and so on clockwise.
  float angle = std::atan2(-pull_right, pull_down) * kDegPerRad;
  if (angle < 0.0F)
    angle += 360.0F;
  const float centre = 90.0F * static_cast<float>(std::to_underlying(current_));
  float off = std::fabs(angle - centre);
  if (off > 180.0F)
    off = 360.0F - off;
  if (off <= 45.0F + tuning_.hysteresis_deg) {
    pending_.reset();
    return current_;
  }
  const auto quarter = static_cast<std::uint8_t>(
      static_cast<int>(std::lround(angle / 90.0F)) & 3);
  const auto candidate = static_cast<Rotation>(quarter);
  if (pending_ != candidate) {
    pending_ = candidate;
    pending_since_ms_ = now_ms;
    return current_;
  }
  // Unsigned subtraction, so a wrapping clock still measures the hold.
  if (now_ms - pending_since_ms_ >= tuning_.stable_ms) {
    current_ = candidate;
    pending_.reset();
  }
  return current_;
}

} // namespace opensensor::ui
