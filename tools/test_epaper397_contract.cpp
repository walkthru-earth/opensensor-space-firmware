#include "../firmware/esp-idf-waveshare-epaper-397/logger/sensor_modules.h"
#include "../firmware/esp-idf-waveshare-epaper-397/logger/telemetry_contract.h"
#include "board_template.h"
#include "footer_fixture.h"
#include "live_sample.h"
#include "live_widest.h"
#include <lz4_codec.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <os_gnss.h>
#include <os_logger_cadence.h>
#include <os_logger_footer.h>
#include <os_logger_live.h>
#include <os_logger_location.h>

using namespace telemetry;
using namespace telemetry::contract;
namespace sen63c = ossensor::sen63c;

namespace {
bool sink(void *context, const std::uint8_t *bytes, std::size_t count) {
  return std::fwrite(bytes, 1, count, static_cast<FILE *>(context)) == count;
}

constexpr std::int64_t kStart = 1000000;
constexpr std::int64_t kNow = kStart + 2 * kSen6xPmWarmupUs;

// A measuring SEN63C with a fresh result at `now`, every value in range.
board_modules::Sen63cSnapshot measuring(std::int64_t now) {
  board_modules::Sen63cSnapshot s{};
  s.present = true;
  s.measuring = true;
  s.start_mono_us = now - 2 * kSen6xPmWarmupUs;
  s.has_values = true;
  s.received_mono_us = now - 250000;
  s.values.pm1_ug_m3 = 1.2F;
  s.values.pm25_ug_m3 = 3.4F;
  s.values.pm4_ug_m3 = 5.6F;
  s.values.pm10_ug_m3 = 7.8F;
  s.values.temperature_c = -3.7F;
  s.values.relative_humidity_percent = 63.4F;
  s.values.co2_ppm = std::int16_t{612};
  s.has_device_status = true;
  return s;
}

// Widest values a real producer writes while each key is valid. INT64
// counters and the microsecond `utc` take INT64_MAX so no uptime or date
// horizon is assumed, 19 digits.
// `sen` is one digit. A mass is a u16 tenth below the 0xFFFF sentinel.
// `t` is clamped to -40..80 and `rh` to 0..100 by reading_of(). `co2` is a
// signed int16, so -32768 prints widest.
constexpr std::int64_t kI64 = std::numeric_limits<std::int64_t>::max();
constexpr livewidest::Widest kWidest[] = {
    {"seq", kI64, 0.0F}, {"mono", kI64, 0.0F},  {"utc", kI64, 0.0F},
    {"sen", 9, 0.0F},    {"m1", 0, 6553.4F},    {"m25", 0, 6553.4F},
    {"m4", 0, 6553.4F},  {"m10", 0, 6553.4F},   {"t", 0, -40.0F},
    {"rh", 0, 100.0F},   {"co2", -32768, 0.0F},
};
// Measured 2026-11-17 with every declared key present, a one digit code
// counted as null since null is wider, and each float at its field's
// decimals (all print one). The widest object is 175 bytes, 480 - 175 = 305
// bytes of headroom. The all-null object sent on overflow is 118 bytes. A
// change here means a LIVE key, its widest value or its decimals changed,
// update the numbers.
constexpr std::size_t kWidestLiveBytes = 175;
constexpr std::size_t kAllNullLiveBytes = 118;
void assert_live_keys(const oslogger::Field *table, std::size_t count) {
  const auto bytes = livewidest::measure(table, count, kWidest);
  const auto nulls = livewidest::all_null(table, count);
  if (bytes != kWidestLiveBytes || nulls != kAllNullLiveBytes) {
    std::fprintf(stderr, "LIVE widest %zu bytes, all null %zu bytes\n", bytes,
                 nulls);
    std::abort();
  }
  assert(bytes <= ble::kMaxJson && nulls <= ble::kMaxJson);
}

void assert_status_gates() {
  using namespace board_modules;
  // Missing until the sensor answers and runs.
  board_modules::Sen63cSnapshot none{};
  assert(status_of(none, kNow) == sen63c::kStatusMissing);
  auto stopped = measuring(kNow);
  stopped.measuring = false;
  assert(status_of(stopped, kNow) == sen63c::kStatusMissing);
  const auto idle = reading_of(stopped, kNow);
  assert(!idle.temperature_c && !idle.since_start_s && idle.device_status);
  // Warm-up runs from the last start, and a start after the row warms.
  auto s = measuring(kNow);
  s.start_mono_us = kNow - kSen6xPmWarmupUs + 1;
  assert(status_of(s, kNow) == sen63c::kStatusWarming);
  s.start_mono_us = kNow - kSen6xPmWarmupUs;
  assert(status_of(s, kNow) == sen63c::kStatusValid);
  s.start_mono_us = kNow + 1;
  assert(status_of(s, kNow) == sen63c::kStatusWarming);
  assert(!reading_of(s, kNow).since_start_s);
  // RH and T read through the PM warm-up.
  s.start_mono_us = kNow - 1000000;
  const auto warm = reading_of(s, kNow);
  assert(warm.temperature_c && warm.relative_humidity_percent &&
         !warm.pm25_ug_m3 && !warm.co2_ppm && warm.since_start_s == 1U);
  // Stale after kSen6xStaleAfterUs, inclusive edge still fresh.
  s = measuring(kNow);
  s.received_mono_us = kNow - kSen6xStaleAfterUs;
  assert(status_of(s, kNow) == sen63c::kStatusValid);
  s.received_mono_us = kNow - kSen6xStaleAfterUs - 1;
  assert(status_of(s, kNow) == sen63c::kStatusStale);
  assert(!reading_of(s, kNow).temperature_c);
  // A result stamped after the row is fresh.
  s.received_mono_us = kNow + 5;
  assert(status_of(s, kNow) == sen63c::kStatusValid);
  // A PM error keeps RH and T, an RHT error drops them, a fan speed warning
  // is no error.
  s = measuring(kNow);
  s.device_status = opensensor::Sen6xStatus::kPmError;
  auto r = reading_of(s, kNow);
  assert(r.status == sen63c::kStatusSensorError && r.temperature_c &&
         !r.pm25_ug_m3 && r.device_status == 0x800U);
  s.device_status = opensensor::Sen6xStatus::kRhtError;
  r = reading_of(s, kNow);
  assert(r.status == sen63c::kStatusSensorError && !r.temperature_c &&
         !r.relative_humidity_percent);
  s.device_status = opensensor::Sen6xStatus::kFanSpeedWarning;
  assert(status_of(s, kNow) == sen63c::kStatusValid);
  // Range gates null one value, never the row.
  s = measuring(kNow);
  s.values.relative_humidity_percent = 100.1F;
  s.values.temperature_c = 80.0F;
  r = reading_of(s, kNow);
  assert(r.status == sen63c::kStatusValid && !r.relative_humidity_percent &&
         r.temperature_c == 80.0F && r.pm25_ug_m3 == 3.4F);
  s.values.temperature_c = -40.1F;
  assert(!reading_of(s, kNow).temperature_c);
  // A sentinel value stays null.
  s = measuring(kNow);
  s.values.co2_ppm.reset();
  s.values.pm4_ug_m3.reset();
  r = reading_of(s, kNow);
  assert(!r.co2_ppm && !r.pm4_ug_m3 && r.pm10_ug_m3);
}

// The module writes only its columns, and collect restarts the status
// latch from the newest read so a cleared error reaches one row.
void assert_module() {
  board_modules::Sen63cSnapshot snapshot = measuring(kNow);
  constinit static board_modules::Sources sources{};
  sources.sensor = &snapshot;
  static constexpr auto kTable = board_modules::table(sources);
  static_assert(kTable[0].procedure == sen63c::kProcedure);
  snapshot.device_status = opensensor::Sen6xStatus::kPmError;
  snapshot.device_status_latest = 0;
  Sample first{}, second{};
  board_modules::collect(kTable, first, kNow);
  assert(first.data[sen6x_status] == sen63c::kStatusSensorError &&
         first.data[sen6x_device_status] == 0x800);
  assert(sources.last.status == sen63c::kStatusSensorError);
  board_modules::collect(kTable, second, kNow);
  assert(second.data[sen6x_status] == sen63c::kStatusValid &&
         second.data[sen6x_device_status] == 0 && second.valid[co2_ppm] &&
         second.data[co2_ppm] == 612);
  for (std::size_t i = 0; i < field_count; ++i) {
    const bool own = kFields[i].procedure == std::string_view{"sen63c"};
    assert(own || !second.valid[i]);
  }
}

void assert_gates() {
  assert_status_gates();
  assert_module();
  Sample earlier{}, corrected{};
  apply_clock(earlier, 30000000, 15000000, 1788890000000000000LL, 1);
  const auto utc = earlier.data[event_time_utc_us];
  apply_clock(corrected, 40000000, 35000000, 1788889000000000000LL, 2);
  assert(corrected.data[clock_epoch] == 2);
  assert(corrected.data[event_time_utc_us] == 1788889005000000LL);
  assert(earlier.data[event_time_utc_us] == utc);
  Sample unsynced{};
  apply_clock(unsynced, 30000000, 0, 0, 0, kClockNetwork);
  assert(unsynced.data[clock_status] == kClockNone);
  assert(!unsynced.valid[event_time_utc_us]);
}

// The synthetic snapshot of row i. Rows 0 to 6 each exercise one gate, the
// rest are valid.
board_modules::Sen63cSnapshot snapshot_for(std::size_t i, std::int64_t now) {
  auto s = measuring(now);
  switch (i) {
  case 0:
    s = {};
    break;
  case 1:
    s.start_mono_us = now - 1000000;
    break;
  case 2:
    s.received_mono_us = now - kSen6xStaleAfterUs - 1;
    break;
  case 3:
    s.device_status = opensensor::Sen6xStatus::kPmError;
    break;
  case 4:
    s.device_status = opensensor::Sen6xStatus::kRhtError;
    break;
  case 5:
    s.values.relative_humidity_percent = 101.0F;
    break;
  case 6:
    s.values.co2_ppm.reset();
    break;
  default:
    break;
  }
  return s;
}
} // namespace

namespace {
// The board template (#85) from the board tables, and the bytes the firmware
// embeds. tools/board_template.py compares the two and hashes the embedded
// bytes as desc.
std::string board_template() {
  const boardtemplate::Board board{
      .id = kSchemaName,
      .title = kTemplateTitle,
      .keywords = kTemplateKeywords,
      .dictionary_href = kDictionaryUri,
      .dict = kDictionarySha256,
      .sample_interval_ms = oslogger::cadence::kDefaultSampleIntervalMs,
      .proto_min = ble::kProtocolVersion,
  };
  return boardtemplate::build(board, kFields, kSemantics, kClasses, kDerived);
}
bool template_mode(const char *mode) {
  if (std::strcmp(mode, "template") == 0) {
    const auto text = board_template();
    std::fwrite(text.data(), 1, text.size(), stdout);
    return true;
  }
  if (std::strcmp(mode, "embedded") == 0) {
    std::fwrite(kTemplateJson, 1, sizeof(kTemplateJson) - 1, stdout);
    return true;
  }
  return false;
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && template_mode(argv[1]))
    return 0;
  if (argc == 2 && std::strcmp(argv[1], "dictionary") == 0) {
    std::printf("{\"schema\":\"%s\",\"schema_version\":%d,"
                "\"dictionary\":\"%s\",\"firmware\":\"%s\",\"sha256\":\"%s\","
                "\"uri\":\"%s\",\"descriptor_sha256\":\"%s\",\"fields\":[",
                kSchemaName, int(kSchemaVersion), kDictionaryVersion, kFirmware,
                kDictionarySha256, kDictionaryUri, kDescriptorSha256);
    for (std::size_t i = 0; i < field_count; ++i) {
      const auto &field = kFields[i];
      std::printf(
          "%s{\"name\":\"%s\",\"type\":%u,\"procedure\":\"%s\","
          "\"unit\":\"%s\",\"validity\":\"%s\",\"property\":\"%s\","
          "\"field_id\":%zu,\"live\":\"%s\",\"decimals\":%u}",
          i ? "," : "", field.name, unsigned(field.type), field.procedure,
          field.unit, field.validity, field.property_uri, i + 1,
          field.live_key ? field.live_key : "", unsigned(field.decimals));
    }
    std::puts("]}");
    return 0;
  }
  assert_live_keys(kFields, field_count);
  if (argc == 2 && std::strcmp(argv[1], "live") == 0) {
    livesample::print(kFields, field_count);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "footer") == 0) {
    footerfixture::print();
    return 0;
  }
  if (argc != 4 && argc != 5)
    return 2;
  assert_gates();
  const bool located = argc == 5 && std::strcmp(argv[4], "located") == 0;
  const bool compressed = std::strcmp(argv[2], "lz4") == 0;
  const bool anchored = std::strcmp(argv[3], "anchored") == 0;
  static oslogger::LocationSample<Sample> rows[90]{};
  const config::LocationSettings location =
      located ? config::LocationSettings{0x8828308281fffffULL, 8, "US"}
              : config::LocationSettings{};
  Column columns[field_count]{};
  static Workspace workspace{};
  Lz4Workspace lz4{};
  const auto compression = lz4.configuration();
  for (std::size_t i = 0; i < 90; ++i) {
    auto &row = rows[i];
    row.location = location;
    // The production path of collect(), with no receiver fix.
    const auto chosen =
        opensensor::gnss::select_location(row.location, std::nullopt, 0);
    oslogger::apply_cell(
        row, chosen.cell, chosen.source,
        {h3_cell_id, h3_resolution, location_source, geometry});
    const auto now = std::int64_t(10000000 + i * 10000000);
    row.integer(schema_version, kSchemaVersion);
    row.counter(device_id, 123);
    row.counter(boot_id_hi, 456);
    row.counter(boot_id_lo, 789);
    row.counter(sequence, i);
    row.counter(monotonic_us, now);
    row.counter(scheduled_us, now - 123);
    row.counter(sample_jitter_us, 123);
    apply_clock(row, now, 15000000, 1788890000000000000LL, anchored ? 1 : 0);
    row.counter(collection_completed_mono_us, now + 1234);
    board_modules::write_sen63c(
        row, board_modules::reading_of(snapshot_for(i, now), now));
  }
  prepare_columns(columns, rows);
  FILE *file = std::fopen(argv[1], "wb");
  if (!file)
    return 3;
  Writer writer;
  auto result = writer.begin(sink, file, workspace, columns, field_count,
                             compressed ? &compression : nullptr);
  if (result.ok)
    result = writer.row_group(90, sequence);
  // The two footer keys the logger writes, built by the firmware code. geo
  // takes its bbox from the rows written, so it follows the row group.
  static char json[oslogger::footer::kMaxJsonBytes + 1];
  static char geo[oslogger::footer::kMaxGeoJsonBytes + 1];
  const auto metadata = oslogger::footer::metadata(
      footerfixture::build(json, 900, 1, compressed),
      footerfixture::geo(geo, writer, geometry), true);
  if (result.ok)
    result = writer.finish(metadata.entries.data(), metadata.count, kFirmware,
                           kCreatedBy);
  const bool closed = std::fclose(file) == 0;
  return result.ok && closed ? 0 : 4;
}
