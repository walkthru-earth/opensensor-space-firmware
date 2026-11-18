#pragma once

// The Waveshare V2 sensor modules of the plugin contract (#17 step 5,
// docs/shared/plugins/README.md section 10). The board Hooks::collect loops
// over the static constexpr table that table() builds, one entry per module
// the board.json lists. Each module writes only its own generated columns
// through ossensor::Writer. A column resolves by name at build time. The
// build stops when a write names a Column of another module, or uses another
// physical type than the board field.
//
// Pure and host tested. The board passes its hardware reads in as Sources
// callbacks, so tools/test_waveshare_contract.cpp runs the same table with
// fakes and compares its rows with the pre-module collect.

#include "telemetry_contract.h"
#include <array>
#include <battery_health/battery_health_module.h>
#include <cstdint>
#include <device_config.h>
#include <max17048/max17048_module.h>
#include <os_sensor_module.h>
#include <pms5003t/pms5003t_module.h>
#include <span>
#include <utility>

#include "modules/waveshare/waveshare_module.h"

namespace board_modules {
namespace contract = telemetry::contract;
namespace pms5003t = ossensor::pms5003t;
namespace waveshare = ossensor::waveshare;
namespace max17048 = ossensor::max17048;
namespace battery_health = ossensor::battery_health;

// The board reads behind the modules. Main loop only. The module table
// holds a non-const pointer to one instance, and collect_gauge writes its
// gauge member on every row, so no other task may touch it.
struct Sources {
  // The latest checksum-valid PMS5003T frame, from the UART poll.
  const contract::PmsSnapshot *latest = nullptr;
  // The owner-provisioned sensor identity, config::get on the device.
  config::Settings (*settings)() = nullptr;
  // One MAX17048 read and classifier update, read_gauge on the device.
  contract::BatteryHealth (*read_gauge)(std::int64_t now_us) = nullptr;
  // The max17048 module stores its read here for battery_health, which runs
  // after it in the table.
  contract::BatteryHealth gauge{};
};

inline constexpr auto kPmsSlots =
    ossensor::resolve(pms5003t::kColumns, std::span{contract::kFields});
inline constexpr auto kIdentitySlots =
    ossensor::resolve(waveshare::kColumns, std::span{contract::kFields});
inline constexpr auto kGaugeSlots =
    ossensor::resolve(max17048::kColumns, std::span{contract::kFields});
inline constexpr auto kHealthSlots =
    ossensor::resolve(battery_health::kColumns, std::span{contract::kFields});

// The board status enums name the same codes as the generated constants,
// because telemetry_classes.inc spells its CLASS rows with the enums.
static_assert(contract::kPmsMissing == pms5003t::kStatusMissing &&
              int(plantower::Reading::Missing) == pms5003t::kStatusMissing &&
              int(plantower::Reading::Warming) == pms5003t::kStatusWarming &&
              int(plantower::Reading::Stale) == pms5003t::kStatusStale &&
              int(plantower::Reading::SensorError) ==
                  pms5003t::kStatusSensorError &&
              int(plantower::Reading::Valid) == pms5003t::kStatusValid &&
              contract::kPmsWarming == pms5003t::kStatusWarming &&
              contract::kPmsStale == pms5003t::kStatusStale &&
              contract::kPmsSensorError == pms5003t::kStatusSensorError &&
              contract::kPmsValid == pms5003t::kStatusValid &&
              contract::kPmsModelMismatch == pms5003t::kStatusModelMismatch &&
              contract::kPmsSleeping == pms5003t::kStatusSleeping);
static_assert(contract::kGaugeNotInstalled ==
                  max17048::kStatusBatteryNotInstalledOrDisabled &&
              contract::kGaugeReadError ==
                  max17048::kStatusReadErrorOrImplausible &&
              contract::kGaugeValid == max17048::kStatusValid);

// The latest frame with its age at `now`, saturated so a sensor that went
// silent 49.7 days ago stays stale (#169).
[[nodiscard]] inline contract::PmsSnapshot
aged(const contract::PmsSnapshot &latest, std::int64_t now) {
  contract::PmsSnapshot snapshot = latest;
  if (snapshot.present)
    snapshot.age_ms = plantower::frame_age_ms(now, snapshot.received_mono_us);
  return snapshot;
}
// The shared Plantower rule (pms_status.h), then the model check. Warm-up runs
// from the last PMS wake (pms_warming). Today that is boot. Status 5 means the
// parser was built for a model without the PMS5003T tail. The frame itself
// cannot tell a PMS5003T from a PMSA003, so a wired PMSA003 is not status 5.
// Its tail words only fail the range gates in write_pms (#169).
[[nodiscard]] inline std::int32_t status_of(const contract::PmsSnapshot &pms,
                                            std::int64_t now) {
  const auto &frame = pms.frame;
  const auto reading = plantower::classify(
      {.present = pms.present,
       .warming = contract::pms_warming(now, pms.last_wake_mono_us),
       .age_ms = pms.age_ms,
       .sensor_error = frame.sensor_error},
      contract::kPmsStaleAfterMs);
  if (reading != plantower::Reading::Valid)
    return static_cast<std::int32_t>(reading);
  return !frame.has_temperature_humidity || frame.particle_count_fields != 4
             ? pms5003t::kStatusModelMismatch
             : pms5003t::kStatusValid;
}

// pms5003t, every column of core, tail and received. Apply only to a fresh,
// zeroed row.
inline void write_pms(oslogger::Row &row, const contract::PmsSnapshot &pms,
                      std::int64_t now) {
  using enum pms5003t::Column;
  const ossensor::Writer<pms5003t::Column, contract::kFields, kPmsSlots> out{
      row};
  const auto &frame = pms.frame;
  const auto status = status_of(pms, now);
  out.integer<pms_status>(status);
  out.counter<pms_frames>(pms.frames);
  out.counter<pms_checksum_errors>(pms.checksum_errors);
  out.counter<pms_length_errors>(pms.length_errors);
  if (pms.present) {
    out.counter<pms_age_ms>(pms.age_ms);
    out.counter<pms_received_mono_us>(pms.received_mono_us);
    out.integer<pms_firmware>(frame.firmware_version);
    out.integer<pms_error>(frame.sensor_error);
  }
  if (status != pms5003t::kStatusValid)
    return;
  out.integer<pm1_cf1_ug_m3>(frame.cf1_pm1);
  out.integer<pm25_cf1_ug_m3>(frame.cf1_pm25);
  out.integer<pm10_cf1_ug_m3>(frame.cf1_pm10);
  out.integer<pm1_atmospheric_ug_m3>(frame.atmospheric_pm1);
  out.integer<pm25_atmospheric_ug_m3>(frame.atmospheric_pm25);
  out.integer<pm10_atmospheric_ug_m3>(frame.atmospheric_pm10);
  out.integer<particles_gt03_per_01l>(frame.particle_counts[0]);
  out.integer<particles_gt05_per_01l>(frame.particle_counts[1]);
  out.integer<particles_gt10_per_01l>(frame.particle_counts[2]);
  out.integer<particles_gt25_per_01l>(frame.particle_counts[3]);
  if (frame.temperature_deci_c >= contract::kPmsTemperatureMinDeciC &&
      frame.temperature_deci_c <= contract::kPmsTemperatureMaxDeciC)
    out.number<ambient_temperature_c>(frame.temperature_deci_c / 10.0F);
  if (frame.humidity_deci_percent <= contract::kPmsHumidityMaxDeciPercent)
    out.number<relative_humidity_percent>(frame.humidity_deci_percent / 10.0F);
}

// waveshare pms_identity, the owner-provisioned serial and the batch PM2.5
// candidate, gated on the same status_of as write_pms.
inline void write_identity(oslogger::Row &row, const contract::PmsSnapshot &pms,
                           std::int64_t now,
                           const contract::SensorProfile &sensor) {
  using enum waveshare::Column;
  const ossensor::Writer<waveshare::Column, contract::kFields, kIdentitySlots>
      out{row};
  const auto status = status_of(pms, now);
  const auto code = contract::serial_code(sensor);
  if (code)
    out.counter<sensor_serial_code>(code);
  const auto candidate =
      !sensor.batch_candidate            ? contract::kBatchDisabled
      : !contract::exact_batch(sensor)   ? contract::kBatchIdentityMismatch
      : status != pms5003t::kStatusValid ? contract::kBatchSourceUnavailable
                                         : contract::kBatchCandidate;
  out.integer<pm25_batch_candidate_status>(candidate);
  if (candidate == contract::kBatchCandidate)
    out.number<pm25_batch_candidate_ug_m3>(0.003964F *
                                           pms.frame.particle_counts[0]);
}

// max17048, core and v5. gauge_status 2 needs both words. A good VCELL with
// a failed or implausible SOC still writes battery_mv under status 1 and
// leaves battery_percent null. Every v5 column stays null when its own
// register failed.
inline void write_gauge(oslogger::Row &row,
                        const contract::BatteryHealth &health) {
  using enum max17048::Column;
  const ossensor::Writer<max17048::Column, contract::kFields, kGaugeSlots> out{
      row};
  const auto &reading = health.reading;
  const bool vcell_ok = reading.vcell_valid();
  const bool soc_ok = reading.soc_valid();
  out.integer<gauge_status>(
      !health.battery_installed ? max17048::kStatusBatteryNotInstalledOrDisabled
      : !(vcell_ok && soc_ok)   ? max17048::kStatusReadErrorOrImplausible
                                : max17048::kStatusValid);
  if (!health.battery_installed)
    return;
  if (vcell_ok && reading.millivolts > 0)
    out.integer<battery_mv>(reading.millivolts);
  if (soc_ok && reading.percent >= 0 && reading.percent <= 100)
    out.integer<battery_percent>(reading.percent);
  const auto &ext = health.extended;
  if (soc_ok)
    out.number<battery_soc_raw_percent>(static_cast<float>(
        opensensor::Max17048::raw_soc_percent(reading.raw_soc)));
  if (ext.crate.ok)
    out.number<battery_charge_rate_percent_per_h>(
        static_cast<float>(ext.crate_milli_percent_per_hour()) / 1000.0F);
  if (ext.status.ok)
    out.integer<gauge_status_flags>(ext.status_flags());
  if (ext.mode.ok)
    out.integer<gauge_hibernating>(ext.hibernating());
}

// battery_health. The state comes from VCELL alone and is always written
// when a battery is installed, Unknown when no voltage is classified.
// Minutes to empty needs a trusted SOC.
inline void write_health(oslogger::Row &row,
                         const contract::BatteryHealth &health) {
  using enum battery_health::Column;
  const ossensor::Writer<battery_health::Column, contract::kFields,
                         kHealthSlots>
      out{row};
  if (!health.battery_installed)
    return;
  out.integer<battery_health_state>(
      static_cast<std::int32_t>(health.assessment.state));
  if (health.assessment.minutes_to_empty >= 0)
    out.integer<battery_minutes_to_empty>(health.assessment.minutes_to_empty);
}

// Module::collect entries. The context is the board Sources. The
// signature is fixed by ossensor::Module, so context stays non-const.
// cppcheck-suppress constParameterCallback
inline void collect_pms(oslogger::Row &row, std::int64_t now, void *context) {
  const auto &sources = *static_cast<const Sources *>(context);
  write_pms(row, aged(*sources.latest, now), now);
}
inline void collect_identity(oslogger::Row &row, std::int64_t now,
                             // cppcheck-suppress constParameterCallback
                             void *context) {
  const auto &sources = *static_cast<const Sources *>(context);
  const auto settings = sources.settings();
  write_identity(row, aged(*sources.latest, now), now,
                 {settings.sensor_vendor, settings.sensor_model,
                  settings.sensor_serial, settings.sensor_batch_candidate});
}
inline void collect_gauge(oslogger::Row &row, std::int64_t now, void *context) {
  auto &sources = *static_cast<Sources *>(context);
  sources.gauge = sources.read_gauge(now);
  write_gauge(row, sources.gauge);
}
inline void collect_health(oslogger::Row &row, std::int64_t, void *context) {
  write_health(row, static_cast<const Sources *>(context)->gauge);
}

// The module table in board.json order of first use. max17048 runs before
// battery_health, which reads its stored gauge read. None has an init yet,
// init_board still owns the UART and I2C bring-up.
[[nodiscard]] consteval std::array<ossensor::Module, 4>
table(Sources &sources) {
  return {{
      {.procedure = pms5003t::kProcedure,
       .init = nullptr,
       .collect = collect_pms,
       .context = &sources},
      {.procedure = waveshare::kProcedure,
       .init = nullptr,
       .collect = collect_identity,
       .context = &sources},
      {.procedure = max17048::kProcedure,
       .init = nullptr,
       .collect = collect_gauge,
       .context = &sources},
      {.procedure = battery_health::kProcedure,
       .init = nullptr,
       .collect = collect_health,
       .context = &sources},
  }};
}

// The board Hooks::collect body.
inline void collect(std::span<const ossensor::Module> modules,
                    oslogger::Row &row, std::int64_t now) {
  for (const auto &module : modules)
    module.collect(row, now, module.context);
}
} // namespace board_modules
