// UI strings in English and Egyptian Arabic, one per line. The English text
// matches the e-paper pages word for word. Units, sensor names and protocol
// names stay in Latin script inside the Arabic text.
//
// OS_TEXT(Id, "en", "ar_EG") is translated and drawn in its language's natural
// direction. OS_TEXT_LTR(Id, "text") is the same in both languages and always
// stays left to right, a clock, a unit, a metric name or a number format.
//
// tools/gen_arabic_fonts.py reads this file to pick the Arabic glyphs it bakes,
// so run pixi run text-fonts after changing an Arabic string. Each printf
// conversion must appear in both languages in the same order.

// Header.
OS_TEXT_LTR(Brand, "OpenSensor")
OS_TEXT_LTR(ClockUnset, "--:-- UTC")
OS_TEXT_LTR(ClockHm, "%02d:%02d UTC")
OS_TEXT_LTR(TagWifi, "WiFi")
OS_TEXT_LTR(TagBle, "BLE")
OS_TEXT_LTR(TagSd, "SD")
OS_TEXT(TagNoSd, "no SD", "مفيش SD")

// Tile captions.
OS_TEXT(Warming, "warming %u s", "بيسخن %u ث")
OS_TEXT(NoReading, "no reading", "مفيش قراية")
OS_TEXT(NoSensorCaption, "no sensor", "مفيش حساس")

// Dashboard hero.
OS_TEXT(LastTwoHours, "Last 2 hours", "آخر ساعتين")
OS_TEXT(MaxValue, "max %.1f", "أعلى %.1f")
OS_TEXT(MinValue, "min %.1f", "أقل %.1f")
OS_TEXT(TwoHoursAgo, "-2 h", "من ساعتين")
OS_TEXT(Collecting, "collecting", "لسه بيجمع")

// PM2.5 bands.
OS_TEXT(BandGood, "Good", "كويس")
OS_TEXT(BandModerate, "Moderate", "متوسط")
OS_TEXT(BandSensitive, "Sensitive groups", "مش كويس للحساسين")
OS_TEXT(BandUnhealthy, "Unhealthy", "وحش")
OS_TEXT(BandVeryUnhealthy, "Very unhealthy", "وحش قوي")
OS_TEXT(BandHazardous, "Hazardous", "خطر")

// Metric labels and units.
OS_TEXT_LTR(MetricPm1, "PM1")
OS_TEXT_LTR(MetricPm25, "PM2.5")
OS_TEXT_LTR(MetricPm4, "PM4")
OS_TEXT_LTR(MetricPm10, "PM10")
OS_TEXT_LTR(MetricCo2, "CO₂")
OS_TEXT(MetricTemp, "Temp", "الحرارة")
OS_TEXT(MetricHumidity, "Humidity", "الرطوبة")
OS_TEXT_LTR(UnitMicrogram, "µg/m³")
OS_TEXT_LTR(UnitPpm, "ppm")
OS_TEXT_LTR(UnitCelsius, "°C")
OS_TEXT_LTR(UnitRh, "%RH")

// Metric chart pages.
OS_TEXT_LTR(PageCount, "%u/%u")
OS_TEXT(ChartSubtitle, "%.*s, last 24 h", "%.*s، آخر 24 ساعة")
OS_TEXT(Now, "now", "دلوقتي")
OS_TEXT(Min, "min", "أقل")
OS_TEXT(Max, "max", "أعلى")
OS_TEXT(Ago24h, "-24 h", "من 24 ساعة")
OS_TEXT(Ago18h, "-18 h", "من 18 ساعة")
OS_TEXT(Ago12h, "-12 h", "من 12 ساعة")
OS_TEXT(Ago6h, "-6 h", "من 6 ساعات")
OS_TEXT(NoDataYet, "No data yet", "لسه مفيش بيانات")

// Link states.
OS_TEXT(LinkOff, "off", "مقفول")
OS_TEXT(LinkConnected, "connected", "متوصل")
OS_TEXT(LinkAdvertising, "advertising", "مستني الموبايل")
OS_TEXT(LinkConnecting, "connecting", "بيحاول يتوصل")

// SEN63C device status bits.
OS_TEXT(SensorFanSpeedWarn, "fan speed warn", "تحذير سرعة المروحة")
OS_TEXT(SensorCo2Error, "CO₂ error", "عطل CO₂")
OS_TEXT(SensorPmError, "PM error", "عطل PM")
OS_TEXT(SensorRhtError, "RH/T error", "عطل RH/T")
OS_TEXT(SensorFanError, "fan error", "عطل المروحة")
OS_TEXT(SensorOk, "ok", "تمام")

// Status page.
OS_TEXT(Status, "Status", "الحالة")
OS_TEXT(Clock, "Clock", "الساعة")
OS_TEXT_LTR(ClockDateTime, "%04d-%02u-%02u %02d:%02d UTC")
OS_TEXT(ClockNotSet, "not set, waiting for time", "مش مظبوطة، مستنية الوقت")
OS_TEXT(Uptime, "Uptime", "شغال بقاله")
OS_TEXT(UptimeDays, "%u d %02u:%02u", "%u يوم %02u:%02u")
OS_TEXT_LTR(UptimeHms, "%02u:%02u:%02u")
OS_TEXT(Samples, "Samples", "العينات")
OS_TEXT(SdCard, "SD card", "كارت SD")
OS_TEXT(SdFree, "ok, %.2f GB free", "تمام، فاضي %.2f GB")
OS_TEXT(SdMissing, "missing or failed", "مش موجود أو بايظ")
OS_TEXT(Bluetooth, "Bluetooth", "بلوتوث")
OS_TEXT(WiFi, "Wi-Fi", "واي فاي")
OS_TEXT(Thing, "Thing", "كود الجهاز")
OS_TEXT(Unset, "unset", "مش متحدد")
OS_TEXT(Firmware, "Firmware", "السوفتوير")
OS_TEXT(Sensor, "Sensor", "الحساس")
OS_TEXT_LTR(SensorVersion, "SEN63C %.*s, fw %.*s")
OS_TEXT(SensorState, "Sensor state", "حالة الحساس")
OS_TEXT(NotRead, "not read", "لسه متقرتش")
OS_TEXT(SensorNotFound, "SEN63C not found on I2C", "مش لاقيين SEN63C على I2C")
OS_TEXT(BandNote, "PM2.5 word uses the US EPA 24 h AQI bands as a guide", "وصف PM2.5 ماشي على درجات AQI الأمريكية (EPA) لـ 24 ساعة، للاسترشاد بس")

// Pairing screen.
OS_TEXT(PairTitle, "Pair with phone", "اربط بالموبايل")
OS_TEXT(PairEnterCode, "Enter this code on the phone", "اكتب الكود ده على الموبايل")
OS_TEXT(PairDevice, "Device %.*s", "اسم الجهاز %.*s")
OS_TEXT(PairOnlyReader, "Only someone reading this screen can pair.", "اللي شايف الشاشة دي بس هو اللي يقدر يربط.")

// Sleeping screen.
OS_TEXT(Asleep, "Screen is asleep", "الشاشة نايمة")
OS_TEXT(AsleepWake, "Press any button to wake it", "دوس على أي زرار عشان تصحيها")
OS_TEXT(Device, "Device", "اسم الجهاز")

// Settings rows.
OS_TEXT(Settings, "Settings", "الإعدادات")
OS_TEXT(RowRefresh, "Screen refresh", "تحديث الشاشة")
OS_TEXT(RowStartOn, "Start on", "صفحة البداية")
OS_TEXT(RowTheme, "Theme", "المظهر")
OS_TEXT(RowFanCleaning, "Fan cleaning", "تنضيف المروحة")
OS_TEXT(RowCo2Calibration, "CO₂ calibration", "معايرة CO₂")
OS_TEXT(RowAltitude, "Altitude", "الارتفاع")
OS_TEXT(RowTempOffset, "Temp offset", "تعديل الحرارة")
OS_TEXT(RowSleepAfter, "Sleep after", "تنام بعد")

// Settings values.
OS_TEXT(Refresh10s, "10 s", "10 ث")
OS_TEXT(Refresh30s, "30 s", "30 ث")
OS_TEXT(Refresh1min, "1 min", "دقيقة")
OS_TEXT(Refresh5min, "5 min", "5 دقايق")
OS_TEXT(Off, "Off", "مقفول")
OS_TEXT(Sleep10min, "10 min", "10 دقايق")
OS_TEXT(Sleep30min, "30 min", "نص ساعة")
OS_TEXT(Sleep1h, "1 h", "ساعة")
OS_TEXT(StartLastPage, "Last page", "آخر صفحة")
OS_TEXT(StartDashboard, "Dashboard", "الرئيسية")
OS_TEXT(ThemeDark, "Dark", "غامق")
OS_TEXT(ThemeLight, "Light", "فاتح")
OS_TEXT(NotSetUp, "Not set up", "مش متظبط")
OS_TEXT(On, "On", "شغال")
OS_TEXT(NoSensor, "No sensor", "مفيش حساس")
OS_TEXT(FanRunning, "Running", "بينضف")
OS_TEXT(FanRunNow, "Run now", "نضف دلوقتي")
OS_TEXT(OffLabOnly, "Off (lab only)", "مقفول (للمعمل بس)")
OS_TEXT_LTR(AltitudeMeters, "%d m")
OS_TEXT_LTR(TempOffsetValue, "%s%d.%d °C")

// Settings hints.
OS_TEXT(HintAdjust, "Turn to adjust, press to keep, EXIT to cancel", "لف عشان تعدل، دوس عشان تحفظ، EXIT للإلغاء")
OS_TEXT(HintWifiSwitched, "Wi-Fi is switched from the phone app", "الواي فاي بيتشغل من أبلكيشن الموبايل")
OS_TEXT(HintWifiSetUp, "Set up Wi-Fi from the phone app", "اظبط الواي فاي من أبلكيشن الموبايل")
OS_TEXT(HintFieldUse, "Keep on in the field, off is for lab tests", "خليه شغال في الاستخدام العادي، والقفل لتجارب المعمل بس")
OS_TEXT(HintPick, "Turn to pick, press to change, MENU to close", "لف عشان تختار، دوس عشان تغير، MENU للقفل")
