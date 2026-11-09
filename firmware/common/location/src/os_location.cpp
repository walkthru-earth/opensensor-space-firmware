#include "os_location.h"
#include "iso_alpha2.h"
#include <h3api.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <utility>

// cellsToMultiPolygon is internal in H3 4.5.0 (cellsToMultiPoly.h, "will be
// moved to h3api.h.in when it is ready"), so it is declared here. Where the
// H3 private headers are on the include path, the header comes first and the
// compiler rejects any mismatch with the declaration below.
// Its inline overflow check compares int64_t with uint64_t, valid C that C++
// -Wsign-compare flags, so that warning is off for the header alone.
#if __has_include("cellsToMultiPoly.h")
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
extern "C" {
#include "cellsToMultiPoly.h"
}
#pragma GCC diagnostic pop
#endif
extern "C" {
H3Error H3_EXPORT(cellsToMultiPolygon)(const H3Index *cells,
                                       const int64_t numCells,
                                       GeoMultiPolygon *out);
void H3_EXPORT(destroyGeoMultiPolygon)(GeoMultiPolygon *mpoly);
}

namespace opensensor::location {
bool valid_cell(Cell cell) { return isValidCell(cell) != 0; }

int resolution(Cell cell) {
  return valid_cell(cell) ? getResolution(cell) : -1;
}

bool parse_cell(const char *text, Cell &cell) {
  // Constrain input before calling H3's numeric parser (which accepts numeric
  // prefixes). Valid cells have 15 hex digits; reject
  // prefixes/signs/whitespace.
  if (!text || std::strlen(text) != kCellTextBytes - 1)
    return false;
  for (const char *p = text; *p; ++p)
    if (!(*p >= '0' && *p <= '9') && !(*p >= 'a' && *p <= 'f') &&
        !(*p >= 'A' && *p <= 'F'))
      return false;
  H3Index parsed = 0;
  if (stringToH3(text, &parsed) != E_SUCCESS || !valid_cell(parsed))
    return false;
  cell = parsed;
  return true;
}

bool format_cell(Cell cell, char *text, std::size_t size) {
  if (!text || size < kCellTextBytes || !valid_cell(cell))
    return false;
  // Upstream reserves 17 bytes for arbitrary uint64 indexes; a validated
  // cell needs only 15 digits. Keep the public canonical buffer size at 16.
  char canonical[17]{};
  if (h3ToString(cell, canonical, sizeof(canonical)) != E_SUCCESS)
    return false;
  std::memcpy(text, canonical, kCellTextBytes);
  return true;
}

bool coarsen(Cell input, int maximum_resolution, Cell &published) {
  if (maximum_resolution < 0 || maximum_resolution > 15 || !valid_cell(input))
    return false;
  H3Index parent = 0;
  if (cellToParent(input, std::min(maximum_resolution, getResolution(input)),
                   &parent) != E_SUCCESS ||
      !valid_cell(parent))
    return false;
  published = parent;
  return true;
}

std::expected<Cell, PublishError> published_cell(Cell input) {
  if (!valid_cell(input))
    return std::unexpected(PublishError::Invalid);
  if (getResolution(input) < kPublishedResolution)
    return std::unexpected(PublishError::TooCoarse);
  Cell parent = 0;
  if (!coarsen(input, kPublishedResolution, parent))
    return std::unexpected(PublishError::Invalid);
  return parent;
}

bool center_degrees(Cell cell, double &latitude, double &longitude) {
  if (!valid_cell(cell))
    return false;
  LatLng center{};
  if (cellToLatLng(cell, &center) != E_SUCCESS)
    return false;
  latitude = radsToDegs(center.lat);
  longitude = radsToDegs(center.lng);
  return true;
}

bool center_e7(Cell cell, std::int64_t &latitude, std::int64_t &longitude) {
  double lat, lon;
  if (!center_degrees(cell, lat, lon))
    return false;
  latitude = std::llround(lat * 10000000.0);
  longitude = std::llround(lon * 10000000.0);
  return true;
}

bool from_degrees(double latitude, double longitude, int res, Cell &cell) {
  if (!std::isfinite(latitude) || !std::isfinite(longitude) || latitude < -90 ||
      latitude > 90 || longitude < -180 || longitude > 180 || res < 0 ||
      res > 15)
    return false;
  const LatLng coordinates{degsToRads(latitude), degsToRads(longitude)};
  H3Index indexed = 0;
  if (latLngToCell(&coordinates, res, &indexed) != E_SUCCESS ||
      !valid_cell(indexed))
    return false;
  cell = indexed;
  return true;
}

bool valid_country(const char *country) {
  if (!country || country[0] < 'A' || country[0] > 'Z' || country[1] < 'A' ||
      country[1] > 'Z' || country[2] != '\0')
    return false;
  for (std::size_t i = 0; i + 1 < sizeof(kIsoAlpha2) - 1; i += 3)
    if (kIsoAlpha2[i] == country[0] && kIsoAlpha2[i + 1] == country[1])
      return true;
  return false;
}

const char *multipolygon_error_name(MultiPolygonError error) {
  switch (error) {
  case MultiPolygonError::Empty:
    return "empty";
  case MultiPolygonError::InvalidCell:
    return "invalid-cell";
  case MultiPolygonError::MixedResolution:
    return "mixed-resolution";
  case MultiPolygonError::DuplicateCell:
    return "duplicate-cell";
  case MultiPolygonError::TooManyCells:
    return "too-many-cells";
  case MultiPolygonError::Memory:
    return "memory";
  case MultiPolygonError::PointsFull:
    return "points-full";
  case MultiPolygonError::RingsFull:
    return "rings-full";
  case MultiPolygonError::PolygonsFull:
    return "polygons-full";
  case MultiPolygonError::Failed:
    return "failed";
  }
  return "unknown";
}

namespace {
// Frees what a successful cellsToMultiPolygon returned. H3 frees its own
// partial work before it returns an error, and leaves `polygons` NULL for
// zero cells, so this is only armed on success.
class OwnedMultiPolygon {
public:
  GeoMultiPolygon value{};
  bool armed = false;
  OwnedMultiPolygon() = default;
  OwnedMultiPolygon(const OwnedMultiPolygon &) = delete;
  OwnedMultiPolygon &operator=(const OwnedMultiPolygon &) = delete;
  ~OwnedMultiPolygon() {
    if (armed)
      H3_EXPORT(destroyGeoMultiPolygon)(&value);
  }
};

MultiPolygonError map_h3_error(H3Error error) {
  switch (error) {
  case E_CELL_INVALID:
    return MultiPolygonError::InvalidCell;
  case E_RES_MISMATCH:
    return MultiPolygonError::MixedResolution;
  case E_DUPLICATE_INPUT:
    return MultiPolygonError::DuplicateCell;
  case E_MEMORY_BOUNDS:
    return MultiPolygonError::TooManyCells;
  case E_MEMORY_ALLOC:
    return MultiPolygonError::Memory;
  default:
    return MultiPolygonError::Failed;
  }
}

// h3-js 4.5.0 converts with `rad * 180 / Math.PI`, left to right. H3's own
// radsToDegs multiplies by a rounded 180/pi constant, which can differ in the
// last bit, so it is not used here.
double degrees(double radians) { return radians * 180.0 / std::numbers::pi; }
} // namespace

std::expected<MultiPolygonView, MultiPolygonError>
cells_to_multipolygon(std::span<const Cell> cells, std::span<LngLat> points,
                      std::span<Ring> rings, std::span<Polygon> polygons) {
  if (cells.empty())
    return std::unexpected(MultiPolygonError::Empty);
  // H3 takes an int64_t count, and rings and polygons index points and rings
  // with 32 bits. Both conversions are checked rather than assumed.
  if (std::cmp_greater(cells.size(), INT64_MAX))
    return std::unexpected(MultiPolygonError::TooManyCells);
  if (std::cmp_greater(points.size(), UINT32_MAX) ||
      std::cmp_greater(rings.size(), UINT32_MAX))
    return std::unexpected(MultiPolygonError::Failed);
  OwnedMultiPolygon result;
  const H3Error error = H3_EXPORT(cellsToMultiPolygon)(
      cells.data(), static_cast<int64_t>(cells.size()), &result.value);
  if (error != E_SUCCESS)
    return std::unexpected(map_h3_error(error));
  result.armed = true;
  const GeoMultiPolygon &h3 = result.value;
  if (h3.numPolygons <= 0 || h3.polygons == nullptr)
    return std::unexpected(MultiPolygonError::Failed);

  std::size_t point_count = 0;
  std::size_t ring_count = 0;
  std::size_t polygon_count = 0;
  // Copies one loop and closes it. Ring and point bounds are checked before
  // any write, so a full buffer never takes a partial ring.
  const auto add_ring =
      [&](const GeoLoop &loop) -> std::expected<void, MultiPolygonError> {
    // geoMultiPolygonToLinkedGeoPolygon, which h3-js reads, fails the same way.
    if (loop.numVerts < 3 || loop.verts == nullptr)
      return std::unexpected(MultiPolygonError::Failed);
    const auto count = static_cast<std::size_t>(loop.numVerts) + 1;
    if (ring_count == rings.size())
      return std::unexpected(MultiPolygonError::RingsFull);
    if (count > points.size() - point_count)
      return std::unexpected(MultiPolygonError::PointsFull);
    rings[ring_count++] = {.first = static_cast<std::uint32_t>(point_count),
                           .count = static_cast<std::uint32_t>(count)};
    for (int i = 0; i < loop.numVerts; ++i)
      points[point_count++] = {.lng = degrees(loop.verts[i].lng),
                               .lat = degrees(loop.verts[i].lat)};
    points[point_count] =
        points[point_count - static_cast<std::size_t>(loop.numVerts)];
    ++point_count;
    return {};
  };
  for (int p = 0; p < h3.numPolygons; ++p) {
    const GeoPolygon &polygon = h3.polygons[p];
    if (polygon.numHoles < 0 || (polygon.numHoles > 0 && !polygon.holes))
      return std::unexpected(MultiPolygonError::Failed);
    if (polygon_count == polygons.size())
      return std::unexpected(MultiPolygonError::PolygonsFull);
    const auto first_ring = static_cast<std::uint32_t>(ring_count);
    if (const auto added = add_ring(polygon.geoloop); !added)
      return std::unexpected(added.error());
    for (int h = 0; h < polygon.numHoles; ++h)
      if (const auto added = add_ring(polygon.holes[h]); !added)
        return std::unexpected(added.error());
    polygons[polygon_count++] = {
        .first_ring = first_ring,
        .ring_count = static_cast<std::uint32_t>(ring_count) - first_ring};
  }
  return MultiPolygonView{.polygons = polygons.first(polygon_count),
                          .rings = rings.first(ring_count),
                          .points = points.first(point_count)};
}
} // namespace opensensor::location
