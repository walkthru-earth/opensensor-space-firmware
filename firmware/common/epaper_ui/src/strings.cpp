#include "epaper_ui/strings.h"

#include <array>
#include <cstddef>
#include <utility>

namespace opensensor::ui {
namespace {

struct Entry {
  Str id{};
  const char *text = nullptr;
};

// English. Each row names its id, and the check below proves the rows are
// in id order with none missing, so the lookup is a plain index.
static constexpr std::array kEnglish{
    Entry{Str::Brand, "OpenSensor"},
    Entry{Str::ClockFmt, "%02d:%02d UTC"},
    Entry{Str::ClockUnset, "--:-- UTC"},
    Entry{Str::TagWifi, "WiFi"},
    Entry{Str::TagBle, "BLE"},
    Entry{Str::TagSd, "SD"},
    Entry{Str::TagNoSd, "no SD"},
    Entry{Str::NoValue, "--"},
    Entry{Str::WarmingFmt, "warming %u s"},
    Entry{Str::NoReading, "no reading"},
    Entry{Str::NoSensor, "no sensor"},
    Entry{Str::Last2Hours, "Last 2 hours"},
    Entry{Str::MaxFmt, "max %.1f"},
    Entry{Str::MinFmt, "min %.1f"},
    Entry{Str::Minus2h, "-2 h"},
    Entry{Str::Collecting, "collecting"},
    Entry{Str::ChartSubFmt, "%.*s, last 24 h"},
    Entry{Str::StatNow, "now"},
    Entry{Str::StatMin, "min"},
    Entry{Str::StatMax, "max"},
    Entry{Str::AxisMinus24h, "-24 h"},
    Entry{Str::AxisMinus18h, "-18 h"},
    Entry{Str::AxisMinus12h, "-12 h"},
    Entry{Str::AxisMinus6h, "-6 h"},
    Entry{Str::AxisNow, "now"},
    Entry{Str::NoDataYet, "No data yet"},
    Entry{Str::StatusTitle, "Status"},
    Entry{Str::RowClock, "Clock"},
    Entry{Str::ClockDateFmt, "%04d-%02u-%02u %02d:%02d UTC"},
    Entry{Str::ClockNotSet, "not set, waiting for time"},
    Entry{Str::RowUptime, "Uptime"},
    Entry{Str::UptimeDaysFmt, "%u d %02u:%02u"},
    Entry{Str::RowSamples, "Samples"},
    Entry{Str::RowSdCard, "SD card"},
    Entry{Str::SdOkFmt, "ok, %.2f GB free"},
    Entry{Str::SdMissing, "missing or failed"},
    Entry{Str::RowBluetooth, "Bluetooth"},
    Entry{Str::RowWifi, "Wi-Fi"},
    Entry{Str::LinkOff, "off"},
    Entry{Str::LinkAdvertising, "advertising"},
    Entry{Str::LinkConnecting, "connecting"},
    Entry{Str::LinkConnected, "connected"},
    Entry{Str::RowThing, "Thing"},
    Entry{Str::ThingUnset, "unset"},
    Entry{Str::RowFirmware, "Firmware"},
    Entry{Str::RowSensor, "Sensor"},
    Entry{Str::SensorFmt, "%.*s %.*s, fw %.*s"},
    Entry{Str::SensorMissingFmt, "%.*s not found on I2C"},
    Entry{Str::RowSensorState, "Sensor state"},
    Entry{Str::SensorOk, "ok"},
    Entry{Str::SensorNotRead, "not read"},
    Entry{Str::BitFanSpeed, "fan speed warn"},
    Entry{Str::BitCo2, "CO₂ error"},
    Entry{Str::BitPm, "PM error"},
    Entry{Str::BitRht, "RH/T error"},
    Entry{Str::BitFan, "fan error"},
    Entry{Str::BandFootnote,
          "PM2.5 word uses the US EPA 24 h AQI bands as a guide"},
    Entry{Str::PairTitle, "Pair with phone"},
    Entry{Str::PairPrompt, "Enter this code on the phone"},
    Entry{Str::PairDeviceFmt, "Device %.*s"},
    Entry{Str::PairFootnote, "Only someone reading this screen can pair."},
    Entry{Str::AsleepTitle, "Screen is asleep"},
    Entry{Str::AsleepHint, "Press any button to wake it"},
    Entry{Str::AsleepDevice, "Device"},
    Entry{Str::SettingsTitle, "Settings"},
    Entry{Str::LabelRefresh, "Screen refresh"},
    Entry{Str::LabelStartOn, "Start on"},
    Entry{Str::LabelTheme, "Theme"},
    Entry{Str::LabelRotation, "Rotation"},
    Entry{Str::LabelWifi, "Wi-Fi"},
    Entry{Str::LabelFanCleaning, "Fan cleaning"},
    Entry{Str::LabelCo2Asc, "CO₂ calibration"},
    Entry{Str::LabelAltitude, "Altitude"},
    Entry{Str::LabelTempOffset, "Temp offset"},
    Entry{Str::LabelSleepAfter, "Sleep after"},
    Entry{Str::Every10s, "10 s"},
    Entry{Str::Every30s, "30 s"},
    Entry{Str::Every1min, "1 min"},
    Entry{Str::Every5min, "5 min"},
    Entry{Str::On, "On"},
    Entry{Str::Off, "Off"},
    Entry{Str::After10min, "10 min"},
    Entry{Str::After30min, "30 min"},
    Entry{Str::After1h, "1 h"},
    Entry{Str::StartLastPage, "Last page"},
    Entry{Str::StartDashboard, "Dashboard"},
    Entry{Str::ThemeDark, "Dark"},
    Entry{Str::ThemeLight, "Light"},
    Entry{Str::RotateAuto, "Auto"},
    Entry{Str::Rotate0, "0°"},
    Entry{Str::Rotate90, "90°"},
    Entry{Str::Rotate180, "180°"},
    Entry{Str::Rotate270, "270°"},
    Entry{Str::WifiNotSetUp, "Not set up"},
    Entry{Str::FanNoSensor, "No sensor"},
    Entry{Str::FanRunning, "Running"},
    Entry{Str::FanRunNow, "Run now"},
    Entry{Str::AscOffLab, "Off (lab only)"},
    Entry{Str::AltitudeFmt, "%d m"},
    Entry{Str::TempOffsetFmt, "%s%d.%d °C"},
    Entry{Str::HintEdit, "Turn to adjust, press to keep, EXIT to cancel"},
    Entry{Str::HintWifiSet, "Wi-Fi is switched from the phone app"},
    Entry{Str::HintWifiUnset, "Set up Wi-Fi from the phone app"},
    Entry{Str::HintCo2Asc, "Keep on in the field, off is for lab tests"},
    Entry{Str::HintPick, "Turn to pick, press to change, MENU to close"},
    Entry{Str::ArrowDown, "↓"},
    Entry{Str::ArrowUp, "↑"},
    Entry{Str::Ellipsis, ".."},
    Entry{Str::MetricPm1, "PM1"},
    Entry{Str::MetricPm25, "PM2.5"},
    Entry{Str::MetricPm4, "PM4"},
    Entry{Str::MetricPm10, "PM10"},
    Entry{Str::MetricCo2, "CO₂"},
    Entry{Str::MetricTemp, "Temp"},
    Entry{Str::MetricRh, "Humidity"},
    Entry{Str::UnitUgM3, "µg/m³"},
    Entry{Str::UnitPpm, "ppm"},
    Entry{Str::UnitCelsius, "°C"},
    Entry{Str::UnitPercentRh, "%RH"},
    Entry{Str::BandGood, "Good"},
    Entry{Str::BandModerate, "Moderate"},
    Entry{Str::BandSensitive, "Sensitive groups"},
    Entry{Str::BandUnhealthy, "Unhealthy"},
    Entry{Str::BandVeryUnhealthy, "Very unhealthy"},
    Entry{Str::BandHazardous, "Hazardous"},
};

consteval bool in_id_order() {
  if (kEnglish.size() != std::to_underlying(Str::Count))
    return false;
  for (std::size_t i = 0; i < kEnglish.size(); ++i)
    if (std::to_underlying(kEnglish[i].id) != i || kEnglish[i].text == nullptr)
      return false;
  return true;
}
static_assert(in_id_order(), "kEnglish rows must follow the Str ids");

// Only the text pointers reach flash, the ids above are a build time check.
consteval std::array<const char *, kEnglish.size()> texts() {
  std::array<const char *, kEnglish.size()> out{};
  for (std::size_t i = 0; i < kEnglish.size(); ++i)
    out[i] = kEnglish[i].text;
  return out;
}
static constexpr auto kText = texts();

} // namespace

const char *tr_fmt(Str id) {
  const auto i = std::to_underlying(id);
  return i < kText.size() ? kText[i] : "";
}

std::string_view tr(Str id) { return tr_fmt(id); }

} // namespace opensensor::ui
