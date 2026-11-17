// clang-format off
// GENERATED from firmware/common/sensors/modules/sen63c/module.json by tools/plugin_contract.py.
// Do not edit. Change the manifest and run `pixi run plugin-headers`.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ossensor::sen63c {
inline constexpr std::string_view kProcedure = "sen63c";
enum class Column : std::uint8_t {
  sen6x_status,
  sen6x_device_status,
  pm1_mass_ug_m3,
  pm25_mass_ug_m3,
  pm4_mass_ug_m3,
  pm10_mass_ug_m3,
  ambient_temperature_c,
  relative_humidity_percent,
  co2_ppm,
};
inline constexpr std::array<std::string_view, 9> kColumns{
    "sen6x_status",
    "sen6x_device_status",
    "pm1_mass_ug_m3",
    "pm25_mass_ug_m3",
    "pm4_mass_ug_m3",
    "pm10_mass_ug_m3",
    "ambient_temperature_c",
    "relative_humidity_percent",
    "co2_ppm",
};
enum class Segment : std::uint8_t {
  core,
};
inline constexpr std::array<Segment, 9> kColumnSegments{
    Segment::core,
    Segment::core,
    Segment::core,
    Segment::core,
    Segment::core,
    Segment::core,
    Segment::core,
    Segment::core,
    Segment::core,
};
inline constexpr Column kStatusColumn = Column::sen6x_status;
inline constexpr std::int32_t kStatusMissing = 0;
inline constexpr std::int32_t kStatusWarming = 1;
inline constexpr std::int32_t kStatusStale = 2;
inline constexpr std::int32_t kStatusSensorError = 3;
inline constexpr std::int32_t kStatusValid = 4;
} // namespace ossensor::sen63c
// clang-format on
