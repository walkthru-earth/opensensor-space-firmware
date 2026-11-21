#pragma once

// Pixel geometry shared by the canvas, the layouts and the panel interface.

#include <cstdint>

namespace opensensor::ui {

enum class Ink : std::uint8_t { White, Black };

[[nodiscard]] constexpr Ink invert(Ink ink) {
  return ink == Ink::Black ? Ink::White : Ink::Black;
}

struct Point {
  int x = 0;
  int y = 0;
};

struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  [[nodiscard]] constexpr int right() const { return x + w; }
  [[nodiscard]] constexpr int bottom() const { return y + h; }
  bool operator==(const Rect &) const = default;
};

enum class Align : std::uint8_t { Left, Center, Right };

} // namespace opensensor::ui
