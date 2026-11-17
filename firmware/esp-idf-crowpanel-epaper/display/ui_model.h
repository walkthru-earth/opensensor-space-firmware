#pragma once

// What the pages draw. The board fills a Model from its own sample and state
// each cycle, the pages never read hardware. A value that is not Valid is
// never printed, so a warming or missing sensor shows a dash, not a guess.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace opensensor::ui {

enum class Metric : std::uint8_t { Pm1, Pm25, Pm4, Pm10, Co2, Temp, Rh };
inline constexpr std::size_t kMetricCount = 7;

[[nodiscard]] constexpr std::size_t index(Metric m) {
  return static_cast<std::size_t>(m);
}

struct MetricInfo {
  std::string_view label; // Tile and chart title.
  std::string_view unit;
  int decimals = 0;
  // Seconds after the measurement starts before a value is shown. SEN63C PM
  // settles after 30 s and its CO2 reads unknown for up to 24 s.
  std::uint16_t warmup_s = 0;
  // The smallest span a chart axis shows, so sensor noise on a steady room
  // does not fill the whole height.
  float min_span = 0.0F;
  bool zero_floor = false; // Chart axes start at 0.
};

inline constexpr std::array<MetricInfo, kMetricCount> kMetricInfo{{
    {.label = "PM1",
     .unit = "µg/m³",
     .decimals = 1,
     .warmup_s = 30,
     .min_span = 5.0F,
     .zero_floor = true},
    {.label = "PM2.5",
     .unit = "µg/m³",
     .decimals = 1,
     .warmup_s = 30,
     .min_span = 5.0F,
     .zero_floor = true},
    {.label = "PM4",
     .unit = "µg/m³",
     .decimals = 1,
     .warmup_s = 30,
     .min_span = 5.0F,
     .zero_floor = true},
    {.label = "PM10",
     .unit = "µg/m³",
     .decimals = 1,
     .warmup_s = 30,
     .min_span = 5.0F,
     .zero_floor = true},
    {.label = "CO₂",
     .unit = "ppm",
     .decimals = 0,
     .warmup_s = 24,
     .min_span = 100.0F,
     .zero_floor = false},
    {.label = "Temp",
     .unit = "°C",
     .decimals = 1,
     .warmup_s = 2,
     .min_span = 2.0F,
     .zero_floor = false},
    {.label = "Humidity",
     .unit = "%RH",
     .decimals = 0,
     .warmup_s = 2,
     .min_span = 10.0F,
     .zero_floor = false},
}};

[[nodiscard]] constexpr const MetricInfo &info(Metric m) {
  return kMetricInfo[index(m)];
}

enum class Quality : std::uint8_t { Valid, Warming, Absent };

struct Reading {
  float value = 0.0F; // Meaningful only when quality is Valid.
  Quality quality = Quality::Absent;
  std::uint16_t warm_left_s = 0; // Seconds left while Warming.
};

// Classifies one value. `since_start_s` is the time since the sensor began
// measuring, nullopt when it is not measuring. A value inside the warm-up is
// Warming even when the sensor reports one.
[[nodiscard]] Reading quality_for(Metric m, std::optional<float> value,
                                  std::optional<std::uint32_t> since_start_s);

enum class Link : std::uint8_t { Off, Waiting, Connected };

struct Status {
  std::optional<std::int64_t> utc_s; // Unix seconds, nullopt until anchored.
  bool sd_ok = false;
  std::uint64_t sd_free_bytes = 0;
  Link ble = Link::Off;
  Link wifi = Link::Off;
  std::uint32_t samples = 0;
  std::uint32_t uptime_s = 0;
  std::string_view firmware; // Firmware identity string.
  std::string_view thing;    // Short Thing id.
  std::string_view sensor_serial;
  std::string_view sensor_fw;                 // "2.0" style.
  std::optional<std::uint32_t> sensor_status; // SEN6x device status register.
  bool sensor_present = false;
};

struct Model {
  std::array<Reading, kMetricCount> now{};
  Status status{};
};

// Writes the value with the metric's decimals, or "--" when it is not Valid.
// Returns the text inside `buf`.
std::string_view format_reading(std::span<char> buf, Metric m,
                                const Reading &r);

} // namespace opensensor::ui
