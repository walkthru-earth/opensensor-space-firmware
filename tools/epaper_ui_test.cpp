// Host tests for the shared e-paper UI, canvas clipping, UTF-8, history
// rings, chart gaps, PM2.5 bands, reading quality, navigation, the display
// profile and refresh policy, rotation and the Presenter, the motion sensor
// tracker, the settings blob and popup, the theme, the sleeping screen and
// the CrowPanel's boot order of the chart history restore, and the Arabic
// strings, glyphs and fit. Built and run by tools/test_epaper_ui.py with
// sanitizers.

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
#include <utility>
#include <vector>

#include "display/display_profile.h"
#include "display_profile.h"
#include "epaper_ui/air_band.h"
#include "epaper_ui/canvas.h"
#include "epaper_ui/chart.h"
#include "epaper_ui/history.h"
#include "epaper_ui/orientation.h"
#include "epaper_ui/pages.h"
#include "epaper_ui/panel.h"
#include "epaper_ui/presenter.h"
#include "epaper_ui/profile.h"
#include "epaper_ui/settings.h"
#include "epaper_ui/strings.h"
#include "epaper_ui/ui_model.h"
#include "epaper_ui_host_profiles.h"
#include "os_text.h"
#include "restore_order.h"

using namespace opensensor::ui;
using opensensor::epd::Refresh;
using opensensor::ui::host::kHost400x300;
using opensensor::ui::host::kHost800x480;

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

// The 400 x 300 class at R0, the frame most tests draw.
constexpr int kWidth = 400;
constexpr int kHeight = 300;
constexpr std::size_t kFrameBytes = 15000;

Canvas must(std::expected<Canvas, CanvasError> c) {
  if (!c) {
    std::fprintf(stderr, "FAIL canvas %s\n", to_string(c.error()));
    std::exit(1);
  }
  return *c;
}

// A logical frame and its canvas. `bytes` holds logical_frame_bytes(), the
// canvas uses its first bytes_for() bytes and the rest stays zero.
struct Buffer {
  explicit Buffer(const DisplayProfile &p = kHost400x300,
                  Rotation r = Rotation::R0)
      : bytes(p.logical_frame_bytes()),
        canvas(must(Canvas::create(bytes, p, r))) {}
  std::vector<std::uint8_t> bytes;
  Canvas canvas;
};

const FontSet &kFonts = fonts::compact;

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
  CHECK(find_glyph(kFonts.label, 'A') != nullptr);
  CHECK(find_glyph(kFonts.hero, 'A') == nullptr);
  CHECK(find_glyph(kFonts.hero, '7') != nullptr);
  CHECK(find_glyph(kFonts.small, 0x00B3) != nullptr);
  auto b = std::make_unique<Buffer>();
  const int adv = b->canvas.text(kFonts.value, 10, 40, "12.4");
  CHECK(adv == text_width(kFonts.value, "12.4"));
  CHECK(adv > 0);
  CHECK(ink_count(b->canvas, {0, 0, 200, 60}) > 0);
  // Right aligned text ends at x.
  b->canvas.clear();
  b->canvas.text(kFonts.label, 200, 100, "ppm", Align::Right);
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
  line_chart(b->canvas, plot, {.ring = nan3, .start = 0, .restarts = {}},
             {.zero_floor = false,
              .min_span = 0.0F,
              .x_labels = {},
              .empty_text = tr(Str::NoDataYet)});
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
    CHECK(text_width(kFonts.label, tr(row.word)) + 16 < 200);
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

  RefreshPolicy policy{kHost400x300.refresh};
  CHECK(policy.next(false, false) == Refresh::Full);
  for (int i = 0; i < kHost400x300.refresh.partials_per_full; ++i)
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
                 .sensor_model = "SEN63C",
                 .sensor_serial = "1234567890ABCDEF1234567890ABCDEF",
                 .sensor_fw = "2.0",
                 .sensor_status = 0xFFFFFFFFU,
                 .sensor_present = true,
                 .pairing_passkey = std::nullopt,
                 .device_name = "OS-ABCDEF",
                 // Past 100 percent and charging, the clamp and the mark.
                 .battery = Battery{.percent = 255, .charging = true}};
  for (std::uint32_t t = 0; t < 3 * 3600; t += 10)
    h->push(t, all(static_cast<float>(t % 500)));
  for (std::uint8_t p = 0; p < kPageCount; ++p)
    render(b->canvas, p, full, *h);
  full.status.utc_s = -1;
  render(b->canvas, kStatusPage, full, *h);
}

// The battery charge fills in proportion and clamps past 100 percent, the
// header without a battery draws as it did before the icon, and the icon
// only ever adds ink to the header's right side.
void test_battery_header() {
  static_assert(battery_fill(26, 0) == 0);
  static_assert(battery_fill(26, 50) == 13);
  static_assert(battery_fill(26, 100) == 26);
  static_assert(battery_fill(26, 150) == 26);
  static_assert(battery_fill(26, 255) == 26);
  static_assert(battery_fill(0, 50) == 0);
  static_assert(battery_fill(-3, 100) == 0);
  CHECK(battery_fill(12, 1) == 0);
  CHECK(battery_fill(12, 5) == 1);
  CHECK(battery_fill(12, 99) == 12);
  for (int p = 0; p <= 255; ++p) {
    const int f = battery_fill(26, static_cast<std::uint8_t>(p));
    CHECK(f >= 0 && f <= 26);
    if (p > 0)
      CHECK(f >= battery_fill(26, static_cast<std::uint8_t>(p - 1)));
  }

  auto h = std::make_unique<History>();
  Model m{};
  m.status.utc_s = 1795437120;
  m.status.sd_ok = true;
  m.status.ble = Link::Connected;
  m.status.wifi = Link::Waiting;
  // The frame drawn with `battery`, at `p` and `r`.
  const auto draw = [&](const DisplayProfile &p, Rotation r,
                        std::optional<Battery> battery) {
    Buffer b(p, r);
    m.status.battery = battery;
    render(b.canvas, kDashboardPage, m, *h);
    return b.bytes;
  };
  // The rows of a frame from `y0` down, R0 rows run width / 8 bytes.
  const auto rows_from = [](const std::vector<std::uint8_t> &f, int width,
                            int y0) {
    return std::vector<std::uint8_t>(
        f.begin() + static_cast<std::ptrdiff_t>(y0 * width / 8), f.end());
  };
  for (const DisplayProfile *p : {&kHost400x300, &kHost800x480}) {
    Buffer probe(*p, Rotation::R0);
    const int w = probe.canvas.width();
    const int rule_y = probe.canvas.layout().header.height;
    const auto none = draw(*p, Rotation::R0, std::nullopt);
    const auto empty = draw(*p, Rotation::R0, Battery{.percent = 0});
    const auto half = draw(*p, Rotation::R0, Battery{.percent = 50});
    const auto full = draw(*p, Rotation::R0, Battery{.percent = 100});
    const auto over = draw(*p, Rotation::R0, Battery{.percent = 200});
    const auto charging =
        draw(*p, Rotation::R0, Battery{.percent = 50, .charging = true});
    CHECK(none != empty);
    CHECK(empty != half);
    CHECK(half != full);
    CHECK(half != charging);
    // A percent past 100 draws as 100.
    CHECK(full == over);
    // Only the header moves, the rule and every row under it stay.
    CHECK(rows_from(none, w, rule_y) == rows_from(full, w, rule_y));
    // The percent ends on the right margin, as the last tag did, and the
    // icon adds ink to the header.
    Buffer with(*p, Rotation::R0);
    m.status.battery = Battery{.percent = 100};
    render(with.canvas, kDashboardPage, m, *h);
    Buffer without(*p, Rotation::R0);
    m.status.battery = std::nullopt;
    render(without.canvas, kDashboardPage, m, *h);
    const int margin = with.canvas.layout().header.margin;
    CHECK(ink_count(with.canvas, {w - margin, 0, margin, rule_y}) == 0);
    CHECK(ink_count(with.canvas, {0, 0, w, rule_y}) >
          ink_count(without.canvas, {0, 0, w, rule_y}));
  }
  // At 300 x 400 the three tags leave the clock no room for even the icon,
  // so the header draws as it does with no battery. With one tag fewer the
  // icon comes back.
  CHECK(draw(kHost400x300, Rotation::R90, Battery{.percent = 87}) ==
        draw(kHost400x300, Rotation::R90, std::nullopt));
  m.status.wifi = Link::Off;
  CHECK(draw(kHost400x300, Rotation::R90, Battery{.percent = 87}) !=
        draw(kHost400x300, Rotation::R90, std::nullopt));
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
  CHECK(text_width(kFonts.hero, "000042") > 3 * text_width(kFonts.hero, "42"));
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
  CHECK(d.theme == Theme::Light && d.rotation == ScreenRotation::R0);
  CHECK(d.language == Language::English);
  CHECK(defaults(false) == d);
  CHECK(defaults(true).rotation == ScreenRotation::Auto);

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
                   .theme = Theme::Dark,
                   .rotation = ScreenRotation::R270,
                   .language = Language::Arabic};
  const SettingsBlob b = encode(s);
  CHECK(b.size() == 13 && b[0] == 3 && b[10] == 1 && b[11] == 3 && b[12] == 1);
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
  bad[0] = 4;
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
  // A version 1 blob saved before the theme existed, ten bytes, keeps every
  // older field and gives Light and the default rotation.
  std::vector<std::uint8_t> old(b.begin(), b.begin() + 10);
  old[0] = 1;
  Settings want_old = s;
  want_old.theme = Theme::Light;
  want_old.rotation = ScreenRotation::R0;
  want_old.language = Language::English;
  CHECK(decode(old) == want_old);
  // A version 1 blob of eleven bytes migrates. Every field stays and the
  // rotation is the board default, Auto only with a motion sensor.
  std::vector<std::uint8_t> v1(b.begin(), b.begin() + 11);
  v1[0] = 1;
  Settings want_v1 = s;
  want_v1.rotation = ScreenRotation::R0;
  want_v1.language = Language::English;
  CHECK(decode(v1) == want_v1);
  want_v1.rotation = ScreenRotation::Auto;
  CHECK(decode(v1, true) == want_v1);
  // Version 1 never had bytes 11 and 12, stray ones are not read.
  v1.push_back(2);
  v1.push_back(1);
  want_v1.rotation = ScreenRotation::R0;
  CHECK(decode(v1) == want_v1);
  // The migrated settings save as version 3 and read back the same.
  CHECK(encode(want_v1)[0] == 3);
  CHECK(decode(encode(want_v1)) == want_v1);
  // A version 2 blob, twelve bytes, migrates. Every field stays, rotation
  // too, and the language is English.
  std::vector<std::uint8_t> v2(b.begin(), b.begin() + 12);
  v2[0] = 2;
  Settings want_v2 = s;
  want_v2.language = Language::English;
  CHECK(decode(v2) == want_v2);
  CHECK(decode(v2, true) == want_v2);
  // Version 2 never had byte 12, a stray Arabic one is not read.
  v2.push_back(1);
  CHECK(decode(v2) == want_v2);
  CHECK(decode(encode(want_v2)) == want_v2);
  // A version 2 blob from before the theme row was set, eleven bytes, keeps
  // its fields and gives the default rotation and English.
  std::vector<std::uint8_t> v2_short(b.begin(), b.begin() + 11);
  v2_short[0] = 2;
  Settings want_v2_short = want_v2;
  want_v2_short.rotation = ScreenRotation::R0;
  CHECK(decode(v2_short) == want_v2_short);
  // Both languages survive the round trip.
  for (const Language lang : {Language::English, Language::Arabic}) {
    Settings t{};
    t.language = lang;
    CHECK(decode(encode(t)).language == lang);
  }
  // A longer blob reads its first thirteen bytes.
  std::vector<std::uint8_t> longer(b.begin(), b.end());
  longer.push_back(0xEE);
  CHECK(decode(longer) == s);
  // Every rotation survives the round trip, Auto only with a sensor.
  for (const ScreenRotation r : {ScreenRotation::R0, ScreenRotation::R90,
                                 ScreenRotation::R180, ScreenRotation::R270}) {
    Settings t{};
    t.rotation = r;
    CHECK(decode(encode(t)).rotation == r);
    CHECK(decode(encode(t), true).rotation == r);
  }
  {
    Settings t{};
    t.rotation = ScreenRotation::Auto;
    CHECK(decode(encode(t), true).rotation == ScreenRotation::Auto);
    CHECK(decode(encode(t), false).rotation == ScreenRotation::R0);
  }

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
  CHECK(out_of_range(11, 5).rotation == ScreenRotation::R0);
  CHECK(out_of_range(11, 0xFF).rotation == ScreenRotation::R0);
  CHECK(out_of_range(11, 5).theme == Theme::Dark);
  CHECK(out_of_range(12, 2).language == Language::English);
  CHECK(out_of_range(12, 0xFF).language == Language::English);
  CHECK(out_of_range(12, 2).rotation == ScreenRotation::R270);
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
  // The display rows sit together, Theme right after Start on and Language
  // right after Rotation.
  {
    constexpr std::array<Row, kRowCount> kOrder{
        Row::Refresh,  Row::StartPage,  Row::Theme,       Row::Rotation,
        Row::Language, Row::Wifi,       Row::FanCleaning, Row::Co2Asc,
        Row::Altitude, Row::TempOffset, Row::SleepAfter};
    PopupState p = opened();
    Settings s{};
    for (const Row want : kOrder) {
      CHECK(p.selected == want);
      static_cast<void>(popup_input(p, s, kAllThere, Input::Down));
    }
    CHECK(p.selected == Row::Refresh);
    CHECK(row_label(Row::Theme) == "Theme");
    CHECK(row_label(Row::Rotation) == "Rotation");
    CHECK(row_label(Row::Language) == "Language");
  }
  // Rotation cycles 0, 90, 180, 270 without a motion sensor, and Auto first
  // with one.
  {
    std::array<char, 24> buf{};
    PopupState p = at(Row::Rotation);
    Settings s{};
    CHECK(value_of(Row::Rotation, s, p, kAllThere, buf) == "0°");
    for (const std::string_view want : {"90°", "180°", "270°", "0°"}) {
      CHECK(popup_input(p, s, kAllThere, Input::Press) == Effect::Changed);
      CHECK(value_of(Row::Rotation, s, p, kAllThere, buf) == want);
    }
    CHECK(s.rotation == ScreenRotation::R0);
    SettingsInfo moving = kAllThere;
    moving.orientation_source = true;
    Settings m = defaults(true);
    CHECK(value_of(Row::Rotation, m, p, moving, buf) == "Auto");
    for (const std::string_view want : {"0°", "90°", "180°", "270°", "Auto"}) {
      CHECK(popup_input(p, m, moving, Input::Press) == Effect::Changed);
      CHECK(value_of(Row::Rotation, m, p, moving, buf) == want);
    }
    CHECK(m.rotation == ScreenRotation::Auto);
    // A stored Auto on a board without a sensor steps on to 0.
    Settings stray{};
    stray.rotation = ScreenRotation::Auto;
    CHECK(popup_input(p, stray, kAllThere, Input::Press) == Effect::Changed);
    CHECK(stray.rotation == ScreenRotation::R0);
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
    RefreshPolicy policy{kHost400x300.refresh};
    static_cast<void>(policy.next(false, false));
    CHECK(policy.next(false, false) == Refresh::Partial);
    CHECK(policy.next(dark.page_changed, dark.full_refresh) == Refresh::Full);
    // A rotation change asks for a full refresh too.
    ui.popup.selected = Row::Rotation;
    const Outcome turned = handle(ui, s, info, Input::Press);
    CHECK(turned.effect == Effect::Changed && turned.full_refresh);
    CHECK(s.rotation == ScreenRotation::R90 && ui.popup.open);
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
  // On the screen's own settings, Last viewed from the phone records the
  // shown page as the knob does, and Dashboard leaves last_page alone.
  {
    Settings shown;
    PhoneDraft last;
    CHECK(set_phone_key(last, "panel.start_page", "last_viewed").has_value());
    apply_phone_draft(shown, last, 2);
    CHECK(shown.start_page == StartPage::LastViewed && shown.last_page == 2);
    CHECK(start_page(shown) == 2);
    PhoneDraft dash;
    CHECK(set_phone_key(dash, "panel.start_page", "dashboard").has_value());
    apply_phone_draft(shown, dash, 3);
    CHECK(shown.start_page == StartPage::Dashboard && shown.last_page == 2);
    PhoneDraft other;
    CHECK(set_phone_key(other, "panel.theme", "dark").has_value());
    apply_phone_draft(shown, other, 4);
    CHECK(shown.last_page == 2);
    apply_phone_draft(shown, last, kPageCount);
    CHECK(shown.last_page == 2);
  }

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

constexpr std::array<Rotation, 4> kRotations{Rotation::R0, Rotation::R90,
                                             Rotation::R180, Rotation::R270};

// The CrowPanel's own profile is the host 400 x 300 one, field by field, so
// the golden frames in tools/test_epaper_ui.py are the frames it sends.
void test_crowpanel_profile() {
  const DisplayProfile &board = opensensor::crowpanel::kDisplayProfile;
  const DisplayProfile &host = kHost400x300;
  CHECK(board.width == host.width && board.height == host.height);
  CHECK(board.depth == host.depth);
  CHECK(board.refresh.fast == host.refresh.fast);
  CHECK(board.refresh.partial == host.refresh.partial);
  CHECK(board.refresh.partial_window == host.refresh.partial_window);
  CHECK(board.refresh.full_ms == host.refresh.full_ms);
  CHECK(board.refresh.fast_ms == host.refresh.fast_ms);
  CHECK(board.refresh.partial_ms == host.refresh.partial_ms);
  CHECK(board.refresh.partials_per_full == host.refresh.partials_per_full);
  CHECK(board.font_scale == host.font_scale);
  CHECK(board.layout == host.layout);
  CHECK(board.rotated_layout == host.rotated_layout);
  CHECK(board.mount == host.mount);
  CHECK(board.orientation.present == host.orientation.present);
  CHECK(validate(board).has_value());
}

// The ePaper-3.97 profile is the host 800 x 480 one apart from its measured
// full refresh and its motion sensor, so the 800 x 480 previews are the
// frames it sends.
void test_epaper397_profile() {
  const DisplayProfile &board = opensensor::epaper397::kDisplayProfile;
  const DisplayProfile &host = kHost800x480;
  CHECK(board.width == host.width && board.height == host.height);
  CHECK(board.depth == host.depth);
  CHECK(board.frame_bytes() == host.frame_bytes());
  CHECK(board.logical_frame_bytes() == host.logical_frame_bytes());
  CHECK(board.font_scale == host.font_scale);
  CHECK(board.layout == host.layout);
  CHECK(board.rotated_layout == host.rotated_layout);
  CHECK(board.mount == host.mount);
  CHECK(board.refresh.fast && board.refresh.partial &&
        board.refresh.partial_window);
  CHECK(board.refresh.full_ms == 2531);
  CHECK(board.refresh.fast_ms == 0 && board.refresh.partial_ms == 0);
  CHECK(board.refresh.partials_per_full == 20);
  CHECK(board.orientation.present);
  CHECK(validate(board).has_value());
  // The tracker reads board::kImuToPanel, the table the board's own
  // quarter_turn() logs with. A device turned clockwise by q quarters shows
  // the picture turned back by q, so each pose the board logs as quarter q
  // settles on R(4 - q). Since 2026-11-21 IMU x runs against panel x, so a
  // quarter turn clockwise reads +1 g on IMU x.
  CHECK(board.orientation.axes.right == Axis::NegX);
  CHECK(board.orientation.axes.down == Axis::PosY);
  struct Pose {
    std::int16_t x;
    std::int16_t y;
  };
  constexpr std::array<Pose, 4> kPoses{
      {{0, -16384}, {16384, 0}, {0, 16384}, {-16384, 0}}};
  for (std::size_t q = 0; q < kPoses.size(); ++q) {
    const board::Accel a{.x = kPoses[q].x, .y = kPoses[q].y, .z = 0};
    CHECK(board::quarter_turn(a) == q);
    OrientationTracker t{board.orientation.axes, board.mount};
    const Gravity up{.x = static_cast<float>(a.x),
                     .y = static_cast<float>(a.y),
                     .z = static_cast<float>(a.z)};
    for (std::uint32_t ms = 0; ms <= 2500; ms += 500)
      static_cast<void>(t.update(up, ms));
    CHECK(t.current() == static_cast<Rotation>((4 - q) % 4));
  }
  // Lying flat keeps the picture where it is, face down or face up, where
  // the owner's board read az about -19000.
  for (const float z : {16384.0F, -19000.0F}) {
    OrientationTracker flat{board.orientation.axes, Rotation::R90};
    for (std::uint32_t ms = 0; ms <= 2500; ms += 500)
      static_cast<void>(flat.update(Gravity{.x = 0.0F, .y = 0.0F, .z = z}, ms));
    CHECK(flat.current() == Rotation::R90);
    CHECK(!board::quarter_turn(
        {.x = 0, .y = 0, .z = static_cast<std::int16_t>(z)}));
  }
}

void test_profile() {
  CHECK(validate(kHost400x300).has_value());
  CHECK(validate(kHost800x480).has_value());
  CHECK(kHost400x300.frame_bytes() == kFrameBytes);
  CHECK(kHost400x300.logical_frame_bytes() == 15200);
  CHECK(kHost800x480.frame_bytes() == 48000);
  CHECK(kHost800x480.logical_frame_bytes() == 48000);
  const auto refused = [](DisplayProfile p, ProfileError want) {
    const auto r = validate(p);
    return !r.has_value() && r.error() == want;
  };
  DisplayProfile p = kHost400x300;
  p.width = 404;
  CHECK(refused(p, ProfileError::BadSize));
  p = kHost400x300;
  p.height = 0;
  CHECK(refused(p, ProfileError::BadSize));
  p = kHost400x300;
  p.depth = ColorDepth::Grey4;
  CHECK(refused(p, ProfileError::UnsupportedDepth));
  p = kHost400x300;
  p.rotated_layout = nullptr;
  CHECK(refused(p, ProfileError::NoLayout));
  p = kHost400x300;
  p.layout = &kLayout800x480;
  CHECK(refused(p, ProfileError::LayoutMismatch));
  p = kHost400x300;
  p.rotated_layout = &kLayout400x300;
  CHECK(refused(p, ProfileError::LayoutMismatch));
  p = kHost400x300;
  p.font_scale = FontScale::Large;
  CHECK(refused(p, ProfileError::LayoutMismatch));
  p = kHost400x300;
  p.refresh.partial = false;
  p.refresh.partial_window = true;
  CHECK(refused(p, ProfileError::BadRefreshCaps));
  p = kHost400x300;
  p.refresh.partials_per_full = 0;
  CHECK(refused(p, ProfileError::BadRefreshCaps));

  // Every layout table matches the size and font scale it is named for.
  CHECK(kLayout400x300.width == 400 && kLayout400x300.height == 300);
  CHECK(kLayout300x400.width == 300 && kLayout300x400.height == 400);
  CHECK(kLayout800x480.width == 800 && kLayout800x480.height == 480);
  CHECK(kLayout480x800.width == 480 && kLayout480x800.height == 800);
  CHECK(&kLayout400x300.fonts.hero == &fonts::compact.hero);
  CHECK(&kLayout480x800.fonts.hero == &fonts::large.hero);

  // The canvas takes its size from the profile and the rotation.
  std::vector<std::uint8_t> frame(kHost400x300.logical_frame_bytes());
  for (const Rotation r : kRotations) {
    const auto c = Canvas::create(frame, kHost400x300, r);
    CHECK(c.has_value());
    if (!c)
      continue;
    const bool tall = swaps_axes(r);
    CHECK(c->width() == (tall ? 300 : 400) &&
          c->height() == (tall ? 400 : 300));
    CHECK(c->stride() == (tall ? 38 : 50));
    CHECK(&c->layout() == (tall ? &kLayout300x400 : &kLayout400x300));
  }
  CHECK(Canvas::bytes_for(kHost400x300, Rotation::R90) == 15200);
  CHECK(Canvas::bytes_for(kHost400x300, Rotation::R180) == 15000);
  std::vector<std::uint8_t> short_frame(15100);
  const auto too_short =
      Canvas::create(short_frame, kHost400x300, Rotation::R90);
  CHECK(!too_short && too_short.error() == CanvasError::FrameSize);
  CHECK(Canvas::create(short_frame, kHost400x300, Rotation::R0).has_value());
  DisplayProfile grey = kHost400x300;
  grey.depth = ColorDepth::Grey4;
  const auto bad = Canvas::create(frame, grey);
  CHECK(!bad && bad.error() == CanvasError::BadProfile);
}

// The policy never asks for a mode the caps lack, whatever the inputs.
void test_policy_caps() {
  for (int bits = 0; bits < 8; ++bits) {
    RefreshCaps caps{.fast = (bits & 1) != 0,
                     .partial = (bits & 2) != 0,
                     .partial_window = (bits & 4) != 0,
                     .full_ms = 0,
                     .fast_ms = 0,
                     .partial_ms = 0,
                     .partials_per_full = 5};
    RefreshPolicy policy{caps};
    CHECK(policy.next(false, false) == Refresh::Full);
    int run = 0;
    for (int i = 0; i < 200; ++i) {
      const bool page = i % 7 == 3;
      const bool forced = i % 23 == 11;
      const Refresh m = policy.next(page, forced);
      CHECK(m != Refresh::Fast || caps.fast);
      CHECK(m != Refresh::Partial || caps.partial);
      if (forced)
        CHECK(m == Refresh::Full);
      // A page change is never Partial, the old page would ghost. It is
      // Fast where the panel has it, unless the partial run is due a Full.
      if (page)
        CHECK(m == Refresh::Full || (caps.fast && m == Refresh::Fast));
      if (page && !forced && run == 0 && !caps.partial)
        CHECK(m == (caps.fast ? Refresh::Fast : Refresh::Full));
      run = m == Refresh::Partial ? run + 1 : 0;
      CHECK(run <= caps.partials_per_full);
    }
  }
  // Without partial, plain updates are Fast where the panel has it.
  RefreshPolicy fast_only{{.fast = true}};
  CHECK(fast_only.next(false, false) == Refresh::Full);
  CHECK(fast_only.next(false, false) == Refresh::Fast);
  RefreshPolicy full_only{{}};
  CHECK(full_only.next(false, false) == Refresh::Full);
  CHECK(full_only.next(false, false) == Refresh::Full);
}

// Panel bit (x, y) of a frame in the profile's convention.
bool panel_bit(std::span<const std::uint8_t> f, const DisplayProfile &p, int x,
               int y) {
  const auto i =
      static_cast<std::size_t>(y) * static_cast<std::size_t>(p.stride()) +
      static_cast<std::size_t>(x / 8);
  return (f[i] & (0x80U >> (x % 8))) != 0;
}

// Where logical (lx, ly) lands on the panel, the mapping orientation.h
// describes, written out pixel by pixel.
std::pair<int, int> on_panel(const DisplayProfile &p, Rotation r, int lx,
                             int ly) {
  switch (r) {
  case Rotation::R0:
    return {lx, ly};
  case Rotation::R90:
    return {p.width - 1 - ly, lx};
  case Rotation::R180:
    return {p.width - 1 - lx, p.height - 1 - ly};
  case Rotation::R270:
    return {ly, p.height - 1 - lx};
  }
  return {0, 0};
}

void test_rotate() {
  for (const DisplayProfile *p : {&kHost400x300, &kHost800x480}) {
    std::vector<std::uint8_t> panel(p->frame_bytes());
    for (const Rotation r : kRotations) {
      Buffer b(*p, r);
      Canvas &c = b.canvas;
      // A scatter of pixels, the corners and the top left text, so a wrong
      // turn or mirror shows.
      std::uint32_t seed = 7;
      for (int i = 0; i < 4000; ++i) {
        seed = seed * 1664525U + 1013904223U;
        c.pixel(static_cast<int>((seed >> 8) %
                                 static_cast<std::uint32_t>(c.width())),
                static_cast<int>((seed >> 20) %
                                 static_cast<std::uint32_t>(c.height())));
      }
      c.pixel(0, 0);
      c.pixel(c.width() - 1, c.height() - 1);
      static_cast<void>(c.text(c.fonts().label, 4, 30, "Rotation"));
      std::fill(panel.begin(), panel.end(), std::uint8_t{0xA5});
      rotate_frame(b.bytes, *p, r, panel);
      bool every = true;
      int ink = 0;
      for (int ly = 0; ly < c.height(); ++ly)
        for (int lx = 0; lx < c.width(); ++lx) {
          const auto [px, py] = on_panel(*p, r, lx, ly);
          const bool want = c.get(lx, ly) == Ink::Black;
          ink += want ? 1 : 0;
          every = every && panel_bit(panel, *p, px, py) == want;
        }
      CHECK(every);
      CHECK(ink > 3000);
      // Every panel byte was written, the 0xA5 fill is gone where the
      // drawn frame is white.
      int panel_ink = 0;
      for (int y = 0; y < p->height; ++y)
        for (int x = 0; x < p->width; ++x)
          panel_ink += panel_bit(panel, *p, x, y) ? 1 : 0;
      CHECK(panel_ink == ink);
      // R0 is the drawn frame byte for byte.
      if (r == Rotation::R0)
        CHECK(std::equal(panel.begin(), panel.end(), b.bytes.begin()));
    }
  }
  // A short buffer is left alone.
  std::vector<std::uint8_t> small(10, 0x5A);
  std::vector<std::uint8_t> logical(kHost400x300.logical_frame_bytes());
  rotate_frame(logical, kHost400x300, Rotation::R90, small);
  CHECK(small[0] == 0x5A && small[9] == 0x5A);
}

void test_changed_window() {
  std::vector<std::uint8_t> a(kFrameBytes);
  std::vector<std::uint8_t> b(kFrameBytes);
  CHECK(changed_window(a, b, kHost400x300).w == 0);
  // One pixel at (13, 7) is the byte holding it.
  b[7 * 50 + 1] = 0x04;
  Rect w = changed_window(a, b, kHost400x300);
  CHECK(w.x == 8 && w.y == 7 && w.w == 8 && w.h == 1);
  // A second one widens it to whole bytes.
  b[120 * 50 + 49] = 0x01;
  w = changed_window(a, b, kHost400x300);
  CHECK(w.x == 8 && w.y == 7 && w.w == 392 && w.h == 114);
  CHECK(w.x % 8 == 0 && w.w % 8 == 0);
  CHECK(changed_window(a, std::span<const std::uint8_t>{b}.first(10),
                       kHost400x300)
            .w == 0);
}

// A Panel for the host. It keeps what it was asked to show.
struct FakePanel {
  std::vector<std::uint8_t> glass;
  Refresh last_mode = Refresh::Full;
  Rect last_window{};
  int shows = 0;
  bool fail = false;
  std::expected<void, opensensor::epd::Error>
  show(std::span<const std::uint8_t> frame, Refresh mode, Rect window) {
    if (fail)
      return std::unexpected(opensensor::epd::Error::Spi);
    glass.assign(frame.begin(), frame.end());
    last_mode = mode;
    last_window = window;
    ++shows;
    return {};
  }
  void sleep() {}
};
static_assert(opensensor::epd::Panel<FakePanel>);

void test_presenter() {
  for (const DisplayProfile *p : {&kHost400x300, &kHost800x480}) {
    std::vector<std::uint8_t> logical(p->logical_frame_bytes());
    std::vector<std::uint8_t> panel_buf(p->frame_bytes());
    std::vector<std::uint8_t> shown(p->frame_bytes());
    std::vector<std::uint8_t> short_buf(p->frame_bytes() - 1);
    const auto refused = Presenter::create(*p, logical, short_buf, shown);
    CHECK(!refused && refused.error() == PresenterError::BufferSize);
    const auto short_logical = Presenter::create(
        *p, std::span{logical}.first(p->frame_bytes() - 1), panel_buf, shown);
    CHECK(!short_logical &&
          short_logical.error() == PresenterError::BufferSize);
    auto made = Presenter::create(*p, logical, panel_buf, shown);
    CHECK(made.has_value());
    if (!made)
      continue;
    Presenter &pr = *made;
    FakePanel glass;
    const Rect whole{0, 0, p->width, p->height};
    const auto draw = [&](Rotation r, int dots) {
      Canvas c = must(pr.canvas(r));
      c.clear();
      static_cast<void>(c.text(c.fonts().title, 10, 60, "Air"));
      for (int i = 0; i < dots; ++i)
        c.pixel(20 + 3 * i, 100);
    };
    // The first frame is Full over the whole panel.
    draw(Rotation::R0, 1);
    CHECK(pr.compose());
    Plan plan = pr.plan(false, false);
    CHECK(plan.mode == Refresh::Full && plan.window == whole);
    CHECK(pr.present(glass, plan).has_value());
    CHECK(std::equal(glass.glass.begin(), glass.glass.end(),
                     pr.panel_frame().begin()));
    // The same picture again needs no refresh.
    draw(Rotation::R0, 1);
    CHECK(!pr.compose());
    // A small change is Partial, windowed where the profile allows it.
    draw(Rotation::R0, 2);
    CHECK(pr.compose());
    plan = pr.plan(false, false);
    CHECK(plan.mode == Refresh::Partial);
    if (p->refresh.partial_window)
      CHECK(plan.window.y == 100 && plan.window.h == 1 && plan.window.x == 16 &&
            plan.window.w == 8);
    else
      CHECK(plan.window == whole);
    CHECK(pr.present(glass, plan).has_value());
    // A failed show keeps the old frame as the one on the glass.
    glass.fail = true;
    draw(Rotation::R0, 3);
    CHECK(pr.compose());
    plan = pr.plan(false, false);
    CHECK(!pr.present(glass, plan).has_value());
    glass.fail = false;
    draw(Rotation::R0, 2);
    CHECK(!pr.compose());
    // A turn always differs and is always Full over the whole panel, even
    // when the turned picture happened to match.
    draw(Rotation::R180, 2);
    CHECK(pr.compose());
    plan = pr.plan(false, false);
    CHECK(plan.mode == Refresh::Full && plan.window == whole);
    CHECK(pr.present(glass, plan).has_value());
    draw(Rotation::R180, 2);
    CHECK(!pr.compose());
    draw(Rotation::R90, 2);
    CHECK(pr.compose());
    CHECK(pr.plan(false, false).mode == Refresh::Full);
    // A page change on a panel with Fast is Fast.
    CHECK(pr.present(glass, {.mode = Refresh::Full, .window = whole})
              .has_value());
    draw(Rotation::R90, 9);
    CHECK(pr.compose());
    CHECK(pr.plan(true, false).mode ==
          (p->refresh.fast ? Refresh::Fast : Refresh::Full));
  }
}

// One accelerometer reading for a device turned `deg` clockwise from upright
// in its own plane, tipped `tilt` of the way towards lying flat, in the
// sensor axes of `axes`. Reads up, away from the ground.
Gravity reading(SensorAxes axes, float deg, float tilt = 0.0F) {
  const float rad = deg / 57.29578F;
  // Upright pull is panel down. Turning the device by deg clockwise makes
  // the pull come from the panel's left, so in panel axes the pull is
  // (-sin, cos). The reading is the negative, scaled by how upright it is.
  const float upright = std::sqrt(1.0F - tilt * tilt);
  const float right = std::sin(rad) * upright;
  const float down = -std::cos(rad) * upright;
  Gravity g{};
  const auto put = [&g](Axis a, float v) {
    switch (a) {
    case Axis::PosX:
      g.x = v;
      break;
    case Axis::NegX:
      g.x = -v;
      break;
    case Axis::PosY:
      g.y = v;
      break;
    case Axis::NegY:
      g.y = -v;
      break;
    case Axis::PosZ:
      g.z = v;
      break;
    case Axis::NegZ:
      g.z = -v;
      break;
    }
  };
  put(axes.right, right);
  put(axes.down, down);
  // The third axis takes the tilt. Any axis not used above.
  const bool x_used = axes.right == Axis::PosX || axes.right == Axis::NegX ||
                      axes.down == Axis::PosX || axes.down == Axis::NegX;
  const bool y_used = axes.right == Axis::PosY || axes.right == Axis::NegY ||
                      axes.down == Axis::PosY || axes.down == Axis::NegY;
  if (!x_used)
    g.x = tilt;
  else if (!y_used)
    g.y = tilt;
  else
    g.z = tilt;
  return g;
}

void test_orientation() {
  const SensorAxes axes{};
  // Upright keeps R0, each quarter turn held for 2 s turns the picture.
  {
    OrientationTracker t{axes};
    CHECK(t.update(reading(axes, 0.0F), 0) == Rotation::R0);
    CHECK(t.update(reading(axes, 90.0F), 100) == Rotation::R0);
    CHECK(t.update(reading(axes, 90.0F), 2099) == Rotation::R0);
    CHECK(t.update(reading(axes, 90.0F), 2100) == Rotation::R90);
    CHECK(t.current() == Rotation::R90);
    for (std::uint32_t ms = 3000; ms <= 5000; ms += 500)
      static_cast<void>(t.update(reading(axes, 180.0F), ms));
    CHECK(t.current() == Rotation::R180);
    for (std::uint32_t ms = 6000; ms <= 8000; ms += 500)
      static_cast<void>(t.update(reading(axes, 270.0F), ms));
    CHECK(t.current() == Rotation::R270);
    for (std::uint32_t ms = 9000; ms <= 11000; ms += 500)
      static_cast<void>(t.update(reading(axes, 0.0F), ms));
    CHECK(t.current() == Rotation::R0);
  }
  // Jitter around 45 degrees never turns it, from either side.
  {
    OrientationTracker t{axes};
    bool stayed = true;
    for (std::uint32_t i = 0; i < 400; ++i) {
      const float deg = 45.0F + ((i % 2) != 0 ? 8.0F : -8.0F) +
                        static_cast<float>(i % 5) - 2.0F;
      stayed = stayed && t.update(reading(axes, deg), i * 50) == Rotation::R0;
    }
    CHECK(stayed);
    OrientationTracker u{axes, Rotation::R90};
    stayed = true;
    for (std::uint32_t i = 0; i < 400; ++i) {
      const float deg = 45.0F + ((i % 2) != 0 ? 8.0F : -8.0F);
      stayed = stayed && u.update(reading(axes, deg), i * 50) == Rotation::R90;
    }
    CHECK(stayed);
    // Held at 59 degrees, inside the band, it stays. At 61 it turns.
    for (std::uint32_t i = 0; i < 100; ++i)
      static_cast<void>(t.update(reading(axes, 59.0F), 30000 + i * 100));
    CHECK(t.current() == Rotation::R0);
    for (std::uint32_t i = 0; i <= 25; ++i)
      static_cast<void>(t.update(reading(axes, 61.0F), 50000 + i * 100));
    CHECK(t.current() == Rotation::R90);
  }
  // A candidate that flickers away restarts its 2 s.
  {
    OrientationTracker t{axes};
    static_cast<void>(t.update(reading(axes, 90.0F), 0));
    static_cast<void>(t.update(reading(axes, 90.0F), 1500));
    static_cast<void>(t.update(reading(axes, 10.0F), 1600));
    CHECK(t.update(reading(axes, 90.0F), 1700) == Rotation::R0);
    CHECK(t.update(reading(axes, 90.0F), 3600) == Rotation::R0);
    CHECK(t.update(reading(axes, 90.0F), 3700) == Rotation::R90);
    // Two candidates in turn never add up.
    OrientationTracker u{axes};
    for (std::uint32_t i = 0; i < 40; ++i)
      static_cast<void>(
          u.update(reading(axes, (i % 2) != 0 ? 90.0F : 270.0F), i * 100));
    CHECK(u.current() == Rotation::R0);
  }
  // Flat on a table, or no reading, keeps the rotation and drops the
  // candidate.
  {
    OrientationTracker t{axes, Rotation::R270};
    for (std::uint32_t i = 0; i < 100; ++i)
      CHECK(t.update(reading(axes, static_cast<float>(i * 37 % 360), 0.95F),
                     i * 100) == Rotation::R270);
    CHECK(t.update(Gravity{0.0F, 0.0F, 1.0F}, 20000) == Rotation::R270);
    CHECK(t.update(Gravity{0.0F, 0.0F, -1.0F}, 20100) == Rotation::R270);
    CHECK(t.update(Gravity{}, 20200) == Rotation::R270);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(t.update(Gravity{nan, 0.0F, 0.0F}, 20300) == Rotation::R270);
    static_cast<void>(t.update(reading(axes, 0.0F), 21000));
    static_cast<void>(t.update(std::nullopt, 22000));
    CHECK(t.update(reading(axes, 0.0F), 23500) == Rotation::R270);
    static_cast<void>(t.update(reading(axes, 0.0F, 0.95F), 24000));
    CHECK(t.update(reading(axes, 0.0F), 25000) == Rotation::R270);
    CHECK(t.update(reading(axes, 0.0F), 27000) == Rotation::R0);
    // Tipped back a little, still upright enough, it still reads.
    OrientationTracker u{axes};
    static_cast<void>(u.update(reading(axes, 180.0F, 0.6F), 0));
    CHECK(u.update(reading(axes, 180.0F, 0.6F), 2000) == Rotation::R180);
  }
  // A clock that wraps still measures the hold.
  {
    OrientationTracker t{axes};
    const std::uint32_t start = 0xFFFFFFFFU - 999U;
    static_cast<void>(t.update(reading(axes, 270.0F), start));
    CHECK(t.update(reading(axes, 270.0F), start + 1999U) == Rotation::R0);
    CHECK(t.update(reading(axes, 270.0F), start + 2000U) == Rotation::R270);
  }
  // The mounting maps sensor axes to the panel. A sensor turned on the board
  // reads the same turn of the device the same way.
  for (const SensorAxes mounted :
       {SensorAxes{.right = Axis::NegY, .down = Axis::PosX},
        SensorAxes{.right = Axis::PosZ, .down = Axis::NegX},
        SensorAxes{.right = Axis::NegX, .down = Axis::NegZ}}) {
    for (const auto [deg, want] :
         {std::pair{90.0F, Rotation::R90}, std::pair{180.0F, Rotation::R180},
          std::pair{270.0F, Rotation::R270}}) {
      OrientationTracker t{mounted};
      static_cast<void>(t.update(reading(mounted, deg), 0));
      CHECK(t.update(reading(mounted, deg), 2000) == want);
      // The default axes read the same gravity as a different turn.
      OrientationTracker wrong{axes};
      static_cast<void>(wrong.update(reading(mounted, deg), 0));
      static_cast<void>(wrong.update(reading(mounted, deg), 2000));
      CHECK(wrong.current() != want || mounted.right == Axis::PosX);
    }
  }
}

void test_effective_rotation() {
  DisplayProfile p = kHost400x300;
  Settings s{};
  CHECK(effective_rotation(s, p, Rotation::R180) == Rotation::R0);
  s.rotation = ScreenRotation::R90;
  CHECK(effective_rotation(s, p, Rotation::R180) == Rotation::R90);
  // A board mounted upside down turns every setting by 180.
  p.mount = Rotation::R180;
  s.rotation = ScreenRotation::R0;
  CHECK(effective_rotation(s, p, Rotation::R0) == Rotation::R180);
  s.rotation = ScreenRotation::R270;
  CHECK(effective_rotation(s, p, Rotation::R0) == Rotation::R90);
  // Auto without a sensor is the mount, with one it is what the sensor says.
  s.rotation = ScreenRotation::Auto;
  CHECK(effective_rotation(s, p, Rotation::R90) == Rotation::R180);
  p.orientation.present = true;
  CHECK(effective_rotation(s, p, Rotation::R90) == Rotation::R90);
  s.rotation = ScreenRotation::R90;
  CHECK(effective_rotation(s, p, Rotation::R90) == Rotation::R270);
}

void test_line_break() {
  const Font &f = kFonts.small;
  constexpr std::string_view text = "Turn to choose, press to change";
  CHECK(line_break(f, text, 1000) == text.size());
  const int whole = text_width(f, text);
  CHECK(line_break(f, text, whole) == text.size());
  const std::size_t cut = line_break(f, text, whole - 1);
  CHECK(cut < text.size() && text[cut] == ' ');
  CHECK(text_width(f, text.substr(0, cut)) <= whole - 1);
  // No space that fits, it stays one line.
  CHECK(line_break(f, "Unbreakable", 5) ==
        std::string_view{"Unbreakable"}.size());
  CHECK(line_break(f, "", 5) == 0);
  // Two lines draw one step apart, one line draws as text() does.
  Buffer a;
  Buffer b;
  CHECK(a.canvas.text_lines(f, 10, 40, text, Align::Left, whole) == 1);
  static_cast<void>(b.canvas.text(f, 10, 40, text, Align::Left));
  CHECK(a.bytes == b.bytes);
  a.canvas.clear();
  CHECK(a.canvas.text_lines(f, 10, 40, text, Align::Left, whole / 2 + 20) == 2);
  CHECK(ink_count(a.canvas, {0, 40 + 2, kWidth, line_step(f)}) > 0);
}

// Every page, the popup and the asleep screen at every rotation of both
// size classes. The header rule spans the logical width each time and no
// drawing escapes the logical frame.
void test_render_rotations() {
  auto h = std::make_unique<History>();
  for (std::uint32_t t = 0; t < 3600; t += 10)
    h->push(t, all(static_cast<float>(t % 300)));
  Model m{};
  for (std::size_t i = 0; i < kMetricCount; ++i)
    m.now[i] = {.value = 1234.5F, .quality = Quality::Valid, .warm_left_s = 0};
  m.status.utc_s = 1795437120;
  m.status.thing = "OS-7F3A2C";
  m.status.device_name = "OS-7F3A2C";
  m.status.sensor_model = "SEN63C";
  m.status.firmware = "opensensor-crowpanel-a-very-long-identity-string";
  m.status.sensor_present = true;
  for (const DisplayProfile *p : {&kHost400x300, &kHost800x480}) {
    for (const Rotation r : kRotations) {
      Buffer b(*p, r);
      Canvas &c = b.canvas;
      const std::size_t used = Canvas::bytes_for(*p, r);
      const int rule_y = c.layout().header.height;
      for (std::uint8_t page = 0; page < kPageCount; ++page) {
        UiState ui{.page = page};
        Settings s{};
        render_screen(c, ui, s, kAllThere, m, *h);
        CHECK(ink_count(c, {0, rule_y, c.width(), 1}) == c.width());
        static_cast<void>(handle(ui, s, kAllThere, Input::Menu));
        for (std::uint8_t row = 0; row < kRowCount; ++row) {
          ui.popup.selected = static_cast<Row>(row);
          render_screen(c, ui, s, kAllThere, m, *h);
        }
        fall_asleep(ui);
        render_screen(c, ui, s, kAllThere, m, *h);
        CHECK(ink_count(c, {0, 0, c.width(), c.height()}) > 300);
      }
      // Nothing past the frame this rotation uses.
      CHECK(std::all_of(b.bytes.begin() + static_cast<std::ptrdiff_t>(used),
                        b.bytes.end(), [](std::uint8_t v) { return v == 0; }));
    }
  }
}

int g_misfits = 0;

// Counts and prints a string `w` pixels wide that passes `max_w`, so every
// string too long for a frame names itself in one run.
void check_fits(int w, int max_w, std::string_view what, const Canvas &c,
                int line) {
  ++g_checks;
  if (w <= max_w)
    return;
  ++g_misfits;
  std::fprintf(stderr, "FAIL line %d: %dx%d, %.*s is %d px, room for %d\n",
               line, c.width(), c.height(), static_cast<int>(what.size()),
               what.data(), w, max_w);
}
#define CHECK_FITS(w, max_w, what, c)                                          \
  check_fits((w), (max_w), (what), (c), __LINE__)

// True when `cp` is an Arabic letter, mark or presentation form, the
// codepoints only the Cairo fallback holds.
bool arabic(char32_t cp) {
  return (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
         (cp >= 0xFE70 && cp <= 0xFEFF);
}

void test_arabic_strings_and_glyphs() {
  // Every id has both languages, and the Arabic is not the English copied.
  int same = 0;
  for (std::size_t i = 0; i < opensensor::text::kStringCount; ++i) {
    const auto id = static_cast<Str>(i);
    CHECK(!opensensor::text::text(opensensor::text::Lang::En, id).empty());
    CHECK(!opensensor::text::text(opensensor::text::Lang::ArEg, id).empty());
    same += opensensor::text::text(opensensor::text::Lang::En, id) ==
                    opensensor::text::text(opensensor::text::Lang::ArEg, id)
                ? 1
                : 0;
  }
  CHECK(same < static_cast<int>(opensensor::text::kStringCount) / 2);

  // Every codepoint an Arabic string draws, after shaping into its
  // contextual forms, is baked. Arabic ones in every font a page draws
  // words with, through the fallback, the rest in the small and label
  // fonts every string can land in.
  for (std::size_t i = 0; i < opensensor::text::kStringCount; ++i) {
    const std::string_view s = opensensor::text::text(
        opensensor::text::Lang::ArEg, static_cast<Str>(i));
    std::array<char32_t, opensensor::text::kMaxLine> line{};
    const auto vis =
        opensensor::text::to_visual(s, line, opensensor::text::Direction::Rtl,
                                    opensensor::text::OnMalformed::Reject);
    CHECK(vis.has_value());
    if (!vis)
      continue;
    for (const FontSet *fs : {&fonts::compact, &fonts::large}) {
      for (const char32_t cp : *vis) {
        const Font *used = nullptr;
        if (arabic(cp)) {
          for (const Font *f : {&fs->small, &fs->label, &fs->title, &fs->value})
            CHECK(find_glyph_or_fallback(*f, cp, used) != nullptr);
        } else {
          for (const Font *f : {&fs->small, &fs->label})
            CHECK(find_glyph_or_fallback(*f, cp, used) != nullptr);
        }
      }
    }
  }
}

void test_language_switch() {
  set_language(Language::English);
  CHECK(language() == Language::English && !rtl());
  CHECK(tr(Str::SettingsTitle) == "Settings");
  set_language(Language::Arabic);
  CHECK(language() == Language::Arabic && rtl());
  CHECK(
      tr(Str::SettingsTitle) ==
      opensensor::text::text(opensensor::text::Lang::ArEg, Str::SettingsTitle));
  CHECK(std::string_view{tr_fmt(Str::AltitudeFmt)} ==
        opensensor::text::text(opensensor::text::Lang::ArEg, Str::AltitudeFmt));
  // The Language row toggles, saves and redraws the whole frame.
  set_language(Language::English);
  Settings s{};
  UiState ui{};
  static_cast<void>(handle(ui, s, kAllThere, Input::Menu));
  ui.popup.selected = Row::Language;
  Outcome o = handle(ui, s, kAllThere, Input::Press);
  CHECK(o.effect == Effect::Changed && o.full_refresh);
  CHECK(s.language == Language::Arabic && ui.popup.open);
  // render_screen() takes the language from the settings.
  {
    auto h = std::make_unique<History>();
    Buffer b;
    render_screen(b.canvas, ui, s, kAllThere, Model{}, *h);
    CHECK(language() == Language::Arabic);
  }
  o = handle(ui, s, kAllThere, Input::Press);
  CHECK(o.effect == Effect::Changed && o.full_refresh);
  CHECK(s.language == Language::English);
  // The value names each language in itself, whatever the panel shows.
  std::array<char, 24> buf{};
  PopupState p{};
  set_language(Language::Arabic);
  s.language = Language::English;
  CHECK(row_value(buf, Row::Language, s, p, kAllThere) == "English");
  set_language(Language::English);
  s.language = Language::Arabic;
  CHECK(row_value(buf, Row::Language, s, p, kAllThere) ==
        opensensor::text::text(opensensor::text::Lang::ArEg, Str::LangArabic));
  CHECK(row_label(Row::Language) == "Language");
}

// The Arabic words fit where the pages put them, at every size class, in
// landscape and portrait.
// The storage fault page draws inside the frame at every size class and
// rotation, and leaves the active language alone (#177).
void test_storage_fault() {
  for (const Language lang : {Language::English, Language::Arabic}) {
    for (const DisplayProfile *prof : {&kHost400x300, &kHost800x480}) {
      for (const Rotation r : {Rotation::R0, Rotation::R90}) {
        set_language(lang);
        Buffer b(*prof, r);
        b.canvas.fill({0, 0, b.canvas.width(), b.canvas.height()});
        render_storage_fault(b.canvas, 4365);
        CHECK(language() == lang);
        const int w = b.canvas.width();
        const int h = b.canvas.height();
        CHECK(ink_count(b.canvas, {0, 0, w, h}) > 500);
        // Nothing reaches the edge rows, so no line clipped off the frame.
        CHECK(ink_count(b.canvas, {0, 0, w, 2}) == 0);
        CHECK(ink_count(b.canvas, {0, h - 2, w, 2}) == 0);
        // The same page whatever language was active.
        Buffer other(*prof, r);
        set_language(lang == Language::English ? Language::Arabic
                                               : Language::English);
        render_storage_fault(other.canvas, 4365);
        CHECK(other.bytes == b.bytes);
        // The error code is drawn.
        Buffer code(*prof, r);
        render_storage_fault(code.canvas, 261);
        CHECK(code.bytes != b.bytes);
      }
    }
  }
  set_language(Language::English);
}

// The Thing id fault page (#225) draws the same storage fault layout with a
// code line naming the stage and the error, inside the frame at every size
// class and rotation, the long code wrapped at its spaces.
void test_thing_id_fault() {
  for (const Language lang : {Language::English, Language::Arabic}) {
    for (const DisplayProfile *prof : {&kHost400x300, &kHost800x480}) {
      for (const Rotation r : {Rotation::R0, Rotation::R90}) {
        for (const char *stage : {"open", "read", "write", "format"}) {
          for (const int error : {0, 4354, -2147483647 - 1}) {
            set_language(lang);
            Buffer b(*prof, r);
            b.canvas.fill({0, 0, b.canvas.width(), b.canvas.height()});
            render_thing_id_fault(b.canvas, stage, error);
            CHECK(language() == lang);
            const int w = b.canvas.width();
            const int h = b.canvas.height();
            CHECK(ink_count(b.canvas, {0, 0, w, h}) > 500);
            CHECK(ink_count(b.canvas, {0, 0, w, 2}) == 0);
            CHECK(ink_count(b.canvas, {0, h - 2, w, 2}) == 0);
            CHECK(ink_count(b.canvas, {0, 0, 2, h}) == 0);
            CHECK(ink_count(b.canvas, {w - 2, 0, 2, h}) == 0);
            Buffer other(*prof, r);
            set_language(lang == Language::English ? Language::Arabic
                                                   : Language::English);
            render_thing_id_fault(other.canvas, stage, error);
            CHECK(other.bytes == b.bytes);
          }
        }
        // The stage, the error and the page itself are drawn.
        Buffer read(*prof, r), write(*prof, r), code(*prof, r), nvs(*prof, r);
        render_thing_id_fault(read.canvas, "read", 4355);
        render_thing_id_fault(write.canvas, "write", 4355);
        render_thing_id_fault(code.canvas, "read", 4364);
        render_storage_fault(nvs.canvas, 4355);
        CHECK(read.bytes != write.bytes);
        CHECK(read.bytes != code.bytes);
        CHECK(read.bytes != nvs.bytes);
        // A missing stage name still draws a page.
        Buffer none(*prof, r);
        render_thing_id_fault(none.canvas, nullptr, 0);
        CHECK(ink_count(none.canvas, {0, 0, none.canvas.width(),
                                      none.canvas.height()}) > 500);
      }
    }
  }
  set_language(Language::English);
}

void test_arabic_fit() {
  // Every value a row can show, across settings and boards.
  std::vector<std::pair<Settings, SettingsInfo>> variants;
  for (std::uint8_t k = 0; k < 5; ++k) {
    Settings s{};
    s.refresh = static_cast<RefreshEvery>(std::min<std::uint8_t>(k, 3));
    s.start_page = k % 2 == 0 ? StartPage::Dashboard : StartPage::LastViewed;
    s.theme = k % 2 == 0 ? Theme::Light : Theme::Dark;
    s.rotation = static_cast<ScreenRotation>(k);
    s.language = k % 2 == 0 ? Language::English : Language::Arabic;
    s.co2_asc = k % 2 == 0;
    s.altitude_m = k % 2 == 0 ? 0 : kAltitudeMaxM;
    s.temp_offset_dc = k % 2 == 0 ? kTempOffsetMinDc : kTempOffsetMaxDc;
    s.sleep_after = static_cast<SleepAfter>(std::min<std::uint8_t>(k, 3));
    SettingsInfo info{.wifi_configured = k != 2,
                      .wifi_on = k % 2 == 1,
                      .sensor_present = k != 3,
                      .fan_cleaning = k == 1};
    variants.emplace_back(s, info);
  }
  constexpr std::array<Str, 10> kStatusRows{
      Str::RowClock,     Str::RowUptime,     Str::RowSamples, Str::RowSdCard,
      Str::RowBluetooth, Str::RowWifi,       Str::RowThing,   Str::RowFirmware,
      Str::RowSensor,    Str::RowSensorState};
  constexpr std::array<Str, 5> kHints{Str::HintEdit, Str::HintWifiSet,
                                      Str::HintWifiUnset, Str::HintCo2Asc,
                                      Str::HintPick};
  for (const Language lang : {Language::English, Language::Arabic}) {
    set_language(lang);
    for (const DisplayProfile *prof : {&kHost400x300, &kHost800x480}) {
      for (const Rotation r : {Rotation::R0, Rotation::R90}) {
        Buffer b(*prof, r);
        const Canvas &c = b.canvas;
        const Layout &l = c.layout();
        const FontSet &fs = c.fonts();
        // The popup, a label and its value with a gap between them.
        const int row_w = l.popup.w - 2 * l.popup.pad;
        for (std::uint8_t i = 0; i < kRowCount; ++i) {
          const auto row = static_cast<Row>(i);
          const std::string_view label = row_label(row);
          for (const auto &[s, info] : variants) {
            std::array<char, 24> buf{};
            const PopupState p{};
            const std::string_view v = row_value(buf, row, s, p, info);
            CHECK_FITS(text_width(fs.label, label) + 8 +
                           text_width(fs.label, v),
                       row_w, label, c);
          }
        }
        CHECK_FITS(text_width(fs.title, tr(Str::SettingsTitle)), row_w,
                   tr(Str::SettingsTitle), c);
        // A hint is one line on a landscape frame and at most two on a
        // portrait one.
        for (const Str id : kHints) {
          const std::string_view hint = tr(id);
          if (c.width() > c.height()) {
            CHECK_FITS(text_width(fs.small, hint), row_w, hint, c);
          } else {
            const std::size_t cut = line_break(fs.small, hint, row_w);
            std::string_view rest = hint.substr(cut);
            while (!rest.empty() && rest.front() == ' ')
              rest.remove_prefix(1);
            CHECK_FITS(text_width(fs.small, rest), row_w, hint, c);
          }
        }
        // A footer is one line on a landscape frame and at most two on a
        // portrait one, the layout has room for no more.
        const int edge = l.header.margin;
        for (const auto &[id, max_w] :
             {std::pair{Str::BandFootnote, c.width() - l.page.title.x - edge},
              std::pair{Str::PairFootnote, c.width() - 2 * edge}}) {
          const std::string_view foot = tr(id);
          if (c.width() > c.height()) {
            CHECK_FITS(text_width(fs.small, foot), max_w, foot, c);
          } else {
            std::string_view rest =
                foot.substr(line_break(fs.small, foot, max_w));
            while (!rest.empty() && rest.front() == ' ')
              rest.remove_prefix(1);
            CHECK_FITS(text_width(fs.small, rest), max_w, foot, c);
          }
        }
        CHECK_FITS(text_width(fs.title, tr(Str::PairTitle)),
                   c.width() - 2 * edge, tr(Str::PairTitle), c);
        CHECK_FITS(text_width(fs.label, tr(Str::PairPrompt)),
                   c.width() - 2 * edge, tr(Str::PairPrompt), c);
        // The storage fault page (#177). The title and the off line are one
        // line each, the help at most two.
        CHECK_FITS(text_width(fs.title, tr(Str::StorageFaultTitle)),
                   c.width() - 2 * edge, tr(Str::StorageFaultTitle), c);
        CHECK_FITS(text_width(fs.label, tr(Str::StorageFaultOff)),
                   c.width() - 2 * edge, tr(Str::StorageFaultOff), c);
        {
          const std::string_view help = tr(Str::StorageFaultHelp);
          std::string_view rest =
              help.substr(line_break(fs.label, help, c.width() - 2 * edge));
          while (!rest.empty() && rest.front() == ' ')
            rest.remove_prefix(1);
          CHECK_FITS(text_width(fs.label, rest), c.width() - 2 * edge, help, c);
        }
        // A status name ends before its value starts.
        for (const Str id : kStatusRows)
          CHECK_FITS(text_width(fs.small, tr(id)) + 4,
                     l.status.value_x - l.page.title.x, tr(id), c);
        // A tile label and its unit share the tile's top line.
        for (std::size_t m = 0; m < kMetricCount; ++m) {
          const MetricInfo &mi = info(static_cast<Metric>(m));
          CHECK_FITS(text_width(fs.label, tr(mi.label)) + 6 +
                         text_width(fs.small, tr(mi.unit)),
                     l.tiles.w - 2 * l.tiles.pad, tr(mi.label), c);
        }
      }
    }
  }
  set_language(Language::English);
  CHECK(g_misfits == 0);
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
  test_battery_header();
  test_pairing_screen();
  test_settings_blob();
  test_popup_rows();
  test_popup_numeric();
  test_ui_handle();
  test_render_settings_and_sleep();
  test_theme();
  test_phone_keys();
  test_restore_order();
  test_profile();
  test_crowpanel_profile();
  test_epaper397_profile();
  test_policy_caps();
  test_rotate();
  test_changed_window();
  test_presenter();
  test_orientation();
  test_effective_rotation();
  test_line_break();
  test_render_rotations();
  test_arabic_strings_and_glyphs();
  test_language_switch();
  test_arabic_fit();
  test_storage_fault();
  test_thing_id_fault();
  std::printf("epaper ui, %d checks passed\n", g_checks);
  return 0;
}
