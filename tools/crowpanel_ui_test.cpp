// Host tests for the CrowPanel e-paper UI, canvas clipping, UTF-8, history
// rings, chart gaps, PM2.5 bands, reading quality, navigation and the refresh
// cadence. Built and run by tools/test_crowpanel_ui.py with sanitizers.

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string_view>

#include "air_band.h"
#include "canvas.h"
#include "chart.h"
#include "history.h"
#include "pages.h"
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
  const Series s = h->series(Metric::Pm25, Window::Hour);
  CHECK(s.size() == History::kHourSlots);
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

  // Same slot replaces in the hour ring and averages in the day ring.
  h->push(1065, all(7.0F));
  CHECK(h->series(Metric::Rh, Window::Hour)[n - 1] == 7.0F);
  CHECK(h->series(Metric::Rh, Window::Day)[d.size() - 1] == 6.0F);
  // Time going backwards is ignored.
  h->push(900, all(99.0F));
  CHECK(h->series(Metric::Rh, Window::Hour)[n - 1] == 7.0F);

  // Minute 18 has only NaN, minutes 19 and 20 have no push at all.
  h->push(1080, all(kNaN));
  h->advance(1200);
  const Series d2 = h->series(Metric::Temp, Window::Day);
  CHECK(std::isnan(d2[d2.size() - 1]));
  CHECK(std::isnan(d2[d2.size() - 2]));
  CHECK(std::isnan(d2[d2.size() - 3]));
  CHECK(d2[d2.size() - 4] == 6.0F);

  // A gap longer than the window clears it.
  h->push(1200 + 2 * 3600, all(3.0F));
  const Series s3 = h->series(Metric::Pm1, Window::Hour);
  int finite = 0;
  for (std::size_t i = 0; i < s3.size(); ++i)
    finite += std::isfinite(s3[i]) ? 1 : 0;
  CHECK(finite == 1);
  // Wrapping past the ring end keeps order oldest first.
  auto w = std::make_unique<History>();
  for (std::uint32_t t = 0; t < 2 * 3600; t += 10)
    w->push(t, all(static_cast<float>(t)));
  const Series s4 = w->series(Metric::Pm10, Window::Hour);
  CHECK(s4[s4.size() - 1] == 7190.0F);
  CHECK(s4[0] == 7190.0F - 10.0F * (History::kHourSlots - 1));
}

void test_chart() {
  const std::array<float, 3> nan3{kNaN, kNaN, kNaN};
  CHECK(!data_range({.ring = nan3, .start = 0}).valid);
  const std::array<float, 4> some{3.0F, kNaN, -1.0F, 8.0F};
  const Range r = data_range({.ring = some, .start = 1});
  CHECK(r.valid && r.lo == -1.0F && r.hi == 8.0F);

  Ticks t = nice_ticks(0.0F, 37.3F, 5);
  CHECK(t.lo == 0.0F && t.hi == 40.0F && t.step == 10.0F && t.count == 5);
  t = nice_ticks(412.0F, 1290.0F, 6);
  CHECK(t.lo <= 412.0F && t.hi >= 1290.0F && t.count <= 6);
  t = nice_ticks(21.0F, 21.0F, 4);
  CHECK(t.lo < 21.0F && t.hi > 21.0F && t.count >= 2);
  t = nice_ticks(20.4F, 21.7F, 5);
  CHECK(t.decimals == 1 && t.step == 0.5F);

  // A NaN in the middle leaves its column blank and breaks the line.
  auto b = std::make_unique<Buffer>();
  const std::array<float, 5> gap{1.0F, 1.0F, kNaN, 1.0F, 1.0F};
  plot_series(b->canvas, {100, 100, 5, 10}, {.ring = gap, .start = 0}, 0.0F,
              2.0F);
  CHECK(ink_count(b->canvas, {102, 90, 1, 30}) == 0);
  CHECK(ink_count(b->canvas, {100, 90, 2, 30}) == 2);
  // A lone point draws a 3 x 3 dot.
  b->canvas.clear();
  const std::array<float, 5> lone{kNaN, kNaN, 1.0F, kNaN, kNaN};
  plot_series(b->canvas, {100, 100, 5, 10}, {.ring = lone, .start = 0}, 0.0F,
              2.0F);
  CHECK(ink_count(b->canvas, {90, 90, 30, 30}) == 9);
  // The envelope keeps a single spike among 1440 points on 344 columns.
  b->canvas.clear();
  std::array<float, 1440> day{};
  day.fill(1.0F);
  day[777] = 100.0F;
  const Rect plot{40, 50, 344, 100};
  plot_series(b->canvas, plot, {.ring = day, .start = 0}, 0.0F, 100.0F);
  CHECK(ink_count(b->canvas, {plot.x, plot.y, plot.w, 1}) >= 1);
  // A whole hour of NaN in a day draws a clean gap.
  b->canvas.clear();
  for (std::size_t i = 600; i < 660; ++i)
    day[i] = kNaN;
  day[777] = 1.0F;
  plot_series(b->canvas, plot, {.ring = day, .start = 0}, 0.0F, 2.0F);
  const int gx = plot.x + static_cast<int>((630 * 344) / 1440);
  CHECK(ink_count(b->canvas, {gx - 3, plot.y, 7, plot.h}) == 0);
  CHECK(ink_count(b->canvas, {plot.x, plot.y, 20, plot.h}) > 0);
  // An empty chart says so instead of drawing a line.
  b->canvas.clear();
  line_chart(b->canvas, plot, {.ring = nan3, .start = 0}, {});
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
  plot_series(b->canvas, spark, {.ring = hour, .start = 0}, 0.0F, 359.0F,
              Detail::Mean);
  bool one_per_column = true;
  for (int x = spark.x; x < spark.right(); ++x)
    one_per_column = one_per_column &&
                     ink_count(b->canvas, {x, spark.y, 1, spark.h}) >= 1 &&
                     ink_count(b->canvas, {x, spark.y, 1, spark.h}) <= 3;
  CHECK(one_per_column);
  b->canvas.clear();
  for (std::size_t i = 90; i < 180; ++i)
    hour[i] = kNaN;
  plot_series(b->canvas, spark, {.ring = hour, .start = 0}, 0.0F, 359.0F,
              Detail::Mean);
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
  CHECK(navigate(2, Input::Menu).page == kStatusPage);
  CHECK(navigate(kStatusPage, Input::Menu).page == kDashboardPage);
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
                 .sensor_present = true};
  for (std::uint32_t t = 0; t < 3 * 3600; t += 10)
    h->push(t, all(static_cast<float>(t % 500)));
  for (std::uint8_t p = 0; p < kPageCount; ++p)
    render(b->canvas, p, full, *h);
  full.status.utc_s = -1;
  render(b->canvas, kStatusPage, full, *h);
}

} // namespace

int main() {
  test_canvas();
  test_utf8_and_text();
  test_history();
  test_chart();
  test_bands();
  test_quality_and_format();
  test_navigation();
  test_render_all_pages();
  std::printf("crowpanel ui, %d checks passed\n", g_checks);
  return 0;
}
