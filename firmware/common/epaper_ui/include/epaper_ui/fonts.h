#pragma once

// 1 bit bitmap fonts baked from Inter by tools/epaper_gen_fonts.py. The
// glyph tables are static constexpr, so they stay in flash .rodata. Each set
// lives in its own generated file, so an image links only the set its layout
// names.
//
// Glyph and Font are the text component's types, so one blitter draws Inter
// and the Arabic Cairo fonts. Each Latin font names the Arabic font of its
// size as its fallback, the compact set the _s sizes and the large set the
// _l sizes, and the hero font none.

#include "os_text_font.h"

namespace opensensor::ui {

using Glyph = text::Glyph;
using Font = text::Font;
using text::find_glyph;

// The five roles the pages draw with. A layout names one set.
struct FontSet {
  const Font &small; // Header, axes and fine print.
  const Font &label; // Tile and row labels.
  const Font &title; // Page titles.
  const Font &value; // Tile values.
  const Font &hero;  // Digits, '.', '-' and space only.
};

namespace fonts {
// 13, 14, 18, 28 and 64 px, for a 400 x 300 class panel.
extern const FontSet compact;
// 20, 22, 30, 46 and 112 px, for an 800 x 480 class panel.
extern const FontSet large;
} // namespace fonts

} // namespace opensensor::ui
