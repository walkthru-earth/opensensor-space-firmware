#pragma once

// From a drawn page to the glass. The pages draw a logical frame at the
// screen's rotation, the Presenter turns it into the panel frame, compares
// it with the frame on the glass, picks the refresh mode and window from the
// profile's RefreshCaps and calls the board's Panel.
//
// Rotation is a pass over the finished frame, not a mapping inside every
// Canvas primitive. The primitives and the glyph blits stay row and byte
// based, R0 is a plain copy so the unrotated frame is the drawn one bit for
// bit, and the pass costs one walk over the pixels, milliseconds against a
// refresh of seconds. It needs a third frame in PSRAM, 15200 B at 400 x 300
// and 48000 B at 800 x 480.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "epaper_ui/canvas.h"
#include "epaper_ui/orientation.h"
#include "epaper_ui/panel.h"
#include "epaper_ui/profile.h"

namespace opensensor::ui {

// Which refresh comes next, from the profile's RefreshCaps. It never returns
// a mode the caps lack. The first update and every forced one are Full. A
// page change is Fast where the panel has it, else Full, so the old page
// does not ghost under the new one. Other updates are Partial where the
// panel has it, and after partials_per_full of those the next is Full.
// Without Partial they are Fast, or Full.
class RefreshPolicy {
public:
  explicit RefreshPolicy(RefreshCaps caps) : caps_(caps) {}
  [[nodiscard]] epd::Refresh next(bool page_changed, bool forced);

private:
  RefreshCaps caps_;
  std::uint16_t partials_ = 0;
  bool shown_ = false;
};

// Turns a logical frame drawn at `rotation` into the panel frame of
// `profile`. `logical` holds Canvas::bytes_for(profile, rotation) bytes,
// `panel` holds profile.frame_bytes(). Padding bits past the logical width
// are ignored.
void rotate_frame(std::span<const std::uint8_t> logical,
                  const DisplayProfile &profile, Rotation rotation,
                  std::span<std::uint8_t> panel);

// The smallest window holding every pixel that differs between two panel
// frames, widened to whole bytes, so a driver can send it as it is. An
// empty Rect when they are equal.
[[nodiscard]] Rect changed_window(std::span<const std::uint8_t> a,
                                  std::span<const std::uint8_t> b,
                                  const DisplayProfile &profile);

enum class [[nodiscard]] PresenterError : std::uint8_t {
  BadProfile, // validate() refused the profile.
  BufferSize, // A buffer is shorter than the profile needs.
};

[[nodiscard]] const char *to_string(PresenterError e);

struct Plan {
  epd::Refresh mode = epd::Refresh::Full;
  Rect window; // Panel coordinates, the whole frame unless partial_window.
};

// Holds three buffers it does not own. `logical` is at least
// profile.logical_frame_bytes(), `panel` and `shown` at least
// profile.frame_bytes(). One task uses it, the one that owns the panel.
class Presenter {
public:
  [[nodiscard]] static std::expected<Presenter, PresenterError>
  create(const DisplayProfile &profile, std::span<std::uint8_t> logical,
         std::span<std::uint8_t> panel, std::span<std::uint8_t> shown);

  // A canvas over the logical buffer at `rotation`, for the next frame.
  [[nodiscard]] std::expected<Canvas, CanvasError> canvas(Rotation rotation);

  // Turns the frame drawn on the last canvas() into the panel frame.
  // Returns false when it is exactly the frame on the glass at the same
  // rotation, so the board can skip the refresh.
  [[nodiscard]] bool compose();

  // The refresh for the composed frame. A rotation other than the one on
  // the glass forces Full, a partial over a turned picture would ghost the
  // old one everywhere.
  [[nodiscard]] Plan plan(bool page_changed, bool forced);

  // Shows the composed frame. On success it becomes the frame on the glass.
  // On failure nothing changes here, the driver's next show() is full.
  template <epd::Panel P>
  [[nodiscard]] std::expected<void, epd::Error> present(P &panel,
                                                        const Plan &plan) {
    const auto shown = panel.show(panel_, plan.mode, plan.window);
    if (shown)
      shown_done();
    return shown;
  }

  [[nodiscard]] const DisplayProfile &profile() const { return *profile_; }
  [[nodiscard]] std::span<const std::uint8_t> panel_frame() const {
    return panel_;
  }

private:
  Presenter(const DisplayProfile &profile, std::span<std::uint8_t> logical,
            std::span<std::uint8_t> panel, std::span<std::uint8_t> shown);
  void shown_done();

  const DisplayProfile *profile_;
  std::span<std::uint8_t> logical_;
  std::span<std::uint8_t> panel_;
  std::span<std::uint8_t> shown_;
  RefreshPolicy policy_;
  Rotation drawn_ = Rotation::R0;
  Rotation on_glass_ = Rotation::R0;
  bool has_shown_ = false;
};

} // namespace opensensor::ui
