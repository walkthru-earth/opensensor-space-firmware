#pragma once

// What the pages draw. The board fills a Model from its own sample and state
// each cycle, the pages never read hardware. A value that is not Valid is
// never printed, so a warming or missing sensor shows a dash, not a guess.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "epaper_ui/strings.h"

namespace opensensor::ui {

enum class Metric : std::uint8_t { Pm1, Pm25, Pm4, Pm10, Co2, Temp, Rh };
inline constexpr std::size_t kMetricCount = 7;

[[nodiscard]] constexpr std::size_t index(Metric m) {
  return static_cast<std::size_t>(m);
}

struct MetricInfo {
  Str label; // Tile and chart title.
  Str unit;
  int decimals = 0;
  // The lowest and highest value a board can hand the UI. The value box
  // reserves a digit cell for each whole digit of the wider one and a sign
  // when the lowest is negative, see value_slots().
  float lowest = 0.0F;
  float highest = 0.0F;
  // Seconds after the measurement starts before a value is shown. SEN63C PM
  // settles after 30 s and its CO2 reads unknown for up to 24 s.
  std::uint16_t warmup_s = 0;
  // The smallest span a chart axis shows, so sensor noise on a steady room
  // does not fill the whole height.
  float min_span = 0.0F;
  bool zero_floor = false; // Chart axes start at 0.
};

// The value ranges the boards feed the UI today, all from the SEN63C.
// PM mass is a uint16 word over 10 with 0xFFFF as unknown, datasheet table
// 32 and os_sen6x.h, so at most 6553.4 ug/m3, past the 0 to 1000 plausible
// range of sen63c/module.json and four whole digits either way. CO2 is an
// int16 word with 0x7FFF as unknown, so at most 32766 ppm, past the 380 to
// 32000 plausible range. The boards keep T in -40 to 80 C and RH in 0 to
// 100 percent and null anything outside (#169, kTemperatureMinC and its
// neighbours in each board's logger/sensor_modules.h).
inline constexpr float kPmHighest = 6553.4F;
inline constexpr float kCo2Highest = 32766.0F;
inline constexpr float kTempLowest = -40.0F;
inline constexpr float kTempHighest = 80.0F;
inline constexpr float kRhHighest = 100.0F;

inline constexpr std::array<MetricInfo, kMetricCount> kMetricInfo{{
    {.label = Str::MetricPm1,
     .unit = Str::UnitUgM3,
     .decimals = 1,
     .lowest = 0.0F,
     .highest = kPmHighest,
     .warmup_s = 30,
     .min_span = 5.0F,
     .zero_floor = true},
    {.label = Str::MetricPm25,
     .unit = Str::UnitUgM3,
     .decimals = 1,
     .lowest = 0.0F,
     .highest = kPmHighest,
     .warmup_s = 30,
     .min_span = 5.0F,
     .zero_floor = true},
    {.label = Str::MetricPm4,
     .unit = Str::UnitUgM3,
     .decimals = 1,
     .lowest = 0.0F,
     .highest = kPmHighest,
     .warmup_s = 30,
     .min_span = 5.0F,
     .zero_floor = true},
    {.label = Str::MetricPm10,
     .unit = Str::UnitUgM3,
     .decimals = 1,
     .lowest = 0.0F,
     .highest = kPmHighest,
     .warmup_s = 30,
     .min_span = 5.0F,
     .zero_floor = true},
    {.label = Str::MetricCo2,
     .unit = Str::UnitPpm,
     .decimals = 0,
     .lowest = 0.0F,
     .highest = kCo2Highest,
     .warmup_s = 24,
     .min_span = 100.0F,
     .zero_floor = false},
    {.label = Str::MetricTemp,
     .unit = Str::UnitCelsius,
     .decimals = 1,
     .lowest = kTempLowest,
     .highest = kTempHighest,
     .warmup_s = 2,
     .min_span = 2.0F,
     .zero_floor = false},
    {.label = Str::MetricRh,
     .unit = Str::UnitPercentRh,
     .decimals = 0,
     .lowest = 0.0F,
     .highest = kRhHighest,
     .warmup_s = 2,
     .min_span = 10.0F,
     .zero_floor = false},
}};

[[nodiscard]] constexpr const MetricInfo &info(Metric m) {
  return kMetricInfo[index(m)];
}

// The digit cells a metric's value reserves on screen, so each digit of a
// value keeps one place from refresh to refresh. `whole` cells left of the
// decimal point, `frac` right of it, and room for a minus sign before them
// when `sign`.
struct ValueSlots {
  int whole = 1;
  int frac = 0;
  bool sign = false;
};

// The whole digits format_reading() prints for `v` with `decimals`.
[[nodiscard]] constexpr int whole_digits(float v, int decimals) {
  double rounded = v < 0.0F ? -static_cast<double>(v) : static_cast<double>(v);
  double half = 0.5;
  for (int i = 0; i < decimals; ++i)
    half /= 10.0;
  rounded += half;
  int digits = 1;
  for (double d = 10.0; rounded >= d; d *= 10.0)
    ++digits;
  return digits;
}

[[nodiscard]] constexpr ValueSlots value_slots(Metric m) {
  const MetricInfo &mi = info(m);
  return {.whole = std::max(whole_digits(mi.lowest, mi.decimals),
                            whole_digits(mi.highest, mi.decimals)),
          .frac = mi.decimals,
          .sign = mi.lowest < 0.0F};
}

static_assert(value_slots(Metric::Pm25).whole == 4);
static_assert(value_slots(Metric::Co2).whole == 5);
static_assert(value_slots(Metric::Temp).whole == 2 &&
              value_slots(Metric::Temp).sign);
static_assert(value_slots(Metric::Rh).whole == 3 &&
              !value_slots(Metric::Rh).sign);

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

// The battery the header shows, set only by a board that reads one and has
// a charge to show. A board with no battery, or none fitted, leaves
// Status::battery empty and the header draws as it always has.
struct Battery {
  std::uint8_t percent = 0; // 0 to 100, a larger value draws as 100.
  bool charging = false;    // True only when the board reads that it charges.
};

struct Status {
  std::optional<std::int64_t> utc_s; // Unix seconds, nullopt until anchored.
  bool sd_ok = false;
  std::uint64_t sd_free_bytes = 0;
  Link ble = Link::Off;
  Link wifi = Link::Off;
  std::uint32_t samples = 0;
  std::uint32_t uptime_s = 0;
  std::string_view firmware;     // Firmware identity string.
  std::string_view thing;        // Short Thing id.
  std::string_view sensor_model; // "SEN63C" style, the board names its part.
  std::string_view sensor_serial;
  std::string_view sensor_fw;                 // "2.0" style.
  std::optional<std::uint32_t> sensor_status; // SEN6x device status register.
  bool sensor_present = false;
  // The BLE passkey while a phone pairs, 0 to 999999. Set only while the
  // pairing is active, then every page gives way to the pairing screen. The
  // screen is the one place the passkey appears, never a log.
  std::optional<std::uint32_t> pairing_passkey;
  std::string_view device_name; // BLE local name, shown with the passkey.
  std::optional<Battery> battery;
};

struct Model {
  std::array<Reading, kMetricCount> now{};
  Status status{};
};

// Writes the value with the metric's decimals, or Str::NoValue when it is
// not Valid.
// Returns the text inside `buf`.
std::string_view format_reading(std::span<char> buf, Metric m,
                                const Reading &r);

} // namespace opensensor::ui
