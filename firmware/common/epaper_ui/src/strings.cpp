#include "epaper_ui/strings.h"

#include <cstddef>
#include <utility>

namespace opensensor::ui {
namespace {

// The display task alone writes and reads it.
constinit Language g_language = Language::English;

[[nodiscard]] text::Lang lang() {
  return g_language == Language::Arabic ? text::Lang::ArEg : text::Lang::En;
}

} // namespace

void set_language(Language l) {
  g_language = l == Language::Arabic ? Language::Arabic : Language::English;
}

Language language() { return g_language; }

bool rtl() { return g_language == Language::Arabic; }

const char *tr_fmt(Str id) {
  if (static_cast<std::size_t>(std::to_underlying(id)) >= text::kStringCount)
    return "";
  return text::c_str(lang(), id);
}

std::string_view tr(Str id) { return tr_fmt(id); }

} // namespace opensensor::ui
