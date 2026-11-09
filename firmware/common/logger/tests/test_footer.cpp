// Board neutral footer, cadence and BLE name checks. A non air quality
// fixture board (no PMS, 30 s period, SL- prefix) writes a real Parquet file
// through the shared writer, and tools/test_logger_footer.py reads it with
// PyArrow and DuckDB. The default `opensensor` footer JSON is pinned (#88).
#include "os_logger_footer.h"
#include <ble_advert.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <source_location>
#include <span>
#include <string>
#include <string_view>

namespace {
using oslogger::Field;
using telemetry::KeyValue;
using telemetry::PhysicalType;
namespace cadence = oslogger::cadence;
namespace footer = oslogger::footer;

// Side effects stay in the condition, so this never compiles out.
void check(bool condition,
           std::source_location where = std::source_location::current()) {
  if (condition)
    return;
  std::fprintf(stderr, "check failed at line %u\n", unsigned(where.line()));
  std::abort();
}

void same(const char *actual, const char *expected) {
  check(actual && expected && std::strcmp(actual, expected) == 0);
}

constexpr Field kRuntimeFields[] = {
    {"sequence", PhysicalType::Int64, "runtime", "count", "always", "urn:seq"},
    {"monotonic_us", PhysicalType::Int64, "runtime", "us", "always",
     "urn:mono"},
    {"event_time_utc_us", PhysicalType::Int64, "runtime", "us_since_unix_epoch",
     "null before a clock anchor", "urn:utc"},
    {"clock_epoch", PhysicalType::Int32, "runtime", "count", "always",
     "urn:epoch"},
};

constexpr const char *kDictSha =
    "0000000000000000000000000000000000000000000000000000000000000001";
constexpr const char *kDescSha =
    "0000000000000000000000000000000000000000000000000000000000000002";

oslogger::Config base_config() {
  return {
      .fields = kRuntimeFields,
      .column_count = std::size(kRuntimeFields),
      .schema_version = 1,
      .schema_name = "schema",
      .firmware = "firmware",
      .dictionary_version = "dictionary",
      .dictionary_uri = "https://example.org/dictionary.json",
      .dictionary_sha256 = kDictSha,
      .configuration_id = "config-id",
      .configuration = "{}",
      .time_semantics = "time",
      .board = "board",
      .descriptor_sha256 = kDescSha,
      .template_href = "https://example.org/template.json",
  };
}

constexpr const char *kBoot = "0123456789abcdef0123456789abcdef";
constexpr oslogger::footer::Ids kIds{
    .device = "d", .thing = "s", .boot = kBoot, .mobility = "fixed"};
constexpr oslogger::footer::File kFile{.rotation_seconds = 900,
                                       .row_groups = 1,
                                       .compression = "LZ4_RAW",
                                       .benchmark = false};

// The `opensensor` JSON for a board that sets no optional field keeps the
// defaults (10000, 90, unknown) in schema order, byte for byte (#88).
void footer_bytes() {
  auto config = base_config();
  check(config.sample_interval_ms == 10000);
  check(config.ble_name_prefix == nullptr); // required board constant
  // The board template is required too, begin() rejects a missing one.
  check(config.descriptor == nullptr && config.descriptor_bytes == 0);
  static char json[footer::kMaxJsonBytes + 1];
  const auto length = footer::build(json, config, kIds, kFile);
  check(length.has_value());
  constexpr std::string_view kFrozen =
      R"({"version":"0.1.0",)"
      R"("thing":{"id":"s","device_id":"d","board":"board","deployment_id":"unknown",)"
      R"("mobility":"fixed"},)"
      R"("firmware":{"version":"firmware","schema":"schema","boot_id":"0123456789abcdef0123456789abcdef"},)"
      R"("dictionary":{"sha256":"0000000000000000000000000000000000000000000000000000000000000001",)"
      R"("version":"dictionary","uri":"https://example.org/dictionary.json"},)"
      R"("template":{"href":"https://example.org/template.json",)"
      R"("sha256":"0000000000000000000000000000000000000000000000000000000000000002"},)"
      R"("acquisition":{"sample_interval_ms":10000,"time_semantics":"time",)"
      R"("config_id":"config-id","config":"{}","calibration_id":"unknown"},)"
      R"("file":{"rotation_interval_s":900,"row_groups":1,"row_group_rows_max":90,)"
      R"("compression":"LZ4_RAW","purpose":"telemetry"}})";
  check(length.has_value() && *length == kFrozen.size());
  check(std::string_view(json) == kFrozen);

  // A codec comparison file at the one hour cap.
  check(footer::build(json, config, kIds,
                      {.rotation_seconds = footer::kMaxRotationSeconds,
                       .row_groups = 4,
                       .compression = "UNCOMPRESSED",
                       .benchmark = true})
            .has_value());
  const std::string_view benchmark(json);
  check(benchmark.ends_with(
      R"("file":{"rotation_interval_s":3600,"row_groups":4,)"
      R"("row_group_rows_max":90,"compression":"UNCOMPRESSED",)"
      R"("purpose":"codec-comparison-duplicate-rows"}})"));

  // Quotes, backslashes and control bytes are escaped, UTF-8 passes through.
  config.deployment_id = "a\"b\\c\n\x01 \xc3\xa9";
  check(footer::build(json, config, kIds, kFile).has_value());
  check(std::string_view(json).find(R"("deployment_id":"a\"b\\c\n\u0001 )"
                                    "\xc3\xa9\"") != std::string_view::npos);
  config = base_config();

  // The file's deployment id snapshot wins over the board constant (#148).
  // Unset, null or empty, the footer keeps "unknown".
  const auto deployment_of = [&](const char *deployment) {
    auto ids = kIds;
    ids.deployment = deployment;
    check(footer::build(json, config, ids, kFile).has_value());
    const std::string_view text(json);
    const auto key = text.find(R"("deployment_id":")");
    check(key != std::string_view::npos);
    const auto start = key + std::string_view(R"("deployment_id":")").size();
    return std::string(text.substr(start, text.find('"', start) - start));
  };
  check(deployment_of("cairo-07") == "cairo-07");
  check(deployment_of("") == "unknown");
  check(deployment_of(nullptr) == "unknown");
  config.deployment_id = "board-default";
  check(deployment_of("") == "board-default");
  check(deployment_of("giza-01") == "giza-01");
  config = base_config();

  // Fails closed, never a partial footer.
  const auto fails = [&](const oslogger::Config &bad, const footer::File &file,
                         footer::Error error) {
    const auto result = footer::build(json, bad, kIds, file);
    check(!result.has_value() && result.error() == error);
  };
  // A bad board constant also fails check_config(), so begin() refuses to
  // start instead of quarantining every file.
  check(footer::check_config(config).has_value());
  const auto config_fails = [&](const oslogger::Config &bad,
                                footer::Error error) {
    fails(bad, kFile, error);
    const auto checked = footer::check_config(bad);
    check(!checked.has_value() && checked.error() == error);
  };
  for (const char *oslogger::Config::*member :
       {&oslogger::Config::board, &oslogger::Config::firmware,
        &oslogger::Config::schema_name, &oslogger::Config::dictionary_sha256,
        &oslogger::Config::dictionary_uri, &oslogger::Config::template_href,
        &oslogger::Config::descriptor_sha256, &oslogger::Config::time_semantics,
        &oslogger::Config::deployment_id}) {
    auto bad = base_config();
    bad.*member = "";
    config_fails(bad, footer::Error::Missing);
    bad.*member = nullptr;
    config_fails(bad, footer::Error::Missing);
  }
  for (const char *oslogger::Config::*member :
       {&oslogger::Config::dictionary_version,
        &oslogger::Config::configuration_id, &oslogger::Config::configuration,
        &oslogger::Config::calibration_id}) {
    auto empty = base_config();
    empty.*member = "";
    check(footer::build(json, empty, kIds, kFile).has_value());
    check(footer::check_config(empty).has_value());
    empty.*member = nullptr;
    config_fails(empty, footer::Error::Missing);
  }
  check(!footer::build(
             json, config,
             {.device = "d", .thing = "", .boot = kBoot, .mobility = "fixed"},
             kFile)
             .has_value());
  // thing.board is the board slug, never free text (stac-opensensor 83787768).
  for (const char *board :
       {"CoreS3 ESP32-S3 rev0.2", "M5Stack-CoreS3", "a_b"}) {
    auto bad = base_config();
    bad.board = board;
    config_fails(bad, footer::Error::Range);
  }
  // firmware.boot_id is the 32 hex token of the file name.
  for (const char *boot : {"b", "0123456789abcdef0123456789abcde",
                           "0123456789abcdef0123456789abcdef0",
                           "0123456789ABCDEF0123456789ABCDEF"}) {
    const auto bad = footer::build(
        json, config,
        {.device = "d", .thing = "s", .boot = boot, .mobility = "fixed"},
        kFile);
    check(!bad.has_value() && bad.error() == footer::Error::Range);
  }
  // thing.mobility is required and one of fixed or moving.
  for (const char *mobility : {"", "parked", "Fixed"}) {
    const auto bad = footer::build(
        json, config,
        {.device = "d", .thing = "s", .boot = kBoot, .mobility = mobility},
        kFile);
    check(!bad.has_value() &&
          bad.error() == (*mobility == '\0' ? footer::Error::Missing
                                            : footer::Error::Range));
  }
  check(!footer::build(json, config,
                       {.device = "d", .thing = "s", .boot = kBoot}, kFile)
             .has_value());
  check(footer::build(
            json, config,
            {.device = "d", .thing = "s", .boot = kBoot, .mobility = "moving"},
            kFile)
            .has_value());
  check(std::string_view(json).find(R"("mobility":"moving"})") !=
        std::string_view::npos);
  fails(config, {.rotation_seconds = 900, .row_groups = 1, .compression = ""},
        footer::Error::Missing);
  fails(config, {.rotation_seconds = 0, .row_groups = 1, .compression = "x"},
        footer::Error::Range);
  fails(config,
        {.rotation_seconds = footer::kMaxRotationSeconds + 1,
         .row_groups = 1,
         .compression = "x"},
        footer::Error::Range);
  fails(config, {.rotation_seconds = 900, .row_groups = 0, .compression = "x"},
        footer::Error::Range);
  // Both digests must match ^[0-9a-f]{64}$.
  for (const char *oslogger::Config::*member :
       {&oslogger::Config::dictionary_sha256,
        &oslogger::Config::descriptor_sha256}) {
    for (const char *digest :
         {"000000000000000000000000000000000000000000000000000000000000001",
          "00000000000000000000000000000000000000000000000000000000000000001",
          "000000000000000000000000000000000000000000000000000000000000000A",
          "000000000000000000000000000000000000000000000000000000000000000g"}) {
      auto bad = base_config();
      bad.*member = digest;
      config_fails(bad, footer::Error::Digest);
    }
  }
  static_assert(footer::error_name(footer::Error::Digest) ==
                std::string_view("footer-bad-sha256"));
  auto zero = base_config();
  zero.sample_interval_ms = 0;
  config_fails(zero, footer::Error::Range);
  // The JSON needs its length plus a NUL.
  static char exact[kFrozen.size() + 1];
  check(footer::build(exact, config, kIds, kFile).has_value());
  const auto overflow = footer::build(std::span<char>(exact, kFrozen.size()),
                                      config, kIds, kFile);
  check(!overflow.has_value() && overflow.error() == footer::Error::Overflow);
  static_assert(footer::error_name(footer::Error::Overflow) ==
                std::string_view("footer-overflow"));

  // The old rule was rotation / 10, capped at 90 rows.
  for (const std::uint32_t rotation : {600U, 900U, 1800U, 3600U}) {
    const std::size_t old_rows = rotation / 10 < 90 ? rotation / 10 : 90;
    check(cadence::rows_per_group(rotation, 10000) == old_rows);
  }
}

// The `geo` JSON, byte for byte, with and without a bbox.
void geo_bytes() {
  static char json[footer::kMaxGeoJsonBytes];
  const auto none = footer::build_geo(json, std::nullopt);
  constexpr std::string_view kNoBox =
      R"({"version":"2.0.0","primary_column":"geometry","columns":)"
      R"({"geometry":{"encoding":"WKB","geometry_types":["Point"]}}})";
  check(none.has_value() && *none == kNoBox.size());
  check(std::string_view(json) == kNoBox);

  const telemetry::PointBounds cairo{.xmin_e7 = 312345678,
                                     .ymin_e7 = 300000000,
                                     .xmax_e7 = 313000001,
                                     .ymax_e7 = 300500000};
  const auto box = footer::build_geo(json, cairo);
  constexpr std::string_view kBox =
      R"({"version":"2.0.0","primary_column":"geometry","columns":)"
      R"({"geometry":{"encoding":"WKB","geometry_types":["Point"],)"
      R"("bbox":[31.2345678,30,31.3000001,30.05]}}})";
  check(box.has_value() && *box == kBox.size());
  check(std::string_view(json) == kBox);

  // The widest bbox still fits the documented buffer.
  const telemetry::PointBounds world{.xmin_e7 = -telemetry::kMaxLongitudeE7 + 1,
                                     .ymin_e7 = -telemetry::kMaxLatitudeE7 + 1,
                                     .xmax_e7 = telemetry::kMaxLongitudeE7 - 1,
                                     .ymax_e7 = telemetry::kMaxLatitudeE7 - 1};
  const auto wide = footer::build_geo(json, world);
  check(wide.has_value() && *wide < footer::kMaxGeoJsonBytes);
  check(std::string_view(json).ends_with(
      R"("bbox":[-179.9999999,-89.9999999,179.9999999,89.9999999]}}})"));

  // Exact decimals, the text parses to the same double as e7 / 1e7.
  std::array<char, 12> digits{};
  const auto decimal = [&](std::int32_t e7) {
    return std::string_view(digits.data(),
                            footer::detail::e7_decimal(digits, e7));
  };
  check(decimal(0) == "0");
  check(decimal(-1) == "-0.0000001");
  check(decimal(10) == "0.000001");
  check(decimal(-1800000000) == "-180");
  check(decimal(900000000) == "90");
  check(decimal(-5) == "-0.0000005");

  // Out of range or inverted bounds fail closed.
  const auto bounds = [](std::int32_t xmin, std::int32_t ymin,
                         std::int32_t xmax, std::int32_t ymax) {
    return telemetry::PointBounds{
        .xmin_e7 = xmin, .ymin_e7 = ymin, .xmax_e7 = xmax, .ymax_e7 = ymax};
  };
  for (const telemetry::PointBounds bad :
       {bounds(-1800000001, 0, 0, 0), bounds(0, 0, 1800000001, 0),
        bounds(0, -900000001, 0, 0), bounds(0, 0, 0, 900000001),
        bounds(2, 0, 1, 0), bounds(0, 2, 0, 1)}) {
    const auto result = footer::build_geo(json, bad);
    check(!result.has_value() && result.error() == footer::Error::Range);
  }
  const auto small =
      footer::build_geo(std::span<char>(json, kNoBox.size()), std::nullopt);
  check(!small.has_value() && small.error() == footer::Error::Overflow);
}

void cadence_bounds() {
  for (const std::uint32_t ms : {1000U, 5000U, 10000U, 30000U, 60000U, 300000U})
    check(cadence::valid_sample_interval(ms));
  for (const std::uint32_t ms : {0U, 999U, 7000U, 45000U, 600000U})
    check(!cadence::valid_sample_interval(ms));
  check(cadence::rows_per_group(600, 1000) == 90);
  check(cadence::rows_per_group(600, 300000) == 2);
  check(cadence::rows_per_group(900, 30000) == 30);
  check(cadence::rows_per_group(3600, 30000) == 90);
  check(cadence::rows_per_group(0, 10000) == 1);
  check(cadence::rows_per_group(900, 0) == 1);
}

// The deadline step poll() runs, driven with a 30 s period as a non 10 s
// board would configure it (#98 M8).
void cadence_deadline() {
  constexpr auto period = cadence::period_us(30000);
  static_assert(period == 30000000);
  static_assert(cadence::period_us(cadence::kDefaultSampleIntervalMs) ==
                10000000);
  constexpr std::int64_t start = 5000000;
  // Not due before the deadline.
  constexpr auto early = cadence::advance(start - 1, start, period);
  static_assert(!early.due && early.skipped == 0 && early.next_us == start);
  // Due at the deadline, stamped with it, next one period later.
  constexpr auto on_time = cadence::advance(start, start, period);
  static_assert(on_time.due && on_time.skipped == 0 &&
                on_time.deadline_us == start &&
                on_time.next_us == start + period);
  // Late within the period, still no skip.
  constexpr auto late = cadence::advance(start + period - 1, start, period);
  static_assert(late.due && late.skipped == 0 && late.deadline_us == start &&
                late.next_us == start + period);
  // Two whole periods missed, the sample takes the latest deadline.
  constexpr auto stalled =
      cadence::advance(start + 2 * period + 7, start, period);
  static_assert(stalled.due && stalled.skipped == 2 &&
                stalled.deadline_us == start + 2 * period &&
                stalled.next_us == start + 3 * period);
  // A period of 0 is never due, so a bad config cannot spin the loop.
  static_assert(!cadence::advance(start + period, start, 0).due);
  // Walk ten minutes of polls every 100 ms. Each deadline is sampled once.
  std::int64_t next = start;
  std::int64_t samples = 0, skipped = 0;
  for (std::int64_t now = 0; now <= start + 20 * period; now += 100000) {
    const auto step = cadence::advance(now, next, period);
    if (!step.due)
      continue;
    check(step.deadline_us == start + samples * period);
    skipped += step.skipped;
    ++samples;
    next = step.next_us;
  }
  check(samples == 21 && skipped == 0 && next == start + 21 * period);
}

void names() {
  std::array<char, 16> name{};
  std::array<char, 24> host{};
  const ble::Identity unset{"s", "0123456789ab", "b", "schema", 0, "sha", "fw"};
  check(unset.name_prefix == nullptr);
  check(!ble::wire::local_name(name, unset.name_prefix, unset.device));
  // CoreS3 and Waveshare pass the OpenSensor prefix.
  check(ble::wire::local_name(name, "OS-", "0123456789ab"));
  same(name.data(), "OS-89ab");
  check(ble::wire::mdns_host(host, name.data()));
  same(host.data(), "os-89ab");
  check(ble::wire::local_name(name, "SL-", "0123456789ab"));
  same(name.data(), "SL-89ab");
  check(ble::wire::mdns_host(host, name.data()));
  same(host.data(), "sl-89ab");
  // Seven prefix characters reach the 11 character scan response limit.
  check(ble::wire::local_name(name, "ABCDEFG", "6b40"));
  same(name.data(), "ABCDEFG6b40");
  ble::wire::ScanResponse response;
  check(ble::wire::legacy_scan_response(name.data(), response));
  check(!ble::wire::legacy_scan_response("ABCDEFGH6b40", response));
  // The prefix validator itself, at and past the seven character limit.
  static_assert(ble::wire::valid_name_prefix("OS-"));
  static_assert(ble::wire::valid_name_prefix("ABCDEFG"));
  static_assert(!ble::wire::valid_name_prefix("ABCDEFGH"));
  check(ble::wire::valid_name_prefix("ABCDEFG"));
  check(!ble::wire::valid_name_prefix("ABCDEFGH"));
  check(!ble::wire::valid_name_prefix(""));
  check(!ble::wire::valid_name_prefix(nullptr));
  for (const char *bad : std::array<const char *, 6>{"", "ABCDEFGH", "OS_",
                                                     "OS ", "os.", nullptr})
    check(!ble::wire::local_name(name, bad, "0123456789ab") && name[0] == '\0');
  std::array<char, 7> small{}; // OS-89ab needs 8 with its NUL
  check(!ble::wire::local_name(small, "OS-", "0123456789ab"));
  check(!ble::wire::local_name(name, "OS-", nullptr));
  check(!ble::wire::mdns_host(host, "") && host[0] == '\0');
  check(!ble::wire::mdns_host(small, "ABCDEFG6b40") && small[0] == '\0');
}

// Non air quality fixture board, a soil probe with no PMS, 30 s period, SL-
// prefix.
constexpr Field kSoilFields[] = {
    kRuntimeFields[0],
    kRuntimeFields[1],
    kRuntimeFields[2],
    kRuntimeFields[3],
    {"soil_moisture_pct", PhysicalType::Float, "fixture capacitive probe",
     "percent", "null when the probe is absent", "urn:soil-moisture"},
    {"soil_temperature_c", PhysicalType::Float, "fixture thermistor",
     "degree_celsius", "null when the probe is absent", "urn:soil-t"},
};

struct SoilRow {
  std::int64_t sequence = 0, monotonic_us = 0, utc_us = 0;
  std::int32_t epoch = 0;
  float moisture = 0, temperature = 0;
  std::uint8_t utc_valid = 0, moisture_valid = 0, temperature_valid = 0;
};

bool file_sink(void *context, const std::uint8_t *bytes, std::size_t length) {
  return std::fwrite(bytes, 1, length, static_cast<FILE *>(context)) == length;
}

void soil_board(const char *path) {
  auto config = base_config();
  config.fields = kSoilFields;
  config.column_count = std::size(kSoilFields);
  config.schema_name = "soil-fixture-v1";
  config.board = "fixture-soil-board";
  config.sample_interval_ms = 30000;
  config.deployment_id = "fixture-plot-7";
  config.calibration_id = "soil-cal-2026-10";
  config.ble_name_prefix = "SL-";
  check(cadence::valid_sample_interval(config.sample_interval_ms));
  check(ble::wire::valid_name_prefix(config.ble_name_prefix));
  for (std::size_t i = 0; i < config.column_count; ++i)
    check(std::strncmp(config.fields[i].name, "pm", 2) != 0);

  // One 3600 s window at 30 s holds 120 rows, written as 90 + 30.
  constexpr std::uint32_t kRotation = 3600;
  const auto group_rows =
      cadence::rows_per_group(kRotation, config.sample_interval_ms);
  check(group_rows == cadence::kMaxBatchRows);
  constexpr std::size_t kRows = 120;
  static std::array<SoilRow, cadence::kMaxBatchRows> batch{};
  std::array<telemetry::Column, std::size(kSoilFields)> columns{};
  const auto stride = sizeof(SoilRow);
  const void *values[] = {&batch[0].sequence, &batch[0].monotonic_us,
                          &batch[0].utc_us,   &batch[0].epoch,
                          &batch[0].moisture, &batch[0].temperature};
  const std::uint8_t *valid[] = {nullptr,
                                 nullptr,
                                 &batch[0].utc_valid,
                                 nullptr,
                                 &batch[0].moisture_valid,
                                 &batch[0].temperature_valid};
  for (std::size_t i = 0; i < columns.size(); ++i)
    columns[i] = {
        .name = kSoilFields[i].name,
        .type = kSoilFields[i].type,
        .values = values[i],
        .stride = stride,
        .valid = valid[i],
        .valid_stride = stride,
        .logical = i == 2 ? telemetry::LogicalType::TimestampMicrosUtc
                          : telemetry::LogicalType::None,
        .field_id = static_cast<std::int32_t>(i + 1),
    };
  static telemetry::Workspace workspace{};
  FILE *file = std::fopen(path, "wb");
  check(file);
  telemetry::Writer writer;
  check(writer.begin(file_sink, file, workspace, columns.data(), columns.size())
            .ok);
  const std::int64_t sample_us = std::int64_t{config.sample_interval_ms} * 1000;
  std::size_t written = 0;
  while (written < kRows) {
    const std::size_t count =
        kRows - written < group_rows ? kRows - written : group_rows;
    for (std::size_t i = 0; i < count; ++i) {
      const auto n = static_cast<std::int64_t>(written + i);
      batch[i] = {.sequence = n,
                  .monotonic_us = 5000000 + n * sample_us,
                  .utc_us = 1790000000000000 + n * sample_us,
                  .epoch = 1,
                  .moisture = 20.0F + static_cast<float>(n % 7),
                  .temperature = 18.5F,
                  .utc_valid = 1,
                  .moisture_valid = static_cast<std::uint8_t>(n % 5 != 0),
                  .temperature_valid = 1};
    }
    check(writer.row_group(count, 1).ok);
    written += count;
  }
  // The one footer key, as oslogger::finalize_file writes it.
  static char json[footer::kMaxJsonBytes + 1];
  check(footer::build(json, config,
                      {.device = "0123456789ab",
                       .thing = "00000000-0000-4000-8000-000000000000",
                       .boot = kBoot,
                       .mobility = "fixed"},
                      {.rotation_seconds = kRotation,
                       .row_groups = writer.row_groups(),
                       .compression = "UNCOMPRESSED",
                       .benchmark = false})
            .has_value());
  // No GEOMETRY column, so the helper the logger uses gives one entry.
  const auto metadata = footer::metadata(json, "", false);
  check(metadata.count == 1);
  check(writer
            .finish(metadata.entries.data(), metadata.count, config.firmware,
                    "fixture-soil-board")
            .ok);
  check(std::fclose(file) == 0);
  std::array<char, 16> name{};
  std::array<char, 24> host{};
  check(ble::wire::local_name(name, config.ble_name_prefix, "0123456789ab"));
  check(ble::wire::mdns_host(host, name.data()));
  std::printf("%s %s\n", name.data(), host.data());
}
} // namespace

int main(int argc, char **argv) {
  check(argc == 2);
  footer_bytes();
  geo_bytes();
  cadence_bounds();
  cadence_deadline();
  names();
  soil_board(argv[1]);
}
