#include "os_text_font.h"

#include <algorithm>

namespace opensensor::text {

const Glyph *find_glyph(const Font &font, std::uint32_t cp) {
  const auto it = std::lower_bound(
      font.glyphs.begin(), font.glyphs.end(), cp,
      [](const Glyph &g, std::uint32_t value) { return g.codepoint < value; });
  if (it == font.glyphs.end() || it->codepoint != cp)
    return nullptr;
  return &*it;
}

} // namespace opensensor::text
