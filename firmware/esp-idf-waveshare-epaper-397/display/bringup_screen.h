#pragma once

// TEMPORARY bring-up screen for the Waveshare ESP32-S3-ePaper-3.97. Four
// boxes with PM2.5, CO2, temperature and RH in blocky digits drawn from
// rectangles, and one status line. It proves the panel and the sensor path
// until the board-neutral e-paper UI replaces it. Pure, no ESP-IDF.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace opensensor::bringup {

inline constexpr int kWidth = 800;
inline constexpr int kHeight = 480;
inline constexpr std::size_t kFrameBytes = kWidth / 8 * kHeight;

// An empty optional draws a dash, the screen never invents a value.
struct Values {
  std::optional<float> pm25_ug_m3;
  std::optional<float> co2_ppm;
  std::optional<float> temperature_c;
  std::optional<float> relative_humidity_percent;
  bool sensor_present = false;
  bool card_mounted = false;
  bool logging = false;
  std::uint32_t rows = 0;
};

// Draws the whole screen into `frame`, 1 bit per pixel, row major, MSB the
// leftmost pixel, a set bit black, the opensensor::epd frame layout.
void render(std::span<std::uint8_t, kFrameBytes> frame, const Values &values);

} // namespace opensensor::bringup
