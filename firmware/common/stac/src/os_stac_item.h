#pragma once

#include "column_semantics.h"
#include "json_writer.h"
#include "os_location.h"
#include "telemetry_logical.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

// The STAC File Item a board writes next to each finalized dated Parquet file
// (#142). It follows the canonical form of the stac-opensensor profile at
// commit 83787768 (PROFILE.md "Canonical File Item" and scripts/canonical.mjs
// buildItem). The host gate stac-item-test proves the output equal to that
// script's examples under its `differences` rules.
//
// Board neutral. A board passes its statistics table, built at compile time
// from its FIELD and SEM tables by stat_columns(), and the logger feeds every
// row of the file. Nothing here touches storage. The logger writes the JSON
// through a json::Sink.
namespace opensensor::stac {

// Item geometry is the union of resolution 8 cells (PROFILE "Geometry").
inline constexpr int kResolution = 8;
static_assert(kResolution == location::kPublishedResolution,
              "the board publishes the resolution the profile requires");
// Rows of one file, 8 row groups of at most 90 rows. The logger checks its
// own limit against this one at compile time.
inline constexpr std::size_t kMaxRows = 720;
inline constexpr std::size_t kMaxColumns = telemetry::kMaxColumns;
// Geometry buffers that always hold the outline of kMaxRows distinct cells.
inline constexpr location::MultiPolygonCapacity kGeometryCapacity =
    location::multipolygon_capacity(kMaxRows, kResolution);

// canonical.mjs EXT, ANON_SIZE and WARNING.
inline constexpr std::string_view kStacVersion = "1.1.0";
inline constexpr std::string_view kTableExtension =
    "https://stac-extensions.github.io/table/v1.3.0/schema.json";
inline constexpr std::string_view kFileExtension =
    "https://stac-extensions.github.io/file/v2.1.0/schema.json";
inline constexpr std::string_view kGridExtension =
    "https://stac-extensions.github.io/grid/v1.1.0/schema.json";
inline constexpr std::string_view kAnonExtension =
    "https://stac-extensions.github.io/anonymized-location/v1.0.0/schema.json";
inline constexpr std::string_view kOpensensorExtension =
    "https://walkthru-earth.github.io/stac-opensensor/v0.1.0/schema.json";
inline constexpr std::string_view kProfileSchema =
    "https://walkthru-earth.github.io/stac-opensensor/v0.1.0/profile.json";
inline constexpr double kAnonSize = 0.01;
inline constexpr std::string_view kFixedWarning =
    "The geometry is the H3 cell chosen by the owner, not the device position.";
inline constexpr std::string_view kMovingWarning =
    "The geometry is the H3 cells the device visited, not its track.";

// How a row slot holds a value. NumericSample::set copies the value bytes
// to the start of the int64 slot.
enum class ValueKind : std::uint8_t { Int32, Int64, Float };
// opensensor:valid_when. Finite reads the gate column, which is the column
// itself in every template the generator writes.
enum class GateKind : std::uint8_t { None, Finite, Equals };

// One numeric measurement column of the board template, the columns
// canonical.mjs columnStatistics reports, in template order.
struct StatColumn {
  std::string_view name;
  std::string_view type; // STAC table type, "int32", "uint8", "float"
  std::uint16_t field = 0;
  ValueKind kind = ValueKind::Int32;
  GateKind gate = GateKind::None;
  std::uint16_t gate_field = 0;
  ValueKind gate_kind = ValueKind::Int32;
  std::int64_t equals = 0;
};

// The STAC table type of a field, as tools/board_template.h names it, or ""
// when canonical.mjs NUMERIC does not hold it.
constexpr std::string_view numeric_type(telemetry::PhysicalType type,
                                        const char *unit) {
  using telemetry::PhysicalType;
  if (telemetry::logical_for(type, unit) == telemetry::LogicalType::Uint8)
    return "uint8";
  switch (type) {
  case PhysicalType::Int32:
    return "int32";
  case PhysicalType::Int64:
    return "int64";
  case PhysicalType::Float:
    return "float";
  case PhysicalType::ByteArray:
    break;
  }
  return "";
}

constexpr ValueKind value_kind(telemetry::PhysicalType type) {
  return type == telemetry::PhysicalType::Int64   ? ValueKind::Int64
         : type == telemetry::PhysicalType::Float ? ValueKind::Float
                                                  : ValueKind::Int32;
}

// canonical.mjs MEASUREMENT, the first role of the column.
constexpr bool measurement(semantics::Role role) {
  return role == semantics::Role::physicalMeasurement ||
         role == semantics::Role::modelResult;
}

template <typename Fields>
constexpr std::size_t field_position(const Fields &fields,
                                     std::string_view name) {
  std::size_t i = 0;
  for (const auto &field : fields) {
    if (field.name != nullptr && std::string_view{field.name} == name)
      return i;
    ++i;
  }
  return i;
}

// Signed decimal integer text, false on any other text or on int64 overflow.
constexpr bool parse_integer(std::string_view text, std::int64_t &value) {
  const bool negative = !text.empty() && text.front() == '-';
  if (negative)
    text.remove_prefix(1);
  if (text.empty())
    return false;
  std::uint64_t magnitude = 0;
  constexpr std::uint64_t kLimit = 9223372036854775808ULL;
  for (const char c : text) {
    if (c < '0' || c > '9')
      return false;
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (magnitude > (kLimit - digit) / 10)
      return false;
    magnitude = magnitude * 10 + digit;
  }
  if (!negative && magnitude == kLimit)
    return false;
  value = negative ? static_cast<std::int64_t>(0 - magnitude)
                   : static_cast<std::int64_t>(magnitude);
  return true;
}

template <typename Fields>
constexpr bool reported(const Fields &fields, const semantics::Column &row,
                        std::size_t i) {
  return measurement(row.role) &&
         !numeric_type(fields[i].type, fields[i].unit).empty();
}

// Number of columns stat_columns() returns.
template <typename Fields>
constexpr std::size_t stat_count(const Fields &fields,
                                 std::span<const semantics::Column> rows) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < rows.size(); ++i)
    if (reported(fields, rows[i], i))
      ++count;
  return count;
}

// The statistics table of a board, in field order, which is template order.
// Run at compile time and checked by stat_columns_valid().
template <std::size_t N, typename Fields>
constexpr std::array<StatColumn, N>
stat_columns(const Fields &fields, std::span<const semantics::Column> rows) {
  std::array<StatColumn, N> out{};
  std::size_t used = 0;
  for (std::size_t i = 0; i < rows.size() && used < N; ++i) {
    if (!reported(fields, rows[i], i))
      continue;
    StatColumn column{
        .name = rows[i].name,
        .type = numeric_type(fields[i].type, fields[i].unit),
        .field = static_cast<std::uint16_t>(i),
        .kind = value_kind(fields[i].type),
        .gate = GateKind::None,
        .gate_field = static_cast<std::uint16_t>(i),
        .gate_kind = value_kind(fields[i].type),
        .equals = 0,
    };
    const auto gate = rows[i].valid_when;
    constexpr std::string_view kEquals = " == ";
    if (gate == "finite") {
      column.gate = GateKind::Finite;
    } else if (const auto at = gate.find(kEquals);
               !gate.empty() && at != std::string_view::npos) {
      const auto target = field_position(fields, gate.substr(0, at));
      std::int64_t value = 0;
      if (target < rows.size() &&
          parse_integer(gate.substr(at + kEquals.size()), value)) {
        column.gate = GateKind::Equals;
        column.gate_field = static_cast<std::uint16_t>(target);
        column.gate_kind = value_kind(fields[target].type);
        column.equals = value;
      }
    }
    out[used++] = column;
  }
  return out;
}

// True when every gate of the table parsed, every field index fits and an
// equality gate reads an integer column. Use in a static_assert.
template <std::size_t N, typename Fields>
constexpr bool stat_columns_valid(const std::array<StatColumn, N> &columns,
                                  const Fields &fields,
                                  std::span<const semantics::Column> rows) {
  if (N != stat_count(fields, rows) || N > kMaxColumns ||
      rows.size() > kMaxColumns)
    return false;
  return std::ranges::none_of(columns, [rows](const StatColumn &column) {
    const auto gate = rows[column.field].valid_when;
    return (column.gate == GateKind::None && !gate.empty()) ||
           (column.gate == GateKind::Equals &&
            column.gate_kind == ValueKind::Float);
  });
}

// Why a file gets no Item or no sidecar. Error only, no success value.
enum class [[nodiscard]] ItemError : std::uint8_t {
  // The profile's no Item cases (canonical.mjs buildItem), in its order.
  NotDated,       // the name is not data_HHMM_<boot>_<first>-<last>-<n>
  NoThing,        // no Thing id
  NoRows,         // the file has no rows
  NullTime,       // a row has no UTC time
  NotLocated,     // no row has a cell, a null or zero cell is unlocated
  NotResolution8, // a cell is invalid or not resolution 8
  FixedManyCells, // a fixed Thing file holds more than one cell
  // Board side failures.
  TimeRange,      // a UTC time is outside 1970 to 2099, see rfc3339()
  TooManyRows,    // more than kMaxRows rows
  BadLayout,      // a field index or row span does not fit the table
  GeometryMemory, // H3 could not allocate
  Geometry,       // any other outline failure
  JsonOverflow,   // the document passed the byte limit
  JsonNotFinite,  // a number is NaN or infinite
  JsonStructure,  // a builder bug, unbalanced JSON
  JsonSink,       // the sink refused bytes, a write failed
};
[[nodiscard]] const char *item_error_name(ItemError error);
// True for the cases where the profile says the file has no Item. The
// logger reports them as skipped, not as failures.
[[nodiscard]] constexpr bool no_item(ItemError error) {
  return error <= ItemError::FixedManyCells;
}

enum class Mobility : std::uint8_t { Fixed, Moving };
[[nodiscard]] constexpr std::string_view mobility_name(Mobility mobility) {
  return mobility == Mobility::Fixed ? "fixed" : "moving";
}

// Running statistics of one column, canonical.mjs columnStatistics.
struct ColumnStats {
  std::uint32_t count = 0;
  double sum = 0;
  double minimum = 0;
  double maximum = 0;
};

// Collects what the Item needs from every row of one file, in row order.
// Lives in the storage worker's PSRAM workspace. Fixed size, no heap.
class Accumulator {
public:
  struct Layout {
    std::span<const StatColumn> columns;
    std::size_t time_field = 0; // event_time_utc_us, INT64
    std::size_t cell_field = 0; // h3_cell_id, INT64
    std::size_t field_count = 0;
  };
  // Checks the layout once. Every index must be under field_count.
  [[nodiscard]] std::expected<void, ItemError> configure(const Layout &layout);
  // Starts a new file.
  void reset();
  // One row as the logger stores it, a slot and a validity byte per field.
  void add_row(std::span<const std::int64_t> data,
               std::span<const std::uint8_t> valid);

  [[nodiscard]] std::size_t rows() const { return rows_; }
  [[nodiscard]] std::span<const StatColumn> columns() const {
    return layout_.columns;
  }
  [[nodiscard]] const ColumnStats &stats(std::size_t i) const {
    return stats_[i];
  }

private:
  friend class ItemBuilder;
  Layout layout_{};
  bool configured_ = false;
  std::array<ColumnStats, kMaxColumns> stats_{};
  // Located cells in row order, a run of one cell kept once.
  std::array<location::Cell, kMaxRows> cells_{};
  std::size_t cell_count_ = 0;
  std::size_t rows_ = 0;
  std::int64_t start_us_ = 0;
  std::int64_t end_us_ = 0;
  bool null_time_ = false;
  bool overflow_ = false;
  bool bad_row_ = false;
};

// The file facts and footer values the Item repeats.
struct ItemFile {
  std::string_view name; // the final name, ending in .parquet
  std::uint64_t size = 0;
  std::span<const std::uint8_t, 32> sha256;
  std::string_view thing; // footer thing.id
  Mobility mobility = Mobility::Fixed;
  std::uint32_t sample_interval_ms = 0;
};

// Caller buffers for the outline, sized by kGeometryCapacity to hold any
// file.
struct GeometryBuffers {
  std::span<location::LngLat> points;
  std::span<location::Ring> rings;
  std::span<location::Polygon> polygons;
};

// A checked Item, ready to stream. Views stay valid while the accumulator,
// the file name and the geometry buffers do.
struct Item {
  std::string_view id;
  location::MultiPolygonView geometry;
  std::array<double, 4> bbox{};
};

// Builds and writes the Item in two steps, so every failure that needs no
// storage comes before the caller opens a file.
class ItemBuilder {
public:
  // Checks the no Item cases, sorts the distinct cells in place and traces
  // the outline. H3 allocates and frees inside this call only.
  [[nodiscard]] static std::expected<Item, ItemError>
  prepare(Accumulator &accumulator, const ItemFile &file,
          const GeometryBuffers &buffers);
  // Streams the Item through `sink`, at most `limit` bytes. Returns the
  // document length.
  [[nodiscard]] static std::expected<std::size_t, ItemError>
  write(const Item &item, const Accumulator &accumulator, const ItemFile &file,
        std::span<char> staging, json::Sink sink, std::size_t limit);
};

// Largest Item, every buffer full. Each point is at most 2 numbers of
// kRealBytes and 4 punctuation bytes, each ring and polygon 3 bytes, each
// statistics entry at most 256 bytes with the name and type, and the fixed
// text under 4 KiB. The logger passes this as the sink limit.
inline constexpr std::size_t kMaxItemBytes =
    kGeometryCapacity.points * (2 * json::kRealBytes + 4) +
    (kGeometryCapacity.rings + kGeometryCapacity.polygons) * 3 +
    kMaxColumns * 256 + 4096;

// canonical.mjs round2, half away from zero to 2 decimals in doubles.
[[nodiscard]] double round2(double value);
// canonical.mjs rfc3339. "YYYY-MM-DDTHH:MM:SS[.ffffff]Z", trailing zeros of
// the fraction dropped. Times from 1970 up to 2100 only, where the
// JavaScript reference is exact. TimeRange outside it.
inline constexpr std::size_t kTimeBytes = 28;
inline constexpr std::int64_t kMaxTimeUs = 4102444800000000; // 2100-01-01
[[nodiscard]] std::expected<std::size_t, ItemError>
rfc3339(std::span<char, kTimeBytes> out, std::int64_t us);
// The Item id, the name without ".parquet", when it matches
// ^(data_[0-9]{4}_[0-9a-f]{32}_[0-9]+-[0-9]+-[0-9]+)\.parquet$.
// The view points into name, so name must outlive it.
[[nodiscard]] std::expected<std::string_view, ItemError>
item_id(std::string_view name);

} // namespace opensensor::stac
