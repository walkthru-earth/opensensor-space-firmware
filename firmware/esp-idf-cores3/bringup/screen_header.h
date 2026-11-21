#pragma once

// CoreS3 screen header layout and the battery icon geometry. Pure, so the
// host contract test compiles it on its own and the static_asserts below run
// there as well as in the board build.
//
// The panel is 320 x 240 and the page scroll bar sits at x 316, so the header
// stays inside x 0..315. M5GFX draws its default font at 6 x 8 pixels per
// character in text size 1 and 12 x 16 in text size 2.
//
//   title, size 2      x 8, y 5, up to x 187 for the longest title
//   row 1, size 1      y 4, WiFi at x 196, BT at x 244, page "1/4" at x 296
//   row 2, size 1      y 17, battery outline x 196..217 with its nub to x 219,
//                      percent at x 224 to x 247, charging bolt x 251..257
//
// The page content starts at y 31, below both rows.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace screen_header {

inline constexpr int kGlyphWidth = 6; // per character, text size 1
inline constexpr int kRightEdge = 315;

inline constexpr std::array<std::string_view, 4> kTitles = {
    "PM mass", "Particle counts", "Device health", "Bluetooth"};
inline constexpr int kTitleX = 8;
inline constexpr int kTitleY = 5;
inline constexpr int kTitleSize = 2;

// Row 1, the link labels and the page counter.
inline constexpr int kRow1Y = 4;
inline constexpr int kWifiX = 196;
inline constexpr std::size_t kWifiChars = 7; // "WiFi ok", "WiFi up", "WiFi .."
inline constexpr int kBtX = 244;
inline constexpr std::size_t kBtChars = 6; // "BT adv", longer than "BT ok"
inline constexpr int kPageX = 296;
inline constexpr std::size_t kPageChars = 3; // "1/4"

// Row 2, the battery. Outline, a nub on the right, then a fill inset by two
// pixels on every side, so one pixel of black separates fill and outline.
inline constexpr int kBatteryX = 196;
inline constexpr int kBatteryY = 16;
inline constexpr int kBatteryWidth = 22;
inline constexpr int kBatteryHeight = 10;
inline constexpr int kNubWidth = 2;
inline constexpr int kNubHeight = 4;
inline constexpr int kFillInset = 2;
inline constexpr int kFillMaxPx = kBatteryWidth - 2 * kFillInset;
inline constexpr int kFillHeight = kBatteryHeight - 2 * kFillInset;
inline constexpr int kPercentX = 224;
inline constexpr int kPercentY = 17;
inline constexpr std::size_t kPercentChars = 4; // "100%"
inline constexpr int kBoltX = 251;
inline constexpr int kBoltWidth = 7;

[[nodiscard]] constexpr int text_end(int x, std::size_t chars, int size = 1) {
  return x + static_cast<int>(chars) * kGlyphWidth * size - 1;
}

[[nodiscard]] constexpr std::int32_t clamp_percent(std::int32_t percent) {
  return percent < 0 ? 0 : percent > 100 ? 100 : percent;
}

// Fill width in pixels for a fuel gauge percent, rounded to the nearest pixel.
[[nodiscard]] constexpr int fill_px(std::int32_t percent) {
  return static_cast<int>((clamp_percent(percent) * kFillMaxPx + 50) / 100);
}

// Fill colour band. Green above 50, yellow from 20 to 50, red below 20.
enum class Band : std::uint8_t { Low, Mid, High };

[[nodiscard]] constexpr Band band_of(std::int32_t percent) {
  const auto clamped = clamp_percent(percent);
  return clamped > 50 ? Band::High : clamped >= 20 ? Band::Mid : Band::Low;
}

static_assert(kFillMaxPx == 18 && kFillHeight == 6);
static_assert(fill_px(0) == 0);
static_assert(fill_px(50) == 9);
static_assert(fill_px(100) == kFillMaxPx);
static_assert(fill_px(101) == kFillMaxPx && fill_px(255) == kFillMaxPx);
static_assert(fill_px(-1) == 0);
static_assert(band_of(0) == Band::Low && band_of(19) == Band::Low);
static_assert(band_of(20) == Band::Mid && band_of(50) == Band::Mid);
static_assert(band_of(51) == Band::High && band_of(100) == Band::High);

[[nodiscard]] constexpr bool titles_fit() {
  return std::ranges::none_of(kTitles, [](std::string_view title) {
    return text_end(kTitleX, title.size(), kTitleSize) >= kWifiX - 4;
  });
}

// Nothing overlaps and everything stays left of the scroll bar.
static_assert(titles_fit(), "a page title runs into the header status labels");
static_assert(text_end(kWifiX, kWifiChars) < kBtX - 2);
static_assert(text_end(kBtX, kBtChars) < kPageX - 2);
static_assert(text_end(kPageX, kPageChars) <= kRightEdge);
static_assert(kBatteryX >= kWifiX && kBatteryY > kRow1Y + 8);
static_assert(kBatteryX + kBatteryWidth + kNubWidth < kPercentX);
static_assert(text_end(kPercentX, kPercentChars) < kBoltX - 2);
static_assert(kBoltX + kBoltWidth - 1 <= kRightEdge);
static_assert(kBatteryY + kBatteryHeight <= 31, "header ends above the page");

} // namespace screen_header
