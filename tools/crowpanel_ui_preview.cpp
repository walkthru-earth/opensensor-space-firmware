// Renders the CrowPanel pages for synthetic scenarios to PBM files, the same
// 1 bit frames the panel receives. tools/test_crowpanel_ui.py turns them into
// PNGs under the board's ignored artifacts/ui-preview/.
//
// Usage: crowpanel-ui-preview <out dir>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>

#include "canvas.h"
#include "history.h"
#include "pages.h"
#include "settings.h"
#include "ui_model.h"

using namespace opensensor::ui;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

struct Buffer {
  std::array<std::uint8_t, kFrameBytes> bytes{};
  Canvas canvas{bytes};
};

bool write_pbm(const std::string &path, const Buffer &b) {
  std::FILE *f = std::fopen(path.c_str(), "wb");
  if (f == nullptr)
    return false;
  std::fprintf(f, "P4\n%d %d\n", kWidth, kHeight);
  const bool ok =
      std::fwrite(b.bytes.data(), 1, b.bytes.size(), f) == b.bytes.size();
  return std::fclose(f) == 0 && ok;
}

// A small deterministic generator so every run draws the same pictures.
struct Lcg {
  std::uint32_t s = 12345;
  float next() {
    s = s * 1664525U + 1013904223U;
    return static_cast<float>(s >> 8) / static_cast<float>(1U << 24);
  }
};

// Plausible indoor air over a day, a cooking spike, a night time CO2 build
// up and a window opened in the morning.
std::array<float, kMetricCount> day_sample(std::uint32_t t, Lcg &rng) {
  const float hours = static_cast<float>(t) / 3600.0F;
  const float phase = hours / 24.0F * 6.2831853F;
  float pm25 = 6.0F + 3.0F * std::sin(phase) + rng.next() * 2.0F;
  const float cook = hours - 19.0F;
  if (cook > 0.0F && cook < 1.5F)
    pm25 += 48.0F * std::exp(-cook * 2.5F);
  float co2 = 520.0F + 380.0F * std::max(0.0F, std::sin(phase - 1.2F)) +
              rng.next() * 25.0F;
  if (hours > 7.0F && hours < 7.5F)
    co2 -= 250.0F;
  const float temp = 22.5F + 1.6F * std::sin(phase - 0.8F) + rng.next() * 0.1F;
  const float rh = 46.0F - 6.0F * std::sin(phase - 0.8F) + rng.next();
  return {pm25 * 0.72F, pm25, pm25 * 1.08F, pm25 * 1.15F + 2.0F, co2, temp, rh};
}

void fill_now(Model &m, const std::array<float, kMetricCount> &v,
              std::uint32_t since_start) {
  for (std::size_t i = 0; i < kMetricCount; ++i) {
    const auto metric = static_cast<Metric>(i);
    m.now[i] = quality_for(
        metric, std::isfinite(v[i]) ? std::optional<float>{v[i]} : std::nullopt,
        since_start);
  }
}

Status base_status() {
  return {.utc_s = 1795437120, // 2026-11-23 10:32 UTC
          .sd_ok = true,
          .sd_free_bytes = 7'214'000'000ULL,
          .ble = Link::Connected,
          .wifi = Link::Waiting,
          .samples = 8640,
          .uptime_s = 86400 + 3 * 3600 + 17 * 60,
          .firmware = "opensensor-crowpanel 0.1.0",
          .thing = "OS-7F3A2C",
          .sensor_serial = "A1B2C3D4E5F60718",
          .sensor_fw = "2.0",
          .sensor_status = 0U,
          .sensor_present = true,
          .pairing_passkey = std::nullopt,
          .device_name = "OS-7F3A2C"};
}

int emit(const std::string &dir, const char *name, std::uint8_t page,
         const Model &m, const History &h) {
  auto b = std::make_unique<Buffer>();
  render(b->canvas, page, m, h);
  const std::string path = dir + "/" + name + ".pbm";
  if (!write_pbm(path, *b)) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return 1;
  }
  std::printf("%s\n", path.c_str());
  return 0;
}

int emit_screen(const std::string &dir, const char *name, const UiState &ui,
                const Settings &s, const SettingsInfo &info, const Model &m,
                const History &h) {
  auto b = std::make_unique<Buffer>();
  render_screen(b->canvas, ui, s, info, m, h);
  const std::string path = dir + "/" + name + ".pbm";
  if (!write_pbm(path, *b)) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return 1;
  }
  std::printf("%s\n", path.c_str());
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <out dir>\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  int rc = 0;

  // A full day, now is 20:00 after the cooking spike.
  {
    auto h = std::make_unique<History>();
    Lcg rng;
    const std::uint32_t end = 20 * 3600;
    std::array<float, kMetricCount> last{};
    for (std::uint32_t t = 0; t <= end; t += 10) {
      last = day_sample(t, rng);
      h->push(t, last);
    }
    Model m{};
    m.status = base_status();
    fill_now(m, last, end);
    rc |= emit(dir, "dashboard", kDashboardPage, m, *h);
    for (std::uint8_t p = 1; p < kStatusPage; ++p) {
      const std::string name =
          "chart-" + std::string(info(kChartOrder[p - 1U]).label == "CO₂"
                                     ? "co2"
                                     : info(kChartOrder[p - 1U]).label);
      rc |= emit(dir, name.c_str(), p, m, *h);
    }
    rc |= emit(dir, "status", kStatusPage, m, *h);

    // The same day with bad air right now.
    std::array<float, kMetricCount> bad = last;
    bad[index(Metric::Pm25)] = 142.6F;
    h->push(end + 10, bad);
    fill_now(m, bad, end + 10);
    rc |= emit(dir, "dashboard-bad-air", kDashboardPage, m, *h);

    // The settings popup over the dashboard, then editing the altitude.
    const SettingsInfo info{.wifi_configured = true,
                            .wifi_on = true,
                            .sensor_present = true,
                            .fan_cleaning = false};
    Settings s{};
    s.refresh = RefreshEvery::S30;
    s.altitude_m = 300;
    s.temp_offset_dc = -15;
    s.sleep_after = SleepAfter::M30;
    UiState ui{};
    static_cast<void>(handle(ui, s, info, Input::Menu));
    rc |= emit_screen(dir, "settings", ui, s, info, m, *h);
    for (int i = 0; i < 5; ++i)
      static_cast<void>(handle(ui, s, info, Input::Down));
    static_cast<void>(handle(ui, s, info, Input::Press));
    static_cast<void>(handle(ui, s, info, Input::Up));
    rc |= emit_screen(dir, "settings-edit-altitude", ui, s, info, m, *h);

    // The screen asleep after 30 minutes idle.
    fall_asleep(ui);
    rc |= emit_screen(dir, "asleep", ui, s, info, m, *h);
  }

  // Just booted, 12 s in, no clock, PM and CO2 still warming.
  {
    auto h = std::make_unique<History>();
    Lcg rng;
    std::array<float, kMetricCount> v{};
    for (std::uint32_t t = 0; t <= 10; t += 10) {
      v = day_sample(t, rng);
      v[index(Metric::Co2)] = kNaN;
      for (const Metric pm :
           {Metric::Pm1, Metric::Pm25, Metric::Pm4, Metric::Pm10})
        v[index(pm)] = kNaN;
      h->push(t, v);
    }
    Model m{};
    m.status = base_status();
    m.status.utc_s = std::nullopt;
    m.status.ble = Link::Waiting;
    m.status.wifi = Link::Off;
    m.status.samples = 2;
    m.status.uptime_s = 12;
    m.status.sensor_status = std::nullopt;
    fill_now(m, v, 12);
    rc |= emit(dir, "dashboard-warming", kDashboardPage, m, *h);
    rc |= emit(dir, "chart-warming", 1, m, *h);
    rc |= emit(dir, "status-warming", kStatusPage, m, *h);
  }

  // Two hours in, the sensor dropped out for 15 minutes and is back, the SD
  // card is missing and the sensor reports a fan speed warning.
  {
    auto h = std::make_unique<History>();
    Lcg rng;
    const std::uint32_t end = 2 * 3600;
    std::array<float, kMetricCount> v{};
    for (std::uint32_t t = 0; t <= end; t += 10) {
      v = day_sample(t + 9 * 3600, rng);
      if (t > end - 40 * 60 && t < end - 25 * 60) {
        h->advance(t);
        continue;
      }
      h->push(t, v);
    }
    Model m{};
    m.status = base_status();
    m.status.sd_ok = false;
    m.status.wifi = Link::Connected;
    m.status.sensor_status = (1U << 21) | (1U << 4);
    fill_now(m, v, 5 * 60);
    m.now[index(Metric::Co2)] = quality_for(Metric::Co2, std::nullopt, 300U);
    rc |= emit(dir, "dashboard-gap", kDashboardPage, m, *h);
    rc |= emit(dir, "chart-gap", 1, m, *h);
    rc |= emit(dir, "status-fault", kStatusPage, m, *h);
  }

  // No sensor on the bus at all.
  {
    auto h = std::make_unique<History>();
    Model m{};
    m.status = base_status();
    m.status.sensor_present = false;
    m.status.sensor_status = std::nullopt;
    fill_now(m, {kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN}, 0);
    std::ranges::fill(
        m.now,
        Reading{.value = 0.0F, .quality = Quality::Absent, .warm_left_s = 0});
    rc |= emit(dir, "dashboard-no-sensor", kDashboardPage, m, *h);
    rc |= emit(dir, "chart-empty", 2, m, *h);

    // Settings on a Thing with no Wi-Fi set up and no sensor, the Wi-Fi row
    // selected, over the status page.
    const SettingsInfo info{.wifi_configured = false,
                            .wifi_on = false,
                            .sensor_present = false,
                            .fan_cleaning = false};
    Settings s{};
    s.co2_asc = false;
    s.start_page = StartPage::LastViewed;
    UiState ui{.page = kStatusPage};
    static_cast<void>(handle(ui, s, info, Input::Menu));
    for (int i = 0; i < 2; ++i)
      static_cast<void>(handle(ui, s, info, Input::Down));
    rc |= emit_screen(dir, "settings-no-wifi-no-sensor", ui, s, info, m, *h);
  }

  // A phone pairs. The passkey here is a fixed example, never a real one.
  {
    auto h = std::make_unique<History>();
    Model m{};
    m.status = base_status();
    m.status.ble = Link::Connected;
    m.status.pairing_passkey = 123456U;
    rc |= emit(dir, "pairing", kDashboardPage, m, *h);
  }
  return rc;
}
