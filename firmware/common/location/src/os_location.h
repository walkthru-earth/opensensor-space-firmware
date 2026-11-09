#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

// Stateless H3 helpers. Persist only the result of published_cell(), and derive
// public coordinates from that cell. A cell center is not the original GPS
// position or an administrative country lookup.
namespace opensensor::location {
using Cell = std::uint64_t;
// Every published cell is H3 res 8 (owner decision OD-2026-11-04-B, #139).
// The config key location.resolution accepts only this value.
constexpr int kPublishedResolution = 8;
constexpr std::size_t kCellTextBytes = 16; // 15 lowercase hex digits + NUL

bool valid_cell(Cell cell);
int resolution(Cell cell); // -1 for an invalid cell, including absent cell 0
bool parse_cell(const char *text, Cell &cell);
bool format_cell(Cell cell, char *text, std::size_t size);
// Maximum disclosure resolution, inclusive 0..15. A coarser input is retained
// as-is; increasing this limit cannot reconstruct finer location information.
bool coarsen(Cell input, int maximum_resolution, Cell &published);
// Why a cell cannot be published.
enum class [[nodiscard]] PublishError : std::uint8_t {
  Invalid,   // not an H3 cell, including the absent cell 0
  TooCoarse, // coarser than res 8, the finer cell cannot be restored
};
// The res 8 cell to publish for `input`. A finer cell becomes its res 8
// parent, a res 8 cell is kept and a coarser one is refused, never upsampled.
[[nodiscard]] std::expected<Cell, PublishError> published_cell(Cell input);
bool center_degrees(Cell cell, double &latitude, double &longitude);
bool center_e7(Cell cell, std::int64_t &latitude, std::int64_t &longitude);
bool from_degrees(double latitude, double longitude, int resolution,
                  Cell &cell);
// Assigned ISO 3166-1 alpha-2 code only; country is explicitly owner-declared.
// Empty input means missing to callers, and is not itself a valid country code.
bool valid_country(const char *country);

// A set of cells as GeoJSON polygons, the union of the cells.
//
// This part depends only on H3 and the standard library, so it can move to a
// shared H3 library as is. It never allocates from the caller's view. H3
// itself allocates while it traces the outline and frees it all before the
// call returns, on every path.
//
// Coordinates are degrees, longitude first, converted as rad * 180 / pi in
// that order, which is how h3-js 4.5.0 converts. Each ring is closed, its
// last point equal to its first. Every polygon has its outer ring first, then
// its holes. Outer rings run counterclockwise and holes clockwise, and the
// polygons come largest outer ring first, all as H3 4.5.0 returns them.
struct LngLat {
  double lng;
  double lat;
};
// Points [first, first + count) of the point buffer, closing point included.
struct Ring {
  std::uint32_t first;
  std::uint32_t count;
};
// Rings [first_ring, first_ring + ring_count) of the ring buffer. The first
// one is the outer ring.
struct Polygon {
  std::uint32_t first_ring;
  std::uint32_t ring_count;
};
// Views into the caller's buffers, valid while those buffers are.
struct MultiPolygonView {
  std::span<const Polygon> polygons;
  std::span<const Ring> rings;
  std::span<const LngLat> points;
};

enum class [[nodiscard]] MultiPolygonError : std::uint8_t {
  Empty,           // no cells
  InvalidCell,     // a value is not an H3 cell, including 0 (E_CELL_INVALID)
  MixedResolution, // the cells differ in resolution (E_RES_MISMATCH)
  DuplicateCell,   // a cell is listed twice (E_DUPLICATE_INPUT)
  TooManyCells,    // H3's working memory size would overflow (E_MEMORY_BOUNDS)
  Memory,          // an H3 allocation failed (E_MEMORY_ALLOC)
  PointsFull,      // the point buffer is too small
  RingsFull,       // the ring buffer is too small
  PolygonsFull,    // the polygon buffer is too small
  Failed,          // any other H3 error, or a ring under 3 points
};
[[nodiscard]] const char *multipolygon_error_name(MultiPolygonError error);

// Buffer sizes that always hold the result for `cells` distinct cells of one
// resolution. Each cell has at most 6 edges and every outline edge adds one
// point, two at odd (Class III) resolutions where an edge can carry a
// distortion vertex (H3 directedEdge.c). Every ring has at least 3 edges and
// adds one closing point, and every polygon holds at least one cell.
struct MultiPolygonCapacity {
  std::size_t points;
  std::size_t rings;
  std::size_t polygons;
};
[[nodiscard]] constexpr MultiPolygonCapacity
multipolygon_capacity(std::size_t cells, int res) {
  const std::size_t edges = 6 * cells;
  const std::size_t per_edge = (res % 2 != 0) ? 2 : 1;
  const std::size_t rings = 2 * cells;
  return {
      .points = edges * per_edge + rings, .rings = rings, .polygons = cells};
}

// The outline of `cells`. Pass the cells sorted ascending and without
// duplicates for the output order stac-opensensor and h3-js agree on, since
// H3 starts each ring at an edge that depends on input order. Returns views
// into `points`, `rings` and `polygons`. On an error those buffers hold no
// usable result.
[[nodiscard]] std::expected<MultiPolygonView, MultiPolygonError>
cells_to_multipolygon(std::span<const Cell> cells, std::span<LngLat> points,
                      std::span<Ring> rings, std::span<Polygon> polygons);
} // namespace opensensor::location
