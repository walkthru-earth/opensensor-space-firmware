// Renders the shared e-paper pages for synthetic scenarios to PBM files, at
// each host size class and each rotation, under <out dir>/<size>/r<deg>/ in
// English and under <out dir>/ar/<size>/r<deg>/ in Egyptian Arabic, with the
// same file names.
// Each file is the frame as a person holding the device sees it, the
// logical frame the pages draw. At r0 that is byte for byte the frame the
// panel receives, at other rotations the Presenter turns it for the panel,
// which the host test checks pixel by pixel. tools/test_epaper_ui.py hashes
// the 400x300/r0 frames against the CrowPanel goldens and turns all of them
// into PNGs under the ignored root artifacts/epaper-ui-preview/. Names ending
// in -dark are the same scenes with Theme::Dark.
//
// Usage: epaper-ui-preview <out dir>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "epaper_ui/canvas.h"
#include "epaper_ui/history.h"
#include "epaper_ui/orientation.h"
#include "epaper_ui/pages.h"
#include "epaper_ui/profile.h"
#include "epaper_ui/settings.h"
#include "epaper_ui/strings.h"
#include "epaper_ui/ui_model.h"
#include "epaper_ui_host_profiles.h"
#include "os_strings.h"

using namespace opensensor::ui;

namespace {

// The language every scene draws in, set once per pass in main().
constinit Language g_language = Language::English;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// Where the frames of one size class and rotation go.
struct Target {
  std::string dir;
  const DisplayProfile *profile = nullptr;
  Rotation rotation;
};

Canvas must(std::expected<Canvas, CanvasError> c) {
  if (!c) {
    std::fprintf(stderr, "canvas %s\n", to_string(c.error()));
    std::exit(1);
  }
  return *c;
}

// A logical frame for the target, white.
struct Buffer {
  explicit Buffer(const Target &t)
      : bytes(t.profile->logical_frame_bytes()),
        canvas(must(Canvas::create(bytes, *t.profile, t.rotation))) {}
  std::vector<std::uint8_t> bytes;
  Canvas canvas;
};

bool write_pbm(const std::string &path, const Canvas &c) {
  std::FILE *f = std::fopen(path.c_str(), "wb");
  if (f == nullptr)
    return false;
  std::fprintf(f, "P4\n%d %d\n", c.width(), c.height());
  const auto bytes = c.frame().first(static_cast<std::size_t>(c.stride()) *
                                     static_cast<std::size_t>(c.height()));
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
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
          .sensor_model = "SEN63C",
          .sensor_serial = "A1B2C3D4E5F60718",
          .sensor_fw = "2.0",
          .sensor_status = 0U,
          .sensor_present = true,
          .pairing_passkey = std::nullopt,
          .device_name = "OS-7F3A2C",
          .battery = std::nullopt};
}

int emit(const Target &t, const char *name, std::uint8_t page, const Model &m,
         const History &h) {
  auto b = std::make_unique<Buffer>(t);
  set_language(g_language);
  render(b->canvas, page, m, h);
  const std::string path = t.dir + "/" + name + ".pbm";
  if (!write_pbm(path, b->canvas)) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return 1;
  }
  std::printf("%s\n", path.c_str());
  return 0;
}

int emit_screen(const Target &t, const char *name, const UiState &ui,
                const Settings &s, const SettingsInfo &info, const Model &m,
                const History &h) {
  auto b = std::make_unique<Buffer>(t);
  Settings drawn = s;
  drawn.language = g_language;
  render_screen(b->canvas, ui, drawn, info, m, h);
  const std::string path = t.dir + "/" + name + ".pbm";
  if (!write_pbm(path, b->canvas)) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return 1;
  }
  std::printf("%s\n", path.c_str());
  return 0;
}

// The settings storage fault page a board draws when NVS cannot start
// (#177). It draws both languages whatever g_language is.
int emit_storage_fault(const Target &t) {
  auto b = std::make_unique<Buffer>(t);
  set_language(g_language);
  render_storage_fault(b->canvas, 4365);
  const std::string path = t.dir + "/storage-fault.pbm";
  if (!write_pbm(path, b->canvas)) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return 1;
  }
  std::printf("%s\n", path.c_str());
  return 0;
}

// The same page when the Thing id cannot load (#225), here a stored value
// of another type.
int emit_thing_id_fault(const Target &t) {
  auto b = std::make_unique<Buffer>(t);
  set_language(g_language);
  render_thing_id_fault(b->canvas, "read", 4355);
  const std::string path = t.dir + "/thing-id-fault.pbm";
  if (!write_pbm(path, b->canvas)) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return 1;
  }
  std::printf("%s\n", path.c_str());
  return 0;
}

// The page through render_screen() with the dark theme, as the panel draws
// it once Theme is set to Dark.
int emit_dark(const Target &t, const char *name, std::uint8_t page,
              const Model &m, const History &h) {
  Settings s{};
  s.theme = Theme::Dark;
  return emit_screen(t, name, UiState{.page = page}, s, SettingsInfo{}, m, h);
}

// The Rotation setting that shows this target's rotation, so the popup
// names the turn the frame is drawn at.
ScreenRotation setting_for(Rotation r) {
  return static_cast<ScreenRotation>(std::to_underlying(r));
}

int scenes(const Target &target) {
  int rc = 0;
  const bool moving = target.profile->orientation.present;

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
    rc |= emit(target, "dashboard", kDashboardPage, m, *h);
    rc |= emit_dark(target, "dashboard-dark", kDashboardPage, m, *h);
    rc |= emit_dark(target, "chart-PM2.5-dark", 1, m, *h);
    for (std::uint8_t p = 1; p < kStatusPage; ++p) {
      // Named by the English label in both languages.
      const std::string_view label = opensensor::text::text(
          opensensor::text::Lang::En, info(kChartOrder[p - 1U]).label);
      const std::string name =
          "chart-" + std::string(label == "CO₂" ? "co2" : label);
      rc |= emit(target, name.c_str(), p, m, *h);
    }
    rc |= emit(target, "status", kStatusPage, m, *h);
    rc |= emit_dark(target, "status-dark", kStatusPage, m, *h);

    // A board with a battery, the header gains the icon and the percent. A
    // charging one shows the plus, and an empty one only the outline.
    {
      Model bm = m;
      bm.status.battery = Battery{.percent = 87, .charging = false};
      rc |= emit(target, "dashboard-battery", kDashboardPage, bm, *h);
      rc |= emit_dark(target, "dashboard-battery-dark", kDashboardPage, bm, *h);
      bm.status.battery = Battery{.percent = 42, .charging = true};
      rc |= emit(target, "status-battery-charging", kStatusPage, bm, *h);
      bm.status.battery = Battery{.percent = 0, .charging = false};
      bm.status.wifi = Link::Off;
      rc |= emit(target, "chart-battery-empty", 1, bm, *h);
    }

    // The same day with bad air right now.
    std::array<float, kMetricCount> bad = last;
    bad[index(Metric::Pm25)] = 142.6F;
    h->push(end + 10, bad);
    fill_now(m, bad, end + 10);
    rc |= emit(target, "dashboard-bad-air", kDashboardPage, m, *h);

    // The settings popup over the dashboard, then editing the altitude.
    const SettingsInfo info{.wifi_configured = true,
                            .wifi_on = true,
                            .sensor_present = true,
                            .fan_cleaning = false,
                            .orientation_source = moving};
    Settings s = defaults(moving);
    s.rotation = setting_for(target.rotation);
    s.refresh = RefreshEvery::S30;
    s.altitude_m = 300;
    s.temp_offset_dc = -15;
    s.sleep_after = SleepAfter::M30;
    UiState ui{};
    static_cast<void>(handle(ui, s, info, Input::Menu));
    rc |= emit_screen(target, "settings", ui, s, info, m, *h);
    // Down twice to Theme and press, the popup now draws dark.
    {
      UiState dark_ui = ui;
      Settings dark = s;
      for (int i = 0; i < 2; ++i)
        static_cast<void>(handle(dark_ui, dark, info, Input::Down));
      static_cast<void>(handle(dark_ui, dark, info, Input::Press));
      rc |= emit_screen(target, "settings-theme-dark", dark_ui, dark, info, m,
                        *h);
      UiState asleep_ui = dark_ui;
      fall_asleep(asleep_ui);
      rc |= emit_screen(target, "asleep-dark", asleep_ui, dark, info, m, *h);
    }
    // Down eight times to Altitude, press to edit, one step up.
    for (int i = 0; i < 8; ++i)
      static_cast<void>(handle(ui, s, info, Input::Down));
    static_cast<void>(handle(ui, s, info, Input::Press));
    static_cast<void>(handle(ui, s, info, Input::Up));
    rc |= emit_screen(target, "settings-edit-altitude", ui, s, info, m, *h);

    // The screen asleep after 30 minutes idle.
    fall_asleep(ui);
    rc |= emit_screen(target, "asleep", ui, s, info, m, *h);
    rc |= emit_storage_fault(target);
    rc |= emit_thing_id_fault(target);
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
    rc |= emit(target, "dashboard-warming", kDashboardPage, m, *h);
    rc |= emit(target, "chart-warming", 1, m, *h);
    rc |= emit(target, "status-warming", kStatusPage, m, *h);
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
    rc |= emit(target, "dashboard-gap", kDashboardPage, m, *h);
    // The widest header, every tag and a full battery on its charger.
    m.status.battery = Battery{.percent = 100, .charging = true};
    rc |= emit(target, "dashboard-gap-battery", kDashboardPage, m, *h);
    m.status.battery = std::nullopt;
    rc |= emit(target, "chart-gap", 1, m, *h);
    rc |= emit(target, "status-fault", kStatusPage, m, *h);
  }

  // Two earlier boots read back from the card, then 50 minutes of this
  // boot. Each reboot draws as a dashed line, in the 24 h chart and in the
  // last 2 h of the dashboard.
  {
    auto h = std::make_unique<History>();
    Lcg rng;
    std::array<float, kMetricCount> v{};
    h->seed_boot();
    for (std::uint32_t t = 0; t < 9 * 3600; t += 10)
      h->seed(t + 25, day_sample(t, rng));
    h->seed_boot();
    for (std::uint32_t t = 0; t < 12 * 3600; t += 10)
      h->seed(t + 18, day_sample(t + 9 * 3600 + 900, rng));
    h->finish_seed();
    const std::uint32_t end = 50 * 60;
    for (std::uint32_t t = 20; t <= end; t += 10) {
      v = day_sample(t + 21 * 3600 + 1800, rng);
      h->push(t, v);
    }
    Model m{};
    m.status = base_status();
    m.status.uptime_s = end;
    fill_now(m, v, end);
    rc |= emit(target, "dashboard-restart", kDashboardPage, m, *h);
    rc |= emit(target, "chart-restart", 1, m, *h);
    rc |= emit_dark(target, "chart-restart-dark", 1, m, *h);
    rc |= emit_dark(target, "dashboard-restart-dark", kDashboardPage, m, *h);
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
    rc |= emit(target, "dashboard-no-sensor", kDashboardPage, m, *h);
    rc |= emit(target, "chart-empty", 2, m, *h);

    // Settings on a Thing with no Wi-Fi set up and no sensor, the Wi-Fi row
    // selected, over the status page.
    const SettingsInfo info{.wifi_configured = false,
                            .wifi_on = false,
                            .sensor_present = false,
                            .fan_cleaning = false,
                            .orientation_source = moving};
    Settings s = defaults(moving);
    s.rotation = setting_for(target.rotation);
    s.co2_asc = false;
    s.start_page = StartPage::LastViewed;
    UiState ui{.page = kStatusPage};
    static_cast<void>(handle(ui, s, info, Input::Menu));
    for (int i = 0; i < 5; ++i)
      static_cast<void>(handle(ui, s, info, Input::Down));
    rc |= emit_screen(target, "settings-no-wifi-no-sensor", ui, s, info, m, *h);
  }

  // A phone pairs. The passkey here is a fixed example, never a real one.
  {
    auto h = std::make_unique<History>();
    Model m{};
    m.status = base_status();
    m.status.ble = Link::Connected;
    m.status.pairing_passkey = 123456U;
    rc |= emit(target, "pairing", kDashboardPage, m, *h);
    rc |= emit_dark(target, "pairing-dark", kDashboardPage, m, *h);
  }
  return rc;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <out dir>\n", argv[0]);
    return 2;
  }
  const std::string out = argv[1];
  struct Size {
    const char *name;
    const DisplayProfile *profile;
  };
  constexpr std::array<Size, 2> kSizes{{
      {.name = "400x300", .profile = &host::kHost400x300},
      {.name = "800x480", .profile = &host::kHost800x480},
  }};
  struct Pass {
    Language language;
    const char *prefix;
  };
  constexpr std::array<Pass, 2> kPasses{{
      {.language = Language::English, .prefix = "/"},
      {.language = Language::Arabic, .prefix = "/ar/"},
  }};
  int rc = 0;
  for (const Pass &pass : kPasses) {
    g_language = pass.language;
    for (const Size &size : kSizes) {
      if (!validate(*size.profile)) {
        std::fprintf(stderr, "profile %s refused\n", size.name);
        return 1;
      }
      for (const Rotation r :
           {Rotation::R0, Rotation::R90, Rotation::R180, Rotation::R270}) {
        const Target t{.dir = out + pass.prefix + size.name + "/r" +
                              std::to_string(90 * std::to_underlying(r)),
                       .profile = size.profile,
                       .rotation = r};
        std::error_code ec;
        std::filesystem::create_directories(t.dir, ec);
        if (ec) {
          std::fprintf(stderr, "cannot create %s\n", t.dir.c_str());
          return 1;
        }
        rc |= scenes(t);
      }
    }
  }
  set_language(Language::English);
  return rc;
}
