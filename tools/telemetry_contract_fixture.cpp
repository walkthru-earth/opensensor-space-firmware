#include "../firmware/common/src/lz4_codec.h"
#include "../firmware/esp-idf-cores3/bringup/telemetry_contract.h"

#include "board_template.h"
#include "footer_fixture.h"
#include "live_sample.h"
#include "live_widest.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <os_logger_cadence.h>
#include <os_logger_footer.h>
#include <os_logger_live.h>
#include <os_logger_location.h>

using namespace telemetry;
using namespace telemetry::contract;

namespace {
bool sink(void *context, const std::uint8_t *bytes, std::size_t count) {
  return std::fwrite(bytes, 1, count, static_cast<FILE *>(context)) == count;
}
} // namespace

namespace {
// Every board LIVE key must be accepted by begin(), and a row with every LIVE
// key valid at its widest real value must still fit one BLE JSON value.
// Widest values a real producer writes while each key is valid. INT64
// counters take INT64_MAX so no uptime horizon is assumed. `utc` is the
// largest microsecond count whose nanosecond form fits INT64, 19 digits.
// Status codes are one digit and PMS words are u16.
constexpr std::int64_t kI64 = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kUtcUs = kI64 / 1000; // 9223372036854775000 ns
constexpr std::int64_t kU16 = 65535;
// `imu_t` is BMI270 int16/512 + 23, so -41.0 is its widest. `vbus` and
// `bat` are M5Unified int16_t mV. `chg` is ChargingStatus (0..2), mapped
// from M5Unified getChargeState() in telemetry_logger.cpp. `t` and `rh`
// have no producer yet (sht20_unresolved). They take a signed int16 deci
// degree and 100.0 so a future sensor cannot push the row over silently.
constexpr livewidest::Widest kWidest[] = {
    {"seq", kI64, 0.0F},    {"mono", kI64, 0.0F},  {"utc", kUtcUs, 0.0F},
    {"pms", 9, 0.0F},       {"c1", kU16, 0.0F},    {"c25", kU16, 0.0F},
    {"c10", kU16, 0.0F},    {"pm1", kU16, 0.0F},   {"pm25", kU16, 0.0F},
    {"pm10", kU16, 0.0F},   {"n03", kU16, 0.0F},   {"n05", kU16, 0.0F},
    {"n1", kU16, 0.0F},     {"n25", kU16, 0.0F},   {"n5", kU16, 0.0F},
    {"n10", kU16, 0.0F},    {"imu_t", 0, -41.0F},  {"als", kU16, 0.0F},
    {"vbus", -32768, 0.0F}, {"bat", -32768, 0.0F}, {"pct", 100, 0.0F},
    {"chg", 2, 0.0F},       {"t", 0, -3276.8F},    {"rh", 0, 100.0F},
};
// Measured 2026-10-26 with every declared key present, a one digit code or
// a 100 percent counted as null since null is wider, and each float at its
// field's decimals (t prints two). The widest object is 333 bytes, 480 - 333 =
// 147 bytes of headroom. The all-null object sent on overflow is 265 bytes. A
// change here means a LIVE key, its widest value or its decimals changed,
// update the numbers.
constexpr std::size_t kWidestLiveBytes = 333;
constexpr std::size_t kAllNullLiveBytes = 265;
void assert_live_keys(const oslogger::Field *table, std::size_t count) {
  const auto bytes = livewidest::measure(table, count, kWidest);
  const auto nulls = livewidest::all_null(table, count);
  assert(bytes <= ble::kMaxJson);
  assert(bytes == kWidestLiveBytes);
  assert(nulls <= ble::kMaxJson);
  assert(nulls == kAllNullLiveBytes);
}
struct BoardTable {
  oslogger::Field fields[field_count];
  BoardTable() {
    for (std::size_t i = 0; i < field_count; ++i)
      fields[i] = {kFields[i].name,      kFields[i].type,
                   kFields[i].procedure, kFields[i].unit,
                   kFields[i].validity,  kFields[i].property_uri,
                   kFields[i].live_key,  kFields[i].decimals};
  }
};
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
  const BoardTable board;
  assert_live_keys(board.fields, field_count);
  if (argc == 2 && template_mode(argv[1]))
    return 0;
  if (argc == 2 && std::strcmp(argv[1], "live") == 0) {
    livesample::print(board.fields, field_count);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "footer") == 0) {
    footerfixture::print();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "dictionary") == 0) {
    std::printf("{\"schema\":\"%s\",\"schema_version\":%d,"
                "\"dictionary\":\"%s\",\"firmware\":\"%s\",\"sha256\":\"%s\","
                "\"uri\":\"%s\",\"descriptor_sha256\":\"%s\",\"fields\":[",
                kSchemaName, int(kSchemaVersion), kDictionaryVersion, kFirmware,
                kDictionarySha256, kDictionaryUri, kDescriptorSha256);
    for (std::size_t i = 0; i < field_count; ++i) {
      const auto &f = kFields[i];
      // Dictionary tokens are compile-time literals, validated by the host.
      std::printf("%s{\"name\":\"%s\",\"type\":%u,\"procedure\":\"%s\","
                  "\"unit\":\"%s\",\"validity\":\"%s\",\"property\":\"%s\","
                  "\"field_id\":%zu,\"live\":\"%s\",\"decimals\":%u}",
                  i ? "," : "", f.name, unsigned(f.type), f.procedure, f.unit,
                  f.validity, f.property_uri, i + 1,
                  f.live_key ? f.live_key : "", unsigned(f.decimals));
    }
    std::puts("]}");
    return 0;
  }
  if (argc != 4 && argc != 5)
    return 2;
  const bool located = argc == 5 && std::strcmp(argv[4], "located") == 0;
  const bool compressed = std::strcmp(argv[2], "lz4") == 0;
  const bool anchored = std::strcmp(argv[3], "anchored") == 0;
  oslogger::LocationSample<Sample> rows[90]{};
  const config::LocationSettings location =
      located ? config::LocationSettings{0x8828308281fffffULL, 8, "US"}
              : config::LocationSettings{};
  Column columns[field_count]{};
  Workspace workspace{};
  Lz4Workspace lz4{};
  const auto compression = lz4.configuration();
  for (std::size_t i = 0; i < 90; ++i) {
    auto &row = rows[i];
    row.location = location;
    oslogger::apply_location(
        row, row.location,
        {h3_cell_id, h3_resolution, location_source, geometry});
    const auto now = std::int64_t(20000000 + i * 10000000);
    row.integer(schema_version, kSchemaVersion);
    row.counter(sequence, i);
    row.counter(monotonic_us, now);
    row.counter(scheduled_us, now - 123);
    row.counter(sample_jitter_us, 123);
    apply_clock(row, now, 15000000, 1788890000000000000LL, anchored ? 1 : 0);
    row.counter(collection_completed_mono_us, now + 1234);
    if (i > 0) {
      row.counter(pms_received_mono_us, now - 250000);
      row.counter(pms_age_ms, 250);
      row.integer(pms_status, 4);
      for (std::size_t j = 0; j < 12; ++j)
        row.integer(static_cast<Field>(pm1_cf1_ug_m3 + j), i + j);
    } else
      row.integer(pms_status, 0);
    row.number(accel_x_g, 0.125F);
    row.number(ambient_temperature_c, std::numeric_limits<float>::quiet_NaN());
    assert(!row.valid[ambient_temperature_c]);
    assert(!row.valid[battery_current_ma]);
  }
  // A new anchor must not change a previously captured row.
  Sample corrected{};
  const auto old_utc = rows[1].data[event_time_utc_us];
  apply_clock(corrected, 30000000, 25000000, 1788889000000000000LL, 2);
  assert(corrected.data[clock_epoch] == 2);
  assert(corrected.data[event_time_utc_us] == 1788889005000000LL);
  assert(rows[1].data[event_time_utc_us] == old_utc);
  // An RTC-restored anchor is dated like a host one but carries code 2; with
  // no anchor the source code is ignored and the status stays 0.
  Sample restored{}, unanchored{};
  apply_clock(restored, 30000000, 25000000, 1788889000000000000LL, 1,
              kClockRtc);
  assert(restored.data[clock_status] == kClockRtc);
  assert(restored.data[clock_epoch] == 1);
  assert(restored.data[event_time_utc_us] == 1788889005000000LL);
  Sample network{};
  apply_clock(network, 30000000, 25000000, 1788889000123456000LL, 2,
              kClockNetwork);
  assert(network.data[clock_status] == kClockNetwork);
  assert(network.valid[event_time_utc_us] &&
         network.data[event_time_utc_us] == 1788889005123456LL);
  assert(network.valid[clock_anchor_mono_us] &&
         network.valid[clock_anchor_utc_us]);
  apply_clock(unanchored, 30000000, 0, 0, 0, kClockRtc);
  assert(unanchored.data[clock_status] == kClockNone);
  assert(!unanchored.valid[event_time_utc_us]);
  prepare_columns(columns, rows);
  FILE *file = std::fopen(argv[1], "wb");
  if (!file)
    return 3;
  // Same calls as the logger: one row group per 90-row batch, rows ascending
  // by sequence, firmware identity in created_by.
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
