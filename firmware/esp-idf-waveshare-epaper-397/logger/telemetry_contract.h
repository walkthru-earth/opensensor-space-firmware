#pragma once

#include "telemetry_descriptor_digest.h"
#include "telemetry_dictionary_digest.h"
#include "telemetry_logical.h"
#include "telemetry_template.h"
#include <algorithm>
#include <column_semantics.h>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <os_logger.h>
#include <os_logger_live.h>
#include <pinned_uri.h>
#include <span>
#include <string_view>
#include <utc_clock.h>

namespace telemetry {
namespace contract {
// Schema v1 is the first Waveshare ESP32-S3-ePaper-3.97 table. It starts on
// the Thing baseline (#83), so field ids are plain 1-based positions and
// append only. The SEN63C writes Float mass columns at 0.1 ug/m3, never the
// Int32 Plantower columns of the CoreS3 and Waveshare V2.
constexpr std::int32_t kSchemaVersion = 1;
constexpr const char *kSchemaName = "waveshare-epaper-397-telemetry-v1";
constexpr const char *kDictionaryVersion = "waveshare-epaper-397-telemetry-v1";
constexpr const char *kFirmware = "idf-epaper397-parquet-v0.1";
constexpr const char *kCreatedBy = "opensensor-space-firmware version 0.3";
// Footer thing.board, the board slug of the template path. The free text name
// is the template title (stac-opensensor 83787768).
constexpr const char *kBoard = "waveshare-esp32-s3-epaper-397";
// Raw telemetry_fields.inc at a pinned commit on main (#89), so the bytes at
// the URI hash to kDictionarySha256. It is also the template describedby href.
// Written by `pixi run pin-uris`, checked by pinned-uris-test.
constexpr const char *kDictionaryUri =
    "https://raw.githubusercontent.com/walkthru-earth/"
    "opensensor-space-firmware/"
    "4261fa62738696d264c29f0fcfd493f844d9b725/"
    "firmware/esp-idf-waveshare-epaper-397/logger/telemetry_fields.inc";
static_assert(
    opensensor::pinned::names(kDictionaryUri,
                              "firmware/esp-idf-waveshare-epaper-397/logger/"
                              "telemetry_fields.inc"),
    "kDictionaryUri is the raw field table at a full commit");
constexpr const char *kConfigurationId = "waveshare-epaper-397-acquisition-v1";
// SEN63C datasheet timing. PM settles 30 s after a start, CO2 conditions for
// 24 s, and a result older than 3 s is stale. kConfiguration embeds the same
// numbers.
constexpr std::int64_t kSen6xPmWarmupUs = 30000000;
constexpr std::int64_t kSen6xCo2WarmupUs = 24000000;
constexpr std::int64_t kSen6xStaleAfterUs = 3000000;
constexpr const char *kConfiguration =
    "{\"sample_interval_ms\":10000,\"sen6x_model\":\"SEN63C\","
    "\"sen6x_i2c_sda\":43,\"sen6x_i2c_scl\":44,\"sen6x_address\":107,"
    "\"sen6x_i2c_hz\":100000,\"pm_warmup_us\":30000000,"
    "\"co2_warmup_us\":24000000,\"stale_after_us\":3000000,"
    "\"temperature_range_c\":[-40,80],\"humidity_range_percent\":[0,100],"
    "\"snapshot\":\"latest-available-not-average\"}";
constexpr const char *kUnknown = "unknown";
constexpr const char *kTimeSemantics =
    "event_time_utc_us estimates snapshot start; collection_completed_mono_us "
    "bounds sequential reads; the SEN63C result is the latest data-ready "
    "reading, not sensor phenomenon time; result/ingestion time and clock "
    "uncertainty unknown; a valid PCF85063 RTC read is an anchor of class "
    "rtc until a host or network anchor replaces it; no retroactive UTC "
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
static_assert(field_count == 39, "Version the schema when changing fields");
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
  kClockRtc = opensensor::utc::Rtc,
  kClockNetwork = opensensor::utc::Network,
};
// The caller supplies a coherent RTC, host or SNTP anchor. Each captured row
// keeps its original epoch permanently.
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

// Status legend of sen6x_status. telemetry_classes.inc names the same values
// for the board template (#85), and the static_assert below keeps the two
// equal. The legend is a compile time check only, the footer carries no code
// meanings (#88).
constexpr const char *kSen6xStatusLegend =
    "0=missing,1=warming,2=stale,3=sensor-error,4=valid";

// classification:classes for the board template (#85), out of the dict
// digest like kSemantics.
static constexpr opensensor::semantics::Class kClasses[] = {
#define CLASS(column, value, name, nodata)                                     \
  {#column, static_cast<std::int64_t>(value), name, nodata},
#include "telemetry_classes.inc"
#undef CLASS
};
// Code columns with no classes. sen6x_device_status is the raw SEN6x device
// status register, a bitmask.
static constexpr std::string_view kClasslessColumns[] = {"sen6x_device_status"};
static_assert(opensensor::semantics::classes_complete(kSemantics, kClasses,
                                                      kClasslessColumns),
              "every code or boolean column needs telemetry_classes.inc rows");
static_assert(opensensor::semantics::matches_legend(kClasses, "sen6x_status",
                                                    kSen6xStatusLegend),
              "telemetry_classes.inc and the status legend differ");
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

// opensensor:derived of the modelResult columns (#85). This board has none.
static constexpr std::span<const opensensor::semantics::Derived> kDerived{};
static_assert(opensensor::semantics::derived_complete(kFields, kSemantics,
                                                      kDerived),
              "every modelResult column needs one kDerived entry");

// Board template title and keywords (#85). The title names the board and its
// sensors by make and model. Keywords are generic search terms.
constexpr const char *kTemplateTitle =
    "Waveshare ESP32-S3-ePaper-3.97 with Sensirion SEN63C";
static constexpr std::string_view kTemplateKeywords[] = {
    "air quality",     "particulate matter", "PM2.5",          "carbon dioxide",
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
    "e7188b817e7b6790c388dc90528de219d532598f/"
    "docs/shared/templates/waveshare-esp32-s3-epaper-397/"
    "a19c450b6fc5bc4bb76e9b52e03aa75bee05cd3c1505d961771279366c820439.json";
static_assert(opensensor::pinned::names_template(
                  kTemplateHref, "waveshare-esp32-s3-epaper-397",
                  kDictionarySha256),
              "kTemplateHref is the published template of this dict at a "
              "full commit");
} // namespace contract
} // namespace telemetry
