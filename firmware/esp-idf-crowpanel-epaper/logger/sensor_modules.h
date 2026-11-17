#pragma once

// The CrowPanel sensor module of the plugin contract (#17 step 5,
// docs/shared/plugins/README.md section 10). The board Hooks::collect loops
// over the static constexpr table that table() builds, one entry per module
// the board.json lists. The module writes only its own generated columns
// through ossensor::Writer. A column resolves by name at build time.
//
// Pure and host tested. The board task fills a Sen63cSnapshot from its 1 s
// SEN63C poll, and tools/test_crowpanel_contract.cpp runs the same table
// with fake snapshots. reading_of() is the one place that decides the status
// and which values are good. The row and the e-paper both use it, so the
// screen never shows a value the file holds as null (#165).

#include "telemetry_contract.h"
#include <array>
#include <cstdint>
#include <optional>
#include <os_sen6x.h>
#include <os_sensor_module.h>
#include <sen63c/sen63c_module.h>
#include <span>

namespace board_modules {
namespace contract = telemetry::contract;
namespace sen63c = ossensor::sen63c;

// What the board task knows about the SEN63C. Board task only, the UI gets
// a copy of the derived Sen63cReading.
struct Sen63cSnapshot {
  // The sensor answered on the bus. Cleared after repeated failures.
  bool present = false;
  // Continuous measurement was started and has not been lost since.
  bool measuring = false;
  // Monotonic time of the last successful start, the warm-up origin.
  std::int64_t start_mono_us = 0;
  // A measured values read succeeded since the last start.
  bool has_values = false;
  // Monotonic time of that read.
  std::int64_t received_mono_us = 0;
  opensensor::Sen63cValues values{};
  // A device status read succeeded since boot.
  bool has_device_status = false;
  // Every status bit read since the last row, ORed, so an error bit the
  // board read and cleared between two rows still reaches the next one.
  std::uint32_t device_status = 0;
  // The most recent status read, what the latch restarts from.
  std::uint32_t device_status_latest = 0;
};

// One classification of a snapshot at `now`, everything the row and the
// screen need. An empty optional is a null cell and a dash on screen.
struct Sen63cReading {
  std::int32_t status = sen63c::kStatusMissing;
  std::optional<std::uint32_t> device_status;
  std::optional<float> pm1_ug_m3;
  std::optional<float> pm25_ug_m3;
  std::optional<float> pm4_ug_m3;
  std::optional<float> pm10_ug_m3;
  std::optional<float> temperature_c;
  std::optional<float> relative_humidity_percent;
  std::optional<std::int32_t> co2_ppm;
  // Whole seconds since the last start while measuring, for the warm-up
  // countdown on screen.
  std::optional<std::uint32_t> since_start_s;
};

// Datasheet operating range of the SEN63C RH and T outputs (#169). A value
// outside it is a decode or sensor fault and stays null.
inline constexpr float kTemperatureMinC = -40.0F;
inline constexpr float kTemperatureMaxC = 80.0F;
inline constexpr float kHumidityMinPercent = 0.0F;
inline constexpr float kHumidityMaxPercent = 100.0F;

// Age of the newest result. A result stamped after `now` counts as new.
[[nodiscard]] constexpr std::int64_t age_us(const Sen63cSnapshot &s,
                                            std::int64_t now) {
  return now > s.received_mono_us ? now - s.received_mono_us : 0;
}
// The sensor runs and its newest result is at most kSen6xStaleAfterUs old.
[[nodiscard]] constexpr bool fresh(const Sen63cSnapshot &s, std::int64_t now) {
  return s.present && s.measuring && s.has_values &&
         age_us(s, now) <= contract::kSen6xStaleAfterUs;
}
// Inside the PM warm-up after the last start. A start stamped after `now`
// counts as warming, so a row never reads valid before the window.
[[nodiscard]] constexpr bool warming(const Sen63cSnapshot &s,
                                     std::int64_t now) {
  return now < s.start_mono_us ||
         now - s.start_mono_us < contract::kSen6xPmWarmupUs;
}

// The sen6x_status code, in the order of the legend checks.
[[nodiscard]] constexpr std::int32_t status_of(const Sen63cSnapshot &s,
                                               std::int64_t now) {
  if (!s.present || !s.measuring)
    return sen63c::kStatusMissing;
  if (warming(s, now))
    return sen63c::kStatusWarming;
  if (!fresh(s, now))
    return sen63c::kStatusStale;
  if (opensensor::Sen6xStatus{.raw = s.device_status}.sen63c_error())
    return sen63c::kStatusSensorError;
  return sen63c::kStatusValid;
}

[[nodiscard]] constexpr std::optional<float>
in_range(std::optional<float> value, float low, float high) {
  if (value && *value >= low && *value <= high)
    return value;
  return std::nullopt;
}

[[nodiscard]] constexpr Sen63cReading reading_of(const Sen63cSnapshot &s,
                                                 std::int64_t now) {
  Sen63cReading r{};
  r.status = status_of(s, now);
  if (s.present && s.has_device_status)
    r.device_status = s.device_status;
  if (s.present && s.measuring && now >= s.start_mono_us)
    r.since_start_s =
        static_cast<std::uint32_t>((now - s.start_mono_us) / 1000000);
  // RH and T settle in seconds and stay good through the PM warm-up and a
  // PM, CO2 or fan error. Their own error bit nulls them.
  const opensensor::Sen6xStatus device{.raw = s.device_status};
  if (fresh(s, now) && !device.rht_error()) {
    r.temperature_c =
        in_range(s.values.temperature_c, kTemperatureMinC, kTemperatureMaxC);
    r.relative_humidity_percent =
        in_range(s.values.relative_humidity_percent, kHumidityMinPercent,
                 kHumidityMaxPercent);
  }
  if (r.status != sen63c::kStatusValid)
    return r;
  r.pm1_ug_m3 = s.values.pm1_ug_m3;
  r.pm25_ug_m3 = s.values.pm25_ug_m3;
  r.pm4_ug_m3 = s.values.pm4_ug_m3;
  r.pm10_ug_m3 = s.values.pm10_ug_m3;
  if (s.values.co2_ppm)
    r.co2_ppm = *s.values.co2_ppm;
  return r;
}

inline constexpr auto kSlots =
    ossensor::resolve(sen63c::kColumns, std::span{contract::kFields});

// sen63c core. Apply only to a fresh, zeroed row.
inline void write_sen63c(oslogger::Row &row, const Sen63cReading &r) {
  using enum sen63c::Column;
  const ossensor::Writer<sen63c::Column, contract::kFields, kSlots> out{row};
  out.integer<sen6x_status>(r.status);
  if (r.device_status)
    out.counter<sen6x_device_status>(*r.device_status);
  if (r.pm1_ug_m3)
    out.number<pm1_mass_ug_m3>(*r.pm1_ug_m3);
  if (r.pm25_ug_m3)
    out.number<pm25_mass_ug_m3>(*r.pm25_ug_m3);
  if (r.pm4_ug_m3)
    out.number<pm4_mass_ug_m3>(*r.pm4_ug_m3);
  if (r.pm10_ug_m3)
    out.number<pm10_mass_ug_m3>(*r.pm10_ug_m3);
  if (r.temperature_c)
    out.number<ambient_temperature_c>(*r.temperature_c);
  if (r.relative_humidity_percent)
    out.number<relative_humidity_percent>(*r.relative_humidity_percent);
  if (r.co2_ppm)
    out.integer<co2_ppm>(*r.co2_ppm);
}

// The board reads behind the module. Board task only.
struct Sources {
  Sen63cSnapshot *sensor = nullptr;
  // The reading of the last row, kept for the screen.
  Sen63cReading last{};
};

// Module::collect entry. Writes the row from the snapshot, keeps the
// reading and restarts the status latch from the newest read.
inline void collect_sen63c(oslogger::Row &row, std::int64_t now,
                           void *context) {
  auto &sources = *static_cast<Sources *>(context);
  sources.last = reading_of(*sources.sensor, now);
  write_sen63c(row, sources.last);
  sources.sensor->device_status = sources.sensor->device_status_latest;
}

// The module table in board.json order. The board task owns the I2C
// bring-up, so the module has no init.
[[nodiscard]] consteval std::array<ossensor::Module, 1>
table(Sources &sources) {
  return {{
      {.procedure = sen63c::kProcedure,
       .init = nullptr,
       .collect = collect_sen63c,
       .context = &sources},
  }};
}

// The board Hooks::collect body.
inline void collect(std::span<const ossensor::Module> modules,
                    oslogger::Row &row, std::int64_t now) {
  for (const auto &module : modules)
    module.collect(row, now, module.context);
}
} // namespace board_modules
