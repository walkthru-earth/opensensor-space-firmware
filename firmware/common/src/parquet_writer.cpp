#include "parquet_writer.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <string.h>

// Wire format references (checked 2026-09-17 against parquet-format 2.14.0):
// https://github.com/apache/parquet-format/blob/apache-parquet-format-2.14.0/src/main/thrift/parquet.thrift
// https://parquet.apache.org/docs/file-format/data-pages/encodings/
// https://github.com/apache/thrift/blob/master/doc/specs/thrift-compact-protocol.md
// Statistics follow the TYPE_ORDER writer rules introduced in 2.13 (nan_count,
// NaN exclusion, signed zero normalization). Checked 2026-11-09 against the
// sources. PyArrow 26.0.1 drops min/max under any order other than TYPE_ORDER,
// and DuckDB 2.0.0 knows only TYPE_ORDER and reads the bounds that way. The
// deprecated min/max fields are omitted on purpose: every targeted reader uses
// min_value/max_value, and duplicating them would grow the footer by two values
// per column chunk.
// This is a deliberately small format subset, not a general Parquet library.
namespace telemetry {
namespace {

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "Parquet FLOAT requires IEEE 754 binary32");

static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559,
              "WKB and BoundingBox require IEEE 754 binary64");

constexpr uint8_t kTrue = 1, kFalse = 2, kByte = 3, kI16 = 4, kI32 = 5,
                  kI64 = 6, kDouble = 7, kBinary = 8, kList = 9, kStruct = 12;
// GEOMETRY, GeospatialStatistics and INTEGER(8, unsigned) follow
// parquet-format 2.12.0 parquet.thrift and Geospatial.md (checked 2026-10-26).
// A 2D WKB Point is the byte order, a uint32 type, then x and y as doubles.
constexpr size_t kWkbPointBytes = 1 + 4 + 8 + 8;
constexpr uint32_t kWkbPoint = 1;        // ISO WKB and Geospatial.md type code
constexpr uint64_t kConvertedUint8 = 11; // ConvertedType UINT_8
constexpr const char *kDefaultCreatedBy =
    "opensensor-space-firmware version 0.3";

class Output {
public:
  // Staging state lives in the caller so bytes may stay buffered between
  // calls (the Writer keeps a row group's tail and the magic there).
  Output(Sink sink, void *context, Workspace &workspace, size_t &used,
         uint64_t &position, uint64_t &accepted)
      : sink_(sink), context_(context), workspace_(workspace), used_(used),
        position_(position), accepted_(accepted) {}

  void bytes(const void *source, size_t size) {
    const auto *p = static_cast<const uint8_t *>(source);
    while (size && ok_) {
      size_t take = sizeof(workspace_.buffer) - used_;
      if (take > size)
        take = size;
      memcpy(workspace_.buffer + used_, p, take);
      used_ += take;
      position_ += take;
      p += take;
      size -= take;
      if (used_ == sizeof(workspace_.buffer))
        flush();
    }
  }
  void byte(uint8_t value) { bytes(&value, 1); }
  void varint(uint64_t value) {
    do {
      const uint8_t next = static_cast<uint8_t>(value & 127);
      value >>= 7;
      byte(next | (value ? 128 : 0));
    } while (value && ok_);
  }
  // All Thrift integer values generated here are nonnegative metadata.
  void integer(uint64_t value) { varint(value << 1); }
  void little_endian(uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) {
      byte(static_cast<uint8_t>(value));
      value >>= 8;
    }
  }
  // One length-prefixed string assembled from up to four parts.
  void string(const char *a, const char *b = nullptr, const char *c = nullptr,
              const char *d = nullptr) {
    const char *parts[] = {a, b, c, d};
    size_t length = 0;
    for (const char *part : parts) {
      // cppcheck-suppress useStlAlgorithm
      length += part ? strlen(part) : 0;
    }
    varint(length);
    for (const char *part : parts)
      if (part)
        bytes(part, strlen(part));
  }
  void list(uint8_t type, size_t count) {
    if (count < 15) {
      byte(static_cast<uint8_t>((count << 4) | type));
    } else {
      byte(0xf0 | type);
      varint(count);
    }
  }
  void flush() {
    if (!ok_ || !used_)
      return;
    ok_ = sink_(context_, workspace_.buffer, used_);
    if (ok_)
      accepted_ += used_;
    used_ = 0;
  }
  bool ok() const { return ok_; }
  uint64_t position() const { return position_; }

private:
  Sink sink_;
  void *context_;
  Workspace &workspace_;
  size_t &used_;
  uint64_t &position_;
  uint64_t &accepted_;
  bool ok_ = true;
};

// One object per nested struct keeps field-id deltas local, without a stack of
// dynamic protocol objects. Call end() explicitly to write the STOP marker.
class Struct {
public:
  explicit Struct(Output &output) : out_(output) {}
  void field(uint8_t id, uint8_t type) {
    const uint8_t delta = id - previous_;
    if (id > previous_ && delta <= 15) {
      out_.byte(static_cast<uint8_t>((delta << 4) | type));
    } else {
      out_.byte(type);
      out_.integer(id);
    }
    previous_ = id;
  }
  void number(uint8_t id, uint8_t type, uint64_t value) {
    field(id, type);
    out_.integer(value);
  }
  // Compact protocol folds a bool into the field header type nibble.
  void boolean(uint8_t id, bool value) { field(id, value ? kTrue : kFalse); }
  void string(uint8_t id, const char *value) {
    field(id, kBinary);
    out_.string(value);
  }
  // Compact protocol writes an i8 as one raw byte.
  void byte(uint8_t id, uint8_t value) {
    field(id, kByte);
    out_.byte(value);
  }
  // Compact protocol writes a double as its IEEE 754 bits, little endian.
  void real(uint8_t id, double value) {
    field(id, kDouble);
    out_.little_endian(std::bit_cast<uint64_t>(value), 8);
  }
  void binary(uint8_t id, uint64_t bits, size_t width) {
    field(id, kBinary);
    out_.varint(width);
    out_.little_endian(bits, width);
  }
  void list(uint8_t id, uint8_t type, size_t count) {
    field(id, kList);
    out_.list(type, count);
  }
  void end() { out_.byte(0); }

private:
  Output &out_;
  uint8_t previous_ = 0;
};

// Bytes each row takes in the caller's buffer. A geometry row is the int64
// slot that pack_point_e7 fills.
size_t width(PhysicalType type) {
  return type == PhysicalType::Int64 || type == PhysicalType::ByteArray ? 8 : 4;
}
// Bytes each present value takes in a PLAIN page, BYTE_ARRAY length included.
size_t plain_width(PhysicalType type) {
  return type == PhysicalType::ByteArray ? 4 + kWkbPointBytes : width(type);
}

double degrees(int32_t e7) { return static_cast<double>(e7) / 1e7; }

bool present(const Column &column, size_t row) {
  return !column.valid || column.valid[row * column.valid_stride] != 0;
}

size_t varint_size(uint64_t value) {
  size_t result = 1;
  while (value >>= 7)
    ++result;
  return result;
}

size_t present_count(const Column &column, size_t rows) {
  if (!column.valid)
    return rows;
  size_t count = 0;
  for (size_t row = 0; row < rows; ++row)
    count += present(column, row);
  return count;
}

// The bit width is 1 for a flat optional field. RLE runs need one value byte;
// their length is ULEB128(run_length << 1). DataPageV1 prefixes the stream
// length.
size_t definition_levels(const Column &column, size_t rows,
                         Output *output = nullptr) {
  size_t length = 0;
  size_t first = 0;
  while (first < rows) {
    const bool value = present(column, first);
    size_t end = first + 1;
    while (end < rows && present(column, end) == value)
      ++end;
    const uint64_t header = static_cast<uint64_t>(end - first) << 1;
    length += varint_size(header) + 1;
    if (output) {
      output->varint(header);
      output->byte(value ? 1 : 0);
    }
    first = end;
  }
  return length;
}

size_t payload_size(const Column &column, size_t rows) {
  return present_count(column, rows) * plain_width(column.type) +
         (column.valid ? 4 + definition_levels(column, rows) : 0);
}

uint64_t value_bits(const Column &column, size_t row) {
  const auto *values = static_cast<const uint8_t *>(column.values);
  if (width(column.type) == 8) {
    uint64_t value;
    memcpy(&value, values + row * column.stride, sizeof(value));
    return value;
  }
  uint32_t value;
  memcpy(&value, values + row * column.stride, sizeof(value));
  return value;
}

void payload_bytes(Output &out, const Column &column, size_t rows) {
  if (column.valid) {
    out.little_endian(definition_levels(column, rows), 4);
    definition_levels(column, rows, &out);
  }
  for (size_t row = 0; row < rows && out.ok(); ++row) {
    if (!present(column, row))
      continue;
    // memcpy avoids alignment/aliasing assumptions; emit explicitly LE.
    const uint64_t bits = value_bits(column, row);
    if (column.type != PhysicalType::ByteArray) {
      out.little_endian(bits, width(column.type));
      continue;
    }
    // BYTE_ARRAY length, then a little endian 2D WKB Point, x lon and y lat.
    const auto slot = static_cast<int64_t>(bits);
    out.little_endian(kWkbPointBytes, 4);
    out.byte(1);
    out.little_endian(kWkbPoint, 4);
    out.little_endian(
        std::bit_cast<uint64_t>(degrees(point_longitude_e7(slot))), 8);
    out.little_endian(std::bit_cast<uint64_t>(degrees(point_latitude_e7(slot))),
                      8);
  }
}

uint32_t float_bits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

// A Geometry chunk's bounding box, packed as ChunkRecord documents. False
// when a point lies outside longitude -180..180 or latitude -90..90.
bool point_statistics(const Column &column, size_t rows, ChunkRecord &record) {
  int32_t xmin = 0, ymin = 0, xmax = 0, ymax = 0;
  for (size_t row = 0; row < rows; ++row) {
    if (!present(column, row))
      continue;
    const auto slot = static_cast<int64_t>(value_bits(column, row));
    const int32_t x = point_longitude_e7(slot);
    const int32_t y = point_latitude_e7(slot);
    if (x < -kMaxLongitudeE7 || x > kMaxLongitudeE7 || y < -kMaxLatitudeE7 ||
        y > kMaxLatitudeE7)
      return false;
    if (!record.has_bounds || x < xmin)
      xmin = x;
    if (!record.has_bounds || x > xmax)
      xmax = x;
    if (!record.has_bounds || y < ymin)
      ymin = y;
    if (!record.has_bounds || y > ymax)
      ymax = y;
    record.has_bounds = true;
  }
  record.min = static_cast<uint64_t>(pack_point_e7(xmin, ymin));
  record.max = static_cast<uint64_t>(pack_point_e7(xmax, ymax));
  return true;
}

// Bounds use the column's TYPE_ORDER: signed integers; floats compared as
// values with NaN excluded, then zero signs normalized for the footer. A
// Uint8 column holds 0..255, where signed and unsigned order agree. False
// when a value lies outside its logical type's range.
bool statistics(const Column &column, size_t rows, ChunkRecord &record) {
  record.null_count = rows - present_count(column, rows);
  record.nan_count = 0;
  record.has_bounds = false;
  record.min = record.max = 0;
  if (column.logical == LogicalType::Geometry)
    return point_statistics(column, rows, record);
  int32_t min32 = 0, max32 = 0;
  int64_t min64 = 0, max64 = 0;
  float minf = 0, maxf = 0;
  for (size_t row = 0; row < rows; ++row) {
    if (!present(column, row))
      continue;
    const uint64_t bits = value_bits(column, row);
    if (column.type == PhysicalType::Float) {
      float value;
      const uint32_t narrow = static_cast<uint32_t>(bits);
      memcpy(&value, &narrow, sizeof(value));
      if (std::isnan(value)) {
        ++record.nan_count;
        continue;
      }
      if (!record.has_bounds || value < minf)
        minf = value;
      if (!record.has_bounds || value > maxf)
        maxf = value;
    } else if (column.type == PhysicalType::Int64) {
      int64_t value;
      memcpy(&value, &bits, sizeof(value));
      if (!record.has_bounds || value < min64)
        min64 = value;
      if (!record.has_bounds || value > max64)
        max64 = value;
    } else {
      int32_t value;
      const uint32_t narrow = static_cast<uint32_t>(bits);
      memcpy(&value, &narrow, sizeof(value));
      if (column.logical == LogicalType::Uint8 && (value < 0 || value > 255))
        return false;
      if (!record.has_bounds || value < min32)
        min32 = value;
      if (!record.has_bounds || value > max32)
        max32 = value;
    }
    record.has_bounds = true;
  }
  if (!record.has_bounds)
    return true;
  if (column.type == PhysicalType::Float) {
    // Signed zeros compare equal; the footer must show -0.0 low, +0.0 high.
    record.min = minf == 0.0F ? float_bits(-0.0F) : float_bits(minf);
    record.max = maxf == 0.0F ? float_bits(0.0F) : float_bits(maxf);
  } else if (column.type == PhysicalType::Int64) {
    memcpy(&record.min, &min64, sizeof(record.min));
    memcpy(&record.max, &max64, sizeof(record.max));
  } else {
    uint32_t bits;
    memcpy(&bits, &min32, sizeof(bits));
    record.min = bits;
    memcpy(&bits, &max32, sizeof(bits));
    record.max = bits;
  }
  return true;
}

struct MemorySink {
  uint8_t *data = nullptr;
  size_t capacity = 0;
  size_t used = 0;
};
bool memory_sink(void *context, const uint8_t *data, size_t size) {
  auto &memory = *static_cast<MemorySink *>(context);
  if (size > memory.capacity - memory.used)
    return false;
  memcpy(memory.data + memory.used, data, size);
  memory.used += size;
  return true;
}

bool page(Output &out, const Column &column, size_t rows, Workspace &workspace,
          const Compression *compression, ChunkRecord &record) {
  const size_t payload = payload_size(column, rows);
  size_t compressed = payload;
  if (compression) {
    // Both Outputs reuse the staging buffer, never while the other has bytes.
    out.flush();
    if (!out.ok())
      return false;
    MemorySink memory{compression->raw, compression->raw_capacity};
    size_t scratch_used = 0;
    uint64_t scratch_position = 0, scratch_accepted = 0;
    Output raw(memory_sink, &memory, workspace, scratch_used, scratch_position,
               scratch_accepted);
    payload_bytes(raw, column, rows);
    raw.flush();
    if (!raw.ok() || memory.used != payload)
      return false;
    compressed = compression->compress(compression->context, compression->raw,
                                       payload, compression->encoded,
                                       compression->encoded_capacity);
    if (!compressed || compressed > compression->encoded_capacity)
      return false;
  }
  const auto header_start = out.position();
  Struct header(out);
  header.number(1, kI32, 0); // DATA_PAGE
  header.number(2, kI32, payload);
  header.number(3, kI32, compressed);
  header.field(5, kStruct);
  Struct data(out);
  data.number(1, kI32, rows);
  data.number(2, kI32, 0); // PLAIN
  data.number(3, kI32, 3); // RLE definition levels
  data.number(4, kI32, 3); // RLE repetition levels (no bytes: max level 0)
  data.end();
  header.end();
  record.uncompressed_size = out.position() - header_start + payload;
  if (compression)
    out.bytes(compression->encoded, compressed);
  else
    payload_bytes(out, column, rows);
  return out.ok();
}

void schema_element(Output &out, const Column &column) {
  Struct leaf(out);
  leaf.number(1, kI32, static_cast<uint8_t>(column.type));
  leaf.number(3, kI32, column.valid ? 1 : 0);
  leaf.string(4, column.name);
  // Readers that predate LogicalType know UINT_8 only as a ConvertedType.
  if (column.logical == LogicalType::Uint8)
    leaf.number(6, kI32, kConvertedUint8);
  leaf.number(9, kI32, static_cast<uint64_t>(column.field_id));
  if (column.logical == LogicalType::Uint8) {
    leaf.field(10, kStruct); // LogicalType union
    Struct logical(out);
    logical.field(10, kStruct); // INTEGER
    Struct integer(out);
    integer.byte(1, 8);        // bitWidth
    integer.boolean(2, false); // isSigned
    integer.end();
    logical.end();
  } else if (column.logical == LogicalType::Geometry) {
    leaf.field(10, kStruct); // LogicalType union
    Struct logical(out);
    logical.field(17, kStruct); // GEOMETRY, no crs means OGC:CRS84
    Struct geometry(out);
    geometry.end();
    logical.end();
  } else if (column.logical == LogicalType::TimestampMicrosUtc) {
    leaf.field(10, kStruct); // LogicalType union
    Struct logical(out);
    logical.field(8, kStruct); // TIMESTAMP
    Struct timestamp(out);
    timestamp.boolean(1, true);  // isAdjustedToUTC
    timestamp.field(2, kStruct); // TimeUnit union
    Struct unit(out);
    unit.field(2, kStruct); // MICROS
    Struct micros(out);
    micros.end();
    unit.end();
    timestamp.end();
    logical.end();
  }
  leaf.end();
}

void column_chunk(Output &out, const Column &column, const ChunkRecord &record,
                  uint64_t rows, Codec codec) {
  Struct chunk(out);
  chunk.number(2, kI64, 0); // metadata lives only in this footer
  chunk.field(3, kStruct);
  Struct meta(out);
  meta.number(1, kI32, static_cast<uint8_t>(column.type));
  meta.list(2, kI32, 2);
  out.integer(0); // PLAIN
  out.integer(3); // RLE
  meta.list(3, kBinary, 1);
  out.string(column.name);
  meta.number(4, kI32, static_cast<uint8_t>(codec));
  meta.number(5, kI64, rows);
  meta.number(6, kI64, record.uncompressed_size);
  meta.number(7, kI64, record.size);
  meta.number(9, kI64, record.offset);
  meta.field(12, kStruct);
  Struct stats(out);
  stats.number(3, kI64, record.null_count);
  const bool geometry = column.logical == LogicalType::Geometry;
  // GEOMETRY has no sort order, so its bounds go in GeospatialStatistics.
  if (record.has_bounds && !geometry) {
    const size_t bytes = width(column.type);
    stats.binary(5, record.max, bytes);
    stats.binary(6, record.min, bytes);
    stats.boolean(7, true);
    stats.boolean(8, true);
  }
  if (column.type == PhysicalType::Float)
    stats.number(9, kI64, record.nan_count);
  stats.end();
  if (geometry && record.has_bounds) {
    const auto low = static_cast<int64_t>(record.min);
    const auto high = static_cast<int64_t>(record.max);
    meta.field(17, kStruct); // GeospatialStatistics
    Struct geo(out);
    geo.field(1, kStruct); // BoundingBox, 2D
    Struct box(out);
    box.real(1, degrees(point_longitude_e7(low)));  // xmin
    box.real(2, degrees(point_longitude_e7(high))); // xmax
    box.real(3, degrees(point_latitude_e7(low)));   // ymin
    box.real(4, degrees(point_latitude_e7(high)));  // ymax
    box.end();
    geo.list(2, kI32, 1); // geospatial_types
    out.integer(kWkbPoint);
    geo.end();
  }
  meta.end();
  chunk.end();
}

void row_group_metadata(Output &out, const Column *columns, size_t count,
                        const ChunkRecord *chunks, const GroupRecord &group,
                        size_t ordinal, Codec codec) {
  Struct row_group(out);
  row_group.list(1, kStruct, count);
  for (size_t i = 0; i < count; ++i)
    column_chunk(out, columns[i], chunks[i], group.rows, codec);
  row_group.number(2, kI64, group.uncompressed_size);
  row_group.number(3, kI64, group.rows);
  if (group.sorted_by >= 0) {
    row_group.list(4, kStruct, 1);
    Struct sorting(out);
    sorting.number(1, kI32, static_cast<uint64_t>(group.sorted_by));
    sorting.boolean(2, false); // ascending
    sorting.boolean(3, false); // nulls last
    sorting.end();
  }
  row_group.number(5, kI64, group.offset);
  row_group.number(6, kI64, group.compressed_size);
  row_group.number(7, kI16, ordinal);
  row_group.end();
}

void footer(Output &out, const Column *columns, size_t count,
            const Workspace &workspace, size_t groups, uint64_t rows,
            const KeyValue *metadata, size_t metadata_count, Codec codec,
            const char *build, const char *created_by) {
  Struct file(out);
  file.number(1, kI32, 1);
  file.list(2, kStruct, count + 1);
  Struct root(out);
  root.string(4, "telemetry");
  root.number(5, kI32, count);
  root.end();
  for (size_t i = 0; i < count; ++i)
    schema_element(out, columns[i]);
  file.number(3, kI64, rows);
  file.list(4, kStruct, groups);
  for (size_t g = 0; g < groups; ++g)
    row_group_metadata(out, columns, count, workspace.chunks[g],
                       workspace.groups[g], g, codec);
  if (metadata_count) {
    file.list(5, kStruct, metadata_count);
    for (size_t i = 0; i < metadata_count; ++i) {
      Struct kv(out);
      kv.string(1, metadata[i].key);
      kv.string(2, metadata[i].value);
      kv.end();
    }
  }
  file.field(6, kBinary);
  if (!created_by)
    created_by = kDefaultCreatedBy;
  if (build && build[0])
    out.string(created_by, " (build ", build, ")");
  else
    out.string(created_by);
  // Column orders are required for min_value/max_value to be meaningful.
  file.list(7, kStruct, count);
  for (size_t i = 0; i < count; ++i) {
    Struct order(out);
    order.field(1, kStruct); // TYPE_ORDER
    Struct type_defined(out);
    type_defined.end();
    order.end();
  }
  file.end();
}

bool bounded_string(const char *value, size_t limit, bool allow_empty = false) {
  if (!value || (!allow_empty && !value[0]))
    return false;
  for (size_t i = 0; i <= limit; ++i) {
    if (!value[i])
      return true;
  }
  return false;
}

} // namespace

Result Writer::fail(const char *error) {
  active_ = false;
  return {false, accepted_, error};
}

Result Writer::begin(Sink sink, void *context, Workspace &workspace,
                     const Column *columns, size_t column_count,
                     const Compression *compression) {
  active_ = false;
  groups_ = 0;
  used_ = 0;
  rows_ = position_ = accepted_ = 0;
  if (compression &&
      (compression->codec != Codec::Lz4Raw || !compression->raw ||
       !compression->encoded || !compression->compress ||
       compression->encoded_capacity > INT32_MAX))
    return {false, 0, "invalid compression configuration"};
  if (!sink || !columns || !column_count || column_count > kMaxColumns)
    return {false, 0, "invalid argument or capacity exceeded"};
  for (size_t i = 0; i < column_count; ++i) {
    const Column &column = columns[i];
    if (!bounded_string(column.name, 255) ||
        (column.type != PhysicalType::Int32 &&
         column.type != PhysicalType::Int64 &&
         column.type != PhysicalType::Float &&
         column.type != PhysicalType::ByteArray)) {
      return {false, 0, "invalid column name or type"};
    }
    if (column.field_id <= 0)
      return {false, 0, "invalid column field_id"};
    bool matches = false;
    switch (column.logical) {
    case LogicalType::None:
      matches = column.type != PhysicalType::ByteArray;
      break;
    case LogicalType::TimestampMicrosUtc:
      matches = column.type == PhysicalType::Int64;
      break;
    case LogicalType::Uint8:
      matches = column.type == PhysicalType::Int32;
      break;
    case LogicalType::Geometry:
      matches = column.type == PhysicalType::ByteArray;
      break;
    }
    if (!matches)
      return {false, 0, "logical type does not match physical type"};
    for (size_t j = 0; j < i; ++j) {
      if (!strcmp(column.name, columns[j].name))
        return {false, 0, "duplicate column name"};
      if (column.field_id == columns[j].field_id)
        return {false, 0, "duplicate column field_id"};
    }
  }
  sink_ = sink;
  context_ = context;
  workspace_ = &workspace;
  columns_ = columns;
  column_count_ = column_count;
  compressed_ = compression != nullptr;
  if (compressed_)
    compression_ = *compression;
  active_ = true;
  // The magic stays in the staging buffer until the first page or the footer
  // flushes it, so a rejected first row group leaves the sink untouched.
  Output out(sink_, context_, *workspace_, used_, position_, accepted_);
  out.bytes("PAR1", 4);
  return {true, accepted_, nullptr};
}

Result Writer::row_group(size_t row_count, int sorted_by) {
  if (!active_)
    return {false, accepted_, "writer is not active"};
  if (!row_count || row_count > kMaxRows || groups_ >= kMaxRowGroups ||
      sorted_by >= static_cast<int>(column_count_))
    return fail("invalid row count, sort column or row-group capacity");
  for (size_t i = 0; i < column_count_; ++i) {
    const Column &column = columns_[i];
    if (!column.values || column.stride < width(column.type) ||
        column.stride > SIZE_MAX / row_count ||
        (column.valid &&
         (!column.valid_stride || column.valid_stride > SIZE_MAX / row_count)))
      return fail("invalid column buffer or stride");
    if (compressed_ &&
        payload_size(column, row_count) > compression_.raw_capacity)
      return fail("compression page capacity exceeded");
  }
  Output out(sink_, context_, *workspace_, used_, position_, accepted_);
  ChunkRecord *chunks = workspace_->chunks[groups_];
  GroupRecord &group = workspace_->groups[groups_];
  group = GroupRecord{row_count, out.position(), 0, 0, sorted_by};
  for (size_t i = 0; i < column_count_ && out.ok(); ++i) {
    ChunkRecord &record = chunks[i];
    if (!statistics(columns_[i], row_count, record))
      return fail("value outside its logical type range");
    record.offset = out.position();
    if (!page(out, columns_[i], row_count, *workspace_,
              compressed_ ? &compression_ : nullptr, record))
      return fail("page encoding/compression or sink failed");
    record.size = out.position() - record.offset;
    group.compressed_size += record.size;
    group.uncompressed_size += record.uncompressed_size;
  }
  out.flush();
  if (!out.ok())
    return fail("sink write failed");
  ++groups_;
  rows_ += row_count;
  return {true, accepted_, nullptr};
}

std::optional<PointBounds> Writer::point_bounds(size_t column) const {
  if (!workspace_ || column >= column_count_ ||
      columns_[column].logical != LogicalType::Geometry)
    return std::nullopt;
  std::optional<PointBounds> bounds;
  for (size_t g = 0; g < groups_; ++g) {
    const ChunkRecord &record = workspace_->chunks[g][column];
    if (!record.has_bounds)
      continue;
    const auto low = static_cast<int64_t>(record.min);
    const auto high = static_cast<int64_t>(record.max);
    const PointBounds chunk{
        .xmin_e7 = point_longitude_e7(low),
        .ymin_e7 = point_latitude_e7(low),
        .xmax_e7 = point_longitude_e7(high),
        .ymax_e7 = point_latitude_e7(high),
    };
    if (!bounds) {
      bounds = chunk;
      continue;
    }
    bounds->xmin_e7 = std::min(bounds->xmin_e7, chunk.xmin_e7);
    bounds->ymin_e7 = std::min(bounds->ymin_e7, chunk.ymin_e7);
    bounds->xmax_e7 = std::max(bounds->xmax_e7, chunk.xmax_e7);
    bounds->ymax_e7 = std::max(bounds->ymax_e7, chunk.ymax_e7);
  }
  return bounds;
}

Result Writer::finish(const KeyValue *metadata, size_t metadata_count,
                      const char *build, const char *created_by) {
  if (!active_)
    return {false, accepted_, "writer is not active"};
  if (metadata_count > 64 || (metadata_count && !metadata) ||
      (build && !bounded_string(build, 64, true)) ||
      (created_by && !bounded_string(created_by, 128)))
    return fail("invalid metadata or build identity");
  for (size_t i = 0; i < metadata_count; ++i) {
    if (!bounded_string(metadata[i].key, 255) ||
        !bounded_string(metadata[i].value, 4096, true))
      return fail("invalid metadata key or value");
  }
  Output out(sink_, context_, *workspace_, used_, position_, accepted_);
  const uint64_t footer_start = out.position();
  footer(out, columns_, column_count_, *workspace_, groups_, rows_, metadata,
         metadata_count, compressed_ ? compression_.codec : Codec::Uncompressed,
         build, created_by);
  out.little_endian(out.position() - footer_start, 4);
  out.bytes("PAR1", 4);
  out.flush();
  active_ = false;
  return {out.ok(), accepted_, out.ok() ? nullptr : "sink write failed"};
}

Result write_parquet(Sink sink, void *context, const Column *columns,
                     size_t column_count, size_t row_count,
                     Workspace &workspace, const KeyValue *metadata,
                     size_t metadata_count, const Compression *compression,
                     int sorted_by, const char *build, const char *created_by) {
  if (row_count > kMaxRows)
    return {false, 0, "invalid argument or capacity exceeded"};
  Writer writer;
  Result result = writer.begin(sink, context, workspace, columns, column_count,
                               compression);
  if (result.ok && row_count)
    result = writer.row_group(row_count, sorted_by);
  if (result.ok)
    result = writer.finish(metadata, metadata_count, build, created_by);
  return result;
}

} // namespace telemetry
