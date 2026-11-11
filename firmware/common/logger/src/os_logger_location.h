#pragma once

#include <device_config.h>
#include <os_location.h>
#include <parquet_writer.h>
#include <telemetry_logical.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace oslogger {
// Captured at acquisition, copied with the queue and RAM batch. Later settings
// changes cannot relabel buffered samples of an open file.
template <typename Row> struct LocationSample : Row {
  config::LocationSettings location{};
  // Footer thing.deployment_id, snapshotted with the location (#148).
  config::DeploymentId deployment{};
};

// The labels a file keeps for all its rows, compared between a new sample and
// the open file or RAM batch. A change in either closes the file. Owner title,
// description, provider and url are not labels and never split a file.
template <typename A, typename B>
[[nodiscard]] bool same_file_labels(const A &a, const B &b) {
  return config::same_location(a.location, b.location) &&
         config::same_deployment(a.deployment, b.deployment);
}

// Nullable fields, in order: cell ID, actual resolution, location_source and
// the GEOMETRY slot, a 2D point at the cell centre packed by
// telemetry::pack_point_e7. Source None or an invalid cell produces four
// nulls. The point is always calculated from the published cell, never from
// an incoming GPS position. The caller has already made the cell res 8, see
// opensensor::gnss::select_location.
template <typename Row>
bool apply_cell(Row &row, opensensor::location::Cell published,
                telemetry::LocationSource source,
                const std::array<std::size_t, 4> &fields) {
  for (auto index : fields)
    row.valid[index] = 0;
  if (source == telemetry::LocationSource::None)
    return false;
  const auto center = opensensor::location::center_e7(published);
  const auto resolution = opensensor::location::resolution(published);
  if (!center || !resolution)
    return false;
  row.counter(fields[0], static_cast<std::int64_t>(published));
  row.integer(fields[1], *resolution);
  row.integer(fields[2], std::to_underlying(source));
  // center_e7 keeps latitude within 90 and longitude within 180 degrees, so
  // both fit an int32.
  row.counter(fields[3], telemetry::pack_point_e7(
                             static_cast<std::int32_t>(center->longitude),
                             static_cast<std::int32_t>(center->latitude)));
  return true;
}
} // namespace oslogger
