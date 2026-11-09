// Build through pixi (tools/test_parquet.py). Writes pairs of files through
// the exact firmware writer, one with IntegerEncoding::Plain and one with
// IntegerEncoding::DeltaBinaryPacked, from the same rows (#135).
//
// Usage: delta_fixture <directory>
//
// edge-<rows>-<plain|delta>-<none|lz4>.parquet hold the DELTA edge cases,
// constant and stepping values, wrapping INT32 and INT64 min and max deltas,
// deltas that need the full 32 or 64 bits, random values, a zero width
// miniblock before and after a wide one, nulls, an all null column, and FLOAT
// and GEOMETRY columns that must stay PLAIN.
//
// real-<groups>-<plain|delta>-<none|lz4>.parquet hold 90 row groups shaped
// like the CoreS3 telemetry dictionary, with its 81 columns in order and
// values that move the way the bench files do. One JSON line per real file
// on stdout gives its size for docs/shared/parquet-small-files.md.
//
// The binary also checks the writer contracts that need no reader, and
// returns nonzero when one fails.
#include "../firmware/common/src/lz4_codec.h"
#include "../firmware/common/src/parquet_writer.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {
using telemetry::IntegerEncoding;
using telemetry::LogicalType;
using telemetry::PhysicalType;

enum class Kind : uint8_t {
  Const,        // base
  Linear,       // base + step * row + noise
  Noise,        // base + noise
  Wrap,         // type min on even rows, type max on odd rows
  Full,         // deltas alternate type min and type max, from type min
  Random,       // every bit random
  ZeroThenWide, // rows 0 to 32 fall by kFall, then random 16 bit noise
  WideZeroWide, // noise, then rows 32 to 64 fall by kFall, then noise again
  Clock,        // hhmmss of base seconds + 10 * row
  Float,        // base + noise / 100 as float
  Point,        // pack_point_e7 near Cairo
};
enum class Nulls : uint8_t { Required, Every3, Run, All, Some, AllPresent };

struct Spec {
  const char *name;
  PhysicalType type;
  LogicalType logical;
  Kind kind;
  Nulls nulls;
  int64_t base;
  int64_t step;
  int64_t noise;
};

uint64_t mix(uint64_t x) { // splitmix64 finalizer
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

uint64_t random_bits(size_t column, size_t row) {
  return mix((static_cast<uint64_t>(column) << 32) ^ row);
}

// A symmetric integer in -noise .. noise.
int64_t symmetric(size_t column, size_t row, int64_t noise) {
  if (noise <= 0)
    return 0;
  const auto span = static_cast<uint64_t>(2 * noise + 1);
  return static_cast<int64_t>(random_bits(column, row) % span) - noise;
}

bool is_present(Nulls nulls, size_t row) {
  switch (nulls) {
  case Nulls::Required:
  case Nulls::AllPresent:
    return true;
  case Nulls::Every3:
    return row % 3 != 0;
  case Nulls::Run:
    return row % 3 != 0 && (row < 40 || row >= 80);
  case Nulls::All:
    return false;
  case Nulls::Some:
    return row % 17 != 5;
  }
  return true;
}

constexpr int64_t kFall = 1000000;

// The value of one row as an unsigned bit pattern of the column width.
uint64_t value(const Spec &spec, size_t column, size_t row) {
  const bool wide = spec.type != PhysicalType::Int32;
  const uint64_t low = wide ? uint64_t{1} << 63 : uint64_t{1} << 31;
  const uint64_t high = low - 1;
  const uint64_t mask = wide ? UINT64_MAX : UINT32_MAX;
  const auto as_bits = [&](int64_t v) {
    return static_cast<uint64_t>(v) & mask;
  };
  const auto r = static_cast<int64_t>(row);
  switch (spec.kind) {
  case Kind::Const:
    return as_bits(spec.base);
  case Kind::Linear:
    return as_bits(spec.base + spec.step * r +
                   symmetric(column, row, spec.noise));
  case Kind::Noise:
    return as_bits(spec.base + symmetric(column, row, spec.noise));
  case Kind::Wrap:
    return (row % 2 ? high : low) & mask;
  case Kind::Full: {
    // v0 = min, then add min and max in turn with wrap, so every block holds
    // deltas of both extremes and needs the full width.
    uint64_t v = low;
    for (size_t i = 1; i <= row; ++i)
      v = (v + (i % 2 ? low : high)) & mask;
    return v;
  }
  case Kind::Random:
    return random_bits(column, row) & mask;
  // The falling run gives deltas of exactly -kFall, below every other delta
  // of the block, so its miniblock is the block minimum and has width 0
  // while its neighbours are wide. That proves a zero width miniblock
  // before or between wide ones is written as no bytes.
  case Kind::ZeroThenWide:
    return row <= 32 ? as_bits(-kFall * r) : random_bits(column, row) & 0xffffU;
  case Kind::WideZeroWide:
    return row >= 32 && row <= 64 ? as_bits(-kFall * (r - 32))
                                  : random_bits(column, row) & 0xffffU;
  case Kind::Clock: {
    const int64_t seconds = (spec.base + 10 * r) % 86400;
    return as_bits(seconds / 3600 * 10000 + seconds / 60 % 60 * 100 +
                   seconds % 60);
  }
  case Kind::Float: {
    const auto f = static_cast<float>(spec.base) +
                   static_cast<float>(symmetric(column, row, spec.noise)) / 100;
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
  }
  case Kind::Point:
    return static_cast<uint64_t>(
        telemetry::pack_point_e7(312345678 + static_cast<int32_t>(row % 3),
                                 300444000 - static_cast<int32_t>(row % 2)));
  }
  return 0;
}

constexpr int64_t kI32Min = std::numeric_limits<int32_t>::min();
constexpr int64_t kUtc = 1762646400000000; // 2025-11-09T00:00:00Z in us
constexpr int64_t kMono = 123456789012;
constexpr int64_t kTenSeconds = 10000000;

using enum Kind;
using enum Nulls;
constexpr auto I32 = PhysicalType::Int32;
constexpr auto I64 = PhysicalType::Int64;
constexpr auto F32 = PhysicalType::Float;
constexpr auto WKB = PhysicalType::ByteArray;
constexpr auto None = LogicalType::None;

const Spec kEdges[] = {
    {"i32_const", I32, None, Const, Required, 7, 0, 0},
    {"i32_step", I32, None, Linear, Required, -100, 3, 0},
    {"i32_wrap", I32, None, Wrap, Required, 0, 0, 0},
    {"i32_full", I32, None, Full, Required, 0, 0, 0},
    {"i32_random", I32, None, Random, Required, 0, 0, 0},
    {"i32_zero_then_wide", I32, None, ZeroThenWide, Required, 0, 0, 0},
    {"i32_wide_zero_wide", I32, None, WideZeroWide, Required, 0, 0, 0},
    {"i32_nulls", I32, None, Linear, Run, kI32Min, 5, 2},
    {"i32_wrap_nulls", I32, None, Wrap, Every3, 0, 0, 0},
    {"i32_all_null", I32, None, Linear, All, 0, 1, 0},
    {"i32_all_present", I32, None, Noise, AllPresent, 1000, 0, 50},
    {"i32_uint8", I32, LogicalType::Uint8, Noise, Some, 128, 0, 127},
    {"i64_utc", I64, LogicalType::TimestampMicrosUtc, Linear, Required, kUtc,
     kTenSeconds, 3000},
    {"i64_wrap", I64, None, Wrap, Required, 0, 0, 0},
    {"i64_full", I64, None, Full, Required, 0, 0, 0},
    {"i64_random", I64, None, Random, Required, 0, 0, 0},
    {"i64_zero_then_wide", I64, None, ZeroThenWide, Required, 0, 0, 0},
    {"i64_wide_zero_wide", I64, None, WideZeroWide, Required, 0, 0, 0},
    {"i64_nulls", I64, None, Linear, Run, -kUtc, -kTenSeconds, 7},
    {"i64_wrap_nulls", I64, None, Wrap, Every3, 0, 0, 0},
    {"i64_all_null", I64, None, Linear, All, 0, 1, 0},
    {"i64_full_nulls", I64, None, Full, Some, 0, 0, 0},
    {"f32_temperature", F32, None, Float, Required, 21, 0, 300},
    {"f32_nulls", F32, None, Float, Every3, -5, 0, 1000},
    {"geometry", WKB, LogicalType::Geometry, Point, Some, 0, 0, 0},
};

// The CoreS3 dictionary (firmware/esp-idf-cores3/bringup/telemetry_fields.inc)
// in order, with values shaped like a bench window.
const Spec kReal[] = {
    {"schema_version", I32, None, Const, Required, 6, 0, 0},
    {"device_id", I64, None, Const, Required, 0x3c84279fd2a0, 0, 0},
    {"boot_id_hi", I64, None, Const, Required, 0x495ccef9f3eb8c59, 0, 0},
    {"boot_id_lo", I64, None, Const, Required, 0x17070c921c9baec8, 0, 0},
    {"sequence", I64, None, Linear, Required, 4210, 1, 0},
    {"monotonic_us", I64, None, Linear, Required, kMono, kTenSeconds, 2000},
    {"scheduled_us", I64, None, Linear, Required, kMono, kTenSeconds, 0},
    {"sample_jitter_us", I64, None, Noise, Required, 1500, 0, 1500},
    {"event_time_utc_us", I64, LogicalType::TimestampMicrosUtc, Linear,
     Required, kUtc, kTenSeconds, 2000},
    {"clock_status", I32, None, Const, Required, 2, 0, 0},
    {"pms_status", I32, None, Const, Required, 0, 0, 0},
    {"pms_age_ms", I64, None, Noise, Required, 450, 0, 450},
    {"pm1_cf1_ug_m3", I32, None, Noise, Required, 9, 0, 2},
    {"pm25_cf1_ug_m3", I32, None, Noise, Required, 14, 0, 3},
    {"pm10_cf1_ug_m3", I32, None, Noise, Required, 17, 0, 4},
    {"pm1_atmospheric_ug_m3", I32, None, Noise, Required, 9, 0, 2},
    {"pm25_atmospheric_ug_m3", I32, None, Noise, Required, 14, 0, 3},
    {"pm10_atmospheric_ug_m3", I32, None, Noise, Required, 17, 0, 4},
    {"particles_gt03_per_01l", I32, None, Noise, Required, 1650, 0, 220},
    {"particles_gt05_per_01l", I32, None, Noise, Required, 480, 0, 70},
    {"particles_gt10_per_01l", I32, None, Noise, Required, 90, 0, 20},
    {"particles_gt25_per_01l", I32, None, Noise, Required, 8, 0, 4},
    {"particles_gt50_per_01l", I32, None, Noise, Required, 2, 0, 2},
    {"particles_gt100_per_01l", I32, None, Noise, Required, 1, 0, 1},
    {"pms_firmware", I32, None, Const, Required, 145, 0, 0},
    {"pms_error", I32, None, Const, Required, 0, 0, 0},
    {"pms_frames", I64, None, Linear, Required, 81234, 10, 1},
    {"pms_checksum_errors", I64, None, Const, Required, 0, 0, 0},
    {"pms_length_errors", I64, None, Const, Required, 0, 0, 0},
    {"imu_fresh_mask", I32, None, Const, Required, 7, 0, 0},
    {"accel_x_g", F32, None, Float, Required, 0, 0, 3},
    {"accel_y_g", F32, None, Float, Required, 0, 0, 3},
    {"accel_z_g", F32, None, Float, Required, 1, 0, 3},
    {"gyro_x_dps", F32, None, Float, Required, 0, 0, 40},
    {"gyro_y_dps", F32, None, Float, Required, 0, 0, 40},
    {"gyro_z_dps", F32, None, Float, Required, 0, 0, 40},
    {"mag_x_raw", I32, None, Noise, Some, -420, 0, 30},
    {"mag_y_raw", I32, None, Noise, Some, 1210, 0, 30},
    {"mag_z_raw", I32, None, Noise, Some, -3050, 0, 30},
    {"imu_temperature_c", F32, None, Float, Required, 31, 0, 40},
    {"light_ch0_raw", I32, None, Noise, Required, 220, 0, 15},
    {"light_ch1_raw", I32, None, Noise, Required, 95, 0, 8},
    {"proximity_raw", I32, None, Noise, Required, 3, 0, 2},
    {"light_status", I32, None, Const, Required, 0, 0, 0},
    {"proximity_status", I32, None, Const, Required, 0, 0, 0},
    {"vbus_mv", I32, None, Noise, Required, 5010, 0, 25},
    {"battery_mv", I32, None, Linear, Required, 4120, 0, 4},
    {"battery_percent", I32, None, Const, Required, 87, 0, 0},
    {"charging_status", I32, None, Const, Required, 1, 0, 0},
    {"battery_current_ma", I32, None, Const, All, 0, 0, 0},
    {"external_5v_enabled", I32, None, Const, Required, 0, 0, 0},
    {"usb_output_enabled", I32, None, Const, Required, 1, 0, 0},
    {"rtc_date_yyyymmdd", I32, None, Const, Required, 20251109, 0, 0},
    {"rtc_time_hhmmss", I32, None, Clock, Required, 36000, 0, 0},
    {"rtc_read_ok", I32, None, Const, Required, 1, 0, 0},
    {"touch_points", I32, None, Const, Required, 0, 0, 0},
    {"touch_x_px", I32, None, Const, All, 0, 0, 0},
    {"touch_y_px", I32, None, Const, All, 0, 0, 0},
    {"heap_free_bytes", I64, None, Noise, Required, 151200, 0, 4000},
    {"heap_min_free_bytes", I64, None, Const, Required, 98112, 0, 0},
    {"psram_free_bytes", I64, None, Noise, Required, 7712000, 0, 3000},
    {"sd_total_bytes", I64, None, Const, Required, 31902400512, 0, 0},
    {"sd_used_bytes", I64, None, Linear, Required, 1048576000, 512, 0},
    {"rows_dropped", I64, None, Const, Required, 0, 0, 0},
    {"sample_deadlines_missed", I64, None, Const, Required, 0, 0, 0},
    {"storage_errors", I64, None, Const, Required, 0, 0, 0},
    {"files_finalized", I64, None, Const, Required, 46, 0, 0},
    {"last_write_us", I64, None, Noise, Required, 16000, 0, 14000},
    {"last_sync_us", I64, None, Noise, Required, 45000, 0, 20000},
    {"queue_high_water", I32, None, Const, Required, 2, 0, 0},
    {"ambient_temperature_c", F32, None, Float, All, 0, 0, 0},
    {"relative_humidity_percent", F32, None, Float, All, 0, 0, 0},
    {"clock_epoch", I32, None, Const, Required, 3, 0, 0},
    {"collection_completed_mono_us", I64, None, Linear, Required, kMono + 41000,
     kTenSeconds, 3000},
    {"pms_received_mono_us", I64, None, Linear, Required, kMono - 450000,
     kTenSeconds, 450000},
    {"clock_anchor_mono_us", I64, None, Const, Required, 4100000, 0, 0},
    {"clock_anchor_utc_us", I64, None, Const, Required, kUtc - 4100000, 0, 0},
    {"h3_cell_id", I64, None, Const, Required, 0x883e6a3a2bfffff, 0, 0},
    {"h3_resolution", I32, None, Const, Required, 8, 0, 0},
    {"location_source", I32, LogicalType::Uint8, Const, Required, 1, 0, 0},
    {"geometry", WKB, LogicalType::Geometry, Point, Required, 0, 0, 0},
};

// Column major buffers, each slot 8 bytes, little endian like the firmware.
struct Table {
  std::vector<telemetry::Column> columns;
  std::vector<std::vector<uint8_t>> values;
  std::vector<std::vector<uint8_t>> valid;
};

void fill(Table &table, const Spec *specs, size_t count, size_t rows,
          size_t first_row) {
  table.columns.resize(count);
  table.values.resize(count);
  table.valid.resize(count);
  for (size_t c = 0; c < count; ++c) {
    const Spec &spec = specs[c];
    auto &slots = table.values[c];
    auto &valid = table.valid[c];
    slots.assign(rows * 8 + 8, 0);
    valid.assign(rows + 1, 0);
    for (size_t r = 0; r < rows; ++r) {
      const uint64_t bits = value(spec, c, first_row + r);
      for (size_t b = 0; b < 8; ++b)
        slots[r * 8 + b] = static_cast<uint8_t>(bits >> (8 * b));
      valid[r] = is_present(spec.nulls, first_row + r) ? 1 : 0;
    }
    table.columns[c] = {
        .name = spec.name,
        .type = spec.type,
        .values = slots.data(),
        .stride = 8,
        .valid = spec.nulls == Required ? nullptr : valid.data(),
        .valid_stride = 1,
        .logical = spec.logical,
        .field_id = static_cast<int32_t>(c + 1),
    };
  }
}

bool file_sink(void *context, const uint8_t *bytes, size_t length) {
  return fwrite(bytes, 1, length, static_cast<FILE *>(context)) == length;
}
bool null_sink(void *, const uint8_t *, size_t) { return true; }

struct Written {
  bool ok;
  uint64_t bytes;
};

// Host scratch sized from the public bound, as a board would size its own.
Written write(const std::string &path, const Spec *specs, size_t count,
              size_t rows, size_t groups, bool lz4, IntegerEncoding integers) {
  Table table;
  fill(table, specs, count, rows, 0);
  telemetry::Workspace workspace{};
  telemetry::Lz4Workspace codec{};
  auto compression = codec.configuration();
  std::vector<uint8_t> raw(4 + rows * 3 +
                           telemetry::delta_binary_packed_bound(rows, 8) +
                           rows * (4 + 21));
  std::vector<uint8_t> encoded(LZ4_COMPRESSBOUND(raw.size()));
  compression.raw = raw.data();
  compression.raw_capacity = raw.size();
  compression.encoded = encoded.data();
  compression.encoded_capacity = encoded.size();
  FILE *file = fopen(path.c_str(), "wb");
  if (!file)
    return {false, 0};
  telemetry::Writer writer;
  auto result = writer.begin(file_sink, file, workspace, table.columns.data(),
                             count, lz4 ? &compression : nullptr, integers);
  for (size_t g = 0; g < groups && result.ok; ++g) {
    fill(table, specs, count, rows, g * rows);
    result = writer.row_group(rows);
  }
  if (result.ok)
    result = writer.finish(nullptr, 0, "delta-fixture");
  const bool closed = fclose(file) == 0;
  if (!result.ok)
    fprintf(stderr, "%s: %s\n", path.c_str(), result.error);
  return {result.ok && closed, result.bytes_written};
}

// Contracts that need no reader. Each returns a distinct exit code.
int contracts() {
  // The bound matches its documented formula at the sizes the tests use.
  if (telemetry::delta_binary_packed_bound(0, 8) != 2 + 1 + 1 + 10 ||
      telemetry::delta_binary_packed_bound(1, 4) != 2 + 1 + 1 + 10 ||
      telemetry::delta_binary_packed_bound(129, 8) !=
          2 + 1 + 2 + 10 + (10 + 4 + 4 * 32 * 8) ||
      telemetry::delta_binary_packed_bound(130, 4) !=
          2 + 1 + 2 + 10 + 2 * (10 + 4 + 4 * 32 * 4))
    return 20;
  // An unknown encoding is refused before any byte reaches the sink.
  Table table;
  fill(table, kEdges, std::size(kEdges), 3, 0);
  telemetry::Workspace workspace{};
  telemetry::Writer writer;
  if (writer
          .begin(null_sink, nullptr, workspace, table.columns.data(),
                 table.columns.size(), nullptr, static_cast<IntegerEncoding>(1))
          .ok ||
      writer.active())
    return 21;
  // The worst case INT64 page, 129 values whose 128 deltas need 64 bits, is
  // exactly the bound. One byte less LZ4 scratch than the page fails closed
  // with no byte written, and exactly enough succeeds.
  const Spec worst[] = {{"i64_full", I64, None, Full, Required, 0, 0, 0}};
  fill(table, worst, 1, 129, 0);
  const size_t page = telemetry::delta_binary_packed_bound(129, 8);
  std::vector<uint8_t> raw(page);
  std::vector<uint8_t> encoded(LZ4_COMPRESSBOUND(page));
  telemetry::Lz4Workspace codec{};
  for (const size_t capacity : {page - 1, page}) {
    auto compression = codec.configuration();
    compression.raw = raw.data();
    compression.raw_capacity = capacity;
    compression.encoded = encoded.data();
    compression.encoded_capacity = encoded.size();
    uint64_t accepted = 0;
    const auto counting = [](void *context, const uint8_t *, size_t size) {
      *static_cast<uint64_t *>(context) += size;
      return true;
    };
    const auto result = telemetry::write_parquet(
        counting, &accepted, table.columns.data(), 1, 129, workspace, nullptr,
        0, &compression, -1, nullptr, nullptr,
        IntegerEncoding::DeltaBinaryPacked);
    if (capacity < page && (result.ok || accepted))
      return 22;
    if (capacity == page && !result.ok)
      return 23;
  }
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  if (const int failed = contracts())
    return failed;
  const std::string directory = argv[1];
  struct Variant {
    const char *label;
    IntegerEncoding integers;
  };
  const Variant variants[] = {{"plain", IntegerEncoding::Plain},
                              {"delta", IntegerEncoding::DeltaBinaryPacked}};
  const size_t edge_rows[] = {1, 2, 3, 32, 33, 90, 128, 129, 130, 257, 1000};
  for (const size_t rows : edge_rows)
    for (const Variant &variant : variants)
      for (const bool lz4 : {false, true}) {
        const std::string path = directory + "/edge-" + std::to_string(rows) +
                                 "-" + variant.label + "-" +
                                 (lz4 ? "lz4" : "none") + ".parquet";
        if (!write(path, kEdges, std::size(kEdges), rows, 1, lz4,
                   variant.integers)
                 .ok)
          return 4;
      }
  for (const size_t groups : {1, 4})
    for (const Variant &variant : variants)
      for (const bool lz4 : {false, true}) {
        const std::string path = directory + "/real-" + std::to_string(groups) +
                                 "-" + variant.label + "-" +
                                 (lz4 ? "lz4" : "none") + ".parquet";
        const auto written = write(path, kReal, std::size(kReal), 90, groups,
                                   lz4, variant.integers);
        if (!written.ok)
          return 5;
        printf("{\"file\": \"%s\", \"groups\": %zu, \"encoding\": \"%s\", "
               "\"codec\": \"%s\", \"bytes\": %llu}\n",
               path.substr(directory.size() + 1).c_str(), groups, variant.label,
               lz4 ? "lz4" : "none",
               static_cast<unsigned long long>(written.bytes));
      }
  return 0;
}
