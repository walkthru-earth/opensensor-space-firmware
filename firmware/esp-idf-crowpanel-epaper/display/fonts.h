#pragma once

// 1 bit bitmap fonts baked from Inter by tools/crowpanel_gen_fonts.py. The
// glyph tables are static constexpr, so they stay in flash .rodata.

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

// The glyph for `cp`, or nullptr when the font lacks it.
[[nodiscard]] const Glyph *find_glyph(const Font &font, std::uint32_t cp);

namespace fonts {
extern const Font small; // 12 px medium, header, axes and fine print.
extern const Font label; // 14 px semibold, tile and row labels.
extern const Font title; // 18 px bold, page titles.
extern const Font value; // 28 px bold, tile values.
extern const Font hero;  // 64 px bold, digits, '.', '-' and space only.
} // namespace fonts

} // namespace opensensor::ui
