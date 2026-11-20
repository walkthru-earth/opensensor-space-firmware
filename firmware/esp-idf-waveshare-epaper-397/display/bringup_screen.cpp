// TEMPORARY, see bringup_screen.h. The glyphs are a 3x5 block font drawn
// for this screen, each lit cell one filled square.
#include "bringup_screen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <optional>
#include <string_view>

namespace opensensor::bringup {
namespace {

constexpr int kStride = kWidth / 8;

struct Glyph {
  char c = ' ';
  std::uint16_t bits = 0; // 5 rows of 3, top left is bit 14.
};

// 15 characters, '#' lit, anything else dark.
consteval std::uint16_t rows(std::string_view s) {
  return std::accumulate(
      s.begin(), s.end(), std::uint16_t{0}, [](std::uint16_t out, char c) {
        return static_cast<std::uint16_t>((out << 1) | (c == '#' ? 1 : 0));
      });
}

static constexpr std::array<Glyph, 35> kGlyphs{{
    {.c = '0', .bits = rows("####.##.##.####")},
    {.c = '1', .bits = rows(".#.##..#..#.###")},
    {.c = '2', .bits = rows("###..#####..###")},
    {.c = '3', .bits = rows("###..#.##..####")},
    {.c = '4', .bits = rows("#.##.####..#..#")},
    {.c = '5', .bits = rows("####..###..####")},
    {.c = '6', .bits = rows("####..####.####")},
    {.c = '7', .bits = rows("###..#..#..#..#")},
    {.c = '8', .bits = rows("####.#####.####")},
    {.c = '9', .bits = rows("####.####..####")},
    {.c = '.', .bits = rows(".............#.")},
    {.c = '-', .bits = rows("......###......")},
    {.c = '/', .bits = rows("..#..#.#.#..#..")},
    {.c = '%', .bits = rows("#.#..#.#.#..#.#")},
    {.c = '=', .bits = rows("...###...###...")},
    {.c = 'A', .bits = rows(".#.#.#####.##.#")},
    {.c = 'C', .bits = rows("####..#..#..###")},
    {.c = 'D', .bits = rows("##.#.##.##.###.")},
    {.c = 'E', .bits = rows("####..##.#..###")},
    {.c = 'F', .bits = rows("####..##.#..#..")},
    {.c = 'G', .bits = rows("####..#.##.####")},
    {.c = 'H', .bits = rows("#.##.#####.##.#")},
    {.c = 'I', .bits = rows("###.#..#..#.###")},
    {.c = 'K', .bits = rows("#.##.###.#.##.#")},
    {.c = 'L', .bits = rows("#..#..#..#..###")},
    {.c = 'M', .bits = rows("#.########.##.#")},
    {.c = 'N', .bits = rows("##.#.##.##.##.#")},
    {.c = 'O', .bits = rows("####.##.##.####")},
    {.c = 'P', .bits = rows("####.#####..#..")},
    {.c = 'R', .bits = rows("##.#.###.#.##.#")},
    {.c = 'S', .bits = rows("####..###..####")},
    {.c = 'T', .bits = rows("###.#..#..#..#.")},
    {.c = 'U', .bits = rows("#.##.##.##.####")},
    {.c = 'W', .bits = rows("#.##.########.#")},
    {.c = ' ', .bits = 0},
}};

std::uint16_t glyph_bits(char c) {
  const auto *found = std::ranges::find(kGlyphs, c, &Glyph::c);
  return found == kGlyphs.end() ? std::uint16_t{0} : found->bits;
}

class Painter {
public:
  explicit Painter(std::span<std::uint8_t, kFrameBytes> frame)
      : frame_(frame) {}

  void clear() { std::ranges::fill(frame_, std::uint8_t{0}); }

  // Clipped to the panel.
  void fill(int x, int y, int w, int h) {
    const int x0 = x < 0 ? 0 : x;
    const int y0 = y < 0 ? 0 : y;
    const int x1 = x + w > kWidth ? kWidth : x + w;
    const int y1 = y + h > kHeight ? kHeight : y + h;
    for (int row = y0; row < y1; ++row)
      for (int col = x0; col < x1; ++col)
        frame_[static_cast<std::size_t>(row * kStride + col / 8)] |=
            static_cast<std::uint8_t>(0x80U >> (col % 8));
  }

  void frame_rect(int x, int y, int w, int h, int t) {
    fill(x, y, w, t);
    fill(x, y + h - t, w, t);
    fill(x, y, t, h);
    fill(x + w - t, y, t, h);
  }

  // Each glyph is 3 by 5 cells of `cell` pixels with one empty cell after
  // it. Returns the x after the text.
  int text(int x, int y, std::string_view s, int cell) {
    for (char c : s) {
      if (c >= 'a' && c <= 'z')
        c = static_cast<char>(c - 'a' + 'A');
      const std::uint16_t bits = glyph_bits(c);
      for (int r = 0; r < 5; ++r)
        for (int k = 0; k < 3; ++k)
          if (bits & (1U << (14 - (r * 3 + k))))
            fill(x + k * cell, y + r * cell, cell, cell);
      x += 4 * cell;
    }
    return x;
  }

  static int text_width(std::string_view s, int cell) {
    return s.empty() ? 0 : static_cast<int>(s.size()) * 4 * cell - cell;
  }

private:
  std::span<std::uint8_t, kFrameBytes> frame_;
};

struct Box {
  const char *label = "";
  const char *unit = "";
  std::optional<float> value;
  int decimals = 0;
};

void draw_box(Painter &p, int x, int y, int w, int h, const Box &box) {
  p.frame_rect(x + 6, y + 6, w - 12, h - 12, 4);
  p.text(x + 26, y + 24, box.label, 6);
  std::array<char, 16> text{};
  if (box.value && std::isfinite(*box.value))
    std::snprintf(text.data(), text.size(), "%.*f", box.decimals,
                  static_cast<double>(*box.value));
  else
    std::snprintf(text.data(), text.size(), "-");
  // 16 px cells fit 4 glyphs in a box, 13 px cells fit 5.
  const std::string_view value{text.data()};
  const int cell = value.size() <= 4 ? 16 : 13;
  p.text(x + (w - Painter::text_width(value, cell)) / 2, y + 76, value, cell);
  p.text(x + w - 26 - Painter::text_width(box.unit, 5), y + h - 50, box.unit,
         5);
}

} // namespace

void render(std::span<std::uint8_t, kFrameBytes> frame, const Values &v) {
  Painter p{frame};
  p.clear();
  constexpr int kStatusH = 48;
  constexpr int kBoxW = kWidth / 2;
  constexpr int kBoxH = (kHeight - kStatusH) / 2;
  const std::array<Box, 4> boxes{{
      {.label = "PM2.5", .unit = "UG/M3", .value = v.pm25_ug_m3, .decimals = 1},
      {.label = "CO2", .unit = "PPM", .value = v.co2_ppm, .decimals = 0},
      {.label = "TEMP", .unit = "C", .value = v.temperature_c, .decimals = 1},
      {.label = "RH",
       .unit = "%",
       .value = v.relative_humidity_percent,
       .decimals = 0},
  }};
  for (std::size_t i = 0; i < boxes.size(); ++i)
    draw_box(p, static_cast<int>(i % 2) * kBoxW,
             static_cast<int>(i / 2) * kBoxH, kBoxW, kBoxH, boxes[i]);
  std::array<char, 64> status{};
  std::snprintf(status.data(), status.size(), "SEN63C %s  SD %s  LOG %s  %lu",
                v.sensor_present ? "OK" : "NO", v.card_mounted ? "OK" : "NO",
                v.logging ? "ON" : "OFF", static_cast<unsigned long>(v.rows));
  p.text(16, kHeight - kStatusH + 12, status.data(), 5);
}

} // namespace opensensor::bringup
