#pragma once

// The panel's own settings and the settings popup. MENU opens the popup over
// whatever page shows, the knob picks a row and pressing it changes the row.
// Everything here is pure, the board stores the blob in NVS, applies the
// effects and feeds one Input at a time to handle(), so the host tests drive
// exactly what the panel does.
//
// Rotary direction. Down (turning clockwise) moves the selection down the
// list, the same way it moves to the next page. While a number is edited Up
// raises it and Down lowers it, so the value follows the knob the way a
// volume control does.
//
// Wi-Fi is not stored here. It lives in the shared device config, which only
// the control worker may change, so its row shows the state read only.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "epaper_ui/canvas.h"
#include "epaper_ui/history.h"
#include "epaper_ui/orientation.h"
#include "epaper_ui/pages.h"
#include "epaper_ui/profile.h"
#include "epaper_ui/ui_model.h"

namespace opensensor::ui {

// How often the screen redraws.
enum class RefreshEvery : std::uint8_t { S10, S30, M1, M5 };
// The page shown after a reset.
enum class StartPage : std::uint8_t { Dashboard, LastViewed };
// Idle time before the screen sleeps.
enum class SleepAfter : std::uint8_t { Off, M10, M30, H1 };
// Light is black ink on white. Dark inverts the whole frame, white ink on
// black. The asleep screen stays light either way, so the resting glass is
// mostly white.
enum class Theme : std::uint8_t { Light, Dark };
// How the picture sits on the glass, clockwise on top of the board's mount.
// Auto follows the board's motion sensor and exists only where the profile
// has one.
enum class ScreenRotation : std::uint8_t { R0, R90, R180, R270, Auto };

inline constexpr std::uint16_t kAltitudeMaxM = 3000;
inline constexpr std::uint16_t kAltitudeStepM = 50;
// Tenths of a degree C, -10.0 to +5.0 C in 0.1 C steps.
inline constexpr std::int16_t kTempOffsetMinDc = -100;
inline constexpr std::int16_t kTempOffsetMaxDc = 50;
inline constexpr std::int16_t kTempOffsetStepDc = 1;

struct Settings {
  RefreshEvery refresh = RefreshEvery::S10;
  StartPage start_page = StartPage::Dashboard;
  std::uint8_t last_page = kDashboardPage; // 0 to kPageCount - 1.
  // SEN63C CO2 automatic self calibration. The datasheet wants it on in the
  // field, off is for a lab with a reference gas.
  bool co2_asc = true;
  std::uint16_t altitude_m = 0; // SEN63C CO2 pressure compensation.
  std::int16_t temp_offset_dc = 0;
  SleepAfter sleep_after = SleepAfter::Off;
  Theme theme = Theme::Light;
  ScreenRotation rotation = ScreenRotation::R0;
  bool operator==(const Settings &) const = default;
};

[[nodiscard]] std::uint32_t refresh_seconds(const Settings &s);
// Idle seconds before the screen sleeps, nullopt when it never does.
[[nodiscard]] std::optional<std::uint32_t>
sleep_after_seconds(const Settings &s);
// The page to show after a reset.
[[nodiscard]] std::uint8_t start_page(const Settings &s);

// The defaults for a board. Rotation is Auto with a motion sensor, else 0.
[[nodiscard]] Settings defaults(bool orientation_source);

// The rotation to draw at. Auto takes `sensed`, the board's
// OrientationTracker result, which already reads in panel axes. A fixed
// setting turns the picture on top of the profile's mount.
[[nodiscard]] Rotation effective_rotation(const Settings &s,
                                          const DisplayProfile &profile,
                                          Rotation sensed);

// The NVS blob, version 2, 12 bytes, multi-byte fields little endian.
//   0 version (2)        1 refresh            2 start_page
//   3 last_page          4 co2_asc (0 or 1)   5..6 altitude_m (u16)
//   7..8 temp_offset_dc (i16, two's complement)   9 sleep_after
//   10 theme             11 rotation (ScreenRotation)
// Version 1 is the same layout without byte 11. decode() still reads it,
// keeps every field and gives rotation its default, and the next save
// writes version 2 in place. An image that only knows version 1 reads a
// version 2 blob as all defaults, going back is not supported.
inline constexpr std::uint8_t kSettingsVersion = 2;
inline constexpr std::size_t kSettingsBlobBytes = 12;
using SettingsBlob = std::array<std::uint8_t, kSettingsBlobBytes>;

[[nodiscard]] SettingsBlob encode(const Settings &s);
// Never fails. An unknown version gives defaults(orientation_source). A
// field the blob is too short for, or one out of range, gives that field's
// default. Auto without a motion sensor gives the default rotation.
[[nodiscard]] Settings decode(std::span<const std::uint8_t> blob,
                              bool orientation_source = false);

enum class Row : std::uint8_t {
  Refresh,
  StartPage,
  Theme,
  Rotation,
  Wifi,
  FanCleaning,
  Co2Asc,
  Altitude,
  TempOffset,
  SleepAfter,
};
inline constexpr std::uint8_t kRowCount = 10;

// Board state the popup shows and that decides what Press does.
struct SettingsInfo {
  bool wifi_configured = false; // The device config has an SSID.
  bool wifi_on = false;
  bool sensor_present = false;
  bool fan_cleaning = false; // A fan cleaning runs now.
  // The profile has a motion sensor, so Rotation offers Auto.
  bool orientation_source = false;
};

struct PopupState {
  bool open = false;
  Row selected = Row::Refresh;
  bool editing = false;        // Altitude or TempOffset is being adjusted.
  std::int32_t edit_value = 0; // The value shown while editing.
};

// What the board does after an input.
enum class [[nodiscard]] Effect : std::uint8_t {
  None,
  Opened,   // The popup opened.
  Changed,  // Settings changed, save 5 s after the last change and apply.
  FanClean, // Start a SEN63C fan cleaning.
  Closed,   // The popup closed.
};

// One input while the popup is open, or MENU to open it. Up and Down move the
// selection and wrap. Press cycles a choice row and returns Changed. Rotation
// cycles Auto, 0, 90, 180, 270 with a motion sensor and 0 to 270 without.
// Press on Altitude or TempOffset starts an edit, Up and Down then step and
// clamp, Press keeps the value (Changed when it differs) and EXIT cancels.
// Press on Wi-Fi does nothing. Press on FanCleaning returns FanClean only
// with a sensor that is not cleaning already. MENU or EXIT closes the popup,
// MENU while editing drops the edit and closes too.
[[nodiscard]] Effect popup_input(PopupState &p, Settings &s,
                                 const SettingsInfo &board, Input in);

// The whole input state of the panel.
struct UiState {
  std::uint8_t page = kDashboardPage;
  PopupState popup{};
  // Set by fall_asleep(). The next input only wakes the screen.
  bool asleep = false;
};

// The board calls this once the screen has been idle for
// sleep_after_seconds(). It closes the popup and drops an unsaved edit.
void fall_asleep(UiState &ui);

struct Outcome {
  bool page_changed = false;
  bool full_refresh = false;
  Effect effect = Effect::None;
};

// The one entry point for a key. Any key wakes a sleeping screen and does
// nothing else. With the popup open the key goes to popup_input(). Otherwise
// MENU opens the popup and every other key goes to navigate(). With
// StartPage::LastViewed a page change updates last_page and returns Changed.
// A theme or rotation change asks for a full refresh, a partial one would
// leave the old frame ghosting under every pixel that flipped.
[[nodiscard]] Outcome handle(UiState &ui, Settings &s,
                             const SettingsInfo &board, Input in);

// The label and the value text of one row, as the popup draws them.
[[nodiscard]] std::string_view row_label(Row r);
[[nodiscard]] std::string_view row_value(std::span<char> buf, Row r,
                                         const Settings &s, const PopupState &p,
                                         const SettingsInfo &board);

// The framed popup over whatever the canvas holds.
void render_settings(Canvas &c, const Settings &s, const PopupState &p,
                     const SettingsInfo &board);

// The full frame for the state. The pairing screen wins over sleep and the
// popup, so a passkey always shows. Asleep draws render_asleep(). Otherwise
// the page, with the popup over it when open. Theme::Dark inverts the whole
// frame at the end, except the asleep screen, which always stays light.
void render_screen(Canvas &c, const UiState &ui, const Settings &s,
                   const SettingsInfo &board, const Model &model,
                   const History &history);

} // namespace opensensor::ui
