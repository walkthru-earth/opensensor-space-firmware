// Host checks for cells_to_multipolygon as the firmware uses it, run by
// tools/test_h3_polygon.py against the pinned embedded-h3-location library
// compiled with H3_ALLOC_PREFIX=os_h3_, the device setting. The script links
// the real device hooks, firmware/common/location/src/os_h3_alloc.cpp, built
// against tools/host_fakes/h3_alloc. This file defines the heap_caps calls
// those hooks make over a counting heap with the ESP-IDF 6.1 semantics, so a
// zero size returns NULL. The heap counts every H3 allocation, can fail any
// one of them, and proves nothing is left after each call. Host numbers are
// 64-bit. They bound, not measure, the device heap.
#include "os_location.h"

#include <esp_heap_caps.h>
#include <h3api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <string_view>
#include <vector>

namespace {
using opensensor::location::Cell;
using opensensor::location::cells_to_multipolygon;
using opensensor::location::Error;
using opensensor::location::LngLat;
using opensensor::location::MultiPolygon;
using opensensor::location::multipolygon_capacity;
using opensensor::location::Polygon;
using opensensor::location::Ring;

// Not an Error value (they start at 1), for a call that did not fail.
constexpr Error kNoError{};

int failures = 0;

void check(bool ok, const char *what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", what);
    ++failures;
  }
}

// ---- Counting heap under the device hooks ----

// The default CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL. Plain malloc of this size
// or less prefers internal RAM on a PSRAM board (heap_caps.c).
constexpr std::size_t kAlwaysInternal = 16384;

struct Heap {
  std::size_t live_bytes = 0;
  std::size_t peak_bytes = 0;
  std::size_t live_blocks = 0;
  std::size_t allocations = 0;
  std::size_t largest = 0;
  std::size_t small_live = 0; // live bytes in blocks <= kAlwaysInternal
  std::size_t small_peak = 0;
  std::size_t small_allocations = 0;
  std::size_t fail_at = SIZE_MAX; // allocation index to fail
  std::size_t zero_requests = 0;  // heap_caps calls that asked for 0 bytes
  std::size_t one_byte = 0;       // blocks of 1 byte, a rounded up 0
  std::size_t wrong_caps = 0;     // calls without exactly SPIRAM | 8BIT
};
Heap heap;

void reset_heap(std::size_t fail_at = SIZE_MAX) {
  heap = {};
  heap.fail_at = fail_at;
}

constexpr std::size_t kHeader = alignof(std::max_align_t);

// heap_caps_malloc(0) returns NULL on ESP-IDF 6.1 (heap_caps_base.c), so a
// zero size fails here too.
void *take(std::size_t size, bool zero) {
  if (size == 0)
    ++heap.zero_requests;
  if (heap.allocations++ == heap.fail_at || size == 0)
    return nullptr;
  if (size == 1)
    ++heap.one_byte;
  auto *block = static_cast<unsigned char *>(std::malloc(kHeader + size));
  if (!block)
    return nullptr;
  std::memcpy(block, &size, sizeof size);
  if (zero)
    std::memset(block + kHeader, 0, size);
  heap.live_bytes += size;
  heap.peak_bytes = std::max(heap.peak_bytes, heap.live_bytes);
  ++heap.live_blocks;
  heap.largest = std::max(heap.largest, size);
  if (size <= kAlwaysInternal) {
    heap.small_live += size;
    heap.small_peak = std::max(heap.small_peak, heap.small_live);
    ++heap.small_allocations;
  }
  return block + kHeader;
}

std::size_t size_of(void *pointer) {
  std::size_t size = 0;
  std::memcpy(&size, static_cast<unsigned char *>(pointer) - kHeader,
              sizeof size);
  return size;
}

void give(void *pointer) {
  if (!pointer)
    return;
  const std::size_t size = size_of(pointer);
  heap.live_bytes -= size;
  --heap.live_blocks;
  if (size <= kAlwaysInternal)
    heap.small_live -= size;
  std::free(static_cast<unsigned char *>(pointer) - kHeader);
}
} // namespace

namespace {
constexpr std::uint32_t kPsram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

void note_caps(std::uint32_t caps) {
  if (caps != kPsram)
    ++heap.wrong_caps;
}
} // namespace

// The heap_caps calls os_h3_alloc.cpp makes, ESP-IDF 6.1 semantics at
// fff9895c (components/heap/heap_caps_base.c), see esp_heap_caps.h here.
extern "C" {
void *heap_caps_malloc(std::size_t size, std::uint32_t caps) {
  note_caps(caps);
  return take(size, false);
}
void *heap_caps_calloc(std::size_t n, std::size_t size, std::uint32_t caps) {
  note_caps(caps);
  std::size_t bytes = 0;
  if (__builtin_mul_overflow(n, size, &bytes))
    return nullptr;
  return take(bytes, true);
}
void *heap_caps_realloc(void *pointer, std::size_t size, std::uint32_t caps) {
  note_caps(caps);
  if (!pointer)
    return take(size, false);
  if (size == 0) {
    ++heap.zero_requests;
    give(pointer);
    return nullptr;
  }
  void *moved = take(size, false);
  if (!moved)
    return nullptr; // the old block stays, as heap_caps_realloc leaves it
  std::memcpy(moved, pointer, std::min(size, size_of(pointer)));
  give(pointer);
  return moved;
}
void heap_caps_free(void *pointer) { give(pointer); }

// Defined in firmware/common/location/src/os_h3_alloc.cpp.
void *os_h3_malloc(std::size_t size);
void *os_h3_calloc(std::size_t count, std::size_t size);
void *os_h3_realloc(void *pointer, std::size_t size);
void os_h3_free(void *pointer);
}

namespace {

// ---- Helpers ----

Cell cell_at(double lat, double lng, int res) {
  const LatLng point{.lat = lat * std::numbers::pi / 180.0,
                     .lng = lng * std::numbers::pi / 180.0};
  H3Index cell = 0;
  check(latLngToCell(&point, res, &cell) == E_SUCCESS, "latLngToCell");
  return cell;
}

std::vector<Cell> disk(Cell origin, int k) {
  std::int64_t size = 0;
  check(maxGridDiskSize(k, &size) == E_SUCCESS, "maxGridDiskSize");
  std::vector<Cell> out(static_cast<std::size_t>(size));
  check(gridDisk(origin, k, out.data()) == E_SUCCESS, "gridDisk");
  std::erase(out, Cell{0});
  std::ranges::sort(out);
  return out;
}

struct Buffers {
  std::vector<LngLat> points;
  std::vector<Ring> rings;
  std::vector<Polygon> polygons;
  explicit Buffers(std::size_t cells, int res) {
    const auto capacity = multipolygon_capacity(cells, res);
    check(capacity.has_value(), "capacity");
    if (!capacity)
      return;
    points.resize(capacity->points);
    rings.resize(capacity->rings);
    polygons.resize(capacity->polygons);
  }
};

auto run(const std::vector<Cell> &cells, Buffers &buffers) {
  return cells_to_multipolygon(cells, buffers.points, buffers.rings,
                               buffers.polygons);
}

// Twice the signed area in the lng/lat plane, positive counterclockwise.
double signed_area(const MultiPolygon &view, const Ring &ring) {
  double sum = 0;
  for (std::uint32_t i = 0; i + 1 < ring.count; ++i) {
    const auto &a = view.points[ring.first + i];
    const auto &b = view.points[ring.first + i + 1];
    sum += a.longitude * b.latitude - b.longitude * a.latitude;
  }
  return sum;
}

bool closed(const MultiPolygon &view) {
  for (const auto &ring : view.rings) {
    if (ring.count < 4)
      return false;
    const auto &first = view.points[ring.first];
    const auto &last = view.points[ring.first + ring.count - 1];
    if (first.longitude != last.longitude || first.latitude != last.latitude)
      return false;
  }
  return true;
}

bool covers_buffers(const MultiPolygon &view) {
  // Rings follow one another and polygons cover every ring, in order.
  std::uint32_t point = 0;
  for (const auto &ring : view.rings) {
    if (ring.first != point)
      return false;
    point += ring.count;
  }
  std::uint32_t ring = 0;
  for (const auto &polygon : view.polygons) {
    if (polygon.first_ring != ring || polygon.ring_count == 0)
      return false;
    ring += polygon.ring_count;
  }
  return point == view.points.size() && ring == view.rings.size();
}

double degrees(double radians) { return radians * 180.0 / std::numbers::pi; }

// The output must equal, bit for bit, what cellsToLinkedMultiPolygon gives,
// which is the call h3-js 4.5.0 makes for cellsToMultiPolygon(cells, true).
// h3-js then closes each ring with its first point.
bool equals_linked(const std::vector<Cell> &cells, const MultiPolygon &view) {
  LinkedGeoPolygon linked{};
  if (cellsToLinkedMultiPolygon(cells.data(), static_cast<int>(cells.size()),
                                &linked) != E_SUCCESS)
    return false;
  bool same = true;
  std::size_t p = 0;
  std::size_t r = 0;
  std::size_t at = 0;
  for (const LinkedGeoPolygon *polygon = &linked; polygon && same;
       polygon = polygon->next, ++p) {
    if (p >= view.polygons.size()) {
      same = false;
      break;
    }
    std::size_t rings_here = 0;
    for (const LinkedGeoLoop *loop = polygon->first; loop && same;
         loop = loop->next, ++r, ++rings_here) {
      if (r >= view.rings.size()) {
        same = false;
        break;
      }
      at = view.rings[r].first;
      std::size_t count = 0;
      const LinkedLatLng *first = loop->first;
      for (const LinkedLatLng *v = first; v; v = v->next, ++count, ++at)
        if (count >= view.rings[r].count ||
            view.points[at].longitude != degrees(v->vertex.lng) ||
            view.points[at].latitude != degrees(v->vertex.lat))
          same = false;
      if (!first || count + 1 != view.rings[r].count ||
          view.points[at].longitude != degrees(first->vertex.lng) ||
          view.points[at].latitude != degrees(first->vertex.lat))
        same = false;
    }
    if (rings_here != view.polygons[p].ring_count)
      same = false;
  }
  if (p != view.polygons.size() || r != view.rings.size())
    same = false;
  destroyLinkedMultiPolygon(&linked);
  return same;
}

std::uint64_t rng_state = 0x9e3779b97f4a7c15ULL;
std::uint64_t next_random() {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}
double uniform(double low, double high) {
  return low + (high - low) * static_cast<double>(next_random() >> 11) /
                   static_cast<double>(1ULL << 53);
}

// ---- Cases ----

void single_cell() {
  const Cell cell = cell_at(30.0444, 31.2357, 8);
  const std::vector<Cell> cells{cell};
  Buffers buffers(1, 8);
  reset_heap();
  const auto view = run(cells, buffers);
  check(heap.live_blocks == 0 && heap.live_bytes == 0, "single frees all");
  check(view.has_value(), "single succeeds");
  if (!view)
    return;
  check(view->polygons.size() == 1 && view->rings.size() == 1 &&
            view->points.size() == 7,
        "single is one hexagon, closed");
  check(closed(*view) && covers_buffers(*view), "single closed and packed");
  check(signed_area(*view, view->rings[0]) > 0, "outer ring counterclockwise");
  // Same corners as cellToBoundary, in some rotation.
  CellBoundary boundary{};
  check(cellToBoundary(cell, &boundary) == E_SUCCESS && boundary.numVerts == 6,
        "boundary");
  for (int i = 0; i < boundary.numVerts; ++i) {
    bool found = false;
    for (std::size_t j = 0; j < 6; ++j)
      found = found || (std::fabs(view->points[j].longitude -
                                  degrees(boundary.verts[i].lng)) < 1e-12 &&
                        std::fabs(view->points[j].latitude -
                                  degrees(boundary.verts[i].lat)) < 1e-12);
    check(found, "single corner matches cellToBoundary");
  }
  check(equals_linked(cells, *view), "single equals linked");
}

void shapes() {
  const Cell origin = cell_at(29.9688, 31.1342, 8);
  // Two neighbors make one ring of 10 corners.
  {
    auto cells = disk(origin, 1);
    std::vector<Cell> pair{origin, cells[0] == origin ? cells[1] : cells[0]};
    std::ranges::sort(pair);
    Buffers buffers(2, 8);
    const auto view = run(pair, buffers);
    check(view && view->polygons.size() == 1 && view->rings.size() == 1 &&
              view->points.size() == 11,
          "two neighbors are one ring");
    check(view && equals_linked(pair, *view), "two neighbors equal linked");
  }
  // Far apart cells are separate polygons.
  {
    std::vector<Cell> apart{origin, cell_at(30.0444, 31.2357, 8),
                            cell_at(31.2001, 29.9187, 8)};
    std::ranges::sort(apart);
    Buffers buffers(apart.size(), 8);
    const auto view = run(apart, buffers);
    check(view && view->polygons.size() == 3 && view->rings.size() == 3 &&
              closed(*view) && covers_buffers(*view),
          "three apart are three polygons");
    check(view && equals_linked(apart, *view), "apart equal linked");
  }
  // The six around a cell enclose it, so one polygon with a hole.
  {
    auto ring = disk(origin, 1);
    std::erase(ring, origin);
    Buffers buffers(ring.size(), 8);
    const auto view = run(ring, buffers);
    check(view && view->polygons.size() == 1 && view->rings.size() == 2 &&
              view->polygons[0].ring_count == 2,
          "ring of six has a hole");
    if (view) {
      check(signed_area(*view, view->rings[0]) > 0, "outer counterclockwise");
      check(signed_area(*view, view->rings[1]) < 0, "hole clockwise");
      check(view->rings[1].count == 7, "hole is the center hexagon");
      check(closed(*view) && covers_buffers(*view), "hole closed and packed");
      check(equals_linked(ring, *view), "ring of six equals linked");
    }
  }
  // A pentagon has five sides.
  {
    std::array<H3Index, 12> pentagons{};
    check(getPentagons(8, pentagons.data()) == E_SUCCESS, "getPentagons");
    const std::vector<Cell> one{pentagons[0]};
    Buffers buffers(1, 8);
    const auto view = run(one, buffers);
    check(view && view->rings.size() == 1 && view->points.size() == 6,
          "pentagon has five corners");
    check(view && equals_linked(one, *view), "pentagon equals linked");
  }
}

void errors() {
  const Cell a = cell_at(30.0444, 31.2357, 8);
  const Cell b = cell_at(30.1, 31.3, 8);
  Buffers buffers(4, 8);
  const auto error_of = [&](std::vector<Cell> cells) {
    reset_heap();
    const auto view = run(cells, buffers);
    check(heap.live_blocks == 0, "error path frees all");
    return view ? kNoError : view.error();
  };
  check(error_of({}) == Error::NoCells, "empty");
  check(error_of({0}) == Error::InvalidCell, "zero is invalid");
  // The reserved high bit set makes any index invalid (H3 isValidCell).
  check(error_of({a, b | (1ULL << 63)}) == Error::InvalidCell,
        "high bit is invalid");
  check(error_of({a, cell_at(30.0444, 31.2357, 9)}) == Error::MixedResolution,
        "mixed resolution");
  check(error_of({a, b, a}) == Error::DuplicateCell,
        "duplicate (H3 E_DUPLICATE_INPUT)");
  check(std::string_view(opensensor::location::error_name(
            Error::DuplicateCell)) == "duplicate_cell",
        "error name");
}

void full_buffers() {
  const Cell origin = cell_at(29.9688, 31.1342, 8);
  auto ring = disk(origin, 1);
  std::erase(ring, origin);
  std::vector<Cell> cells = ring;
  cells.push_back(cell_at(30.0444, 31.2357, 8));
  std::ranges::sort(cells);
  Buffers exact(cells.size(), 8);
  const auto view = run(cells, exact);
  check(view.has_value(), "full buffers baseline");
  if (!view)
    return;
  const std::size_t points = view->points.size();
  const std::size_t rings = view->rings.size();
  const std::size_t polygons = view->polygons.size();
  std::vector<LngLat> p(points);
  std::vector<Ring> r(rings);
  std::vector<Polygon> g(polygons);
  check(cells_to_multipolygon(cells, p, r, g).has_value(), "exact size fits");
  const auto error_with = [&](std::size_t pn, std::size_t rn, std::size_t gn) {
    std::vector<LngLat> sp(pn);
    std::vector<Ring> sr(rn);
    std::vector<Polygon> sg(gn);
    reset_heap();
    const auto result = cells_to_multipolygon(cells, sp, sr, sg);
    check(heap.live_blocks == 0, "full buffer frees all");
    return result ? kNoError : result.error();
  };
  check(error_with(points - 1, rings, polygons) == Error::PointBufferFull,
        "one point short");
  check(error_with(points, rings - 1, polygons) == Error::RingBufferFull,
        "one ring short");
  check(error_with(points, rings, polygons - 1) == Error::PolygonBufferFull,
        "one polygon short");
  check(error_with(0, 0, 0) == Error::PolygonBufferFull, "no buffers at all");
}

void out_of_memory() {
  const Cell origin = cell_at(29.9688, 31.1342, 8);
  auto cells = disk(origin, 2);
  std::erase(cells, origin); // a hole
  cells.push_back(cell_at(30.0444, 31.2357, 8));
  std::ranges::sort(cells);
  Buffers buffers(cells.size(), 8);
  reset_heap();
  check(run(cells, buffers).has_value(), "oom baseline");
  const std::size_t total = heap.allocations;
  check(total > 4, "oom baseline allocates");
  std::size_t memory_errors = 0;
  for (std::size_t index = 0; index < total; ++index) {
    reset_heap(index);
    const auto view = run(cells, buffers);
    check(heap.live_blocks == 0 && heap.live_bytes == 0,
          "every failed allocation frees all");
    if (!view && view.error() == Error::OutOfMemory)
      ++memory_errors;
    else
      check(false, "a failed allocation reports Memory");
  }
  check(memory_errors == total, "oom every index");
  std::printf("h3 polygon: %zu allocations, each one failed in turn, no leak\n",
              total);
}

void random_sets() {
  std::size_t sets = 0;
  for (int round = 0; round < 300; ++round) {
    const int res = 5 + static_cast<int>(next_random() % 6); // 5..10
    const double lat = uniform(-89.9, 89.9);
    const double lng = uniform(-180.0, 180.0);
    const Cell origin = cell_at(lat, lng, res);
    auto cells = disk(origin, 1 + static_cast<int>(next_random() % 4));
    // Drop a random share to make holes and islands.
    std::erase_if(cells, [](Cell) { return next_random() % 3 == 0; });
    if (cells.empty())
      continue;
    Buffers buffers(cells.size(), res);
    reset_heap();
    const auto view = run(cells, buffers);
    check(heap.live_blocks == 0, "random frees all");
    check(view.has_value(), "random succeeds within capacity");
    if (!view)
      continue;
    check(closed(*view) && covers_buffers(*view), "random closed and packed");
    check(equals_linked(cells, *view), "random equals linked");
    ++sets;
  }
  // Pentagons at odd resolutions carry distortion corners, the worst case for
  // the capacity bound.
  for (const int res : {1, 3, 5, 7, 9}) {
    std::array<H3Index, 12> pentagons{};
    check(getPentagons(res, pentagons.data()) == E_SUCCESS, "pentagons");
    for (const H3Index pentagon : pentagons) {
      auto cells = disk(pentagon, 2);
      Buffers buffers(cells.size(), res);
      const auto view = run(cells, buffers);
      check(view && equals_linked(cells, *view),
            "pentagon disk fits capacity and equals linked");
      ++sets;
    }
  }
  // Near both poles.
  for (const double lat : {89.99, -89.99}) {
    auto cells = disk(cell_at(lat, 0, 8), 3);
    Buffers buffers(cells.size(), 8);
    const auto view = run(cells, buffers);
    check(view && equals_linked(cells, *view), "pole disk equals linked");
    ++sets;
  }
  std::printf("h3 polygon: %zu random, pentagon and pole sets equal "
              "cellsToLinkedMultiPolygon bit for bit\n",
              sets);
}

// Each device hook on its own, with the sizes H3 v4.5.0 never passes too.
// Every size goes through nonzero_alloc_size, so a zero request gets a 1 byte
// block where the ESP-IDF heap alone returns NULL, and every block is PSRAM.
void hooks() {
  reset_heap();
  void *malloc0 = os_h3_malloc(0);
  check(malloc0 && size_of(malloc0) == 1, "malloc(0) gets 1 byte");
  os_h3_free(malloc0);
  void *calloc_count0 = os_h3_calloc(0, 8);
  check(calloc_count0 && size_of(calloc_count0) == 8,
        "calloc(0, 8) gets 8 bytes");
  os_h3_free(calloc_count0);
  void *calloc_size0 = os_h3_calloc(8, 0);
  check(calloc_size0 && size_of(calloc_size0) == 8,
        "calloc(8, 0) gets 8 bytes");
  os_h3_free(calloc_size0);
  check(!os_h3_calloc(SIZE_MAX, 2), "calloc overflow returns NULL");
  void *realloc_null0 = os_h3_realloc(nullptr, 0);
  check(realloc_null0 && size_of(realloc_null0) == 1,
        "realloc(NULL, 0) gets 1 byte");
  // H3 calls realloc only to shrink a loop of 3 or more vertices
  // (cellsToMultiPoly.c line 367). A size 0 request still gets a 1 byte
  // block that free releases, the library guidance in location.h.
  void *grown = os_h3_realloc(realloc_null0, 64);
  check(grown && size_of(grown) == 64, "realloc grows");
  static_cast<unsigned char *>(grown)[0] = 0x5a;
  void *shrunk = os_h3_realloc(grown, 0);
  check(shrunk && size_of(shrunk) == 1 &&
            static_cast<unsigned char *>(shrunk)[0] == 0x5a,
        "realloc(p, 0) keeps a 1 byte block");
  os_h3_free(shrunk);
  os_h3_free(nullptr);
  check(heap.live_blocks == 0 && heap.live_bytes == 0, "hooks free all");
  check(heap.zero_requests == 0, "hooks never pass 0 to the heap");
  check(heap.wrong_caps == 0, "hooks ask for SPIRAM | 8BIT");
  reset_heap();
}

// The whole globe asks H3 for a zero byte block, the SortableLoop array for
// no loops (cellsToMultiPoly.c line 388 in H3 v4.5.0). The device malloc hook
// rounds it up, without that the ESP-IDF heap returns NULL and the call fails
// with OutOfMemory. hooks() covers the calloc and realloc arguments.
void globe() {
  std::vector<Cell> cells(static_cast<std::size_t>(res0CellCount()));
  check(cells.size() == 122, "122 res 0 cells");
  check(getRes0Cells(cells.data()) == E_SUCCESS, "getRes0Cells");
  std::ranges::sort(cells);
  Buffers buffers(cells.size(), 0);
  reset_heap();
  const auto view = run(cells, buffers);
  // Before equals_linked, whose H3 reference call also allocates.
  const std::size_t total = heap.allocations;
  check(heap.live_blocks == 0, "globe frees all");
  check(heap.one_byte > 0 && heap.zero_requests == 0,
        "globe zero requests reach the heap as 1 byte");
  check(heap.wrong_caps == 0, "globe allocates PSRAM only");
  check(view && closed(*view) && equals_linked(cells, *view),
        "globe succeeds through the device hooks and equals linked");
  // Every failed allocation on the globe path leaves nothing behind.
  for (std::size_t index = 0; index < total; ++index) {
    reset_heap(index);
    const auto failed = run(cells, buffers);
    check(!failed && failed.error() == Error::OutOfMemory &&
              heap.live_blocks == 0,
          "globe out of memory frees all");
  }
  reset_heap();
}

void worst_case(const char *name, const std::vector<Cell> &cells) {
  Buffers buffers(cells.size(), 8);
  reset_heap();
  const auto view = run(cells, buffers);
  check(view.has_value(), "worst case succeeds");
  check(heap.live_blocks == 0, "worst case frees all");
  const auto capacity =
      multipolygon_capacity(cells.size(), 8)
          .value_or(opensensor::location::MultiPolygonCapacity{});
  std::printf(
      "h3 polygon heap %s: %zu cells, %zu polygons %zu rings %zu points, "
      "peak %zu B in %zu allocations, largest %zu B, %zu allocations <= %zu B "
      "with peak %zu B, caller buffers %zu B\n",
      name, cells.size(), view ? view->polygons.size() : 0,
      view ? view->rings.size() : 0, view ? view->points.size() : 0,
      heap.peak_bytes, heap.allocations, heap.largest, heap.small_allocations,
      kAlwaysInternal, heap.small_peak,
      capacity.points * sizeof(LngLat) + capacity.rings * sizeof(Ring) +
          capacity.polygons * sizeof(Polygon));
}

void worst_cases() {
  // A file holds at most kMaxRowGroups x kMaxBatchRows = 8 x 90 = 720 rows
  // (parquet_writer.h, os_logger_cadence.h), so at most 720 distinct cells.
  constexpr std::size_t kCells = 720;
  // Every cell its own polygon, the most loops and allocations.
  std::vector<Cell> apart;
  for (std::size_t i = 0; apart.size() < kCells; ++i)
    apart.push_back(cell_at(20.0 + 0.05 * static_cast<double>(i / 30),
                            25.0 + 0.05 * static_cast<double>(i % 30), 8));
  std::ranges::sort(apart);
  check(std::ranges::adjacent_find(apart) == apart.end(), "apart distinct");
  worst_case("720 apart", apart);
  // One solid blob.
  auto blob = disk(cell_at(30.0444, 31.2357, 8), 15); // 721 cells
  blob.resize(kCells);
  worst_case("720 blob", blob);
  // A checkerboard of every other cell along lines, many holes.
  auto big = disk(cell_at(30.0444, 31.2357, 8), 22);
  std::vector<Cell> sparse;
  for (std::size_t i = 0; i < big.size() && sparse.size() < kCells; i += 2)
    sparse.push_back(big[i]);
  worst_case("720 sparse", sparse);
}
} // namespace

int main() {
  single_cell();
  shapes();
  errors();
  full_buffers();
  out_of_memory();
  random_sets();
  hooks();
  globe();
  worst_cases();
  if (failures) {
    std::fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  std::printf("h3 polygon: device hooks, shapes, winding, closure, errors, "
              "buffer bounds, out of memory, globe and parity passed\n");
  return 0;
}
