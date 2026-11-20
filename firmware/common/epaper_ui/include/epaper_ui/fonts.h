#pragma once

// 1 bit bitmap fonts baked from Inter by tools/epaper_gen_fonts.py. The
// glyph tables are static constexpr, so they stay in flash .rodata. Each set
// lives in its own generated file, so an image links only the set its layout
// names.

#include <cstdint>
#include <span>

namespace opensensor::ui {

struct Glyph {
  std::uint32_t codepoint;
  std::uint32_t offset; // Into Font::bits, rows MSB first, padded to a byte.
  std::uint8_t width;
  std::uint8_t height;
  std::int8_t x_offset; // From the pen position.
  std::int8_t y_offset; // Top row relative to the baseline, negative is up.
  std::uint8_t advance;
};

struct Font {
  std::span<const Glyph> glyphs; // Sorted by codepoint.
  const std::uint8_t *bits = nullptr;
  int ascent = 0;
  int descent = 0;
};

// The five roles the pages draw with. A layout names one set.
struct FontSet {
  const Font &small; // Header, axes and fine print.
  const Font &label; // Tile and row labels.
  const Font &title; // Page titles.
  const Font &value; // Tile values.
  const Font &hero;  // Digits, '.', '-' and space only.
};

// The glyph for `cp`, or nullptr when the font lacks it.
[[nodiscard]] const Glyph *find_glyph(const Font &font, std::uint32_t cp);

namespace fonts {
// 13, 14, 18, 28 and 64 px, for a 400 x 300 class panel.
extern const FontSet compact;
// 20, 22, 30, 46 and 112 px, for an 800 x 480 class panel.
extern const FontSet large;
} // namespace fonts

} // namespace opensensor::ui
