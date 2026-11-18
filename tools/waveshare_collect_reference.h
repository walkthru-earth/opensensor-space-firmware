#pragma once

// The Waveshare logger collect from before the module split (#17 step 5),
// kept verbatim as the reference of the row equality fixture in
// test_waveshare_contract.cpp. Test only. Never include it in firmware.
// Change it only to follow a contract change the modules make on purpose.

#include "../firmware/esp-idf-waveshare-sim7670g/logger/telemetry_contract.h"
#include <algorithm>
#include <cstdint>
#include <device_config.h>

namespace reference {
using namespace telemetry::contract;

// Warm-up runs from the last PMS wake (pms_warming). Today that is boot.
// Apply only to a fresh, zeroed row.
inline void apply_pms(Sample &row, const PmsSnapshot &pms, std::int64_t now,
                      const SensorProfile &sensor = {}) {
  const auto &frame = pms.frame;
  const auto status =
      !pms.present                              ? kPmsMissing
      : pms_warming(now, pms.last_wake_mono_us) ? kPmsWarming
      : pms.age_ms > kPmsStaleAfterMs           ? kPmsStale
      : frame.sensor_error                      ? kPmsSensorError
      : !frame.has_temperature_humidity || frame.particle_count_fields != 4
          ? kPmsModelMismatch
          : kPmsValid;
  row.integer(pms_status, status);
  const auto identity = serial_code(sensor);
  if (identity)
    row.counter(sensor_serial_code, identity);
  const auto candidate_status = !sensor.batch_candidate ? kBatchDisabled
                                : !exact_batch(sensor)  ? kBatchIdentityMismatch
                                : status != kPmsValid ? kBatchSourceUnavailable
                                                      : kBatchCandidate;
  row.integer(pm25_batch_candidate_status, candidate_status);
  row.counter(pms_frames, pms.frames);
  row.counter(pms_checksum_errors, pms.checksum_errors);
  row.counter(pms_length_errors, pms.length_errors);
  if (pms.present) {
    row.counter(pms_age_ms, pms.age_ms);
    row.counter(pms_received_mono_us, pms.received_mono_us);
    row.integer(pms_firmware, frame.firmware_version);
    row.integer(pms_error, frame.sensor_error);
  }
  if (status != kPmsValid)
    return;
  row.integer(pm1_cf1_ug_m3, frame.cf1_pm1);
  row.integer(pm25_cf1_ug_m3, frame.cf1_pm25);
  row.integer(pm10_cf1_ug_m3, frame.cf1_pm10);
  row.integer(pm1_atmospheric_ug_m3, frame.atmospheric_pm1);
  row.integer(pm25_atmospheric_ug_m3, frame.atmospheric_pm25);
  row.integer(pm10_atmospheric_ug_m3, frame.atmospheric_pm10);
  for (std::size_t i = 0; i < 4; ++i)
    row.integer(particles_gt03_per_01l + i, frame.particle_counts[i]);
  if (candidate_status == kBatchCandidate)
    row.number(pm25_batch_candidate_ug_m3,
               0.003964F * frame.particle_counts[0]);
  if (frame.temperature_deci_c >= -100 && frame.temperature_deci_c <= 600)
    row.number(ambient_temperature_c, frame.temperature_deci_c / 10.0F);
  if (frame.humidity_deci_percent <= 1000)
    row.number(relative_humidity_percent, frame.humidity_deci_percent / 10.0F);
}
struct GaugeSnapshot {
  bool battery_installed = false; // explicit owner configuration only
  bool vcell_ok = false;          // VCELL read and in range
  bool soc_ok = false;            // SOC read and in range
  std::int32_t millivolts = 0;
  std::int32_t percent = 0; // whole percent, truncated and capped at 100
};
// Decoding and range checks live in the shared opensensor::Max17048 driver.
// This board only adds the explicit battery-installed configuration.
inline GaugeSnapshot
gauge_snapshot(const opensensor::Max17048Reading &reading) {
  return {true, reading.vcell_valid(), reading.soc_valid(), reading.millivolts,
          reading.percent};
}
// gauge_status 2 needs both words. A good VCELL with a failed or implausible
// SOC still writes battery_mv under status 1 and leaves battery_percent null.
inline void apply_gauge(Sample &row, const GaugeSnapshot &gauge) {
  row.integer(gauge_status, !gauge.battery_installed ? kGaugeNotInstalled
                            : !(gauge.vcell_ok && gauge.soc_ok)
                                ? kGaugeReadError
                                : kGaugeValid);
  if (!gauge.battery_installed)
    return;
  if (gauge.vcell_ok && gauge.millivolts > 0)
    row.integer(battery_mv, gauge.millivolts);
  if (gauge.soc_ok && gauge.percent >= 0 && gauge.percent <= 100)
    row.integer(battery_percent, gauge.percent);
}

// Fields 57..62. Every column stays null when its own register failed.
// Raw SOC and minutes to empty need a valid SOC. The health state comes from
// VCELL alone and is always written, Unknown when no voltage is classified.
inline void apply_battery_health(Sample &row, const BatteryHealth &health) {
  if (!health.battery_installed)
    return;
  const auto &ext = health.extended;
  if (health.reading.soc_valid())
    row.number(battery_soc_raw_percent,
               static_cast<float>(opensensor::Max17048::raw_soc_percent(
                   health.reading.raw_soc)));
  if (ext.crate.ok)
    row.number(battery_charge_rate_percent_per_h,
               static_cast<float>(ext.crate_milli_percent_per_hour()) /
                   1000.0F);
  if (ext.status.ok)
    row.integer(gauge_status_flags, ext.status_flags());
  if (ext.mode.ok)
    row.integer(gauge_hibernating, ext.hibernating());
  row.integer(battery_health_state,
              static_cast<std::int32_t>(health.assessment.state));
  if (health.assessment.minutes_to_empty >= 0)
    row.integer(battery_minutes_to_empty, health.assessment.minutes_to_empty);
}

// The pre-module Hooks::collect body, with its board reads passed in.
inline void collect(Sample &row, std::int64_t now, const PmsSnapshot &latest,
                    const config::Settings &settings,
                    const BatteryHealth &health) {
  PmsSnapshot snapshot = latest;
  // Saturated, never wrapped (#169).
  if (snapshot.present) {
    const auto age = now <= snapshot.received_mono_us
                         ? std::int64_t{0}
                         : (now - snapshot.received_mono_us) / 1000;
    snapshot.age_ms = static_cast<std::uint32_t>(
        std::min<std::int64_t>(age, std::int64_t{UINT32_MAX}));
  }
  apply_pms(row, snapshot, now,
            {settings.sensor_vendor, settings.sensor_model,
             settings.sensor_serial, settings.sensor_batch_candidate});
  apply_gauge(row, !health.battery_installed ? GaugeSnapshot{}
                                             : gauge_snapshot(health.reading));
  apply_battery_health(row, health);
}
} // namespace reference
