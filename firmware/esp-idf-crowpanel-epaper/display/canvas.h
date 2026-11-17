#pragma once

// A 400 x 300 one bit frame for the CrowPanel 4.2 inch e-paper. Pure C++, no
// ESP-IDF, so the host tests and the preview render the same pixels the
// panel shows. The layout matches the UC8276C RAM, 50 bytes per row, the most
// significant bit is the left pixel and a set bit is black.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "fonts.h"

namespace opensensor::ui {

inline constexpr int kWidth = 400;
inline constexpr int kHeight = 300;
inline constexpr int kStride = kWidth / 8;
inline constexpr std::size_t kFrameBytes = std::size_t{kStride} * kHeight;

using Frame = std::span<std::uint8_t, kFrameBytes>;
using ConstFrame = std::span<const std::uint8_t, kFrameBytes>;

enum class Ink : std::uint8_t { White, Black };

[[nodiscard]] constexpr Ink invert(Ink ink) {
  return ink == Ink::Black ? Ink::White : Ink::Black;
}

struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  [[nodiscard]] constexpr int right() const { return x + w; }
  [[nodiscard]] constexpr int bottom() const { return y + h; }
};

enum class Align : std::uint8_t { Left, Center, Right };

// Draws into a frame it does not own. Every primitive clips to the frame, so
// no call can write outside it whatever the coordinates.
class Canvas {
public:
  explicit Canvas(Frame frame) : frame_(frame) {}

  void clear(Ink ink = Ink::White);
  void pixel(int x, int y, Ink ink = Ink::Black);
  [[nodiscard]] Ink get(int x, int y) const;
  void hline(int x, int y, int w, Ink ink = Ink::Black);
  void vline(int x, int y, int h, Ink ink = Ink::Black);
  void rect(Rect r, Ink ink = Ink::Black);
  void fill(Rect r, Ink ink = Ink::Black);
  void invert_rect(Rect r);
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

  // UTF-8 text with its baseline at `y`. Codepoints the font lacks and
  // malformed bytes are skipped. Returns the advance in pixels.
  int text(const Font &font, int x, int y, std::string_view utf8,
           Ink ink = Ink::Black);
  // The same, aligned on `x` as the left edge, centre or right edge.
  int text(const Font &font, int x, int y, std::string_view utf8, Align align,
           Ink ink = Ink::Black);

  [[nodiscard]] ConstFrame frame() const { return frame_; }

private:
  Frame frame_;
};

// The advance of `utf8` in `font`, the same sum Canvas::text() moves the pen.
[[nodiscard]] int text_width(const Font &font, std::string_view utf8);

// Decodes the next codepoint at `pos` and moves `pos` past it. Malformed or
// truncated sequences return U+FFFD and move one byte, so a loop always ends.
[[nodiscard]] std::uint32_t next_codepoint(std::string_view utf8,
                                           std::size_t &pos);

} // namespace opensensor::ui
