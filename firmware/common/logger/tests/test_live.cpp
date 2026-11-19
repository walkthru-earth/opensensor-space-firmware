#include <os_logger_live.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using oslogger::Field;
using telemetry::PhysicalType;

namespace {
Field field(const char *name, PhysicalType type, const char *live_key) {
  Field result;
  result.name = name;
  result.type = type;
  result.procedure = "fixture";
  result.unit = "1";
  result.validity = "always";
  result.property_uri = "urn:fixture";
  result.live_key = live_key;
  result.decimals = type == PhysicalType::Float ? 1 : 0;
  return result;
}

void keys() {
  const Field good[] = {field("a", PhysicalType::Int64, "seq"),
                        field("b", PhysicalType::Int32, nullptr),
                        field("c", PhysicalType::Float, "imu_t"),
                        field("d", PhysicalType::Int32, "n10")};
  assert(oslogger::valid_live_keys(good, 4));
  assert(oslogger::valid_live_keys(nullptr, 0));
  const Field duplicate[] = {field("a", PhysicalType::Int64, "t"),
                             field("b", PhysicalType::Float, "t")};
  assert(!oslogger::valid_live_keys(duplicate, 2));
  // A GEOMETRY point stays in the file, LIVE carries numbers only (#83).
  const Field point[] = {field("geometry", PhysicalType::ByteArray, "geo")};
  assert(!oslogger::valid_live_keys(point, 1));
  const Field file_point[] = {
      field("geometry", PhysicalType::ByteArray, nullptr)};
  assert(oslogger::valid_live_keys(file_point, 1));
  for (const char *bad : {"", "T", "a-b", "a\"b", "sixteen_chars_xx"}) {
    const Field one[] = {field("a", PhysicalType::Int32, bad)};
    assert(!oslogger::valid_live_keys(one, 1));
  }
  const Field longest[] = {field("a", PhysicalType::Int32, "fifteen_chars_x")};
  assert(oslogger::valid_live_keys(longest, 1));
  Field too_precise[] = {field("a", PhysicalType::Float, "ax")};
  too_precise[0].decimals = oslogger::kMaxLiveDecimals;
  assert(oslogger::valid_live_keys(too_precise, 1));
  too_precise[0].decimals = oslogger::kMaxLiveDecimals + 1;
  assert(!oslogger::valid_live_keys(too_precise, 1));
}

// Each Float prints with its own field's decimals, rounded, integers ignore it.
void decimals() {
  Field fields[] = {field("accel", PhysicalType::Float, "ax"),
                    field("whole", PhysicalType::Float, "w"),
                    field("fine", PhysicalType::Float, "f"),
                    field("count", PhysicalType::Int32, "n")};
  fields[0].decimals = 3;
  fields[1].decimals = 0;
  fields[2].decimals = oslogger::kMaxLiveDecimals;
  fields[3].decimals = 3;
  oslogger::Row row{};
  row.set(0, -0.0123f);
  row.set(1, 26.6f);
  row.set(2, 1.0f);
  row.set(3, std::int32_t{7});
  char json[96];
  assert(oslogger::format_live(fields, 4, row, json, sizeof(json)));
  assert(std::strcmp(json, "{\"ax\":-0.012,\"w\":27,\"f\":1.000000,"
                           "\"n\":7}") == 0);
  // A value too wide for the value buffer at its decimals is no reading.
  row.set(2, 1e30f);
  assert(oslogger::format_live(fields, 4, row, json, sizeof(json)));
  assert(std::strstr(json, ",\"f\":null,"));
}

// Prints each document for the runner to parse with a real JSON reader.
void documents() {
  const Field fields[] = {
      field("sequence", PhysicalType::Int64, "seq"),
      field("file_only", PhysicalType::Int32, nullptr),
      field("pm25", PhysicalType::Int32, "pm25"),
      field("temperature", PhysicalType::Float, "t"),
      field("pm1", PhysicalType::Int32, "pm1"),
      field("utc", PhysicalType::Int64, "utc"),
      field("humidity", PhysicalType::Float, "rh"),
  };
  oslogger::Row row{};
  row.set(0, std::int64_t{42});
  row.set(1, std::int32_t{7}); // valid but has no key
  row.set(2, std::int32_t{-12});
  row.set(3, 26.06f);
  row.set(4, std::numeric_limits<std::int32_t>::min());
  row.set(6, std::numeric_limits<float>::quiet_NaN()); // never "nan"
  char json[256];
  const auto length = oslogger::format_live(fields, 7, row, json, sizeof(json));
  assert(length == std::strlen(json));
  // Every declared key is present. Unset utc and NaN rh are null, not zero.
  assert(std::strcmp(json, "{\"seq\":42,\"pm25\":-12,\"t\":26.1,"
                           "\"pm1\":-2147483648,\"utc\":null,"
                           "\"rh\":null}") == 0);
  std::puts(json);
  // Exactly enough room for the NUL succeeds, one byte less is refused.
  char exact[128];
  assert(oslogger::format_live(fields, 7, row, exact, length + 1) == length);
  assert(oslogger::format_live(fields, 7, row, exact, length) == 0);
  assert(exact[0] == '\0');
  row.set(5, std::numeric_limits<std::int64_t>::max());
  row.set(6, -0.04f);
  assert(oslogger::format_live(fields, 7, row, json, sizeof(json)));
  std::puts(json);
  const oslogger::Row empty{};
  assert(oslogger::format_live(fields, 7, empty, json, sizeof(json)));
  assert(std::strcmp(json, "{\"seq\":null,\"pm25\":null,\"t\":null,"
                           "\"pm1\":null,\"utc\":null,\"rh\":null}") == 0);
  std::puts(json);
  // Infinity is no reading either.
  row.set(6, std::numeric_limits<float>::infinity());
  assert(oslogger::format_live(fields, 7, row, json, sizeof(json)));
  assert(std::strstr(json, ",\"rh\":null}"));
  // A table with no declared keys publishes an empty object.
  assert(oslogger::format_live(fields + 1, 1, row, json, sizeof(json)) == 2);
  assert(std::strcmp(json, "{}") == 0);
  assert(oslogger::format_live(fields, 7, row, json, 0) == 0);
  assert(oslogger::format_live(fields, 7, row, nullptr, 8) == 0);
}

// LIVE `utc` carries the microseconds its column stores, unscaled.
void utc_scale() {
  Field utc = field("event_time_utc_us", PhysicalType::Int64, "utc");
  utc.unit = "us_since_unix_epoch";
  oslogger::Row row{};
  row.set(0, std::int64_t{1757990410000123});
  char json[64];
  assert(oslogger::format_live(&utc, 1, row, json, sizeof(json)));
  assert(std::strcmp(json, "{\"utc\":1757990410000123}") == 0);
  // The whole INT64 range prints, so no UTC is dropped for its size.
  row.set(0, std::numeric_limits<std::int64_t>::max());
  assert(oslogger::format_live(&utc, 1, row, json, sizeof(json)));
  assert(std::strcmp(json, "{\"utc\":9223372036854775807}") == 0);
  row.set(0, std::numeric_limits<std::int64_t>::min());
  assert(oslogger::format_live(&utc, 1, row, json, sizeof(json)));
  assert(std::strcmp(json, "{\"utc\":-9223372036854775808}") == 0);
}
// An object over the BLE cap logs its real size once and publishes every key
// as null, so the previous sample is never served as current. Fitting again
// restores it.
void overflow() {
  static const char *const names[] = {
      "k00", "k01", "k02", "k03", "k04", "k05", "k06", "k07", "k08", "k09",
      "k10", "k11", "k12", "k13", "k14", "k15", "k16", "k17", "k18", "k19"};
  constexpr std::size_t count = sizeof(names) / sizeof(names[0]);
  Field fields[count];
  for (std::size_t i = 0; i < count; ++i)
    fields[i] = field("wide", PhysicalType::Int64, names[i]);
  constexpr std::size_t kCap = ble::kMaxJson;
  oslogger::Row wide{};
  for (std::size_t i = 0; i < count; ++i)
    wide.set(i, std::numeric_limits<std::int64_t>::max());
  oslogger::Row small{};
  small.set(0, std::int64_t{7});

  char big[1024];
  const auto full =
      oslogger::format_live(fields, count, wide, big, sizeof(big));
  assert(full > kCap);
  assert(oslogger::live_size(fields, count, wide) == full);
  // {"k00":7} then 19 keys of ,"kNN":null
  const std::size_t small_size = 1 + 7 + 19 * 11 + 1;
  assert(oslogger::live_size(fields, count, small) == small_size);
  char nulls[kCap + 1];
  const oslogger::Row none{};
  const auto null_size =
      oslogger::format_live(fields, count, none, nulls, sizeof(nulls));
  assert(null_size == 1 + 20 * 11 - 1 + 1);
  assert(std::strstr(nulls, "{\"k00\":null,\"k01\":null,") == nulls);

  oslogger::LiveOverflow gate;
  char json[kCap + 1];
  auto live = gate.publish(fields, count, small, json, sizeof(json), 0);
  assert(live.length == small_size && !live.log && live.needed == 0);
  assert(std::strstr(json, "{\"k00\":7,\"k01\":null,") == json);

  live = gate.publish(fields, count, wide, json, sizeof(json), 10000000);
  assert(live.log && live.needed == full);
  assert(live.length == null_size && std::strcmp(json, nulls) == 0);
  char line[64];
  const auto written =
      oslogger::format_live_overflow(line, sizeof(line), live.needed, kCap);
  assert(written == std::strlen(line));
  char expected[64];
  std::snprintf(expected, sizeof(expected),
                "PARQUET LIVE overflow bytes=%zu max=480\n", full);
  assert(std::strcmp(line, expected) == 0);
  assert(oslogger::format_live_overflow(line, 8, live.needed, kCap) == 0);
  assert(line[0] == '\0');

  // Still overflowing within the hour, cleared again but not logged again.
  live = gate.publish(fields, count, wide, json, sizeof(json), 20000000);
  assert(!live.log && live.needed == full && live.length == null_size);
  assert(std::strcmp(json, nulls) == 0);

  live = gate.publish(fields, count, small, json, sizeof(json), 30000000);
  assert(!live.log && live.length == small_size);

  // A later overflow logs again once the repeat window has passed.
  const auto later = 10000000 + oslogger::LiveOverflow::kRepeatUs;
  live = gate.publish(fields, count, wide, json, sizeof(json), later - 1);
  assert(!live.log && live.length == null_size);
  live = gate.publish(fields, count, wide, json, sizeof(json), later);
  assert(live.log && live.length == null_size);

  // No buffer means nothing to publish and nothing to log.
  live = gate.publish(fields, count, wide, nullptr, sizeof(json), later * 2);
  assert(!live.log && live.length == 0);
}
} // namespace

int main() {
  keys();
  decimals();
  documents();
  utc_scale();
  overflow();
  return 0;
}
