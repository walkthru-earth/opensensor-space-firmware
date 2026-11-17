// Host tests for the CrowPanel e-paper UI, canvas clipping, UTF-8, history
// rings, chart gaps, PM2.5 bands, reading quality, navigation, the refresh
// cadence, the settings blob and popup and the sleeping screen. Built and run
// by tools/test_crowpanel_ui.py with sanitizers.

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "air_band.h"
#include "canvas.h"
#include "chart.h"
#include "history.h"
#include "pages.h"
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

  const SettingsBlob db = encode(d);
  CHECK(db[0] == kSettingsVersion);
  CHECK(decode(db) == d);

  const Settings s{.refresh = RefreshEvery::M5,
                   .start_page = StartPage::LastViewed,
                   .last_page = kStatusPage,
                   .co2_asc = false,
                   .altitude_m = 2950,
                   .temp_offset_dc = -73,
                   .sleep_after = SleepAfter::H1};
  const SettingsBlob b = encode(s);
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
  // A longer blob reads its first ten bytes.
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
    CHECK(handle(ui, s, info, Input::Press).effect == Effect::Changed);
    CHECK(s.refresh == RefreshEvery::S30);
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
  test_pairing_screen();
  test_settings_blob();
  test_popup_rows();
  test_popup_numeric();
  test_ui_handle();
  test_render_settings_and_sleep();
  std::printf("crowpanel ui, %d checks passed\n", g_checks);
  return 0;
}
