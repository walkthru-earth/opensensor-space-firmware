#include "epaper_ui/ui_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace opensensor::ui {

Reading quality_for(Metric m, std::optional<float> value,
                    std::optional<std::uint32_t> since_start_s) {
  if (!since_start_s)
    return {.value = 0.0F, .quality = Quality::Absent, .warm_left_s = 0};
  const std::uint16_t warmup = info(m).warmup_s;
  if (*since_start_s < warmup)
    return {.value = 0.0F,
            .quality = Quality::Warming,
            .warm_left_s = static_cast<std::uint16_t>(warmup - *since_start_s)};
  if (!value || !std::isfinite(*value))
    return {.value = 0.0F, .quality = Quality::Absent, .warm_left_s = 0};
  return {.value = *value, .quality = Quality::Valid, .warm_left_s = 0};
}

std::string_view format_reading(std::span<char> buf, Metric m,
                                const Reading &r) {
  if (buf.empty())
    return {};
  int n = 0;
  if (r.quality != Quality::Valid) {
    const std::string_view none = tr(Str::NoValue);
    n = std::snprintf(buf.data(), buf.size(), "%.*s",
                      static_cast<int>(none.size()), none.data());
  } else {
    const int decimals = info(m).decimals;
    auto v = static_cast<double>(r.value);
    // A value that rounds to zero prints as 0, never as -0.0.
    if (std::fabs(v) < 0.5 * std::pow(10.0, -decimals))
      v = 0.0;
    n = std::snprintf(buf.data(), buf.size(), "%.*f", decimals, v);
  }
  if (n < 0)
    return {};
  return {buf.data(), std::min(static_cast<std::size_t>(n), buf.size() - 1)};
}

} // namespace opensensor::ui
