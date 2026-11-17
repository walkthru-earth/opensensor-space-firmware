#include "settings.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <utility>

namespace opensensor::ui {
namespace {

static constexpr std::array<std::uint32_t, 4> kRefreshSeconds{10, 30, 60, 300};
static constexpr std::array<std::string_view, 4> kRefreshNames{
    "10 s", "30 s", "1 min", "5 min"};
static constexpr std::array<std::uint32_t, 4> kSleepSeconds{0, 600, 1800, 3600};
static constexpr std::array<std::string_view, 4> kSleepNames{"Off", "10 min",
                                                             "30 min", "1 h"};
static constexpr std::array<std::string_view, kRowCount> kRowLabels{
    "Screen refresh",  "Start on", "Wi-Fi",       "Fan cleaning",
    "CO₂ calibration", "Altitude", "Temp offset", "Sleep after"};

// The range and step of a numeric row.
struct Numeric {
  std::int32_t lo = 0;
  std::int32_t hi = 0;
  std::int32_t step = 0;
};
static constexpr Numeric kAltitude{
    .lo = 0, .hi = kAltitudeMaxM, .step = kAltitudeStepM};
static constexpr Numeric kTempOffset{
    .lo = kTempOffsetMinDc, .hi = kTempOffsetMaxDc, .step = kTempOffsetStepDc};

template <typename E, std::size_t N>
[[nodiscard]] constexpr E next_choice(E e) {
  return static_cast<E>((std::to_underlying(e) + 1U) % N);
}

[[nodiscard]] std::optional<Numeric> numeric_of(Row r) {
  if (r == Row::Altitude)
    return kAltitude;
  if (r == Row::TempOffset)
    return kTempOffset;
  return std::nullopt;
}

[[nodiscard]] std::int32_t numeric_value(Row r, const Settings &s) {
  return r == Row::Altitude ? std::int32_t{s.altitude_m}
                            : std::int32_t{s.temp_offset_dc};
}

[[nodiscard]] Row step_row(Row r, int delta) {
  const int n = kRowCount;
  return static_cast<Row>((std::to_underlying(r) + n + delta) % n);
}

std::string_view view(std::span<char> buf, int n) {
  if (n < 0 || buf.empty())
    return {};
  return {buf.data(), std::min(static_cast<std::size_t>(n), buf.size() - 1)};
}

std::string_view copy_into(std::span<char> buf, std::string_view s) {
  return view(buf, std::snprintf(buf.data(), buf.size(), "%.*s",
                                 static_cast<int>(s.size()), s.data()));
}

} // namespace

std::uint32_t refresh_seconds(const Settings &s) {
  const auto i = std::to_underlying(s.refresh);
  return i < kRefreshSeconds.size() ? kRefreshSeconds[i] : kRefreshSeconds[0];
}

std::optional<std::uint32_t> sleep_after_seconds(const Settings &s) {
  const auto i = std::to_underlying(s.sleep_after);
  if (i == 0 || i >= kSleepSeconds.size())
    return std::nullopt;
  return kSleepSeconds[i];
}

std::uint8_t start_page(const Settings &s) {
  if (s.start_page == StartPage::LastViewed && s.last_page < kPageCount)
    return s.last_page;
  return kDashboardPage;
}

SettingsBlob encode(const Settings &s) {
  const auto alt = s.altitude_m;
  const auto off = std::bit_cast<std::uint16_t>(s.temp_offset_dc);
  return {
      kSettingsVersion,
      std::to_underlying(s.refresh),
      std::to_underlying(s.start_page),
      s.last_page,
      static_cast<std::uint8_t>(s.co2_asc ? 1U : 0U),
      static_cast<std::uint8_t>(alt & 0xFFU),
      static_cast<std::uint8_t>(alt >> 8U),
      static_cast<std::uint8_t>(off & 0xFFU),
      static_cast<std::uint8_t>(off >> 8U),
      std::to_underlying(s.sleep_after),
  };
}

Settings decode(std::span<const std::uint8_t> blob) {
  Settings s{};
  if (blob.empty() || blob[0] != kSettingsVersion)
    return s;
  const auto has = [&](std::size_t end) { return blob.size() >= end; };
  if (has(2) && blob[1] < kRefreshSeconds.size())
    s.refresh = static_cast<RefreshEvery>(blob[1]);
  if (has(3) && blob[2] <= std::to_underlying(StartPage::LastViewed))
    s.start_page = static_cast<StartPage>(blob[2]);
  if (has(4) && blob[3] < kPageCount)
    s.last_page = blob[3];
  if (has(5) && blob[4] <= 1U)
    s.co2_asc = blob[4] == 1U;
  if (has(7)) {
    const auto alt = static_cast<std::uint16_t>(blob[5] | (blob[6] << 8U));
    if (alt <= kAltitudeMaxM)
      s.altitude_m = alt;
  }
  if (has(9)) {
    const auto off = std::bit_cast<std::int16_t>(
        static_cast<std::uint16_t>(blob[7] | (blob[8] << 8U)));
    if (off >= kTempOffsetMinDc && off <= kTempOffsetMaxDc)
      s.temp_offset_dc = off;
  }
  if (has(10) && blob[9] < kSleepSeconds.size())
    s.sleep_after = static_cast<SleepAfter>(blob[9]);
  return s;
}

Effect popup_input(PopupState &p, Settings &s, const SettingsInfo &board,
                   Input in) {
  if (!p.open) {
    if (in != Input::Menu)
      return Effect::None;
    p = {.open = true,
         .selected = Row::Refresh,
         .editing = false,
         .edit_value = 0};
    return Effect::Opened;
  }
  if (p.editing) {
    const auto num = numeric_of(p.selected);
    if (!num) {
      p.editing = false;
      return Effect::None;
    }
    switch (in) {
    case Input::Up:
      p.edit_value = std::min(p.edit_value + num->step, num->hi);
      return Effect::None;
    case Input::Down:
      p.edit_value = std::max(p.edit_value - num->step, num->lo);
      return Effect::None;
    case Input::Press: {
      p.editing = false;
      const std::int32_t v = std::clamp(p.edit_value, num->lo, num->hi);
      if (v == numeric_value(p.selected, s))
        return Effect::None;
      if (p.selected == Row::Altitude)
        s.altitude_m = static_cast<std::uint16_t>(v);
      else
        s.temp_offset_dc = static_cast<std::int16_t>(v);
      return Effect::Changed;
    }
    case Input::Exit:
      p.editing = false;
      return Effect::None;
    case Input::Menu:
      p.editing = false;
      p.open = false;
      return Effect::Closed;
    }
    return Effect::None;
  }
  switch (in) {
  case Input::Up:
    p.selected = step_row(p.selected, -1);
    return Effect::None;
  case Input::Down:
    p.selected = step_row(p.selected, 1);
    return Effect::None;
  case Input::Menu:
  case Input::Exit:
    p.open = false;
    return Effect::Closed;
  case Input::Press:
    break;
  }
  switch (p.selected) {
  case Row::Refresh:
    s.refresh = next_choice<RefreshEvery, kRefreshSeconds.size()>(s.refresh);
    return Effect::Changed;
  case Row::StartPage:
    s.start_page = s.start_page == StartPage::Dashboard ? StartPage::LastViewed
                                                        : StartPage::Dashboard;
    return Effect::Changed;
  case Row::Wifi:
    // Read only, the shared device config owns Wi-Fi.
    return Effect::None;
  case Row::FanCleaning:
    return board.sensor_present && !board.fan_cleaning ? Effect::FanClean
                                                       : Effect::None;
  case Row::Co2Asc:
    s.co2_asc = !s.co2_asc;
    return Effect::Changed;
  case Row::Altitude:
  case Row::TempOffset:
    p.editing = true;
    p.edit_value = numeric_value(p.selected, s);
    return Effect::None;
  case Row::SleepAfter:
    s.sleep_after =
        next_choice<SleepAfter, kSleepSeconds.size()>(s.sleep_after);
    return Effect::Changed;
  }
  return Effect::None;
}

void fall_asleep(UiState &ui) {
  ui.asleep = true;
  ui.popup.open = false;
  ui.popup.editing = false;
}

Outcome handle(UiState &ui, Settings &s, const SettingsInfo &board, Input in) {
  if (ui.asleep) {
    ui.asleep = false;
    return {
        .page_changed = false, .full_refresh = true, .effect = Effect::None};
  }
  Outcome out{};
  if (ui.popup.open || in == Input::Menu) {
    out.effect = popup_input(ui.popup, s, board, in);
    out.full_refresh = out.effect == Effect::Closed;
  } else {
    if (ui.page >= kPageCount)
      ui.page = kDashboardPage;
    const Nav nav = navigate(ui.page, in);
    out.page_changed = nav.page != ui.page;
    out.full_refresh = nav.full_refresh;
    ui.page = nav.page;
  }
  // Only a Thing that starts on the last page writes it, so turning pages
  // costs no NVS write otherwise.
  if (s.start_page == StartPage::LastViewed && s.last_page != ui.page &&
      (out.effect == Effect::None || out.effect == Effect::Changed)) {
    s.last_page = ui.page;
    out.effect = Effect::Changed;
  }
  return out;
}

std::string_view row_label(Row r) {
  const auto i = std::to_underlying(r);
  return i < kRowLabels.size() ? kRowLabels[i] : std::string_view{};
}

std::string_view row_value(std::span<char> buf, Row r, const Settings &s,
                           const PopupState &p, const SettingsInfo &board) {
  const bool editing = p.editing && p.selected == r;
  switch (r) {
  case Row::Refresh: {
    const auto i = std::to_underlying(s.refresh);
    return copy_into(buf, i < kRefreshNames.size() ? kRefreshNames[i] : "");
  }
  case Row::StartPage:
    return copy_into(buf, s.start_page == StartPage::LastViewed ? "Last page"
                                                                : "Dashboard");
  case Row::Wifi:
    if (!board.wifi_configured)
      return copy_into(buf, "Not set up");
    return copy_into(buf, board.wifi_on ? "On" : "Off");
  case Row::FanCleaning:
    if (!board.sensor_present)
      return copy_into(buf, "No sensor");
    return copy_into(buf, board.fan_cleaning ? "Running" : "Run now");
  case Row::Co2Asc:
    return copy_into(buf, s.co2_asc ? "On" : "Off (lab only)");
  case Row::Altitude: {
    const std::int32_t v = editing ? p.edit_value : s.altitude_m;
    return view(buf, std::snprintf(buf.data(), buf.size(), "%d m",
                                   static_cast<int>(v)));
  }
  case Row::TempOffset: {
    const std::int32_t v = editing ? p.edit_value : s.temp_offset_dc;
    const std::int32_t a = v < 0 ? -v : v;
    const char *sign = v < 0 ? "-" : (v > 0 ? "+" : "");
    return view(buf, std::snprintf(buf.data(), buf.size(), "%s%d.%d °C", sign,
                                   static_cast<int>(a / 10),
                                   static_cast<int>(a % 10)));
  }
  case Row::SleepAfter: {
    const auto i = std::to_underlying(s.sleep_after);
    return copy_into(buf, i < kSleepNames.size() ? kSleepNames[i] : "");
  }
  }
  return {};
}

namespace {

constexpr int kPopupW = 320;
constexpr int kPopupHead = 32;
constexpr int kPopupRow = 23;
constexpr int kPopupFoot = 28;
constexpr int kPopupH = kPopupHead + kRowCount * kPopupRow + kPopupFoot;
constexpr int kPad = 12;

std::string_view hint_for(const PopupState &p, const SettingsInfo &board) {
  if (p.editing)
    return "Turn to adjust, press to keep, EXIT to cancel";
  switch (p.selected) {
  case Row::Wifi:
    return board.wifi_configured ? "Wi-Fi is switched from the phone app"
                                 : "Set up Wi-Fi from the phone app";
  case Row::Co2Asc:
    return "Keep on in the field, off is for lab tests";
  default:
    return "Turn to pick, press to change, MENU to close";
  }
}

} // namespace

void render_settings(Canvas &c, const Settings &s, const PopupState &p,
                     const SettingsInfo &board) {
  const Rect box{(kWidth - kPopupW) / 2, (kHeight - kPopupH) / 2, kPopupW,
                 kPopupH};
  // A white margin keeps the page under it from touching the frame.
  c.fill({box.x - 3, box.y - 3, box.w + 6, box.h + 6}, Ink::White);
  c.rect(box);
  c.rect({box.x + 1, box.y + 1, box.w - 2, box.h - 2});
  c.text(fonts::title, box.x + kPad, box.y + 23, "Settings");
  c.hline(box.x + 8, box.y + kPopupHead - 3, box.w - 16);

  const int right = box.right() - kPad;
  for (std::uint8_t i = 0; i < kRowCount; ++i) {
    const auto r = static_cast<Row>(i);
    const int ry = box.y + kPopupHead + i * kPopupRow;
    const int base = ry + 16;
    const bool selected = r == p.selected;
    const bool editing = selected && p.editing;
    const Rect bar{box.x + 5, ry, box.w - 10, kPopupRow - 1};
    if (selected)
      c.fill(bar);
    const Ink ink = selected ? Ink::White : Ink::Black;
    c.text(fonts::label, box.x + kPad, base, row_label(r), ink);
    std::array<char, 24> buf{};
    const std::string_view v = row_value(buf, r, s, p, board);
    if (!editing) {
      c.text(fonts::label, right, base, v, Align::Right, ink);
      continue;
    }
    // The value being edited sits in a white pill with the knob's arrows.
    const int vw = text_width(fonts::label, v);
    const int aw = text_width(fonts::label, "↓");
    const int pw = vw + 2 * aw + 20;
    const Rect pill{right + 4 - pw, ry + 2, pw, kPopupRow - 5};
    c.fill_round_rect(pill, Ink::White);
    c.text(fonts::label, pill.x + 5, base, "↓");
    c.text(fonts::label, pill.x + 5 + aw + 5, base, v);
    c.text(fonts::label, pill.right() - 5 - aw, base, "↑");
  }

  const int fy = box.bottom() - kPopupFoot;
  c.hline(box.x + 8, fy + 3, box.w - 16);
  c.text(fonts::small, kWidth / 2, box.bottom() - 9, hint_for(p, board),
         Align::Center);
}

void render_screen(Canvas &c, const UiState &ui, const Settings &s,
                   const SettingsInfo &board, const Model &model,
                   const History &history) {
  if (model.status.pairing_passkey) {
    render(c, ui.page, model, history);
    return;
  }
  if (ui.asleep) {
    render_asleep(c, model);
    return;
  }
  render(c, ui.page, model, history);
  if (ui.popup.open)
    render_settings(c, s, ui.popup, board);
}

} // namespace opensensor::ui
