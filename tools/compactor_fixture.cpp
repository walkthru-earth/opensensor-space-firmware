// Host fixture for tools/compact_archive.py and its test. Writes one boot of
// one Thing as consecutive UTC rotation windows in the dated Hive layout the
// storage worker uses, with exactly the firmware writer and the board field
// table, so the compactor reads the same bytes a card or phone archive holds.
//
// Build with -DOS_FIELDS_INC="\"<board>/telemetry_fields.inc\"".
// Usage:
//   compactor_fixture root none|lz4 thing_uuid boot_hex32 device_id
//                     start_utc_us files rotation_s cell_hex|none
//                     lon_e7 lat_e7 seed footer.json
// Rows are 10 s apart from start_utc_us (whole seconds). Each file holds the
// rows of one rotation window (600, 900, 1800 or 3600 s) in row groups of at
// most 90 rows, so a boot that starts mid window opens with a short file, as
// on the board. Writes root/thing=<uuid>/year=YYYY/month=MM/day=DD/
// data_HHMM_<boot>_<first>-<last>-0.parquet and prints one JSON line with the
// file count, rows and bytes.
//
// Columns follow prepare_columns() in the shared logger, as in
// partial_rebuild_fixture.cpp. Values are deterministic for a seed and shaped
// like the 2026-10-26 CoreS3 Thing file (random walks for sensors, slow
// counters, constant codes, `unavailable` fields always null), so size
// measurements are near real ones. The `opensensor` footer value comes from
// footer.json verbatim and the `geo` value is built here like
// os_logger_footer.h build_geo(). This is not the full board footer.
#include "../firmware/common/src/lz4_codec.h"
#include "../firmware/common/src/parquet_writer.h"
#include "../firmware/common/src/telemetry_logical.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifndef OS_FIELDS_INC
#error "define OS_FIELDS_INC as the quoted path of a board telemetry_fields.inc"
#endif

namespace {
using telemetry::Column;
using telemetry::LogicalType;
using telemetry::PhysicalType;

struct FieldDef {
  const char *name;
  PhysicalType type;
  const char *unit;
  const char *validity;
  int decimals;
};

constexpr FieldDef kFields[] = {
#define FIELD(name, type, procedure, unit, validity, live, decimals)           \
  {#name, PhysicalType::type, unit, #validity, decimals},
#include OS_FIELDS_INC
#undef FIELD
};
constexpr std::size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);
static_assert(kFieldCount <= telemetry::kMaxColumns);
constexpr std::int64_t kPeriodUs = 10'000'000;
constexpr std::size_t kMaxGroupRows = 90; // cadence::kMaxBatchRows

struct Row {
  std::array<std::uint64_t, kFieldCount> data{};
  std::array<std::uint8_t, kFieldCount> valid{};
};

struct Rng {
  std::uint64_t state;
  std::uint64_t next() {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  // Uniform in [-1, 1].
  double unit() { return static_cast<double>(next() >> 11) * 0x1.0p-52 - 1.0; }
  std::int64_t below(std::int64_t bound) {
    return static_cast<std::int64_t>(next() %
                                     static_cast<std::uint64_t>(bound));
  }
};

bool file_sink(void *context, const std::uint8_t *bytes, std::size_t length) {
  return std::fwrite(bytes, 1, length, static_cast<FILE *>(context)) == length;
}

bool is(const char *a, const char *b) { return std::strcmp(a, b) == 0; }
bool has(const char *a, const char *b) { return std::strstr(a, b) != nullptr; }

void put32(Row &row, std::size_t field, std::int64_t value) {
  // The writer reads the low four bytes (host LE).
  row.data[field] =
      static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
}

void put_float(Row &row, std::size_t field, double value) {
  const auto narrow = static_cast<float>(value);
  std::uint32_t bits;
  std::memcpy(&bits, &narrow, sizeof(bits));
  row.data[field] = bits;
}

// Per field walk state, seeded so every Thing has its own level.
struct Walk {
  double level = 0;
  double step = 0;
  double low = 0;
  double high = 0;
  double quantum = 1;
};

Walk walk_for(const FieldDef &field, Rng &rng) {
  const auto u = field.unit;
  Walk w{};
  if (is(u, "g")) {
    w = {.level = has(field.name, "_z") ? 0.99 : 0.02,
         .step = 0.0005,
         .low = -2,
         .high = 2,
         .quantum = 1.0 / 4096};
  } else if (is(u, "deg/s")) {
    w = {.level = 0,
         .step = 0.06,
         .low = -5,
         .high = 5,
         .quantum = 1.0 / 16.384};
  } else if (is(u, "degC")) {
    w = {.level = 24 + 6 * rng.unit(),
         .step = 0.05,
         .low = -10,
         .high = 50,
         .quantum = field.type == PhysicalType::Float ? 1.0 / 200 : 1};
  } else if (is(u, "%")) {
    w = {.level = 45 + 10 * rng.unit(),
         .step = 0.2,
         .low = 0,
         .high = 100,
         .quantum = field.type == PhysicalType::Float ? 1.0 / 100 : 1};
  } else if (is(u, "ug/m3")) {
    w = {.level = 25 + 10 * rng.unit(),
         .step = 0.8,
         .low = 0,
         .high = 500,
         .quantum = field.type == PhysicalType::Float ? 0.1 : 1};
  } else if (is(u, "count/0.1L")) {
    w = {.level = 1500 + 500 * rng.unit(), .step = 40, .low = 0, .high = 65535};
  } else if (is(u, "ppm")) {
    w = {.level = 600 + 100 * rng.unit(), .step = 6, .low = 400, .high = 5000};
  } else if (is(u, "mV")) {
    w = {.level = 4200 + 400 * rng.unit(), .step = 8, .low = 0, .high = 5500};
  } else if (is(u, "raw_count")) {
    w = {.level = 1000 * rng.unit(), .step = 6, .low = -32768, .high = 32767};
  } else if (is(u, "byte")) {
    w = {
        .level = 45000 + 4000 * rng.unit(), .step = 64, .low = 0, .high = 1e12};
  } else {
    w = {.level = 50 + 40 * rng.unit(),
         .step = 1,
         .low = -1e9,
         .high = 1e9,
         .quantum = field.type == PhysicalType::Float ? 0.01 : 1};
  }
  return w;
}

double advance(Walk &w, Rng &rng) {
  w.level += w.step * rng.unit();
  if (w.level < w.low)
    w.level = w.low;
  if (w.level > w.high)
    w.level = w.high;
  return std::round(w.level / w.quantum) * w.quantum;
}

struct Plan {
  bool compressed;
  const char *root;
  const char *thing;
  const char *boot;
  std::int64_t device_id;
  std::int64_t start_utc_us;
  std::size_t files;
  std::int64_t rotation_s;
  bool located;
  std::int64_t cell;
  std::int32_t lon_e7;
  std::int32_t lat_e7;
  std::uint64_t seed;
  std::string footer;
};

struct State {
  Rng rng;
  std::array<Walk, kFieldCount> walks{};
  std::array<std::int64_t, kFieldCount> constant{};
  std::int64_t boot_hi = 0;
  std::int64_t boot_lo = 0;
  std::int64_t sd_used = 147456;
  std::int64_t frames = 0;
  std::int64_t files_finalized = 0;
  std::int64_t last_write_us = 0;
};

std::int64_t parse_hex64(const char *text) {
  return static_cast<std::int64_t>(std::strtoull(text, nullptr, 16));
}

// Fills global row `r` of this boot.
void fill(Row &row, std::size_t r, const Plan &plan, State &state) {
  const auto offset = static_cast<std::int64_t>(r) * kPeriodUs;
  const auto jitter = 1000 + state.rng.below(15000);
  const auto mono = 1'873'739 + offset + jitter;
  const auto utc = plan.start_utc_us + offset + jitter;
  const std::time_t seconds = static_cast<std::time_t>(utc / 1'000'000);
  std::tm tm{};
  gmtime_r(&seconds, &tm);
  state.frames += 10 + state.rng.below(3);
  for (std::size_t i = 0; i < kFieldCount; ++i) {
    const auto &field = kFields[i];
    const auto u = field.unit;
    row.valid[i] = 1;
    if (is(field.validity, "unavailable")) {
      row.valid[i] = 0;
      row.data[i] = 0;
      continue;
    }
    const auto logical = telemetry::logical_for(field.type, u);
    if (is(field.name, "schema_version")) {
      put32(row, i, 6);
    } else if (is(field.name, "device_id")) {
      row.data[i] = static_cast<std::uint64_t>(plan.device_id);
    } else if (is(field.name, "boot_id_hi")) {
      row.data[i] = static_cast<std::uint64_t>(state.boot_hi);
    } else if (is(field.name, "boot_id_lo")) {
      row.data[i] = static_cast<std::uint64_t>(state.boot_lo);
    } else if (is(field.name, "sequence")) {
      row.data[i] = r;
    } else if (is(field.name, "scheduled_us")) {
      row.data[i] = static_cast<std::uint64_t>(1'873'739 + offset);
    } else if (is(field.name, "sample_jitter_us")) {
      row.data[i] = static_cast<std::uint64_t>(jitter);
    } else if (is(field.name, "clock_anchor_mono_us")) {
      row.data[i] = 1'873'527;
    } else if (is(field.name, "clock_anchor_utc_us")) {
      row.data[i] = static_cast<std::uint64_t>(plan.start_utc_us - 9'000'000);
    } else if (is(field.name, "last_write_us") ||
               is(field.name, "last_sync_us")) {
      row.data[i] = static_cast<std::uint64_t>(state.last_write_us);
    } else if (is(field.name, "files_finalized")) {
      row.data[i] = static_cast<std::uint64_t>(state.files_finalized);
    } else if (is(field.name, "sd_used_bytes")) {
      row.data[i] = static_cast<std::uint64_t>(state.sd_used);
    } else if (is(field.name, "sd_total_bytes")) {
      row.data[i] = 31'441'764'352ULL;
    } else if (has(field.name, "frames")) {
      row.data[i] = static_cast<std::uint64_t>(state.frames);
    } else if (is(u, "us_since_unix_epoch")) {
      row.data[i] = static_cast<std::uint64_t>(utc);
    } else if (is(u, "us")) {
      row.data[i] = static_cast<std::uint64_t>(mono + state.rng.below(8000));
    } else if (is(u, "ms")) {
      row.data[i] = static_cast<std::uint64_t>(state.rng.below(1000));
    } else if (is(u, "YYYYMMDD")) {
      put32(row, i,
            (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday);
    } else if (is(u, "hhmmss")) {
      put32(row, i, tm.tm_hour * 10000 + tm.tm_min * 100 + tm.tm_sec);
    } else if (is(u, "h3_index")) {
      row.valid[i] = plan.located;
      row.data[i] = static_cast<std::uint64_t>(plan.cell);
    } else if (is(u, "resolution_0_15")) {
      row.valid[i] = plan.located;
      put32(row, i, 8);
    } else if (logical == LogicalType::Uint8) {
      row.valid[i] = plan.located;
      put32(row, i, 1);
    } else if (field.type == PhysicalType::ByteArray) {
      row.valid[i] = plan.located;
      row.data[i] = static_cast<std::uint64_t>(
          telemetry::pack_point_e7(plan.lon_e7, plan.lat_e7));
    } else if (is(u, "code") || is(u, "boolean") || is(u, "bitmask") ||
               is(u, "count") || is(u, "1")) {
      // Codes and flags hold still, rare counters stay at their start.
      row.data[i] = 0;
      if (field.type == PhysicalType::Int32)
        put32(row, i, state.constant[i]);
      else
        row.data[i] = static_cast<std::uint64_t>(state.constant[i]);
    } else if (field.type == PhysicalType::Float) {
      put_float(row, i, advance(state.walks[i], state.rng));
    } else {
      const auto value =
          static_cast<std::int64_t>(advance(state.walks[i], state.rng));
      if (field.type == PhysicalType::Int32)
        put32(row, i, value);
      else
        row.data[i] = static_cast<std::uint64_t>(value);
    }
  }
}

// GeoParquet 2.0.0 `geo` value as os_logger_footer.h build_geo() writes it,
// bbox only when a row is located.
std::string geo_json(const std::optional<telemetry::PointBounds> &bounds) {
  std::string out =
      R"({"version":"2.0.0","primary_column":"geometry","columns":{"geometry":{"encoding":"WKB","geometry_types":["Point"])";
  if (bounds) {
    const auto e7 = [](std::int32_t value) {
      std::array<char, 16> text{};
      const auto sign = value < 0 ? "-" : "";
      const auto magnitude = static_cast<std::uint32_t>(
          value < 0 ? -static_cast<std::int64_t>(value) : value);
      std::snprintf(text.data(), text.size(), "%s%u.%07u", sign,
                    magnitude / 10'000'000U, magnitude % 10'000'000U);
      return std::string(text.data());
    };
    out += ",\"bbox\":[" + e7(bounds->xmin_e7) + "," + e7(bounds->ymin_e7) +
           "," + e7(bounds->xmax_e7) + "," + e7(bounds->ymax_e7) + "]";
  }
  out += "}}}";
  return out;
}

int index_of(const char *name) {
  for (std::size_t i = 0; i < kFieldCount; ++i)
    if (is(kFields[i].name, name))
      return static_cast<int>(i);
  return -1;
}

enum class ReadError { Open, Close, Empty };

[[nodiscard]] std::expected<std::string, ReadError>
read_text(const char *path) {
  FILE *file = std::fopen(path, "rb");
  if (!file)
    return std::unexpected(ReadError::Open);
  std::string out;
  std::array<char, 4096> buffer{};
  std::size_t got = 0;
  while ((got = std::fread(buffer.data(), 1, buffer.size(), file)) > 0)
    out.append(buffer.data(), got);
  if (std::fclose(file) != 0)
    return std::unexpected(ReadError::Close);
  if (out.empty())
    return std::unexpected(ReadError::Empty);
  return out;
}

// Writes one file for the rotation window that holds global row `first`, in
// row groups of at most kMaxGroupRows. Returns the row after the window.
struct FileResult {
  std::size_t next_row;
  unsigned long long bytes;
};

enum class WriteError { Directory, Open, Writer };

[[nodiscard]] std::expected<FileResult, WriteError>
write_window(std::size_t first, const Plan &plan, State &state,
             std::vector<Row> &rows, std::span<const Column> columns,
             telemetry::Workspace &workspace, telemetry::Lz4Workspace &lz4,
             int sequence) {
  const auto nominal = [&](std::size_t r) {
    return plan.start_utc_us + static_cast<std::int64_t>(r) * kPeriodUs;
  };
  const std::int64_t window_us = plan.rotation_s * 1'000'000;
  const std::int64_t window_start = nominal(first) / window_us * window_us;
  const std::int64_t window_end = window_start + window_us;
  std::size_t last = first;
  while (nominal(last + 1) < window_end)
    ++last;
  const std::time_t seconds =
      static_cast<std::time_t>(window_start / 1'000'000);
  std::tm tm{};
  gmtime_r(&seconds, &tm);
  std::array<char, 64> date{};
  std::snprintf(date.data(), date.size(), "/year=%04d/month=%02d/day=%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  const std::string partition =
      std::string(plan.root) + "/thing=" + plan.thing + date.data();
  std::error_code ec;
  std::filesystem::create_directories(partition, ec);
  if (ec)
    return std::unexpected(WriteError::Directory);
  std::array<char, 128> name{};
  std::snprintf(name.data(), name.size(), "/data_%02d%02d_%s_%zu-%zu-0.parquet",
                tm.tm_hour, tm.tm_min, plan.boot, first, last);
  const std::string path = partition + name.data();
  FILE *file = std::fopen(path.c_str(), "wb");
  if (!file)
    return std::unexpected(WriteError::Open);
  const auto compression = lz4.configuration();
  telemetry::Writer writer;
  auto result =
      writer.begin(file_sink, file, workspace, columns.data(), columns.size(),
                   plan.compressed ? &compression : nullptr);
  for (std::size_t r = first; r <= last && result.ok;) {
    std::size_t count = 0;
    for (; count < kMaxGroupRows && r <= last; ++count, ++r)
      fill(rows[count], r, plan, state);
    result = writer.row_group(count, sequence);
  }
  const int geometry = index_of("geometry");
  const std::string geo =
      geometry < 0 ? std::string() : geo_json(writer.point_bounds(geometry));
  const telemetry::KeyValue metadata[] = {{"opensensor", plan.footer.c_str()},
                                          {"geo", geo.c_str()}};
  if (result.ok)
    result = writer.finish(metadata, geometry < 0 ? 1 : 2, "compactor-fixture",
                           "opensensor-space-firmware fixture");
  const bool closed = std::fclose(file) == 0;
  if (!result.ok || !closed) {
    std::fprintf(stderr, "write failed: %s\n",
                 result.error ? result.error : "close");
    return std::unexpected(WriteError::Writer);
  }
  state.files_finalized += 1;
  state.last_write_us =
      1'873'739 + static_cast<std::int64_t>(last + 1) * kPeriodUs;
  state.sd_used += static_cast<std::int64_t>(result.bytes_written);
  return FileResult{.next_row = last + 1, .bytes = result.bytes_written};
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 14)
    return 2;
  Plan plan{
      .compressed = is(argv[2], "lz4"),
      .root = argv[1],
      .thing = argv[3],
      .boot = argv[4],
      .device_id = std::strtoll(argv[5], nullptr, 10),
      .start_utc_us = std::strtoll(argv[6], nullptr, 10),
      .files = std::strtoul(argv[7], nullptr, 10),
      .rotation_s = std::strtoll(argv[8], nullptr, 10),
      .located = !is(argv[9], "none"),
      .cell = is(argv[9], "none") ? 0 : parse_hex64(argv[9]),
      .lon_e7 = static_cast<std::int32_t>(std::strtol(argv[10], nullptr, 10)),
      .lat_e7 = static_cast<std::int32_t>(std::strtol(argv[11], nullptr, 10)),
      .seed = std::strtoull(argv[12], nullptr, 10),
      .footer = {},
  };
  const int sequence = index_of("sequence");
  auto footer = read_text(argv[13]);
  const bool rotation_ok = plan.rotation_s == 600 || plan.rotation_s == 900 ||
                           plan.rotation_s == 1800 || plan.rotation_s == 3600;
  if (!plan.files || !rotation_ok || std::strlen(plan.boot) != 32 ||
      sequence < 0 || plan.start_utc_us % 1'000'000 != 0 || !footer)
    return 2;
  plan.footer = std::move(*footer);

  State state{.rng = {.state = plan.seed}};
  state.boot_hi = parse_hex64(std::string(plan.boot, 16).c_str());
  state.boot_lo = parse_hex64(plan.boot + 16);
  for (std::size_t i = 0; i < kFieldCount; ++i) {
    state.walks[i] = walk_for(kFields[i], state.rng);
    const auto u = kFields[i].unit;
    state.constant[i] = is(u, "code")      ? 1 + state.rng.below(3)
                        : is(u, "boolean") ? 1
                        : is(u, "bitmask") ? 7
                                           : 0;
  }

  std::vector<Row> rows(kMaxGroupRows);
  std::array<Column, kFieldCount> columns{};
  for (std::size_t i = 0; i < kFieldCount; ++i) {
    const auto &field = kFields[i];
    columns[i] = Column{field.name,
                        field.type,
                        &rows[0].data[i],
                        sizeof(Row),
                        &rows[0].valid[i],
                        sizeof(Row),
                        telemetry::logical_for(field.type, field.unit),
                        static_cast<std::int32_t>(i + 1)};
  }
  telemetry::Workspace workspace{};
  telemetry::Lz4Workspace lz4{};
  unsigned long long total_bytes = 0;
  std::size_t row = 0;
  for (std::size_t f = 0; f < plan.files; ++f) {
    const auto written =
        write_window(row, plan, state, rows, columns, workspace, lz4, sequence);
    if (!written)
      return 3 + static_cast<int>(std::to_underlying(written.error()));
    total_bytes += written->bytes;
    row = written->next_row;
  }
  std::printf("{\"columns\":%zu,\"files\":%zu,\"rows\":%zu,\"bytes\":%llu}\n",
              kFieldCount, plan.files, row, total_bytes);
  return 0;
}
