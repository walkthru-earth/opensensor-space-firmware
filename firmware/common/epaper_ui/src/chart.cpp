#include "epaper_ui/chart.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace opensensor::ui {

Range data_range(const Series &s) {
  Range r;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const float v = s[i];
    if (!std::isfinite(v))
      continue;
    if (!r.valid) {
      r = {.lo = v, .hi = v, .valid = true};
      continue;
    }
    r.lo = std::min(r.lo, v);
    r.hi = std::max(r.hi, v);
  }
  return r;
}

Ticks nice_ticks(float lo, float hi, int max_ticks) {
  // Two ticks cannot always cover a range, -1 to 1 needs three with any
  // step, so three is the floor.
  max_ticks = std::max(max_ticks, 3);
  if (!std::isfinite(lo) || !std::isfinite(hi))
    return {};
  if (hi < lo)
    std::swap(lo, hi);
  if (hi - lo < 1e-6F) {
    const float pad = std::max(std::fabs(lo) * 0.1F, 1.0F);
    lo -= pad;
    hi += pad;
  }
  const double span = static_cast<double>(hi) - lo;
  const double raw = span / (max_ticks - 1);
  double mag = std::pow(10.0, std::floor(std::log10(raw)));
  // Rounding the ends out to the step can add a tick, so the search goes on
  // to 1, 2 and 5 times the next powers of ten until the ticks fit. Any step
  // of at least twice `raw` fits, 20 times `mag` at the latest, so four
  // powers of ten is a safe bound.
  double step = mag;
  bool fits = false;
  for (int decade = 0; decade < 4 && !fits; ++decade, mag *= 10.0) {
    for (const double f : {1.0, 2.0, 5.0}) {
      step = f * mag;
      const double first = std::floor(lo / step) * step;
      const double last = std::ceil(hi / step) * step;
      if (std::lround((last - first) / step) + 1 <= max_ticks) {
        fits = true;
        break;
      }
    }
  }
  const double first = std::floor(lo / step) * step;
  const double last = std::ceil(hi / step) * step;
  int decimals = 0;
  for (double s = step; decimals < 3 && std::fabs(s - std::round(s)) > 1e-6;
       s *= 10.0)
    ++decimals;
  return {.lo = static_cast<float>(first),
          .hi = static_cast<float>(last),
          .step = static_cast<float>(step),
          .count = static_cast<int>(std::lround((last - first) / step)) + 1,
          .decimals = decimals};
}

int y_for(float v, float lo, float hi, Rect plot) {
  const float span = hi - lo;
  const float t = span > 0.0F ? (v - lo) / span : 0.5F;
  const int y =
      plot.bottom() - 1 -
      static_cast<int>(std::lround(t * static_cast<float>(plot.h - 1)));
  return std::clamp(y, plot.y, plot.bottom() - 1);
}

Range widen(Range r, bool zero_floor, float min_span) {
  if (!r.valid)
    return r;
  if (zero_floor)
    r.lo = std::min(r.lo, 0.0F);
  const float span = std::max(min_span, 1e-3F);
  if (r.hi - r.lo >= span)
    return r;
  if (zero_floor && r.lo >= 0.0F) {
    r.hi = r.lo + span;
    return r;
  }
  const float mid = (r.lo + r.hi) / 2.0F;
  r.lo = mid - span / 2.0F;
  r.hi = mid + span / 2.0F;
  return r;
}

namespace {

// The points of column px, [a, b), summarised.
struct Column {
  float mn = 0.0F;
  float mx = 0.0F;
  float first = 0.0F;
  float last = 0.0F;
  float mean = 0.0F;
  bool any = false;
};

Column summarise(const Series &s, std::size_t a, std::size_t b) {
  Column col;
  double sum = 0.0;
  int count = 0;
  for (std::size_t i = a; i < b; ++i) {
    const float v = s[i];
    if (!std::isfinite(v))
      continue;
    if (!col.any) {
      col.mn = col.mx = col.first = v;
      col.any = true;
    }
    col.mn = std::min(col.mn, v);
    col.mx = std::max(col.mx, v);
    col.last = v;
    sum += static_cast<double>(v);
    ++count;
  }
  if (count > 0)
    col.mean = static_cast<float>(sum / count);
  return col;
}

} // namespace

void plot_series(Canvas &c, Rect plot, const Series &s, float lo, float hi,
                 Detail detail) {
  const std::size_t n = s.size();
  if (n == 0 || plot.w <= 1 || plot.h <= 1)
    return;
  const auto w = static_cast<std::size_t>(plot.w);
  if (n <= w) {
    // One x per point. Neighbours that are both present join with a line.
    const auto x_of = [&](std::size_t i) {
      return n == 1 ? plot.x
                    : plot.x + static_cast<int>((i * (w - 1)) / (n - 1));
    };
    for (std::size_t i = 0; i < n; ++i) {
      const float v = s[i];
      if (!std::isfinite(v))
        continue;
      const int x = x_of(i);
      const int y = y_for(v, lo, hi, plot);
      const bool prev = i > 0 && std::isfinite(s[i - 1]);
      const bool next = i + 1 < n && std::isfinite(s[i + 1]);
      if (prev)
        c.line(x_of(i - 1), y_for(s[i - 1], lo, hi, plot), x, y);
      if (!prev && !next)
        c.fill({x - 1, y - 1, 3, 3});
      else
        c.pixel(x, y);
    }
    return;
  }
  // More points than columns. Column px holds points [px*n/w, (px+1)*n/w).
  // A column that starts with a present point joins the previous column when
  // that one ended with a present point, a NaN at either edge breaks the line.
  bool have_prev = false;
  int prev_x = 0;
  int prev_y = 0;
  for (std::size_t px = 0; px < w; ++px) {
    const std::size_t a = (px * n) / w;
    const std::size_t b = ((px + 1) * n) / w;
    const Column col = summarise(s, a, b);
    const int x = plot.x + static_cast<int>(px);
    if (!col.any) {
      have_prev = false;
      continue;
    }
    const bool joins = have_prev && std::isfinite(s[a]);
    const bool next_present = b < n && std::isfinite(s[b]);
    if (detail == Detail::Mean) {
      const int y = y_for(col.mean, lo, hi, plot);
      if (joins)
        c.line(prev_x, prev_y, x, y);
      else if (!next_present)
        c.fill({x - 1, y - 1, 3, 3});
      else
        c.pixel(x, y);
      prev_y = y;
    } else {
      if (joins)
        c.line(prev_x, prev_y, x, y_for(col.first, lo, hi, plot));
      const int y_hi = y_for(col.mx, lo, hi, plot);
      const int y_lo = y_for(col.mn, lo, hi, plot);
      c.vline(x, y_hi, y_lo - y_hi + 1);
      if (!joins && !next_present)
        c.fill({x - 1, y_hi - 1, 3, y_lo - y_hi + 3});
      prev_y = y_for(col.last, lo, hi, plot);
    }
    have_prev = std::isfinite(s[b - 1]);
    prev_x = x;
  }
}

void restart_marks(Canvas &c, Rect plot, const Series &s) {
  const std::size_t n = s.size();
  if (n == 0 || plot.w <= 1 || s.restarts.empty())
    return;
  const auto w = static_cast<std::size_t>(plot.w);
  for (std::size_t i = 0; i < n; ++i) {
    if (!s.restart(i))
      continue;
    // The column plot_series() draws point i in. With more points than
    // columns, column px holds [px*n/w, (px+1)*n/w).
    const std::size_t px =
        n <= w ? (n == 1 ? 0 : (i * (w - 1)) / (n - 1)) : ((i + 1) * w - 1) / n;
    // Longer dashes than the dotted grid, so the two never read alike.
    c.dotted_vline(plot.x + static_cast<int>(px), plot.y, plot.h, 4, 2);
  }
}

void sparkline(Canvas &c, Rect r, const Series &s, bool zero_floor,
               float min_span) {
  const Range range = widen(data_range(s), zero_floor, min_span);
  if (!range.valid)
    return;
  plot_series(c, r, s, range.lo, range.hi, Detail::Mean);
}

void line_chart(Canvas &c, Rect plot, const Series &s,
                const ChartOptions &opt) {
  const ChartStyle &cs = c.layout().chart;
  const Font &f = c.fonts().small;
  c.rect({plot.x - 1, plot.y - 1, plot.w + 2, plot.h + 2});
  // X labels evenly spread under the frame, first left aligned, last right.
  const std::size_t nl = opt.x_labels.size();
  for (std::size_t i = 0; i < nl; ++i) {
    const int x =
        nl == 1
            ? plot.x
            : plot.x + static_cast<int>((i * static_cast<std::size_t>(plot.w)) /
                                        (nl - 1));
    const Align a = i == 0        ? Align::Left
                    : i + 1 == nl ? Align::Right
                                  : Align::Center;
    c.vline(x, plot.bottom(), cs.x_tick);
    c.text(f, x, plot.bottom() + cs.x_label_gap + f.ascent, opt.x_labels[i], a);
  }

  const Range range = widen(data_range(s), opt.zero_floor, opt.min_span);
  if (!range.valid) {
    c.text(c.fonts().label, plot.x + plot.w / 2,
           plot.y + plot.h / 2 + cs.empty_dy, opt.empty_text, Align::Center);
    return;
  }
  const int max_ticks = std::clamp(plot.h / std::max(cs.tick_spacing, 1), 3, 6);
  const Ticks t = nice_ticks(range.lo, range.hi, max_ticks);
  std::array<char, 16> buf{};
  for (int i = 0; i < t.count; ++i) {
    const float v = t.lo + t.step * static_cast<float>(i);
    const int y = y_for(v, t.lo, t.hi, plot);
    if (i != 0 && i + 1 != t.count)
      c.dotted_hline(plot.x, y, plot.w, 1, 3);
    c.hline(plot.x - cs.y_tick_dx, y, cs.y_tick_len);
    const int n = std::snprintf(buf.data(), buf.size(), "%.*f", t.decimals,
                                static_cast<double>(v));
    const std::string_view label =
        n < 0
            ? std::string_view{}
            : std::string_view{buf.data(), std::min(static_cast<std::size_t>(n),
                                                    buf.size() - 1)};
    c.text(f, plot.x - cs.y_label_dx, y + cs.y_label_dy, label, Align::Right);
  }
  // `trace` pixels thick so the line reads across a room, flat runs and
  // steep edges alike. Each extra pass shifts down and right inside the
  // frame.
  const int extra = std::max(cs.trace, 1) - 1;
  const Rect inner{plot.x, plot.y, plot.w - extra, plot.h - extra};
  restart_marks(c, inner, s);
  plot_series(c, inner, s, t.lo, t.hi);
  for (int k = 1; k <= extra; ++k) {
    plot_series(c, {inner.x, inner.y + k, inner.w, inner.h}, s, t.lo, t.hi);
    plot_series(c, {inner.x + k, inner.y, inner.w, inner.h}, s, t.lo, t.hi);
  }
}

} // namespace opensensor::ui
