#pragma once

// What the CrowPanel 4.2 inch glass offers the shared e-paper UI. The
// UC8276C driver here refreshes the whole frame only, full with the clean
// waveform or partial with DU against the last frame, so no fast mode and
// no partial window. A full refresh every 30 partials clears the ghosts,
// the cadence the board used before the UI moved to firmware/common. The
// panel is mounted landscape and the board has no motion sensor, so the
// Rotation setting offers 0, 90, 180 and 270 and starts at 0.
//
// tools/epaper_ui_host_profiles.h holds the same values as kHost400x300,
// and tools/epaper_ui_test.cpp fails when the two drift apart, so the
// golden frames there stay the frames this board sends.

#include "epaper_ui/layout.h"
#include "epaper_ui/profile.h"

namespace opensensor::crowpanel {

inline constexpr ui::DisplayProfile kDisplayProfile{
    .width = 400,
    .height = 300,
    .depth = ui::ColorDepth::Mono1,
    .refresh = {.fast = false,
                .partial = true,
                .partial_window = false,
                .full_ms = 0,
                .fast_ms = 0,
                .partial_ms = 0,
                .partials_per_full = 30},
    .font_scale = ui::FontScale::Compact,
    .layout = &ui::kLayout400x300,
    .rotated_layout = &ui::kLayout300x400,
    .mount = ui::Rotation::R0,
    .orientation = {},
};

} // namespace opensensor::crowpanel
