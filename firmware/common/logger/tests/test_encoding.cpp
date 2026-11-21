// Integer encoding switch tests (#135). tools/test_logger_encoding.py builds
// this file twice, once as the default build and once with
// -DCONFIG_OS_LOGGER_DELTA_INTEGERS=1, the way sdkconfig.h defines a bool
// that is on. OS_EXPECT_DELTA tells each build what the switch must give.
//
// Usage: encoding-test <directory>
// Writes switch-none.parquet and switch-lz4.parquet through the real writer
// with oslogger::encoding::kIntegers, as os_logger.cpp passes it, and
// default-none.parquet and default-lz4.parquet with the writer default, so
// the Python side can prove an off switch is byte for byte the old file.
#include "lz4_codec.h"
#include "os_logger_encoding.h"
#include "parquet_writer.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#ifndef OS_EXPECT_DELTA
#error "build with -DOS_EXPECT_DELTA=0 or 1"
#endif

namespace encoding = oslogger::encoding;
using telemetry::IntegerEncoding;
using telemetry::PhysicalType;

namespace {
static_assert(encoding::kDeltaIntegers == (OS_EXPECT_DELTA != 0));
static_assert(encoding::kIntegers == (OS_EXPECT_DELTA
                                          ? IntegerEncoding::DeltaBinaryPacked
                                          : IntegerEncoding::Plain));
static_assert(encoding::integers(false) == IntegerEncoding::Plain);
static_assert(encoding::integers(true) == IntegerEncoding::DeltaBinaryPacked);
static_assert(std::string_view{encoding::name(IntegerEncoding::Plain)} ==
              "PLAIN");
static_assert(std::string_view{
                  encoding::name(IntegerEncoding::DeltaBinaryPacked)} ==
              "DELTA_BINARY_PACKED");
static_assert(std::string_view{
                  encoding::name(static_cast<IntegerEncoding>(1))} ==
              "unknown");

// One logger like row. Integers that step, jump to the type limits, wrap and
// go missing, next to a FLOAT that must stay PLAIN.
struct Row {
  int64_t utc_us;
  int64_t sequence;
  int64_t extreme;
  int32_t counter;
  int32_t wrap;
  float temperature;
  uint8_t present;
  uint8_t absent;
};

constexpr size_t kRows = 90;
constexpr size_t kGroups = 2;

void fill(std::array<Row, kRows> &rows, size_t group) {
  for (size_t r = 0; r < kRows; ++r) {
    const size_t i = group * kRows + r;
    rows[r] = {
        .utc_us = 1762646400000000 + static_cast<int64_t>(i) * 10000000 +
                  static_cast<int64_t>(i % 7) * 1000,
        .sequence = 4210 + static_cast<int64_t>(i),
        .extreme = i % 2 ? std::numeric_limits<int64_t>::max()
                         : std::numeric_limits<int64_t>::min(),
        .counter = static_cast<int32_t>(1650 + (i * 37) % 220),
        .wrap = i % 2 ? std::numeric_limits<int32_t>::max()
                      : std::numeric_limits<int32_t>::min(),
        .temperature = 21.0F + static_cast<float>(i % 13) / 4.0F,
        .present = static_cast<uint8_t>(i % 3 != 0),
        .absent = 0,
    };
  }
}

bool file_sink(void *context, const uint8_t *bytes, size_t length) {
  return fwrite(bytes, 1, length, static_cast<FILE *>(context)) == length;
}

// The writer and LZ4 state live for the whole run, as in the logger's PSRAM
// WriterState, never on this stack.
telemetry::Workspace workspace{};
telemetry::Lz4Workspace lz4{};
std::array<Row, kRows> rows{};

bool write(const std::string &path, bool compressed, bool use_switch) {
  const Row &first = rows.front();
  const std::array<telemetry::Column, 7> columns{{
      {.name = "event_time_utc_us",
       .type = PhysicalType::Int64,
       .values = &first.utc_us,
       .stride = sizeof(Row),
       .logical = telemetry::LogicalType::TimestampMicrosUtc,
       .field_id = 1},
      {.name = "sequence",
       .type = PhysicalType::Int64,
       .values = &first.sequence,
       .stride = sizeof(Row),
       .field_id = 2},
      {.name = "extreme",
       .type = PhysicalType::Int64,
       .values = &first.extreme,
       .stride = sizeof(Row),
       .valid = &first.present,
       .valid_stride = sizeof(Row),
       .field_id = 3},
      {.name = "counter",
       .type = PhysicalType::Int32,
       .values = &first.counter,
       .stride = sizeof(Row),
       .valid = &first.present,
       .valid_stride = sizeof(Row),
       .field_id = 4},
      {.name = "wrap",
       .type = PhysicalType::Int32,
       .values = &first.wrap,
       .stride = sizeof(Row),
       .field_id = 5},
      {.name = "all_null",
       .type = PhysicalType::Int64,
       .values = &first.sequence,
       .stride = sizeof(Row),
       .valid = &first.absent,
       .valid_stride = sizeof(Row),
       .field_id = 6},
      {.name = "temperature",
       .type = PhysicalType::Float,
       .values = &first.temperature,
       .stride = sizeof(Row),
       .field_id = 7},
  }};
  FILE *file = fopen(path.c_str(), "wb");
  if (!file)
    return false;
  const auto compression = lz4.configuration();
  const telemetry::Compression *codec = compressed ? &compression : nullptr;
  telemetry::Writer writer;
  fill(rows, 0);
  auto result = use_switch
                    ? writer.begin(file_sink, file, workspace, columns.data(),
                                   columns.size(), codec, encoding::kIntegers)
                    : writer.begin(file_sink, file, workspace, columns.data(),
                                   columns.size(), codec);
  for (size_t g = 0; g < kGroups && result.ok; ++g) {
    fill(rows, g);
    result = writer.row_group(kRows, 1);
  }
  if (result.ok)
    result = writer.finish(nullptr, 0, "encoding-test");
  const bool closed = fclose(file) == 0;
  if (!result.ok)
    fprintf(stderr, "%s: %s\n", path.c_str(), result.error);
  return result.ok && closed;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const std::string directory = argv[1];
  for (const bool compressed : {false, true}) {
    const std::string codec = compressed ? "lz4" : "none";
    if (!write(directory + "/switch-" + codec + ".parquet", compressed, true) ||
        !write(directory + "/default-" + codec + ".parquet", compressed, false))
      return 3;
  }
  printf("encoding switch: integers=%s\n", encoding::name(encoding::kIntegers));
  return 0;
}
