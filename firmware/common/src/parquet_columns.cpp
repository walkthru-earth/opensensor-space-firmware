#include "parquet_columns.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <type_traits>
#include <utility>

#include "lz4_codec.h"

namespace telemetry {
namespace {

// Thrift Compact type ids (parquet.thrift is encoded with TCompactProtocol).
constexpr uint8_t kBoolTrue = 1, kBoolFalse = 2, kByte = 3, kI16 = 4, kI32 = 5,
                  kI64 = 6, kDouble = 7, kBinary = 8, kList = 9, kSet = 10,
                  kMap = 11, kStruct = 12;
// Nesting we accept while skipping fields we do not need. A PageHeader is
// two levels deep; Statistics add one more.
constexpr int kMaxDepth = 6;
// Bytes of a PageHeader we look at. Our writer emits about 20, pyarrow with
// page statistics a few dozen. A longer header is refused, never read past.
constexpr size_t kMaxHeaderBytes = 256;

constexpr int32_t kDataPage = 0;
constexpr int32_t kPlain = 0;
constexpr int32_t kRle = 3;
constexpr int32_t kDeltaBinaryPacked = 5;

class Thrift {
public:
  explicit Thrift(std::span<const uint8_t> bytes) : bytes_(bytes) {}

  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] size_t used() const { return at_; }

  uint8_t byte() {
    if (at_ >= bytes_.size()) {
      ok_ = false;
      return 0;
    }
    return bytes_[at_++];
  }

  uint64_t varint() {
    uint64_t value = 0;
    for (int shift = 0; shift < 64 && ok_; shift += 7) {
      const uint8_t b = byte();
      value |= static_cast<uint64_t>(b & 0x7f) << shift;
      if (!(b & 0x80))
        return value;
    }
    ok_ = false;
    return 0;
  }

  int64_t zigzag() {
    const uint64_t v = varint();
    return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
  }

  int32_t i32() {
    const int64_t v = zigzag();
    if (v < std::numeric_limits<int32_t>::min() ||
        v > std::numeric_limits<int32_t>::max()) {
      ok_ = false;
      return 0;
    }
    return static_cast<int32_t>(v);
  }

  // Next field header. Returns false at STOP or on error. `last` is the
  // previous field id of this struct, as the compact delta needs.
  bool field(int16_t &last, int16_t &id, uint8_t &type) {
    const uint8_t header = byte();
    if (!ok_ || header == 0)
      return false;
    type = header & 0x0f;
    const uint8_t delta = header >> 4;
    if (delta == 0) {
      const int64_t raw = zigzag();
      if (raw < 0 || raw > std::numeric_limits<int16_t>::max()) {
        ok_ = false;
        return false;
      }
      id = static_cast<int16_t>(raw);
    } else {
      id = static_cast<int16_t>(last + delta);
    }
    last = id;
    return ok_;
  }

  void skip(uint8_t type, int depth) {
    if (depth > kMaxDepth) {
      ok_ = false;
      return;
    }
    switch (type) {
    case kBoolTrue:
    case kBoolFalse:
      return; // a struct field's bool lives in its header
    case kByte:
      byte();
      return;
    case kI16:
    case kI32:
    case kI64:
      varint();
      return;
    case kDouble:
      advance(8);
      return;
    case kBinary:
      advance(varint());
      return;
    case kList:
    case kSet: {
      const uint8_t header = byte();
      uint64_t count = header >> 4;
      const uint8_t element = header & 0x0f;
      if (count == 15)
        count = varint();
      // Every element is at least one byte, so a count past the remaining
      // bytes is corrupt and the loop below stays bounded by the input.
      if (count > bytes_.size() - std::min(at_, bytes_.size())) {
        ok_ = false;
        return;
      }
      for (uint64_t i = 0; i < count && ok_; ++i) {
        if (element == kBoolTrue || element == kBoolFalse)
          byte(); // a list's bool takes a byte
        else
          skip(element, depth + 1);
      }
      return;
    }
    case kMap: {
      const uint64_t count = varint();
      if (count == 0)
        return;
      const uint8_t types = byte();
      if (count > bytes_.size() - std::min(at_, bytes_.size())) {
        ok_ = false;
        return;
      }
      for (uint64_t i = 0; i < count && ok_; ++i) {
        skip(types >> 4, depth + 1);
        skip(types & 0x0f, depth + 1);
      }
      return;
    }
    case kStruct: {
      int16_t last = 0, id = 0;
      uint8_t inner = 0;
      while (field(last, id, inner))
        skip(inner, depth + 1);
      return;
    }
    default:
      ok_ = false;
    }
  }

private:
  void advance(uint64_t size) {
    if (size > bytes_.size() - std::min(at_, bytes_.size())) {
      ok_ = false;
      return;
    }
    at_ += static_cast<size_t>(size);
  }

  std::span<const uint8_t> bytes_;
  size_t at_ = 0;
  bool ok_ = true;
};

struct Page {
  int32_t type = -1;
  int32_t uncompressed = -1;
  int32_t compressed = -1;
  int32_t num_values = -1;
  int32_t encoding = -1;
  int32_t definition_encoding = -1;
  size_t header_bytes = 0;
};

[[nodiscard]] std::expected<Page, ColumnError>
page_header(std::span<const uint8_t> bytes) {
  Thrift in(bytes);
  Page page{};
  bool data_header = false;
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  while (in.field(last, id, type)) {
    if (id >= 1 && id <= 3 && type != kI32)
      return std::unexpected(ColumnError::PageHeader);
    if (id == 1)
      page.type = in.i32();
    else if (id == 2)
      page.uncompressed = in.i32();
    else if (id == 3)
      page.compressed = in.i32();
    else if (id == 5 && type == kStruct) {
      data_header = true;
      int16_t inner_last = 0, inner_id = 0;
      uint8_t inner = 0;
      while (in.field(inner_last, inner_id, inner)) {
        if (inner_id >= 1 && inner_id <= 3 && inner != kI32)
          return std::unexpected(ColumnError::PageHeader);
        if (inner_id == 1)
          page.num_values = in.i32();
        else if (inner_id == 2)
          page.encoding = in.i32();
        else if (inner_id == 3)
          page.definition_encoding = in.i32();
        else
          in.skip(inner, 2);
      }
    } else {
      in.skip(type, 1);
    }
  }
  if (!in.ok() || page.type < 0 || page.uncompressed < 0 || page.compressed < 0)
    return std::unexpected(ColumnError::PageHeader);
  if (page.type != kDataPage)
    return std::unexpected(ColumnError::PageType);
  if (!data_header || page.num_values < 0 || page.encoding < 0 ||
      page.definition_encoding < 0)
    return std::unexpected(ColumnError::PageHeader);
  page.header_bytes = in.used();
  return page;
}

uint64_t little_endian(const uint8_t *bytes, size_t width) {
  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i)
    value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
  return value;
}

// Expands a bit width 1 RLE/bit-packed hybrid stream into `valid`, one byte
// per value. Every value must be 0 or 1, the runs must cover exactly
// valid.size() values, and the stream must end exactly at its last run (the
// last bit-packed group may carry padding bits, which are ignored).
[[nodiscard]] std::expected<size_t, ColumnError>
definition_levels(std::span<const uint8_t> stream, std::span<uint8_t> valid) {
  Thrift in(stream); // only its bounded varint and byte are used
  size_t produced = 0, present = 0;
  while (produced < valid.size()) {
    const uint64_t header = in.varint();
    if (!in.ok())
      return std::unexpected(ColumnError::Levels);
    if ((header & 1) == 0) {
      const uint64_t run = header >> 1;
      const uint8_t value = in.byte();
      if (!in.ok() || run == 0 || value > 1 || run > valid.size() - produced)
        return std::unexpected(ColumnError::Levels);
      for (uint64_t i = 0; i < run; ++i)
        valid[produced++] = value;
      present += value ? run : 0;
      continue;
    }
    const uint64_t groups = header >> 1;
    if (groups == 0 || groups > stream.size())
      return std::unexpected(ColumnError::Levels);
    for (uint64_t g = 0; g < groups; ++g) {
      const uint8_t bits = in.byte();
      if (!in.ok())
        return std::unexpected(ColumnError::Levels);
      for (int bit = 0; bit < 8; ++bit) {
        if (produced == valid.size()) {
          // Only the final group may hold padding past the last value.
          if (g + 1 != groups)
            return std::unexpected(ColumnError::Levels);
          break;
        }
        const uint8_t value = (bits >> bit) & 1;
        valid[produced++] = value;
        present += value;
      }
    }
  }
  if (in.used() != stream.size())
    return std::unexpected(ColumnError::Levels);
  return present;
}

size_t value_width(PhysicalType type) {
  return type == PhysicalType::Int32 || type == PhysicalType::Float ? 4 : 8;
}

// Writes the present values of one page, in row order, and the null fill.
[[nodiscard]] std::expected<void, ColumnError>
values(std::span<const uint8_t> bytes, PhysicalType type,
       std::span<const uint8_t> valid, size_t first, const ColumnOutput &out) {
  const size_t width = value_width(type);
  const size_t present = std::accumulate(valid.begin(), valid.end(), size_t{0});
  if (bytes.size() != present * width)
    return std::unexpected(ColumnError::Values);
  size_t at = 0;
  for (size_t i = 0; i < valid.size(); ++i) {
    const size_t row = first + i;
    out.valid[row] = valid[i];
    if (!valid[i]) {
      if (type == PhysicalType::Float)
        out.floats[row] = std::numeric_limits<float>::quiet_NaN();
      else if (type == PhysicalType::Int64)
        out.int64s[row] = 0;
      else
        out.int32s[row] = 0;
      continue;
    }
    const uint64_t bits = little_endian(bytes.data() + at, width);
    at += width;
    if (type == PhysicalType::Float)
      out.floats[row] = std::bit_cast<float>(static_cast<uint32_t>(bits));
    else if (type == PhysicalType::Int64)
      out.int64s[row] = std::bit_cast<int64_t>(bits);
    else
      out.int32s[row] = std::bit_cast<int32_t>(static_cast<uint32_t>(bits));
  }
  return {};
}

// DELTA_BINARY_PACKED, as apache/parquet-format Encodings.md defines it.
// A header of ULEB128 block size, miniblocks per block and total value
// count, then the zigzag first value. Each block holds a zigzag min delta,
// one bit width byte per miniblock and the miniblocks, bit packed LSB first
// and padded to full size. In the last block the widths of miniblocks past
// the last value are present with any value, and those miniblocks have no
// bytes. Arithmetic wraps in the column's two's complement width.
class DeltaStream {
public:
  explicit DeltaStream(std::span<const uint8_t> bytes) : bytes_(bytes) {}

  [[nodiscard]] size_t left() const { return bytes_.size() - at_; }

  // ULEB128 of at most 10 bytes whose value fits 64 bits. Overlong or
  // truncated input is an error.
  [[nodiscard]] std::expected<uint64_t, ColumnError> uleb() {
    uint64_t value = 0;
    for (unsigned i = 0; i < 10; ++i) {
      if (at_ >= bytes_.size())
        return std::unexpected(ColumnError::Delta);
      const uint8_t b = bytes_[at_++];
      if (i == 9 && b > 1)
        return std::unexpected(ColumnError::Delta);
      value |= static_cast<uint64_t>(b & 0x7f) << (7 * i);
      if (!(b & 0x80))
        return value;
    }
    return std::unexpected(ColumnError::Delta);
  }

  [[nodiscard]] std::expected<int64_t, ColumnError> zigzag() {
    const auto raw = uleb();
    if (!raw)
      return std::unexpected(raw.error());
    return std::bit_cast<int64_t>((*raw >> 1) ^ (0 - (*raw & 1)));
  }

  [[nodiscard]] std::expected<std::span<const uint8_t>, ColumnError>
  take(uint64_t size) {
    if (size > left())
      return std::unexpected(ColumnError::Delta);
    const auto bytes = bytes_.subspan(at_, static_cast<size_t>(size));
    at_ += static_cast<size_t>(size);
    return bytes;
  }

private:
  std::span<const uint8_t> bytes_;
  size_t at_ = 0;
};

// Value `index` of a miniblock packed LSB first at `width` bits. The caller
// has checked that the body holds every bit of it.
uint64_t unpack(std::span<const uint8_t> body, size_t index, unsigned width) {
  uint64_t value = 0;
  size_t bit = index * width;
  for (unsigned got = 0; got < width;) {
    const unsigned offset = bit % 8;
    const unsigned part = std::min(8U - offset, width - got);
    const uint64_t bits =
        static_cast<uint64_t>(body[bit / 8] >> offset) & ((1U << part) - 1U);
    value |= bits << got;
    got += part;
    bit += part;
  }
  return value;
}

// The largest block we accept. The spec bounds it only by ULEB128, and this
// keeps every size product below 2^64.
constexpr uint64_t kMaxDeltaBlock = std::numeric_limits<uint32_t>::max();

// Decodes exactly out.size() values. The stream must state that count and
// end at its last miniblock, so a short, long or padded stream is refused.
template <typename T>
[[nodiscard]] std::expected<void, ColumnError>
delta_values(std::span<const uint8_t> bytes, std::span<T> out) {
  using U = std::make_unsigned_t<T>;
  constexpr unsigned kBits = 8 * sizeof(T);
  DeltaStream in(bytes);
  const auto block = in.uleb();
  const auto miniblocks = in.uleb();
  const auto count = in.uleb();
  const auto first = in.zigzag();
  if (!block || !miniblocks || !count || !first)
    return std::unexpected(ColumnError::Delta);
  if (*block == 0 || *block % 128 != 0 || *block > kMaxDeltaBlock ||
      *miniblocks == 0 || *block % *miniblocks != 0 ||
      (*block / *miniblocks) % 32 != 0 || *count != out.size())
    return std::unexpected(ColumnError::Delta);
  if (*first < std::numeric_limits<T>::min() ||
      *first > std::numeric_limits<T>::max())
    return std::unexpected(ColumnError::Delta);
  // Both stay below 2^32, so the body size below fits 64 bits.
  const uint64_t per_miniblock = *block / *miniblocks;
  const uint64_t widths_per_block = *miniblocks;
  size_t done = 0;
  U last = static_cast<U>(*first);
  if (!out.empty())
    out[done++] = std::bit_cast<T>(last);
  while (done < out.size()) {
    const auto min_delta = in.zigzag();
    if (!min_delta || *min_delta < std::numeric_limits<T>::min() ||
        *min_delta > std::numeric_limits<T>::max())
      return std::unexpected(ColumnError::Delta);
    const auto widths = in.take(widths_per_block);
    if (!widths)
      return std::unexpected(widths.error());
    for (const uint8_t width : *widths) {
      // Widths past the last value are arbitrary and carry no body.
      if (done == out.size())
        continue;
      if (width > kBits)
        return std::unexpected(ColumnError::Delta);
      const auto body = in.take(per_miniblock / 8 * width);
      if (!body)
        return std::unexpected(body.error());
      const auto n = static_cast<size_t>(
          std::min<uint64_t>(per_miniblock, out.size() - done));
      for (size_t i = 0; i < n; ++i) {
        last += static_cast<U>(*min_delta) +
                static_cast<U>(unpack(*body, i, width));
        out[done++] = std::bit_cast<T>(last);
      }
    }
  }
  if (in.left() != 0)
    return std::unexpected(ColumnError::Delta);
  return {};
}

// Moves the `present` decoded values at the front of `rows` to the rows
// `valid` marks, and writes 0 to the null rows. Walking backwards never
// overwrites a value before it moves, since a present value only moves on.
template <typename T>
void spread(std::span<T> rows, std::span<const uint8_t> valid,
            size_t present) {
  size_t from = present;
  for (size_t i = rows.size(); i-- > 0;)
    rows[i] = valid[i] ? rows[--from] : T{0};
}

// Decodes a DELTA_BINARY_PACKED INT32 or INT64 page body into its rows.
[[nodiscard]] std::expected<void, ColumnError>
delta_page(std::span<const uint8_t> bytes, PhysicalType type,
           std::span<const uint8_t> valid, size_t first,
           const ColumnOutput &out) {
  const size_t present = std::accumulate(valid.begin(), valid.end(), size_t{0});
  if (type == PhysicalType::Int64) {
    const auto rows = out.int64s.subspan(first, valid.size());
    const auto decoded = delta_values(bytes, rows.first(present));
    if (!decoded)
      return decoded;
    spread(rows, valid, present);
    return {};
  }
  if (type != PhysicalType::Int32)
    return std::unexpected(ColumnError::Encoding);
  const auto rows = out.int32s.subspan(first, valid.size());
  const auto decoded = delta_values(bytes, rows.first(present));
  if (!decoded)
    return decoded;
  spread(rows, valid, present);
  return {};
}

// Reads one page's stored bytes at `at`, decompresses them and writes its
// rows to `out` from row `first`. A separate frame keeps read_column small.
[[nodiscard]] std::expected<void, ColumnError>
page_values(ReadAt read_at, void *context, uint64_t at, const Page &page,
            Codec codec, const SchemaColumn &schema, std::span<uint8_t> scratch,
            size_t first, const ColumnOutput &out) {
  const auto values_in_page = static_cast<size_t>(page.num_values);
  const auto raw_size = static_cast<size_t>(page.uncompressed);
  const auto stored = static_cast<size_t>(page.compressed);
  // Scratch holds the stored bytes and, for LZ4_RAW, the decoded payload
  // after them, since LZ4 cannot decode in place.
  const bool lz4 = codec == Codec::Lz4Raw;
  if (stored > scratch.size() || (lz4 && raw_size > scratch.size() - stored))
    return std::unexpected(ColumnError::Capacity);
  if (stored && !read_at(context, at, scratch.data(), stored))
    return std::unexpected(ColumnError::Read);
  std::span<const uint8_t> payload = scratch.first(stored);
  if (lz4) {
    if (stored > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        raw_size > static_cast<size_t>(std::numeric_limits<int>::max()))
      return std::unexpected(ColumnError::Length);
    const int decoded_bytes = LZ4_decompress_safe(
        reinterpret_cast<const char *>(scratch.data()),
        reinterpret_cast<char *>(scratch.data() + stored),
        static_cast<int>(stored), static_cast<int>(raw_size));
    if (decoded_bytes < 0 || static_cast<size_t>(decoded_bytes) != raw_size)
      return std::unexpected(ColumnError::Lz4);
    payload = scratch.subspan(stored, raw_size);
  }

  const std::span<uint8_t> page_valid =
      out.valid.subspan(first, values_in_page);
  std::span<const uint8_t> value_bytes = payload;
  if (schema.nullable) {
    if (payload.size() < 4)
      return std::unexpected(ColumnError::Levels);
    const uint64_t level_bytes = little_endian(payload.data(), 4);
    if (level_bytes > payload.size() - 4)
      return std::unexpected(ColumnError::Levels);
    const auto levels = definition_levels(
        payload.subspan(4, static_cast<size_t>(level_bytes)), page_valid);
    if (!levels)
      return std::unexpected(levels.error());
    value_bytes = payload.subspan(4 + static_cast<size_t>(level_bytes));
  } else {
    std::fill(page_valid.begin(), page_valid.end(), uint8_t{1});
  }
  if (page.encoding == kDeltaBinaryPacked)
    return delta_page(value_bytes, schema.type, page_valid, first, out);
  // values() reads the mask from out.valid and writes it back unchanged.
  return values(value_bytes, schema.type, page_valid, first, out);
}

} // namespace

const char *column_error_name(ColumnError error) {
  using enum ColumnError;
  switch (error) {
  case BadArgument:
    return "argument";
  case Read:
    return "read";
  case PageHeader:
    return "page-header";
  case PageType:
    return "page-type";
  case Encoding:
    return "encoding";
  case Codec:
    return "codec";
  case Lz4:
    return "lz4";
  case Length:
    return "length";
  case Levels:
    return "levels";
  case Values:
    return "values";
  case Capacity:
    return "capacity";
  case Delta:
    return "delta";
  }
  return "unknown";
}

std::optional<size_t> find_column(const FileSummary &file,
                                  std::string_view name) {
  for (size_t c = 0; c < file.column_count && c < kMaxColumns; ++c) {
    const auto &column = file.columns[c];
    if (std::string_view(column.name, strnlen(column.name, kMaxNameLength)) ==
        name)
      return c;
  }
  return std::nullopt;
}

std::expected<size_t, ColumnError> read_column(ReadAt read_at, void *context,
                                               const FileSummary &file,
                                               size_t group, size_t column,
                                               std::span<uint8_t> scratch,
                                               const ColumnOutput &out) {
  if (!read_at || group >= file.row_group_count || group >= kMaxRowGroups ||
      column >= file.column_count || column >= kMaxColumns)
    return std::unexpected(ColumnError::BadArgument);
  const SchemaColumn &schema = file.columns[column];
  const ColumnChunkSummary &chunk = file.row_groups[group].columns[column];
  const uint64_t rows64 = file.row_groups[group].num_rows;
  if (rows64 > out.valid.size())
    return std::unexpected(ColumnError::BadArgument);
  const auto rows = static_cast<size_t>(rows64);
  switch (schema.type) {
  case PhysicalType::Float:
    if (out.floats.size() < rows)
      return std::unexpected(ColumnError::BadArgument);
    break;
  case PhysicalType::Int64:
    if (out.int64s.size() < rows)
      return std::unexpected(ColumnError::BadArgument);
    break;
  case PhysicalType::Int32:
    if (out.int32s.size() < rows)
      return std::unexpected(ColumnError::BadArgument);
    break;
  default:
    return std::unexpected(ColumnError::BadArgument);
  }
  if (chunk.codec != Codec::Uncompressed && chunk.codec != Codec::Lz4Raw)
    return std::unexpected(ColumnError::Codec);
  // A dictionary page sits before the first data page.
  if (chunk.start_offset != chunk.data_page_offset)
    return std::unexpected(ColumnError::Encoding);
  // The footer reader already bounds the chunk by the data region. Check
  // again here so this function stands on its own.
  const uint64_t begin = chunk.data_page_offset;
  if (chunk.num_values != rows64 || file.file_size < 8 ||
      begin > file.file_size ||
      chunk.total_compressed_size > file.file_size - begin)
    return std::unexpected(ColumnError::Length);
  const uint64_t end = begin + chunk.total_compressed_size;

  uint64_t at = begin;
  size_t decoded = 0;
  while (decoded < rows || at < end) {
    if (at >= end)
      return std::unexpected(ColumnError::Length);
    const size_t window =
        static_cast<size_t>(std::min<uint64_t>(end - at, kMaxHeaderBytes));
    if (window > scratch.size())
      return std::unexpected(ColumnError::Capacity);
    if (!read_at(context, at, scratch.data(), window))
      return std::unexpected(ColumnError::Read);
    const auto header = page_header(scratch.first(window));
    if (!header)
      return std::unexpected(header.error());
    const Page &page = *header;
    const bool integer = schema.type == PhysicalType::Int32 ||
                         schema.type == PhysicalType::Int64;
    if (page.encoding != kPlain &&
        !(integer && page.encoding == kDeltaBinaryPacked))
      return std::unexpected(ColumnError::Encoding);
    if (schema.nullable && page.definition_encoding != kRle)
      return std::unexpected(ColumnError::Encoding);
    const auto values_in_page = static_cast<size_t>(page.num_values);
    const auto stored = static_cast<size_t>(page.compressed);
    at += page.header_bytes;
    if (values_in_page > rows - decoded || stored > end - at)
      return std::unexpected(ColumnError::Length);
    if (chunk.codec == Codec::Uncompressed &&
        stored != static_cast<size_t>(page.uncompressed))
      return std::unexpected(ColumnError::Length);

    const auto written = page_values(read_at, context, at, page, chunk.codec,
                                     schema, scratch, decoded, out);
    if (!written)
      return std::unexpected(written.error());
    at += stored;
    decoded += values_in_page;
  }
  return decoded;
}

} // namespace telemetry
