#pragma once

// Bounded Parquet column page decoder for the flat files parquet_writer.h
// emits, used to read finalized rows back on device (the CrowPanel chart
// history after a reset) and by the host reader test.
//
// One call decodes one column chunk of one row group. It reads the Thrift
// Compact PageHeader of every DATA_PAGE (v1) in the chunk, decompresses an
// LZ4_RAW payload into caller scratch, expands the RLE/bit-packed definition
// levels (bit width 1, a flat optional leaf) and the PLAIN values into
// caller spans. Dictionary pages, DataPageV2, DELTA encodings, other codecs
// and BYTE_ARRAY columns are refused with an error, never guessed at.
//
// Zero heap. The caller owns the FileSummary from read_parquet_footer(), the
// scratch and the outputs. Every length in a page header is checked against
// the chunk, the scratch and the row count before it is used, so a corrupt
// or truncated file fails with a ColumnError and never reads or writes out
// of bounds. Stack use stays well under 1 KB (checked by the host test).

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include "parquet_reader.h"

namespace telemetry {

enum class [[nodiscard]] ColumnError : uint8_t {
  BadArgument, // index out of range, BYTE_ARRAY, or an output too small
  Read,        // ReadAt failed
  PageHeader,  // malformed or truncated Thrift PageHeader
  PageType,    // not a v1 DATA_PAGE (dictionary, index or v2 page)
  Encoding,    // a value encoding other than PLAIN, levels other than RLE
  Codec,       // a codec other than UNCOMPRESSED or LZ4_RAW
  Lz4,         // LZ4 block did not decode to exactly the promised size
  Length,      // page sizes disagree with the chunk or with each other
  Levels,      // definition level stream malformed or wrong row count
  Values,      // value bytes disagree with the present row count
  Capacity,    // scratch smaller than the page needs
};

[[nodiscard]] const char *column_error_name(ColumnError error);

// Where one column chunk lands. Only the span that matches the column's
// physical type is used and it must hold at least the row group's rows.
// `valid` gets 1 for a present row and 0 for a null one. A null FLOAT row
// is written as NaN and a null integer row as 0.
struct ColumnOutput {
  std::span<float> floats{};
  std::span<int64_t> int64s{};
  std::span<int32_t> int32s{};
  std::span<uint8_t> valid{};
};

// The first column of `file` whose name equals `name`.
[[nodiscard]] std::optional<size_t> find_column(const FileSummary &file,
                                                std::string_view name);

// Decodes column `column` of row group `group`. Returns the rows written,
// which is always the row group's num_rows.
[[nodiscard]] std::expected<size_t, ColumnError>
read_column(ReadAt read_at, void *context, const FileSummary &file,
            size_t group, size_t column, std::span<uint8_t> scratch,
            const ColumnOutput &out);

} // namespace telemetry
