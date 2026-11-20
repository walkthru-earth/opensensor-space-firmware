#pragma once

// The UI string table, English and Egyptian Arabic. Rows live in
// os_strings.inc. The tables are constexpr arrays of string literals, so they
// sit in flash and every entry is NUL terminated for snprintf.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "os_text.h"

namespace opensensor::text {

enum class Lang : std::uint8_t { En, ArEg };

enum class StringId : std::uint16_t {
#define OS_TEXT(id, en, ar) id,
#define OS_TEXT_LTR(id, en) id,
#include "os_strings.inc"
#undef OS_TEXT
#undef OS_TEXT_LTR
};

inline constexpr std::size_t kStringCount = 0
#define OS_TEXT(id, en, ar) +1
#define OS_TEXT_LTR(id, en) +1
#include "os_strings.inc"
#undef OS_TEXT
#undef OS_TEXT_LTR
    ;

inline constexpr std::array<const char *, kStringCount> kEn{
#define OS_TEXT(id, en, ar) en,
#define OS_TEXT_LTR(id, en) en,
#include "os_strings.inc"
#undef OS_TEXT
#undef OS_TEXT_LTR
};

inline constexpr std::array<const char *, kStringCount> kArEg{
#define OS_TEXT(id, en, ar) ar,
#define OS_TEXT_LTR(id, en) en,
#include "os_strings.inc"
#undef OS_TEXT
#undef OS_TEXT_LTR
};

// True for a string that stays left to right in every language.
inline constexpr std::array<bool, kStringCount> kKeepLtr{
#define OS_TEXT(id, en, ar) false,
#define OS_TEXT_LTR(id, en) true,
#include "os_strings.inc"
#undef OS_TEXT
#undef OS_TEXT_LTR
};

// The string as a NUL terminated literal, safe as a printf format.
[[nodiscard]] constexpr const char *c_str(Lang lang, StringId id) {
  const auto i = static_cast<std::size_t>(std::to_underlying(id));
  return lang == Lang::ArEg ? kArEg[i] : kEn[i];
}

[[nodiscard]] constexpr std::string_view text(Lang lang, StringId id) {
  return c_str(lang, id);
}

// The base direction to lay the string out in, after formatting.
[[nodiscard]] constexpr Direction direction(Lang lang, StringId id) {
  const auto i = static_cast<std::size_t>(std::to_underlying(id));
  return lang == Lang::ArEg && !kKeepLtr[i] ? Direction::Rtl : Direction::Ltr;
}

} // namespace opensensor::text
