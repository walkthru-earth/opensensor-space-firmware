#pragma once

// US EPA AQI categories for PM2.5 from the 2024 PM NAAQS revision, as listed
// in the EPA "Technical Assistance Document for the Reporting of Daily Air
// Quality, the Air Quality Index (AQI)", EPA-454/B-24-002, May 2024, table 6.
// https://document.airnow.gov/technical-assistance-document-for-the-reporting-of-daily-air-quailty.pdf
// The document truncates PM2.5 to one decimal before the lookup. The
// breakpoints are for a 24 h mean, the dashboard applies them to the live
// value as a guide and says so on the status page.

#include "epaper_ui/strings.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace opensensor::ui {

enum class AirBand : std::uint8_t {
  Good,
  Moderate,
  SensitiveGroups,
  Unhealthy,
  VeryUnhealthy,
  Hazardous,
};

struct AirBandRow {
  float upper = 0.0F; // Inclusive upper bound in µg/m³ after truncation.
  AirBand band = AirBand::Good;
  Str word;
};

inline constexpr std::array<AirBandRow, 6> kPm25Bands{{
    {.upper = 9.0F, .band = AirBand::Good, .word = Str::BandGood},
    {.upper = 35.4F, .band = AirBand::Moderate, .word = Str::BandModerate},
    {.upper = 55.4F,
     .band = AirBand::SensitiveGroups,
     .word = Str::BandSensitive},
    {.upper = 125.4F, .band = AirBand::Unhealthy, .word = Str::BandUnhealthy},
    {.upper = 225.4F,
     .band = AirBand::VeryUnhealthy,
     .word = Str::BandVeryUnhealthy},
    {.upper = INFINITY, .band = AirBand::Hazardous, .word = Str::BandHazardous},
}};

// The row for a PM2.5 value. Negative and NaN inputs never reach here, the
// caller only bands a Valid reading.
[[nodiscard]] inline const AirBandRow &pm25_band(float ugm3) {
  // Truncate to 0.1 the way the EPA table does. The small epsilon keeps a
  // value like 9.0 that float stores as 8.99999 inside its band.
  const float tenths = std::floor(ugm3 * 10.0F + 1e-3F) / 10.0F;
  const auto row = std::ranges::find_if(kPm25Bands, [&](const AirBandRow &b) {
    return tenths <= b.upper + 1e-4F;
  });
  return row == kPm25Bands.end() ? kPm25Bands.back() : *row;
}

} // namespace opensensor::ui
