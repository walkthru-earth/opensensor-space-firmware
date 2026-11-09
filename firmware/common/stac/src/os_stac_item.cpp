#include "os_stac_item.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>

// Built with -ffp-contract=off (CMakeLists.txt and the host test), so
// round2() and the sums round each operation as JavaScript does and the
// compiler never fuses a multiply and an add.
namespace opensensor::stac {

const char *item_error_name(ItemError error) {
  switch (error) {
  case ItemError::NotDated:
    return "not-dated";
  case ItemError::NoThing:
    return "no-thing";
  case ItemError::NoRows:
    return "no-rows";
  case ItemError::NullTime:
    return "null-time";
  case ItemError::NotLocated:
    return "not-located";
  case ItemError::NotResolution8:
    return "not-resolution-8";
  case ItemError::FixedManyCells:
    return "fixed-many-cells";
  case ItemError::TimeRange:
    return "time-range";
  case ItemError::TooManyRows:
    return "too-many-rows";
  case ItemError::BadLayout:
    return "bad-layout";
  case ItemError::GeometryMemory:
    return "geometry-memory";
  case ItemError::Geometry:
    return "geometry";
  case ItemError::JsonOverflow:
    return "json-overflow";
  case ItemError::JsonNotFinite:
    return "json-not-finite";
  case ItemError::JsonStructure:
    return "json-structure";
  case ItemError::JsonSink:
    return "json-sink";
  }
  return "unknown";
}

namespace {
std::int32_t read_int32(std::int64_t slot) {
  std::int32_t value = 0;
  std::memcpy(&value, &slot, sizeof(value));
  return value;
}
float read_float(std::int64_t slot) {
  float value = 0;
  std::memcpy(&value, &slot, sizeof(value));
  return value;
}
// The value JavaScript sees once canonical.mjs widens it. A float column is
// Math.fround of its value, which is the float itself.
double read_number(std::int64_t slot, ValueKind kind) {
  switch (kind) {
  case ValueKind::Int32:
    return static_cast<double>(read_int32(slot));
  case ValueKind::Int64:
    return static_cast<double>(slot);
  case ValueKind::Float:
    return static_cast<double>(read_float(slot));
  }
  return 0;
}

// canonical.mjs gatePasses. A null gate value fails. Equality on an integer
// column compares the integers, never through a double.
bool gate_passes(const StatColumn &column, std::span<const std::int64_t> data,
                 std::span<const std::uint8_t> valid) {
  if (valid[column.gate_field] == 0)
    return false;
  const auto slot = data[column.gate_field];
  switch (column.gate) {
  case GateKind::None:
    return true;
  case GateKind::Finite:
    return column.gate_kind != ValueKind::Float ||
           std::isfinite(read_float(slot));
  case GateKind::Equals:
    switch (column.gate_kind) {
    case ValueKind::Int32:
      return read_int32(slot) == column.equals;
    case ValueKind::Int64:
      return slot == column.equals;
    case ValueKind::Float:
      return static_cast<double>(read_float(slot)) ==
             static_cast<double>(column.equals);
    }
  }
  return false;
}

ItemError json_error(json::Error error) {
  switch (error) {
  case json::Error::Overflow:
    return ItemError::JsonOverflow;
  case json::Error::NotFinite:
    return ItemError::JsonNotFinite;
  case json::Error::Structure:
    return ItemError::JsonStructure;
  case json::Error::Sink:
    return ItemError::JsonSink;
  }
  return ItemError::JsonStructure;
}

bool digits(std::string_view text, std::size_t count) {
  if (text.size() != count)
    return false;
  return std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}
bool lower_hex(std::string_view text, std::size_t count) {
  if (text.size() != count)
    return false;
  return std::ranges::all_of(text, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
// [0-9]+ followed by `end`, which is consumed.
bool number_then(std::string_view &text, char end) {
  std::size_t i = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9')
    ++i;
  if (i == 0 || i == text.size() || text[i] != end)
    return false;
  text.remove_prefix(i + 1);
  return true;
}

// Days since 1970-01-01 to a proleptic Gregorian date, Howard Hinnant's
// civil_from_days (chrono-Compatible Low-Level Date Algorithms).
struct Date {
  std::int64_t year;
  unsigned month;
  unsigned day;
};
Date civil_from_days(std::int64_t days) {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned day = doy - (153 * mp + 2) / 5 + 1;
  const unsigned month = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t year = static_cast<std::int64_t>(yoe) + era * 400;
  return {.year = month <= 2 ? year + 1 : year, .month = month, .day = day};
}

// Writes the distinct sorted cells' outline in GeoJSON coordinate nesting.
void ring(json::Writer &json, const location::MultiPolygon &geometry,
          const location::Ring &r) {
  json.begin_array();
  for (std::uint32_t i = 0; i < r.count; ++i) {
    const auto &point = geometry.points[r.first + i];
    json.begin_array();
    json.real_value(point.longitude);
    json.real_value(point.latitude);
    json.end_array();
  }
  json.end_array();
}
void polygon(json::Writer &json, const location::MultiPolygon &geometry,
             const location::Polygon &p) {
  json.begin_array();
  for (std::uint32_t i = 0; i < p.ring_count; ++i)
    ring(json, geometry, geometry.rings[p.first_ring + i]);
  json.end_array();
}
} // namespace

double round2(double value) {
  // Math.sign(v) * Math.floor(Math.abs(v) * 100 + 0.5) / 100, one rounding
  // per operation, left to right.
  const double sign = value > 0 ? 1.0 : value < 0 ? -1.0 : value;
  const double scaled = std::fabs(value) * 100.0;
  const double shifted = scaled + 0.5;
  const double floored = std::floor(shifted);
  const double signed_floor = sign * floored;
  return signed_floor / 100.0;
}

std::expected<std::size_t, ItemError> rfc3339(std::span<char, kTimeBytes> out,
                                              std::int64_t us) {
  if (us < 0 || us >= kMaxTimeUs)
    return std::unexpected(ItemError::TimeRange);
  const std::int64_t seconds = us / 1000000;
  const std::int64_t micros = us % 1000000;
  const std::int64_t days = seconds / 86400;
  const auto second_of_day = static_cast<unsigned>(seconds % 86400);
  const Date date = civil_from_days(days);
  std::size_t used = 0;
  const auto put = [&](unsigned value, std::size_t width) {
    for (std::size_t i = width; i-- > 0;) {
      out[used + i] = static_cast<char>('0' + value % 10);
      value /= 10;
    }
    used += width;
  };
  put(static_cast<unsigned>(date.year), 4);
  out[used++] = '-';
  put(date.month, 2);
  out[used++] = '-';
  put(date.day, 2);
  out[used++] = 'T';
  put(second_of_day / 3600, 2);
  out[used++] = ':';
  put(second_of_day / 60 % 60, 2);
  out[used++] = ':';
  put(second_of_day % 60, 2);
  if (micros != 0) {
    out[used++] = '.';
    put(static_cast<unsigned>(micros), 6);
    while (out[used - 1] == '0')
      --used;
  }
  out[used++] = 'Z';
  return used;
}

std::expected<std::string_view, ItemError> item_id(std::string_view name) {
  constexpr std::string_view kPrefix = "data_";
  constexpr std::string_view kSuffix = ".parquet";
  if (!name.starts_with(kPrefix) || !name.ends_with(kSuffix))
    return std::unexpected(ItemError::NotDated);
  const auto id = name.substr(0, name.size() - kSuffix.size());
  auto rest = id.substr(kPrefix.size());
  // HHMM_ then 32 lowercase hex digits and _.
  if (rest.size() < 4 + 1 + 32 + 1 || !digits(rest.substr(0, 4), 4) ||
      rest[4] != '_' || !lower_hex(rest.substr(5, 32), 32) || rest[37] != '_')
    return std::unexpected(ItemError::NotDated);
  rest.remove_prefix(38);
  if (!number_then(rest, '-') || !number_then(rest, '-') || rest.empty() ||
      !digits(rest, rest.size()))
    return std::unexpected(ItemError::NotDated);
  return id;
}

std::expected<void, ItemError> Accumulator::configure(const Layout &layout) {
  configured_ = false;
  if (layout.field_count == 0 || layout.columns.size() > kMaxColumns ||
      layout.time_field >= layout.field_count ||
      layout.cell_field >= layout.field_count)
    return std::unexpected(ItemError::BadLayout);
  if (std::ranges::any_of(layout.columns, [&layout](const StatColumn &column) {
        return column.field >= layout.field_count ||
               column.gate_field >= layout.field_count;
      }))
    return std::unexpected(ItemError::BadLayout);
  layout_ = layout;
  configured_ = true;
  reset();
  return {};
}

void Accumulator::reset() {
  stats_.fill(ColumnStats{});
  cell_count_ = 0;
  rows_ = 0;
  start_us_ = 0;
  end_us_ = 0;
  null_time_ = false;
  overflow_ = false;
  bad_row_ = !configured_;
}

void Accumulator::add_row(std::span<const std::int64_t> data,
                          std::span<const std::uint8_t> valid) {
  if (!configured_ || data.size() < layout_.field_count ||
      valid.size() < layout_.field_count) {
    bad_row_ = true;
    return;
  }
  if (rows_ == kMaxRows) {
    overflow_ = true;
    return;
  }
  ++rows_;
  if (valid[layout_.time_field] == 0) {
    null_time_ = true;
  } else {
    const auto time = data[layout_.time_field];
    start_us_ = rows_ == 1 ? time : std::min(start_us_, time);
    end_us_ = rows_ == 1 ? time : std::max(end_us_, time);
  }
  if (valid[layout_.cell_field] != 0) {
    const auto cell = static_cast<location::Cell>(data[layout_.cell_field]);
    if (cell != 0 && (cell_count_ == 0 || cells_[cell_count_ - 1] != cell))
      cells_[cell_count_++] = cell; // cell_count_ <= rows_ <= kMaxRows
  }
  for (std::size_t i = 0; i < layout_.columns.size(); ++i) {
    const auto &column = layout_.columns[i];
    if (valid[column.field] == 0)
      continue;
    const double value = read_number(data[column.field], column.kind);
    if (!std::isfinite(value) || !gate_passes(column, data, valid))
      continue;
    auto &entry = stats_[i];
    entry.minimum = entry.count == 0 ? value : std::min(entry.minimum, value);
    entry.maximum = entry.count == 0 ? value : std::max(entry.maximum, value);
    entry.sum += value;
    ++entry.count;
  }
}

std::expected<Item, ItemError>
ItemBuilder::prepare(Accumulator &accumulator, const ItemFile &file,
                     const GeometryBuffers &buffers) {
  if (accumulator.bad_row_)
    return std::unexpected(ItemError::BadLayout);
  if (accumulator.overflow_)
    return std::unexpected(ItemError::TooManyRows);
  const auto id = item_id(file.name);
  if (!id)
    return std::unexpected(id.error());
  if (file.thing.empty())
    return std::unexpected(ItemError::NoThing);
  if (accumulator.rows_ == 0)
    return std::unexpected(ItemError::NoRows);
  if (accumulator.null_time_)
    return std::unexpected(ItemError::NullTime);
  if (accumulator.cell_count_ == 0)
    return std::unexpected(ItemError::NotLocated);
  // [...new Set(located)].sort(). Every resolution 8 cell is 15 hex digits,
  // so numeric order is the order of the strings. A set holding any other
  // cell gets no Item, whatever its order.
  const auto cells =
      std::span(accumulator.cells_).first(accumulator.cell_count_);
  std::ranges::sort(cells);
  const auto last = std::ranges::unique(cells);
  accumulator.cell_count_ =
      static_cast<std::size_t>(last.begin() - cells.begin());
  const auto distinct = cells.first(accumulator.cell_count_);
  // published_cell keeps a valid res 8 cell as is. Any other value is
  // refused or would change, and canonical.mjs gives that file no Item, so
  // the board never relabels a cell here.
  for (const auto cell : distinct) {
    const auto published = location::published_cell(cell);
    if (!published || *published != cell)
      return std::unexpected(ItemError::NotResolution8);
  }
  if (file.mobility == Mobility::Fixed && distinct.size() > 1)
    return std::unexpected(ItemError::FixedManyCells);
  if (accumulator.start_us_ < 0 || accumulator.end_us_ >= kMaxTimeUs)
    return std::unexpected(ItemError::TimeRange);

  const auto geometry = location::cells_to_multipolygon(
      distinct, buffers.points, buffers.rings, buffers.polygons);
  if (!geometry)
    return std::unexpected(geometry.error() == location::Error::OutOfMemory
                               ? ItemError::GeometryMemory
                               : ItemError::Geometry);
  Item item{.id = *id, .geometry = *geometry, .bbox = {}};
  constexpr double kInf = std::numeric_limits<double>::infinity();
  item.bbox = {kInf, kInf, -kInf, -kInf};
  for (const auto &point : geometry->points) {
    item.bbox[0] = std::min(item.bbox[0], point.longitude);
    item.bbox[1] = std::min(item.bbox[1], point.latitude);
    item.bbox[2] = std::max(item.bbox[2], point.longitude);
    item.bbox[3] = std::max(item.bbox[3], point.latitude);
  }
  return item;
}

std::expected<std::size_t, ItemError>
ItemBuilder::write(const Item &item, const Accumulator &accumulator,
                   const ItemFile &file, std::span<char> staging,
                   json::Sink sink, std::size_t limit) {
  std::array<char, kTimeBytes> start{};
  std::array<char, kTimeBytes> end{};
  const auto start_length = rfc3339(start, accumulator.start_us_);
  const auto end_length = rfc3339(end, accumulator.end_us_);
  if (!start_length)
    return std::unexpected(start_length.error());
  if (!end_length)
    return std::unexpected(end_length.error());
  const bool fixed = file.mobility == Mobility::Fixed;
  std::array<char, 3 + location::kCellTextBytes> grid{'H', '3', '-'};
  if (fixed && !location::format_cell(
                   accumulator.cells_[0],
                   std::span(grid).subspan<3, location::kCellTextBytes>()))
    return std::unexpected(ItemError::NotResolution8);
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::array<char, 4 + 2 * 32> checksum{'1', '2', '2', '0'};
  for (std::size_t i = 0; i < file.sha256.size(); ++i) {
    checksum[4 + 2 * i] = kHex[file.sha256[i] >> 4];
    checksum[5 + 2 * i] = kHex[file.sha256[i] & 0x0f];
  }
  // "./" + id + ".parquet"
  std::array<char, 2 + 256 + 8> href{};
  if (item.id.size() > 256)
    return std::unexpected(ItemError::NotDated);
  std::size_t href_length = 0;
  for (const char c : std::string_view{"./"})
    href[href_length++] = c;
  for (const char c : item.id)
    href[href_length++] = c;
  for (const char c : std::string_view{".parquet"})
    href[href_length++] = c;

  json::Writer json(staging, sink, limit);
  json.begin();
  json.text("type", "Feature");
  json.text("stac_version", kStacVersion);
  json.open_array("stac_extensions");
  json.string_value(kTableExtension);
  json.string_value(kFileExtension);
  if (fixed)
    json.string_value(kGridExtension);
  json.string_value(kAnonExtension);
  json.string_value(kOpensensorExtension);
  json.string_value(kProfileSchema);
  json.close_array();
  json.text("id", item.id);
  json.text("collection", file.thing);

  json.open("geometry");
  const bool single = item.geometry.polygons.size() == 1;
  json.text("type", single ? "Polygon" : "MultiPolygon");
  json.key("coordinates");
  if (single) {
    polygon(json, item.geometry, item.geometry.polygons[0]);
  } else {
    json.begin_array();
    for (const auto &p : item.geometry.polygons)
      polygon(json, item.geometry, p);
    json.end_array();
  }
  json.close();
  json.open_array("bbox");
  for (const double value : item.bbox)
    json.real_value(value);
  json.close_array();

  json.open("properties");
  json.null("datetime");
  json.text("start_datetime", {start.data(), *start_length});
  json.text("end_datetime", {end.data(), *end_length});
  json.text("opensensor:mobility", mobility_name(file.mobility));
  json.real("opensensor:sample_interval_s",
            static_cast<double>(file.sample_interval_ms) / 1000.0);
  if (fixed)
    json.text("grid:code", {grid.data(), 3 + location::kCellTextBytes - 1});
  json.real("anon:size", kAnonSize);
  json.text("anon:warning", fixed ? kFixedWarning : kMovingWarning);
  json.close();

  json.open_array("links");
  constexpr std::string_view kRels[] = {"root", "parent", "collection"};
  constexpr std::string_view kHrefs[] = {"../../../../catalog.json",
                                         "../../../collection.json",
                                         "../../../collection.json"};
  for (std::size_t i = 0; i < std::size(kRels); ++i) {
    json.begin_object();
    json.text("rel", kRels[i]);
    json.text("href", kHrefs[i]);
    json.text("type", "application/json");
    json.end_object();
  }
  json.close_array();

  json.open("assets");
  json.open("data");
  json.text("href", {href.data(), href_length});
  json.text("type", "application/vnd.apache.parquet");
  json.open_array("roles");
  json.string_value("data");
  json.close_array();
  json.number("file:size", file.size);
  json.text("file:checksum", {checksum.data(), checksum.size()});
  json.number("table:row_count", accumulator.rows_);
  const auto columns = accumulator.columns();
  if (!columns.empty()) {
    const auto rows = static_cast<double>(accumulator.rows_);
    json.open_array("table:columns");
    for (std::size_t i = 0; i < columns.size(); ++i) {
      const auto &stats = accumulator.stats(i);
      json.begin_object();
      json.text("name", columns[i].name);
      json.text("type", columns[i].type);
      json.open("statistics");
      if (stats.count == 0) {
        json.real("valid_percent", 0.0);
      } else {
        const auto count = static_cast<double>(stats.count);
        json.real("minimum", stats.minimum);
        json.real("maximum", stats.maximum);
        json.real("mean", round2(stats.sum / count));
        const double percent = 100.0 * count;
        json.real("valid_percent", round2(percent / rows));
      }
      json.close();
      json.end_object();
    }
    json.close_array();
  }
  json.close();
  json.close();
  json.close();
  const auto length = json.finish();
  if (!length)
    return std::unexpected(json_error(length.error()));
  return *length;
}

} // namespace opensensor::stac
