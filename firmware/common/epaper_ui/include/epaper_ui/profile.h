#pragma once

// What one e-paper panel can do, as the shared UI sees it. The board owns
// its profile, usually a static constexpr next to its driver, and the UI
// derives every size, font and refresh choice from it. No board name and no
// pin appears here.

#include <cstddef>
#include <cstdint>
#include <expected>

#include "epaper_ui/orientation.h"

namespace opensensor::ui {

struct Layout;

// Bits per pixel the panel takes. Only Mono1 renders today. Grey4 is named
// so a profile can say what its glass offers, validate() refuses it until
// the canvas grows a grey path.
enum class ColorDepth : std::uint8_t { Mono1, Grey4 };

// The font size class a layout draws with.
enum class FontScale : std::uint8_t { Compact, Large };

// The refresh modes the controller offers. Full is always there. Fast is a
// quicker full screen waveform, Partial updates without a flash and leaves
// ghosts. A panel with partial_window refreshes a byte aligned window, one
// without takes the whole frame for every partial. Durations are typical
// milliseconds, 0 when not measured on the bench. partials_per_full bounds
// the ghosting, after that many partials the next update is full.
struct RefreshCaps {
  bool fast = false;
  bool partial = false;
  bool partial_window = false;
  std::uint16_t full_ms = 0;
  std::uint16_t fast_ms = 0;
  std::uint16_t partial_ms = 0;
  std::uint16_t partials_per_full = 0;
};

// Frame convention for every profile. One bit per pixel, rows top to bottom,
// width / 8 bytes per row, the most significant bit of a byte is its
// leftmost pixel and a set bit is black. Width is a multiple of 8. This is
// the panel frame. The pages draw a logical frame, the same but rotated, and
// at R90 and R270 its rows are (height + 7) / 8 bytes, see Presenter.
struct DisplayProfile {
  int width = 0;
  int height = 0;
  ColorDepth depth = ColorDepth::Mono1;
  RefreshCaps refresh{};
  FontScale font_scale = FontScale::Compact;
  // The layout table for this size class. Its width, height and font scale
  // match the profile. A board names it here, so its image links only that
  // table and its fonts.
  const Layout *layout = nullptr;
  // The portrait table, height x width with the same font scale, used at
  // R90 and R270.
  const Layout *rotated_layout = nullptr;
  // How the board mounts the panel. The user's Rotation setting turns the
  // picture further, and Auto ignores it because the motion sensor already
  // reads in panel axes.
  Rotation mount = Rotation::R0;
  OrientationSource orientation{};

  [[nodiscard]] constexpr int stride() const { return width / 8; }
  [[nodiscard]] constexpr std::size_t frame_bytes() const {
    return width > 0 && height > 0 ? static_cast<std::size_t>(stride()) *
                                         static_cast<std::size_t>(height)
                                   : 0;
  }
  // A buffer for the logical frame at any rotation, the larger of the
  // landscape and the portrait frame.
  [[nodiscard]] constexpr std::size_t logical_frame_bytes() const {
    if (width <= 0 || height <= 0)
      return 0;
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    const std::size_t portrait = (h + 7) / 8 * w;
    const std::size_t landscape = (w + 7) / 8 * h;
    return portrait > landscape ? portrait : landscape;
  }
  // The layout for a rotation, null when the profile has none.
  [[nodiscard]] constexpr const Layout *layout_for(Rotation r) const {
    return swaps_axes(r) ? rotated_layout : layout;
  }
};

enum class [[nodiscard]] ProfileError : std::uint8_t {
  BadSize, // Width or height not positive, or width not a multiple of 8.
  UnsupportedDepth, // Only Mono1 renders.
  NoLayout,         // The layout or the rotated layout is missing.
  LayoutMismatch,   // A layout is for another size or font scale.
  BadRefreshCaps,   // A partial window without partial refresh, or
                    // partial refresh with no partials_per_full bound.
};

[[nodiscard]] std::expected<void, ProfileError>
validate(const DisplayProfile &p);

[[nodiscard]] const char *to_string(ProfileError e);

} // namespace opensensor::ui
