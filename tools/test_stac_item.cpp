// Host checks for firmware/common/stac. Run by tools/test_stac_item.py once
// per board, with that board's telemetry_contract.h on the include path.
//
//   test_stac_item table           prints the statistics table of the board
//   test_stac_item unit <numbers>  round2, rfc3339, Item id and error paths
//   test_stac_item item <input>    builds one Item, prints it or the reason
#include "numeric_sample.h"
#include "os_stac_item.h"
#include "telemetry_contract.h"

#include <h3api.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace stac = opensensor::stac;
namespace location = opensensor::location;
using telemetry::contract::kFields;
using telemetry::contract::kSemantics;

// The same expression the board wiring uses.
constexpr auto kStatColumns =
    stac::stat_columns<stac::stat_count(kFields, std::span(kSemantics))>(
        kFields, std::span(kSemantics));
static_assert(stac::stat_columns_valid(kStatColumns, kFields,
                                       std::span(kSemantics)),
              "the board statistics table parses");
constexpr std::size_t kFieldCount = std::size(kFields);
using Sample = opensensor::NumericSample<kFieldCount>;

int failures = 0;

void check(bool ok, const char *what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", what);
    ++failures;
  }
}

std::size_t position(std::string_view name) {
  return stac::field_position(kFields, name);
}

// Only the fields the statistics table reads, the rest keep their defaults.
constexpr opensensor::semantics::Column
semantic(std::string_view name, opensensor::semantics::Role role,
         std::string_view valid_when) {
  opensensor::semantics::Column column{};
  column.name = name;
  column.role = role;
  column.valid_when = valid_when;
  return column;
}

// What a null slot holds in the item mode. The equality a gate wants, a
// located res 8 cell, a real time, or a plain reading.
std::int64_t poison(std::size_t field) {
  opensensor::NumericSample<1> slot;
  for (const auto &column : kStatColumns)
    if (column.gate == stac::GateKind::Equals && column.gate_field == field) {
      if (column.gate_kind == stac::ValueKind::Int32)
        slot.set(0, static_cast<std::int32_t>(column.equals));
      else
        slot.set(0, column.equals);
      return slot.data[0];
    }
  const std::string_view name = kFields[field].name;
  switch (kFields[field].type) {
  case telemetry::PhysicalType::Int32:
    slot.set(0, std::int32_t{1});
    break;
  case telemetry::PhysicalType::Int64:
    slot.set(0, name == "h3_cell_id"          ? std::int64_t{0x887576970bfffff}
                : name == "event_time_utc_us" ? std::int64_t{1792846800000000}
                                              : std::int64_t{1});
    break;
  case telemetry::PhysicalType::Float:
    slot.set(0, 1.0F);
    break;
  case telemetry::PhysicalType::ByteArray:
    break;
  }
  return slot.data[0];
}

struct Geometry {
  std::vector<location::LngLat> points;
  std::vector<location::Ring> rings;
  std::vector<location::Polygon> polygons;
  Geometry()
      : points(stac::kGeometryCapacity.points),
        rings(stac::kGeometryCapacity.rings),
        polygons(stac::kGeometryCapacity.polygons) {}
  stac::GeometryBuffers buffers() {
    return {.points = points, .rings = rings, .polygons = polygons};
  }
};

struct Collect {
  std::string text;
  std::size_t calls = 0;
  std::size_t fail_at = static_cast<std::size_t>(-1);
};

bool collect(void *context, std::span<const char> bytes) {
  auto &state = *static_cast<Collect *>(context);
  if (state.calls++ == state.fail_at)
    return false;
  state.text.append(bytes.data(), bytes.size());
  return true;
}

stac::Accumulator::Layout board_layout() {
  return {.columns = kStatColumns,
          .time_field = position("event_time_utc_us"),
          .cell_field = position("h3_cell_id"),
          .field_count = kFieldCount};
}

const char *kind_name(stac::ValueKind kind) {
  switch (kind) {
  case stac::ValueKind::Int32:
    return "int32";
  case stac::ValueKind::Int64:
    return "int64";
  case stac::ValueKind::Float:
    return "float";
  }
  return "?";
}

// ---- table ----

int table() {
  for (const auto &column : kStatColumns) {
    const std::string name(column.name);
    const std::string type(column.type);
    const std::string gate_name(kFields[column.gate_field].name);
    switch (column.gate) {
    case stac::GateKind::None:
      std::printf("%s %s none\n", name.c_str(), type.c_str());
      break;
    case stac::GateKind::Finite:
      std::printf("%s %s finite %s\n", name.c_str(), type.c_str(),
                  gate_name.c_str());
      break;
    case stac::GateKind::Equals:
      std::printf("%s %s equals %s %" PRId64 "\n", name.c_str(), type.c_str(),
                  gate_name.c_str(), column.equals);
      break;
    }
    // The slot kind always follows the field type.
    check(column.kind == stac::value_kind(kFields[column.field].type),
          "column kind follows the field");
    check(column.gate_kind == stac::value_kind(kFields[column.gate_field].type),
          "gate kind follows the gate field");
    check(std::string_view{kind_name(column.kind)} != "?", "kind name");
  }
  return failures == 0 ? 0 : 1;
}

// ---- unit ----

std::string from_hex(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i + 1 < text.size(); i += 2)
    out.push_back(static_cast<char>(
        std::strtoul(std::string(text.substr(i, 2)).c_str(), nullptr, 16)));
  return out;
}

double from_bits(const std::string &hex) {
  return std::bit_cast<double>(
      static_cast<std::uint64_t>(std::strtoull(hex.c_str(), nullptr, 16)));
}

// Lines from test_stac_item.py, each from canonical.mjs at 83787768.
//   round2 <input bits> <result bits>
//   rfc3339 <microseconds> <text>
//   id x<hex name> <x<hex id> or ->
void reference_numbers(const char *path, std::size_t &round2_count,
                       std::size_t &time_count, std::size_t &id_count) {
  std::ifstream in(path);
  check(in.good(), "numbers file opens");
  std::string kind;
  while (in >> kind) {
    if (kind == "round2") {
      std::string input;
      std::string result;
      in >> input >> result;
      const double got = stac::round2(from_bits(input));
      const double want = from_bits(result);
      if (std::bit_cast<std::uint64_t>(got) !=
          std::bit_cast<std::uint64_t>(want)) {
        std::fprintf(stderr, "FAIL round2 %s want %s got %.17g\n",
                     input.c_str(), result.c_str(), got);
        ++failures;
      }
      ++round2_count;
    } else if (kind == "rfc3339") {
      std::int64_t us = 0;
      std::string want;
      in >> us >> want;
      std::array<char, stac::kTimeBytes> out{};
      const auto length = stac::rfc3339(out, us);
      const std::string got =
          length ? std::string(out.data(), *length) : std::string("<error>");
      if (got != want) {
        std::fprintf(stderr, "FAIL rfc3339 %" PRId64 " want %s got %s\n", us,
                     want.c_str(), got.c_str());
        ++failures;
      }
      ++time_count;
    } else if (kind == "id") {
      std::string name;
      std::string want;
      in >> name >> want;
      // item_id returns a view into its argument, so keep the bytes alive.
      const std::string decoded = from_hex(std::string_view(name).substr(1));
      const auto id = stac::item_id(decoded);
      const bool match = want != "-";
      if (match != id.has_value() ||
          (match && *id != from_hex(std::string_view(want).substr(1)))) {
        std::fprintf(stderr, "FAIL item id %s\n", name.c_str());
        ++failures;
      }
      check(id.has_value() || id.error() == stac::ItemError::NotDated,
            "a bad name is NotDated");
      ++id_count;
    } else {
      check(false, "numbers line kind");
      return;
    }
  }
}

void time_edges() {
  std::array<char, stac::kTimeBytes> out{};
  const auto before = stac::rfc3339(out, -1);
  check(!before && before.error() == stac::ItemError::TimeRange,
        "a time before 1970 fails");
  const auto after = stac::rfc3339(out, stac::kMaxTimeUs);
  check(!after && after.error() == stac::ItemError::TimeRange,
        "a time in 2100 fails");
  const auto longest = stac::rfc3339(out, stac::kMaxTimeUs - 1);
  check(longest && std::string_view(out.data(), *longest) ==
                       "2099-12-31T23:59:59.999999Z",
        "last microsecond before 2100");
  check(longest && *longest == stac::kTimeBytes - 1, "longest time fits");
}

location::Cell cell_at(double lat, double lng) {
  const LatLng point{.lat = lat * std::numbers::pi / 180.0,
                     .lng = lng * std::numbers::pi / 180.0};
  H3Index cell = 0;
  check(latLngToCell(&point, stac::kResolution, &cell) == E_SUCCESS,
        "latLngToCell");
  return cell;
}

constexpr std::array<std::uint8_t, 32> kSha{};
constexpr std::string_view kName =
    "data_1300_0f3a9c2e5b7d41e8a6c3f0b2d9e4a713_2881-2970-0.parquet";

stac::ItemFile file_facts(stac::Mobility mobility) {
  return {.name = kName,
          .size = 1,
          .sha256 = kSha,
          .thing = "3f2b9c1e-6a7d-4e58-9b1c-2d4f6a8e0c13",
          .mobility = mobility,
          .sample_interval_ms = 10000};
}

// A row with a time and a cell and every statistics column set, integer
// columns to `integer` and float columns to `reading`.
Sample row_at(std::int64_t time, location::Cell cell, float reading,
              std::int64_t integer = 1) {
  Sample sample;
  sample.set(position("event_time_utc_us"), time);
  sample.set(position("h3_cell_id"), static_cast<std::int64_t>(cell));
  const auto put = [&](std::size_t field, stac::ValueKind kind,
                       std::int64_t value) {
    switch (kind) {
    case stac::ValueKind::Int32:
      sample.set(field, static_cast<std::int32_t>(value));
      break;
    case stac::ValueKind::Int64:
      sample.set(field, value);
      break;
    case stac::ValueKind::Float:
      sample.set(field, reading);
      break;
    }
  };
  for (const auto &column : kStatColumns)
    put(column.field, column.kind, integer);
  // Pass every equality gate.
  for (const auto &column : kStatColumns)
    if (column.gate == stac::GateKind::Equals)
      put(column.gate_field, column.gate_kind, column.equals);
  return sample;
}

void add(stac::Accumulator &accumulator, const Sample &sample) {
  accumulator.add_row(sample.data, sample.valid);
}

stac::ItemError prepare_error(stac::Accumulator &accumulator,
                              const stac::ItemFile &file,
                              const stac::GeometryBuffers &buffers) {
  const auto item = stac::ItemBuilder::prepare(accumulator, file, buffers);
  return item ? stac::ItemError::JsonStructure : item.error();
}

void layout_errors() {
  using stac::ItemError;
  stac::Accumulator accumulator;
  const auto layout = board_layout();
  check(accumulator.configure(layout).has_value(), "board layout configures");
  auto bad = layout;
  bad.field_count = 0;
  check(!accumulator.configure(bad), "no fields fails");
  bad = layout;
  bad.time_field = kFieldCount;
  check(!accumulator.configure(bad), "time field out of range fails");
  bad = layout;
  bad.cell_field = kFieldCount;
  check(!accumulator.configure(bad), "cell field out of range fails");
  std::array<stac::StatColumn, 1> wide{kStatColumns[0]};
  wide[0].field = static_cast<std::uint16_t>(kFieldCount);
  bad = layout;
  bad.columns = wide;
  check(!accumulator.configure(bad), "column field out of range fails");
  wide[0] = kStatColumns[0];
  wide[0].gate_field = static_cast<std::uint16_t>(kFieldCount);
  check(!accumulator.configure(bad), "gate field out of range fails");
  std::vector<stac::StatColumn> many(stac::kMaxColumns + 1, kStatColumns[0]);
  bad = layout;
  bad.columns = many;
  const auto too_many = accumulator.configure(bad);
  check(!too_many && too_many.error() == ItemError::BadLayout,
        "too many columns fails");
  // An unconfigured accumulator never yields an Item.
  Geometry geometry;
  add(accumulator, row_at(1792843200000000, cell_at(30.0444, 31.2357), 1));
  check(prepare_error(accumulator, file_facts(stac::Mobility::Fixed),
                      geometry.buffers()) == ItemError::BadLayout,
        "unconfigured accumulator fails");
  // A short row sticks until the next file.
  check(accumulator.configure(layout).has_value(), "configures again");
  const Sample sample = row_at(1792843200000000, cell_at(30.0444, 31.2357), 1);
  accumulator.add_row(std::span(sample.data).first(kFieldCount - 1),
                      sample.valid);
  add(accumulator, sample);
  check(prepare_error(accumulator, file_facts(stac::Mobility::Fixed),
                      geometry.buffers()) == ItemError::BadLayout,
        "short row fails");
  accumulator.reset();
  add(accumulator, sample);
  check(stac::ItemBuilder::prepare(
            accumulator, file_facts(stac::Mobility::Fixed), geometry.buffers())
            .has_value(),
        "reset clears a short row");
}

void row_and_time_limits() {
  using stac::ItemError;
  stac::Accumulator accumulator;
  check(accumulator.configure(board_layout()).has_value(), "configures");
  Geometry geometry;
  const auto cell = cell_at(30.0444, 31.2357);
  for (std::size_t i = 0; i < stac::kMaxRows; ++i)
    add(accumulator,
        row_at(1792843200000000 + static_cast<std::int64_t>(i) * 10000000, cell,
               1));
  check(stac::ItemBuilder::prepare(
            accumulator, file_facts(stac::Mobility::Fixed), geometry.buffers())
            .has_value(),
        "720 rows build");
  add(accumulator, row_at(1792843200000000, cell, 1));
  check(accumulator.rows() == stac::kMaxRows, "row 721 is not counted");
  check(prepare_error(accumulator, file_facts(stac::Mobility::Fixed),
                      geometry.buffers()) == ItemError::TooManyRows,
        "721 rows fail");
  for (const std::int64_t time : {std::int64_t{-1}, stac::kMaxTimeUs}) {
    accumulator.reset();
    add(accumulator, row_at(1792843200000000, cell, 1));
    add(accumulator, row_at(time, cell, 1));
    check(prepare_error(accumulator, file_facts(stac::Mobility::Fixed),
                        geometry.buffers()) == ItemError::TimeRange,
          "time outside 1970 to 2099 fails");
  }
}

void geometry_and_sink_errors() {
  using stac::ItemError;
  stac::Accumulator accumulator;
  check(accumulator.configure(board_layout()).has_value(), "configures");
  add(accumulator, row_at(1792843200000000, cell_at(30.0444, 31.2357), 1));
  // Buffers too small for one hexagon.
  std::array<location::LngLat, 3> points{};
  std::array<location::Ring, 1> rings{};
  std::array<location::Polygon, 1> polygons{};
  check(
      prepare_error(accumulator, file_facts(stac::Mobility::Fixed),
                    {.points = points, .rings = rings, .polygons = polygons}) ==
          ItemError::Geometry,
      "short point buffer fails");
  Geometry geometry;
  const auto file = file_facts(stac::Mobility::Fixed);
  const auto item =
      stac::ItemBuilder::prepare(accumulator, file, geometry.buffers());
  check(item.has_value(), "one row builds");
  if (!item)
    return;
  std::array<char, 64> staging{};
  Collect whole;
  const auto length = stac::ItemBuilder::write(
      *item, accumulator, file, staging, {.write = collect, .context = &whole},
      stac::kMaxItemBytes);
  check(length && *length == whole.text.size(), "one row writes");
  for (const std::size_t fail_at : {std::size_t{0}, std::size_t{5}}) {
    Collect failing;
    failing.fail_at = fail_at;
    const auto result = stac::ItemBuilder::write(
        *item, accumulator, file, staging,
        {.write = collect, .context = &failing}, stac::kMaxItemBytes);
    check(!result && result.error() == ItemError::JsonSink,
          "a failed write is JsonSink");
  }
  if (length) {
    Collect exact;
    check(stac::ItemBuilder::write(*item, accumulator, file, staging,
                                   {.write = collect, .context = &exact},
                                   *length)
              .has_value(),
          "limit equal to the length passes");
    Collect short_limit;
    const auto over = stac::ItemBuilder::write(
        *item, accumulator, file, staging,
        {.write = collect, .context = &short_limit}, *length - 1);
    check(!over && over.error() == ItemError::JsonOverflow,
          "limit one short is JsonOverflow");
  }
  Collect empty;
  const auto no_staging = stac::ItemBuilder::write(
      *item, accumulator, file, std::span<char>{},
      {.write = collect, .context = &empty}, stac::kMaxItemBytes);
  check(!no_staging && no_staging.error() == ItemError::JsonOverflow,
        "no staging fails");
  // A name longer than the href buffer.
  std::string long_name = "data_1300_0f3a9c2e5b7d41e8a6c3f0b2d9e4a713_" +
                          std::string(300, '1') + "-1-1.parquet";
  auto long_file = file;
  long_file.name = long_name;
  const auto long_item =
      stac::ItemBuilder::prepare(accumulator, long_file, geometry.buffers());
  check(long_item.has_value(), "a long name is still dated");
  if (long_item) {
    Collect out;
    const auto result = stac::ItemBuilder::write(
        *long_item, accumulator, long_file, staging,
        {.write = collect, .context = &out}, stac::kMaxItemBytes);
    check(!result && result.error() == stac::ItemError::NotDated,
          "an id over 256 bytes fails");
  }
}

// The largest document the board can write stays under kMaxItemBytes. 720
// distinct cells far apart, long coordinates and the longest readings.
void largest_item() {
  stac::Accumulator accumulator;
  check(accumulator.configure(board_layout()).has_value(), "configures");
  for (std::size_t i = 0; i < stac::kMaxRows; ++i) {
    const auto cell = cell_at(-60.123456 - 0.05 * static_cast<double>(i / 30),
                              -170.123456 + 0.05 * static_cast<double>(i % 30));
    const bool even = i % 2 == 0;
    const float reading = even ? -3.40282347e+38F : 1.17549435e-38F;
    const std::int64_t integer = even
                                     ? std::numeric_limits<std::int32_t>::min()
                                     : std::numeric_limits<std::int32_t>::max();
    add(accumulator,
        row_at(1792843200123457 + static_cast<std::int64_t>(i) * 10000001, cell,
               reading, integer));
  }
  Geometry geometry;
  const auto file = file_facts(stac::Mobility::Moving);
  const auto item =
      stac::ItemBuilder::prepare(accumulator, file, geometry.buffers());
  check(item.has_value(), "720 apart cells build");
  if (!item)
    return;
  check(item->geometry.polygons.size() == stac::kMaxRows,
        "720 apart cells are 720 polygons");
  std::array<char, 256> staging{};
  Collect out;
  const auto length = stac::ItemBuilder::write(
      *item, accumulator, file, staging, {.write = collect, .context = &out},
      stac::kMaxItemBytes);
  check(length.has_value(), "largest Item writes");
  if (length) {
    check(*length <= stac::kMaxItemBytes, "largest Item fits the limit");
    std::printf("largest Item %zu bytes, limit %zu\n", *length,
                stac::kMaxItemBytes);
  }
}

void stat_table_rules() {
  // The board table holds every reported column and parses every gate.
  check(kStatColumns.size() == stac::stat_count(kFields, std::span(kSemantics)),
        "table size");
  check(stac::numeric_type(telemetry::PhysicalType::Int32, "uint8_code") ==
            "uint8",
        "uint8 code");
  check(stac::numeric_type(telemetry::PhysicalType::ByteArray, "text").empty(),
        "text is not numeric");
  check(
      stac::numeric_type(telemetry::PhysicalType::ByteArray, "wkb_point_crs84")
          .empty(),
      "geometry is not numeric");
  std::int64_t value = 0;
  check(stac::parse_integer("-9223372036854775808", value) &&
            value == std::numeric_limits<std::int64_t>::min(),
        "int64 min parses");
  check(!stac::parse_integer("9223372036854775808", value),
        "int64 max + 1 fails");
  check(!stac::parse_integer("", value) && !stac::parse_integer("-", value) &&
            !stac::parse_integer("4a", value) &&
            !stac::parse_integer(" 4", value),
        "non integers fail");
  // A gate the parser cannot read makes the table invalid.
  using enum opensensor::semantics::Role;
  static constexpr opensensor::semantics::Column kOdd[] = {
      semantic("a", physicalMeasurement, "a != 4"),
  };
  struct Field {
    const char *name;
    telemetry::PhysicalType type;
    const char *unit;
  };
  static constexpr Field kOddFields[] = {
      {.name = "a", .type = telemetry::PhysicalType::Int32, .unit = "1"}};
  constexpr auto odd = stac::stat_columns<1>(kOddFields, std::span(kOdd));
  static_assert(!stac::stat_columns_valid(odd, kOddFields, std::span(kOdd)),
                "an unparsed gate is refused");
  static constexpr opensensor::semantics::Column kFloatGate[] = {
      semantic("a", physicalMeasurement, "b == 1"),
      semantic("b", qualityInformation, ""),
  };
  static constexpr Field kFloatFields[] = {
      {.name = "a", .type = telemetry::PhysicalType::Int32, .unit = "1"},
      {.name = "b", .type = telemetry::PhysicalType::Float, .unit = "1"}};
  constexpr auto float_gate =
      stac::stat_columns<1>(kFloatFields, std::span(kFloatGate));
  static_assert(!stac::stat_columns_valid(float_gate, kFloatFields,
                                          std::span(kFloatGate)),
                "an equality gate on a float column is refused");
  static_assert(stac::no_item(stac::ItemError::FixedManyCells) &&
                    !stac::no_item(stac::ItemError::TimeRange),
                "no Item cases come first");
}

int unit(const char *numbers) {
  std::size_t round2_count = 0;
  std::size_t time_count = 0;
  std::size_t id_count = 0;
  reference_numbers(numbers, round2_count, time_count, id_count);
  check(round2_count > 4000 && time_count > 1000 && id_count > 30,
        "reference table sizes");
  time_edges();
  stat_table_rules();
  layout_errors();
  row_and_time_limits();
  geometry_and_sink_errors();
  largest_item();
  for (int e = 0; e <= static_cast<int>(stac::ItemError::JsonSink); ++e)
    check(std::string_view{stac::item_error_name(
              static_cast<stac::ItemError>(e))} != "unknown",
          "every error has a name");
  if (failures) {
    std::fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  std::printf("stac unit: %zu round2, %zu rfc3339 and %zu id results matched "
              "canonical.mjs, error paths passed\n",
              round2_count, time_count, id_count);
  return 0;
}

// ---- item ----
//
// Input lines from test_stac_item.py.
//   file <name>  size <n>  sha256 <hex>  thing <id or ->
//   mobility fixed|moving  interval_ms <n>
//   col <name> <value or null>...  one line per column, every row
int item(const char *path) {
  std::ifstream in(path);
  if (!in.good()) {
    std::fprintf(stderr, "cannot open %s\n", path);
    return 2;
  }
  std::string name;
  std::string thing;
  std::uint64_t size = 0;
  std::array<std::uint8_t, 32> sha{};
  auto mobility = stac::Mobility::Fixed;
  std::uint32_t interval_ms = 0;
  std::vector<Sample> rows;
  std::size_t row_count = static_cast<std::size_t>(-1);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream words(line);
    std::string key;
    words >> key;
    if (key == "file") {
      words >> name;
    } else if (key == "size") {
      words >> size;
    } else if (key == "sha256") {
      std::string hex;
      words >> hex;
      const auto bytes = from_hex(hex);
      if (bytes.size() != sha.size())
        return 2;
      std::ranges::copy(bytes, reinterpret_cast<char *>(sha.data()));
    } else if (key == "thing") {
      words >> thing;
      if (thing == "-")
        thing.clear();
    } else if (key == "mobility") {
      std::string text;
      words >> text;
      mobility =
          text == "fixed" ? stac::Mobility::Fixed : stac::Mobility::Moving;
    } else if (key == "interval_ms") {
      words >> interval_ms;
    } else if (key == "col") {
      std::string column;
      words >> column;
      const auto field = position(column);
      if (field >= kFieldCount) {
        std::fprintf(stderr, "unknown column %s\n", column.c_str());
        return 2;
      }
      std::vector<std::string> values;
      for (std::string value; words >> value;)
        values.push_back(value);
      if (row_count == static_cast<std::size_t>(-1)) {
        row_count = values.size();
        rows.resize(row_count);
      } else if (values.size() != row_count) {
        std::fprintf(stderr, "column %s has %zu rows\n", column.c_str(),
                     values.size());
        return 2;
      }
      const bool cell = column == "h3_cell_id";
      for (std::size_t i = 0; i < row_count; ++i) {
        const auto &text = values[i];
        if (text == "null")
          continue;
        switch (kFields[field].type) {
        case telemetry::PhysicalType::Int32:
          rows[i].set(field, static_cast<std::int32_t>(
                                 std::strtol(text.c_str(), nullptr, 10)));
          break;
        case telemetry::PhysicalType::Int64:
          rows[i].set(field, cell ? static_cast<std::int64_t>(std::strtoull(
                                        text.c_str(), nullptr, 16))
                                  : static_cast<std::int64_t>(std::strtoll(
                                        text.c_str(), nullptr, 10)));
          break;
        case telemetry::PhysicalType::Float:
          // Math.fround of the JSON double, round to nearest even.
          rows[i].set(field,
                      static_cast<float>(std::strtod(text.c_str(), nullptr)));
          break;
        case telemetry::PhysicalType::ByteArray:
          std::fprintf(stderr, "column %s is not numeric\n", column.c_str());
          return 2;
        }
      }
    }
  }
  stac::Accumulator accumulator;
  if (!accumulator.configure(board_layout())) {
    std::fprintf(stderr, "board layout refused\n");
    return 2;
  }
  // A null slot keeps valid 0, but its data holds a value that would pass,
  // so a builder that reads data without its validity byte is caught.
  for (auto &row : rows)
    for (std::size_t field = 0; field < kFieldCount; ++field)
      if (row.valid[field] == 0) {
        row.data[field] = poison(field);
        row.valid[field] = 0;
      }
  for (const auto &row : rows)
    add(accumulator, row);
  const stac::ItemFile file{.name = name,
                            .size = size,
                            .sha256 = sha,
                            .thing = thing,
                            .mobility = mobility,
                            .sample_interval_ms = interval_ms};
  Geometry geometry;
  const auto prepared =
      stac::ItemBuilder::prepare(accumulator, file, geometry.buffers());
  if (!prepared) {
    std::printf("%s %s\n", stac::no_item(prepared.error()) ? "none" : "error",
                stac::item_error_name(prepared.error()));
    return 0;
  }
  // An odd staging size, so chunks split numbers and keys.
  std::array<char, 61> staging{};
  Collect out;
  const auto length = stac::ItemBuilder::write(
      *prepared, accumulator, file, staging,
      {.write = collect, .context = &out}, stac::kMaxItemBytes);
  if (!length) {
    std::printf("error %s\n", stac::item_error_name(length.error()));
    return 0;
  }
  // The bytes do not depend on the staging size.
  std::vector<char> large(4096);
  Collect again;
  const auto second = stac::ItemBuilder::write(
      *prepared, accumulator, file, large,
      {.write = collect, .context = &again}, stac::kMaxItemBytes);
  if (!second || *second != *length || again.text != out.text ||
      out.text.size() != *length) {
    std::fprintf(stderr, "staging size changed the bytes\n");
    return 1;
  }
  std::fwrite(out.text.data(), 1, out.text.size(), stdout);
  std::printf("\n");
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  const std::string_view mode = argc >= 2 ? argv[1] : "";
  if (mode == "table" && argc == 2)
    return table();
  if (mode == "unit" && argc == 3)
    return unit(argv[2]);
  if (mode == "item" && argc == 3)
    return item(argv[2]);
  std::fprintf(stderr, "usage: %s table | unit <numbers> | item <input>\n",
               argv[0]);
  return 2;
}
