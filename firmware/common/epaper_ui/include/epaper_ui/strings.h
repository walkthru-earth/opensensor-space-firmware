#pragma once

// Every word the pages draw, by id. One English table today. A translation
// is a second table of the same ids, picked at run time, plus the glyphs its
// script needs in the font sets, and right to left or joined scripts also
// change shape() in canvas.h. Nothing here translates yet.
//
// A format id holds printf conversions. Its argument list is fixed by the id,
// so a translation keeps the same conversions in the same order. Pass it to
// snprintf through tr_fmt(), never through tr().data().

#include <cstdint>
#include <string_view>

namespace opensensor::ui {

enum class Str : std::uint16_t {
  // Header.
  Brand,
  ClockFmt, // %02d hours, %02d minutes.
  ClockUnset,
  TagWifi,
  TagBle,
  TagSd,
  TagNoSd,
  // A value that is not valid, and why.
  NoValue,
  WarmingFmt, // %u seconds left.
  NoReading,
  NoSensor,
  // Dashboard hero.
  Last2Hours,
  MaxFmt, // %.1f
  MinFmt, // %.1f
  Minus2h,
  Collecting,
  // Chart pages.
  ChartSubFmt, // %.*s unit.
  StatNow,
  StatMin,
  StatMax,
  AxisMinus24h,
  AxisMinus18h,
  AxisMinus12h,
  AxisMinus6h,
  AxisNow,
  NoDataYet,
  // Status page.
  StatusTitle,
  RowClock,
  ClockDateFmt, // %04d-%02u-%02u %02d:%02d
  ClockNotSet,
  RowUptime,
  UptimeDaysFmt, // %u days, %02u hours, %02u minutes.
  RowSamples,
  RowSdCard,
  SdOkFmt, // %.2f GB free.
  SdMissing,
  RowBluetooth,
  RowWifi,
  LinkOff,
  LinkAdvertising,
  LinkConnecting,
  LinkConnected,
  RowThing,
  ThingUnset,
  RowFirmware,
  RowSensor,
  SensorFmt,        // %.*s model, %.*s serial, %.*s firmware.
  SensorMissingFmt, // %.*s model.
  RowSensorState,
  SensorOk,
  SensorNotRead,
  BitFanSpeed,
  BitCo2,
  BitPm,
  BitRht,
  BitFan,
  BandFootnote,
  // Pairing.
  PairTitle,
  PairPrompt,
  PairDeviceFmt, // %.*s device name.
  PairFootnote,
  // Asleep.
  AsleepTitle,
  AsleepHint,
  AsleepDevice,
  // Settings popup.
  SettingsTitle,
  LabelRefresh,
  LabelStartOn,
  LabelTheme,
  LabelRotation,
  LabelWifi,
  LabelFanCleaning,
  LabelCo2Asc,
  LabelAltitude,
  LabelTempOffset,
  LabelSleepAfter,
  Every10s,
  Every30s,
  Every1min,
  Every5min,
  On,
  Off,
  After10min,
  After30min,
  After1h,
  StartLastPage,
  StartDashboard,
  ThemeDark,
  ThemeLight,
  RotateAuto,
  Rotate0,
  Rotate90,
  Rotate180,
  Rotate270,
  WifiNotSetUp,
  FanNoSensor,
  FanRunning,
  FanRunNow,
  AscOffLab,
  AltitudeFmt,   // %d metres.
  TempOffsetFmt, // %s sign, %d whole, %d tenths of a degree C.
  HintEdit,
  HintWifiSet,
  HintWifiUnset,
  HintCo2Asc,
  HintPick,
  ArrowDown,
  ArrowUp,
  Ellipsis,
  // Metric labels and units.
  MetricPm1,
  MetricPm25,
  MetricPm4,
  MetricPm10,
  MetricCo2,
  MetricTemp,
  MetricRh,
  UnitUgM3,
  UnitPpm,
  UnitCelsius,
  UnitPercentRh,
  // PM2.5 band words.
  BandGood,
  BandModerate,
  BandSensitive,
  BandUnhealthy,
  BandVeryUnhealthy,
  BandHazardous,
  Count,
};

// The UTF-8 text of `id`, empty for an id out of range.
[[nodiscard]] std::string_view tr(Str id);

// The same text as a NUL terminated C string, for a format id passed to
// snprintf. Never null.
[[nodiscard]] const char *tr_fmt(Str id);

} // namespace opensensor::ui
