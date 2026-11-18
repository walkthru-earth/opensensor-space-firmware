// Build through pixi (tools/test_parquet_reader.py); runs exactly the firmware
// footer reader on the host.
//
// Usage:
//   parquet_reader_fixture dump input.parquet
//     Decode through both entry points, require identical results, and print
//     the summary and Iceberg metrics as one JSON object. Exit 1 with
//     {"error": ...} when the reader refuses the file.
//   parquet_reader_fixture fuzz input.parquet iterations seed
//     Mutate and truncate the footer and feed every variant to both entry
//     points. Accepted variants must still satisfy the summary invariants.
//     Sanitizers catch memory and undefined-behaviour faults.
//   parquet_reader_fixture columns input.parquet
//     Decode every INT32, INT64 and FLOAT column of every row group through
//     the page decoder (parquet_columns.h) and print the rows as one JSON
//     object, FLOAT as its 32 bit pattern and a null as null. A column the
//     decoder refuses prints {"name":...,"error":...}.
//   parquet_reader_fixture pagefuzz input.parquet iterations seed
//     Keep the footer, mutate and truncate the page bytes and the chunk
//     layout, and decode every column. Accepted columns must still hold
//     exactly the group's rows with a 0 or 1 mask.
//   parquet_reader_fixture pagecases
//     Hand-built pages, one per hostile shape, each with the exact error the
//     decoder must return, plus accepted RLE, bit-packed and two page chunks.
#include "../firmware/common/src/lz4_codec.h"
#include "../firmware/common/src/parquet_columns.h"
#include "../firmware/common/src/parquet_reader.h"

#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using telemetry::FileSummary;
using telemetry::IcebergDataFileMetrics;

struct Buffer {
  const uint8_t *data;
  size_t size;
  size_t reads;
};

bool buffer_read(void *context, uint64_t offset, uint8_t *dest, size_t size) {
  auto &buffer = *static_cast<Buffer *>(context);
  ++buffer.reads;
  if (offset > buffer.size || size > buffer.size - offset)
    return false;
  memcpy(dest, buffer.data + offset, size);
  return true;
}

bool load(const char *path, std::vector<uint8_t> &bytes) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return false;
  uint8_t chunk[4096];
  size_t n = 0;
  while ((n = fread(chunk, 1, sizeof(chunk), file)) > 0)
    bytes.insert(bytes.end(), chunk, chunk + n);
  return fclose(file) == 0;
}

const char *type_name(telemetry::PhysicalType type) {
  switch (type) {
  case telemetry::PhysicalType::Int32:
    return "INT32";
  case telemetry::PhysicalType::Int64:
    return "INT64";
  case telemetry::PhysicalType::Float:
    return "FLOAT";
  case telemetry::PhysicalType::ByteArray:
    return "BYTE_ARRAY";
  }
  return "?";
}

const char *logical_name(telemetry::LogicalType logical) {
  switch (logical) {
  case telemetry::LogicalType::None:
    return "none";
  case telemetry::LogicalType::TimestampMicrosUtc:
    return "timestamp_micros_utc";
  case telemetry::LogicalType::Uint8:
    return "uint8";
  case telemetry::LogicalType::Geometry:
    return "geometry";
  }
  return "?";
}

void hex(const uint8_t *bytes, size_t size) {
  putchar('"');
  for (size_t i = 0; i < size; ++i)
    printf("%02x", bytes[i]);
  putchar('"');
}

void print_u64_or_null(bool present, uint64_t value) {
  if (present)
    printf("%" PRIu64, value);
  else
    printf("null");
}

void dump(const FileSummary &file, const IcebergDataFileMetrics *metrics,
          const char *metrics_error) {
  printf("{\"version\":%d,\"num_rows\":%" PRIu64 ",\"file_size\":%" PRIu64
         ",\"footer_size\":%u,\"columns\":[",
         file.version, file.num_rows, file.file_size, file.footer_size);
  for (size_t c = 0; c < file.column_count; ++c) {
    const auto &column = file.columns[c];
    printf("%s{\"name\":\"%s\",\"type\":\"%s\",\"nullable\":%s,"
           "\"timestamp_micros_utc\":%s,\"logical\":\"%s\",\"field_id\":%d}",
           c ? "," : "", column.name, type_name(column.type),
           column.nullable ? "true" : "false",
           column.logical == telemetry::LogicalType::TimestampMicrosUtc
               ? "true"
               : "false",
           logical_name(column.logical), column.field_id);
  }
  printf("],\"row_groups\":[");
  for (size_t g = 0; g < file.row_group_count; ++g) {
    const auto &group = file.row_groups[g];
    printf("%s{\"num_rows\":%" PRIu64 ",\"file_offset\":%" PRIu64
           ",\"total_byte_size\":%" PRIu64 ",\"columns\":[",
           g ? "," : "", group.num_rows, group.file_offset,
           group.total_byte_size);
    for (size_t c = 0; c < file.column_count; ++c) {
      const auto &chunk = group.columns[c];
      printf("%s{\"codec\":%d,\"num_values\":%" PRIu64
             ",\"total_uncompressed_size\":%" PRIu64
             ",\"total_compressed_size\":%" PRIu64
             ",\"data_page_offset\":%" PRIu64 ",\"start_offset\":%" PRIu64
             ",\"null_count\":",
             c ? "," : "", static_cast<int>(chunk.codec), chunk.num_values,
             chunk.total_uncompressed_size, chunk.total_compressed_size,
             chunk.data_page_offset, chunk.start_offset);
      print_u64_or_null(chunk.has_null_count, chunk.null_count);
      printf(",\"nan_count\":");
      print_u64_or_null(chunk.has_nan_count, chunk.nan_count);
      printf(",\"min\":");
      print_u64_or_null(chunk.has_bounds, chunk.min);
      printf(",\"max\":");
      print_u64_or_null(chunk.has_bounds, chunk.max);
      putchar('}');
    }
    printf("]}");
  }
  printf("]");
  if (!metrics) {
    printf(",\"metrics_error\":\"%s\"}\n", metrics_error);
    return;
  }
  printf(",\"iceberg\":{\"record_count\":%" PRIu64
         ",\"file_size_in_bytes\":%" PRIu64 ",\"split_offsets\":[",
         metrics->record_count, metrics->file_size_in_bytes);
  for (size_t g = 0; g < metrics->split_offset_count; ++g)
    printf("%s%" PRIu64, g ? "," : "", metrics->split_offsets[g]);
  printf("],\"columns\":[");
  for (size_t c = 0; c < metrics->column_count; ++c) {
    const auto &column = metrics->columns[c];
    printf("%s{\"field_id\":%d,\"column_size\":%" PRIu64
           ",\"value_count\":%" PRIu64 ",\"null_value_count\":",
           c ? "," : "", column.field_id, column.column_size,
           column.value_count);
    print_u64_or_null(column.has_null_value_count, column.null_value_count);
    printf(",\"nan_value_count\":");
    print_u64_or_null(column.has_nan_value_count, column.nan_value_count);
    printf(",\"lower_bound\":");
    if (column.has_bounds)
      hex(column.lower_bound, column.bound_size);
    else
      printf("null");
    printf(",\"upper_bound\":");
    if (column.has_bounds)
      hex(column.upper_bound, column.bound_size);
    else
      printf("null");
    putchar('}');
  }
  printf("]}}\n");
}

// Invariants every accepted footer must satisfy, whatever the input bytes.
bool consistent(const FileSummary &file, size_t file_size) {
  if (file.file_size != file_size || !file.column_count ||
      file.column_count > telemetry::kMaxColumns ||
      file.row_group_count > telemetry::kMaxRowGroups ||
      file.footer_size > file_size - 12)
    return false;
  const uint64_t data_end = file_size - 8 - file.footer_size;
  uint64_t rows = 0;
  for (size_t c = 0; c < file.column_count; ++c) {
    const size_t length =
        strnlen(file.columns[c].name, telemetry::kMaxNameLength);
    if (!length || length >= telemetry::kMaxNameLength)
      return false;
  }
  for (size_t g = 0; g < file.row_group_count; ++g) {
    const auto &group = file.row_groups[g];
    if (group.num_rows > file.num_rows - rows)
      return false;
    rows += group.num_rows;
    for (size_t c = 0; c < file.column_count; ++c) {
      const auto &chunk = group.columns[c];
      if (chunk.num_values != group.num_rows ||
          chunk.start_offset + chunk.total_compressed_size > data_end ||
          (chunk.has_null_count && chunk.null_count > chunk.num_values))
        return false;
    }
  }
  return rows == file.num_rows;
}

bool same(const FileSummary &a, const FileSummary &b) {
  // Every decoded field is written explicitly, so padding aside the two
  // summaries must agree; compare the parts the reader filled.
  if (a.version != b.version || a.num_rows != b.num_rows ||
      a.file_size != b.file_size || a.footer_size != b.footer_size ||
      a.column_count != b.column_count ||
      a.row_group_count != b.row_group_count)
    return false;
  for (size_t c = 0; c < a.column_count; ++c) {
    const auto &x = a.columns[c], &y = b.columns[c];
    if (strcmp(x.name, y.name) != 0 || x.type != y.type ||
        x.nullable != y.nullable || x.logical != y.logical ||
        x.field_id != y.field_id)
      return false;
  }
  for (size_t g = 0; g < a.row_group_count; ++g) {
    const auto &x = a.row_groups[g], &y = b.row_groups[g];
    if (x.num_rows != y.num_rows || x.file_offset != y.file_offset ||
        x.total_byte_size != y.total_byte_size)
      return false;
    for (size_t c = 0; c < a.column_count; ++c) {
      const auto &p = x.columns[c], &q = y.columns[c];
      if (p.codec != q.codec || p.num_values != q.num_values ||
          p.total_compressed_size != q.total_compressed_size ||
          p.data_page_offset != q.data_page_offset ||
          p.start_offset != q.start_offset ||
          p.has_null_count != q.has_null_count ||
          p.null_count != q.null_count || p.has_nan_count != q.has_nan_count ||
          p.nan_count != q.nan_count || p.has_bounds != q.has_bounds ||
          p.min != q.min || p.max != q.max)
        return false;
    }
  }
  return true;
}

struct State {
  FileSummary memory;
  FileSummary streamed;
  IcebergDataFileMetrics metrics;
  uint8_t scratch[1 << 16];
};

int run_dump(const char *path, State &state) {
  std::vector<uint8_t> bytes;
  if (!load(path, bytes))
    return 3;
  const auto in_memory =
      telemetry::read_parquet_footer(bytes.data(), bytes.size(), state.memory);
  Buffer buffer{bytes.data(), bytes.size(), 0};
  const auto streamed = telemetry::read_parquet_footer(
      buffer_read, &buffer, bytes.size(), state.scratch, sizeof(state.scratch),
      state.streamed);
  if (in_memory.ok != streamed.ok)
    return 4;
  if (!in_memory.ok) {
    printf("{\"error\":\"%s\"}\n", in_memory.error);
    return 1;
  }
  // Header magic, tail, then the footer: three reads, never the pages.
  if (!same(state.memory, state.streamed) || buffer.reads != 3 ||
      !consistent(state.memory, bytes.size()))
    return 5;
  const auto metrics =
      telemetry::iceberg_data_file_metrics(state.memory, state.metrics);
  dump(state.memory, metrics.ok ? &state.metrics : nullptr, metrics.error);
  return 0;
}

uint32_t next(uint64_t &seed) {
  seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
  return static_cast<uint32_t>(seed >> 33);
}

int run_fuzz(const char *path, unsigned long iterations, uint64_t seed,
             State &state) {
  std::vector<uint8_t> original;
  if (!load(path, original) || original.size() < 12)
    return 3;
  uint32_t footer = 0;
  memcpy(&footer, original.data() + original.size() - 8, 4);
  if (footer > original.size() - 12)
    return 3;
  const size_t footer_start = original.size() - 8 - footer;
  unsigned long accepted = 0;
  std::vector<uint8_t> bytes;
  for (unsigned long i = 0; i < iterations; ++i) {
    bytes = original;
    const uint32_t mode = next(seed) % 8;
    const size_t edits = 1 + next(seed) % 4;
    for (size_t e = 0; e < edits; ++e) {
      const size_t at = footer_start + next(seed) % footer;
      switch (mode) {
      case 0: // flip one bit
        bytes[at] ^= static_cast<uint8_t>(1U << (next(seed) % 8));
        break;
      case 1: // random byte
        bytes[at] = static_cast<uint8_t>(next(seed));
        break;
      case 2: // varint continuation everywhere
        bytes[at] |= 0x80U;
        break;
      case 3: // long-form list or field headers
        bytes[at] = static_cast<uint8_t>(0xF0U | (next(seed) % 13));
        break;
      case 4: { // truncate the footer, keep a valid-looking tail
        const uint32_t cut = 1 + next(seed) % footer;
        bytes.erase(bytes.begin() + static_cast<long>(footer_start),
                    bytes.begin() + static_cast<long>(footer_start + cut));
        e = edits;
        break;
      }
      case 5: { // wrong footer length
        uint32_t length = next(seed);
        if (next(seed) % 2)
          length = footer + (next(seed) % 64) - 32;
        memcpy(bytes.data() + bytes.size() - 8, &length, 4);
        e = edits;
        break;
      }
      case 6: // zero a run
        for (size_t z = at; z < bytes.size() - 8 && z < at + 8; ++z)
          bytes[z] = 0;
        break;
      default: // drop the whole file to a random prefix plus the tail
        bytes.erase(bytes.begin() +
                        static_cast<long>(next(seed) % (bytes.size() - 8)),
                    bytes.end() - 8);
        e = edits;
        break;
      }
    }
    const auto in_memory = telemetry::read_parquet_footer(
        bytes.data(), bytes.size(), state.memory);
    Buffer buffer{bytes.data(), bytes.size(), 0};
    const auto streamed = telemetry::read_parquet_footer(
        buffer_read, &buffer, bytes.size(), state.scratch,
        sizeof(state.scratch), state.streamed);
    if (in_memory.ok != streamed.ok) {
      fprintf(stderr, "entry points disagree at iteration %lu\n", i);
      return 6;
    }
    if (!in_memory.ok)
      continue;
    ++accepted;
    if (!same(state.memory, state.streamed) ||
        !consistent(state.memory, bytes.size())) {
      fprintf(stderr, "inconsistent summary at iteration %lu\n", i);
      return 7;
    }
    telemetry::iceberg_data_file_metrics(state.memory, state.metrics);
  }
  printf("{\"iterations\":%lu,\"accepted\":%lu}\n", iterations, accepted);
  return 0;
}

// ---------------------------------------------------------------------------
// Page decoder (parquet_columns.h).

constexpr size_t kMaxRows = 1 << 16;

struct Columns {
  std::vector<float> floats = std::vector<float>(kMaxRows);
  std::vector<int64_t> int64s = std::vector<int64_t>(kMaxRows);
  std::vector<int32_t> int32s = std::vector<int32_t>(kMaxRows);
  std::vector<uint8_t> valid = std::vector<uint8_t>(kMaxRows);
  std::vector<uint8_t> scratch = std::vector<uint8_t>(1 << 20);
  telemetry::ColumnOutput output() {
    return {
        .floats = floats, .int64s = int64s, .int32s = int32s, .valid = valid};
  }
};

bool loaded_footer(const std::vector<uint8_t> &bytes, State &state) {
  return telemetry::read_parquet_footer(bytes.data(), bytes.size(),
                                        state.memory)
      .ok;
}

int run_columns(const char *path, State &state, Columns &columns) {
  std::vector<uint8_t> bytes;
  if (!load(path, bytes))
    return 3;
  if (!loaded_footer(bytes, state)) {
    printf("{\"error\":\"footer\"}\n");
    return 1;
  }
  const FileSummary &file = state.memory;
  if (file.num_rows > kMaxRows)
    return 3;
  Buffer buffer{bytes.data(), bytes.size(), 0};
  printf("{\"columns\":[");
  for (size_t c = 0; c < file.column_count; ++c) {
    const auto &column = file.columns[c];
    if (column.type == telemetry::PhysicalType::ByteArray)
      continue;
    const auto found = telemetry::find_column(file, column.name);
    if (!found || *found > c)
      return 8;
    printf("%s{\"name\":\"%s\"", c ? "," : "", column.name);
    std::string rows;
    const char *error = nullptr;
    for (size_t g = 0; g < file.row_group_count && !error; ++g) {
      const auto decoded = telemetry::read_column(
          buffer_read, &buffer, file, g, c, columns.scratch, columns.output());
      if (!decoded) {
        error = telemetry::column_error_name(decoded.error());
        break;
      }
      if (*decoded != file.row_groups[g].num_rows)
        return 9;
      char text[32];
      for (size_t r = 0; r < *decoded; ++r) {
        if (!columns.valid[r]) {
          if (column.type == telemetry::PhysicalType::Float &&
              !std::isnan(columns.floats[r]))
            return 10; // a null FLOAT must come back as NaN
          snprintf(text, sizeof(text), "null");
        } else if (column.type == telemetry::PhysicalType::Float) {
          snprintf(text, sizeof(text), "%u",
                   std::bit_cast<uint32_t>(columns.floats[r]));
        } else if (column.type == telemetry::PhysicalType::Int64) {
          snprintf(text, sizeof(text), "%" PRId64, columns.int64s[r]);
        } else {
          snprintf(text, sizeof(text), "%d", columns.int32s[r]);
        }
        if (!rows.empty())
          rows += ',';
        rows += text;
      }
    }
    if (error)
      printf(",\"error\":\"%s\"}", error);
    else
      printf(",\"rows\":[%s]}", rows.c_str());
  }
  printf("]}\n");
  return 0;
}

int run_pagefuzz(const char *path, unsigned long iterations, uint64_t seed,
                 State &state, Columns &columns) {
  std::vector<uint8_t> original;
  if (!load(path, original) || !loaded_footer(original, state))
    return 3;
  const FileSummary pristine = state.memory;
  const size_t data_end = original.size() - 8 - pristine.footer_size;
  unsigned long accepted = 0, refused = 0;
  std::vector<uint8_t> bytes;
  for (unsigned long i = 0; i < iterations; ++i) {
    bytes = original;
    FileSummary &file = state.memory;
    file = pristine;
    size_t limit = bytes.size();
    const uint32_t mode = next(seed) % 7;
    const size_t edits = 1 + next(seed) % 4;
    for (size_t e = 0; e < edits; ++e) {
      const size_t at = 4 + next(seed) % (data_end - 4);
      const size_t g = next(seed) % file.row_group_count;
      const size_t c = next(seed) % file.column_count;
      auto &chunk = file.row_groups[g].columns[c];
      switch (mode) {
      case 0:
        bytes[at] ^= static_cast<uint8_t>(1U << (next(seed) % 8));
        break;
      case 1:
        bytes[at] = static_cast<uint8_t>(next(seed));
        break;
      case 2: // varint continuation or long-form headers in a page header
        bytes[chunk.data_page_offset + next(seed) % 8] |= 0x80U;
        break;
      case 3: // the read stops short, as a cut file does
        limit = at;
        break;
      case 4: // a chunk shorter or longer than its pages
        chunk.total_compressed_size += (next(seed) % 64) - 32;
        break;
      case 5: // a chunk that starts inside its page
        chunk.data_page_offset += 1 + next(seed) % 16;
        chunk.start_offset = chunk.data_page_offset;
        break;
      default: // zero a run
        for (size_t z = at; z < data_end && z < at + 8; ++z)
          bytes[z] = 0;
        break;
      }
    }
    Buffer buffer{bytes.data(), limit, 0};
    for (size_t g = 0; g < file.row_group_count; ++g) {
      for (size_t c = 0; c < file.column_count; ++c) {
        if (file.columns[c].type == telemetry::PhysicalType::ByteArray)
          continue;
        // Leave a short scratch now and then so Capacity paths run too.
        const size_t room =
            next(seed) % 5 == 0 ? next(seed) % 512 : columns.scratch.size();
        const auto decoded = telemetry::read_column(
            buffer_read, &buffer, file, g, c,
            std::span(columns.scratch).first(room), columns.output());
        if (!decoded) {
          ++refused;
          continue;
        }
        ++accepted;
        if (*decoded != file.row_groups[g].num_rows) {
          fprintf(stderr, "row count at iteration %lu\n", i);
          return 7;
        }
        for (size_t r = 0; r < *decoded; ++r)
          if (columns.valid[r] > 1)
            return 7;
      }
    }
  }
  printf("{\"iterations\":%lu,\"accepted\":%lu,\"refused\":%lu}\n", iterations,
         accepted, refused);
  return 0;
}

// A tiny Thrift Compact writer for hand-built page headers.
struct Page {
  std::vector<uint8_t> bytes;
  void varint(uint64_t v) {
    while (v >= 0x80) {
      bytes.push_back(static_cast<uint8_t>(v | 0x80));
      v >>= 7;
    }
    bytes.push_back(static_cast<uint8_t>(v));
  }
  void i32_field(uint8_t delta, int64_t v) {
    bytes.push_back(static_cast<uint8_t>(delta << 4 | 5));
    varint(static_cast<uint64_t>((v << 1) ^ (v >> 63)));
  }
  // A PageHeader with a DataPageHeader, as the writer emits it.
  void header(int type, int64_t raw, int64_t stored, int64_t values,
              int encoding = 0, int levels = 3) {
    i32_field(1, type);
    i32_field(1, raw);
    i32_field(1, stored);
    bytes.push_back(0x2c); // field 5, struct
    i32_field(1, values);
    i32_field(1, encoding);
    i32_field(1, levels);
    i32_field(1, 3);
    bytes.push_back(0);
    bytes.push_back(0);
  }
  void le(uint64_t v, size_t width) {
    for (size_t i = 0; i < width; ++i)
      bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void append(const std::vector<uint8_t> &more) {
    bytes.insert(bytes.end(), more.begin(), more.end());
  }
};

struct Case {
  const char *name;
  telemetry::PhysicalType type;
  bool nullable;
  telemetry::Codec codec;
  size_t rows;
  std::vector<uint8_t> chunk; // the pages, written after a 4 byte magic
  // nullopt means the decode must succeed.
  std::optional<telemetry::ColumnError> want;
  size_t scratch = 4096;
  int64_t chunk_delta = 0; // added to total_compressed_size
  bool fail_reads = false;
  bool dictionary = false;
};

std::vector<uint8_t> lz4(const std::vector<uint8_t> &raw) {
  std::vector<uint8_t> out(LZ4_compressBound(static_cast<int>(raw.size())));
  const int n = LZ4_compress_default(reinterpret_cast<const char *>(raw.data()),
                                     reinterpret_cast<char *>(out.data()),
                                     static_cast<int>(raw.size()),
                                     static_cast<int>(out.size()));
  out.resize(static_cast<size_t>(n));
  return out;
}

// A whole nullable FLOAT page: a 4 byte level length, the level stream and
// the present values.
std::vector<uint8_t> nullable_payload(const std::vector<uint8_t> &levels,
                                      const std::vector<float> &present) {
  Page p;
  p.le(levels.size(), 4);
  p.append(levels);
  for (const float v : present)
    p.le(std::bit_cast<uint32_t>(v), 4);
  return p.bytes;
}

std::vector<uint8_t> page(const std::vector<uint8_t> &payload, size_t values,
                          bool compress = false, int type = 0,
                          int encoding = 0) {
  Page p;
  const auto stored = compress ? lz4(payload) : payload;
  p.header(type, static_cast<int64_t>(payload.size()),
           static_cast<int64_t>(stored.size()), static_cast<int64_t>(values),
           encoding);
  p.append(stored);
  return p.bytes;
}

std::vector<uint8_t> required_int32(std::initializer_list<int32_t> values) {
  Page p;
  for (const int32_t v : values)
    p.le(static_cast<uint32_t>(v), 4);
  return p.bytes;
}

std::vector<uint8_t> prefix(std::vector<uint8_t> bytes, size_t size) {
  bytes.resize(size);
  return bytes;
}

std::vector<uint8_t> join(std::vector<uint8_t> a,
                          const std::vector<uint8_t> &b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

int run_pagecases(State &state, Columns &columns) {
  using telemetry::Codec;
  using telemetry::ColumnError;
  using telemetry::PhysicalType;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  // Rows 0..5 of a nullable FLOAT: 1.5, null, null, -2, NaN, null.
  const std::vector<uint8_t> rle = {0x02, 1, 0x04, 0, 0x04, 1, 0x02, 0};
  const std::vector<uint8_t> packed = {0x03, 0b00011001};
  const std::vector<float> present = {1.5f, -2.0f, nan};
  const auto good = nullable_payload(rle, present);
  std::vector<uint8_t> bad_lz4;
  {
    // A literal run far longer than the stored block.
    Page p;
    p.header(0, static_cast<int64_t>(good.size()), 6, 6);
    p.append({0xf0, 0xff, 0xff, 0xff, 0x00, 0x01});
    bad_lz4 = p.bytes;
  }
  std::vector<uint8_t> short_lz4;
  {
    // Stored bytes decode to one byte fewer than the header promises.
    Page p;
    const auto stored = lz4(std::vector<uint8_t>(good.begin(), good.end() - 1));
    p.header(0, static_cast<int64_t>(good.size()),
             static_cast<int64_t>(stored.size()), 6);
    p.append(stored);
    short_lz4 = p.bytes;
  }
  std::vector<uint8_t> big_header;
  {
    Page p;
    p.header(0, 8, 9000, 2, 0, 3);
    p.append(required_int32({1, 2}));
    big_header = p.bytes;
  }
  std::vector<uint8_t> raw_mismatch;
  {
    Page p;
    p.header(0, 12, 8, 2, 0, 3);
    p.append(required_int32({1, 2}));
    raw_mismatch = p.bytes;
  }
  std::vector<uint8_t> v2;
  {
    Page p;
    p.i32_field(1, 3); // DATA_PAGE_V2
    p.i32_field(1, 8);
    p.i32_field(1, 8);
    p.bytes.push_back(0);
    p.append(required_int32({1, 2}));
    v2 = p.bytes;
  }
  std::vector<uint8_t> stats_header;
  {
    // pyarrow style: a crc and Statistics inside DataPageHeader are skipped.
    Page p;
    p.i32_field(1, 0);
    p.i32_field(1, 8);
    p.i32_field(1, 8);
    p.i32_field(1, 12345);   // crc
    p.bytes.push_back(0x1c); // field 5, struct
    p.i32_field(1, 2);
    p.i32_field(1, 0);
    p.i32_field(1, 3);
    p.i32_field(1, 3);
    p.bytes.push_back(0x1c); // field 5, Statistics
    p.bytes.push_back(0x18); // field 1, binary
    p.varint(4);
    p.le(1, 4);
    p.bytes.push_back(0x36); // field 4, i64 delta 3
    p.varint(0);
    p.bytes.push_back(0);
    p.bytes.push_back(0);
    p.bytes.push_back(0);
    p.append(required_int32({7, -9}));
    stats_header = p.bytes;
  }
  const std::vector<Case> cases = {
      {.name = "rle-uncompressed",
       .type = PhysicalType::Float,
       .nullable = true,
       .codec = Codec::Uncompressed,
       .rows = 6,
       .chunk = page(good, 6),
       .want = std::nullopt},
      {.name = "rle-lz4",
       .type = PhysicalType::Float,
       .nullable = true,
       .codec = Codec::Lz4Raw,
       .rows = 6,
       .chunk = page(good, 6, true),
       .want = std::nullopt},
      {.name = "bit-packed",
       .type = PhysicalType::Float,
       .nullable = true,
       .codec = Codec::Uncompressed,
       .rows = 6,
       .chunk = page(nullable_payload(packed, present), 6),
       .want = std::nullopt},
      {.name = "two-pages",
       .type = PhysicalType::Int32,
       .nullable = false,
       .codec = Codec::Lz4Raw,
       .rows = 4,
       .chunk = join(page(required_int32({1, 2}), 2, true),
                     page(required_int32({3, 4}), 2, true)),
       .want = std::nullopt},
      {.name = "page-statistics",
       .type = PhysicalType::Int32,
       .nullable = false,
       .codec = Codec::Uncompressed,
       .rows = 2,
       .chunk = stats_header,
       .want = std::nullopt},
      {.name = "truncated-page",
       .type = PhysicalType::Float,
       .nullable = true,
       .codec = Codec::Uncompressed,
       .rows = 6,
       .chunk = page(good, 6),
       .want = ColumnError::Length,
       .chunk_delta = -3},
      {.name = "trailing-chunk-bytes",
       .type = PhysicalType::Float,
       .nullable = true,
       .codec = Codec::Uncompressed,
       .rows = 6,
       .chunk = page(good, 6),
       .want = ColumnError::PageHeader,
       .chunk_delta = 2},
      {.name = "header-cut",
       .type = PhysicalType::Float,
       .nullable = true,
       .codec = Codec::Uncompressed,
       .rows = 6,
       .chunk = prefix(page(good, 6), 5),
       .want = ColumnError::PageHeader},
      {.name = "compressed-size-past-chunk",
       .type = PhysicalType::Int32,
       .nullable = false,
       .codec = Codec::Uncompressed,
       .rows = 2,
       .chunk = big_header,
       .want = ColumnError::Length},
      {.name = "raw-size-mismatch",
       .type = PhysicalType::Int32,
       .nullable = false,
       .codec = Codec::Uncompressed,
       .rows = 2,
       .chunk = raw_mismatch,
       .want = ColumnError::Length},
      {.name = "too-many-values",
       .type = PhysicalType::Int32,
       .nullable = false,
       .codec = Codec::Uncompressed,
       .rows = 1,
       .chunk = page(required_int32({1, 2}), 2),
       .want = ColumnError::Length},
      {.name = "too-few-values",
       .type = PhysicalType::Int32,
       .nullable = false,
       .codec = Codec::Uncompressed,
       .rows = 3,
       .chunk = page(required_int32({1, 2}), 2),
       .want = ColumnError::Length},
      {.name = "bad-lz4",
       .type = PhysicalType::Float,
       .nullable = true,
       .codec = Codec::Lz4Raw,
       .rows = 6,
       .chunk = bad_lz4,
       .want = ColumnError::Lz4},
      {.name = "short-lz4",
       .type = PhysicalType::Float,
       .nullable = true,
       .codec = Codec::Lz4Raw,
       .rows = 6,
       .chunk = short_lz4,
       .want = ColumnError::Lz4},
      {.name = "level-overrun",
       .type = PhysicalType::Float,
       .nullable = true,
       .codec = Codec::Uncompressed,
       .rows = 6,
       .chunk = page(nullable_payload({0x0e, 1}, present), 6),
       .want = ColumnError::Levels},
      {.name = "level-short",
       .type = PhysicalType::Float,
       .nullable = true,
       .codec = Codec::Uncompressed,
       .rows = 6,
       .chunk = page(nullable_payload({0x02, 1}, {1.0f}), 6),
       .want = ColumnError::Levels},
      {.name = "level-value-2",
       .type = PhysicalType::Float,
       .nullable = true,
       .codec = Codec::Uncompressed,
       .rows = 6,
       .chunk = page(nullable_payload({0x0c, 2}, {}), 6),
       .want = ColumnError::Levels},
      {.name = "level-zero-run",
       .type = PhysicalType::Float,
       .nullable = true,
       .codec = Codec::Uncompressed,
       .rows = 6,
       .chunk = page(nullable_payload({0x00, 1, 0x0c, 0}, {}), 6),
       .want = ColumnError::Levels},
      {.name = "level-trailing",
       .type = PhysicalType::Float,
       .nullable = true,
       .codec = Codec::Uncompressed,
       .rows = 6,
       .chunk = page(nullable_payload({0x0c, 0, 0x02}, {}), 6),
       .want = ColumnError::Levels},
      {.name = "level-length-past-page",
       .type = PhysicalType::Float,
       .nullable = true,
       .codec = Codec::Uncompressed,
       .rows = 2,
       .chunk = page({0xff, 0, 0, 0, 0x04, 0}, 2),
       .want = ColumnError::Levels},
      {.name = "level-varint-overflow",
       .type = PhysicalType::Float,
       .nullable = true,
       .codec = Codec::Uncompressed,
       .rows = 2,
       .chunk = page(nullable_payload({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                       0xff, 0xff, 0xff, 0x01},
                                      {}),
                     2),
       .want = ColumnError::Levels},
      {.name = "values-short",
       .type = PhysicalType::Float,
       .nullable = true,
       .codec = Codec::Uncompressed,
       .rows = 6,
       .chunk = page(nullable_payload(rle, {1.5f, -2.0f}), 6),
       .want = ColumnError::Values},
      {.name = "values-long",
       .type = PhysicalType::Int32,
       .nullable = false,
       .codec = Codec::Uncompressed,
       .rows = 2,
       .chunk = page(required_int32({1, 2, 3}), 2),
       .want = ColumnError::Values},
      {.name = "dictionary-page",
       .type = PhysicalType::Int32,
       .nullable = false,
       .codec = Codec::Uncompressed,
       .rows = 2,
       .chunk = page(required_int32({1, 2}), 2, false, 2),
       .want = ColumnError::PageType},
      {.name = "data-page-v2",
       .type = PhysicalType::Int32,
       .nullable = false,
       .codec = Codec::Uncompressed,
       .rows = 2,
       .chunk = v2,
       .want = ColumnError::PageType},
      {.name = "dictionary-offset",
       .type = PhysicalType::Int32,
       .nullable = false,
       .codec = Codec::Uncompressed,
       .rows = 2,
       .chunk = page(required_int32({1, 2}), 2),
       .want = ColumnError::Encoding,
       .dictionary = true},
      {.name = "delta-encoding",
       .type = PhysicalType::Int32,
       .nullable = false,
       .codec = Codec::Uncompressed,
       .rows = 2,
       .chunk = page(required_int32({1, 2}), 2, false, 0, 5),
       .want = ColumnError::Encoding},
      {.name = "zstd",
       .type = PhysicalType::Int32,
       .nullable = false,
       .codec = static_cast<Codec>(6),
       .rows = 2,
       .chunk = page(required_int32({1, 2}), 2),
       .want = ColumnError::Codec},
      {.name = "scratch-too-small",
       .type = PhysicalType::Float,
       .nullable = true,
       .codec = Codec::Lz4Raw,
       .rows = 6,
       .chunk = page(good, 6, true),
       .want = ColumnError::Capacity,
       .scratch = 40},
      {.name = "read-fails",
       .type = PhysicalType::Int32,
       .nullable = false,
       .codec = Codec::Uncompressed,
       .rows = 2,
       .chunk = page(required_int32({1, 2}), 2),
       .want = ColumnError::Read,
       .fail_reads = true},
      {.name = "byte-array",
       .type = PhysicalType::ByteArray,
       .nullable = false,
       .codec = Codec::Uncompressed,
       .rows = 2,
       .chunk = page(required_int32({1, 2}), 2),
       .want = ColumnError::BadArgument},
  };
  size_t passed = 0;
  for (const Case &test : cases) {
    std::vector<uint8_t> bytes = {'P', 'A', 'R', '1'};
    bytes.insert(bytes.end(), test.chunk.begin(), test.chunk.end());
    bytes.resize(bytes.size() + 8); // room for a tail, never read
    FileSummary &file = state.memory;
    file = FileSummary{};
    file.file_size = bytes.size();
    file.num_rows = test.rows;
    file.column_count = 1;
    file.row_group_count = 1;
    snprintf(file.columns[0].name, telemetry::kMaxNameLength, "x");
    file.columns[0].type = test.type;
    file.columns[0].nullable = test.nullable;
    file.row_groups[0].num_rows = test.rows;
    auto &chunk = file.row_groups[0].columns[0];
    chunk.codec = test.codec;
    chunk.num_values = test.rows;
    chunk.data_page_offset = 4;
    chunk.start_offset = test.dictionary ? 0 : 4;
    chunk.total_compressed_size = static_cast<uint64_t>(
        static_cast<int64_t>(test.chunk.size()) + test.chunk_delta);
    Buffer buffer{bytes.data(), test.fail_reads ? 0 : bytes.size(), 0};
    const auto decoded = telemetry::read_column(
        buffer_read, &buffer, file, 0, 0,
        std::span(columns.scratch).first(test.scratch), columns.output());
    const bool ok = test.want ? !decoded && decoded.error() == *test.want
                              : decoded.has_value() && *decoded == test.rows;
    if (!ok) {
      fprintf(stderr, "case %s: got %s\n", test.name,
              decoded ? "ok" : telemetry::column_error_name(decoded.error()));
      return 11;
    }
    ++passed;
  }
  // The accepted cases decoded the rows they promise.
  const auto check_floats = [&](const Case &test) {
    std::vector<uint8_t> bytes = {'P', 'A', 'R', '1'};
    bytes.insert(bytes.end(), test.chunk.begin(), test.chunk.end());
    bytes.resize(bytes.size() + 8);
    FileSummary &file = state.memory;
    file.file_size = bytes.size();
    file.columns[0].type = test.type;
    file.columns[0].nullable = test.nullable;
    auto &chunk = file.row_groups[0].columns[0];
    chunk.codec = test.codec;
    chunk.start_offset = 4;
    chunk.total_compressed_size = test.chunk.size();
    file.row_groups[0].num_rows = chunk.num_values = test.rows;
    Buffer buffer{bytes.data(), bytes.size(), 0};
    const auto decoded = telemetry::read_column(
        buffer_read, &buffer, file, 0, 0, columns.scratch, columns.output());
    const uint8_t mask[6] = {1, 0, 0, 1, 1, 0};
    if (!decoded || *decoded != 6)
      return false;
    for (size_t r = 0; r < 6; ++r)
      if (columns.valid[r] != mask[r] ||
          (!mask[r] && !std::isnan(columns.floats[r])))
        return false;
    return columns.floats[0] == 1.5f && columns.floats[3] == -2.0f &&
           std::isnan(columns.floats[4]);
  };
  for (size_t i = 0; i < 3; ++i)
    if (!check_floats(cases[i])) {
      fprintf(stderr, "case %s decoded the wrong rows\n", cases[i].name);
      return 12;
    }
  printf("{\"cases\":%zu}\n", passed);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  // FileSummary and metrics are large; keep them off the stack as the
  // firmware must.
  auto state = std::make_unique<State>();
  if (argc == 3 && strcmp(argv[1], "dump") == 0)
    return run_dump(argv[2], *state);
  if (argc == 5 && strcmp(argv[1], "fuzz") == 0)
    return run_fuzz(argv[2], strtoul(argv[3], nullptr, 10),
                    strtoull(argv[4], nullptr, 10), *state);
  auto columns = std::make_unique<Columns>();
  if (argc == 3 && strcmp(argv[1], "columns") == 0)
    return run_columns(argv[2], *state, *columns);
  if (argc == 5 && strcmp(argv[1], "pagefuzz") == 0)
    return run_pagefuzz(argv[2], strtoul(argv[3], nullptr, 10),
                        strtoull(argv[4], nullptr, 10), *state, *columns);
  if (argc == 2 && strcmp(argv[1], "pagecases") == 0)
    return run_pagecases(*state, *columns);
  return 2;
}
