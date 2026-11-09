// Build through pixi; uses exactly the firmware writer on the host.
// Usage: parquet_fixture output.parquet [rows=90] [columns=8] [none|lz4]
//        [row_groups=1] [plain|delta]
// delta writes INT32 and INT64 columns as DELTA_BINARY_PACKED (#135).
// rows is per row group; row group g holds global rows g*rows .. g*rows+rows-1
// and the row buffer is refilled between groups, as the logger does.
#include "../firmware/common/src/lz4_codec.h"
#include "../firmware/common/src/parquet_writer.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace {
struct Row {
  int64_t time_ms;
  int32_t counter;
  float temperature;
  int64_t signed_extreme;
  uint8_t alternating;
  uint8_t absent;
  uint8_t present;
  float positive_zero;
};

bool file_sink(void *context, const uint8_t *bytes, size_t length) {
  return fwrite(bytes, 1, length, static_cast<FILE *>(context)) == length;
}

struct FailingSink {
  size_t calls = 0;
};

bool fail_sink(void *context, const uint8_t *, size_t) {
  auto &state = *static_cast<FailingSink *>(context);
  return ++state.calls == 1;
}

void fill(std::vector<Row> &rows, size_t count, size_t first_index) {
  for (size_t r = 0; r < count; ++r) {
    const size_t i = first_index + r;
    rows[r] = {1700000000000LL + static_cast<int64_t>(i) * 10000,
               i % 2 ? std::numeric_limits<int32_t>::max()
                     : std::numeric_limits<int32_t>::min(),
               static_cast<float>(i) * 0.25F - 10.0F,
               i % 2 ? std::numeric_limits<int64_t>::max()
                     : std::numeric_limits<int64_t>::min(),
               static_cast<uint8_t>(i % 2),
               0,
               7,
               0.0F};
  }
  // Only the first row group carries the special float values, so a second
  // group has plain finite bounds and no NaN.
  if (count > 4 && first_index == 0) {
    rows[0].temperature = -0.0F;
    rows[1].temperature = std::numeric_limits<float>::infinity();
    rows[2].temperature = -std::numeric_limits<float>::infinity();
    rows[3].temperature = std::numeric_limits<float>::quiet_NaN();
  }
}
} // namespace

int main(int argc, char **argv) {
  if (argc < 2 || argc > 7)
    return 2;
  const size_t count = argc > 2 ? strtoul(argv[2], nullptr, 10) : 90;
  const size_t column_count = argc > 3 ? strtoul(argv[3], nullptr, 10) : 8;
  const bool compressed = argc > 4 && strcmp(argv[4], "lz4") == 0;
  const size_t groups = argc > 5 ? strtoul(argv[5], nullptr, 10) : 1;
  const auto integers = argc > 6 && strcmp(argv[6], "delta") == 0
                            ? telemetry::IntegerEncoding::DeltaBinaryPacked
                            : telemetry::IntegerEncoding::Plain;
  if (count > telemetry::kMaxRows || column_count < 8 ||
      column_count > telemetry::kMaxColumns || !groups ||
      groups > telemetry::kMaxRowGroups)
    return 2;
  std::vector<Row> rows(count ? count : 1);
  fill(rows, count, 0);
  const Row &first = rows.front();
  using telemetry::PhysicalType;
  std::array<telemetry::Column, telemetry::kMaxColumns> columns;
  columns[0] = {"time_ms", PhysicalType::Int64, &first.time_ms, sizeof(Row)};
  columns[1] = {"counter", PhysicalType::Int32, &first.counter, sizeof(Row)};
  columns[2] = {"temperature", PhysicalType::Float, &first.temperature,
                sizeof(Row)};
  columns[3] = {"optional_counter", PhysicalType::Int32, &first.counter,
                sizeof(Row),        &first.alternating,  sizeof(Row)};
  columns[4] = {"all_null",  PhysicalType::Float, &first.temperature,
                sizeof(Row), &first.absent,       sizeof(Row)};
  columns[5] = {"signed_extreme", PhysicalType::Int64, &first.signed_extreme,
                sizeof(Row)};
  columns[6] = {"optional_extreme", PhysicalType::Int64, &first.signed_extreme,
                sizeof(Row),        &first.alternating,  sizeof(Row)};
  columns[7] = {"all_present", PhysicalType::Float, &first.temperature,
                sizeof(Row),   &first.present,      sizeof(Row)};
  // time_ms doubles as a TIMESTAMP(MICROS, UTC) leaf; the value is just an
  // INT64 to the writer, the annotation is what readers see.
  columns[0].logical = telemetry::LogicalType::TimestampMicrosUtc;
  std::array<std::array<char, 32>, telemetry::kMaxColumns> names{};
  for (size_t i = 8; i < column_count; ++i) {
    snprintf(names[i].data(), names[i].size(), "extra_%02zu", i);
    columns[i] = {names[i].data(), PhysicalType::Float, &first.temperature,
                  sizeof(Row)};
  }
  if (column_count > 8)
    columns[8] = {"positive_zero", PhysicalType::Float, &first.positive_zero,
                  sizeof(Row)};
  // Every leaf carries a stable Iceberg field id; the boards use the 1-based
  // position in their append-only field table, and so does this fixture.
  for (size_t i = 0; i < column_count; ++i)
    columns[i].field_id = static_cast<int32_t>(i + 1);
  telemetry::Workspace workspace{};
  telemetry::Lz4Workspace lz4{};
  // Fixed-capacity adapter checks include incompressible/empty/short blocks.
  // These are host-only scratch arrays, not firmware task stack allocations.
  std::array<uint8_t, 1024> decoded{};
  auto bounded = lz4.configuration();
  uint32_t random = 0x6b40;
  for (size_t length = 0; length <= 1024; ++length) {
    for (size_t i = 0; i < length; ++i) {
      random = random * 1664525U + 1013904223U;
      bounded.raw[i] = static_cast<uint8_t>(random >> 24);
    }
    const size_t packed =
        bounded.compress(bounded.context, bounded.raw, length, bounded.encoded,
                         bounded.encoded_capacity);
    if (!packed || packed > bounded.encoded_capacity ||
        LZ4_decompress_safe(
            reinterpret_cast<const char *>(bounded.encoded),
            reinterpret_cast<char *>(decoded.data()), static_cast<int>(packed),
            static_cast<int>(length)) != static_cast<int>(length) ||
        std::memcmp(decoded.data(), bounded.raw, length) != 0)
      return 9;
  }
  auto compression = lz4.configuration();
  // Large fixtures exercise format limits on the host only. Firmware remains
  // fixed at telemetry::kLz4RawBytes page scratch and 90 rows.
  std::vector<uint8_t> raw(count * 10 + 4);
  std::vector<uint8_t> encoded(LZ4_COMPRESSBOUND(raw.size()));
  compression.raw = raw.data();
  compression.raw_capacity = raw.size();
  compression.encoded = encoded.data();
  compression.encoded_capacity = encoded.size();
  const telemetry::KeyValue metadata[] = {
      {"sample_interval_ms", "10000"},
      {"fixture", "firmware writer interoperability"}};
  FILE *file = fopen(argv[1], "wb");
  if (!file)
    return 3;
  telemetry::Writer writer;
  telemetry::Result result;
  {
    // The firmware builds this struct on the stack of the function that
    // opens the file; row groups are written minutes later. The writer must
    // therefore copy it: clobber the caller's copy right after begin().
    telemetry::Compression temporary = compression;
    result =
        writer.begin(file_sink, file, workspace, columns.data(), column_count,
                     compressed ? &temporary : nullptr, integers);
    memset(&temporary, 0xA5, sizeof(temporary));
  }
  for (size_t g = 0; g < groups && result.ok && count; ++g) {
    fill(rows, count, g * count);
    result = writer.row_group(count, 0); // time_ms ascends within a group
  }
  if (result.ok)
    result = writer.finish(metadata, 2, "fixture");
  const bool closed = fclose(file) == 0;
  if (!result.ok || !closed) {
    fprintf(stderr, "write failed: %s\n",
            result.error ? result.error : "close");
    return 4;
  }
  fill(rows, count, 0);
  FailingSink failure;
  const auto failed = telemetry::write_parquet(
      fail_sink, &failure, columns.data(), column_count, count, workspace,
      metadata, 2, compressed ? &compression : nullptr);
  if (result.bytes_written > 512 && (failed.ok || failure.calls != 2))
    return 5;
  const auto invalid =
      telemetry::write_parquet(fail_sink, &failure, columns.data(),
                               telemetry::kMaxColumns + 1, count, workspace);
  if (invalid.ok || invalid.bytes_written)
    return 6;
  if (count) {
    auto broken = compression;
    broken.compress = [](void *, const uint8_t *, size_t, uint8_t *,
                         size_t) -> size_t { return 0; };
    failure.calls = 0;
    const auto codec_failed = telemetry::write_parquet(
        fail_sink, &failure, columns.data(), column_count, count, workspace,
        metadata, 2, &broken);
    if (codec_failed.ok || codec_failed.bytes_written != 4 ||
        failure.calls != 1)
      return 8;
    compression.raw_capacity = 0;
    const auto rejected = telemetry::write_parquet(
        fail_sink, &failure, columns.data(), column_count, count, workspace,
        metadata, 2, &compression);
    if (rejected.ok || rejected.bytes_written)
      return 7;
    // A logical annotation on the wrong physical type is refused up front.
    auto wrong = columns;
    wrong[1].logical = telemetry::LogicalType::TimestampMicrosUtc;
    failure.calls = 0;
    const auto mismatched = telemetry::write_parquet(
        fail_sink, &failure, wrong.data(), column_count, count, workspace);
    if (mismatched.ok || mismatched.bytes_written || failure.calls)
      return 10;
    // Missing and duplicate field ids are refused before any byte is written.
    for (const int32_t bad_id : {0, -1, 2}) {
      auto ids = columns;
      ids[column_count - 1].field_id = bad_id;
      failure.calls = 0;
      const auto refused = telemetry::write_parquet(
          fail_sink, &failure, ids.data(), column_count, count, workspace);
      if (refused.ok || refused.bytes_written || failure.calls)
        return 12;
    }
    // Row-group capacity is enforced, and a failed writer stays inert.
    telemetry::Writer capacity;
    failure.calls = 0;
    auto step =
        capacity.begin([](void *, const uint8_t *, size_t) { return true; },
                       nullptr, workspace, columns.data(), column_count);
    for (size_t g = 0; g < telemetry::kMaxRowGroups && step.ok; ++g)
      step = capacity.row_group(1);
    if (!step.ok || capacity.row_groups() != telemetry::kMaxRowGroups ||
        capacity.row_group(1).ok || capacity.finish().ok || capacity.active())
      return 11;
  }
  printf("rows=%zu columns=%zu row_groups=%zu bytes=%llu workspace=%zu\n",
         count, column_count, count ? groups : 0,
         static_cast<unsigned long long>(result.bytes_written),
         sizeof(workspace));
  return 0;
}
