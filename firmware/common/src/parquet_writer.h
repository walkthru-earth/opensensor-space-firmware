#pragma once

#include <optional>
#include <stddef.h>
#include <stdint.h>

namespace telemetry {

constexpr size_t kMaxColumns = 96;
// Per row group. Several row groups may share one file (kMaxRowGroups).
constexpr size_t kMaxRows = 65536;
constexpr size_t kMaxRowGroups = 8;

enum class PhysicalType : uint8_t {
  Int32 = 1,
  Int64 = 2,
  Float = 4,
  ByteArray = 6
};
enum class Codec : uint8_t { Uncompressed = 0, Lz4Raw = 7 };
// Value encoding of INT32 and INT64 columns. FLOAT columns and the GEOMETRY
// column stay PLAIN whatever this says. The numbers are the parquet.thrift
// Encoding codes. Plain is the default and what every board writes today.
// DeltaBinaryPacked is the DELTA_BINARY_PACKED layout of parquet-format
// 2.14.0 Encodings.md with blocks of 128 values in 4 miniblocks of 32, for
// the on device measurement in #135.
enum class IntegerEncoding : uint8_t { Plain = 0, DeltaBinaryPacked = 5 };

// Most bytes a DELTA_BINARY_PACKED stream of `values` integers of
// `value_bytes` (4 or 8) bytes can take. The header holds 128, 4, the count
// and the first value. Each block of up to 128 deltas holds a min delta of at
// most 10 bytes, 4 width bytes and at most 4 miniblocks of 32 slots at the
// full value width, since the spec forbids wider packing than PLAIN.
constexpr size_t delta_binary_packed_bound(size_t values, size_t value_bytes) {
  size_t count_bytes = 1;
  for (size_t rest = values >> 7; rest; rest >>= 7)
    ++count_bytes;
  const size_t deltas = values ? values - 1 : 0;
  const size_t blocks = (deltas + 127) / 128;
  return 2 + 1 + count_bytes + 10 + blocks * (10 + 4 + 4 * 32 * value_bytes);
}
// Parquet LogicalType annotation on the schema leaf.
//
// TimestampMicrosUtc is TIMESTAMP(MICROS, isAdjustedToUTC=true), only valid
// for Int64. Physical bytes are unchanged and readers present the column as
// instants. MICROS is Apache Iceberg `timestamptz` in every format version
// (NANOS would be the v3 only `timestamptz_ns`), so these files can be
// registered in a v3 table without rewrite.
//
// Uint8 is INTEGER(8, unsigned) with ConvertedType UINT_8, only valid for
// Int32. Every present value must be 0..255.
//
// Geometry is the parquet-format 2.12 GEOMETRY type with no crs, so
// OGC:CRS84 longitude and latitude. It is the only annotation of ByteArray,
// and this writer stores 2D WKB Points only. Each row's 8 byte slot holds
// pack_point_e7(longitude, latitude) and the page holds the 21 byte little
// endian WKB Point with x = longitude and y = latitude in degrees. The chunk
// carries GeospatialStatistics (bbox and type code 1) and no min/max.
enum class LogicalType : uint8_t {
  None = 0,
  TimestampMicrosUtc = 1,
  Uint8 = 2,
  Geometry = 3
};

// A Geometry slot. Degrees times 1e7, longitude in the low 32 bits and
// latitude in the high 32 bits, both two's complement. Explicit shifts keep
// the slot independent of host endianness.
constexpr int64_t pack_point_e7(int32_t longitude_e7, int32_t latitude_e7) {
  return static_cast<int64_t>(
      (static_cast<uint64_t>(static_cast<uint32_t>(latitude_e7)) << 32) |
      static_cast<uint64_t>(static_cast<uint32_t>(longitude_e7)));
}
constexpr int32_t point_longitude_e7(int64_t slot) {
  return static_cast<int32_t>(
      static_cast<uint32_t>(static_cast<uint64_t>(slot) & 0xffffffffU));
}
constexpr int32_t point_latitude_e7(int64_t slot) {
  return static_cast<int32_t>(
      static_cast<uint32_t>(static_cast<uint64_t>(slot) >> 32));
}
constexpr int32_t kMaxLongitudeE7 = 1800000000;
constexpr int32_t kMaxLatitudeE7 = 900000000;

// Bounding box of the points of one Geometry column, degrees times 1e7.
struct PointBounds {
  int32_t xmin_e7;
  int32_t ymin_e7;
  int32_t xmax_e7;
  int32_t ymax_e7;
};

// Caller owns separate raw/encoded page buffers and the codec's state. The
// callback returns compressed byte count, or zero on failure; no silent codec
// fallback is permitted inside a column chunk.
struct Compression {
  Codec codec;
  uint8_t *raw;
  size_t raw_capacity;
  uint8_t *encoded;
  size_t encoded_capacity;
  size_t (*compress)(void *, const uint8_t *, size_t, uint8_t *, size_t);
  void *context;
};

// values points at the first field in an array of rows, or a contiguous column.
// A null valid pointer makes the field required. Otherwise each strided byte is
// zero for null, nonzero for present. Buffers must remain immutable while a
// row group is being written; between row groups the caller may refill them.
// field_id is written as SchemaElement.field_id on every leaf. It must be
// positive and unique within the file; Apache Iceberg uses it, not the name,
// to map Parquet columns to table columns. Callers derive it from a stable
// table (the board telemetry dictionaries use 1-based append-only position).
struct Column {
  const char *name = nullptr;
  PhysicalType type = PhysicalType::Int32;
  const void *values = nullptr;
  size_t stride = 0;
  const uint8_t *valid = nullptr;
  size_t valid_stride = 1;
  LogicalType logical = LogicalType::None;
  int32_t field_id = 0;
};

struct KeyValue {
  const char *key;
  const char *value;
};

// Return true ONLY when every byte was accepted; false aborts without retries.
using Sink = bool (*)(void *, const uint8_t *, size_t);

// Footer facts for one column chunk. min/max hold the PLAIN little-endian bit
// pattern of the value (4 or 8 bytes used). Floats follow the parquet-format
// 2.13 TYPE_ORDER rules: NaN excluded and counted, -0.0 for a zero minimum,
// +0.0 for a zero maximum, no bounds when every present value is NaN. For a
// Geometry column min is pack_point_e7(xmin, ymin) and max is
// pack_point_e7(xmax, ymax), written as GeospatialStatistics.
struct ChunkRecord {
  uint64_t offset;
  uint64_t size;
  uint64_t uncompressed_size;
  uint64_t null_count;
  uint64_t nan_count;
  uint64_t min;
  uint64_t max;
  bool has_bounds;
};

struct GroupRecord {
  uint64_t rows;
  uint64_t offset;
  uint64_t compressed_size;
  uint64_t uncompressed_size;
  int sorted_by; // column index whose values ascend within the group, or -1
};

// Caller-owned (static, heap, or PSRAM): no allocation or page-sized stack use.
struct Workspace {
  ChunkRecord chunks[kMaxRowGroups][kMaxColumns];
  GroupRecord groups[kMaxRowGroups];
  uint8_t buffer[512];
};

struct Result {
  bool ok;
  // Bytes accepted by successful sink calls since begin(). A failing sink may
  // have accepted part of its final block, which this interface cannot count.
  uint64_t bytes_written;
  const char *error;
};

// Flat numeric schema plus 2D WKB Point geometry, PLAIN DataPageV1 (one page
// per column chunk, DELTA_BINARY_PACKED integers when asked),
// UNCOMPRESSED or caller-supplied LZ4_RAW, RLE definition levels, Thrift
// Compact footer with per-chunk statistics (null_count, min_value/max_value,
// exact flags, nan_count for FLOAT) and TYPE_ORDER column orders. A file holds
// up to kMaxRowGroups row groups written one at a time:
//
//   Writer writer;
//   writer.begin(sink, context, workspace, columns, count, compression);
//   writer.row_group(rows, sorted_by);   // repeat as batches complete
//   writer.finish(metadata, metadata_count, build);
//
// The column array must stay valid and unchanged (names/types/annotation)
// until finish(). The Compression struct is copied by begin(); only the
// buffers and codec state it points to must outlive the file. After any
// failure the writer is inert; the caller owns close/sync/rename and
// recovery. This finalizes file contents only.
//
// No page index (ColumnIndex/OffsetIndex) and no bloom filters, on purpose
// (#25 Phase 1, docs/shared/parquet-small-files.md). With one page per chunk
// and about 90 rows per row group, a page index repeats the chunk statistics
// and adds 1.6 to 1.7 KB per file, and a bloom filter costs bytes and RAM
// without pruning anything at this size. Host compaction adds both.
class Writer {
public:
  // integers picks the INT32 and INT64 value encoding for the whole file.
  Result begin(Sink sink, void *context, Workspace &workspace,
               const Column *columns, size_t column_count,
               const Compression *compression = nullptr,
               IntegerEncoding integers = IntegerEncoding::Plain);
  Result row_group(size_t row_count, int sorted_by = -1);
  // build is appended to created_by as "(build <build>)" when given. Omitting
  // created_by writes "opensensor-space-firmware version 0.3", the name both
  // boards use (#111).
  Result finish(const KeyValue *metadata = nullptr, size_t metadata_count = 0,
                const char *build = nullptr, const char *created_by = nullptr);
  bool active() const { return active_; }
  // File bounding box of a Geometry column over the row groups written so
  // far, or nullopt when the column is not Geometry or holds no point.
  [[nodiscard]] std::optional<PointBounds> point_bounds(size_t column) const;
  size_t row_groups() const { return groups_; }
  uint64_t rows() const { return rows_; }
  uint64_t bytes_written() const { return accepted_; }

private:
  Result fail(const char *error);
  bool active_ = false;
  Sink sink_ = nullptr;
  void *context_ = nullptr;
  Workspace *workspace_ = nullptr;
  const Column *columns_ = nullptr;
  size_t column_count_ = 0;
  bool compressed_ = false;
  Compression compression_{}; // copy: the caller's struct may be a temporary
  IntegerEncoding integers_ = IntegerEncoding::Plain;
  size_t groups_ = 0;
  size_t used_ = 0; // bytes staged in workspace_->buffer
  uint64_t rows_ = 0;
  uint64_t position_ = 0;
  uint64_t accepted_ = 0;
};

// One row group (or zero when row_count is 0) in one call.
Result write_parquet(Sink sink, void *context, const Column *columns,
                     size_t column_count, size_t row_count,
                     Workspace &workspace, const KeyValue *metadata = nullptr,
                     size_t metadata_count = 0,
                     const Compression *compression = nullptr,
                     int sorted_by = -1, const char *build = nullptr,
                     const char *created_by = nullptr,
                     IntegerEncoding integers = IntegerEncoding::Plain);

} // namespace telemetry
