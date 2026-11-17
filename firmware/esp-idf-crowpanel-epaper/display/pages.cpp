#include "pages.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>

#include "air_band.h"
#include "chart.h"

namespace opensensor::ui {
namespace {

using Text = std::array<char, 48>;

constexpr int kHeaderH = 22;
constexpr int kMargin = 8;

std::string_view view(const Text &t, int n) {
  if (n < 0)
    return {};
  return {t.data(), std::min(static_cast<std::size_t>(n), t.size() - 1)};
}

// Draws `s` and cuts it with ".." when it would pass `max_w` pixels.
void text_fit(Canvas &c, const Font &f, int x, int y, std::string_view s,
              int max_w) {
  if (text_width(f, s) <= max_w) {
    c.text(f, x, y, s);
    return;
  }
  const int dots = text_width(f, "..");
  std::size_t pos = 0;
  std::size_t keep = 0;
  int w = 0;
  while (pos < s.size()) {
    const Glyph *g = find_glyph(f, next_codepoint(s, pos));
    const int adv = g != nullptr ? g->advance : 0;
    if (w + adv + dots > max_w)
      break;
    w += adv;
    keep = pos;
  }
  const int drawn = c.text(f, x, y, s.substr(0, keep));
  c.text(f, x + drawn, y, "..");
}

// A small label box, filled when `on`.
int tag(Canvas &c, int right, std::string_view s, bool on) {
  const Font &f = fonts::small;
  const int w = text_width(f, s) + 8;
  const Rect r{right - w, 3, w, 16};
  if (on) {
    c.fill_round_rect(r);
    c.text(f, r.x + 4, 15, s, Ink::White);
  } else {
    c.round_rect(r);
    c.text(f, r.x + 4, 15, s);
  }
  return r.x - 4;
}

void header(Canvas &c, const Status &st) {
  c.text(fonts::label, kMargin, 16, "OpenSensor");
  Text t{};
  if (st.utc_s) {
    const std::int64_t day_s = ((*st.utc_s % 86400) + 86400) % 86400;
    const int n = std::snprintf(t.data(), t.size(), "%02d:%02d UTC",
                                static_cast<int>(day_s / 3600),
                                static_cast<int>((day_s / 60) % 60));
    c.text(fonts::label, 200, 16, view(t, n), Align::Center);
  } else {
    c.text(fonts::label, 200, 16, "--:-- UTC", Align::Center);
  }
  int right = kWidth - kMargin;
  if (st.wifi != Link::Off)
    right = tag(c, right, "WiFi", st.wifi == Link::Connected);
  if (st.ble != Link::Off)
    right = tag(c, right, "BLE", st.ble == Link::Connected);
  static_cast<void>(tag(c, right, st.sd_ok ? "SD" : "no SD", st.sd_ok));
  c.hline(0, kHeaderH, kWidth);
}

// The caption under a value that is not Valid.
std::string_view not_valid_caption(const Reading &r, const Status &st,
                                   Text &t) {
  if (r.quality == Quality::Warming) {
    const int n = std::snprintf(t.data(), t.size(), "warming %u s",
                                static_cast<unsigned>(r.warm_left_s));
    return view(t, n);
  }
  return st.sensor_present ? "no reading" : "no sensor";
}

void hero(Canvas &c, const Model &m, const History &h) {
  const Reading &r = m.now[index(Metric::Pm25)];
  const MetricInfo &mi = info(Metric::Pm25);
  const int lw = c.text(fonts::label, kMargin + 2, 42, mi.label);
  c.text(fonts::small, kMargin + 2 + lw + 6, 42, mi.unit);

  Text t{};
  const std::string_view v = format_reading(t, Metric::Pm25, r);
  c.text(fonts::hero, kMargin, 106, v);

  // The band pill, or what is missing. `word` may view `cap`, so it lives
  // at this scope.
  // cppcheck-suppress variableScope
  Text cap{};
  std::string_view word;
  bool filled = false;
  if (r.quality == Quality::Valid) {
    word = pm25_band(r.value).word;
    filled = true;
  } else {
    word = not_valid_caption(r, m.status, cap);
  }
  const Font &pf = fonts::label;
  const Rect pill{kMargin, 114, text_width(pf, word) + 16, 21};
  if (filled) {
    c.fill_round_rect(pill);
    c.text(pf, pill.x + 8, pill.y + 15, word, Ink::White);
  } else {
    c.round_rect(pill);
    c.text(pf, pill.x + 8, pill.y + 15, word);
  }

  // The last 2 h on the right.
  const Rect plot{218, 50, 172, 66};
  const Series s = h.series(Metric::Pm25, Window::Recent);
  c.text(fonts::small, plot.x, 42, "Last 2 hours");
  const Range range = data_range(s);
  if (range.valid) {
    Text a{};
    Text b{};
    const int na = std::snprintf(a.data(), a.size(), "max %.1f",
                                 static_cast<double>(range.hi));
    const int nb = std::snprintf(b.data(), b.size(), "min %.1f",
                                 static_cast<double>(range.lo));
    c.text(fonts::small, plot.right(), 42, view(a, na), Align::Right);
    c.text(fonts::small, plot.right(), plot.bottom() + 16, view(b, nb),
           Align::Right);
  }
  c.dotted_hline(plot.x, plot.y - 2, plot.w, 1, 3);
  c.hline(plot.x, plot.bottom() + 2, plot.w);
  c.text(fonts::small, plot.x, plot.bottom() + 16, "-2 h");
  // No zero floor here, the labels name the data min and max, so the trace
  // spans exactly that band unless it is narrower than the metric's min_span.
  if (range.valid)
    sparkline(c, plot, s, false, mi.min_span);
  else
    c.text(fonts::small, plot.x + plot.w / 2, plot.y + plot.h / 2 + 4,
           "collecting", Align::Center);
}

void tile(Canvas &c, Rect box, Metric metric, const Model &m,
          const History &h) {
  const MetricInfo &mi = info(metric);
  const Reading &r = m.now[index(metric)];
  const int x = box.x + kMargin;
  const int right = box.right() - kMargin;
  c.text(fonts::label, x, box.y + 18, mi.label);
  c.text(fonts::small, right, box.y + 18, mi.unit, Align::Right);
  Text t{};
  c.text(fonts::value, x, box.y + 50, format_reading(t, metric, r));
  if (r.quality != Quality::Valid) {
    Text cap{};
    c.text(fonts::small, right, box.y + 50, not_valid_caption(r, m.status, cap),
           Align::Right);
  }
  const Rect spark{x, box.y + 57, box.w - 2 * kMargin, 16};
  sparkline(c, spark, h.series(metric, Window::Recent), mi.zero_floor,
            mi.min_span);
}

void dashboard(Canvas &c, const Model &m, const History &h) {
  hero(c, m, h);
  constexpr int kTop = 146;
  constexpr int kTileH = 77;
  constexpr int kTileW = 133;
  c.hline(0, kTop - 1, kWidth);
  c.hline(0, kTop + kTileH, kWidth);
  c.vline(kTileW, kTop, 2 * kTileH + 1);
  c.vline(2 * kTileW + 1, kTop, 2 * kTileH + 1);
  constexpr std::array<Metric, 6> kTiles{Metric::Co2, Metric::Temp,
                                         Metric::Rh,  Metric::Pm1,
                                         Metric::Pm4, Metric::Pm10};
  for (std::size_t i = 0; i < kTiles.size(); ++i) {
    const int col = static_cast<int>(i % 3);
    const int row = static_cast<int>(i / 3);
    const Rect box{col * (kTileW + 1), kTop + row * (kTileH + 1), kTileW,
                   kTileH};
    tile(c, box, kTiles[i], m, h);
  }
}

void page_number(Canvas &c, std::uint8_t page) {
  Text t{};
  const int n = std::snprintf(t.data(), t.size(), "%u/%u",
                              static_cast<unsigned>(page) + 1,
                              static_cast<unsigned>(kPageCount));
  c.text(fonts::small, kWidth - kMargin, 44, view(t, n), Align::Right);
}

void chart_page(Canvas &c, std::uint8_t page, const Model &m,
                const History &h) {
  const Metric metric = kChartOrder[page - 1U];
  const MetricInfo &mi = info(metric);
  const int tw = c.text(fonts::title, kMargin + 2, 46, mi.label);
  Text sub{};
  const int ns =
      std::snprintf(sub.data(), sub.size(), "%.*s, last 24 h",
                    static_cast<int>(mi.unit.size()), mi.unit.data());
  c.text(fonts::small, kMargin + 2 + tw + 8, 46, view(sub, ns));
  page_number(c, page);

  const Series s = h.series(metric, Window::Day);
  const Range range = data_range(s);
  const Reading &now = m.now[index(metric)];
  struct Stat {
    std::string_view name;
    Reading r;
  };
  const std::array<Stat, 3> stats{{
      {.name = "now", .r = now},
      {.name = "min",
       .r = {.value = range.lo,
             .quality = range.valid ? Quality::Valid : Quality::Absent,
             .warm_left_s = 0}},
      {.name = "max",
       .r = {.value = range.hi,
             .quality = range.valid ? Quality::Valid : Quality::Absent,
             .warm_left_s = 0}},
  }};
  int x = kMargin + 2;
  for (const auto &st : stats) {
    const int nw = c.text(fonts::small, x, 68, st.name);
    Text t{};
    c.text(fonts::label, x + nw + 5, 68, format_reading(t, metric, st.r));
    x += 120;
  }

  constexpr std::array<std::string_view, 5> kLabels{"-24 h", "-18 h", "-12 h",
                                                    "-6 h", "now"};
  line_chart(c, {46, 82, 344, 186}, s,
             {.zero_floor = mi.zero_floor,
              .min_span = mi.min_span,
              .x_labels = kLabels,
              .empty_text = "No data yet"});
}

void row(Canvas &c, int y, std::string_view name, std::string_view value) {
  c.text(fonts::small, kMargin + 2, y, name);
  text_fit(c, fonts::label, 116, y, value, kWidth - 116 - kMargin);
}

std::string_view link_word(Link l, std::string_view waiting) {
  switch (l) {
  case Link::Off:
    return "off";
  case Link::Waiting:
    return waiting;
  case Link::Connected:
    return "connected";
  }
  return "off";
}

// SEN6x device status bits that the SEN63C sets, Sensirion SEN6x datasheet
// "Read Device Status" (0xD206).
struct StatusBit {
  int bit = 0;
  std::string_view name;
};
constexpr std::array<StatusBit, 5> kStatusBits{{
    {.bit = 21, .name = "fan speed warn"},
    {.bit = 12, .name = "CO₂ error"},
    {.bit = 11, .name = "PM error"},
    {.bit = 6, .name = "RH/T error"},
    {.bit = 4, .name = "fan error"},
}};

std::string_view sensor_status_text(std::uint32_t bits, Text &t) {
  if (bits == 0)
    return "ok";
  int n = 0;
  std::uint32_t known = 0;
  for (const auto &sb : kStatusBits) {
    const std::uint32_t mask = 1U << sb.bit;
    known |= mask;
    if ((bits & mask) == 0)
      continue;
    const int w = std::snprintf(
        t.data() + n, t.size() - static_cast<std::size_t>(n), "%s%.*s",
        n == 0 ? "" : ", ", static_cast<int>(sb.name.size()), sb.name.data());
    if (w < 0 || n + w >= static_cast<int>(t.size()))
      return view(t, static_cast<int>(t.size()) - 1);
    n += w;
  }
  if ((bits & ~known) != 0) {
    const int w = std::snprintf(
        t.data() + n, t.size() - static_cast<std::size_t>(n), "%s0x%08X",
        n == 0 ? "" : ", ", static_cast<unsigned>(bits & ~known));
    if (w > 0)
      n = std::min(n + w, static_cast<int>(t.size()) - 1);
  }
  return view(t, n);
}

// Howard Hinnant's days to civil date, valid for every int64 day we print.
void civil(std::int64_t days, int &y, unsigned &mo, unsigned &d) {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  mo = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int>(static_cast<std::int64_t>(yoe) + era * 400 +
                       (mo <= 2 ? 1 : 0));
}

void status_page(Canvas &c, std::uint8_t page, const Model &m) {
  const Status &st = m.status;
  c.text(fonts::title, kMargin + 2, 46, "Status");
  page_number(c, page);
  int y = 72;
  constexpr int kRow = 21;
  Text t{};

  if (st.utc_s) {
    const std::int64_t s = *st.utc_s;
    const std::int64_t days = (s >= 0 ? s : s - 86399) / 86400;
    const std::int64_t sod = s - days * 86400;
    int yr = 0;
    unsigned mo = 0;
    unsigned d = 0;
    civil(days, yr, mo, d);
    const int n = std::snprintf(
        t.data(), t.size(), "%04d-%02u-%02u %02d:%02d UTC", yr, mo, d,
        static_cast<int>(sod / 3600), static_cast<int>((sod / 60) % 60));
    row(c, y, "Clock", view(t, n));
  } else {
    row(c, y, "Clock", "not set, waiting for time");
  }
  y += kRow;

  {
    // unsigned, not uint32_t, which is long on Xtensa and int on the host.
    const auto u = static_cast<unsigned>(st.uptime_s);
    const int n = u >= 86400
                      ? std::snprintf(t.data(), t.size(), "%u d %02u:%02u",
                                      u / 86400, (u / 3600) % 24, (u / 60) % 60)
                      : std::snprintf(t.data(), t.size(), "%02u:%02u:%02u",
                                      u / 3600, (u / 60) % 60, u % 60);
    row(c, y, "Uptime", view(t, n));
    y += kRow;
  }
  {
    const int n = std::snprintf(t.data(), t.size(), "%u",
                                static_cast<unsigned>(st.samples));
    row(c, y, "Samples", view(t, n));
    y += kRow;
  }
  if (st.sd_ok) {
    const double gb = static_cast<double>(st.sd_free_bytes) / 1e9;
    const int n = std::snprintf(t.data(), t.size(), "ok, %.2f GB free", gb);
    row(c, y, "SD card", view(t, n));
  } else {
    row(c, y, "SD card", "missing or failed");
  }
  y += kRow;
  row(c, y, "Bluetooth", link_word(st.ble, "advertising"));
  y += kRow;
  row(c, y, "Wi-Fi", link_word(st.wifi, "connecting"));
  y += kRow;
  row(c, y, "Thing", st.thing.empty() ? std::string_view{"unset"} : st.thing);
  y += kRow;
  row(c, y, "Firmware", st.firmware);
  y += kRow;
  if (st.sensor_present) {
    const int n = std::snprintf(
        t.data(), t.size(), "SEN63C %.*s, fw %.*s",
        static_cast<int>(st.sensor_serial.size()), st.sensor_serial.data(),
        static_cast<int>(st.sensor_fw.size()), st.sensor_fw.data());
    row(c, y, "Sensor", view(t, n));
    y += kRow;
    Text bits{};
    row(c, y, "Sensor state",
        st.sensor_status ? sensor_status_text(*st.sensor_status, bits)
                         : std::string_view{"not read"});
  } else {
    row(c, y, "Sensor", "SEN63C not found on I2C");
  }

  c.hline(0, 274, kWidth);
  c.text(fonts::small, kMargin + 2, 291,
         "PM2.5 word uses the US EPA 24 h AQI bands as a guide");
}

} // namespace

Nav navigate(std::uint8_t page, Input in) {
  if (page >= kPageCount)
    page = kDashboardPage;
  switch (in) {
  case Input::Up:
    return {.page =
                static_cast<std::uint8_t>((page + kPageCount - 1) % kPageCount),
            .full_refresh = false};
  case Input::Down:
    return {.page = static_cast<std::uint8_t>((page + 1) % kPageCount),
            .full_refresh = false};
  case Input::Press:
    return {.page = page, .full_refresh = true};
  case Input::Menu:
    return {.page = page == kStatusPage ? kDashboardPage : kStatusPage,
            .full_refresh = false};
  case Input::Exit:
    return {.page = kDashboardPage, .full_refresh = false};
  }
  return {.page = page, .full_refresh = false};
}

Refresh RefreshPolicy::next(bool page_changed, bool forced) {
  if (!shown_ || page_changed || forced || partials_ >= kPartialsPerFull) {
    shown_ = true;
    partials_ = 0;
    return Refresh::Full;
  }
  ++partials_;
  return Refresh::Partial;
}

void render(Canvas &c, std::uint8_t page, const Model &model,
            const History &history) {
  c.clear();
  if (page >= kPageCount)
    page = kDashboardPage;
  header(c, model.status);
  if (page == kDashboardPage)
    dashboard(c, model, history);
  else if (page == kStatusPage)
    status_page(c, page, model);
  else
    chart_page(c, page, model, history);
}

} // namespace opensensor::ui
