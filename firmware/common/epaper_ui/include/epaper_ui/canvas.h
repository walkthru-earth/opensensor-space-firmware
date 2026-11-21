#pragma once

// A one bit logical frame sized by a DisplayProfile and a Rotation, in the
// frame convention of epaper_ui/panel.h with rows of (width + 7) / 8 bytes.
// At R0 and R180 that is the panel frame, at R90 and R270 the canvas is the
// portrait frame and the Presenter turns it onto the panel. Pure C++, no
// ESP-IDF, so the host tests and the preview render the same pixels a panel
// shows.
//
// The size is a run time value, not a template argument. One compiled
// renderer then serves every size class, and the host test draws a 400 x 300
// and an 800 x 480 frame from the same binary. A template would compile the
// pages once per size, which costs flash for nothing on a board with one
// panel. The cost is a multiply by a member instead of a constant per pixel,
// which is noise next to a refresh that takes seconds over SPI.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "epaper_ui/fonts.h"
#include "epaper_ui/geometry.h"
#include "epaper_ui/layout.h"
#include "epaper_ui/profile.h"
#include "os_text.h"

namespace opensensor::ui {

enum class [[nodiscard]] CanvasError : std::uint8_t {
  BadProfile, // validate() refused the profile.
  FrameSize,  // The buffer is shorter than the frame for this rotation.
};

[[nodiscard]] const char *to_string(CanvasError e);

// Draws into a frame it does not own. Every primitive clips to the frame, so
// no call can write outside it whatever the coordinates.
class Canvas {
public:
  // The profile must outlive the canvas, boards keep theirs static. The
  // canvas uses the first bytes_for(profile, rotation) bytes of `frame`, a
  // buffer of profile.logical_frame_bytes() fits every rotation.
  [[nodiscard]] static std::expected<Canvas, CanvasError>
  create(std::span<std::uint8_t> frame, const DisplayProfile &profile,
         Rotation rotation = Rotation::R0);
  [[nodiscard]] static std::size_t bytes_for(const DisplayProfile &profile,
                                             Rotation rotation);

  void clear(Ink ink = Ink::White);
  void pixel(int x, int y, Ink ink = Ink::Black);
  [[nodiscard]] Ink get(int x, int y) const;
  void hline(int x, int y, int w, Ink ink = Ink::Black);
  void vline(int x, int y, int h, Ink ink = Ink::Black);
  void rect(Rect r, Ink ink = Ink::Black);
  void fill(Rect r, Ink ink = Ink::Black);
  void invert_rect(Rect r);
  // Flips every pixel of the frame, black becomes white and white black.
  void invert_all();
  // A rectangle with its four corner pixels cut, which reads as rounded at
  // this size.
  void round_rect(Rect r, Ink ink = Ink::Black);
  void fill_round_rect(Rect r, Ink ink = Ink::Black);
  // Bresenham, both ends included.
  void line(int x0, int y0, int x1, int y1, Ink ink = Ink::Black);
  // A horizontal rule with `on` pixels drawn and `off` skipped.
  void dotted_hline(int x, int y, int w, int on = 1, int off = 2,
                    Ink ink = Ink::Black);
  void dotted_vline(int x, int y, int h, int on = 1, int off = 2,
                    Ink ink = Ink::Black);

  // UTF-8 text with its baseline at `y`, laid out by shape(), from `x` as
  // its left edge whatever its direction. Codepoints neither the font nor
  // its fallback holds are skipped. Returns the advance in pixels.
  int text(const Font &font, int x, int y, std::string_view utf8,
           Ink ink = Ink::Black);
  // The same, aligned on `x` as the left edge, centre or right edge.
  int text(const Font &font, int x, int y, std::string_view utf8, Align align,
           Ink ink = Ink::Black);
  // The same on one line when it fits `max_w`, else on two, broken at the
  // last space that fits in reading order, the second line_step() lower.
  // Returns the lines drawn. A line that still does not fit clips at the
  // frame.
  int text_lines(const Font &font, int x, int y, std::string_view utf8,
                 Align align, int max_w, Ink ink = Ink::Black);

  [[nodiscard]] int width() const { return width_; }
  [[nodiscard]] int height() const { return height_; }
  [[nodiscard]] int stride() const { return stride_; }
  [[nodiscard]] const DisplayProfile &profile() const { return *profile_; }
  [[nodiscard]] Rotation rotation() const { return rotation_; }
  [[nodiscard]] const Layout &layout() const { return *layout_; }
  [[nodiscard]] const FontSet &fonts() const { return layout().fonts; }
  [[nodiscard]] std::span<const std::uint8_t> frame() const { return frame_; }

private:
  Canvas(std::span<std::uint8_t> frame, const DisplayProfile &profile,
         Rotation rotation);

  std::span<std::uint8_t> frame_;
  const DisplayProfile *profile_;
  const Layout *layout_;
  Rotation rotation_;
  int width_;
  int height_;
  int stride_;
};

// Decodes the next codepoint at `pos` and moves `pos` past it. Malformed or
// truncated sequences return U+FFFD and move one byte, so a loop always ends.
[[nodiscard]] std::uint32_t next_codepoint(std::string_view utf8,
                                           std::size_t &pos);

// One glyph placed by shape(). `glyph` is null for a codepoint neither the
// font nor its fallback holds, it then takes no room, and `font` is the one
// that holds it, whose bits the blitter reads. `x` is its pen position from
// the left of the run. `end` is the byte offset just past its codepoint in a
// left to right run, and 0 in a right to left one, where the glyphs come in
// visual order.
struct ShapedGlyph {
  const Glyph *glyph = nullptr;
  const Font *font = nullptr;
  int x = 0;
  std::size_t end = 0;
};

// The glyph for `cp` in `font`, else in its fallback. `used` is the font
// that holds it, `font` itself when neither does.
[[nodiscard]] inline const Glyph *
find_glyph_or_fallback(const Font &font, std::uint32_t cp, const Font *&used) {
  used = &font;
  if (const Glyph *g = find_glyph(font, cp); g != nullptr)
    return g;
  if (font.fallback == nullptr)
    return nullptr;
  const Glyph *g = find_glyph(*font.fallback, cp);
  if (g != nullptr)
    used = font.fallback;
  return g;
}

// The one place text is laid out, and Canvas::text, text_width and the cut
// with ".." all go through it. A run with no right to left letter walks
// `utf8` left to right, one glyph per codepoint. A run with one is shaped
// into Arabic contextual forms and reordered to visual order by
// text::to_visual() with a right to left base, on a stack line of
// text::kMaxLine code points. A combining mark sits centred over the glyph
// before it. Calls `visit` with each placed glyph, left to right on the
// glass, until `visit` returns false, and returns the pen advance up to
// that point.
template <typename Visit>
int shape(const Font &font, std::string_view utf8, Visit &&visit) {
  int pen = 0;
  if (text::has_rtl(utf8)) {
    std::array<char32_t, text::kMaxLine> line{};
    const auto vis = text::to_visual(utf8, line, text::Direction::Rtl,
                                     text::OnMalformed::Replace);
    if (vis) {
      int prev_x = 0;
      int prev_adv = 0;
      for (const char32_t cp : *vis) {
        const Font *used = nullptr;
        const Glyph *g = find_glyph_or_fallback(font, cp, used);
        int x = pen;
        if (g != nullptr && g->advance == 0)
          x = prev_x + (prev_adv - g->width) / 2 - g->x_offset;
        if (!visit(ShapedGlyph{.glyph = g, .font = used, .x = x, .end = 0}))
          return pen;
        if (g != nullptr && g->advance != 0) {
          prev_x = pen;
          prev_adv = g->advance;
          pen += g->advance;
        }
      }
      return pen;
    }
    // A line past kMaxLine falls through and draws in logical order.
  }
  std::size_t pos = 0;
  while (pos < utf8.size()) {
    const Font *used = nullptr;
    const Glyph *g =
        find_glyph_or_fallback(font, next_codepoint(utf8, pos), used);
    if (!visit(ShapedGlyph{.glyph = g, .font = used, .x = pen, .end = pos}))
      return pen;
    if (g != nullptr)
      pen += g->advance;
  }
  return pen;
}

// The advance of `utf8` in `font`, the same sum Canvas::text() moves the pen.
[[nodiscard]] int text_width(const Font &font, std::string_view utf8);

// Where a line of `utf8` breaks to fit `max_w` pixels, the byte offset of
// the last space that fits, or utf8.size() when the whole text fits or no
// space does. Pages that are narrow in portrait wrap with it, a text that
// fits draws exactly as Canvas::text() does.
[[nodiscard]] std::size_t line_break(const Font &font, std::string_view utf8,
                                     int max_w);

// The baseline to baseline pitch of `font`.
[[nodiscard]] constexpr int line_step(const Font &font) {
  return font.ascent + font.descent;
}

// The pitch for `utf8`, which takes the taller fallback's room when the
// text holds a right to left letter. The same as line_step(font) otherwise.
[[nodiscard]] int line_step(const Font &font, std::string_view utf8);

} // namespace opensensor::ui
