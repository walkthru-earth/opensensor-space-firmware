#pragma once

#include <embedded_h3/location.h>

#include <cstddef>
#include <expected>

// Firmware location policy over the embedded-h3-location library
// (walkthru-earth/embedded-h3-location, pinned in ../idf_component.yml and
// the board dependencies.lock files). The library does the H3 work and sets
// no resolution limit. This header adds the one firmware rule, every
// published cell is H3 res 8, and re-exports the library names the firmware
// uses so call sites read location::format_cell and so on.
//
// Persist only the result of published_cell(), and derive public coordinates
// from that cell. A cell center is not the original GPS position or an
// administrative country lookup. Country is owner-declared only.
namespace opensensor::location {
using embedded_h3::location::Cell;
using embedded_h3::location::Degrees;
using embedded_h3::location::E7;
using embedded_h3::location::Error;
using embedded_h3::location::LngLat;
using embedded_h3::location::MultiPolygon;
using embedded_h3::location::MultiPolygonCapacity;
using embedded_h3::location::Polygon;
using embedded_h3::location::Ring;

using embedded_h3::location::cells_to_multipolygon;
using embedded_h3::location::center_degrees;
using embedded_h3::location::center_e7;
using embedded_h3::location::coarsen;
using embedded_h3::location::error_name;
using embedded_h3::location::format_cell;
using embedded_h3::location::from_degrees;
using embedded_h3::location::multipolygon_capacity;
using embedded_h3::location::parse_cell;
using embedded_h3::location::resolution;
using embedded_h3::location::valid_cell;
using embedded_h3::location::valid_country;

// Every published cell is H3 res 8 (owner decision OD-2026-11-04-B, #139).
// The config key location.resolution accepts only this value. The library
// sets no limit, so the firmware passes this cap on every call.
inline constexpr int kPublishedResolution = 8;
// 15 lowercase hex digits and a NUL.
inline constexpr std::size_t kCellTextBytes =
    embedded_h3::location::kCellTextBytes;
static_assert(kCellTextBytes == 16);

// The res 8 cell to publish for `input`. A finer cell becomes its res 8
// parent, a res 8 cell is kept and a coarser one is refused with TooCoarse,
// never upsampled. Not an H3 cell, the absent cell 0 included, is
// InvalidCell.
[[nodiscard]] inline std::expected<Cell, Error>
published_cell(Cell input) noexcept {
  return embedded_h3::location::parent_at(input, kPublishedResolution);
}
} // namespace opensensor::location
