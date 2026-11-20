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

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "epaper_ui/fonts.h"
#include "epaper_ui/geometry.h"
#include "epaper_ui/layout.h"
#include "epaper_ui/profile.h"

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

  // UTF-8 text with its baseline at `y`, laid out by shape(). Codepoints the
  // font lacks and malformed bytes are skipped. Returns the advance in
  // pixels.
  int text(const Font &font, int x, int y, std::string_view utf8,
           Ink ink = Ink::Black);
  // The same, aligned on `x` as the left edge, centre or right edge.
  int text(const Font &font, int x, int y, std::string_view utf8, Align align,
           Ink ink = Ink::Black);
  // The same on one line when it fits `max_w`, else on two, broken at the
  // last space that fits, the second line_step() lower. Returns the lines
  // drawn. A line that still does not fit clips at the frame.
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

// One glyph placed by shape(). `glyph` is null for a codepoint the font
// lacks, it then takes no room. `x` is its pen position from the start of
// the run, `end` the byte offset just past its codepoint.
struct ShapedGlyph {
  const Glyph *glyph = nullptr;
  int x = 0;
  std::size_t end = 0;
};

// The one place text is laid out. It walks `utf8` left to right, one glyph
// per codepoint, and calls `visit` with each placed glyph until `visit`
// returns false. Returns the pen advance up to that point. Canvas::text,
// text_width and the cut with ".." all go through it, so right to left runs,
// contextual forms or ligatures change this function and nothing else.
template <typename Visit>
int shape(const Font &font, std::string_view utf8, Visit &&visit) {
  int pen = 0;
  std::size_t pos = 0;
  while (pos < utf8.size()) {
    const Glyph *g = find_glyph(font, next_codepoint(utf8, pos));
    if (!visit(ShapedGlyph{.glyph = g, .x = pen, .end = pos}))
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

} // namespace opensensor::ui
