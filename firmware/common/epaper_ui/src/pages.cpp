#include "epaper_ui/pages.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>

#include "epaper_ui/air_band.h"
#include "epaper_ui/chart.h"
#include "epaper_ui/strings.h"

namespace opensensor::ui {
namespace {

using Text = std::array<char, 48>;

std::string_view view(const Text &t, int n) {
  if (n < 0)
    return {};
  return {t.data(), std::min(static_cast<std::size_t>(n), t.size() - 1)};
}

// Draws `s` and cuts it with an ellipsis when it would pass `max_w` pixels.
// The cut walks shape(), so it lands on a glyph boundary in any script.
void text_fit(Canvas &c, const Font &f, int x, int y, std::string_view s,
              int max_w) {
  if (text_width(f, s) <= max_w) {
    c.text(f, x, y, s);
    return;
  }
  const std::string_view dots = tr(Str::Ellipsis);
  const int dots_w = text_width(f, dots);
  std::size_t keep = 0;
  static_cast<void>(shape(f, s, [&](const ShapedGlyph &sg) {
    const int adv = sg.glyph != nullptr ? sg.glyph->advance : 0;
    if (sg.x + adv + dots_w > max_w)
      return false;
    keep = sg.end;
    return true;
  }));
  const int drawn = c.text(f, x, y, s.substr(0, keep));
  c.text(f, x + drawn, y, dots);
}

// A small label box at the header's right, filled when `on`. Returns the
// right edge for the next tag.
int tag(Canvas &c, int right, Str word, bool on) {
  const HeaderLayout &hl = c.layout().header;
  const Font &f = c.fonts().small;
  const std::string_view s = tr(word);
  const int w = text_width(f, s) + 2 * hl.tag_pad;
  const Rect r{right - w, hl.tag_y, w, hl.tag_h};
  if (on) {
    c.fill_round_rect(r);
    c.text(f, r.x + hl.tag_pad, hl.tag_baseline, s, Ink::White);
  } else {
    c.round_rect(r);
    c.text(f, r.x + hl.tag_pad, hl.tag_baseline, s);
  }
  return r.x - hl.tag_gap;
}

void header(Canvas &c, const Status &st) {
  const HeaderLayout &hl = c.layout().header;
  const FontSet &fs = c.fonts();
  const int w = c.width();
  const int brand_end =
      hl.margin + c.text(fs.label, hl.margin, hl.baseline, tr(Str::Brand));
  int right = w - hl.margin;
  if (st.wifi != Link::Off)
    right = tag(c, right, Str::TagWifi, st.wifi == Link::Connected);
  if (st.ble != Link::Off)
    right = tag(c, right, Str::TagBle, st.ble == Link::Connected);
  right = tag(c, right, st.sd_ok ? Str::TagSd : Str::TagNoSd, st.sd_ok);
  Text t{};
  std::string_view clock = tr(Str::ClockUnset);
  if (st.utc_s) {
    const std::int64_t day_s = ((*st.utc_s % 86400) + 86400) % 86400;
    const int n = std::snprintf(t.data(), t.size(), tr_fmt(Str::ClockFmt),
                                static_cast<int>(day_s / 3600),
                                static_cast<int>((day_s / 60) % 60));
    clock = view(t, n);
  }
  // Centred on clock_x, moved into the gap between the brand and the tags
  // when it would touch either, as on a narrow portrait screen. Where even
  // the gap is too narrow it drops to the small font, centred in the gap.
  for (const Font *f : {&fs.label, &fs.small}) {
    const int cw = text_width(*f, clock);
    const int lo = brand_end + 2 * hl.tag_gap + cw / 2;
    const int hi = right - hl.tag_gap - (cw - cw / 2);
    if (lo <= hi || f == &fs.small) {
      const int cx =
          lo <= hi ? std::clamp(hl.clock_x, lo, hi) : (brand_end + right) / 2;
      c.text(*f, cx, hl.baseline, clock, Align::Center);
      break;
    }
  }
  c.hline(0, hl.height, w);
}

// The caption under a value that is not Valid.
std::string_view not_valid_caption(const Reading &r, const Status &st,
                                   Text &t) {
  if (r.quality == Quality::Warming) {
    const int n = std::snprintf(t.data(), t.size(), tr_fmt(Str::WarmingFmt),
                                static_cast<unsigned>(r.warm_left_s));
    return view(t, n);
  }
  return tr(st.sensor_present ? Str::NoReading : Str::NoSensor);
}

void hero(Canvas &c, const Model &m, const History &h) {
  const HeroLayout &hl = c.layout().hero;
  const FontSet &fs = c.fonts();
  const Reading &r = m.now[index(Metric::Pm25)];
  const MetricInfo &mi = info(Metric::Pm25);
  const int lw = c.text(fs.label, hl.label.x, hl.label.y, tr(mi.label));
  c.text(fs.small, hl.label.x + lw + hl.unit_gap, hl.label.y, tr(mi.unit));

  Text t{};
  const std::string_view v = format_reading(t, Metric::Pm25, r);
  c.text(fs.hero, hl.value.x, hl.value.y, v);

  // The band pill, or what is missing. `word` may view `cap`, so it lives
  // at this scope.
  // cppcheck-suppress variableScope
  Text cap{};
  std::string_view word;
  bool filled = false;
  if (r.quality == Quality::Valid) {
    word = tr(pm25_band(r.value).word);
    filled = true;
  } else {
    word = not_valid_caption(r, m.status, cap);
  }
  const Font &pf = fs.label;
  const Rect pill{hl.pill.x, hl.pill.y, text_width(pf, word) + 2 * hl.pill_pad,
                  hl.pill_h};
  if (filled) {
    c.fill_round_rect(pill);
    c.text(pf, pill.x + hl.pill_pad, pill.y + hl.pill_baseline, word,
           Ink::White);
  } else {
    c.round_rect(pill);
    c.text(pf, pill.x + hl.pill_pad, pill.y + hl.pill_baseline, word);
  }

  // The last 2 h on the right.
  const Rect plot = hl.plot;
  const Series s = h.series(Metric::Pm25, Window::Recent);
  c.text(fs.small, plot.x, hl.plot_caption_y, tr(Str::Last2Hours));
  const Range range = data_range(s);
  const int foot_y = plot.bottom() + hl.plot_foot_dy;
  if (range.valid) {
    Text a{};
    Text b{};
    const int na = std::snprintf(a.data(), a.size(), tr_fmt(Str::MaxFmt),
                                 static_cast<double>(range.hi));
    const int nb = std::snprintf(b.data(), b.size(), tr_fmt(Str::MinFmt),
                                 static_cast<double>(range.lo));
    c.text(fs.small, plot.right(), hl.plot_caption_y, view(a, na),
           Align::Right);
    c.text(fs.small, plot.right(), foot_y, view(b, nb), Align::Right);
  }
  c.dotted_hline(plot.x, plot.y - hl.plot_rule_gap, plot.w, 1, 3);
  c.hline(plot.x, plot.bottom() + hl.plot_rule_gap, plot.w);
  c.text(fs.small, plot.x, foot_y, tr(Str::Minus2h));
  // No zero floor here, the labels name the data min and max, so the trace
  // spans exactly that band unless it is narrower than the metric's min_span.
  if (range.valid) {
    restart_marks(c, plot, s);
    sparkline(c, plot, s, false, mi.min_span);
  } else
    c.text(fs.small, plot.x + plot.w / 2,
           plot.y + plot.h / 2 + hl.plot_empty_dy, tr(Str::Collecting),
           Align::Center);
}

void tile(Canvas &c, Rect box, Metric metric, const Model &m,
          const History &h) {
  const TileLayout &tl = c.layout().tiles;
  const FontSet &fs = c.fonts();
  const MetricInfo &mi = info(metric);
  const Reading &r = m.now[index(metric)];
  const int x = box.x + tl.pad;
  const int right = box.right() - tl.pad;
  c.text(fs.label, x, box.y + tl.label_dy, tr(mi.label));
  c.text(fs.small, right, box.y + tl.label_dy, tr(mi.unit), Align::Right);
  Text t{};
  c.text(fs.value, x, box.y + tl.value_dy, format_reading(t, metric, r));
  if (r.quality != Quality::Valid) {
    Text cap{};
    c.text(fs.small, right, box.y + tl.value_dy,
           not_valid_caption(r, m.status, cap), Align::Right);
  }
  const Rect spark{x, box.y + tl.spark_dy, box.w - 2 * tl.pad, tl.spark_h};
  sparkline(c, spark, h.series(metric, Window::Recent), mi.zero_floor,
            mi.min_span);
}

// The hero and the six other metrics, in this order across then down.
constexpr std::array<Metric, 6> kTiles{Metric::Co2, Metric::Temp, Metric::Rh,
                                       Metric::Pm1, Metric::Pm4,  Metric::Pm10};

void dashboard(Canvas &c, const Model &m, const History &h) {
  hero(c, m, h);
  const TileLayout &tl = c.layout().tiles;
  const int w = c.width();
  // One pixel rules part the tiles, a rule above each row and one right of
  // each column but the last.
  for (int r = 0; r < tl.rows; ++r)
    c.hline(0, tl.top + r * (tl.h + 1) - 1, w);
  for (int col = 1; col < tl.cols; ++col)
    c.vline(col * (tl.w + 1) - 1, tl.top, tl.rows * tl.h + tl.rows - 1);
  const auto cols = static_cast<std::size_t>(std::max(tl.cols, 1));
  const std::size_t shown =
      std::min(kTiles.size(), cols * static_cast<std::size_t>(tl.rows));
  for (std::size_t i = 0; i < shown; ++i) {
    const int col = static_cast<int>(i % cols);
    const int row = static_cast<int>(i / cols);
    const Rect box{col * (tl.w + 1), tl.top + row * (tl.h + 1), tl.w, tl.h};
    tile(c, box, kTiles[i], m, h);
  }
}

void page_number(Canvas &c, std::uint8_t page) {
  const Layout &l = c.layout();
  Text t{};
  const int n = std::snprintf(t.data(), t.size(), "%u/%u",
                              static_cast<unsigned>(page) + 1,
                              static_cast<unsigned>(kPageCount));
  c.text(l.fonts.small, c.width() - l.header.margin, l.page.page_number_y,
         view(t, n), Align::Right);
}

void chart_page(Canvas &c, std::uint8_t page, const Model &m,
                const History &h) {
  const Layout &l = c.layout();
  const ChartPageLayout &cl = l.chart_page;
  const FontSet &fs = l.fonts;
  const Metric metric = kChartOrder[page - 1U];
  const MetricInfo &mi = info(metric);
  const int tw = c.text(fs.title, l.page.title.x, l.page.title.y, tr(mi.label));
  Text sub{};
  const std::string_view unit = tr(mi.unit);
  const int ns = std::snprintf(sub.data(), sub.size(), tr_fmt(Str::ChartSubFmt),
                               static_cast<int>(unit.size()), unit.data());
  c.text(fs.small, l.page.title.x + tw + cl.sub_gap, l.page.title.y,
         view(sub, ns));
  page_number(c, page);

  const Series s = h.series(metric, Window::Day);
  const Range range = data_range(s);
  const Reading &now = m.now[index(metric)];
  struct Stat {
    Str name;
    Reading r;
  };
  const std::array<Stat, 3> stats{{
      {.name = Str::StatNow, .r = now},
      {.name = Str::StatMin,
       .r = {.value = range.lo,
             .quality = range.valid ? Quality::Valid : Quality::Absent,
             .warm_left_s = 0}},
      {.name = Str::StatMax,
       .r = {.value = range.hi,
             .quality = range.valid ? Quality::Valid : Quality::Absent,
             .warm_left_s = 0}},
  }};
  int x = l.page.title.x;
  for (const auto &st : stats) {
    const int nw = c.text(fs.small, x, cl.stats_y, tr(st.name));
    Text t{};
    c.text(fs.label, x + nw + cl.stat_gap, cl.stats_y,
           format_reading(t, metric, st.r));
    x += cl.stats_step;
  }

  const std::array<std::string_view, 5> labels{
      tr(Str::AxisMinus24h), tr(Str::AxisMinus18h), tr(Str::AxisMinus12h),
      tr(Str::AxisMinus6h), tr(Str::AxisNow)};
  line_chart(c, cl.chart, s,
             {.zero_floor = mi.zero_floor,
              .min_span = mi.min_span,
              .x_labels = labels,
              .empty_text = tr(Str::NoDataYet)});
}

void row(Canvas &c, int y, Str name, std::string_view value) {
  const Layout &l = c.layout();
  c.text(l.fonts.small, l.page.title.x, y, tr(name));
  text_fit(c, l.fonts.label, l.status.value_x, y, value,
           c.width() - l.status.value_x - l.header.margin);
}

std::string_view link_word(Link l, Str waiting) {
  switch (l) {
  case Link::Off:
    return tr(Str::LinkOff);
  case Link::Waiting:
    return tr(waiting);
  case Link::Connected:
    return tr(Str::LinkConnected);
  }
  return tr(Str::LinkOff);
}

// SEN6x device status bits that the SEN63C sets, Sensirion SEN6x datasheet
// "Read Device Status" (0xD206).
struct StatusBit {
  int bit = 0;
  Str name;
};
constexpr std::array<StatusBit, 5> kStatusBits{{
    {.bit = 21, .name = Str::BitFanSpeed},
    {.bit = 12, .name = Str::BitCo2},
    {.bit = 11, .name = Str::BitPm},
    {.bit = 6, .name = Str::BitRht},
    {.bit = 4, .name = Str::BitFan},
}};

std::string_view sensor_status_text(std::uint32_t bits, Text &t) {
  if (bits == 0)
    return tr(Str::SensorOk);
  int n = 0;
  std::uint32_t known = 0;
  for (const auto &sb : kStatusBits) {
    const std::uint32_t mask = 1U << sb.bit;
    known |= mask;
    if ((bits & mask) == 0)
      continue;
    const std::string_view name = tr(sb.name);
    const int w = std::snprintf(
        t.data() + n, t.size() - static_cast<std::size_t>(n), "%s%.*s",
        n == 0 ? "" : ", ", static_cast<int>(name.size()), name.data());
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

// The rule and the line under it at the bottom of a page. A line too wide
// for a portrait frame wraps once, the layout leaves room for the second.
void footer(Canvas &c, int x, std::string_view text, Align align) {
  const Layout &l = c.layout();
  c.hline(0, l.page.footer_rule_y, c.width());
  const int max_w = align == Align::Left ? c.width() - x - l.header.margin
                                         : c.width() - 2 * l.header.margin;
  static_cast<void>(
      c.text_lines(l.fonts.small, x, l.page.footer_y, text, align, max_w));
}

void status_page(Canvas &c, std::uint8_t page, const Model &m) {
  const Layout &l = c.layout();
  const Status &st = m.status;
  c.text(l.fonts.title, l.page.title.x, l.page.title.y, tr(Str::StatusTitle));
  page_number(c, page);
  int y = l.status.top;
  const int step = l.status.step;
  Text t{};

  if (st.utc_s) {
    const std::int64_t s = *st.utc_s;
    const std::int64_t days = (s >= 0 ? s : s - 86399) / 86400;
    const std::int64_t sod = s - days * 86400;
    int yr = 0;
    unsigned mo = 0;
    unsigned d = 0;
    civil(days, yr, mo, d);
    const int n = std::snprintf(t.data(), t.size(), tr_fmt(Str::ClockDateFmt),
                                yr, mo, d, static_cast<int>(sod / 3600),
                                static_cast<int>((sod / 60) % 60));
    row(c, y, Str::RowClock, view(t, n));
  } else {
    row(c, y, Str::RowClock, tr(Str::ClockNotSet));
  }
  y += step;

  {
    // unsigned, not uint32_t, which is long on Xtensa and int on the host.
    const auto u = static_cast<unsigned>(st.uptime_s);
    const int n =
        u >= 86400
            ? std::snprintf(t.data(), t.size(), tr_fmt(Str::UptimeDaysFmt),
                            u / 86400, (u / 3600) % 24, (u / 60) % 60)
            : std::snprintf(t.data(), t.size(), "%02u:%02u:%02u", u / 3600,
                            (u / 60) % 60, u % 60);
    row(c, y, Str::RowUptime, view(t, n));
    y += step;
  }
  {
    const int n = std::snprintf(t.data(), t.size(), "%u",
                                static_cast<unsigned>(st.samples));
    row(c, y, Str::RowSamples, view(t, n));
    y += step;
  }
  if (st.sd_ok) {
    const double gb = static_cast<double>(st.sd_free_bytes) / 1e9;
    const int n = std::snprintf(t.data(), t.size(), tr_fmt(Str::SdOkFmt), gb);
    row(c, y, Str::RowSdCard, view(t, n));
  } else {
    row(c, y, Str::RowSdCard, tr(Str::SdMissing));
  }
  y += step;
  row(c, y, Str::RowBluetooth, link_word(st.ble, Str::LinkAdvertising));
  y += step;
  row(c, y, Str::RowWifi, link_word(st.wifi, Str::LinkConnecting));
  y += step;
  row(c, y, Str::RowThing, st.thing.empty() ? tr(Str::ThingUnset) : st.thing);
  y += step;
  row(c, y, Str::RowFirmware, st.firmware);
  y += step;
  if (st.sensor_present) {
    const int n = std::snprintf(
        t.data(), t.size(), tr_fmt(Str::SensorFmt),
        static_cast<int>(st.sensor_model.size()), st.sensor_model.data(),
        static_cast<int>(st.sensor_serial.size()), st.sensor_serial.data(),
        static_cast<int>(st.sensor_fw.size()), st.sensor_fw.data());
    row(c, y, Str::RowSensor, view(t, n));
    y += step;
    Text bits{};
    row(c, y, Str::RowSensorState,
        st.sensor_status ? sensor_status_text(*st.sensor_status, bits)
                         : tr(Str::SensorNotRead));
  } else {
    const int n = std::snprintf(
        t.data(), t.size(), tr_fmt(Str::SensorMissingFmt),
        static_cast<int>(st.sensor_model.size()), st.sensor_model.data());
    row(c, y, Str::RowSensor, view(t, n));
  }

  footer(c, l.page.title.x, tr(Str::BandFootnote), Align::Left);
}

// The whole page while a phone pairs. The passkey is the hero line, so it
// reads from across a room, and the device name tells two boards apart.
void pairing_page(Canvas &c, const Status &st) {
  const Layout &l = c.layout();
  const PairingLayout &pl = l.pairing;
  const int mid = c.width() / 2;
  c.text(l.fonts.title, mid, pl.title_y, tr(Str::PairTitle), Align::Center);
  c.text(l.fonts.label, mid, pl.prompt_y, tr(Str::PairPrompt), Align::Center);
  Text t{};
  const int n = std::snprintf(t.data(), t.size(), "%06u",
                              static_cast<unsigned>(*st.pairing_passkey));
  c.text(l.fonts.hero, mid, pl.code_y, view(t, n), Align::Center);
  if (!st.device_name.empty()) {
    const int m = std::snprintf(t.data(), t.size(), tr_fmt(Str::PairDeviceFmt),
                                static_cast<int>(st.device_name.size()),
                                st.device_name.data());
    c.text(l.fonts.label, mid, pl.device_y, view(t, m), Align::Center);
  }
  footer(c, mid, tr(Str::PairFootnote), Align::Center);
}

// One centred "name value" line of the sleeping screen, skipped when the
// value is empty.
int asleep_line(Canvas &c, int y, Str name, std::string_view value) {
  if (value.empty())
    return y;
  const Layout &l = c.layout();
  const std::string_view nm = tr(name);
  const int nw = text_width(l.fonts.small, nm) + l.asleep.name_gap;
  const int vw = text_width(l.fonts.label, value);
  const int x = (c.width() - nw - vw) / 2;
  c.text(l.fonts.small, x, y, nm);
  c.text(l.fonts.label, x + nw, y, value);
  return y + l.asleep.line_step;
}

} // namespace

void render_asleep(Canvas &c, const Model &model) {
  c.clear();
  const Layout &l = c.layout();
  const AsleepLayout &al = l.asleep;
  const Status &st = model.status;
  const Rect card = al.card;
  const int mid = c.width() / 2;
  c.round_rect(card);
  c.text(l.fonts.label, mid, card.y + al.brand_dy, tr(Str::Brand),
         Align::Center);
  c.text(l.fonts.title, mid, card.y + al.title_dy, tr(Str::AsleepTitle),
         Align::Center);
  c.text(l.fonts.small, mid, card.y + al.hint_dy, tr(Str::AsleepHint),
         Align::Center);
  c.dotted_hline(card.x + al.rule_inset, card.y + al.rule_dy,
                 card.w - 2 * al.rule_inset, 1, 3);
  int y = card.y + al.lines_dy;
  y = asleep_line(c, y, Str::RowThing, st.thing);
  static_cast<void>(asleep_line(c, y, Str::AsleepDevice, st.device_name));
}

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
    return {.page = page, .full_refresh = false};
  case Input::Exit:
    return {.page = kDashboardPage, .full_refresh = false};
  }
  return {.page = page, .full_refresh = false};
}

void render(Canvas &c, std::uint8_t page, const Model &model,
            const History &history) {
  c.clear();
  if (page >= kPageCount)
    page = kDashboardPage;
  header(c, model.status);
  if (model.status.pairing_passkey)
    pairing_page(c, model.status);
  else if (page == kDashboardPage)
    dashboard(c, model, history);
  else if (page == kStatusPage)
    status_page(c, page, model);
  else
    chart_page(c, page, model, history);
}

} // namespace opensensor::ui
