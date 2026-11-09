#include "../firmware/esp-idf-waveshare-sim7670g/logger/sensor_modules.h"
#include "../firmware/esp-idf-waveshare-sim7670g/logger/telemetry_contract.h"
#include "board_template.h"
#include "footer_fixture.h"
#include "live_sample.h"
#include "live_widest.h"
#include "waveshare_collect_reference.h"
#include <lz4_codec.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <os_logger_cadence.h>
#include <os_logger_footer.h>
#include <os_logger_live.h>
#include <os_logger_location.h>
#include <vector>

using namespace telemetry;
using namespace telemetry::contract;

namespace {
bool sink(void *context, const std::uint8_t *bytes, std::size_t count) {
  return std::fwrite(bytes, 1, count, static_cast<FILE *>(context)) == count;
}
std::array<std::uint8_t, 32> sensor_bytes() {
  std::array<std::uint8_t, 32> bytes{};
  bytes[0] = 0x42;
  bytes[1] = 0x4d;
  bytes[3] = 28;
  for (std::size_t i = 0; i < 10; ++i)
    bytes[5 + 2 * i] = static_cast<std::uint8_t>(i + 1);
  bytes[24] = 0xff;
  bytes[25] = 0xdb; // signed -37 deciC
  bytes[26] = 2;
  bytes[27] = 122; // 63.4% RH
  bytes[28] = 7;
  std::uint16_t sum = 0;
  for (std::size_t i = 0; i < 30; ++i)
    sum += bytes[i];
  bytes[30] = static_cast<std::uint8_t>(sum >> 8);
  bytes[31] = static_cast<std::uint8_t>(sum);
  return bytes;
}
PmsSnapshot parsed_snapshot() {
  plantower::Parser parser{plantower::Model::Pms5003t};
  PmsSnapshot pms{};
  for (const auto byte : sensor_bytes())
    pms.present = parser.push(byte, pms.frame);
  assert(pms.present && pms.frame.particle_count_fields == 4);
  assert(pms.frame.temperature_deci_c == -37);
  assert(pms.frame.humidity_deci_percent == 634);
  pms.frames = 1;
  pms.age_ms = 250;
  pms.checksum_errors = 2;
  pms.length_errors = 3;
  return pms;
}
// The module writes under test, in table order. pms5003t and the waveshare
// identity module share one snapshot, max17048 and battery_health one read.
void pms_modules(Sample &row, const PmsSnapshot &pms, std::int64_t now,
                 const SensorProfile &sensor = {}) {
  board_modules::write_pms(row, pms, now);
  board_modules::write_identity(row, pms, now, sensor);
}
void gauge_modules(Sample &row, const BatteryHealth &health) {
  board_modules::write_gauge(row, health);
  board_modules::write_health(row, health);
}
// Board-owned identity rules installed into the shared runtime policy hook.
// Every board LIVE key must be accepted by begin(), and a row with every LIVE
// key valid at its widest real value must still fit one BLE JSON value.
// Widest values a real producer writes while each key is valid. INT64
// counters take INT64_MAX so no uptime horizon is assumed. `utc` is the
// largest microsecond count whose nanosecond form fits INT64, 19 digits.
// Status codes are one digit and PMS words are u16.
constexpr std::int64_t kI64 = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kUtcUs = kI64 / 1000; // 9223372036854775000 ns
constexpr std::int64_t kU16 = 65535;
// `t` is the PMS5003T signed int16 deci degree, so -3276.8 prints widest.
// `rh` is capped at 100.0. `bat` passes the MAX17048 2500..4500 mV check.
// `bstate` is opensensor::battery::State (0..5).
constexpr livewidest::Widest kWidest[] = {
    {"seq", kI64, 0.0F},  {"mono", kI64, 0.0F}, {"utc", kUtcUs, 0.0F},
    {"pms", 9, 0.0F},     {"c1", kU16, 0.0F},   {"c25", kU16, 0.0F},
    {"c10", kU16, 0.0F},  {"pm1", kU16, 0.0F},  {"pm25", kU16, 0.0F},
    {"pm10", kU16, 0.0F}, {"n03", kU16, 0.0F},  {"n05", kU16, 0.0F},
    {"n1", kU16, 0.0F},   {"n25", kU16, 0.0F},  {"t", 0, -3276.8F},
    {"rh", 0, 100.0F},    {"bat", 4500, 0.0F},  {"pct", 100, 0.0F},
    {"bstate", 5, 0.0F},
};
// Measured 2026-10-26 with every declared key present, a one digit code or
// a 100 percent counted as null since null is wider, and each float at its
// field's decimals (t and rh print one). The widest object is 270 bytes,
// 480 - 270 = 210 bytes of headroom. The all-null object sent on overflow is
// 211 bytes. A change here means a LIVE key, its widest value or its decimals
// changed, update the numbers.
constexpr std::size_t kWidestLiveBytes = 270;
constexpr std::size_t kAllNullLiveBytes = 211;
void assert_live_keys(const oslogger::Field *table, std::size_t count) {
  const auto bytes = livewidest::measure(table, count, kWidest);
  const auto nulls = livewidest::all_null(table, count);
  assert(bytes <= ble::kMaxJson);
  assert(bytes == kWidestLiveBytes);
  assert(nulls <= ble::kMaxJson);
  assert(nulls == kAllNullLiveBytes);
}
void assert_identity_rules() {
  using namespace pms5003t;
  assert(valid_serial("PMS5003T-202604081332"));
  assert(valid_serial("PMS5003T-202402291"));           // leap day
  assert(valid_serial("PMS5003T-20260408123456789"));   // 9 unit digits
  assert(!valid_serial("PMS5003T-202602291332"));       // not a leap year
  assert(!valid_serial("PMS5003T-202613011332"));       // month 13
  assert(!valid_serial("PMS5003T-202604001332"));       // day 0
  assert(!valid_serial("PMS5003T-199912311332"));       // before 2000
  assert(!valid_serial("PMS5003T-20260408"));           // no unit digits
  assert(!valid_serial("PMS5003T-202604081234567890")); // too long
  assert(!valid_serial("PMSA003-202604081332"));
  assert(!valid_serial("PMS5003T-2026040813A2"));
  assert(!valid_serial(nullptr));
  assert(serial_code(kVendor, kModel, "PMS5003T-20260408123456789") ==
         20260408123456789LL);
  assert(serial_code("Other", kModel, "PMS5003T-202604081332") == 0);
  assert(serial_code(kVendor, kModel, "PMS5003T-202602291332") == 0);
  assert(batch_eligible(kVendor, kModel, "PMS5003T-2026040813327"));
  assert(!batch_eligible(kVendor, kModel, "PMS5003T-202605011332"));
  assert(!batch_eligible(kVendor, "PMSA003", "PMS5003T-202604081332"));
}
opensensor::Max17048Extended extended_ok() {
  opensensor::Max17048Extended ext;
  ext.mode = {0x1000, true};
  ext.crate = {0xfff6, true};  // -10 LSB, -2.08 %/h
  ext.status = {0x4000, true}; // EnVr only
  return ext;
}
// A fake gauge bus so the driver, adapter and classifier run end to end.
struct GaugeBus {
  std::uint16_t vcell = 0;
  std::uint16_t soc = 0;
  bool nack_vcell = false;
  bool nack_soc = false;
  static bool read(void *context, std::uint8_t, std::uint8_t reg,
                   std::uint8_t *data, std::size_t) {
    const auto &bus = *static_cast<GaugeBus *>(context);
    const bool vcell = reg == opensensor::Max17048::kRegVcell;
    if (!vcell && reg != opensensor::Max17048::kRegSoc)
      return false;
    if (vcell ? bus.nack_vcell : bus.nack_soc)
      return false;
    const auto word = vcell ? bus.vcell : bus.soc;
    data[0] = static_cast<std::uint8_t>(word >> 8);
    data[1] = static_cast<std::uint8_t>(word & 0xff);
    return true;
  }
};
// 3300 mV every tick with a NACKed or implausible SOC reaches Critical
// after debounce_samples. battery_mv is written and SOC columns stay null.
void assert_voltage_only_critical() {
  for (const bool nack : {true, false}) {
    GaugeBus bus{0xa500, 0xffff, false, nack}; // 3300 mV
    const opensensor::Max17048 gauge{{&bus, GaugeBus::read}};
    opensensor::battery::Monitor monitor{kBatteryThresholds};
    opensensor::battery::Assessment last;
    BatteryHealth health{};
    for (int tick = 1; tick <= kBatteryThresholds.debounce_samples + 2;
         ++tick) {
      health = {true, gauge.read(), extended_ok(), {}};
      assert(health.reading.vcell_valid() && !health.reading.soc_valid());
      last = monitor.update(
          battery_sample(tick * 10000, health.reading, health.extended));
      health.assessment = last;
      const bool debounced = tick >= kBatteryThresholds.debounce_samples;
      assert(last.state == (debounced ? opensensor::battery::State::Critical
                                      : opensensor::battery::State::Unknown));
      assert(!last.soc_trusted && last.minutes_to_empty == -1);
    }
    Sample row{};
    gauge_modules(row, health);
    assert(row.data[gauge_status] == 1 && row.valid[battery_mv] &&
           row.data[battery_mv] == 3300 && !row.valid[battery_percent] &&
           !row.valid[battery_soc_raw_percent] &&
           !row.valid[battery_minutes_to_empty]);
    assert(row.valid[battery_health_state] &&
           row.data[battery_health_state] ==
               static_cast<std::int32_t>(opensensor::battery::State::Critical));
    assert(row.valid[battery_charge_rate_percent_per_h] &&
           row.valid[gauge_status_flags] && row.valid[gauge_hibernating]);
  }
  // A NACKed VCELL with a good SOC keeps the percent and no voltage.
  GaugeBus bus{0xa500, 0x4b80, true, false};
  const opensensor::Max17048 gauge{{&bus, GaugeBus::read}};
  Sample row{};
  board_modules::write_gauge(
      row, {.battery_installed = true, .reading = gauge.read()});
  assert(row.data[gauge_status] == 1 && !row.valid[battery_mv] &&
         row.valid[battery_percent] && row.data[battery_percent] == 75);
}
void assert_battery_gates() {
  assert(kBatteryThresholds.validate());
  // Mapping from the driver into the classifier input.
  auto ext = extended_ok();
  ext.status.raw = 0x4100;
  const auto reading = opensensor::Max17048::decode(0xc800, 0x4b80);
  const auto sample = battery_sample(1234, reading, ext);
  assert(sample.now_ms == 1234 && sample.vcell_ok && sample.soc_ok &&
         sample.millivolts == 4000 && sample.raw_soc == 0x4b80 &&
         sample.crate_ok && sample.crate_milli_percent_per_hour == -2080 &&
         sample.status_ok && sample.reset_indicator);
  // VCELL and SOC map on their own. A wild SOC keeps the voltage.
  const auto wild_soc =
      battery_sample(0, opensensor::Max17048::decode(0xd200, 0xffff), ext);
  assert(wild_soc.vcell_ok && wild_soc.millivolts == 4200 && !wild_soc.soc_ok);
  const auto wild_vcell =
      battery_sample(0, opensensor::Max17048::decode(0xffff, 0x4b80), ext);
  assert(!wild_vcell.vcell_ok && wild_vcell.soc_ok);
  assert(!battery_sample(0, opensensor::Max17048Reading{}, ext).vcell_ok);
  // Not installed leaves every new column null.
  Sample absent{};
  gauge_modules(absent, {});
  for (const auto field :
       {battery_soc_raw_percent, battery_charge_rate_percent_per_h,
        gauge_status_flags, gauge_hibernating, battery_health_state,
        battery_minutes_to_empty})
    assert(!absent.valid[field]);
  // Each register keeps its own column.
  BatteryHealth partial{true, reading, extended_ok(), {}};
  partial.extended.crate.ok = false;
  partial.extended.mode.ok = false;
  Sample row{};
  gauge_modules(row, partial);
  assert(row.valid[battery_soc_raw_percent] &&
         !row.valid[battery_charge_rate_percent_per_h] &&
         row.valid[gauge_status_flags] &&
         row.data[gauge_status_flags] == 0x40 &&
         !row.valid[gauge_hibernating] && row.valid[battery_health_state] &&
         row.data[battery_health_state] == 0 &&
         !row.valid[battery_minutes_to_empty]);
  // An uncapped raw SOC above 100% is still written as measured.
  Sample over{};
  gauge_modules(over,
                {true, opensensor::Max17048::decode(0xd5c0, 0x6bc9), {}, {}});
  float over_percent = 0;
  std::memcpy(&over_percent, &over.data[battery_soc_raw_percent],
              sizeof(over_percent));
  assert(over.valid[battery_soc_raw_percent] && over_percent > 107.7F &&
         over_percent < 107.8F);
  assert_voltage_only_critical();
  // A gauge reset at boot reports settling until the window ends.
  opensensor::battery::Monitor monitor{kBatteryThresholds};
  monitor.note_gauge_reset(0);
  opensensor::battery::Assessment last;
  for (int i = 1; i <= 3; ++i)
    last = monitor.update(battery_sample(i * 10000, reading, extended_ok()));
  assert(last.state == opensensor::battery::State::Settling &&
         !last.soc_trusted);
  last = monitor.update(battery_sample(3600000, reading, extended_ok()));
  assert(last.state == opensensor::battery::State::Ok &&
         last.minutes_to_empty == 2177);
}
// Warm-up runs from the last PMS wake, not from boot.
void assert_warmup_from_wake() {
  // Boot wake is 0, so the window matches the boot uptime gate.
  static_assert(pms_warming(0, 0) && pms_warming(kPmsWarmupUs - 1, 0));
  static_assert(!pms_warming(kPmsWarmupUs, 0));
  constexpr std::int64_t wake = 3600000000LL;
  static_assert(pms_warming(wake, wake) &&
                pms_warming(wake + kPmsWarmupUs - 1, wake));
  static_assert(!pms_warming(wake + kPmsWarmupUs, wake));
  // A wake stamped after the row time never reads as valid.
  static_assert(pms_warming(wake - 1, wake) && pms_warming(0, wake));
  // Large monotonic values near the end of the int64 range stay exact.
  constexpr auto late = std::numeric_limits<std::int64_t>::max() - 1;
  static_assert(pms_warming(late, late - kPmsWarmupUs + 1));
  static_assert(!pms_warming(late, late - kPmsWarmupUs));

  auto pms = parsed_snapshot();
  Sample boot{}, woke{}, warming{}, ready{}, early{};
  pms_modules(boot, pms, wake);
  assert(boot.data[pms_status] == kPmsValid && boot.valid[pm1_cf1_ug_m3]);
  pms.last_wake_mono_us = wake;
  pms_modules(woke, pms, wake);
  assert(woke.data[pms_status] == kPmsWarming && !woke.valid[pm1_cf1_ug_m3]);
  pms_modules(warming, pms, wake + kPmsWarmupUs - 1);
  assert(warming.data[pms_status] == kPmsWarming &&
         !warming.valid[ambient_temperature_c]);
  pms_modules(ready, pms, wake + kPmsWarmupUs);
  // After the window the row matches the boot case value for value.
  assert(ready.data == boot.data && ready.valid == boot.valid);
  pms_modules(early, pms, wake - 1);
  assert(early.data[pms_status] == kPmsWarming);
}
// Row equality fixture (#17 step 5). The module table and the pre-module
// collect in waveshare_collect_reference.h see the same board reads and
// must give byte identical rows. The fakes stand in for the UART snapshot,
// config::get and read_gauge.
PmsSnapshot fake_latest{};
config::Settings fake_settings{};
BatteryHealth fake_gauge{};
int gauge_reads = 0;
config::Settings read_fake_settings() { return fake_settings; }
BatteryHealth read_fake_gauge(std::int64_t) {
  ++gauge_reads;
  return fake_gauge;
}
constinit board_modules::Sources fake_sources{.latest = &fake_latest,
                                              .settings = read_fake_settings,
                                              .read_gauge = read_fake_gauge};
constexpr auto kFakeModules = board_modules::table(fake_sources);
static_assert(kFakeModules[0].procedure == "pms5003t" &&
              kFakeModules[1].procedure == "owner_provisioned_identity" &&
              kFakeModules[2].procedure == "max17048" &&
              kFakeModules[3].procedure == "battery_health");

template <std::size_t N> void copy_text(char (&out)[N], const char *text) {
  std::snprintf(out, N, "%s", text);
}
config::Settings settings_for(const SensorProfile &sensor) {
  config::Settings settings{};
  copy_text(settings.sensor_vendor, sensor.vendor);
  copy_text(settings.sensor_model, sensor.model);
  copy_text(settings.sensor_serial, sensor.serial);
  settings.sensor_batch_candidate = sensor.batch_candidate;
  return settings;
}
struct PmsCase {
  const char *name;
  PmsSnapshot latest;
  std::int64_t now;
};
std::vector<PmsCase> pms_cases() {
  constexpr std::int64_t now = 3 * kPmsWarmupUs;
  auto valid = parsed_snapshot();
  valid.received_mono_us = now - 250000;
  std::vector<PmsCase> cases{{"missing", PmsSnapshot{}, now},
                             {"valid", valid, now},
                             {"warming", valid, kPmsWarmupUs - 1}};
  cases[2].latest.received_mono_us = kPmsWarmupUs - 1 - 250000;
  auto edge = valid;
  edge.received_mono_us = now - 5000999; // age 5000 ms, still fresh
  cases.push_back({"stale edge", edge, now});
  auto stale = valid;
  stale.received_mono_us = now - 5001000;
  cases.push_back({"stale", stale, now});
  auto woke = valid;
  woke.last_wake_mono_us = now - 1000;
  cases.push_back({"warming after wake", woke, now});
  auto early = valid;
  early.last_wake_mono_us = now + 1;
  cases.push_back({"wake after row time", early, now});
  auto error = valid;
  error.frame.sensor_error = 9;
  cases.push_back({"sensor error", error, now});
  auto counts = valid;
  counts.frame.particle_count_fields = 6;
  cases.push_back({"model mismatch counts", counts, now});
  auto no_climate = valid;
  no_climate.frame.has_temperature_humidity = false;
  cases.push_back({"model mismatch climate", no_climate, now});
  auto wet = valid;
  wet.frame.humidity_deci_percent = 1001;
  cases.push_back({"rh over 1000", wet, now});
  auto full = valid;
  full.frame.humidity_deci_percent = 1000;
  full.frame.temperature_deci_c = 32767;
  full.frame.particle_counts = {65535, 0, 1, 2, 0, 0};
  cases.push_back({"rh 1000 and widest words", full, now});
  return cases;
}
struct GaugeCase {
  const char *name;
  BatteryHealth health;
};
std::vector<GaugeCase> gauge_cases() {
  const auto good = opensensor::Max17048::decode(0xc800, 0x4b80);
  auto soc_nack = good;
  soc_nack.soc_ok = false;
  auto vcell_nack = good;
  vcell_nack.vcell_ok = false;
  auto partial = extended_ok();
  partial.crate.ok = false;
  partial.mode.ok = false;
  opensensor::battery::Assessment critical{};
  critical.state = opensensor::battery::State::Critical;
  opensensor::battery::Assessment ok{};
  ok.state = opensensor::battery::State::Ok;
  ok.minutes_to_empty = 2177;
  auto empty = ok;
  empty.minutes_to_empty = 0;
  return {
      {"not installed", {}},
      {"not installed with stale read", {false, good, extended_ok(), ok}},
      {"both reads failed", {.battery_installed = true}},
      {"volts only", {true, soc_nack, extended_ok(), critical}},
      {"soc only", {true, vcell_nack, extended_ok(), {}}},
      {"wild soc",
       {true, opensensor::Max17048::decode(0xd200, 0xffff), {}, {}}},
      {"soc over 100",
       {true, opensensor::Max17048::decode(0xd5c0, 0x6bc9), extended_ok(), ok}},
      {"extended reads failed", {true, good, {}, ok}},
      {"partial extended", {true, good, partial, ok}},
      {"valid", {true, good, extended_ok(), ok}},
      {"zero minutes to empty", {true, good, extended_ok(), empty}},
  };
}
void assert_row_equality() {
  const SensorProfile profiles[] = {
      {},
      {"Plantower", "PMS5003T", "PMS5003T-202604081332", true},
      {"Plantower", "PMS5003T", "PMS5003T-202604081332", false},
      {"Plantower", "PMS5003T", "PMS5003T-2026040813327", true},
      {"Plantower", "PMS5003T", "PMS5003T-202605011332", true},
      {"Other", "PMS5003T", "PMS5003T-202604081332", true},
  };
  std::size_t compared = 0;
  for (const auto &pms : pms_cases())
    for (const auto &gauge : gauge_cases())
      for (const auto &sensor : profiles) {
        fake_latest = pms.latest;
        fake_settings = settings_for(sensor);
        fake_gauge = gauge.health;
        Sample expected{}, actual{};
        reference::collect(expected, pms.now, fake_latest, fake_settings,
                           gauge.health);
        const int reads = gauge_reads;
        board_modules::collect(kFakeModules, actual, pms.now);
        const bool same =
            std::memcmp(expected.data.data(), actual.data.data(),
                        sizeof(expected.data)) == 0 &&
            std::memcmp(expected.valid.data(), actual.valid.data(),
                        sizeof(expected.valid)) == 0;
        if (!same || gauge_reads != reads + 1) {
          std::fprintf(stderr, "row equality: %s, %s, serial \"%s\"\n",
                       pms.name, gauge.name, sensor.serial);
          std::abort();
        }
        ++compared;
      }
  assert(compared ==
         pms_cases().size() * gauge_cases().size() * std::size(profiles));
  // The fixture covers each status the brief names.
  Sample warming{}, missing{}, mismatch{}, failed{};
  fake_latest = pms_cases()[2].latest;
  fake_gauge = gauge_cases()[2].health;
  board_modules::collect(kFakeModules, warming, pms_cases()[2].now);
  assert(warming.data[pms_status] == kPmsWarming &&
         warming.data[gauge_status] == kGaugeReadError);
  fake_latest = pms_cases()[0].latest;
  board_modules::collect(kFakeModules, missing, pms_cases()[0].now);
  assert(missing.data[pms_status] == kPmsMissing && !missing.valid[pms_age_ms]);
  fake_latest = pms_cases()[8].latest;
  board_modules::collect(kFakeModules, mismatch, pms_cases()[8].now);
  assert(mismatch.data[pms_status] == kPmsModelMismatch);
  fake_gauge = gauge_cases()[0].health;
  board_modules::collect(kFakeModules, failed, pms_cases()[8].now);
  assert(failed.data[gauge_status] == kGaugeNotInstalled &&
         !failed.valid[battery_health_state]);
}
void assert_gates() {
  assert_identity_rules();
  assert_warmup_from_wake();
  auto pms = parsed_snapshot();
  Sample warm{}, ready{}, stale{}, bad_model{}, bad_gauge{}, installed{};
  pms_modules(warm, pms, kPmsWarmupUs - 1);
  assert(warm.data[pms_status] == kPmsWarming &&
         !warm.valid[ambient_temperature_c]);
  pms.age_ms = kPmsStaleAfterMs;
  pms_modules(ready, pms, kPmsWarmupUs);
  assert(ready.data[pms_status] == kPmsValid && ready.valid[pm1_cf1_ug_m3]);
  const SensorProfile exact{"Plantower", "PMS5003T", "PMS5003T-202604081332",
                            true};
  Sample candidate{};
  pms_modules(candidate, pms, kPmsWarmupUs, exact);
  assert(candidate.data[pm25_batch_candidate_status] == kBatchCandidate &&
         candidate.valid[pm25_batch_candidate_ug_m3] &&
         candidate.data[sensor_serial_code] == 202604081332LL &&
         candidate.data[pm25_atmospheric_ug_m3] ==
             ready.data[pm25_atmospheric_ug_m3]);
  Sample longer_serial{};
  pms_modules(longer_serial, pms, kPmsWarmupUs,
              {"Plantower", "PMS5003T", "PMS5003T-2026040813327", true});
  assert(longer_serial.data[pm25_batch_candidate_status] == kBatchCandidate &&
         longer_serial.data[sensor_serial_code] == 2026040813327LL);
  Sample other_batch{};
  pms_modules(other_batch, pms, kPmsWarmupUs,
              {"Plantower", "PMS5003T", "PMS5003T-202605011332", true});
  assert(other_batch.data[pm25_batch_candidate_status] ==
             kBatchIdentityMismatch &&
         !other_batch.valid[pm25_batch_candidate_ug_m3]);
  Sample wrong_vendor{};
  pms_modules(wrong_vendor, pms, kPmsWarmupUs,
              {"Other", "PMS5003T", "PMS5003T-202604081332", true});
  assert(wrong_vendor.data[pm25_batch_candidate_status] ==
             kBatchIdentityMismatch &&
         !wrong_vendor.valid[sensor_serial_code]);
  Sample unavailable{};
  pms_modules(unavailable, pms, kPmsWarmupUs - 1, exact);
  assert(unavailable.data[pm25_batch_candidate_status] ==
             kBatchSourceUnavailable &&
         !unavailable.valid[pm25_batch_candidate_ug_m3]);
  ++pms.age_ms;
  pms_modules(stale, pms, kPmsWarmupUs);
  assert(stale.data[pms_status] == kPmsStale && !stale.valid[pm1_cf1_ug_m3]);
  pms.age_ms = 250;
  plantower::Parser wrong{plantower::Model::Pmsa003};
  for (const auto byte : sensor_bytes())
    wrong.push(byte, pms.frame);
  pms_modules(bad_model, pms, kPmsWarmupUs);
  assert(bad_model.data[pms_status] == kPmsModelMismatch);
  assert(!bad_model.valid[ambient_temperature_c]);
  // Neither word read. The values stay out of the row.
  board_modules::write_gauge(bad_gauge,
                             {.battery_installed = true,
                              .reading = {.millivolts = 4200, .percent = 100}});
  assert(bad_gauge.data[gauge_status] == 1 && !bad_gauge.valid[battery_mv] &&
         !bad_gauge.valid[battery_percent]);
  const auto decoded = opensensor::Max17048::decode(0xd200, 0x6400);
  assert(decoded.vcell_valid() && decoded.soc_valid() &&
         decoded.millivolts == 4200 && decoded.percent == 100);
  board_modules::write_gauge(installed,
                             {.battery_installed = true, .reading = decoded});
  assert(installed.data[gauge_status] == 2 && installed.valid[battery_mv] &&
         installed.valid[battery_percent]);
  const auto no_volts = opensensor::Max17048::decode(0, 0x5000);
  assert(!no_volts.vcell_valid() && no_volts.soc_valid());
  const auto over_full = opensensor::Max17048::decode(0xd5c0, 0x6bc9);
  assert(over_full.vcell_valid() && over_full.soc_valid() &&
         over_full.millivolts == 4275 && over_full.percent == 100);
  const auto wild = opensensor::Max17048::decode(0xd200, 0xffff);
  assert(wild.vcell_valid() && !wild.soc_valid());
  Sample volts_only{};
  board_modules::write_gauge(volts_only,
                             {.battery_installed = true, .reading = wild});
  assert(volts_only.data[gauge_status] == 1 && volts_only.valid[battery_mv] &&
         volts_only.data[battery_mv] == 4200 &&
         !volts_only.valid[battery_percent]);
  Sample earlier{}, corrected{};
  apply_clock(earlier, 30000000, 15000000, 1788890000000000000LL, 1);
  const auto utc = earlier.data[event_time_utc_us];
  apply_clock(corrected, 40000000, 35000000, 1788889000000000000LL, 2);
  assert(corrected.data[clock_epoch] == 2);
  assert(corrected.data[event_time_utc_us] == 1788889005000000LL);
  assert(earlier.data[event_time_utc_us] == utc);
  Sample network{};
  apply_clock(network, 40000000, 35000000, 1788889000123456000LL, 3,
              kClockNetwork);
  assert(network.data[clock_status] == kClockNetwork);
  assert(network.valid[event_time_utc_us] &&
         network.data[event_time_utc_us] == 1788889005123456LL);
  assert(network.valid[clock_anchor_mono_us] &&
         network.valid[clock_anchor_utc_us]);
  Sample unsynced{};
  apply_clock(unsynced, 30000000, 0, 0, 0, kClockNetwork);
  assert(unsynced.data[clock_status] == kClockNone);
  assert(!unsynced.valid[event_time_utc_us]);
  assert_battery_gates();
  assert_row_equality();
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
  assert_live_keys(kFields, field_count);
  if (argc == 2 && template_mode(argv[1]))
    return 0;
  if (argc == 2 && std::strcmp(argv[1], "live") == 0) {
    livesample::print(kFields, field_count);
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
  if (argc != 4 && argc != 5)
    return 2;
  assert_gates();
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
  opensensor::battery::Monitor monitor{kBatteryThresholds};
  for (std::size_t i = 0; i < 90; ++i) {
    auto &row = rows[i];
    row.location = location;
    oslogger::apply_location(
        row, row.location,
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
    auto pms = parsed_snapshot();
    pms.received_mono_us = now - 250000;
    pms.present = i != 0;
    if (i == 2)
      pms.age_ms = 5001;
    if (i == 3)
      pms.frame.sensor_error = 9;
    if (i == 4)
      pms.frame.particle_count_fields = 6;
    if (i == 6)
      pms.frame.humidity_deci_percent = 1001;
    pms_modules(row, pms, now,
                {"Plantower", "PMS5003T", "PMS5003T-202604081332", i != 5});
    BatteryHealth health{}; // i == 0, explicit no-battery configuration
    if (i != 0) {
      health.battery_installed = true;
      if (i == 1) {
        // Both basic reads failed, a default reading has no ok flag.
      } else if (i == 2) {
        // VCELL read, SOC NACKed. Voltage is published without SOC.
        health.reading = opensensor::Max17048::decode(0xc800, 0x4b80);
        health.reading.soc_ok = false;
        health.extended = extended_ok();
      } else {
        health.reading = opensensor::Max17048::decode(0xc800, 0x4b80);
        health.extended = extended_ok();
      }
      health.assessment = monitor.update(
          battery_sample(now / 1000, health.reading, health.extended));
    }
    gauge_modules(row, health);
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
