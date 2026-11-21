#pragma once

// Screen rotation, and turning the screen with the device when the board has
// a motion sensor. Pure C++, no IMU code. A board with an accelerometer
// reads it in its own driver and hands the shared code one Gravity value per
// poll, the shared code decides when the picture turns.
//
// Rotation is how far the picture turns clockwise on the glass. At R0 the
// logical top left is the panel's top left. At R90 and R270 the logical
// width is the panel height, so the pages lay out for a portrait frame.

#include <cstdint>
#include <optional>
#include <utility>

namespace opensensor::ui {

enum class Rotation : std::uint8_t { R0, R90, R180, R270 };

// `a` then `b`, both clockwise.
[[nodiscard]] constexpr Rotation compose(Rotation a, Rotation b) {
  return static_cast<Rotation>((std::to_underlying(a) + std::to_underlying(b)) &
                               3U);
}

// True when the logical width is the panel height.
[[nodiscard]] constexpr bool swaps_axes(Rotation r) {
  return r == Rotation::R90 || r == Rotation::R270;
}

// One axis of the motion sensor, with its sign.
enum class Axis : std::uint8_t { PosX, NegX, PosY, NegY, PosZ, NegZ };

// How the board mounts its motion sensor. `right` is the sensor axis that
// points to the panel's right edge, `down` the one that points to its bottom
// edge, both with the panel at R0. The third axis is not needed, the shared
// code only uses it through the length of the whole vector.
struct SensorAxes {
  Axis right = Axis::PosX;
  Axis down = Axis::PosY;
};

// What a board offers to turn the screen by itself. `present` false, the
// default, means no motion sensor, then the Rotation setting has no Auto.
struct OrientationSource {
  bool present = false;
  SensorAxes axes{};
};

// One accelerometer reading in sensor axes, any unit. At rest it points away
// from the ground, the way an accelerometer reads 1 g upward when it lies
// still. A board whose driver reports the pull instead negates all three.
struct Gravity {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
};

// The stability rule. The defaults are what the shared UI ships.
struct OrientationTuning {
  // A new orientation must hold this long before the screen turns.
  std::uint32_t stable_ms = 2000;
  // The device counts as flat, and keeps its rotation, while the share of
  // gravity in the panel plane is below this. 0.5 is about 30 degrees from
  // lying flat.
  float flat_share = 0.5F;
  // How far past the 45 degree boundary the tilt must go before a new
  // orientation is even a candidate. Without it a device leaning near 45
  // degrees would flip back and forth.
  float hysteresis_deg = 15.0F;
};

// The rotation whose "down" is closest to the pull of gravity, with the rule
// above. A board polls update() from its display task between two panel
// refreshes, so a turn never starts in the middle of one. A changed result
// is the board's cue to redraw, the Presenter makes that redraw full.
class OrientationTracker {
public:
  explicit OrientationTracker(SensorAxes axes, Rotation initial = Rotation::R0,
                              OrientationTuning tuning = {});

  // Feeds one reading, nullopt when the sensor gave none. `now_ms` is a
  // monotonic clock in milliseconds, it may wrap. Returns the rotation to
  // draw. No reading or a flat device keeps the current rotation and drops
  // any candidate.
  Rotation update(std::optional<Gravity> reading, std::uint32_t now_ms);

  [[nodiscard]] Rotation current() const { return current_; }

private:
  SensorAxes axes_;
  OrientationTuning tuning_;
  Rotation current_;
  std::optional<Rotation> pending_;
  std::uint32_t pending_since_ms_ = 0;
};

} // namespace opensensor::ui
