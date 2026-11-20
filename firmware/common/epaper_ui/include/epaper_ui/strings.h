#pragma once

// Every word the pages draw, by id, in English and Egyptian Arabic. The rows
// live in firmware/common/text/src/os_strings.inc, the one string table, and
// Str is its StringId. tr() and tr_fmt() answer in the active language, which
// render_screen() sets from Settings::language before it draws.
//
// A format id holds printf conversions. Its argument list is fixed by the id,
// and both languages keep the same conversions in the same order. Pass it to
// snprintf through tr_fmt(), never through tr().data().

#include <cstdint>
#include <string_view>

#include "os_strings.h"

namespace opensensor::ui {

using Str = text::StringId;

// The language the pages draw in. Arabic lays its lines out right to left
// and mirrors the page where a row reads from one side.
enum class Language : std::uint8_t { English, Arabic };

// The display task's language for every later tr() and tr_fmt(). Only the
// task that draws calls it, so it needs no lock.
void set_language(Language l);
[[nodiscard]] Language language();
// True when the active language reads right to left.
[[nodiscard]] bool rtl();

// The UTF-8 text of `id`, empty for an id out of range.
[[nodiscard]] std::string_view tr(Str id);

// The same text as a NUL terminated C string, for a format id passed to
// snprintf. Never null.
[[nodiscard]] const char *tr_fmt(Str id);

} // namespace opensensor::ui
