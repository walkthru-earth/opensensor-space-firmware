#include "canvas.h"

#include <algorithm>
#include <cstdlib>

namespace opensensor::ui {

const Glyph *find_glyph(const Font &font, std::uint32_t cp) {
  const auto it = std::lower_bound(
      font.glyphs.begin(), font.glyphs.end(), cp,
      [](const Glyph &g, std::uint32_t value) { return g.codepoint < value; });
  if (it == font.glyphs.end() || it->codepoint != cp)
    return nullptr;
  return &*it;
}

std::uint32_t next_codepoint(std::string_view utf8, std::size_t &pos) {
  constexpr std::uint32_t kReplacement = 0xFFFD;
  const auto byte = [&](std::size_t i) {
    return static_cast<std::uint8_t>(utf8[i]);
  };
  const std::uint8_t lead = byte(pos);
  int extra = 0;
  std::uint32_t cp = 0;
  if (lead < 0x80) {
    ++pos;
    return lead;
  }
  if ((lead & 0xE0) == 0xC0) {
    extra = 1;
    cp = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    extra = 2;
    cp = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    extra = 3;
    cp = lead & 0x07;
  } else {
    ++pos;
    return kReplacement;
  }
  if (pos + static_cast<std::size_t>(extra) >= utf8.size()) {
    ++pos;
    return kReplacement;
  }
  for (int i = 1; i <= extra; ++i) {
    const std::uint8_t b = byte(pos + static_cast<std::size_t>(i));
    if ((b & 0xC0) != 0x80) {
      ++pos;
      return kReplacement;
    }
    cp = (cp << 6) | (b & 0x3F);
  }
  pos += static_cast<std::size_t>(extra) + 1;
  return cp;
}

int text_width(const Font &font, std::string_view utf8) {
  int width = 0;
  std::size_t pos = 0;
  while (pos < utf8.size()) {
    const Glyph *g = find_glyph(font, next_codepoint(utf8, pos));
    if (g != nullptr)
      width += g->advance;
  }
  return width;
}

void Canvas::clear(Ink ink) {
  std::fill(frame_.begin(), frame_.end(),
            ink == Ink::Black ? std::uint8_t{0xFF} : std::uint8_t{0x00});
}

void Canvas::pixel(int x, int y, Ink ink) {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight)
    return;
  const auto index = static_cast<std::size_t>(y * kStride + (x >> 3));
  const auto mask = static_cast<std::uint8_t>(0x80u >> (x & 7));
  if (ink == Ink::Black)
    frame_[index] = static_cast<std::uint8_t>(frame_[index] | mask);
  else
    frame_[index] = static_cast<std::uint8_t>(frame_[index] & ~mask);
}

Ink Canvas::get(int x, int y) const {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight)
    return Ink::White;
  const auto index = static_cast<std::size_t>(y * kStride + (x >> 3));
  return (frame_[index] & (0x80u >> (x & 7))) != 0 ? Ink::Black : Ink::White;
}

void Canvas::hline(int x, int y, int w, Ink ink) {
  if (y < 0 || y >= kHeight || w <= 0)
    return;
  const int x0 = std::max(x, 0);
  const int x1 = std::min(x + w, kWidth);
  for (int i = x0; i < x1; ++i)
    pixel(i, y, ink);
}

void Canvas::vline(int x, int y, int h, Ink ink) {
  if (x < 0 || x >= kWidth || h <= 0)
    return;
  const int y0 = std::max(y, 0);
  const int y1 = std::min(y + h, kHeight);
  for (int i = y0; i < y1; ++i)
    pixel(x, i, ink);
}

void Canvas::rect(Rect r, Ink ink) {
  if (r.w <= 0 || r.h <= 0)
    return;
  hline(r.x, r.y, r.w, ink);
  hline(r.x, r.bottom() - 1, r.w, ink);
  vline(r.x, r.y, r.h, ink);
  vline(r.right() - 1, r.y, r.h, ink);
}

void Canvas::fill(Rect r, Ink ink) {
  for (int y = r.y; y < r.bottom(); ++y)
    hline(r.x, y, r.w, ink);
}

void Canvas::invert_rect(Rect r) {
  for (int y = r.y; y < r.bottom(); ++y)
    for (int x = r.x; x < r.right(); ++x)
      pixel(x, y, invert(get(x, y)));
}

void Canvas::invert_all() {
  std::transform(
      frame_.begin(), frame_.end(), frame_.begin(),
      [](std::uint8_t byte) { return static_cast<std::uint8_t>(~byte); });
}

void Canvas::round_rect(Rect r, Ink ink) {
  if (r.w < 3 || r.h < 3) {
    rect(r, ink);
    return;
  }
  hline(r.x + 1, r.y, r.w - 2, ink);
  hline(r.x + 1, r.bottom() - 1, r.w - 2, ink);
  vline(r.x, r.y + 1, r.h - 2, ink);
  vline(r.right() - 1, r.y + 1, r.h - 2, ink);
}

void Canvas::fill_round_rect(Rect r, Ink ink) {
  if (r.w < 3 || r.h < 3) {
    fill(r, ink);
    return;
  }
  hline(r.x + 1, r.y, r.w - 2, ink);
  fill({r.x, r.y + 1, r.w, r.h - 2}, ink);
  hline(r.x + 1, r.bottom() - 1, r.w - 2, ink);
}

void Canvas::line(int x0, int y0, int x1, int y1, Ink ink) {
  const int dx = std::abs(x1 - x0);
  const int dy = -std::abs(y1 - y0);
  const int sx = x0 < x1 ? 1 : -1;
  const int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  // The longest line inside the frame has fewer steps than this, so a
  // coordinate far outside it cannot spin for long.
  for (int steps = 0; steps <= 4 * (kWidth + kHeight) + dx - dy; ++steps) {
    pixel(x0, y0, ink);
    if (x0 == x1 && y0 == y1)
      return;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void Canvas::dotted_hline(int x, int y, int w, int on, int off, Ink ink) {
  const int period = std::max(on, 1) + std::max(off, 0);
  for (int i = 0; i < w; ++i)
    if (i % period < std::max(on, 1))
      pixel(x + i, y, ink);
}

void Canvas::dotted_vline(int x, int y, int h, int on, int off, Ink ink) {
  const int period = std::max(on, 1) + std::max(off, 0);
  for (int i = 0; i < h; ++i)
    if (i % period < std::max(on, 1))
      pixel(x, y + i, ink);
}

int Canvas::text(const Font &font, int x, int y, std::string_view utf8,
                 Ink ink) {
  int pen = x;
  std::size_t pos = 0;
  while (pos < utf8.size()) {
    const Glyph *g = find_glyph(font, next_codepoint(utf8, pos));
    if (g == nullptr)
      continue;
    const int stride = (g->width + 7) / 8;
    const std::uint8_t *rows = font.bits + g->offset;
    for (int r = 0; r < g->height; ++r) {
      for (int c = 0; c < g->width; ++c) {
        if ((rows[r * stride + (c >> 3)] & (0x80u >> (c & 7))) != 0)
          pixel(pen + g->x_offset + c, y + g->y_offset + r, ink);
      }
    }
    pen += g->advance;
  }
  return pen - x;
}

int Canvas::text(const Font &font, int x, int y, std::string_view utf8,
                 Align align, Ink ink) {
  const int width = text_width(font, utf8);
  if (align == Align::Center)
    x -= width / 2;
  else if (align == Align::Right)
    x -= width;
  return text(font, x, y, utf8, ink);
}

} // namespace opensensor::ui
