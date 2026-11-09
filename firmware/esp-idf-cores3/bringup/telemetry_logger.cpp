#include "telemetry_logger.h"
#include "debug_log.h"
#include "ltr553.h"
#include "telemetry_contract.h"
#include <M5Unified.h>
#include <array>
#include <ctime>
#include <esp_vfs_fat.h>
#include <os_logger.h>
#include <os_stac_item.h>
#include <utc_clock.h>

namespace telemetry {
namespace {
using namespace contract;
Ltr553 light_sensor;
PmsSnapshot latest;
std::array<oslogger::Field, field_count> fields;
// charging_status writes M5Unified is_charging_t, which telemetry_classes.inc
// names through ChargingStatus.
constexpr bool same_code(ChargingStatus code,
                         m5::Power_Class::is_charging_t m5_code) {
  return std::int32_t(code) == std::int32_t(m5_code);
}
static_assert(same_code(kChargingDischarging,
                        m5::Power_Class::is_discharging) &&
                  same_code(kChargingCharging, m5::Power_Class::is_charging) &&
                  same_code(kChargingUnknown, m5::Power_Class::charge_unknown),
              "ChargingStatus must match M5Unified is_charging_t");

void collect(oslogger::Row &row, std::int64_t now, std::int64_t, void *) {
  const auto &pms = latest;
  const PmsStatus status = !pms.present        ? kPmsMissing
                           : now < 30000000    ? kPmsWarming
                           : pms.age_ms > 5000 ? kPmsStale
                           : pms.error         ? kPmsSensorError
                                               : kPmsValid;
  row.integer(pms_status, status);
  if (pms.present) {
    row.counter(pms_age_ms, pms.age_ms);
    row.counter(pms_received_mono_us, pms.received_mono_us);
    row.integer(pms_firmware, pms.firmware);
    row.integer(pms_error, pms.error);
  }
  if (status == kPmsValid)
    for (std::size_t i = 0; i < 12; ++i)
      row.integer(static_cast<Field>(pm1_cf1_ug_m3 + i), pms.values[i]);
  row.counter(pms_frames, pms.frames);
  row.counter(pms_checksum_errors, pms.checksum_errors);
  row.counter(pms_length_errors, pms.length_errors);
  const auto fresh =
      M5.Imu.isEnabled() ? M5.Imu.update() : m5::IMU_Class::sensor_mask_none;
  row.integer(imu_fresh_mask, fresh);
  m5::IMU_Class::imu_data_t imu{};
  M5.Imu.getImuData(&imu);
  if (fresh & m5::IMU_Class::sensor_mask_accel) {
    row.number(accel_x_g, imu.accel.x);
    row.number(accel_y_g, imu.accel.y);
    row.number(accel_z_g, imu.accel.z);
  }
  if (fresh & m5::IMU_Class::sensor_mask_gyro) {
    row.number(gyro_x_dps, imu.gyro.x);
    row.number(gyro_y_dps, imu.gyro.y);
    row.number(gyro_z_dps, imu.gyro.z);
  }
  if (fresh & m5::IMU_Class::sensor_mask_mag) {
    row.integer(mag_x_raw, M5.Imu.getRawData(6));
    row.integer(mag_y_raw, M5.Imu.getRawData(7));
    row.integer(mag_z_raw, M5.Imu.getRawData(8));
  }
  float temperature;
  if (M5.Imu.isEnabled() && M5.Imu.getTemp(&temperature))
    row.number(imu_temperature_c, temperature);
  const auto light = light_sensor.read();
  row.integer(light_status, !light_sensor.available() ? kLightUnavailable
                            : light.io_error          ? kLightIoError
                            : !light.als_fresh        ? kLightNotFresh
                            : light.als_valid         ? kLightValid
                                                      : kLightInvalid);
  row.integer(proximity_status, !light_sensor.available()
                                    ? kProximityUnavailable
                                : light.io_error         ? kProximityIoError
                                : !light.proximity_fresh ? kProximityNotFresh
                                : light.proximity_valid  ? kProximityValid
                                                         : kProximitySaturated);
  if (light.als_valid) {
    row.integer(light_ch0_raw, light.als_ch0);
    row.integer(light_ch1_raw, light.als_ch1);
  }
  if (light.proximity_valid)
    row.integer(proximity_raw, light.proximity);
  row.integer(vbus_mv, M5.Power.getVBUSVoltage());
  row.integer(battery_mv, M5.Power.getBatteryVoltage());
  const int battery = M5.Power.getBatteryLevel();
  if (battery >= 0 && battery <= 100)
    row.integer(battery_percent, battery);
  row.integer(charging_status, M5.Power.isCharging());
  // CoreS3 getBatteryCurrent() is an unsupported constant zero. Leave null.
  row.integer(external_5v_enabled, M5.Power.getExtOutput());
  row.integer(usb_output_enabled, M5.Power.getUsbOutput());
  m5::rtc_datetime_t rtc{};
  const bool rtc_ok = M5.Rtc.isEnabled() && M5.Rtc.getDateTime(&rtc);
  row.integer(rtc_read_ok, rtc_ok);
  if (rtc_ok) {
    row.integer(rtc_date_yyyymmdd,
                rtc.date.year * 10000 + rtc.date.month * 100 + rtc.date.date);
    row.integer(rtc_time_hhmmss, rtc.time.hours * 10000 +
                                     rtc.time.minutes * 100 + rtc.time.seconds);
  }
  row.integer(touch_points, M5.Touch.getCount());
  if (M5.Touch.getCount()) {
    const auto &touch = M5.Touch.getDetail();
    row.integer(touch_x_px, touch.x);
    row.integer(touch_y_px, touch.y);
  }
}

bool rtc_read(std::int64_t &seconds, void *) {
  if (!M5.Rtc.isEnabled() || M5.Rtc.getVoltLow())
    return false;
  m5::rtc_datetime_t rtc{};
  if (!M5.Rtc.getDateTime(&rtc))
    return false;
  const int year = rtc.date.year;
  const int month = rtc.date.month;
  const int day = rtc.date.date;
  if (year < 2020 || year > 2099 || month < 1 || month > 12 ||
      rtc.time.hours < 0 || rtc.time.hours > 23 || rtc.time.minutes < 0 ||
      rtc.time.minutes > 59 || rtc.time.seconds < 0 || rtc.time.seconds > 59)
    return false;
  constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const int max_day = days[month - 1] + (month == 2 && year % 4 == 0);
  if (day < 1 || day > max_day)
    return false;
  seconds =
      opensensor::utc::days_from_civil(year, unsigned(month), unsigned(day)) *
          86400 +
      rtc.time.hours * 3600 + rtc.time.minutes * 60 + rtc.time.seconds;
  return opensensor::utc::supported_epoch(seconds);
}

bool rtc_write(std::int64_t seconds, void *) {
  if (!M5.Rtc.isEnabled())
    return false;
  const auto value = static_cast<std::time_t>(seconds);
  std::tm utc{};
  if (!::gmtime_r(&value, &utc))
    return false;
  M5.Rtc.setDateTime(m5::rtc_datetime_t(utc));
  std::int64_t readback = 0;
  return rtc_read(readback, nullptr) && readback >= seconds &&
         readback - seconds <= 1;
}

// The STAC File Item statistics columns (#142), the numeric measurement
// columns of the board template in template order.
constexpr auto kStatColumns = opensensor::stac::stat_columns<
    opensensor::stac::stat_count(kFields, std::span(kSemantics))>(
    kFields, std::span(kSemantics));
static_assert(opensensor::stac::stat_columns_valid(kStatColumns, kFields,
                                                   std::span(kSemantics)),
              "every STAC statistics column must have a gate the Item reads");

// OpenSensor BLE name prefix, so the board advertises OS-xxxx.
constexpr char kBleNamePrefix[] = "OS-";
oslogger::Config logger_config;
std::uint64_t cached_total_bytes = 0;
std::uint64_t cached_used_bytes = 0;

void refresh_capacity() {
  // Called only by the archive worker under the shared display/storage mutex.
  // Also after a remount, so a card inserted after boot reports capacity.
  std::uint64_t total = 0, free = 0;
  if (esp_vfs_fat_info("/sd", &total, &free) == ESP_OK && free <= total) {
    cached_total_bytes = total;
    cached_used_bytes = total - free;
  } else {
    cached_total_bytes = cached_used_bytes = 0;
  }
}
} // namespace

void begin_logger(bool sd_mounted) {
  for (std::size_t i = 0; i < field_count; ++i) {
    const auto &field = kFields[i];
    fields[i] = {field.name,     field.type,     field.procedure,
                 field.unit,     field.validity, field.property_uri,
                 field.live_key, field.decimals};
  }
  logger_config = {
      .fields = fields.data(),
      .column_count = field_count,
      .schema_version = kSchemaVersion,
      .schema_name = kSchemaName,
      .firmware = kFirmware,
      .created_by = kCreatedBy,
      .dictionary_version = kDictionaryVersion,
      .dictionary_uri = kDictionaryUri,
      .dictionary_sha256 = kDictionarySha256,
      .configuration_id = kConfigurationId,
      .configuration = kConfiguration,
      .time_semantics = kTimeSemantics,
      .board = kBoard,
      .output_directory = "/sd/output",
      .legacy_directory = "/sd/parquet",
      .ble_name_prefix = kBleNamePrefix,
      .descriptor = kTemplateJson,
      .descriptor_bytes = sizeof(kTemplateJson) - 1,
      .descriptor_sha256 = kDescriptorSha256,
      .template_href = kTemplateHref,
      .self_test_field = "pm25_atmospheric_ug_m3",
      .stac_columns = kStatColumns.data(),
      .stac_column_count = kStatColumns.size(),
  };
  const oslogger::Hooks hooks{
      .collect = collect,
      .storage_total_bytes =
          [](void *) {
            refresh_capacity();
            return cached_total_bytes;
          },
      .storage_used_bytes =
          [](void *) {
            refresh_capacity();
            return cached_used_bytes;
          },
      .storage_free_bytes = [](std::uint64_t &free_bytes,
                               void *) { return sd_free_bytes(free_bytes); },
      .storage_cluster_bytes = [](void *) { return sd_cluster_bytes(); },
      .storage_volume_serial = [](void *) { return sd_volume_serial(); },
      .storage_remount = [](void *) { return remount_sd(); },
      .rtc_read = rtc_read,
      .rtc_write = rtc_write,
  };
  const bool light = light_sensor.begin();
  oslog.printf("PARQUET HARDWARE board=cores3 light_available=%s\n",
               light ? "true" : "false");
  oslogger::begin(logger_config, hooks, sd_mounted);
}
void poll_logger(const PmsSnapshot &pms) {
  latest = pms;
  oslogger::poll();
}
void lock_display() { oslogger::lock_bus(); }
void unlock_display() { oslogger::unlock_bus(); }
bool start_links(bool display_detected) {
  return oslogger::start_links(display_detected);
}
bool enqueue_request(const ble::ControlRequest &request) {
  return oslogger::enqueue_request(request);
}
const char *thing_text_id() { return oslogger::thing_text_id(); }
const char *device_text_id() { return oslogger::device_text_id(); }
const char *firmware_text_id() { return oslogger::firmware_text_id(); }
ClockView clock_view() {
  const auto view = oslogger::clock_view();
  return {view.utc_ns, view.epoch, view.source, view.rtc_state,
          view.source_name};
}
} // namespace telemetry
