// Every word the e-paper pages draw, in English and Egyptian Arabic, one row
// per line. The row order is the id order, and epaper_ui's Str is this
// StringId, so this file is the single string table.
//
// OS_TEXT(Id, "en", "ar_EG") is translated and drawn in its language's natural
// direction. OS_TEXT_LTR(Id, "text") is the same in both languages and always
// stays left to right, a clock, a unit, a metric name or a number format.
// Units, sensor names, protocol names and digits stay Latin in the Arabic.
//
// A format row holds printf conversions, its argument list is fixed by the
// id, and every conversion appears in both languages in the same order. Pass
// it to snprintf through c_str() or tr_fmt(), never through a string_view.
//
// tools/gen_arabic_fonts.py reads this file to pick the Arabic glyphs it
// bakes, so run pixi run text-fonts after changing an Arabic string. Keep
// each row on one line with no escapes, the generator parses it.

// Header. ClockFmt takes %02d hours and %02d minutes.
OS_TEXT_LTR(Brand, "OpenSensor")
OS_TEXT_LTR(ClockFmt, "%02d:%02d UTC")
OS_TEXT_LTR(ClockUnset, "--:-- UTC")
OS_TEXT_LTR(TagWifi, "WiFi")
OS_TEXT_LTR(TagBle, "BLE")
OS_TEXT_LTR(TagSd, "SD")
OS_TEXT(TagNoSd, "no SD", "مفيش SD")

// A value that is not valid, and why. WarmingFmt takes %u seconds left.
OS_TEXT_LTR(NoValue, "--")
OS_TEXT(WarmingFmt, "warming %u s", "بيسخن %u ث")
OS_TEXT(NoReading, "no reading", "مفيش قراية")
OS_TEXT(NoSensor, "no sensor", "مفيش حساس")

// Dashboard hero. MaxFmt and MinFmt take %.1f.
OS_TEXT(Last2Hours, "Last 2 hours", "آخر ساعتين")
OS_TEXT(MaxFmt, "max %.1f", "أعلى %.1f")
OS_TEXT(MinFmt, "min %.1f", "أقل %.1f")
OS_TEXT_LTR(Minus2h, "-2 h")
OS_TEXT(Collecting, "collecting", "لسه بيجمع")

// Chart pages. ChartSubFmt takes the unit as %.*s. The Arabic puts it last,
// and an Arabic page draws it as its own left to right run.
OS_TEXT(ChartSubFmt, "%.*s, last 24 h", "آخر 24 ساعة %.*s")
OS_TEXT(StatNow, "now", "دلوقتي")
OS_TEXT(StatMin, "min", "أقل")
OS_TEXT(StatMax, "max", "أعلى")
OS_TEXT_LTR(AxisMinus24h, "-24 h")
OS_TEXT_LTR(AxisMinus18h, "-18 h")
OS_TEXT_LTR(AxisMinus12h, "-12 h")
OS_TEXT_LTR(AxisMinus6h, "-6 h")
OS_TEXT(AxisNow, "now", "دلوقتي")
OS_TEXT(NoDataYet, "No data yet", "لسه مفيش بيانات")

// Status page. ClockDateFmt takes %04d-%02u-%02u %02d:%02d, UptimeDaysFmt
// %u days, %02u hours and %02u minutes, SdOkFmt %.2f GB free. SensorFmt
// takes the model, serial and firmware as %.*s, SensorMissingFmt the model.
OS_TEXT(StatusTitle, "Status", "الحالة")
OS_TEXT(RowClock, "Clock", "الساعة")
OS_TEXT_LTR(ClockDateFmt, "%04d-%02u-%02u %02d:%02d UTC")
OS_TEXT(ClockNotSet, "not set, waiting for time", "مش مظبوطة، مستنية الوقت")
OS_TEXT(RowUptime, "Uptime", "شغال بقاله")
OS_TEXT(UptimeDaysFmt, "%u d %02u:%02u", "%u يوم %02u:%02u")
OS_TEXT(RowSamples, "Samples", "العينات")
OS_TEXT(RowSdCard, "SD card", "كارت SD")
OS_TEXT(SdOkFmt, "ok, %.2f GB free", "تمام، فاضي %.2f GB")
OS_TEXT(SdMissing, "missing or failed", "مش موجود أو بايظ")
OS_TEXT(RowBluetooth, "Bluetooth", "بلوتوث")
OS_TEXT(RowWifi, "Wi-Fi", "واي فاي")
OS_TEXT(LinkOff, "off", "مقفول")
OS_TEXT(LinkAdvertising, "advertising", "مستني الموبايل")
OS_TEXT(LinkConnecting, "connecting", "بيحاول يتوصل")
OS_TEXT(LinkConnected, "connected", "متوصل")
OS_TEXT(RowThing, "Thing", "كود الجهاز")
OS_TEXT(ThingUnset, "unset", "مش متحدد")
OS_TEXT(RowFirmware, "Firmware", "السوفتوير")
OS_TEXT(RowSensor, "Sensor", "الحساس")
OS_TEXT_LTR(SensorFmt, "%.*s %.*s, fw %.*s")
OS_TEXT(SensorMissingFmt, "%.*s not found on I2C", "مش لاقيين %.*s على I2C")
OS_TEXT(RowSensorState, "Sensor state", "حالة الحساس")
OS_TEXT(SensorOk, "ok", "تمام")
OS_TEXT(SensorNotRead, "not read", "لسه متقرتش")
OS_TEXT(BitFanSpeed, "fan speed warn", "سرعة المروحة")
OS_TEXT(BitCo2, "CO₂ error", "عطل CO₂")
OS_TEXT(BitPm, "PM error", "عطل PM")
OS_TEXT(BitRht, "RH/T error", "عطل RH/T")
OS_TEXT(BitFan, "fan error", "عطل المروحة")
OS_TEXT(ListSep, ", ", "، ")
OS_TEXT(BandFootnote, "PM2.5 word uses the US EPA 24 h AQI bands as a guide", "وصف PM2.5 حسب درجات AQI بتاعة EPA الأمريكية")

// Pairing. PairDeviceFmt takes the device name as %.*s.
OS_TEXT(PairTitle, "Pair with phone", "اربط بالموبايل")
OS_TEXT(PairPrompt, "Enter this code on the phone", "اكتب الكود ده على الموبايل")
OS_TEXT(PairDeviceFmt, "Device %.*s", "الجهاز %.*s")
OS_TEXT(PairFootnote, "Only someone reading this screen can pair.", "اللي شايف الشاشة دي بس هو اللي يقدر يربط.")

// Asleep.
OS_TEXT(AsleepTitle, "Screen is asleep", "الشاشة نايمة")
OS_TEXT(AsleepHint, "Press any button to wake it", "دوس على أي زرار عشان تصحيها")
OS_TEXT(AsleepDevice, "Device", "الجهاز")

// Settings popup labels, in row order.
OS_TEXT(SettingsTitle, "Settings", "الإعدادات")
OS_TEXT(LabelRefresh, "Screen refresh", "تحديث الشاشة")
OS_TEXT(LabelStartOn, "Start on", "صفحة البداية")
OS_TEXT(LabelTheme, "Theme", "المظهر")
OS_TEXT(LabelRotation, "Rotation", "اتجاه الشاشة")
OS_TEXT(LabelLanguage, "Language", "اللغة")
OS_TEXT(LabelWifi, "Wi-Fi", "واي فاي")
OS_TEXT(LabelFanCleaning, "Fan cleaning", "تنضيف المروحة")
OS_TEXT(LabelCo2Asc, "CO₂ calibration", "معايرة CO₂")
OS_TEXT(LabelAltitude, "Altitude", "الارتفاع")
OS_TEXT(LabelTempOffset, "Temp offset", "تعديل الحرارة")
OS_TEXT(LabelSleepAfter, "Sleep after", "تنام بعد")

// Settings values. AltitudeFmt takes %d metres, TempOffsetFmt a %s sign,
// %d whole degrees and %d tenths.
OS_TEXT(Every10s, "10 s", "10 ث")
OS_TEXT(Every30s, "30 s", "30 ث")
OS_TEXT(Every1min, "1 min", "دقيقة")
OS_TEXT(Every5min, "5 min", "5 دقايق")
OS_TEXT(On, "On", "شغال")
OS_TEXT(Off, "Off", "مقفول")
OS_TEXT(After10min, "10 min", "10 دقايق")
OS_TEXT(After30min, "30 min", "نص ساعة")
OS_TEXT(After1h, "1 h", "ساعة")
OS_TEXT(StartLastPage, "Last page", "آخر صفحة")
OS_TEXT(StartDashboard, "Dashboard", "الرئيسية")
OS_TEXT(ThemeDark, "Dark", "غامق")
OS_TEXT(ThemeLight, "Light", "فاتح")
OS_TEXT(RotateAuto, "Auto", "تلقائي")
OS_TEXT_LTR(Rotate0, "0°")
OS_TEXT_LTR(Rotate90, "90°")
OS_TEXT_LTR(Rotate180, "180°")
OS_TEXT_LTR(Rotate270, "270°")
OS_TEXT(LangEnglish, "English", "إنجليزي")
OS_TEXT(LangArabic, "Arabic", "عربي")
OS_TEXT(WifiNotSetUp, "Not set up", "مش متظبط")
OS_TEXT(FanNoSensor, "No sensor", "مفيش حساس")
OS_TEXT(FanRunning, "Running", "بينضف")
OS_TEXT(FanRunNow, "Run now", "نضف دلوقتي")
OS_TEXT(AscOffLab, "Off (lab only)", "مقفول (للمعمل بس)")
OS_TEXT_LTR(AltitudeFmt, "%d m")
OS_TEXT_LTR(TempOffsetFmt, "%s%d.%d °C")
OS_TEXT(HintEdit, "Turn to adjust, press to keep, EXIT to cancel", "لف وعدل، دوس واحفظ، EXIT للإلغاء")
OS_TEXT(HintWifiSet, "Wi-Fi is switched from the phone app", "الواي فاي بيتشغل من أبلكيشن الموبايل")
OS_TEXT(HintWifiUnset, "Set up Wi-Fi from the phone app", "اظبط الواي فاي من أبلكيشن الموبايل")
OS_TEXT(HintCo2Asc, "Keep on in the field, off is for lab tests", "سيبه شغال، واقفله لتجارب المعمل بس")
OS_TEXT(HintPick, "Turn to pick, press to change, MENU to close", "لف واختار، دوس وغير، MENU للقفل")
OS_TEXT_LTR(ArrowDown, "↓")
OS_TEXT_LTR(ArrowUp, "↑")
OS_TEXT_LTR(Ellipsis, "..")

// Metric labels and units.
OS_TEXT_LTR(MetricPm1, "PM1")
OS_TEXT_LTR(MetricPm25, "PM2.5")
OS_TEXT_LTR(MetricPm4, "PM4")
OS_TEXT_LTR(MetricPm10, "PM10")
OS_TEXT_LTR(MetricCo2, "CO₂")
OS_TEXT(MetricTemp, "Temp", "الحرارة")
OS_TEXT(MetricRh, "Humidity", "الرطوبة")
OS_TEXT_LTR(UnitUgM3, "µg/m³")
OS_TEXT_LTR(UnitPpm, "ppm")
OS_TEXT_LTR(UnitCelsius, "°C")
OS_TEXT_LTR(UnitPercentRh, "%RH")

// PM2.5 band words.
OS_TEXT(BandGood, "Good", "كويس")
OS_TEXT(BandModerate, "Moderate", "متوسط")
OS_TEXT(BandSensitive, "Sensitive groups", "مش كويس للحساسين")
OS_TEXT(BandUnhealthy, "Unhealthy", "وحش")
OS_TEXT(BandVeryUnhealthy, "Very unhealthy", "وحش قوي")
OS_TEXT(BandHazardous, "Hazardous", "خطر")

// The settings storage fault page (#177), drawn when NVS cannot start. Both
// languages show at once, because the language setting lives in NVS too.
// StorageFaultCodeFmt takes the ESP-IDF error as %d.
OS_TEXT(StorageFaultTitle, "Settings storage error", "مشكلة في ذاكرة الإعدادات")
OS_TEXT(StorageFaultOff, "Logging is off", "التسجيل واقف")
OS_TEXT(StorageFaultHelp, "Read the USB serial log and see the board guide", "اقرا سجل USB وشوف دليل الجهاز")
OS_TEXT_LTR(StorageFaultCodeFmt, "nvs-init error=%d")
