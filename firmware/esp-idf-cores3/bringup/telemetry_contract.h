#pragma once

#include "parquet_writer.h"
#include "telemetry_descriptor_digest.h"
#include "telemetry_dictionary_digest.h"
#include "telemetry_logical.h"
#include "telemetry_template.h"
#include <algorithm>
#include <column_semantics.h>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <numeric_sample.h>
#include <os_logger_live.h>
#include <pinned_uri.h>
#include <span>
#include <string_view>
#include <utc_clock.h>

namespace telemetry {
namespace contract {
// File schema v5 keeps the 81 leaves of v4 in the same order, renames the two
// UTC instants to microseconds (`*_utc_us`, TIMESTAMP(MICROS, UTC)) and writes
// a Parquet field_id on every leaf so an Apache Iceberg v3 table can register
// the file. The dictionary version tracks the field bytes and schema v5 tracks
// the reader contract. Dictionary v5 (firmware v6.13) adds each LIVE key to the
// field table so the digest covers it. Columns are unchanged from v4.
// Dictionary v6 (firmware v6.14) adds per field LIVE decimals, columns stay v5.
// Schema v6 and dictionary v7 (firmware v6.21, #83, owner decision 32) put
// location_source (UINT8) and geometry (native Parquet GEOMETRY, a 2D WKB
// Point at the H3 cell centre) next to h3_cell_id and h3_resolution, field
// ids 80 and 81. Still 81 leaves. This is the Thing field table baseline,
// field ids are plain 1-based positions and append only from here (owner
// answer to bridge question 36, 2026-10-26).
constexpr std::int32_t kSchemaVersion = 6;
constexpr const char *kSchemaName = "cores3-telemetry-v6";
constexpr const char *kDictionaryVersion = "cores3-telemetry-v7";
constexpr const char *kFirmware = "idf-cores3-parquet-v6.30";
constexpr const char *kCreatedBy = "opensensor-space-firmware version 0.3";
// Raw telemetry_fields.inc at a pinned commit on main (#89), so the bytes at
// the URI hash to kDictionarySha256. It is also the template describedby href.
// Written by `pixi run pin-uris`, checked by pinned-uris-test.
constexpr const char *kDictionaryUri =
    "https://raw.githubusercontent.com/walkthru-earth/"
    "opensensor-space-firmware/"
    "093f2b8e45cd2b9e24e1324ad9fa2e6e20ad1d47/"
    "firmware/esp-idf-cores3/bringup/telemetry_fields.inc";
static_assert(opensensor::pinned::names(kDictionaryUri,
                                        "firmware/esp-idf-cores3/bringup/"
                                        "telemetry_fields.inc"),
              "kDictionaryUri is the raw field table at a full commit");
// Version the acquisition procedure separately from the physical file codec.
constexpr const char *kConfigurationId = "cores3-acquisition-v4";
constexpr const char *kConfiguration =
    "{\"sample_interval_ms\":10000,\"pms_warmup_us\":30000000,"
    "\"pms_stale_after_ms\":5000,\"pms_uart_rx\":18,\"pms_uart_tx\":17,"
    "\"pms_baud\":9600,\"m5unified\":\"0.2.29\",\"esp_idf\":\"6.1\",\"ltr553\":"
    "\"raw-v1\","
    "\"snapshot\":\"latest-available-not-average\"}";
constexpr const char *kUnknown = "unknown";
// Footer thing.board, the board slug of the template path. The free text name
// is the template title (stac-opensensor 83787768).
constexpr const char *kBoard = "m5stack-cores3";
constexpr const char *kTimeSemantics =
    "event_time_utc_us estimates snapshot start; collection_completed_mono_us "
    "bounds sequential reads; pms_received_mono_us is last checksum-valid UART "
    "frame receipt, not sensor phenomenon time; result/ingestion time and "
    "clock uncertainty unknown; no retroactive UTC assignment";

enum Field : std::size_t {
#define FIELD(name, type, procedure, unit, validity, live, decimals) name,
#include "telemetry_fields.inc"
#undef FIELD
  field_count
};
struct Definition {
  const char *name = nullptr;
  PhysicalType type = PhysicalType::Int32;
  const char *procedure = nullptr;
  const char *unit = nullptr;
  const char *validity = nullptr;
  const char *property_uri = nullptr;
  const char *live_key = nullptr;
  std::uint8_t decimals = 0;
};
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
static_assert(field_count == 81, "Version the schema when changing fields");
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

using Sample = opensensor::NumericSample<field_count>;

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

// `clock_status` codes. The anchor pairs an external UTC estimate with a
// device monotonic instant; the code says where that pairing came from.
//   0  no anchor: UTC fields null, rows go to the `unsynced` tree
//   1  host estimate supplied on this boot (serial `parquet time`, BLE/LAN
//      SET_TIME); the same value is written to the BM8563 RTC
//   2  restored at boot from the BM8563 RTC, which only ever holds a value a
//      host or SNTP supplied earlier (whole seconds, plus RTC
//      drift since that sync); a later host sync starts a new epoch
//   3  successful Wi-Fi SNTP response on this boot, microsecond resolution
enum ClockSource : std::int32_t {
  kClockNone = opensensor::utc::None,
  kClockHost = opensensor::utc::Host,
  kClockRtc = opensensor::utc::Rtc,
  kClockNetwork = opensensor::utc::Network,
};

// Status codes collect() writes in telemetry_logger.cpp. telemetry_classes.inc
// names each value for the board template (#85), and the status legends below
// must list the same pairs. The legends are compile time checks only, the
// footer carries no code meanings (#88).
enum PmsStatus : std::int32_t {
  kPmsMissing = 0,
  kPmsWarming = 1,
  kPmsStale = 2,
  kPmsSensorError = 3,
  kPmsValid = 4,
};
enum LightStatus : std::int32_t {
  kLightUnavailable = 0,
  kLightNotFresh = 1,
  kLightInvalid = 2,
  kLightValid = 3,
  kLightIoError = 4,
};
enum ProximityStatus : std::int32_t {
  kProximityUnavailable = 0,
  kProximityNotFresh = 1,
  kProximitySaturated = 2,
  kProximityValid = 3,
  kProximityIoError = 4,
};
// Mapped from M5Unified charge_state_t. 0 covers every not charging state
// (not charging, full, disabled, discharging, idle) and 2 every state that was
// not read. This header stays free of M5Unified for the host fixture, so
// telemetry_logger.cpp holds the mapping and its static_assert.
enum ChargingStatus : std::int32_t {
  kChargingDischarging = 0,
  kChargingCharging = 1,
  kChargingUnknown = 2,
};
// The charging_status codes collect() writes, checked against the classes.
constexpr const char *kChargingStatusLegend =
    "0=discharging,1=charging,2=unknown";
constexpr const char *kPmsStatusLegend =
    "0=missing,1=warming,2=stale,3=sensor-error,4=valid";
constexpr const char *kLightStatusLegend =
    "0=unavailable,1=not-fresh,2=invalid,3=valid,4=io-error";
constexpr const char *kProximityStatusLegend =
    "0=unavailable,1=not-fresh,2=saturated,3=valid,4=io-error";

// classification:classes for the board template (#85), out of the dict
// digest like kSemantics.
static constexpr opensensor::semantics::Class kClasses[] = {
#define CLASS(column, value, name, nodata)                                     \
  {#column, static_cast<std::int64_t>(value), name, nodata},
#include "telemetry_classes.inc"
#undef CLASS
};
// Code columns with no classes. pms_firmware and pms_error are raw PMSA003
// frame bytes with no published code list, imu_fresh_mask is a bitmask.
static constexpr std::string_view kClasslessColumns[] = {
    "pms_firmware", "pms_error", "imu_fresh_mask"};
static_assert(opensensor::semantics::classes_complete(kSemantics, kClasses,
                                                      kClasslessColumns),
              "every code or boolean column needs telemetry_classes.inc rows");
static_assert(
    opensensor::semantics::matches_legend(kClasses, "pms_status",
                                          kPmsStatusLegend) &&
        opensensor::semantics::matches_legend(kClasses, "light_status",
                                              kLightStatusLegend) &&
        opensensor::semantics::matches_legend(kClasses, "proximity_status",
                                              kProximityStatusLegend) &&
        opensensor::semantics::matches_legend(kClasses, "charging_status",
                                              kChargingStatusLegend),
    "telemetry_classes.inc and the status legends differ");
// The clock sources this board writes, in code order. clock_status rows
// must be exactly these, each named by opensensor::utc::source_name.
static constexpr std::int64_t kClockSources[] = {kClockNone, kClockHost,
                                                 kClockRtc, kClockNetwork};
static_assert(opensensor::semantics::matches_codes(
                  kClasses, "clock_status", kClockSources,
                  [](std::int64_t code) {
                    return opensensor::utc::source_name(
                        static_cast<std::int32_t>(code));
                  }),
              "clock_status classes must be the board's clock sources");

// No CoreS3 column is a modelResult, so no column needs opensensor:derived.
static constexpr std::span<const opensensor::semantics::Derived> kDerived{};
static_assert(opensensor::semantics::derived_complete(kFields, kSemantics,
                                                      kDerived),
              "every modelResult column needs one kDerived entry");

// Board template title and keywords (#85). The title names the board and its
// sensors by make and model. Keywords are generic search terms.
constexpr const char *kTemplateTitle =
    "M5Stack CoreS3 with Plantower PMSA003, Bosch Sensortec BMI270 and BMM150, "
    "and Lite-On LTR-553ALS-WA";
static constexpr std::string_view kTemplateKeywords[] = {
    "air quality", "particulate matter", "PM2.5", "low-cost sensor"};
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
    "docs/shared/templates/m5stack-cores3/"
    "3b4d8500a19f42c6743b0f7d725276c96ca14a0c249f613048603afbc3cfaff9.json";
static_assert(opensensor::pinned::names_template(kTemplateHref,
                                                 "m5stack-cores3",
                                                 kDictionarySha256),
              "kTemplateHref is the published template of this dict at a "
              "full commit");

// Inputs are a single coherent anchor snapshot taken under the clock mutex.
// Caller provides a fresh zero-initialized row. Unknown UTC stays null.
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
} // namespace contract
} // namespace telemetry
