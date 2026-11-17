#pragma once

// The CrowPanel pages. Page 0 is the dashboard, pages 1 to 7 are the 24 h
// chart of one metric each and page 8 is the status page. Rendering is pure,
// it reads a Model and a History and writes a frame, so the host preview
// draws exactly what the panel shows.

#include <array>
#include <cstdint>

#include "canvas.h"
#include "history.h"
#include "ui_model.h"

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

enum class Refresh : std::uint8_t { Full, Partial };

// Partial refresh is quick and flicker free but leaves ghosts, so every
// page change, every forced redraw and every kPartialsPerFull-th update is a
// full refresh. The first update after boot is always full.
class RefreshPolicy {
public:
  static constexpr std::uint16_t kPartialsPerFull = 30;
  [[nodiscard]] Refresh next(bool page_changed, bool forced);

private:
  std::uint16_t partials_ = 0;
  bool shown_ = false;
};

// Draws `page` of the model into the canvas, clearing it first.
void render(Canvas &c, std::uint8_t page, const Model &model,
            const History &history);

// The sleeping screen, clearing the canvas first. It names the Thing and the
// device and says how to wake it. It shows no reading and no clock, so a
// sleeping panel never passes an old value off as current (#165).
void render_asleep(Canvas &c, const Model &model);

} // namespace opensensor::ui
