#pragma once

// What the Waveshare ePaper-3.97 glass offers the shared e-paper UI. The
// SSD1677 driver here has all three modes. Full is the clean waveform,
// measured at 2531 ms on the owner unit (bench record 2026-11-20). Fast is
// the demo's quicker full screen waveform and Partial refreshes a byte
// aligned window against the last frame. Neither is timed on the bench yet,
// so their durations stay 0. A full refresh after every 20 partials clears
// the ghosts. Partial is not checked on the glass yet, if it misbehaves set
// .partial and .partial_window to false here and the policy uses Fast.
//
// The QMI8658A accelerometer lets the Rotation setting offer Auto. The axes
// come from board::kImuToPanel, so one table in board_io.h says how the IMU
// sits against the panel and the shared tracker reads the same map as the
// board's quarter_turn().
//
// tools/epaper_ui_test.cpp holds this profile against the host 800 x 480
// one, so the previews drawn there are the frames this board sends.

#include "board_io.h"
#include "epaper_ui/layout.h"
#include "epaper_ui/orientation.h"
#include "epaper_ui/profile.h"

namespace opensensor::epaper397 {

// One IMU axis of board::kImuToPanel as a shared ui::Axis. The IMU reads +1 g
// on the axis that points up at rest, the shared Gravity convention, so the
// raw counts go to the tracker unchanged.
[[nodiscard]] consteval ui::Axis axis_of(board::ImuAxis map) {
  const bool negative = map.sign < 0;
  switch (map.axis) {
  case 0:
    return negative ? ui::Axis::NegX : ui::Axis::PosX;
  case 1:
    return negative ? ui::Axis::NegY : ui::Axis::PosY;
  default:
    return negative ? ui::Axis::NegZ : ui::Axis::PosZ;
  }
}
static_assert(axis_of({.axis = 0, .sign = 1}) == ui::Axis::PosX);
static_assert(axis_of({.axis = 1, .sign = -1}) == ui::Axis::NegY);
static_assert(axis_of({.axis = 2, .sign = 1}) == ui::Axis::PosZ);

inline constexpr ui::SensorAxes kSensorAxes{
    .right = axis_of(board::kImuToPanel.x),
    .down = axis_of(board::kImuToPanel.y),
};
// IMU x against panel x since 2026-11-21, IMU y along panel y.
static_assert(kSensorAxes.right == ui::Axis::NegX);
static_assert(kSensorAxes.down == ui::Axis::PosY);

inline constexpr ui::DisplayProfile kDisplayProfile{
    .width = 800,
    .height = 480,
    .depth = ui::ColorDepth::Mono1,
    .refresh = {.fast = true,
                .partial = true,
                .partial_window = true,
                .full_ms = 2531,
                .fast_ms = 0,
                .partial_ms = 0,
                .partials_per_full = 20},
    .font_scale = ui::FontScale::Large,
    .layout = &ui::kLayout800x480,
    .rotated_layout = &ui::kLayout480x800,
    .mount = ui::Rotation::R0,
    .orientation = {.present = true, .axes = kSensorAxes},
};

} // namespace opensensor::epaper397
