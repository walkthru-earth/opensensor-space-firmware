// Host tests for the CrowPanel e-paper UI, canvas clipping, UTF-8, history
// rings, chart gaps, PM2.5 bands, reading quality, navigation, the refresh
// cadence, the settings blob and popup, the theme, the sleeping screen and
// the boot order of the chart history restore. Built and run
// by tools/test_crowpanel_ui.py with sanitizers.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "air_band.h"
#include "canvas.h"
#include "chart.h"
#include "history.h"
#include "pages.h"
#include "restore_order.h"
#include "settings.h"
#include "ui_model.h"

using namespace opensensor::ui;

namespace {

int g_checks = 0;

void check(bool ok, const char *what, int line) {
  ++g_checks;
  if (ok)
    return;
  std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
  std::exit(1);
}
#define CHECK(x) check((x), #x, __LINE__)

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

struct Buffer {
  std::array<std::uint8_t, kFrameBytes> bytes{};
  Canvas canvas{bytes};
};

int ink_count(const Canvas &c, Rect r) {
  int n = 0;
  for (int y = r.y; y < r.bottom(); ++y)
    for (int x = r.x; x < r.right(); ++x)
      n += c.get(x, y) == Ink::Black ? 1 : 0;
  return n;
}

void test_canvas() {
  auto b = std::make_unique<Buffer>();
  Canvas &c = b->canvas;
  c.clear();
  c.pixel(0, 0);
  CHECK(b->bytes[0] == 0x80);
  c.pixel(9, 0);
  CHECK(b->bytes[1] == 0x40);
  c.pixel(399, 299);
  CHECK(b->bytes[kFrameBytes - 1] == 0x01);
  // Far outside the frame, nothing is written and nothing overflows.
  c.pixel(-1, 0);
  c.pixel(400, 0);
  c.pixel(0, 300);
  c.hline(-1000, 5, 3000);
  c.vline(5, -1000, 3000);
  c.fill({-50, -50, 20, 20});
  c.line(-100000, -100000, 100000, 100000);
  c.rect({390, 290, 50, 50});
  CHECK(c.get(-1, -1) == Ink::White);
  c.clear();
  c.fill({10, 10, 4, 3});
  CHECK(ink_count(c, {0, 0, 40, 40}) == 12);
  c.invert_rect({10, 10, 4, 3});
  CHECK(ink_count(c, {0, 0, 40, 40}) == 0);
  // invert_all flips every pixel and twice gives the frame back.
  c.fill({10, 10, 4, 3});
  const auto before = b->bytes;
  c.invert_all();
  CHECK(ink_count(c, {0, 0, kWidth, kHeight}) == kWidth * kHeight - 12);
  CHECK(c.get(10, 10) == Ink::White && c.get(0, 0) == Ink::Black);
  c.invert_all();
  CHECK(b->bytes == before);
  c.clear();
  c.line(0, 0, 9, 3);
  CHECK(c.get(0, 0) == Ink::Black && c.get(9, 3) == Ink::Black);
  c.clear(Ink::Black);
  CHECK(b->bytes[123] == 0xFF);
}

void test_utf8_and_text() {
  std::size_t pos = 0;
  constexpr std::string_view micro = "µg";
  CHECK(next_codepoint(micro, pos) == 0xB5 && pos == 2);
  CHECK(next_codepoint(micro, pos) == 'g' && pos == 3);
  pos = 0;
  constexpr std::string_view sub2 = "CO₂";
  CHECK(next_codepoint(sub2, pos) == 'C');
  CHECK(next_codepoint(sub2, pos) == 'O');
  CHECK(next_codepoint(sub2, pos) == 0x2082 && pos == sub2.size());
  // Truncated and stray bytes move on by one and never read past the end.
  const char truncated[] = {'\xE2', '\x82'};
  pos = 0;
  CHECK(next_codepoint({truncated, 2}, pos) == 0xFFFD && pos == 1);
  pos = 0;
  CHECK(next_codepoint("\x80", pos) == 0xFFFD && pos == 1);
  CHECK(find_glyph(fonts::label, 'A') != nullptr);
  CHECK(find_glyph(fonts::hero, 'A') == nullptr);
  CHECK(find_glyph(fonts::hero, '7') != nullptr);
  CHECK(find_glyph(fonts::small, 0x00B3) != nullptr);
  auto b = std::make_unique<Buffer>();
  const int adv = b->canvas.text(fonts::value, 10, 40, "12.4");
  CHECK(adv == text_width(fonts::value, "12.4"));
  CHECK(adv > 0);
  CHECK(ink_count(b->canvas, {0, 0, 200, 60}) > 0);
  // Right aligned text ends at x.
  b->canvas.clear();
  b->canvas.text(fonts::label, 200, 100, "ppm", Align::Right);
  CHECK(ink_count(b->canvas, {201, 80, 50, 30}) == 0);
  CHECK(ink_count(b->canvas, {150, 80, 51, 30}) > 0);
}

std::array<float, kMetricCount> all(float v) {
  std::array<float, kMetricCount> a{};
  a.fill(v);
  return a;
}

void test_history() {
  auto h = std::make_unique<History>();
  CHECK(h->empty());
  h->push(1000, all(1.0F));
  h->push(1010, all(2.0F));
  h->push(1020, all(kNaN));
  h->push(1060, all(5.0F)); // Skips the 1030, 1040 and 1050 slots.
  const Series s = h->series(Metric::Pm25, Window::Recent);
  CHECK(s.size() == History::kRecentSlots);
  const std::size_t n = s.size();
  CHECK(s[n - 1] == 5.0F);
  CHECK(std::isnan(s[n - 2]) && std::isnan(s[n - 3]) && std::isnan(s[n - 4]));
  CHECK(std::isnan(s[n - 5]));
  CHECK(s[n - 6] == 2.0F && s[n - 7] == 1.0F);
  CHECK(std::isnan(s[0]));

  // Minute 16 (960..1019 s) holds 1 and 2, minute 17 holds NaN then 5.
  const Series d = h->series(Metric::Co2, Window::Day);
  CHECK(d[d.size() - 1] == 5.0F);
  CHECK(d[d.size() - 2] == 1.5F);

  // Same slot replaces in the recent ring and averages in the day ring.
  h->push(1065, all(7.0F));
  CHECK(h->series(Metric::Rh, Window::Recent)[n - 1] == 7.0F);
  CHECK(h->series(Metric::Rh, Window::Day)[d.size() - 1] == 6.0F);
  // Time going backwards is ignored.
  h->push(900, all(99.0F));
  CHECK(h->series(Metric::Rh, Window::Recent)[n - 1] == 7.0F);

  // Minute 18 has only NaN, minutes 19 and 20 have no push at all.
  h->push(1080, all(kNaN));
  h->advance(1200);
  const Series d2 = h->series(Metric::Temp, Window::Day);
  CHECK(std::isnan(d2[d2.size() - 1]));
  CHECK(std::isnan(d2[d2.size() - 2]));
  CHECK(std::isnan(d2[d2.size() - 3]));
  CHECK(d2[d2.size() - 4] == 6.0F);

  // A gap longer than the window clears it.
  h->push(1200 + 3 * 3600, all(3.0F));
  const Series s3 = h->series(Metric::Pm1, Window::Recent);
  int finite = 0;
  for (std::size_t i = 0; i < s3.size(); ++i)
    finite += std::isfinite(s3[i]) ? 1 : 0;
  CHECK(finite == 1);
  // Wrapping past the ring end keeps order oldest first.
  auto w = std::make_unique<History>();
  for (std::uint32_t t = 0; t < 3 * 3600; t += 10)
    w->push(t, all(static_cast<float>(t)));
  const Series s4 = w->series(Metric::Pm10, Window::Recent);
  CHECK(s4[s4.size() - 1] == 10790.0F);
  CHECK(s4[0] == 10790.0F - 10.0F * (History::kRecentSlots - 1));

  // An older sample in the same 10 s slot and minute is ignored, it must not
  // replace the newer value or join the minute mean.
  auto o = std::make_unique<History>();
  const auto last = [&o](Window win) {
    const Series ser = o->series(Metric::Pm25, win);
    return ser[ser.size() - 1];
  };
  o->push(11, all(10.0F));
  o->push(10, all(90.0F));
  CHECK(last(Window::Recent) == 10.0F);
  CHECK(last(Window::Day) == 10.0F);
  // The same time and a later time in the same slot still count.
  o->push(11, all(30.0F));
  CHECK(last(Window::Recent) == 30.0F);
  CHECK(last(Window::Day) == 20.0F);
  o->push(19, all(50.0F));
  CHECK(last(Window::Recent) == 50.0F);
  CHECK(last(Window::Day) == 30.0F);
  // reset() forgets the last push time.
  o->reset();
  o->push(5, all(7.0F));
  CHECK(last(Window::Recent) == 7.0F);
}

// Ring positions, oldest first, that carry a restart bit.
std::vector<std::size_t> restarts_of(const Series &s) {
  std::vector<std::size_t> out;
  for (std::size_t i = 0; i < s.size(); ++i)
    if (s.restart(i))
      out.push_back(i);
  return out;
}

// Every point of every metric in both windows matches, NaN for NaN, and
// neither has a restart point.
bool same_history(const History &a, const History &b) {
  if (a.empty() != b.empty())
    return false;
  for (std::size_t m = 0; m < kMetricCount; ++m)
    for (const Window win : {Window::Recent, Window::Day}) {
      const Series x = a.series(static_cast<Metric>(m), win);
      const Series y = b.series(static_cast<Metric>(m), win);
      if (x.size() != y.size() || x.start != y.start)
        return false;
      if (!restarts_of(x).empty() || !restarts_of(y).empty())
        return false;
      for (std::size_t i = 0; i < x.size(); ++i) {
        const bool both_nan = std::isnan(x[i]) && std::isnan(y[i]);
        if (!both_nan && x[i] != y[i])
          return false;
      }
    }
  return true;
}

// The live script test_history() also runs, as one function so the seeded
// and plain objects see the same calls.
void live_script(History &h) {
  h.push(1000, all(1.0F));
  h.push(1010, all(2.0F));
  h.push(1020, all(kNaN));
  h.push(1060, all(5.0F));
  h.push(1065, all(7.0F));
  h.push(900, all(99.0F));
  h.push(1080, all(kNaN));
  h.advance(1200);
  h.push(1200 + 3 * 3600, all(3.0F));
  h.advance(1200 + 3 * 3600 + 45);
}

void test_history_seed() {
  // No seeding, or seeding calls with no rows, change nothing.
  {
    auto plain = std::make_unique<History>();
    live_script(*plain);
    auto finished = std::make_unique<History>();
    finished->finish_seed();
    live_script(*finished);
    CHECK(same_history(*plain, *finished));
    auto empty_boots = std::make_unique<History>();
    empty_boots->seed_boot();
    empty_boots->seed_boot();
    empty_boots->finish_seed();
    CHECK(empty_boots->empty());
    live_script(*empty_boots);
    CHECK(same_history(*plain, *empty_boots));
    // seed_boot() without finish_seed(), the first push closes seeding.
    auto open = std::make_unique<History>();
    open->seed_boot();
    live_script(*open);
    CHECK(same_history(*plain, *open));
  }

  // One seeded boot then live rows. Recent slots 0, 1, 2 hold the boot,
  // 3 is the restart and the live boot starts at 4 with its second 0.
  {
    auto h = std::make_unique<History>();
    h->seed_boot();
    h->seed(100, all(1.0F));
    CHECK(!h->empty());
    h->seed(110, all(2.0F));
    h->seed(120, all(3.0F));
    h->finish_seed();
    CHECK(!h->empty());
    h->push(5, all(9.0F));
    const Series s = h->series(Metric::Pm25, Window::Recent);
    const std::size_t n = s.size();
    CHECK(s[n - 1] == 9.0F);
    CHECK(std::isnan(s[n - 2]) && s.restart(n - 2));
    CHECK(s[n - 3] == 3.0F && s[n - 4] == 2.0F && s[n - 5] == 1.0F);
    CHECK(std::isnan(s[n - 6]));
    CHECK(restarts_of(s) == std::vector<std::size_t>{n - 2});
    // Minutes of the boot's own clock, 100 and 110 s share minute 1, 120 s
    // is minute 2.
    const Series d = h->series(Metric::Co2, Window::Day);
    const std::size_t nd = d.size();
    CHECK(d[nd - 1] == 9.0F);
    CHECK(std::isnan(d[nd - 2]) && d.restart(nd - 2));
    CHECK(d[nd - 3] == 3.0F && d[nd - 4] == 1.5F);
    CHECK(restarts_of(d) == std::vector<std::size_t>{nd - 2});
    // Live rows keep their own spacing after the restart.
    h->push(45, all(4.0F));
    const Series s2 = h->series(Metric::Pm25, Window::Recent);
    CHECK(s2[n - 1] == 4.0F && s2[n - 5] == 9.0F && s2.restart(n - 6));
    CHECK(std::isnan(s2[n - 2]) && !s2.restart(n - 2));
    // Seeding is closed once live rows came.
    h->seed_boot();
    h->seed(1000, all(50.0F));
    h->finish_seed();
    CHECK(h->series(Metric::Pm25, Window::Recent)[n - 1] == 4.0F);
    CHECK(restarts_of(h->series(Metric::Pm25, Window::Recent)).size() == 1);
  }

  // Two seeded boots, each with its own restart after it.
  {
    auto h = std::make_unique<History>();
    h->seed_boot();
    h->seed(50, all(1.0F));
    h->seed(60, all(2.0F));
    h->seed_boot();
    h->seed(7, all(3.0F));
    h->seed(17, all(4.0F));
    h->finish_seed();
    h->push(0, all(5.0F));
    const Series s = h->series(Metric::Rh, Window::Recent);
    const std::size_t n = s.size();
    CHECK(s[n - 1] == 5.0F && s[n - 3] == 4.0F && s[n - 4] == 3.0F);
    CHECK(s[n - 6] == 2.0F && s[n - 7] == 1.0F);
    CHECK((restarts_of(s) == std::vector<std::size_t>{n - 5, n - 2}));
    // Boot 1 has minutes 0 and 1, boot 2 one minute with 3 and 4.
    const Series d = h->series(Metric::Rh, Window::Day);
    const std::size_t nd = d.size();
    CHECK(d[nd - 1] == 5.0F && d[nd - 3] == 3.5F);
    CHECK(d[nd - 5] == 2.0F && d[nd - 6] == 1.0F);
    CHECK((restarts_of(d) == std::vector<std::size_t>{nd - 4, nd - 2}));
  }

  // A seeded boot with no rows adds no restart, and seed() before any
  // seed_boot() opens the first boot.
  {
    auto h = std::make_unique<History>();
    h->seed(30, all(1.0F));
    h->seed_boot();
    h->seed_boot();
    h->seed(40, all(2.0F));
    h->seed_boot();
    h->finish_seed();
    h->push(0, all(3.0F));
    const Series s = h->series(Metric::Temp, Window::Recent);
    const std::size_t n = s.size();
    CHECK(s[n - 1] == 3.0F && s[n - 3] == 2.0F && s[n - 5] == 1.0F);
    CHECK((restarts_of(s) == std::vector<std::size_t>{n - 4, n - 2}));
  }

  // A minute bucket never mixes boots. Both rows fall in minute 0 of their
  // own boot, they stay apart with a restart between.
  {
    auto h = std::make_unique<History>();
    h->seed_boot();
    h->seed(30, all(10.0F));
    h->seed_boot();
    h->seed(40, all(20.0F));
    h->finish_seed();
    const Series d = h->series(Metric::Pm10, Window::Day);
    const std::size_t nd = d.size();
    CHECK(std::isnan(d[nd - 1]) && d.restart(nd - 1));
    CHECK(d[nd - 2] == 20.0F);
    CHECK(std::isnan(d[nd - 3]) && d.restart(nd - 3));
    CHECK(d[nd - 4] == 10.0F);
    bool mixed = false;
    for (std::size_t i = 0; i < nd; ++i)
      mixed = mixed || d[i] == 15.0F;
    CHECK(!mixed);
  }

  // More than the rings hold drops the oldest, restart bits included.
  {
    auto h = std::make_unique<History>();
    h->seed_boot();
    h->seed(0, all(-1.0F));
    h->seed_boot();
    // 30 h at one row a minute, the value is the boot minute.
    for (std::uint32_t t = 0; t < 30 * 3600; t += 60)
      h->seed(t, all(static_cast<float>(t / 60)));
    h->finish_seed();
    h->push(0, all(-2.0F));
    // Day slots, boot 1 at 0, restart 1, boot 2 at 2..1801, restart 1802,
    // live 1803. The window is slots 364..1803.
    const Series d = h->series(Metric::Pm1, Window::Day);
    const std::size_t nd = d.size();
    CHECK(d[0] == 362.0F);
    CHECK(d[nd - 3] == 1799.0F && d[nd - 1] == -2.0F);
    CHECK(restarts_of(d) == std::vector<std::size_t>{nd - 2});
    // Recent slots, one per minute row, five NaN between.
    const Series s = h->series(Metric::Pm1, Window::Recent);
    const std::size_t n = s.size();
    CHECK(s[n - 1] == -2.0F && s.restart(n - 2) && s[n - 3] == 1799.0F);
    CHECK(std::isnan(s[n - 4]) && s[n - 9] == 1798.0F);
    CHECK(restarts_of(s) == std::vector<std::size_t>{n - 2});
  }

  // advance() before the first live push grows a gap after the restart and
  // never moves it, and the first row then lands at its own time.
  {
    auto h = std::make_unique<History>();
    h->seed_boot();
    h->seed(500, all(1.0F));
    h->seed(510, all(2.0F));
    // No finish_seed(), advance() closes seeding itself.
    h->advance(25);
    const Series s = h->series(Metric::Co2, Window::Recent);
    const std::size_t n = s.size();
    // Live seconds 0, 10 and 20 are slots after the restart.
    CHECK(std::isnan(s[n - 1]) && std::isnan(s[n - 2]) && std::isnan(s[n - 3]));
    CHECK(std::isnan(s[n - 4]) && s.restart(n - 4) && s[n - 5] == 2.0F);
    CHECK(restarts_of(s) == std::vector<std::size_t>{n - 4});
    const Series d = h->series(Metric::Co2, Window::Day);
    const std::size_t nd = d.size();
    CHECK(std::isnan(d[nd - 1]) && d.restart(nd - 2) && d[nd - 3] == 1.5F);
    h->push(29, all(8.0F));
    const Series s2 = h->series(Metric::Co2, Window::Recent);
    CHECK(s2[n - 1] == 8.0F && s2.restart(n - 4) && s2[n - 5] == 2.0F);
    CHECK(restarts_of(s2) == std::vector<std::size_t>{n - 4});
    const Series d2 = h->series(Metric::Co2, Window::Day);
    CHECK(d2[nd - 1] == 8.0F && d2.restart(nd - 2));
    h->advance(200);
    CHECK(restarts_of(h->series(Metric::Co2, Window::Recent)) ==
          std::vector<std::size_t>{n - 22});
  }

  // reset() clears seeds and restarts, and seeding works again after it.
  {
    auto h = std::make_unique<History>();
    h->seed_boot();
    h->seed(100, all(1.0F));
    h->finish_seed();
    h->push(10, all(2.0F));
    h->reset();
    CHECK(h->empty());
    auto fresh = std::make_unique<History>();
    CHECK(same_history(*h, *fresh));
    live_script(*h);
    live_script(*fresh);
    CHECK(same_history(*h, *fresh));
    h->reset();
    h->seed_boot();
    h->seed(100, all(1.0F));
    h->finish_seed();
    const Series s = h->series(Metric::Pm25, Window::Recent);
    CHECK(restarts_of(s) == std::vector<std::size_t>{s.size() - 1});
  }
}

void test_chart() {
  const std::array<float, 3> nan3{kNaN, kNaN, kNaN};
  CHECK(!data_range({.ring = nan3, .start = 0, .restarts = {}}).valid);
  const std::array<float, 4> some{3.0F, kNaN, -1.0F, 8.0F};
  const Range r = data_range({.ring = some, .start = 1, .restarts = {}});
  CHECK(r.valid && r.lo == -1.0F && r.hi == 8.0F);

  Ticks t = nice_ticks(0.0F, 37.3F, 5);
  CHECK(t.lo == 0.0F && t.hi == 40.0F && t.step == 10.0F && t.count == 5);
  t = nice_ticks(412.0F, 1290.0F, 6);
  CHECK(t.lo <= 412.0F && t.hi >= 1290.0F && t.count <= 6);
  t = nice_ticks(21.0F, 21.0F, 4);
  CHECK(t.lo < 21.0F && t.hi > 21.0F && t.count >= 2);
  t = nice_ticks(20.4F, 21.7F, 5);
  CHECK(t.decimals == 1 && t.step == 0.5F);
  // Every 1, 2, 5 step of the first power of ten rounds out to seven ticks
  // here, so the search moves on to 20.
  t = nice_ticks(9.5F, 54.5F, 6);
  CHECK(t.count <= 6 && t.lo <= 9.5F && t.hi >= 54.5F);
  CHECK(t.lo == 0.0F && t.hi == 60.0F && t.step == 20.0F && t.count == 4);
  // Fewer than three ticks cannot always cover a range around zero.
  t = nice_ticks(-1.0F, 1.0F, 2);
  CHECK(t.count <= 3 && t.lo <= -1.0F && t.hi >= 1.0F);
  // A sweep of ranges across magnitudes never exceeds the limit and always
  // covers the data.
  bool sweep_ok = true;
  for (int m = 3; m <= 8; ++m)
    for (int e = -3; e <= 4; ++e)
      for (int a = 0; a <= 40; ++a)
        for (int span = 1; span <= 40; ++span) {
          const float scale = std::pow(10.0F, static_cast<float>(e));
          const float lo = (static_cast<float>(a) * 0.37F - 7.0F) * scale;
          const float hi = lo + static_cast<float>(span) * 0.53F * scale;
          const Ticks st = nice_ticks(lo, hi, m);
          const float slack = st.step * 1e-3F;
          if (st.count > m || st.count < 2 || st.step <= 0.0F ||
              st.lo > lo + slack || st.hi < hi - slack) {
            std::fprintf(stderr, "nice_ticks(%g, %g, %d) count %d step %g\n",
                         static_cast<double>(lo), static_cast<double>(hi), m,
                         st.count, static_cast<double>(st.step));
            sweep_ok = false;
          }
        }
  CHECK(sweep_ok);

  // A NaN in the middle leaves its column blank and breaks the line.
  auto b = std::make_unique<Buffer>();
  const std::array<float, 5> gap{1.0F, 1.0F, kNaN, 1.0F, 1.0F};
  plot_series(b->canvas, {100, 100, 5, 10},
              {.ring = gap, .start = 0, .restarts = {}}, 0.0F, 2.0F);
  CHECK(ink_count(b->canvas, {102, 90, 1, 30}) == 0);
  CHECK(ink_count(b->canvas, {100, 90, 2, 30}) == 2);
  // A lone point draws a 3 x 3 dot.
  b->canvas.clear();
  const std::array<float, 5> lone{kNaN, kNaN, 1.0F, kNaN, kNaN};
  plot_series(b->canvas, {100, 100, 5, 10},
              {.ring = lone, .start = 0, .restarts = {}}, 0.0F, 2.0F);
  CHECK(ink_count(b->canvas, {90, 90, 30, 30}) == 9);
  // The envelope keeps a single spike among 1440 points on 344 columns.
  b->canvas.clear();
  std::array<float, 1440> day{};
  day.fill(1.0F);
  day[777] = 100.0F;
  const Rect plot{40, 50, 344, 100};
  plot_series(b->canvas, plot, {.ring = day, .start = 0, .restarts = {}}, 0.0F,
              100.0F);
  CHECK(ink_count(b->canvas, {plot.x, plot.y, plot.w, 1}) >= 1);
  // A whole hour of NaN in a day draws a clean gap.
  b->canvas.clear();
  for (std::size_t i = 600; i < 660; ++i)
    day[i] = kNaN;
  day[777] = 1.0F;
  plot_series(b->canvas, plot, {.ring = day, .start = 0, .restarts = {}}, 0.0F,
              2.0F);
  const int gx = plot.x + static_cast<int>((630 * 344) / 1440);
  CHECK(ink_count(b->canvas, {gx - 3, plot.y, 7, plot.h}) == 0);
  CHECK(ink_count(b->canvas, {plot.x, plot.y, 20, plot.h}) > 0);
  // A restart draws a dashed line down its column, a plain NaN does not.
  {
    std::array<float, 1440> flat{};
    flat.fill(5.0F);
    flat[720] = kNaN;
    std::array<std::uint8_t, 180> bits{};
    const Rect chart_plot{46, 82, 344, 186};
    const int col = chart_plot.x + static_cast<int>((721 * 343 - 1) / 1440);
    const Rect whole{0, 0, kWidth, kHeight};
    b->canvas.clear();
    line_chart(b->canvas, chart_plot,
               {.ring = flat, .start = 0, .restarts = {}}, {});
    const int plain = ink_count(b->canvas, {col, chart_plot.y, 1, 60});
    const int plain_all = ink_count(b->canvas, whole);
    const int plain_col = ink_count(b->canvas, {col, 0, 1, kHeight});
    bits[720 / 8] = static_cast<std::uint8_t>(1U << (720 % 8));
    b->canvas.clear();
    line_chart(b->canvas, chart_plot,
               {.ring = flat, .start = 0, .restarts = bits}, {});
    const int marked = ink_count(b->canvas, {col, chart_plot.y, 1, 60});
    CHECK(marked >= plain + 25);
    // Only that column changes.
    CHECK(ink_count(b->canvas, whole) - plain_all ==
          ink_count(b->canvas, {col, 0, 1, kHeight}) - plain_col);
    // Restarts in a short series land on the point's own x.
    b->canvas.clear();
    const std::array<float, 5> five{1.0F, 1.0F, kNaN, 1.0F, 1.0F};
    const std::array<std::uint8_t, 1> bit2{0x04};
    restart_marks(b->canvas, {100, 100, 5, 10},
                  {.ring = five, .start = 0, .restarts = bit2});
    CHECK(ink_count(b->canvas, {102, 100, 1, 10}) == 8);
    CHECK(ink_count(b->canvas, {100, 100, 5, 10}) == 8);
  }
  // An empty chart says so instead of drawing a line.
  b->canvas.clear();
  line_chart(b->canvas, plot, {.ring = nan3, .start = 0, .restarts = {}}, {});
  CHECK(ink_count(b->canvas, {plot.x + 100, plot.y + 30, 140, 40}) > 0);

  // widen() centres a narrow range, floors at 0 on request, and leaves a
  // wide or invalid range alone.
  Range w = widen({.lo = 20.0F, .hi = 21.0F, .valid = true}, false, 2.0F);
  CHECK(w.valid && w.lo == 19.5F && w.hi == 21.5F);
  w = widen({.lo = 1.0F, .hi = 2.0F, .valid = true}, true, 5.0F);
  CHECK(w.lo == 0.0F && w.hi == 5.0F);
  w = widen({.lo = 400.0F, .hi = 900.0F, .valid = true}, false, 100.0F);
  CHECK(w.lo == 400.0F && w.hi == 900.0F);
  w = widen({.lo = -3.0F, .hi = -2.0F, .valid = true}, true, 2.0F);
  CHECK(w.lo == -3.5F && w.hi == -1.5F); // Negative data centres.
  CHECK(!widen({}, true, 5.0F).valid);

  // Mean draws one pixel per column on a slow ramp, and a NaN hour stays
  // blank.
  b->canvas.clear();
  std::array<float, 360> hour{};
  for (std::size_t i = 0; i < hour.size(); ++i)
    hour[i] = static_cast<float>(i);
  const Rect spark{10, 10, 100, 200};
  plot_series(b->canvas, spark, {.ring = hour, .start = 0, .restarts = {}},
              0.0F, 359.0F, Detail::Mean);
  bool one_per_column = true;
  for (int x = spark.x; x < spark.right(); ++x)
    one_per_column = one_per_column &&
                     ink_count(b->canvas, {x, spark.y, 1, spark.h}) >= 1 &&
                     ink_count(b->canvas, {x, spark.y, 1, spark.h}) <= 3;
  CHECK(one_per_column);
  b->canvas.clear();
  for (std::size_t i = 90; i < 180; ++i)
    hour[i] = kNaN;
  plot_series(b->canvas, spark, {.ring = hour, .start = 0, .restarts = {}},
              0.0F, 359.0F, Detail::Mean);
  CHECK(ink_count(b->canvas, {spark.x + 27, spark.y, 20, spark.h}) == 0);
  CHECK(ink_count(b->canvas, {spark.x, spark.y, 20, spark.h}) > 0);
}

void test_bands() {
  CHECK(pm25_band(0.0F).band == AirBand::Good);
  CHECK(pm25_band(9.0F).band == AirBand::Good);
  CHECK(pm25_band(9.09F).band == AirBand::Good); // Truncates to 9.0.
  CHECK(pm25_band(9.1F).band == AirBand::Moderate);
  CHECK(pm25_band(35.4F).band == AirBand::Moderate);
  CHECK(pm25_band(35.49F).band == AirBand::Moderate);
  CHECK(pm25_band(35.5F).band == AirBand::SensitiveGroups);
  CHECK(pm25_band(55.4F).band == AirBand::SensitiveGroups);
  CHECK(pm25_band(55.5F).band == AirBand::Unhealthy);
  CHECK(pm25_band(125.4F).band == AirBand::Unhealthy);
  CHECK(pm25_band(125.5F).band == AirBand::VeryUnhealthy);
  CHECK(pm25_band(225.4F).band == AirBand::VeryUnhealthy);
  CHECK(pm25_band(225.5F).band == AirBand::Hazardous);
  CHECK(pm25_band(1000.0F).band == AirBand::Hazardous);
  for (const auto &row : kPm25Bands)
    CHECK(text_width(fonts::label, row.word) + 16 < 200);
}

void test_quality_and_format() {
  Reading r = quality_for(Metric::Pm25, 12.0F, std::nullopt);
  CHECK(r.quality == Quality::Absent);
  r = quality_for(Metric::Pm25, 12.0F, 10U);
  CHECK(r.quality == Quality::Warming && r.warm_left_s == 20);
  r = quality_for(Metric::Co2, std::nullopt, 23U);
  CHECK(r.quality == Quality::Warming && r.warm_left_s == 1);
  r = quality_for(Metric::Co2, 612.4F, 24U);
  CHECK(r.quality == Quality::Valid && r.value == 612.4F);
  r = quality_for(Metric::Pm10, std::nullopt, 400U);
  CHECK(r.quality == Quality::Absent);
  r = quality_for(Metric::Temp, kNaN, 400U);
  CHECK(r.quality == Quality::Absent);

  std::array<char, 16> buf{};
  CHECK(format_reading(
            buf, Metric::Co2,
            {.value = 612.4F, .quality = Quality::Valid, .warm_left_s = 0}) ==
        "612");
  CHECK(format_reading(
            buf, Metric::Temp,
            {.value = -0.04F, .quality = Quality::Valid, .warm_left_s = 0}) ==
        "0.0");
  CHECK(format_reading(
            buf, Metric::Pm25,
            {.value = 12.0F, .quality = Quality::Warming, .warm_left_s = 3}) ==
        "--");
  std::array<char, 3> tiny{};
  CHECK(format_reading(
            tiny, Metric::Co2,
            {.value = 1234.0F, .quality = Quality::Valid, .warm_left_s = 0}) ==
        "12");
}

void test_navigation() {
  CHECK(navigate(0, Input::Up).page == kPageCount - 1);
  CHECK(navigate(kPageCount - 1, Input::Down).page == 0);
  CHECK(navigate(3, Input::Down).page == 4);
  CHECK(!navigate(3, Input::Down).full_refresh);
  const Nav p = navigate(4, Input::Press);
  CHECK(p.page == 4 && p.full_refresh);
  CHECK(navigate(5, Input::Exit).page == kDashboardPage);
  // Menu belongs to the settings popup, navigate keeps the page.
  CHECK(navigate(2, Input::Menu).page == 2);
  CHECK(navigate(kStatusPage, Input::Menu).page == kStatusPage);
  // The status page is one turn back from the dashboard.
  CHECK(navigate(kDashboardPage, Input::Up).page == kStatusPage);
  CHECK(navigate(200, Input::Down).page == 1);

  RefreshPolicy policy;
  CHECK(policy.next(false, false) == Refresh::Full);
  for (int i = 0; i < RefreshPolicy::kPartialsPerFull; ++i)
    CHECK(policy.next(false, false) == Refresh::Partial);
  CHECK(policy.next(false, false) == Refresh::Full);
  CHECK(policy.next(false, false) == Refresh::Partial);
  CHECK(policy.next(true, false) == Refresh::Full);
  CHECK(policy.next(false, true) == Refresh::Full);
}

void test_render_all_pages() {
  auto b = std::make_unique<Buffer>();
  auto h = std::make_unique<History>();
  Model empty{};
  for (std::uint8_t p = 0; p < kPageCount + 2; ++p) {
    render(b->canvas, p, empty, *h);
    // The header rule spans the width on every page.
    CHECK(ink_count(b->canvas, {0, 22, kWidth, 1}) == kWidth);
  }
  Model full{};
  for (std::size_t i = 0; i < kMetricCount; ++i)
    full.now[i] = {
        .value = 1234.5F, .quality = Quality::Valid, .warm_left_s = 0};
  full.status = {.utc_s = 1795000000,
                 .sd_ok = true,
                 .sd_free_bytes = 7'210'000'000ULL,
                 .ble = Link::Connected,
                 .wifi = Link::Waiting,
                 .samples = 4'000'000'000U,
                 .uptime_s = 4'000'000'000U,
                 .firmware = "opensensor-crowpanel-a-very-long-identity-string",
                 .thing = "OS-ABCDEF",
                 .sensor_serial = "1234567890ABCDEF1234567890ABCDEF",
                 .sensor_fw = "2.0",
                 .sensor_status = 0xFFFFFFFFU,
                 .sensor_present = true,
                 .pairing_passkey = std::nullopt,
                 .device_name = "OS-ABCDEF"};
  for (std::uint32_t t = 0; t < 3 * 3600; t += 10)
    h->push(t, all(static_cast<float>(t % 500)));
  for (std::uint8_t p = 0; p < kPageCount; ++p)
    render(b->canvas, p, full, *h);
  full.status.utc_s = -1;
  render(b->canvas, kStatusPage, full, *h);
}

// While a phone pairs every page shows the same pairing screen, the passkey
// drawn with six digits, and the page comes back once the passkey clears.
void test_pairing_screen() {
  auto b = std::make_unique<Buffer>();
  auto first = std::make_unique<Buffer>();
  auto h = std::make_unique<History>();
  Model m{};
  m.status.pairing_passkey = 42U;
  m.status.device_name = "OS-1A2B3C";
  render(first->canvas, kDashboardPage, m, *h);
  for (std::uint8_t p = 1; p < kPageCount; ++p) {
    render(b->canvas, p, m, *h);
    CHECK(b->bytes == first->bytes);
  }
  // The hero digits fill the middle band.
  CHECK(ink_count(first->canvas, {0, 126, kWidth, 64}) > 2000);
  // 000042 is wider than 42 would be alone.
  CHECK(text_width(fonts::hero, "000042") > 3 * text_width(fonts::hero, "42"));
  m.status.pairing_passkey = 999999U;
  m.status.device_name = {};
  render(b->canvas, kStatusPage, m, *h);
  m.status.pairing_passkey.reset();
  render(b->canvas, kStatusPage, m, *h);
  CHECK(b->bytes != first->bytes);
}

bool copy_ok(std::string_view s) {
  return s.find(':') == std::string_view::npos &&
         s.find(';') == std::string_view::npos &&
         s.find("—") == std::string_view::npos;
}

std::string_view value_of(Row r, const Settings &s, const PopupState &p,
                          const SettingsInfo &info, std::array<char, 24> &buf) {
  return row_value(buf, r, s, p, info);
}

void test_settings_blob() {
  const Settings d{};
  CHECK(refresh_seconds(d) == 10);
  CHECK(!sleep_after_seconds(d));
  CHECK(start_page(d) == kDashboardPage);
  CHECK(d.co2_asc && d.altitude_m == 0 && d.temp_offset_dc == 0);
  CHECK(d.theme == Theme::Light);

  const SettingsBlob db = encode(d);
  CHECK(db[0] == kSettingsVersion);
  CHECK(decode(db) == d);

  const Settings s{.refresh = RefreshEvery::M5,
                   .start_page = StartPage::LastViewed,
                   .last_page = kStatusPage,
                   .co2_asc = false,
                   .altitude_m = 2950,
                   .temp_offset_dc = -73,
                   .sleep_after = SleepAfter::H1,
                   .theme = Theme::Dark};
  const SettingsBlob b = encode(s);
  CHECK(b.size() == 11 && b[10] == 1);
  // Explicit little endian, independent of the host.
  CHECK(b[5] == (2950 & 0xFF) && b[6] == (2950 >> 8));
  CHECK(b[7] == 0xB7 && b[8] == 0xFF); // -73 as two's complement.
  CHECK(decode(b) == s);
  CHECK(refresh_seconds(s) == 300);
  CHECK(sleep_after_seconds(s) == 3600U);
  CHECK(start_page(s) == kStatusPage);
  CHECK(refresh_seconds({.refresh = RefreshEvery::S30}) == 30);
  CHECK(refresh_seconds({.refresh = RefreshEvery::M1}) == 60);
  CHECK(sleep_after_seconds({.sleep_after = SleepAfter::M10}) == 600U);
  CHECK(sleep_after_seconds({.sleep_after = SleepAfter::M30}) == 1800U);
  // Every temperature offset survives the round trip.
  for (int v = kTempOffsetMinDc; v <= kTempOffsetMaxDc; ++v) {
    Settings t{};
    t.temp_offset_dc = static_cast<std::int16_t>(v);
    CHECK(decode(encode(t)).temp_offset_dc == v);
  }

  // Empty, a bad version and a short blob.
  CHECK(decode({}) == d);
  SettingsBlob bad = b;
  bad[0] = 2;
  CHECK(decode(bad) == d);
  bad[0] = 0;
  CHECK(decode(bad) == d);
  const std::vector<std::uint8_t> shorter(b.begin(), b.begin() + 5);
  const Settings sh = decode(shorter);
  CHECK(sh.refresh == s.refresh && sh.start_page == s.start_page &&
        sh.last_page == s.last_page && sh.co2_asc == s.co2_asc);
  CHECK(sh.altitude_m == 0 && sh.temp_offset_dc == 0 &&
        sh.sleep_after == SleepAfter::Off);
  // One byte of a two byte field is not enough.
  const std::vector<std::uint8_t> half(b.begin(), b.begin() + 6);
  CHECK(decode(half).altitude_m == 0);
  // A blob saved before the theme existed, ten bytes, keeps every older field
  // and gives Light.
  const std::vector<std::uint8_t> old(b.begin(), b.begin() + 10);
  Settings want_old = s;
  want_old.theme = Theme::Light;
  CHECK(decode(old) == want_old);
  // A longer blob reads its first eleven bytes.
  std::vector<std::uint8_t> longer(b.begin(), b.end());
  longer.push_back(0xEE);
  CHECK(decode(longer) == s);

  // Each field out of range falls back on its own, the rest still decode.
  const auto out_of_range = [&](std::size_t at, std::uint8_t v) {
    SettingsBlob x = b;
    x[at] = v;
    return decode(x);
  };
  CHECK(out_of_range(1, 4).refresh == RefreshEvery::S10);
  CHECK(out_of_range(1, 4).sleep_after == SleepAfter::H1);
  CHECK(out_of_range(2, 2).start_page == StartPage::Dashboard);
  CHECK(out_of_range(3, kPageCount).last_page == kDashboardPage);
  CHECK(out_of_range(4, 2).co2_asc);
  CHECK(out_of_range(9, 4).sleep_after == SleepAfter::Off);
  CHECK(out_of_range(9, 4).altitude_m == 2950);
  CHECK(out_of_range(10, 2).theme == Theme::Light);
  CHECK(out_of_range(10, 0xFF).theme == Theme::Light);
  CHECK(out_of_range(10, 2).sleep_after == SleepAfter::H1);
  SettingsBlob hi = b;
  hi[5] = (3001 & 0xFF);
  hi[6] = (3001 >> 8);
  CHECK(decode(hi).altitude_m == 0 && decode(hi).temp_offset_dc == -73);
  SettingsBlob cold = b;
  cold[7] = 0x9B; // -101
  cold[8] = 0xFF;
  CHECK(decode(cold).temp_offset_dc == 0);
  SettingsBlob warm = b;
  warm[7] = 51;
  warm[8] = 0;
  CHECK(decode(warm).temp_offset_dc == 0);
  // A corrupt start page cannot open an unknown page.
  Settings lost{};
  lost.start_page = StartPage::LastViewed;
  lost.last_page = 200;
  CHECK(start_page(lost) == kDashboardPage);
}

constexpr SettingsInfo kAllThere{.wifi_configured = true,
                                 .wifi_on = true,
                                 .sensor_present = true,
                                 .fan_cleaning = false};

PopupState opened() {
  PopupState p{};
  Settings s{};
  CHECK(popup_input(p, s, kAllThere, Input::Menu) == Effect::Opened);
  CHECK(p.open && p.selected == Row::Refresh && !p.editing);
  return p;
}

PopupState at(Row r) {
  PopupState p = opened();
  p.selected = r;
  return p;
}

void test_popup_rows() {
  // A closed popup ignores everything but Menu.
  {
    PopupState p{};
    Settings s{};
    for (const Input in : {Input::Up, Input::Down, Input::Press, Input::Exit})
      CHECK(popup_input(p, s, kAllThere, in) == Effect::None && !p.open);
  }
  // Selection moves down with Down and wraps both ways.
  {
    PopupState p = opened();
    Settings s{};
    CHECK(popup_input(p, s, kAllThere, Input::Down) == Effect::None);
    CHECK(p.selected == Row::StartPage);
    p.selected = Row::Refresh;
    CHECK(popup_input(p, s, kAllThere, Input::Up) == Effect::None);
    CHECK(p.selected == Row::SleepAfter);
    CHECK(popup_input(p, s, kAllThere, Input::Down) == Effect::None);
    CHECK(p.selected == Row::Refresh);
    for (int i = 0; i < kRowCount; ++i)
      static_cast<void>(popup_input(p, s, kAllThere, Input::Down));
    CHECK(p.selected == Row::Refresh);
    CHECK(s == Settings{});
  }
  // The display rows sit together, Theme right after Start on.
  {
    constexpr std::array<Row, kRowCount> kOrder{
        Row::Refresh,  Row::StartPage,   Row::Theme,
        Row::Wifi,     Row::FanCleaning, Row::Co2Asc,
        Row::Altitude, Row::TempOffset,  Row::SleepAfter};
    PopupState p = opened();
    Settings s{};
    for (const Row want : kOrder) {
      CHECK(p.selected == want);
      static_cast<void>(popup_input(p, s, kAllThere, Input::Down));
    }
    CHECK(p.selected == Row::Refresh);
    CHECK(row_label(Row::Theme) == "Theme");
  }
  // Theme toggles Light and Dark.
  {
    PopupState p = at(Row::Theme);
    Settings s{};
    std::array<char, 24> buf{};
    CHECK(value_of(Row::Theme, s, p, kAllThere, buf) == "Light");
    CHECK(popup_input(p, s, kAllThere, Input::Press) == Effect::Changed);
    CHECK(s.theme == Theme::Dark);
    CHECK(value_of(Row::Theme, s, p, kAllThere, buf) == "Dark");
    CHECK(popup_input(p, s, kAllThere, Input::Press) == Effect::Changed);
    CHECK(s.theme == Theme::Light);
    CHECK(p.open && !p.editing && p.selected == Row::Theme);
  }
  // Refresh cycles 10 s, 30 s, 1 min, 5 min and back.
  {
    PopupState p = at(Row::Refresh);
    Settings s{};
    std::array<char, 24> buf{};
    CHECK(value_of(Row::Refresh, s, p, kAllThere, buf) == "10 s");
    CHECK(popup_input(p, s, kAllThere, Input::Press) == Effect::Changed);
    CHECK(s.refresh == RefreshEvery::S30);
    CHECK(value_of(Row::Refresh, s, p, kAllThere, buf) == "30 s");
    static_cast<void>(popup_input(p, s, kAllThere, Input::Press));
    CHECK(value_of(Row::Refresh, s, p, kAllThere, buf) == "1 min");
    static_cast<void>(popup_input(p, s, kAllThere, Input::Press));
    CHECK(value_of(Row::Refresh, s, p, kAllThere, buf) == "5 min");
    static_cast<void>(popup_input(p, s, kAllThere, Input::Press));
    CHECK(s.refresh == RefreshEvery::S10);
  }
  // Start page toggles.
  {
    PopupState p = at(Row::StartPage);
    Settings s{};
    std::array<char, 24> buf{};
    CHECK(value_of(Row::StartPage, s, p, kAllThere, buf) == "Dashboard");
    CHECK(popup_input(p, s, kAllThere, Input::Press) == Effect::Changed);
    CHECK(s.start_page == StartPage::LastViewed);
    CHECK(value_of(Row::StartPage, s, p, kAllThere, buf) == "Last page");
    CHECK(popup_input(p, s, kAllThere, Input::Press) == Effect::Changed);
    CHECK(s.start_page == StartPage::Dashboard);
  }
  // Wi-Fi is read only status, Press never changes anything.
  {
    std::array<char, 24> buf{};
    for (const bool configured : {false, true}) {
      for (const bool on : {false, true}) {
        PopupState p = at(Row::Wifi);
        Settings s{};
        const SettingsInfo info{.wifi_configured = configured,
                                .wifi_on = on,
                                .sensor_present = true,
                                .fan_cleaning = false};
        CHECK(popup_input(p, s, info, Input::Press) == Effect::None);
        CHECK(s == Settings{} && p.open && !p.editing);
        const std::string_view v = value_of(Row::Wifi, s, p, info, buf);
        CHECK(v == (!configured ? "Not set up" : (on ? "On" : "Off")));
      }
    }
  }
  // Fan cleaning needs a sensor that is not cleaning already.
  {
    std::array<char, 24> buf{};
    PopupState p = at(Row::FanCleaning);
    Settings s{};
    CHECK(popup_input(p, s, kAllThere, Input::Press) == Effect::FanClean);
    CHECK(value_of(Row::FanCleaning, s, p, kAllThere, buf) == "Run now");
    SettingsInfo running = kAllThere;
    running.fan_cleaning = true;
    CHECK(popup_input(p, s, running, Input::Press) == Effect::None);
    CHECK(value_of(Row::FanCleaning, s, p, running, buf) == "Running");
    SettingsInfo none = kAllThere;
    none.sensor_present = false;
    CHECK(popup_input(p, s, none, Input::Press) == Effect::None);
    CHECK(value_of(Row::FanCleaning, s, p, none, buf) == "No sensor");
    CHECK(s == Settings{});
  }
  // CO2 self calibration toggles and says off is for the lab.
  {
    std::array<char, 24> buf{};
    PopupState p = at(Row::Co2Asc);
    Settings s{};
    CHECK(value_of(Row::Co2Asc, s, p, kAllThere, buf) == "On");
    CHECK(popup_input(p, s, kAllThere, Input::Press) == Effect::Changed);
    CHECK(!s.co2_asc);
    CHECK(value_of(Row::Co2Asc, s, p, kAllThere, buf) == "Off (lab only)");
    CHECK(popup_input(p, s, kAllThere, Input::Press) == Effect::Changed);
    CHECK(s.co2_asc);
  }
  // Sleep after cycles Off, 10 min, 30 min, 1 h and back.
  {
    std::array<char, 24> buf{};
    PopupState p = at(Row::SleepAfter);
    Settings s{};
    CHECK(value_of(Row::SleepAfter, s, p, kAllThere, buf) == "Off");
    for (const std::string_view want : {"10 min", "30 min", "1 h", "Off"}) {
      CHECK(popup_input(p, s, kAllThere, Input::Press) == Effect::Changed);
      CHECK(value_of(Row::SleepAfter, s, p, kAllThere, buf) == want);
    }
  }
  // Menu and Exit close it.
  for (const Input in : {Input::Menu, Input::Exit}) {
    PopupState p = opened();
    Settings s{};
    CHECK(popup_input(p, s, kAllThere, in) == Effect::Closed && !p.open);
  }
  // Every label and value obeys the copy rule.
  {
    std::array<char, 24> buf{};
    Settings s{};
    s.co2_asc = false;
    s.temp_offset_dc = -15;
    PopupState p = opened();
    for (std::uint8_t i = 0; i < kRowCount; ++i) {
      const auto r = static_cast<Row>(i);
      CHECK(!row_label(r).empty() && copy_ok(row_label(r)));
      CHECK(!value_of(r, s, p, kAllThere, buf).empty());
      CHECK(copy_ok(value_of(r, s, p, kAllThere, buf)));
    }
  }
}

void test_popup_numeric() {
  std::array<char, 24> buf{};
  // Altitude, Up raises by 50 and clamps at 3000, Down lowers and clamps at 0.
  {
    PopupState p = at(Row::Altitude);
    Settings s{};
    CHECK(value_of(Row::Altitude, s, p, kAllThere, buf) == "0 m");
    CHECK(popup_input(p, s, kAllThere, Input::Press) == Effect::None);
    CHECK(p.editing && p.edit_value == 0);
    CHECK(popup_input(p, s, kAllThere, Input::Down) == Effect::None);
    CHECK(p.edit_value == 0 && p.selected == Row::Altitude);
    for (int i = 0; i < 7; ++i)
      CHECK(popup_input(p, s, kAllThere, Input::Up) == Effect::None);
    CHECK(p.edit_value == 350 && s.altitude_m == 0);
    CHECK(value_of(Row::Altitude, s, p, kAllThere, buf) == "350 m");
    CHECK(popup_input(p, s, kAllThere, Input::Press) == Effect::Changed);
    CHECK(!p.editing && s.altitude_m == 350);
    // Pressing again without a change is not a change.
    CHECK(popup_input(p, s, kAllThere, Input::Press) == Effect::None);
    CHECK(popup_input(p, s, kAllThere, Input::Press) == Effect::None);
    CHECK(s.altitude_m == 350);
    CHECK(popup_input(p, s, kAllThere, Input::Press) == Effect::None);
    for (int i = 0; i < 100; ++i)
      static_cast<void>(popup_input(p, s, kAllThere, Input::Up));
    CHECK(p.edit_value == kAltitudeMaxM);
    // Exit cancels, the popup stays open on the row.
    CHECK(popup_input(p, s, kAllThere, Input::Exit) == Effect::None);
    CHECK(p.open && !p.editing && s.altitude_m == 350);
    CHECK(value_of(Row::Altitude, s, p, kAllThere, buf) == "350 m");
    // Menu while editing drops the edit and closes.
    static_cast<void>(popup_input(p, s, kAllThere, Input::Press));
    static_cast<void>(popup_input(p, s, kAllThere, Input::Down));
    CHECK(popup_input(p, s, kAllThere, Input::Menu) == Effect::Closed);
    CHECK(!p.open && !p.editing && s.altitude_m == 350);
  }
  // Temperature offset steps 0.1 C between -10.0 and +5.0 C.
  {
    PopupState p = at(Row::TempOffset);
    Settings s{};
    CHECK(value_of(Row::TempOffset, s, p, kAllThere, buf) == "0.0 °C");
    static_cast<void>(popup_input(p, s, kAllThere, Input::Press));
    for (int i = 0; i < 15; ++i)
      static_cast<void>(popup_input(p, s, kAllThere, Input::Down));
    CHECK(value_of(Row::TempOffset, s, p, kAllThere, buf) == "-1.5 °C");
    CHECK(popup_input(p, s, kAllThere, Input::Press) == Effect::Changed);
    CHECK(s.temp_offset_dc == -15);
    static_cast<void>(popup_input(p, s, kAllThere, Input::Press));
    for (int i = 0; i < 500; ++i)
      static_cast<void>(popup_input(p, s, kAllThere, Input::Down));
    CHECK(p.edit_value == kTempOffsetMinDc);
    CHECK(value_of(Row::TempOffset, s, p, kAllThere, buf) == "-10.0 °C");
    for (int i = 0; i < 500; ++i)
      static_cast<void>(popup_input(p, s, kAllThere, Input::Up));
    CHECK(p.edit_value == kTempOffsetMaxDc);
    CHECK(value_of(Row::TempOffset, s, p, kAllThere, buf) == "+5.0 °C");
    CHECK(popup_input(p, s, kAllThere, Input::Press) == Effect::Changed);
    CHECK(s.temp_offset_dc == 50);
    s.temp_offset_dc = 20;
    p.editing = false;
    CHECK(value_of(Row::TempOffset, s, p, kAllThere, buf) == "+2.0 °C");
    s.temp_offset_dc = -5;
    CHECK(value_of(Row::TempOffset, s, p, kAllThere, buf) == "-0.5 °C");
  }
}

void test_ui_handle() {
  const SettingsInfo info = kAllThere;
  // Menu opens the popup from every page and keeps the page under it.
  for (std::uint8_t page = 0; page < kPageCount; ++page) {
    UiState ui{.page = page};
    Settings s{};
    const Outcome o = handle(ui, s, info, Input::Menu);
    CHECK(o.effect == Effect::Opened && ui.popup.open && ui.page == page);
    CHECK(!o.page_changed && !o.full_refresh);
    // Turning the knob now moves the selection, not the page.
    const Outcome d = handle(ui, s, info, Input::Down);
    CHECK(ui.page == page && !d.page_changed);
    CHECK(ui.popup.selected == Row::StartPage);
    const Outcome c = handle(ui, s, info, Input::Exit);
    CHECK(c.effect == Effect::Closed && c.full_refresh);
    CHECK(!ui.popup.open && ui.page == page);
    static_cast<void>(handle(ui, s, info, Input::Menu));
    CHECK(handle(ui, s, info, Input::Menu).effect == Effect::Closed);
    CHECK(s == Settings{});
  }
  // With the popup closed the pages behave as navigate() does.
  for (std::uint8_t page = 0; page < kPageCount; ++page) {
    for (const Input in : {Input::Up, Input::Down, Input::Press, Input::Exit}) {
      UiState ui{.page = page};
      Settings s{};
      const Nav want = navigate(page, in);
      const Outcome o = handle(ui, s, info, in);
      CHECK(ui.page == want.page && o.full_refresh == want.full_refresh);
      CHECK(o.page_changed == (want.page != page));
      CHECK(o.effect == Effect::None && !ui.popup.open);
    }
  }
  // Effects pass through handle().
  {
    UiState ui{};
    Settings s{};
    static_cast<void>(handle(ui, s, info, Input::Menu));
    ui.popup.selected = Row::FanCleaning;
    CHECK(handle(ui, s, info, Input::Press).effect == Effect::FanClean);
    ui.popup.selected = Row::Refresh;
    const Outcome r = handle(ui, s, info, Input::Press);
    CHECK(r.effect == Effect::Changed && !r.full_refresh);
    CHECK(s.refresh == RefreshEvery::S30);
  }
  // A theme change asks for a full refresh both ways, any other change and
  // moving the selection do not.
  {
    UiState ui{};
    Settings s{};
    static_cast<void>(handle(ui, s, info, Input::Menu));
    ui.popup.selected = Row::StartPage;
    const Outcome d = handle(ui, s, info, Input::Down);
    CHECK(ui.popup.selected == Row::Theme && !d.full_refresh);
    const Outcome dark = handle(ui, s, info, Input::Press);
    CHECK(dark.effect == Effect::Changed && dark.full_refresh);
    CHECK(s.theme == Theme::Dark && !dark.page_changed && ui.popup.open);
    const Outcome light = handle(ui, s, info, Input::Press);
    CHECK(light.effect == Effect::Changed && light.full_refresh);
    CHECK(s.theme == Theme::Light);
    ui.popup.selected = Row::SleepAfter;
    const Outcome sleep = handle(ui, s, info, Input::Press);
    CHECK(sleep.effect == Effect::Changed && !sleep.full_refresh);
    // The RefreshPolicy turns that request into a full refresh.
    RefreshPolicy policy;
    static_cast<void>(policy.next(false, false));
    CHECK(policy.next(false, false) == Refresh::Partial);
    CHECK(policy.next(dark.page_changed, dark.full_refresh) == Refresh::Full);
  }
  // Last viewed tracks the page, Dashboard leaves last_page alone.
  {
    UiState ui{};
    Settings s{};
    CHECK(handle(ui, s, info, Input::Down).effect == Effect::None);
    CHECK(s.last_page == kDashboardPage);
    static_cast<void>(handle(ui, s, info, Input::Menu));
    ui.popup.selected = Row::StartPage;
    CHECK(handle(ui, s, info, Input::Press).effect == Effect::Changed);
    CHECK(s.start_page == StartPage::LastViewed && s.last_page == 1);
    static_cast<void>(handle(ui, s, info, Input::Exit));
    const Outcome o = handle(ui, s, info, Input::Down);
    CHECK(o.page_changed && o.effect == Effect::Changed && s.last_page == 2);
    CHECK(handle(ui, s, info, Input::Press).effect == Effect::None);
    CHECK(start_page(s) == 2);
  }
  // Any key wakes a sleeping screen and does nothing else.
  for (const Input in :
       {Input::Up, Input::Down, Input::Press, Input::Menu, Input::Exit}) {
    UiState ui{.page = 3};
    Settings s{};
    static_cast<void>(handle(ui, s, info, Input::Menu));
    ui.popup.selected = Row::Altitude;
    static_cast<void>(handle(ui, s, info, Input::Press));
    CHECK(ui.popup.editing);
    fall_asleep(ui);
    CHECK(ui.asleep && !ui.popup.open && !ui.popup.editing);
    const Outcome o = handle(ui, s, info, in);
    CHECK(!ui.asleep && o.full_refresh && o.effect == Effect::None);
    CHECK(ui.page == 3 && !ui.popup.open && s == Settings{});
  }
}

Model sample_model() {
  Model m{};
  for (std::size_t i = 0; i < kMetricCount; ++i)
    m.now[i] = {.value = 888.0F, .quality = Quality::Valid, .warm_left_s = 0};
  m.status.utc_s = 1795437120;
  m.status.thing = "OS-7F3A2C";
  m.status.device_name = "OS-7F3A2C";
  m.status.sensor_present = true;
  return m;
}

void test_render_settings_and_sleep() {
  auto b = std::make_unique<Buffer>();
  auto page = std::make_unique<Buffer>();
  auto h = std::make_unique<History>();
  Model m = sample_model();
  for (std::uint32_t t = 0; t < 3600; t += 10)
    h->push(t, all(static_cast<float>(t % 300)));
  Settings s{};
  for (std::uint8_t p = 0; p < kPageCount; ++p) {
    UiState ui{.page = p};
    render_screen(page->canvas, ui, s, kAllThere, m, *h);
    static_cast<void>(handle(ui, s, kAllThere, Input::Menu));
    render_screen(b->canvas, ui, s, kAllThere, m, *h);
    CHECK(b->bytes != page->bytes);
    // Outside the popup frame the page is untouched.
    bool same = true;
    for (int y = 0; y < 20; ++y)
      for (int x = 0; x < kWidth; ++x)
        same = same && b->canvas.get(x, y) == page->canvas.get(x, y);
    CHECK(same);
    // Every row selected and every numeric row in edit draws.
    for (std::uint8_t r = 0; r < kRowCount; ++r) {
      ui.popup.selected = static_cast<Row>(r);
      ui.popup.editing = false;
      render_screen(b->canvas, ui, s, kAllThere, m, *h);
      if (ui.popup.selected == Row::Altitude ||
          ui.popup.selected == Row::TempOffset) {
        static_cast<void>(handle(ui, s, kAllThere, Input::Press));
        render_screen(b->canvas, ui, s, {}, m, *h);
      }
    }
  }
  // Asleep, no reading and no clock, only the names.
  {
    UiState ui{.page = 1};
    fall_asleep(ui);
    render_screen(b->canvas, ui, s, kAllThere, m, *h);
    auto empty = std::make_unique<Buffer>();
    Model other = sample_model();
    for (auto &r : other.now)
      r.value = 12.0F;
    other.status.utc_s = 0;
    render_screen(empty->canvas, ui, s, kAllThere, other, *h);
    CHECK(b->bytes == empty->bytes);
    // Some ink, and nothing in the header band.
    CHECK(ink_count(b->canvas, {0, 0, kWidth, kHeight}) > 500);
    CHECK(ink_count(b->canvas, {0, 0, kWidth, 40}) == 0);
    // Without names it still draws.
    other.status.thing = {};
    other.status.device_name = {};
    render_asleep(empty->canvas, other);
    CHECK(empty->bytes != b->bytes);
  }
  // A pairing passkey wins over sleep and over the popup.
  {
    Model pm = sample_model();
    pm.status.pairing_passkey = 42U;
    auto want = std::make_unique<Buffer>();
    render(want->canvas, kDashboardPage, pm, *h);
    UiState ui{};
    fall_asleep(ui);
    render_screen(b->canvas, ui, s, kAllThere, pm, *h);
    CHECK(b->bytes == want->bytes);
    ui.asleep = false;
    static_cast<void>(handle(ui, s, kAllThere, Input::Menu));
    render_screen(b->canvas, ui, s, kAllThere, pm, *h);
    CHECK(b->bytes == want->bytes);
  }
}

// True when every bit of `dark` is the opposite of the same bit in `light`.
bool is_inverse(const Buffer &light, const Buffer &dark) {
  for (std::size_t i = 0; i < kFrameBytes; ++i)
    if (static_cast<std::uint8_t>(~light.bytes[i]) != dark.bytes[i])
      return false;
  return true;
}

// The pixels where `dark` is not the inverse of `light`, as a bounding box.
// Empty (w 0) when the frames are exact inverses.
Rect inverse_mismatch(const Buffer &light, const Buffer &dark) {
  int x0 = kWidth;
  int y0 = kHeight;
  int x1 = -1;
  int y1 = -1;
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x)
      if (light.canvas.get(x, y) == dark.canvas.get(x, y)) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
      }
  if (x1 < 0)
    return {};
  return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

// Dark is the exact bitwise inverse of Light for every page, on the pairing
// screen and with the popup open, where only the Theme row's own value text
// differs. The asleep screen is the same light frame in both themes.
void test_theme() {
  auto light = std::make_unique<Buffer>();
  auto dark = std::make_unique<Buffer>();
  auto h = std::make_unique<History>();
  const Model m = sample_model();
  for (std::uint32_t t = 0; t < 3600; t += 10)
    h->push(t, all(static_cast<float>(t % 300)));
  Settings ls{};
  Settings ds{};
  ds.theme = Theme::Dark;
  for (std::uint8_t p = 0; p < kPageCount; ++p) {
    UiState ui{.page = p};
    render_screen(light->canvas, ui, ls, kAllThere, m, *h);
    render_screen(dark->canvas, ui, ds, kAllThere, m, *h);
    CHECK(is_inverse(*light, *dark));
    CHECK(inverse_mismatch(*light, *dark).w == 0);
    static_cast<void>(handle(ui, ls, kAllThere, Input::Menu));
    for (std::uint8_t r = 0; r < kRowCount; ++r) {
      ui.popup.selected = static_cast<Row>(r);
      render_screen(light->canvas, ui, ls, kAllThere, m, *h);
      render_screen(dark->canvas, ui, ds, kAllThere, m, *h);
      // "Light" against "Dark" in the right half of one row, nothing else.
      const Rect diff = inverse_mismatch(*light, *dark);
      CHECK(diff.w > 0 && diff.h > 0 && diff.h < 21);
      CHECK(diff.x > kWidth / 2);
    }
  }
  // Pairing follows the theme.
  {
    Model pm = sample_model();
    pm.status.pairing_passkey = 123456U;
    UiState ui{};
    render_screen(light->canvas, ui, ls, kAllThere, pm, *h);
    render_screen(dark->canvas, ui, ds, kAllThere, pm, *h);
    CHECK(is_inverse(*light, *dark));
    // A pairing phone over a sleeping screen still shows the dark passkey.
    fall_asleep(ui);
    render_screen(dark->canvas, ui, ds, kAllThere, pm, *h);
    CHECK(is_inverse(*light, *dark));
  }
  // Asleep stays light, mostly white glass whatever the theme.
  {
    UiState ui{.page = 2};
    fall_asleep(ui);
    render_screen(light->canvas, ui, ls, kAllThere, m, *h);
    render_screen(dark->canvas, ui, ds, kAllThere, m, *h);
    CHECK(light->bytes == dark->bytes);
    CHECK(ink_count(dark->canvas, {0, 0, kWidth, kHeight}) <
          kWidth * kHeight / 4);
  }
}

void test_phone_keys() {
  // Every key the board declares takes one good value and sets the draft.
  for (const auto key : kPhoneKeys) {
    const std::string_view good =
        key == "panel.theme"                                 ? "dark"
        : key == "panel.start_page"                          ? "last_viewed"
        : key == "panel.refresh_s"                           ? "60"
        : key == "panel.sleep_s"                             ? "600"
        : key == "sen6x.co2_asc" || key == "sen6x.fan_clean" ? "true"
                                                             : "0";
    PhoneDraft d;
    CHECK(set_phone_key(d, key, good).has_value());
    CHECK(!(d == PhoneDraft{}));
  }

  PhoneDraft d;
  CHECK(set_phone_key(d, "panel.theme", "dark").has_value());
  CHECK(set_phone_key(d, "panel.refresh_s", "300").has_value());
  CHECK(set_phone_key(d, "panel.start_page", "last_viewed").has_value());
  CHECK(set_phone_key(d, "panel.sleep_s", "3600").has_value());
  CHECK(set_phone_key(d, "sen6x.co2_asc", "false").has_value());
  CHECK(set_phone_key(d, "sen6x.altitude_m", "3000").has_value());
  CHECK(set_phone_key(d, "sen6x.temp_offset_dc", "-100").has_value());
  CHECK(!d.fan_clean);
  CHECK(set_phone_key(d, "sen6x.fan_clean", "0").has_value());
  CHECK(!d.fan_clean);
  CHECK(set_phone_key(d, "sen6x.fan_clean", "1").has_value());
  CHECK(d.fan_clean);
  // A later 0 does not cancel a cleaning already asked for.
  CHECK(set_phone_key(d, "sen6x.fan_clean", "false").has_value());
  CHECK(d.fan_clean);

  // A refusal leaves the draft as it was.
  const PhoneDraft before = d;
  const auto refused = [&](std::string_view key, std::string_view value,
                           PhoneKeyError want) {
    const auto r = set_phone_key(d, key, value);
    CHECK(!r.has_value() && r.error() == want);
    CHECK(d == before);
  };
  refused("panel.theme", "Dark", PhoneKeyError::Value);
  refused("panel.theme", "", PhoneKeyError::Value);
  refused("panel.refresh_s", "20", PhoneKeyError::Value);
  refused("panel.refresh_s", "010", PhoneKeyError::Value);
  refused("panel.start_page", "settings", PhoneKeyError::Value);
  refused("panel.sleep_s", "off", PhoneKeyError::Value);
  refused("sen6x.co2_asc", "yes", PhoneKeyError::Value);
  refused("sen6x.co2_asc", "TRUE", PhoneKeyError::Value);
  refused("sen6x.altitude_m", "3001", PhoneKeyError::Value);
  refused("sen6x.altitude_m", "-1", PhoneKeyError::Value);
  refused("sen6x.altitude_m", "1e3", PhoneKeyError::Value);
  refused("sen6x.altitude_m", "", PhoneKeyError::Value);
  refused("sen6x.altitude_m", "-", PhoneKeyError::Value);
  refused("sen6x.altitude_m", "123456", PhoneKeyError::Value);
  refused("sen6x.temp_offset_dc", "51", PhoneKeyError::Value);
  refused("sen6x.temp_offset_dc", "-101", PhoneKeyError::Value);
  refused("sen6x.temp_offset_dc", "+5", PhoneKeyError::Value);
  refused("sen6x.fan_clean", "2", PhoneKeyError::Value);
  refused("panel.contrast", "1", PhoneKeyError::Unknown);
  refused("sen6x", "1", PhoneKeyError::Unknown);
  refused("", "", PhoneKeyError::Unknown);

  Settings s;
  apply_phone_draft(s, PhoneDraft{});
  CHECK(s == Settings{});
  apply_phone_draft(s, d);
  CHECK(s.theme == Theme::Dark);
  CHECK(s.refresh == RefreshEvery::M5);
  CHECK(s.start_page == StartPage::LastViewed);
  CHECK(s.sleep_after == SleepAfter::H1);
  CHECK(!s.co2_asc);
  CHECK(s.altitude_m == 3000);
  CHECK(s.temp_offset_dc == -100);
  // The draft's settings survive the NVS blob.
  CHECK(decode(encode(s)) == s);

  // Two requests the UI task has not taken yet merge into one.
  PhoneDraft first;
  CHECK(set_phone_key(first, "panel.theme", "dark").has_value());
  CHECK(set_phone_key(first, "sen6x.altitude_m", "100").has_value());
  CHECK(set_phone_key(first, "sen6x.fan_clean", "1").has_value());
  PhoneDraft second;
  CHECK(set_phone_key(second, "sen6x.altitude_m", "250").has_value());
  CHECK(set_phone_key(second, "sen6x.co2_asc", "0").has_value());
  merge_phone_draft(first, second);
  CHECK(first.theme == Theme::Dark);
  CHECK(first.altitude_m == 250);
  CHECK(first.co2_asc == false);
  CHECK(first.fan_clean);
  CHECK(!first.refresh && !first.start_page && !first.sleep_after &&
        !first.temp_offset_dc);
  PhoneDraft none;
  merge_phone_draft(none, PhoneDraft{});
  CHECK(none == PhoneDraft{});

  std::array<char, 256> page{};
  const std::size_t n = phone_page(page, Settings{});
  CHECK(std::string_view(page.data(), n) ==
        "{\"panel\":{\"theme\":\"light\",\"refresh_s\":\"10\","
        "\"start_page\":\"dashboard\",\"sleep_s\":\"0\"},"
        "\"sen6x\":{\"co2_asc\":true,\"altitude_m\":0,"
        "\"temp_offset_dc\":0}}");
  const std::size_t m = phone_page(page, s);
  CHECK(std::string_view(page.data(), m) ==
        "{\"panel\":{\"theme\":\"dark\",\"refresh_s\":\"300\","
        "\"start_page\":\"last_viewed\",\"sleep_s\":\"3600\"},"
        "\"sen6x\":{\"co2_asc\":false,\"altitude_m\":3000,"
        "\"temp_offset_dc\":-100}}");
  // A buffer one byte short gives 0 and an empty string.
  std::array<char, 256> small{};
  CHECK(phone_page(std::span<char>(small.data(), m), s) == 0);
  CHECK(small[0] == '\0');
  CHECK(phone_page(std::span<char>(small.data(), m + 1), s) == m);
  CHECK(phone_page(std::span<char>{}, s) == 0);
}

namespace order = opensensor::crowpanel::restore_order;

constexpr order::BootKey ring_boot(int age, std::uint64_t key = 0,
                                   std::optional<order::UsedRange> used = {}) {
  return {.ring_age = age, .utc_key = key, .used = used};
}

constexpr order::BootKey dated_boot(std::uint64_t key,
                                    std::optional<order::UsedRange> used = {}) {
  return {.ring_age = -1, .utc_key = key, .used = used};
}

constexpr order::UsedRange used(std::int64_t min, std::int64_t max) {
  return {.min = min, .max = max};
}

struct Ordered {
  std::vector<int> boots;
  bool cut = false;
  std::size_t fallback = 0;
};

Ordered run_order(std::span<const order::BootKey> boots,
                  std::size_t capacity = 32) {
  order::Scratch scratch;
  std::array<std::uint8_t, 32> out{};
  const auto result =
      order::order(boots, std::span{out}.first(capacity), scratch);
  Ordered ordered{.boots = {}, .cut = result.cut, .fallback = result.fallback};
  for (std::size_t k = 0; k < result.count; ++k)
    ordered.boots.push_back(out[k]);
  return ordered;
}

bool same(const Ordered &got, std::vector<int> boots, bool cut,
          std::size_t fallback) {
  return got.boots == boots && got.cut == cut && got.fallback == fallback;
}

// The order works in a constant expression, so it needs no runtime state.
constexpr std::size_t constant_order_count() {
  const std::array boots{ring_boot(0), ring_boot(1),
                         dated_boot(202611171000, used(10, 20))};
  order::Scratch scratch;
  std::array<std::uint8_t, 4> out{};
  return order::order(boots, out, scratch).count;
}
static_assert(constant_order_count() == 0);

void test_restore_order() {
  using order::Relation;
  // by_used. At or below the other's smallest is older, overlap is unknown.
  CHECK(order::by_used(dated_boot(1, used(10, 20)),
                       dated_boot(2, used(20, 30))) == Relation::Older);
  CHECK(order::by_used(dated_boot(1, used(30, 40)),
                       dated_boot(2, used(20, 30))) == Relation::Newer);
  CHECK(order::by_used(dated_boot(1, used(10, 25)),
                       dated_boot(2, used(20, 30))) == Relation::Unknown);
  CHECK(order::by_used(dated_boot(1, used(10, 10)),
                       dated_boot(2, used(10, 10))) == Relation::Unknown);
  CHECK(order::by_used(dated_boot(1), dated_boot(2, used(20, 30))) ==
        Relation::Unknown);

  // Ring only, ring order.
  {
    const std::array boots{ring_boot(0), ring_boot(2), ring_boot(1)};
    CHECK(same(run_order(boots), {1, 2, 0}, false, 0));
  }
  // Dated only, UTC order.
  {
    const std::array boots{dated_boot(202611170900), dated_boot(202611171100),
                           dated_boot(202611171000)};
    CHECK(same(run_order(boots), {1, 2, 0}, false, 0));
  }
  // Mixed, ring boots with dated files prove every step. Used bytes that
  // would say otherwise are never asked.
  {
    std::array boots{ring_boot(1, 202611171400), ring_boot(0, 202611171100),
                     dated_boot(202611171300), dated_boot(202611171000)};
    CHECK(same(run_order(boots), {0, 2, 1, 3}, false, 0));
    boots[2].used = used(1, 2);
    boots[0].used = used(100, 200);
    CHECK(same(run_order(boots), {0, 2, 1, 3}, false, 0));
  }
  // The hardware case. The newest ring boot never had UTC and the dated
  // boots never entered the ring, so the proofs place nothing. Used bytes
  // interleave them, R1 D1 D0 R0 from oldest.
  {
    std::array boots{ring_boot(1, 0, used(5000, 6000)),
                     ring_boot(0, 0, used(1000, 2000)),
                     dated_boot(202611171200, used(3000, 4000)),
                     dated_boot(202611171100, used(2000, 2500))};
    CHECK(same(run_order(boots), {0, 2, 3, 1}, false, 3));
    for (auto &boot : boots)
      boot.used.reset();
    CHECK(same(run_order(boots), {}, true, 0));
  }
  // Overlapping used ranges stay unknown and keep the newer prefix.
  {
    const std::array boots{ring_boot(1, 0, used(9000, 9500)),
                           ring_boot(0, 0, used(3000, 5000)),
                           dated_boot(202611171200, used(4000, 6000))};
    CHECK(same(run_order(boots), {0}, true, 1));
  }
  // Used bytes that contradict a proof are ignored. R1 got UTC after D0,
  // so R0 and R1 are newer than D0 whatever the used bytes say.
  {
    const std::array boots{ring_boot(1, 0, used(100, 200)),
                           ring_boot(0, 202611171200, used(50, 90)),
                           dated_boot(202611171000, used(500, 600))};
    CHECK(same(run_order(boots), {0, 1, 2}, false, 0));
  }
  // Proofs that disagree stop the order, used bytes never break the tie.
  {
    const std::array boots{ring_boot(1, 202611171000, used(9000, 9100)),
                           ring_boot(0, 202611171400),
                           dated_boot(202611171200, used(1, 2))};
    CHECK(same(run_order(boots), {}, true, 0));
  }
  // Missing stats stay unknown, the newer prefix is kept.
  {
    const std::array boots{ring_boot(1, 0, used(5000, 6000)), ring_boot(0),
                           dated_boot(202611171200, used(1000, 2000))};
    CHECK(same(run_order(boots), {0}, true, 1));
    const std::array none{ring_boot(0),
                          dated_boot(202611171200, used(1000, 2000))};
    CHECK(same(run_order(none), {}, true, 0));
  }
  // A full output ends the list without a cut, more than kMaxBoots places
  // nothing and no boots is an empty order.
  {
    const std::array boots{ring_boot(0), ring_boot(1), ring_boot(2)};
    CHECK(same(run_order(boots, 2), {2, 1}, false, 0));
    std::vector<order::BootKey> many(order::kMaxBoots + 1, ring_boot(0));
    CHECK(same(run_order(many), {}, true, 0));
    CHECK(same(run_order({}), {}, false, 0));
  }
}

} // namespace

int main() {
  test_canvas();
  test_utf8_and_text();
  test_history();
  test_history_seed();
  test_chart();
  test_bands();
  test_quality_and_format();
  test_navigation();
  test_render_all_pages();
  test_pairing_screen();
  test_settings_blob();
  test_popup_rows();
  test_popup_numeric();
  test_ui_handle();
  test_render_settings_and_sleep();
  test_theme();
  test_phone_keys();
  test_restore_order();
  std::printf("crowpanel ui, %d checks passed\n", g_checks);
  return 0;
}
