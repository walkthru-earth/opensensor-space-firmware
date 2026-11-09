#pragma once

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <system_error>

// Bounded RFC 8259 JSON writer shared by the Parquet footer and the STAC File
// Item. No heap and no formatting library.
//
// Two modes.
// - Buffer. The whole document goes into the span and finish() adds a NUL.
//   A document that does not fit, NUL included, fails with Overflow.
// - Sink. The span is a staging chunk. Each full chunk goes to the sink and
//   finish() hands over the rest, with no NUL. `limit` caps the total bytes.
//   A sink that fails stops every later write.
//
// The first error sticks and finish() reports it. The caller drops the
// output then, so a short document is never mistaken for a whole one.
namespace opensensor::json {

enum class [[nodiscard]] Error : std::uint8_t {
  // The document, or its NUL in buffer mode, does not fit.
  Overflow,
  // A double is NaN or infinite, which JSON cannot hold.
  NotFinite,
  // Unbalanced or misplaced object, array or key calls.
  Structure,
  // The sink refused a chunk.
  Sink,
};

[[nodiscard]] constexpr const char *error_name(Error error) {
  switch (error) {
  case Error::Overflow:
    return "json-overflow";
  case Error::NotFinite:
    return "json-not-finite";
  case Error::Structure:
    return "json-structure";
  case Error::Sink:
    return "json-sink";
  }
  return "json";
}

// Receives full staging chunks and the final rest. Returns false on failure.
struct Sink {
  bool (*write)(void *context, std::span<const char> bytes) = nullptr;
  void *context = nullptr;
};

// Longest text format_real() writes. "-1.2345678901234567e-308" is 24 bytes.
inline constexpr std::size_t kRealBytes = 32;

// Writes `value` the way ECMAScript Number::toString does (ECMA-262
// Number::toString with radix 10), so the text matches JSON.stringify.
// std::to_chars picks the shortest digits that round trip and, among those,
// the ones closest to the value with ties to even ([charconv.to.chars]),
// which is the choice the ECMA-262 note asks for. This only places those
// digits. -0 writes "0", as JSON.stringify does. NaN and infinity fail.
[[nodiscard]] inline std::expected<std::size_t, Error>
format_real(std::span<char, kRealBytes> out, double value) {
  if (!std::isfinite(value))
    return std::unexpected(Error::NotFinite);
  std::size_t used = 0;
  if (value == 0.0) {
    out[used++] = '0';
    return used;
  }
  if (std::signbit(value)) {
    out[used++] = '-';
    value = -value;
  }
  // d[.ddd]e(+|-)x, at most 17 digits and a 3 digit exponent.
  std::array<char, kRealBytes> scientific{};
  const auto [end, error] =
      std::to_chars(scientific.data(), scientific.data() + scientific.size(),
                    value, std::chars_format::scientific);
  if (error != std::errc{})
    return std::unexpected(Error::Overflow);
  std::array<char, 17> digits{};
  std::size_t k = 0;
  const char *cursor = scientific.data();
  for (; cursor != end && *cursor != 'e'; ++cursor) {
    if (*cursor == '.')
      continue;
    if (k == digits.size())
      return std::unexpected(Error::Overflow);
    digits[k++] = *cursor;
  }
  if (cursor == end || k == 0)
    return std::unexpected(Error::Overflow);
  ++cursor; // past 'e'
  const bool negative_exponent = *cursor == '-';
  ++cursor; // past the sign, to_chars always writes one
  int exponent = 0;
  for (; cursor != end; ++cursor)
    exponent = exponent * 10 + (*cursor - '0');
  if (negative_exponent)
    exponent = -exponent;
  // ECMA-262 names: k digits, the value is 0.digits times 10^n.
  const int n = exponent + 1;
  const auto count = static_cast<int>(k);
  const auto put = [&](char c) { out[used++] = c; };
  if (count <= n && n <= 21) {
    for (std::size_t i = 0; i < k; ++i)
      put(digits[i]);
    for (int i = 0; i < n - count; ++i)
      put('0');
  } else if (0 < n && n <= 21) {
    for (int i = 0; i < count; ++i) {
      if (i == n)
        put('.');
      put(digits[static_cast<std::size_t>(i)]);
    }
  } else if (-6 < n && n <= 0) {
    put('0');
    put('.');
    for (int i = 0; i < -n; ++i)
      put('0');
    for (std::size_t i = 0; i < k; ++i)
      put(digits[i]);
  } else {
    put(digits[0]);
    if (k > 1) {
      put('.');
      for (std::size_t i = 1; i < k; ++i)
        put(digits[i]);
    }
    put('e');
    const int shown = n - 1;
    put(shown < 0 ? '-' : '+');
    std::array<char, 4> power{};
    const auto [power_end, power_error] = std::to_chars(
        power.data(), power.data() + power.size(), shown < 0 ? -shown : shown);
    if (power_error != std::errc{})
      return std::unexpected(Error::Overflow);
    for (const char *c = power.data(); c != power_end; ++c)
      put(*c);
  }
  return used;
}

class Writer {
public:
  // Buffer mode.
  explicit Writer(std::span<char> out) : out_(out) {}
  // Sink mode. `limit` caps the total document bytes.
  Writer(std::span<char> staging, Sink sink, std::size_t limit)
      : out_(staging), sink_(sink), limit_(limit) {}

  // Bare values, for array elements and for the value after key().
  void begin_object() { open_container('{', false); }
  void end_object() { close_container('}', false); }
  void begin_array() { open_container('[', true); }
  void end_array() { close_container(']', true); }
  void key(std::string_view name) {
    if (depth_ == 0 || in_array() || after_key_) {
      fail(Error::Structure);
      return;
    }
    separate();
    string(name);
    put(':');
    after_key_ = true;
  }
  void string_value(std::string_view value) {
    if (!value_slot())
      return;
    string(value);
  }
  void unsigned_value(std::uint64_t value) {
    if (!value_slot())
      return;
    std::array<char, 20> digits{};
    emit_chars(digits, value);
  }
  void signed_value(std::int64_t value) {
    if (!value_slot())
      return;
    std::array<char, 20> digits{};
    emit_chars(digits, value);
  }
  void real_value(double value) {
    if (!value_slot())
      return;
    std::array<char, kRealBytes> text{};
    const auto length = format_real(text, value);
    if (!length) {
      fail(length.error());
      return;
    }
    append({text.data(), *length});
  }
  void null_value() {
    if (!value_slot())
      return;
    append("null");
  }
  void bool_value(bool value) {
    if (!value_slot())
      return;
    append(value ? "true" : "false");
  }
  // A value the caller has already formatted as JSON.
  void raw_value(std::string_view json) {
    if (!value_slot())
      return;
    append(json);
  }

  // Members, a key and its value in one call.
  void begin() { begin_object(); }
  void open(std::string_view name) {
    key(name);
    begin_object();
  }
  void close() { end_object(); }
  void open_array(std::string_view name) {
    key(name);
    begin_array();
  }
  void close_array() { end_array(); }
  void text(std::string_view name, std::string_view value) {
    key(name);
    string_value(value);
  }
  void number(std::string_view name, std::uint64_t value) {
    key(name);
    unsigned_value(value);
  }
  void integer(std::string_view name, std::int64_t value) {
    key(name);
    signed_value(value);
  }
  void real(std::string_view name, double value) {
    key(name);
    real_value(value);
  }
  void null(std::string_view name) {
    key(name);
    null_value();
  }
  void boolean(std::string_view name, bool value) {
    key(name);
    bool_value(value);
  }
  void raw(std::string_view name, std::string_view json) {
    key(name);
    raw_value(json);
  }

  // The document length without the NUL. Buffer mode writes a NUL after it.
  // Sink mode hands the last staged bytes to the sink.
  [[nodiscard]] std::expected<std::size_t, Error> finish() {
    if (ok_ && (depth_ != 0 || after_key_ || !started_))
      fail(Error::Structure);
    if (ok_ && sink_.write) {
      flush();
    } else if (ok_) {
      if (used_ >= out_.size())
        fail(Error::Overflow);
      else
        out_[used_] = '\0';
    }
    if (!ok_)
      return std::unexpected(error_);
    return total_ + used_;
  }

private:
  static constexpr std::size_t kMaxDepth = 32;
  std::span<char> out_;
  Sink sink_{};
  std::size_t limit_ = 0;
  std::size_t used_ = 0;  // staged or buffered bytes
  std::size_t total_ = 0; // bytes already handed to the sink
  std::uint32_t arrays_ = 0;
  std::size_t depth_ = 0;
  Error error_ = Error::Overflow;
  bool ok_ = true;
  bool fresh_ = true;
  bool after_key_ = false;
  bool started_ = false;

  void fail(Error error) {
    if (ok_)
      error_ = error;
    ok_ = false;
  }
  [[nodiscard]] bool in_array() const {
    return depth_ != 0 && ((arrays_ >> (depth_ - 1)) & 1U) != 0;
  }
  // The comma before an element or member, unless it is the first.
  void separate() {
    if (!fresh_)
      put(',');
    fresh_ = false;
  }
  // A value goes after key() inside an object, as an array element, or as
  // the single top level value.
  [[nodiscard]] bool value_slot() {
    if (after_key_) {
      after_key_ = false;
      return ok_;
    }
    if (depth_ == 0 ? started_ : !in_array()) {
      fail(Error::Structure);
      return false;
    }
    started_ = true;
    separate();
    return ok_;
  }
  void open_container(char c, bool array) {
    if (!value_slot())
      return;
    if (depth_ == kMaxDepth) {
      fail(Error::Structure);
      return;
    }
    if (array)
      arrays_ |= 1U << depth_;
    else
      arrays_ &= ~(1U << depth_);
    ++depth_;
    put(c);
    fresh_ = true;
  }
  void close_container(char c, bool array) {
    if (depth_ == 0 || in_array() != array || after_key_) {
      fail(Error::Structure);
      return;
    }
    --depth_;
    put(c);
    fresh_ = false;
  }
  void flush() {
    if (!ok_ || used_ == 0)
      return;
    if (!sink_.write(sink_.context, out_.first(used_))) {
      fail(Error::Sink);
      return;
    }
    total_ += used_;
    used_ = 0;
  }
  void put(char c) {
    if (!ok_)
      return;
    if (sink_.write) {
      if (total_ + used_ >= limit_) {
        fail(Error::Overflow);
        return;
      }
      if (used_ == out_.size())
        flush();
      if (!ok_ || out_.empty()) {
        fail(out_.empty() ? Error::Overflow : error_);
        return;
      }
    } else if (used_ >= out_.size()) {
      fail(Error::Overflow);
      return;
    }
    out_[used_++] = c;
  }
  void append(std::string_view bytes) {
    for (const char c : bytes)
      put(c);
  }
  template <typename Integer>
  void emit_chars(std::array<char, 20> &digits, Integer value) {
    const auto [end, error] =
        std::to_chars(digits.data(), digits.data() + digits.size(), value);
    if (error != std::errc{}) {
      fail(Error::Overflow);
      return;
    }
    append({digits.data(), static_cast<std::size_t>(end - digits.data())});
  }
  // RFC 8259 section 7 escapes, the two character form where one exists.
  // Bytes from 0x80 pass through, so UTF-8 stays UTF-8.
  void string(std::string_view value) {
    static constexpr std::string_view kHex = "0123456789abcdef";
    put('"');
    for (const char c : value) {
      const auto byte = static_cast<unsigned char>(c);
      const char shorthand = c == '"'    ? '"'
                             : c == '\\' ? '\\'
                             : c == '\b' ? 'b'
                             : c == '\f' ? 'f'
                             : c == '\n' ? 'n'
                             : c == '\r' ? 'r'
                             : c == '\t' ? 't'
                                         : '\0';
      if (shorthand != '\0') {
        put('\\');
        put(shorthand);
      } else if (byte < 0x20) {
        append("\\u00");
        put(kHex[byte >> 4]);
        put(kHex[byte & 0x0f]);
      } else {
        put(c);
      }
    }
    put('"');
  }
};

} // namespace opensensor::json
