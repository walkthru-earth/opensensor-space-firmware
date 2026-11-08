#pragma once

#include "pms5003t_identity.h"
#include "telemetry_descriptor_digest.h"
#include "telemetry_dictionary_digest.h"
#include "telemetry_logical.h"
#include "telemetry_template.h"
#include <algorithm>
#include <column_semantics.h>
#include <cstring>
#include <iterator>
#include <os_battery_health.h>
#include <os_logger.h>
#include <os_logger_live.h>
#include <os_max17048.h>
#include <pinned_uri.h>
#include <pms_frame.h>
#include <span>
#include <string_view>
#include <utc_clock.h>

namespace telemetry {
namespace contract {
// Schema v5 keeps the 56 leaves of v4 in the same order and field ids and
// appends six battery columns (field ids 57..62): uncapped raw SOC, CRATE,
// STATUS flags, hibernate state, the shared battery health state and a
// minutes-to-empty estimate. v4 renamed the two UTC instants to
// microseconds and added a Parquet field_id on every leaf for Iceberg v3.
// Schema v6 and dictionary v7 (firmware v2.15, #83, owner decision 32) put
// location_source (UINT8) and geometry (native Parquet GEOMETRY, a 2D WKB
// Point at the H3 cell centre) next to h3_cell_id and h3_resolution, field
// ids 55 and 56. Still 62 leaves. This is the Thing field table baseline,
// field ids are plain 1-based positions and append only from here (owner
// answer to bridge question 36, 2026-10-26).
constexpr std::int32_t kSchemaVersion = 6;
constexpr const char *kSchemaName = "waveshare-sim7670g-telemetry-v6";
constexpr const char *kDictionaryVersion = "waveshare-sim7670g-telemetry-v8";
constexpr const char *kFirmware = "idf-waveshare-parquet-v2.19";
constexpr const char *kCreatedBy = "opensensor-space-firmware version 0.3";
// Footer thing.board, the board slug of the template path. The free text name
// is the template title (stac-opensensor 4408c4e).
constexpr const char *kBoard = "waveshare-esp32-s3-sim7670g";
// Raw telemetry_fields.inc at a pinned commit on main (#89), so the bytes at
// the URI hash to kDictionarySha256. It is also the template describedby href.
// Written by `pixi run pin-uris`, checked by pinned-uris-test.
constexpr const char *kDictionaryUri =
    "https://raw.githubusercontent.com/walkthru-earth/"
    "opensensor-space-firmware/"
    "91c776a17ecadc91f0bd51523c30f691cb9ff5ba/"
    "firmware/esp-idf-waveshare-sim7670g/logger/telemetry_fields.inc";
static_assert(opensensor::pinned::names(
                  kDictionaryUri, "firmware/esp-idf-waveshare-sim7670g/logger/"
                                  "telemetry_fields.inc"),
              "kDictionaryUri is the raw field table at a full commit");
constexpr const char *kConfigurationId = "waveshare-sim7670g-acquisition-v3";
// Uncalibrated starting point. Change only with a dated discharge run and a
// new configuration id; kConfiguration embeds the same numbers.
constexpr opensensor::battery::Thresholds kBatteryThresholds{};
constexpr std::int64_t kPmsWarmupUs = 30000000;
constexpr std::uint32_t kPmsStaleAfterMs = 5000;
constexpr const char *kConfiguration =
    "{\"sample_interval_ms\":10000,\"pms_warmup_us\":30000000,"
    "\"pms_stale_after_ms\":5000,\"pms_uart_rx\":1,\"pms_uart_tx\":2,"
    "\"pms_baud\":9600,\"pms_model\":\"PMS5003T\","
    "\"battery_presence\":\"explicit-owner-configuration\","
    "\"battery_percent\":\"integer-percent-truncated-capped-at-100\","
    "\"snapshot\":\"latest-available-not-average\","
    "\"sensor_identity\":\"owner-provisioned-nvs-and-per-row\","
    "\"batch_candidate\":\"off-by-default-not-cairo-validated\","
    "\"battery_health\":{\"source\":\"max17048-vcell\","
    "\"external_power_mv\":4240,\"external_power_hysteresis_mv\":20,"
    "\"low_enter_mv\":3500,\"critical_enter_mv\":3350,"
    "\"hysteresis_mv\":75,\"debounce_samples\":3,\"settling_minutes\":60,"
    "\"calibration\":\"uncalibrated-defaults\","
    "\"critical_action\":\"finalize-open-file-once\"}}";
constexpr const char *kUnknown = "unknown";
constexpr const char *kTimeSemantics =
    "event_time_utc_us estimates snapshot start; collection_completed_mono_us "
    "bounds sequential reads; pms_received_mono_us is last checksum-valid UART "
    "frame receipt, not sensor phenomenon time; result/ingestion time and "
    "clock uncertainty unknown; no RTC anchor and no retroactive UTC "
    "assignment";

enum Field : std::size_t {
#define FIELD(name, type, procedure, unit, validity, live, decimals) name,
#include "telemetry_fields.inc"
#undef FIELD
  field_count
};
using Definition = oslogger::Field;
// The dictionary carries each LIVE key, so the digest covers it. "" keeps a
// field out of the LIVE snapshot. The shared logger emits keys in
// field-table order.
constexpr const char *live_or_null(const char *key) {
  return key[0] == '\0' ? nullptr : key;
}
// LIVE digits after the point, in the dictionary so the digest covers them.
// Integer fields carry 0, a Float carries 0..kMaxLiveDecimals from its
// sensor resolution.
template <std::size_t N> constexpr bool decimals_fit(const Definition (&f)[N]) {
  return std::all_of(std::begin(f), std::end(f), [](const Definition &field) {
    return field.decimals <=
           (field.type == PhysicalType::Float ? oslogger::kMaxLiveDecimals : 0);
  });
}
// Column semantics for the STAC template (#84). telemetry_semantics.inc stays
// out of the dict digest. It is the single source of each FIELD property_uri,
// a board neutral id looked up by name at compile time, so only the property
// strings reach the image.
static constexpr opensensor::semantics::Column kSemantics[] = {
#define SEM(name, ucum, role, cf_standard_name, aggregate, plausible_min,      \
            plausible_max, settle_s, valid_when, property, sensor_vendor,      \
            sensor_model)                                                      \
  {#name,                                                                      \
   ucum,                                                                       \
   opensensor::semantics::Role::role,                                          \
   cf_standard_name,                                                           \
   opensensor::semantics::Aggregate::aggregate,                                \
   #plausible_min,                                                             \
   #plausible_max,                                                             \
   settle_s,                                                                   \
   valid_when,                                                                 \
   property,                                                                   \
   sensor_vendor,                                                              \
   sensor_model},
#include "telemetry_semantics.inc"
#undef SEM
};
constexpr Definition kFields[] = {
#define FIELD(name, type, procedure, unit, validity, live, decimals)           \
  {#name,                                                                      \
   PhysicalType::type,                                                         \
   #procedure,                                                                 \
   unit,                                                                       \
   #validity,                                                                  \
   opensensor::semantics::property_of(kSemantics, #name),                      \
   live_or_null(live),                                                         \
   decimals},
#include "telemetry_fields.inc"
#undef FIELD
};
static_assert(field_count == 62, "Version the schema when changing fields");
static_assert(field_count <= kMaxColumns, "Parquet schema capacity exceeded");
static_assert(
    decimals_fit(kFields),
    "decimals is 0 for integer fields and at most kMaxLiveDecimals for Float");

static_assert(opensensor::semantics::aligned(kFields, kSemantics),
              "telemetry_semantics.inc must list every FIELD in order");
static_assert(opensensor::semantics::well_formed(kFields, kSemantics),
              "telemetry_semantics.inc has a malformed row");
static_assert(opensensor::semantics::every_property_set(kFields),
              "every FIELD needs a property from telemetry_semantics.inc");
using Sample = oslogger::Row;

// The unit label picks the logical type (telemetry_logical.h), which changes
// what readers present, never the stored slot.
constexpr LogicalType logical_type(const Definition &field) {
  return logical_for(field.type, field.unit);
}
template <typename Row>
inline void prepare_columns(Column *columns, Row *rows) {
  for (std::size_t i = 0; i < field_count; ++i)
    columns[i] = Column{
        kFields[i].name,          kFields[i].type,
        &rows[0].data[i],         sizeof(Row),
        &rows[0].valid[i],        sizeof(Row),
        logical_type(kFields[i]), static_cast<std::int32_t>(i + 1)}; // field_id
}

enum ClockSource : std::int32_t {
  kClockNone = opensensor::utc::None,
  kClockHost = opensensor::utc::Host,
  kClockNetwork = opensensor::utc::Network,
};
// No RTC is available in this board contract. Caller supplies a coherent
// host or SNTP anchor; each captured row keeps its original epoch permanently.
inline void apply_clock(Sample &row, std::int64_t now, std::int64_t mono_anchor,
                        std::int64_t utc_anchor, std::int32_t generation,
                        std::int32_t source = kClockHost) {
  row.integer(clock_status, generation ? source : kClockNone);
  row.integer(clock_epoch, generation);
  if (generation) {
    row.counter(event_time_utc_us,
                opensensor::utc::estimate_us(now, mono_anchor, utc_anchor));
    row.counter(clock_anchor_mono_us, mono_anchor);
    row.counter(clock_anchor_utc_us, opensensor::utc::ns_to_us(utc_anchor));
  }
}

struct PmsSnapshot {
  plantower::Frame frame{};
  std::uint32_t age_ms = 0;
  std::int64_t received_mono_us = 0;
  std::uint32_t frames = 0;
  std::uint32_t checksum_errors = 0;
  std::uint32_t length_errors = 0;
  // Monotonic time the PMS last woke. esp_timer starts at 0 on boot and the
  // sensor is powered from boot, so the default is the boot wake. A future
  // sleep or reset control sets it when the sensor wakes again.
  std::int64_t last_wake_mono_us = 0;
  bool present = false;
};
// True while the PMS is inside its warm-up window after the last wake. A
// clock reading earlier than the wake (a wake stamped after the snapshot
// time) counts as warming, so a row never reads valid before the window.
[[nodiscard]] constexpr bool pms_warming(std::int64_t now,
                                         std::int64_t last_wake_mono_us) {
  return now < last_wake_mono_us || now - last_wake_mono_us < kPmsWarmupUs;
}
enum PmsStatus : std::int32_t {
  kPmsMissing = 0,
  kPmsWarming = 1,
  kPmsStale = 2,
  kPmsSensorError = 3,
  kPmsValid = 4,
  kPmsModelMismatch = 5,
  // Reserved for the PMS duty cycle (#39). The sensor is asleep through its
  // SET pin. No code path writes it yet.
  kPmsSleeping = 6,
};
struct SensorProfile {
  const char *vendor = "";
  const char *model = "";
  const char *serial = "";
  bool batch_candidate = false;
};
enum BatchCandidateStatus : std::int32_t {
  kBatchDisabled = 0,
  kBatchIdentityMismatch = 1,
  kBatchSourceUnavailable = 2,
  kBatchCandidate = 3,
};
inline std::int64_t serial_code(const SensorProfile &sensor) {
  return pms5003t::serial_code(sensor.vendor, sensor.model, sensor.serial);
}
inline bool exact_batch(const SensorProfile &sensor) {
  return pms5003t::batch_eligible(sensor.vendor, sensor.model, sensor.serial);
}
enum GaugeStatus : std::int32_t {
  kGaugeNotInstalled = 0,
  kGaugeReadError = 1,
  kGaugeValid = 2,
};

static_assert(kBatteryThresholds.external_power_mv == 4240 &&
                  kBatteryThresholds.external_power_hysteresis_mv == 20 &&
                  kBatteryThresholds.low_enter_mv == 3500 &&
                  kBatteryThresholds.critical_enter_mv == 3350 &&
                  kBatteryThresholds.hysteresis_mv == 75 &&
                  kBatteryThresholds.debounce_samples == 3 &&
                  kBatteryThresholds.settling_minutes == 60,
              "kConfiguration embeds these thresholds; update both");

// One classifier input from the basic and extended gauge reads. VCELL and
// SOC are each trusted only when their own read and range check passed, so
// a bad SOC never hides a low cell voltage.
inline opensensor::battery::Sample
battery_sample(std::int64_t now_ms, const opensensor::Max17048Reading &reading,
               const opensensor::Max17048Extended &ext) {
  opensensor::battery::Sample sample;
  sample.now_ms = now_ms;
  sample.vcell_ok = reading.vcell_valid();
  sample.millivolts = reading.millivolts;
  sample.soc_ok = reading.soc_valid();
  sample.raw_soc = reading.raw_soc;
  sample.crate_ok = ext.crate.ok;
  sample.crate_milli_percent_per_hour = ext.crate_milli_percent_per_hour();
  sample.status_ok = ext.status.ok;
  sample.reset_indicator =
      ext.status_bit(opensensor::Max17048Extended::kStatusRi);
  return sample;
}

struct BatteryHealth {
  bool battery_installed = false; // explicit owner configuration only
  opensensor::Max17048Reading reading{};
  opensensor::Max17048Extended extended{};
  opensensor::battery::Assessment assessment{};
};

// Status legends of the code columns. telemetry_classes.inc names the same
// values for the board template (#85), and the static_assert below keeps the
// two equal. The legends are compile time checks only, the footer carries no
// code meanings (#88).
constexpr const char *kPmsStatusLegend =
    "0=missing,1=warming,2=stale,3=sensor-error,4=valid,5=model-mismatch,"
    "6=sleeping";
constexpr const char *kGaugeStatusLegend =
    "0=battery-not-installed-or-disabled,1=read-error-or-implausible,2=valid; "
    "gauge_status stays 1 when only VCELL is valid, battery_mv is still "
    "written";
constexpr const char *kBatteryHealthStateLegend =
    "0=unknown,1=external-power-likely,2=ok,3=low,4=critical,5=settling; "
    "VCELL thresholds in acquisition.config are uncalibrated";
constexpr const char *kBatchCandidateStatusLegend =
    "0=disabled,1=identity-mismatch,2=PMS-unavailable,3=candidate-only";

// classification:classes for the board template (#85), out of the dict
// digest like kSemantics.
static constexpr opensensor::semantics::Class kClasses[] = {
#define CLASS(column, value, name, nodata)                                     \
  {#column, static_cast<std::int64_t>(value), name, nodata},
#include "telemetry_classes.inc"
#undef CLASS
};
// Code columns with no classes. pms_firmware and pms_error are raw PMS5003T
// frame bytes with no published code list, gauge_status_flags is a bitmask.
static constexpr std::string_view kClasslessColumns[] = {
    "pms_firmware", "pms_error", "gauge_status_flags"};
static_assert(opensensor::semantics::classes_complete(kSemantics, kClasses,
                                                      kClasslessColumns),
              "every code or boolean column needs telemetry_classes.inc rows");
static_assert(
    opensensor::semantics::matches_legend(kClasses, "pms_status",
                                          kPmsStatusLegend) &&
        opensensor::semantics::matches_legend(kClasses, "gauge_status",
                                              kGaugeStatusLegend) &&
        opensensor::semantics::matches_legend(kClasses, "battery_health_state",
                                              kBatteryHealthStateLegend) &&
        opensensor::semantics::matches_legend(kClasses,
                                              "pm25_batch_candidate_status",
                                              kBatchCandidateStatusLegend),
    "telemetry_classes.inc and the status legends differ");
// The clock sources this board writes, in code order. clock_status rows
// must be exactly these, each named by opensensor::utc::source_name.
static constexpr std::int64_t kClockSources[] = {kClockNone, kClockHost,
                                                 kClockNetwork};
static_assert(opensensor::semantics::matches_codes(
                  kClasses, "clock_status", kClockSources,
                  [](std::int64_t code) {
                    return opensensor::utc::source_name(
                        static_cast<std::int32_t>(code));
                  }),
              "clock_status classes must be the board's clock sources");

// opensensor:derived of the modelResult columns (#85). The waveshare module
// (sensor_modules.h) writes the batch candidate as 0.003964 times the 0.3 um
// count.
static constexpr std::string_view kBatchCandidateSources[] = {
    "particles_gt03_per_01l"};
static constexpr opensensor::semantics::Derived kDerived[] = {
    {"pm25_batch_candidate_ug_m3", kBatchCandidateSources,
     "urn:walkthru-earth:method:airgradient_20260408_count_candidate_v1"}};
static_assert(opensensor::semantics::derived_complete(kFields, kSemantics,
                                                      kDerived),
              "every modelResult column needs one kDerived entry");

// Board template title and keywords (#85). The title names the board and its
// sensors by make and model. Keywords are generic search terms.
constexpr const char *kTemplateTitle =
    "Waveshare ESP32-S3-SIM7670G-4G V2.0 with Plantower PMS5003T and Analog "
    "Devices MAX17048";
static constexpr std::string_view kTemplateKeywords[] = {
    "air quality",     "particulate matter", "PM2.5",
    "air temperature", "relative humidity",  "low-cost sensor"};
// Footer template href (#88, #89). The published template of this dict at
// the commit that last wrote it, whose bytes hash to kDescriptorSha256. It is
// outside the template, so pinning it never moves desc. Written by
// `pixi run pin-uris` after the template commit, checked by pinned-uris-test.
// A PR that moves dict or desc lands with a merge commit so that commit stays
// on main. Every other PR squashes.
constexpr const char *kTemplateHref =
    "https://raw.githubusercontent.com/walkthru-earth/"
    "opensensor-space-firmware/"
    "bb618f32a8a9bfe6a633a22ad5e99d319d55d46b/"
    "docs/shared/templates/waveshare-esp32-s3-sim7670g/"
    "a8605aafb6f76b794e9e7fb32927be6d203afeeb14527206c0e49c1eb0436809.json";
static_assert(opensensor::pinned::names_template(kTemplateHref,
                                                 "waveshare-esp32-s3-sim7670g",
                                                 kDictionarySha256),
              "kTemplateHref is the published template of this dict at a "
              "full commit");
} // namespace contract
} // namespace telemetry
