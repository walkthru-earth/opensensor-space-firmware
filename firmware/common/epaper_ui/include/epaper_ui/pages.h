#pragma once

// The air quality pages. Page 0 is the dashboard, pages 1 to 7 are the 24 h
// chart of one metric each and page 8 is the status page. Rendering is pure,
// it reads a Model and a History and writes a frame, so the host preview
// draws exactly what the panel shows. Every position and font comes from the
// canvas layout and every word from strings.h, so one renderer serves each
// size class.

#include <array>
#include <cstdint>
#include <string_view>

#include "epaper_ui/canvas.h"
#include "epaper_ui/history.h"
#include "epaper_ui/ui_model.h"

namespace opensensor::ui {

inline constexpr std::uint8_t kPageCount = 9;
inline constexpr std::uint8_t kDashboardPage = 0;
inline constexpr std::uint8_t kStatusPage = kPageCount - 1;

// The metric of chart page 1 + i.
inline constexpr std::array<Metric, kMetricCount> kChartOrder{
    Metric::Pm25, Metric::Co2, Metric::Temp, Metric::Rh,
    Metric::Pm1,  Metric::Pm4, Metric::Pm10,
};

// Buttons on the panel. Up and Down are the rotary switch, Press is pushing
// it, Menu and Exit are the two keys on the side. The board passes each one
// to handle() in settings.h, which sends it to the settings popup or here.
enum class Input : std::uint8_t { Up, Down, Press, Menu, Exit };

struct Nav {
  std::uint8_t page;
  bool full_refresh; // The user asked for a clean redraw.
};

// Page navigation with the settings popup closed. Up and Down step through
// the pages and wrap, so the status page is one turn back from the
// dashboard. Press keeps the page and asks for a full refresh to clear
// ghosting. Exit returns to the dashboard. Menu opens the settings popup in
// handle(), so here it keeps the page.
[[nodiscard]] Nav navigate(std::uint8_t page, Input in);

// The filled width of a battery icon whose inside is `inner_w` pixels wide,
// at `percent` rounded to the nearest pixel. A percent past 100 fills it.
[[nodiscard]] constexpr int battery_fill(int inner_w, std::uint8_t percent) {
  if (inner_w <= 0)
    return 0;
  const int p = percent > 100 ? 100 : percent;
  return (inner_w * p + 50) / 100;
}

// Where a metric's value draws. The box holds the widest value of the
// metric in tabular figures, value_slots() cells of digit_cell() with room
// for a minus sign and the decimal point where the metric has them, so each
// digit keeps its cell from refresh to refresh and a partial refresh flips
// only the digits that changed.
struct ValueBox {
  int x = 0; // Left edge.
  int w = 0;
  // Left edge of the decimal point, or the right edge of the last digit for
  // a metric with no decimals. The whole digits fill leftwards from it.
  int anchor = 0;
};

// The box of metric `m` in `font` that starts at `edge` in reading order,
// its left edge, or its right edge when `mirrored`.
[[nodiscard]] ValueBox value_box(const Font &font, Metric m, int edge,
                                 bool mirrored);

// Draws `v` with its baseline at `y`. A number, digits with an optional
// '-' and '.', sits in `box` on its anchor in tabular figures, its digits
// left to right whether or not the box is `mirrored`. Anything else, such
// as Str::NoValue, draws as plain text from the box start, the left edge or
// the right edge when `mirrored`, as a value did before the box. A number
// wider than the box, which only a value outside MetricInfo's range can be,
// keeps its tabular figures and starts at the box start.
void draw_value(Canvas &c, const Font &font, const ValueBox &box, int y,
                std::string_view v, bool mirrored);

// Draws `page` of the model into the canvas, clearing it first.
void render(Canvas &c, std::uint8_t page, const Model &model,
            const History &history);

// The sleeping screen, clearing the canvas first. It names the Thing and the
// device and says how to wake it. It shows no reading and no clock, so a
// sleeping panel never passes an old value off as current (#165).
void render_asleep(Canvas &c, const Model &model);

// The settings storage fault page, clearing the canvas first (#177). A board
// draws it when NVS cannot start, so it holds no reading. It says logging is
// off in English and in Arabic, because the language setting lives in NVS,
// and names the ESP-IDF error. It leaves the active language as it found it.
void render_storage_fault(Canvas &c, int error);

// The same page for a Thing id that cannot load (#225), drawn by a board
// whose logger reported the fault. The code line names the stage ("open",
// "read", "write" or "format") and the ESP-IDF error, 0 for a stored value
// that failed the format check.
void render_thing_id_fault(Canvas &c, const char *stage, int error);

} // namespace opensensor::ui
