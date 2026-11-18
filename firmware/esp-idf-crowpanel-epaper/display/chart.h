#pragma once

// Line charts for a one bit panel. NaN points break the line, a lone point
// draws as a dot, and when there are more points than pixel columns each
// column draws the min to max envelope of its points so no spike is lost.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "canvas.h"
#include "series.h"

namespace opensensor::ui {

struct Range {
  float lo = 0.0F;
  float hi = 0.0F;
  bool valid = false; // False when every point is NaN.
};

// Min and max of the finite points.
[[nodiscard]] Range data_range(const Series &s);

struct Ticks {
  float lo = 0.0F; // First tick, at or below the data.
  float hi = 1.0F; // Last tick, at or above the data.
  float step = 1.0F;
  int count = 2;    // Ticks from lo to hi inclusive.
  int decimals = 0; // Enough to tell the ticks apart.
};

// "Nice" axis ticks (1, 2 or 5 times a power of ten) that cover [lo, hi]
// with at most `max_ticks` ticks, and never fewer than three allowed. A flat
// range is widened so the line sits in the middle.
[[nodiscard]] Ticks nice_ticks(float lo, float hi, int max_ticks);

// The y pixel of `v` inside `plot` for the scale lo..hi, clamped to it.
[[nodiscard]] int y_for(float v, float lo, float hi, Rect plot);

// Widens a data range to at least `min_span`, around its middle, or up from
// 0 with `zero_floor`.
[[nodiscard]] Range widen(Range r, bool zero_floor, float min_span);

// How a pixel column shows several points. Envelope draws their min to max
// so no spike is lost. Mean draws one point per column, calmer for a small
// sparkline.
enum class Detail : std::uint8_t { Envelope, Mean };

// Draws the series into `plot` scaled to lo..hi, no axes.
void plot_series(Canvas &c, Rect plot, const Series &s, float lo, float hi,
                 Detail detail = Detail::Envelope);

// A dashed vertical line across `plot` at each restart point of the series,
// at the x plot_series() gives that point. A reboot of unknown length reads
// apart from a plain gap, which stays blank.
void restart_marks(Canvas &c, Rect plot, const Series &s);

// A sparkline scaled to its own data, column means. Draws nothing when all
// points are NaN.
void sparkline(Canvas &c, Rect r, const Series &s, bool zero_floor,
               float min_span);

struct ChartOptions {
  bool zero_floor = false;
  float min_span = 0.0F;
  std::span<const std::string_view> x_labels; // Spread evenly, oldest first.
  std::string_view empty_text = "No data yet";
};

// A full chart, y tick labels left of `plot`, dotted grid lines, dashed
// restart lines, a frame and the x labels below it.
void line_chart(Canvas &c, Rect plot, const Series &s, const ChartOptions &opt);

} // namespace opensensor::ui
