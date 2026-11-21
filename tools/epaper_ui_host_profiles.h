#pragma once

// The display profiles the host test and the preview draw with, one per
// size class. kHost400x300 has the CrowPanel's caps, mount and source, so
// its R0 frames are the frames that board sends. kHost800x480 stands for a
// larger panel with every refresh mode and a motion sensor, so the test
// covers the paths the CrowPanel lacks. Neither names a board.

#include "epaper_ui/layout.h"
#include "epaper_ui/profile.h"

namespace opensensor::ui::host {

inline constexpr DisplayProfile kHost400x300{
    .width = 400,
    .height = 300,
    .depth = ColorDepth::Mono1,
    .refresh = {.fast = false,
                .partial = true,
                .partial_window = false,
                .full_ms = 0,
                .fast_ms = 0,
                .partial_ms = 0,
                .partials_per_full = 30},
    .font_scale = FontScale::Compact,
    .layout = &kLayout400x300,
    .rotated_layout = &kLayout300x400,
    .mount = Rotation::R0,
    .orientation = {},
};

inline constexpr DisplayProfile kHost800x480{
    .width = 800,
    .height = 480,
    .depth = ColorDepth::Mono1,
    .refresh = {.fast = true,
                .partial = true,
                .partial_window = true,
                .full_ms = 0,
                .fast_ms = 0,
                .partial_ms = 0,
                .partials_per_full = 20},
    .font_scale = FontScale::Large,
    .layout = &kLayout800x480,
    .rotated_layout = &kLayout480x800,
    .mount = Rotation::R0,
    .orientation = {.present = true,
                    .axes = {.right = Axis::PosX, .down = Axis::NegY}},
};

} // namespace opensensor::ui::host
