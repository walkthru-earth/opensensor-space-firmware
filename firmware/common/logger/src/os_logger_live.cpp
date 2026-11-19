#include "os_logger_live.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace oslogger {
namespace {
bool key_chars(const char *key) {
  const auto length = std::strlen(key);
  if (length == 0 || length > kMaxLiveKey)
    return false;
  for (const char *p = key; *p; ++p)
    if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9') && *p != '_')
      return false;
  return true;
}

bool usable_table(const Field *fields, std::size_t count) {
  return (fields || count == 0) && count <= telemetry::kMaxColumns;
}

struct Rendered {
  std::size_t used = 0;   // bytes written to `out`, without the NUL
  std::size_t needed = 0; // bytes the whole object needs, without the NUL
  bool fits = true;
};

// Writes the LIVE object into `out` while it fits and always counts the bytes
// the whole object needs, so an overflow can report its real size. A null
// `out` only counts.
Rendered render(const Field *fields, std::size_t count, const Row &row,
                char *out, std::size_t capacity) {
  Rendered result;
  result.fits = out && capacity > 0;
  // Appends literal text. A value that would not fit marks the whole document
  // as failed instead of publishing a truncated object.
  const auto append = [&](const char *text) {
    const auto length = std::strlen(text);
    result.needed += length;
    if (!result.fits || length >= capacity - result.used) {
      result.fits = false;
      return;
    }
    std::memcpy(out + result.used, text, length + 1);
    result.used += length;
  };
  append("{");
  bool first = true;
  for (std::size_t i = 0; i < count; ++i) {
    const auto &field = fields[i];
    if (!field.live_key)
      continue;
    // Every declared key is always sent. A value with no usable reading is
    // JSON null, never left out and never zero.
    char value[32] = "null";
    if (row.valid[i]) {
      char text[sizeof(value)];
      int written = -1;
      switch (field.type) {
      case telemetry::PhysicalType::Int64: {
        // Every key carries its column's unit, so a UTC instant is sent in
        // the microseconds the file stores.
        written = std::snprintf(text, sizeof(text), "%lld",
                                static_cast<long long>(row.data[i]));
        break;
      }
      case telemetry::PhysicalType::Int32: {
        std::int32_t number;
        std::memcpy(&number, &row.data[i], sizeof(number));
        written =
            std::snprintf(text, sizeof(text), "%ld", static_cast<long>(number));
        break;
      }
      case telemetry::PhysicalType::Float: {
        float number;
        std::memcpy(&number, &row.data[i], sizeof(number));
        if (std::isfinite(number))
          written =
              std::snprintf(text, sizeof(text), "%.*f", int(field.decimals),
                            static_cast<double>(number));
        break;
      }
      case telemetry::PhysicalType::ByteArray:
        // valid_live_keys refuses a key on a byte array column, so a
        // GEOMETRY point never reaches LIVE. Kept null if one ever does.
        break;
      }
      // Non-finite floats and values wider than the scratch buffer stay null.
      if (written >= 0 && std::size_t(written) < sizeof(text))
        std::memcpy(value, text, std::size_t(written) + 1);
    }
    append(first ? "\"" : ",\"");
    append(field.live_key);
    append("\":");
    append(value);
    first = false;
  }
  append("}");
  return result;
}
} // namespace

bool valid_live_keys(const Field *fields, std::size_t count) {
  if (count && !fields)
    return false;
  for (std::size_t i = 0; i < count; ++i) {
    if (fields[i].decimals > kMaxLiveDecimals)
      return false;
    const char *key = fields[i].live_key;
    if (!key)
      continue;
    // LIVE carries numbers only, a GEOMETRY point stays in the file.
    if (!key_chars(key) || fields[i].type == telemetry::PhysicalType::ByteArray)
      return false;
    for (std::size_t j = 0; j < i; ++j)
      if (fields[j].live_key && std::strcmp(fields[j].live_key, key) == 0)
        return false;
  }
  return true;
}

std::size_t format_live(const Field *fields, std::size_t count, const Row &row,
                        char *out, std::size_t capacity) {
  if (!out || capacity == 0 || !usable_table(fields, count))
    return 0;
  const auto result = render(fields, count, row, out, capacity);
  if (!result.fits) {
    out[0] = '\0';
    return 0;
  }
  return result.used;
}

std::size_t live_size(const Field *fields, std::size_t count, const Row &row) {
  if (!usable_table(fields, count))
    return 0;
  return render(fields, count, row, nullptr, 0).needed;
}

std::size_t format_live_overflow(char *out, std::size_t capacity,
                                 std::size_t needed, std::size_t max) {
  if (!out || capacity == 0)
    return 0;
  const int written = std::snprintf(
      out, capacity, "PARQUET LIVE overflow bytes=%lu max=%lu\n",
      static_cast<unsigned long>(needed), static_cast<unsigned long>(max));
  if (written < 0 || std::size_t(written) >= capacity) {
    out[0] = '\0';
    return 0;
  }
  return std::size_t(written);
}

LivePublish LiveOverflow::publish(const Field *fields, std::size_t count,
                                  const Row &row, char *out,
                                  std::size_t capacity, std::int64_t now_us) {
  LivePublish result;
  if (!out || capacity == 0 || !usable_table(fields, count))
    return result;
  result.length = format_live(fields, count, row, out, capacity);
  if (result.length)
    return result;
  // The object does not fit. Publish every declared key as null so no reader
  // keeps the previous sample as current, and log at most once per
  // kRepeatUs. The all-null object is short and the board contract tests
  // prove it fits, so length stays zero only for a broken table.
  result.needed = live_size(fields, count, row);
  static constexpr Row kNoReading{};
  result.length = format_live(fields, count, kNoReading, out, capacity);
  if (!logged_ || now_us - last_log_us_ >= kRepeatUs) {
    logged_ = true;
    last_log_us_ = now_us;
    result.log = true;
  }
  return result;
}
} // namespace oslogger
