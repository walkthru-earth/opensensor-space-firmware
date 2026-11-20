#pragma once

// 1 bit Arabic bitmap fonts baked from Cairo by tools/gen_arabic_fonts.py.
// The layout matches the board bitmap fonts, rows MSB first and padded to a
// byte, a set bit is black, so a board canvas draws these glyphs with the
// same blitter. The tables are static constexpr and stay in flash .rodata.
//
// Each font holds the Presentation Forms-B glyphs the ar_EG string table
// needs after shaping, the Arabic marks and punctuation, and the Arabic-Indic
// digits. Latin text, digits and units come from the board's own fonts.

#include <cstdint>
#include <span>

namespace opensensor::text {

struct Glyph {
  std::uint32_t codepoint;
  std::uint32_t offset; // Into Font::bits, rows MSB first, padded to a byte.
  std::uint8_t width;
  std::uint8_t height;
  std::int8_t x_offset; // From the pen position.
  std::int8_t y_offset; // Top row relative to the baseline, negative is up.
  std::uint8_t advance; // 0 for a combining mark.
};

struct Font {
  std::span<const Glyph> glyphs; // Sorted by codepoint.
  const std::uint8_t *bits = nullptr;
  int ascent = 0;  // Ink extent of the baked set above the baseline.
  int descent = 0; // And below it, marks included.
  // The font that draws a code point this one lacks, or null. The e-paper
  // UI's Latin fonts name the Arabic font of their size here.
  const Font *fallback = nullptr;
};

// The glyph for `cp`, or nullptr when the font lacks it.
[[nodiscard]] const Glyph *find_glyph(const Font &font, std::uint32_t cp);

namespace fonts {
// 400x300 panels, in os_arabic_fonts_s_gen.cpp.
extern const Font ar_body_s;    // Body and row text.
extern const Font ar_heading_s; // Page titles.
// 800x480 panels, about 1.6 times the small sizes, in
// os_arabic_fonts_l_gen.cpp.
extern const Font ar_body_l;
extern const Font ar_heading_l;
} // namespace fonts

} // namespace opensensor::text
