#include "epaper_ui/settings.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <utility>

#include "epaper_ui/strings.h"
#include "os_strings.h"

namespace opensensor::ui {
namespace {

static constexpr std::array<std::uint32_t, 4> kRefreshSeconds{10, 30, 60, 300};
static constexpr std::array<Str, 4> kRefreshNames{
    Str::Every10s, Str::Every30s, Str::Every1min, Str::Every5min};
static constexpr std::array<std::uint32_t, 4> kSleepSeconds{0, 600, 1800, 3600};
static constexpr std::array<Str, 4> kSleepNames{Str::Off, Str::After10min,
                                                Str::After30min, Str::After1h};
static constexpr std::array<Str, kRowCount> kRowLabels{
    Str::LabelRefresh,     Str::LabelStartOn,   Str::LabelTheme,
    Str::LabelRotation,    Str::LabelLanguage,  Str::LabelWifi,
    Str::LabelFanCleaning, Str::LabelCo2Asc,    Str::LabelAltitude,
    Str::LabelTempOffset,  Str::LabelSleepAfter};
// Indexed by ScreenRotation.
static constexpr std::array<Str, 5> kRotationNames{
    Str::Rotate0, Str::Rotate90, Str::Rotate180, Str::Rotate270,
    Str::RotateAuto};
static_assert(std::to_underlying(ScreenRotation::Auto) + 1U ==
              kRotationNames.size());

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

// Auto, 0, 90, 180, 270 and round with a motion sensor, 0 to 270 without.
[[nodiscard]] ScreenRotation next_rotation(ScreenRotation r, bool auto_ok) {
  switch (r) {
  case ScreenRotation::Auto:
    return ScreenRotation::R0;
  case ScreenRotation::R0:
    return ScreenRotation::R90;
  case ScreenRotation::R90:
    return ScreenRotation::R180;
  case ScreenRotation::R180:
    return ScreenRotation::R270;
  case ScreenRotation::R270:
    return auto_ok ? ScreenRotation::Auto : ScreenRotation::R0;
  }
  return ScreenRotation::R0;
}

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

std::string_view copy_into(std::span<char> buf, Str id) {
  return copy_into(buf, tr(id));
}

static constexpr std::array<std::string_view, 2> kThemeKeys{"light", "dark"};
static constexpr std::array<std::string_view, 4> kRefreshKeys{"10", "30", "60",
                                                              "300"};
static constexpr std::array<std::string_view, 2> kStartKeys{"dashboard",
                                                            "last_viewed"};
static constexpr std::array<std::string_view, 4> kSleepKeys{"0", "600", "1800",
                                                            "3600"};

// The index of value in names, as the enum it names.
template <typename E, std::size_t N>
[[nodiscard]] std::optional<E>
choice_of(const std::array<std::string_view, N> &names,
          std::string_view value) {
  const auto it = std::ranges::find(names, value);
  if (it == names.end())
    return std::nullopt;
  return static_cast<E>(it - names.begin());
}

template <typename E, std::size_t N>
[[nodiscard]] std::string_view
name_of(const std::array<std::string_view, N> &names, E e) {
  const auto i = std::to_underlying(e);
  return i < N ? names[i] : names[0];
}

[[nodiscard]] std::optional<bool> boolean_of(std::string_view value) {
  if (value == "1" || value == "true")
    return true;
  if (value == "0" || value == "false")
    return false;
  return std::nullopt;
}

// A plain decimal, an optional minus and 1 to 5 digits, within lo to hi.
[[nodiscard]] std::optional<std::int32_t>
integer_of(std::string_view value, std::int32_t lo, std::int32_t hi) {
  const bool negative = value.starts_with('-');
  if (negative)
    value.remove_prefix(1);
  if (value.empty() || value.size() > 5)
    return std::nullopt;
  std::int32_t n = 0;
  for (const char c : value) {
    if (c < '0' || c > '9')
      return std::nullopt;
    n = n * 10 + (c - '0');
  }
  if (negative)
    n = -n;
  if (n < lo || n > hi)
    return std::nullopt;
  return n;
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

Settings defaults(bool orientation_source) {
  Settings s{};
  if (orientation_source)
    s.rotation = ScreenRotation::Auto;
  return s;
}

Rotation effective_rotation(const Settings &s, const DisplayProfile &profile,
                            Rotation sensed) {
  if (s.rotation == ScreenRotation::Auto)
    return profile.orientation.present ? sensed : profile.mount;
  return compose(profile.mount,
                 static_cast<Rotation>(std::to_underlying(s.rotation) & 3U));
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
      std::to_underlying(s.theme),
      std::to_underlying(s.rotation),
      std::to_underlying(s.language),
  };
}

Settings decode(std::span<const std::uint8_t> blob, bool orientation_source) {
  Settings s = defaults(orientation_source);
  // Version 1 is version 3 without the rotation and language bytes and
  // version 2 without the language byte, so one reader serves all three. An
  // older blob is cut where its version ends, so a stray byte past it cannot
  // set a field that version did not have.
  if (blob.empty() || blob[0] < 1U || blob[0] > kSettingsVersion)
    return s;
  if (blob[0] == 1U)
    blob = blob.first(std::min<std::size_t>(blob.size(), 11));
  else if (blob[0] == 2U)
    blob = blob.first(std::min<std::size_t>(blob.size(), 12));
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
  if (has(11) && blob[10] <= std::to_underlying(Theme::Dark))
    s.theme = static_cast<Theme>(blob[10]);
  if (has(12) && blob[11] <= std::to_underlying(ScreenRotation::Auto) &&
      (orientation_source ||
       blob[11] != std::to_underlying(ScreenRotation::Auto)))
    s.rotation = static_cast<ScreenRotation>(blob[11]);
  if (has(13) && blob[12] <= std::to_underlying(Language::Arabic))
    s.language = static_cast<Language>(blob[12]);
  return s;
}

std::expected<void, PhoneKeyError>
set_phone_key(PhoneDraft &d, std::string_view key, std::string_view value) {
  const auto set =
      [](auto &field,
         const auto &parsed) -> std::expected<void, PhoneKeyError> {
    if (!parsed)
      return std::unexpected(PhoneKeyError::Value);
    field = *parsed;
    return {};
  };
  if (key == "panel.theme")
    return set(d.theme, choice_of<Theme>(kThemeKeys, value));
  if (key == "panel.refresh_s")
    return set(d.refresh, choice_of<RefreshEvery>(kRefreshKeys, value));
  if (key == "panel.start_page")
    return set(d.start_page, choice_of<StartPage>(kStartKeys, value));
  if (key == "panel.sleep_s")
    return set(d.sleep_after, choice_of<SleepAfter>(kSleepKeys, value));
  if (key == "sen6x.co2_asc")
    return set(d.co2_asc, boolean_of(value));
  if (key == "sen6x.altitude_m") {
    const auto n = integer_of(value, 0, kAltitudeMaxM);
    return set(d.altitude_m,
               n ? std::optional<std::uint16_t>(static_cast<std::uint16_t>(*n))
                 : std::nullopt);
  }
  if (key == "sen6x.temp_offset_dc") {
    const auto n = integer_of(value, kTempOffsetMinDc, kTempOffsetMaxDc);
    return set(d.temp_offset_dc,
               n ? std::optional<std::int16_t>(static_cast<std::int16_t>(*n))
                 : std::nullopt);
  }
  if (key == "sen6x.fan_clean") {
    const auto start = boolean_of(value);
    if (!start)
      return std::unexpected(PhoneKeyError::Value);
    d.fan_clean = d.fan_clean || *start;
    return {};
  }
  return std::unexpected(PhoneKeyError::Unknown);
}

void merge_phone_draft(PhoneDraft &into, const PhoneDraft &later) {
  const auto take = [](auto &field, const auto &value) {
    if (value)
      field = value;
  };
  take(into.theme, later.theme);
  take(into.refresh, later.refresh);
  take(into.start_page, later.start_page);
  take(into.sleep_after, later.sleep_after);
  take(into.co2_asc, later.co2_asc);
  take(into.altitude_m, later.altitude_m);
  take(into.temp_offset_dc, later.temp_offset_dc);
  into.fan_clean = into.fan_clean || later.fan_clean;
}

void apply_phone_draft(Settings &s, const PhoneDraft &d) {
  if (d.theme)
    s.theme = *d.theme;
  if (d.refresh)
    s.refresh = *d.refresh;
  if (d.start_page)
    s.start_page = *d.start_page;
  if (d.sleep_after)
    s.sleep_after = *d.sleep_after;
  if (d.co2_asc)
    s.co2_asc = *d.co2_asc;
  if (d.altitude_m)
    s.altitude_m = *d.altitude_m;
  if (d.temp_offset_dc)
    s.temp_offset_dc = *d.temp_offset_dc;
}

void apply_phone_draft(Settings &s, const PhoneDraft &d, std::uint8_t shown) {
  apply_phone_draft(s, d);
  if (d.start_page == StartPage::LastViewed && shown < kPageCount)
    s.last_page = shown;
}

std::size_t phone_page(std::span<char> out, const Settings &s) {
  if (out.empty())
    return 0;
  const auto theme = name_of(kThemeKeys, s.theme);
  const auto refresh = name_of(kRefreshKeys, s.refresh);
  const auto start = name_of(kStartKeys, s.start_page);
  const auto sleep = name_of(kSleepKeys, s.sleep_after);
  const int n =
      std::snprintf(out.data(), out.size(),
                    "{\"panel\":{\"theme\":\"%.*s\",\"refresh_s\":\"%.*s\","
                    "\"start_page\":\"%.*s\",\"sleep_s\":\"%.*s\"},"
                    "\"sen6x\":{\"co2_asc\":%s,\"altitude_m\":%u,"
                    "\"temp_offset_dc\":%d}}",
                    static_cast<int>(theme.size()), theme.data(),
                    static_cast<int>(refresh.size()), refresh.data(),
                    static_cast<int>(start.size()), start.data(),
                    static_cast<int>(sleep.size()), sleep.data(),
                    s.co2_asc ? "true" : "false", unsigned{s.altitude_m},
                    int{s.temp_offset_dc});
  if (n <= 0 || static_cast<std::size_t>(n) >= out.size()) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<std::size_t>(n);
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
  case Row::Theme:
    s.theme = s.theme == Theme::Light ? Theme::Dark : Theme::Light;
    return Effect::Changed;
  case Row::Rotation:
    s.rotation = next_rotation(s.rotation, board.orientation_source);
    return Effect::Changed;
  case Row::Language:
    s.language =
        s.language == Language::English ? Language::Arabic : Language::English;
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
    const Theme theme = s.theme;
    const ScreenRotation rotation = s.rotation;
    const Language lang = s.language;
    out.effect = popup_input(ui.popup, s, board, in);
    out.full_refresh = out.effect == Effect::Closed || s.theme != theme ||
                       s.rotation != rotation || s.language != lang;
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
  return i < kRowLabels.size() ? tr(kRowLabels[i]) : std::string_view{};
}

std::string_view row_value(std::span<char> buf, Row r, const Settings &s,
                           const PopupState &p, const SettingsInfo &board) {
  const bool editing = p.editing && p.selected == r;
  switch (r) {
  case Row::Refresh: {
    const auto i = std::to_underlying(s.refresh);
    return i < kRefreshNames.size() ? copy_into(buf, kRefreshNames[i])
                                    : copy_into(buf, std::string_view{});
  }
  case Row::StartPage:
    return copy_into(buf, s.start_page == StartPage::LastViewed
                              ? Str::StartLastPage
                              : Str::StartDashboard);
  case Row::Theme:
    return copy_into(buf,
                     s.theme == Theme::Dark ? Str::ThemeDark : Str::ThemeLight);
  case Row::Rotation: {
    const auto i = std::to_underlying(s.rotation);
    return i < kRotationNames.size() ? copy_into(buf, kRotationNames[i])
                                     : copy_into(buf, std::string_view{});
  }
  case Row::Language:
    return copy_into(buf, s.language == Language::Arabic
                              ? text::text(text::Lang::ArEg, Str::LangArabic)
                              : text::text(text::Lang::En, Str::LangEnglish));
  case Row::Wifi:
    if (!board.wifi_configured)
      return copy_into(buf, Str::WifiNotSetUp);
    return copy_into(buf, board.wifi_on ? Str::On : Str::Off);
  case Row::FanCleaning:
    if (!board.sensor_present)
      return copy_into(buf, Str::FanNoSensor);
    return copy_into(buf,
                     board.fan_cleaning ? Str::FanRunning : Str::FanRunNow);
  case Row::Co2Asc:
    return copy_into(buf, s.co2_asc ? Str::On : Str::AscOffLab);
  case Row::Altitude: {
    const std::int32_t v = editing ? p.edit_value : s.altitude_m;
    return view(buf,
                std::snprintf(buf.data(), buf.size(), tr_fmt(Str::AltitudeFmt),
                              static_cast<int>(v)));
  }
  case Row::TempOffset: {
    const std::int32_t v = editing ? p.edit_value : s.temp_offset_dc;
    const std::int32_t a = v < 0 ? -v : v;
    const char *sign = v < 0 ? "-" : (v > 0 ? "+" : "");
    return view(buf, std::snprintf(buf.data(), buf.size(),
                                   tr_fmt(Str::TempOffsetFmt), sign,
                                   static_cast<int>(a / 10),
                                   static_cast<int>(a % 10)));
  }
  case Row::SleepAfter: {
    const auto i = std::to_underlying(s.sleep_after);
    return i < kSleepNames.size() ? copy_into(buf, kSleepNames[i])
                                  : copy_into(buf, std::string_view{});
  }
  }
  return {};
}

namespace {

Str hint_for(const PopupState &p, const SettingsInfo &board) {
  if (p.editing)
    return Str::HintEdit;
  switch (p.selected) {
  case Row::Wifi:
    return board.wifi_configured ? Str::HintWifiSet : Str::HintWifiUnset;
  case Row::Co2Asc:
    return Str::HintCo2Asc;
  default:
    return Str::HintPick;
  }
}

} // namespace

void render_settings(Canvas &c, const Settings &s, const PopupState &p,
                     const SettingsInfo &board) {
  const PopupLayout &pl = c.layout().popup;
  const FontSet &fs = c.fonts();
  const int h = pl.height(kRowCount);
  // Centred, but never over the header rule, so its white margin starts a
  // pixel below it.
  const int top = c.layout().header.height + pl.margin + 1;
  const Rect box{(c.width() - pl.w) / 2, std::max((c.height() - h) / 2, top),
                 pl.w, h};
  // Labels start at `lead` and values end at `trail`, swapped in a right to
  // left language.
  const bool mirror = rtl();
  const Align lead_align = mirror ? Align::Right : Align::Left;
  const int lead = mirror ? box.right() - pl.pad : box.x + pl.pad;
  // A white margin keeps the page under it from touching the frame.
  c.fill({box.x - pl.margin, box.y - pl.margin, box.w + 2 * pl.margin,
          box.h + 2 * pl.margin},
         Ink::White);
  c.rect(box);
  c.rect({box.x + 1, box.y + 1, box.w - 2, box.h - 2});
  c.text(fs.title, lead, box.y + pl.title_dy, tr(Str::SettingsTitle),
         lead_align);
  c.hline(box.x + pl.rule_inset, box.y + pl.head - pl.rule_dy,
          box.w - 2 * pl.rule_inset);

  const std::string_view down = tr(Str::ArrowDown);
  const std::string_view up = tr(Str::ArrowUp);
  const int trail = mirror ? box.x + pl.pad : box.right() - pl.pad;
  for (std::uint8_t i = 0; i < kRowCount; ++i) {
    const auto r = static_cast<Row>(i);
    const int ry = box.y + pl.head + i * pl.row;
    const int base = ry + pl.row_baseline;
    const bool selected = r == p.selected;
    const bool editing = selected && p.editing;
    const Rect bar{box.x + pl.bar_inset, ry, box.w - 2 * pl.bar_inset,
                   pl.row - 1};
    if (selected)
      c.fill(bar);
    const Ink ink = selected ? Ink::White : Ink::Black;
    c.text(fs.label, lead, base, row_label(r), lead_align, ink);
    std::array<char, 24> buf{};
    const std::string_view v = row_value(buf, r, s, p, board);
    if (!editing) {
      c.text(fs.label, trail, base, v, mirror ? Align::Left : Align::Right,
             ink);
      continue;
    }
    // The value being edited sits in a white pill with the knob's arrows.
    // The pill reads the same in both languages, down, value, up.
    const int vw = text_width(fs.label, v);
    const int aw = text_width(fs.label, down);
    const int pw = vw + 2 * aw + pl.pill_extra;
    const int pill_x =
        mirror ? trail - pl.pill_overhang : trail + pl.pill_overhang - pw;
    const Rect pill{pill_x, ry + pl.pill_dy, pw, pl.row - pl.pill_shrink};
    c.fill_round_rect(pill, Ink::White);
    c.text(fs.label, pill.x + pl.pill_gap, base, down);
    c.text(fs.label, pill.x + pl.pill_gap + aw + pl.pill_gap, base, v);
    c.text(fs.label, pill.right() - pl.pill_gap - aw, base, up);
  }

  const int fy = box.bottom() - pl.foot;
  c.hline(box.x + pl.rule_inset, fy + pl.rule_dy, box.w - 2 * pl.rule_inset);
  // A hint too wide for a portrait popup takes two lines, the second on the
  // baseline one line would use.
  const std::string_view hint = tr(hint_for(p, board));
  const int max_w = box.w - 2 * pl.pad;
  int hy = box.bottom() - pl.hint_dy;
  if (line_break(fs.small, hint, max_w) < hint.size())
    hy -= line_step(fs.small, hint);
  static_cast<void>(
      c.text_lines(fs.small, c.width() / 2, hy, hint, Align::Center, max_w));
}

void render_screen(Canvas &c, const UiState &ui, const Settings &s,
                   const SettingsInfo &board, const Model &model,
                   const History &history) {
  set_language(s.language);
  if (ui.asleep && !model.status.pairing_passkey) {
    // E-paper rests mostly white, so sleep ignores the theme.
    render_asleep(c, model);
    return;
  }
  render(c, ui.page, model, history);
  if (ui.popup.open && !model.status.pairing_passkey)
    render_settings(c, s, ui.popup, board);
  if (s.theme == Theme::Dark)
    c.invert_all();
}

} // namespace opensensor::ui
