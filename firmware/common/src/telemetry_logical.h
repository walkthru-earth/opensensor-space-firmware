#pragma once

#include "parquet_writer.h"

#include <cstdint>
#include <string_view>

// Parquet logical type of a field table column, chosen by its unit label.
// The annotation changes what readers present, never the stored slot. Every
// writer of a field table (logger, board contracts, host rebuild tools) uses
// this one rule, and tools/rebuild_partial.py mirrors it.
namespace telemetry {
// INT64 microseconds since the Unix epoch, TIMESTAMP(MICROS, UTC).
inline constexpr std::string_view kUtcMicrosUnit = "us_since_unix_epoch";
// INT32 code in 0..255, INTEGER(8, unsigned) so readers show UINT8.
inline constexpr std::string_view kUint8CodeUnit = "uint8_code";
// BYTE_ARRAY 2D WKB Point in OGC:CRS84, the native GEOMETRY type.
inline constexpr std::string_view kWkbPointUnit = "wkb_point_crs84";

// `location_source` codes (#83), the stac-opensensor profile list. Each board
// names them in telemetry_classes.inc. opensensor::gnss::select_location
// picks one per row. Phone comes from the RAM only location.phone_cell, on
// moving Things only (#140).
enum class LocationSource : std::int32_t {
  None = 0,
  Configured = 1,
  Gnss = 2,
  Phone = 3,
};

constexpr LogicalType logical_for(PhysicalType type, const char *unit) {
  if (!unit)
    return LogicalType::None;
  const std::string_view label = unit;
  if (type == PhysicalType::Int64 && label == kUtcMicrosUnit)
    return LogicalType::TimestampMicrosUtc;
  if (type == PhysicalType::Int32 && label == kUint8CodeUnit)
    return LogicalType::Uint8;
  if (type == PhysicalType::ByteArray && label == kWkbPointUnit)
    return LogicalType::Geometry;
  return LogicalType::None;
}

static_assert(logical_for(PhysicalType::Int64, "us_since_unix_epoch") ==
              LogicalType::TimestampMicrosUtc);
static_assert(logical_for(PhysicalType::Int32, "us_since_unix_epoch") ==
              LogicalType::None);
static_assert(logical_for(PhysicalType::Int32, "uint8_code") ==
              LogicalType::Uint8);
static_assert(logical_for(PhysicalType::ByteArray, "wkb_point_crs84") ==
              LogicalType::Geometry);
static_assert(logical_for(PhysicalType::ByteArray, "code") ==
              LogicalType::None);
static_assert(logical_for(PhysicalType::Int64, nullptr) == LogicalType::None);
} // namespace telemetry
