// The shared runtime is board neutral. These fixtures borrow the Waveshare
// PMS5003T rules as a realistic board policy.
#include "../firmware/esp-idf-waveshare-sim7670g/logger/pms5003t_identity.h"
#include <debug_log.h>
#include <device_config.h>
#include <numeric_sample.h>
#include <nvs.h>
#include <os_gnss.h>
#include <os_location.h>
#include <os_logger_location.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr config::SensorPolicy kPms5003t{pms5003t::kVendor, pms5003t::kModel,
                                         pms5003t::valid_serial,
                                         pms5003t::batch_eligible};

// Pinned `cfg` blob contract. A layout change must bump the version.
constexpr std::size_t kHeaderBytes = 12;
constexpr std::size_t kPayloadBytes = 817;
constexpr std::size_t kPairAt = 0, kPinAt = 1, kDisplayAt = 5, kWifiAt = 6,
                      kLanAt = 7, kSsidAt = 8, kPskAt = 41, kTokenAt = 105,
                      kSensorAt = 137, kLocationAt = 217, kNtpAt = 257;

std::uint32_t crc32(const std::uint8_t *data, std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
std::uint32_t le32(const std::uint8_t *p) {
  return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 |
         std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
void put_le32(std::uint8_t *p, std::uint32_t value) {
  for (int i = 0; i < 4; ++i)
    p[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
std::vector<std::uint8_t> &cfg() { return fake_platform::nvs.blobs.at("cfg"); }
std::uint8_t *payload() { return cfg().data() + kHeaderBytes; }
char *field(std::size_t offset) {
  return reinterpret_cast<char *>(payload() + offset);
}
void set_field(std::size_t offset, std::size_t size, const char *text) {
  std::memset(field(offset), 0, size);
  std::memcpy(field(offset), text, std::strlen(text));
}
void reseal() {
  put_le32(cfg().data() + 8, crc32(payload(), cfg().size() - kHeaderBytes));
}
bool has_old_key() {
  for (const auto *key : {"pair", "pin", "disp", "wifion", "ssid", "psk",
                          "lanon", "token", "sensor", "location", "ntp"})
    if (fake_platform::nvs.types.count(key))
      return true;
  return false;
}
void check_blob_header() {
  const auto &blob = cfg();
  assert(fake_platform::nvs.types.at("cfg") == 9);
  assert(blob.size() == kHeaderBytes + kPayloadBytes);
  assert(std::memcmp(blob.data(), "AQCF", 4) == 0);
  assert(blob[4] == 1 && blob[5] == 0);
  assert(blob[6] == (kPayloadBytes & 0xff) && blob[7] == (kPayloadBytes >> 8));
  assert(le32(blob.data() + 8) ==
         crc32(blob.data() + kHeaderBytes, kPayloadBytes));
}
std::string log_text() {
  static char logs[DebugLog::kFallbackRingBytes + 1];
  std::uint32_t total = 0;
  const auto count = oslog.tail(logs, sizeof(logs) - 1, total);
  logs[count] = '\0';
  return logs;
}

// Pre-blob keys: every setting in its own typed key, as firmware before the
// `cfg` blob committed them. Since #111 they are never read.
void seed_old_keys(bool with_policy_records) {
  auto &nvs = fake_platform::nvs;
  nvs = {};
  const auto number = [&](const char *key, std::uint32_t value, unsigned type) {
    nvs.numbers[key] = value;
    nvs.types[key] = type;
  };
  const auto string = [&](const char *key, const char *value) {
    nvs.strings[key] = value;
    nvs.types[key] = 8;
  };
  number("pair", 1, 1);
  number("pin", 654321, 4);
  number("disp", 0, 1);
  number("wifion", 1, 1);
  string("ssid", "legacy-net");
  string("psk", "legacy-password");
  number("lanon", 0, 1);
  nvs.blobs["token"] = std::vector<std::uint8_t>(config::kTokenBytes, 0x5a);
  nvs.types["token"] = 9;
  string("sensor", "Plantower|PMS5003T|PMS5003T-202604081332|1");
  if (with_policy_records) {
    string("location", "|5|EG");
    string("ntp", "1|0|0|7200|ntp.local|");
  }
}
// First boot defaults. None of the seeded old key values may leak through.
void check_defaults(const config::Settings &settings, bool display) {
  assert(settings.display == display);
  assert(settings.pair ==
         (display ? config::PairMode::Random : config::PairMode::Fixed));
  assert(settings.pin < 1000000 && settings.pin != config::kFactoryDefaultPin &&
         settings.pin != 654321);
  assert(!settings.wifi_on && settings.lan_on && !settings.ssid[0] &&
         !settings.psk[0]);
  bool old_token = true;
  for (const auto byte : settings.token)
    old_token = old_token && byte == 0x5a;
  assert(!old_token);
  assert(!settings.sensor_serial[0] && !settings.sensor_batch_candidate);
  assert(settings.location.cell == 0 && !settings.location.country[0]);
  assert(settings.ntp.enabled && settings.ntp.dhcp &&
         settings.ntp.public_fallback && settings.ntp.interval_s != 7200);
}
// Old keys stay exactly as older firmware left them, with `cfg` beside them.
void check_old_keys_kept(const fake_platform::Values &old) {
  const auto &nvs = fake_platform::nvs;
  assert(nvs.numbers == old.numbers && nvs.strings == old.strings);
  for (const auto &entry : old.blobs)
    assert(nvs.blobs.at(entry.first) == entry.second);
  for (const auto &entry : old.types)
    assert(nvs.types.at(entry.first) == entry.second);
  assert(nvs.blobs.size() == old.blobs.size() + 1);
}
bool same(const config::Settings &a, const config::Settings &b) {
  return a.pair == b.pair && a.pin == b.pin && a.display == b.display &&
         a.wifi_on == b.wifi_on && std::strcmp(a.ssid, b.ssid) == 0 &&
         std::strcmp(a.psk, b.psk) == 0 && a.lan_on == b.lan_on &&
         std::strcmp(a.sensor_vendor, b.sensor_vendor) == 0 &&
         std::strcmp(a.sensor_model, b.sensor_model) == 0 &&
         std::strcmp(a.sensor_serial, b.sensor_serial) == 0 &&
         a.sensor_batch_candidate == b.sensor_batch_candidate &&
         config::same_location(a.location, b.location) &&
         a.location.phone_cell == b.location.phone_cell &&
         a.ntp.enabled == b.ntp.enabled && a.ntp.dhcp == b.ntp.dhcp &&
         a.ntp.public_fallback == b.ntp.public_fallback &&
         a.ntp.interval_s == b.ntp.interval_s &&
         std::strcmp(a.ntp.servers[0], b.ntp.servers[0]) == 0 &&
         std::strcmp(a.ntp.servers[1], b.ntp.servers[1]) == 0 &&
         std::memcmp(a.token, b.token, config::kTokenBytes) == 0;
}

bool apply(const std::string &lines, config::Actions &actions, char *bad_key) {
  return config::apply_lines(lines.data(), lines.size(), bad_key, 64, actions);
}
void accept(const std::string &lines) {
  config::Actions actions;
  char bad_key[64];
  assert(apply(lines, actions, bad_key));
  assert(bad_key[0] == '\0');
}
bool same_owner(const config::OwnerSettings &a,
                const config::OwnerSettings &b) {
  return std::strcmp(a.title.data(), b.title.data()) == 0 &&
         std::strcmp(a.description.data(), b.description.data()) == 0 &&
         std::strcmp(a.provider.data(), b.provider.data()) == 0 &&
         std::strcmp(a.url.data(), b.url.data()) == 0 &&
         config::same_deployment(a.deployment, b.deployment);
}
void reject(const std::string &lines, const char *expected_key) {
  const auto before = config::get();
  const auto before_owner = config::owner_settings();
  const auto writes = fake_platform::nvs.writes;
  config::Actions actions;
  actions.clear_bonds = true; // output actions must remain unchanged on failure
  char bad_key[64];
  assert(!apply(lines, actions, bad_key));
  if (expected_key)
    assert(std::strcmp(bad_key, expected_key) == 0);
  assert(same(before, config::get()));
  assert(same_owner(before_owner, config::owner_settings()));
  assert(actions.clear_bonds);
  assert(fake_platform::nvs.writes == writes);
}

void first_boot(bool display) {
  assert(config::load(display));
  const auto first = config::get();
  assert(first.display == display);
  assert(first.pair ==
         (display ? config::PairMode::Random : config::PairMode::Fixed));
  assert(first.pin < 1000000 && first.pin != config::kFactoryDefaultPin);
  assert(!first.wifi_on && first.lan_on && !first.ssid[0] && !first.psk[0]);
  assert(!config::reboot_required());
  // A fresh device seeds one CRC-protected record with one write and commit.
  check_blob_header();
  assert(le32(payload() + kPinAt) == first.pin);
  assert(std::memcmp(payload() + kTokenAt, first.token, config::kTokenBytes) ==
         0);
  assert(!has_old_key());
  assert(fake_platform::nvs.writes == 1 && fake_platform::nvs.commits == 1);
  assert(
      config::load(!display)); // stored capability/pairing defaults are frozen
  assert(same(first, config::get()));
  fake_platform::nvs = {}; // a second board's clean NVS gets another PIN/token
  assert(config::load(display));
  const auto second = config::get();
  assert(second.pin != first.pin);
  assert(std::memcmp(second.token, first.token, config::kTokenBytes) != 0);
  // A failed seed still boots with the new PIN and token in RAM. It marks
  // `cfg` unsure, so the next SET_CONFIG writes `cfg` even when it changes
  // only an owner key, and a reboot then keeps that PIN and token (#152).
  fake_platform::nvs = {};
  fake_platform::nvs.fail_write_key = "cfg";
  assert(config::load(display));
  fake_platform::nvs.fail_write_key.clear();
  assert(!fake_platform::nvs.blobs.count("cfg"));
  assert(log_text().find("CONFIG ERROR operation=nvs-seed-or-migrate") !=
         std::string::npos);
  const auto seeded = config::get();
  const auto seed_writes = fake_platform::nvs.writes;
  accept("owner.title=Seeded");
  // The owner record and the unsure `cfg`.
  assert(fake_platform::nvs.writes == seed_writes + 2);
  check_blob_header();
  assert(le32(payload() + kPinAt) == seeded.pin);
  assert(std::memcmp(payload() + kTokenAt, seeded.token, config::kTokenBytes) ==
         0);
  // `cfg` is sure again, so an equal request writes nothing.
  accept("owner.title=Seeded");
  assert(fake_platform::nvs.writes == seed_writes + 2);
  assert(config::load(display));
  assert(same(seeded, config::get()));
}

void migration() {
  assert(config::load(false));
  const auto original = config::get();
  put_le32(payload() + kPinAt, config::kFactoryDefaultPin);
  reseal();
  assert(config::load(false));
  const auto migrated = config::get();
  assert(migrated.pin != config::kFactoryDefaultPin && migrated.pin < 1000000);
  assert(migrated.pair == config::PairMode::Fixed);
  assert(std::memcmp(migrated.token, original.token, config::kTokenBytes) == 0);
  check_blob_header();
  assert(le32(payload() + kPinAt) == migrated.pin);
  assert(config::load(false));
  assert(same(migrated, config::get()));
  // An old per-key universal PIN is not read. The board gets a fresh random
  // PIN and the old key is left in place.
  seed_old_keys(true);
  fake_platform::nvs.numbers["pin"] = config::kFactoryDefaultPin;
  assert(config::load(false));
  const auto fresh = config::get();
  check_defaults(fresh, false);
  check_blob_header();
  assert(le32(payload() + kPinAt) == fresh.pin);
  assert(fake_platform::nvs.numbers.at("pin") == config::kFactoryDefaultPin);
  assert(config::load(false));
  assert(same(fresh, config::get()));
}

void validation() {
  assert(config::load(true));
  reject("wifi.on=1", "wifi.ssid");
  reject("wifi.ssid=accepted\nble.pin=wrong", "ble.pin");
  reject("lan.rotate_token=true\nunknown=1", "unknown");
  reject("ble.pair=fixed\nwifi.on=yes", "wifi.on");
  reject("ble.pin=01234", "ble.pin");
  reject("ble.pin=01234x", "ble.pin");
  reject("ble.pair=invalid", "ble.pair");
  reject("\r\n\n", "empty");
  reject(std::string(config::kNtpServerMax + 32, 'x'), "line-too-long");
  reject("missing-equals", "missing-equals");
  std::string nul = "wifi.ssid=visible";
  nul.push_back('\0');
  nul += "hidden";
  reject(nul, nullptr);
  config::Actions actions;
  char bad_key[64];
  assert(apply("ble.pair=fixed\r\nble.pin=012345\nble.clear_bonds=1\n"
               "lan.rotate_token=true\n",
               actions, bad_key));
  assert(actions.ble_changed && actions.clear_bonds && actions.rotate_token);
  assert(config::get().pin == 12345 && config::reboot_required());
  assert(config::load(true));
  assert(!config::reboot_required());
}

void credentials() {
  assert(config::load(false));
  reject("wifi.ssid=" + std::string(33, 's'), "wifi.ssid");
  reject("wifi.psk=" + std::string(7, 'p'), "wifi.psk");
  reject("wifi.psk=" + std::string(64, 'p'), "wifi.psk");
  reject("wifi.ssid=bad\tname", "wifi.ssid");
  reject(std::string("wifi.psk=bad") + char(127) + "password", "wifi.psk");
  accept("wifi.ssid=" + std::string(32, 's') +
         "\nwifi.psk=" + std::string(63, 'p'));
  accept("wifi.psk=\nwifi.on=true"); // an open network is explicit and valid
  accept("wifi.ssid=fixture\"\\name\nwifi.psk=fixture-only-password\nble.pin="
         "654321");
  const auto settings = config::get();
  config::WifiView wifi{"connected", "192.0.2.1",  -40, "00:00:00:00:00:01",
                        1,           "aq-fixture", 2};
  char json[512];
  const auto size = config::build_json(json, sizeof(json), wifi);
  assert(size > 0 && size == std::strlen(json));
  assert(!std::strstr(json, settings.psk));
  assert(!std::strstr(json, "654321"));
  assert(!std::strstr(json, "\"pin\":"));
  assert(!std::strstr(json, "\"psk\":"));
  assert(!std::strstr(json, "\"token\":"));
  char truncated[8];
  assert(config::build_json(truncated, sizeof(truncated), wifi) == 0);
  char logs[DebugLog::kFallbackRingBytes + 1];
  std::uint32_t total = 0;
  const auto count = oslog.tail(logs, sizeof(logs) - 1, total);
  logs[count] = '\0';
  assert(!std::strstr(logs, settings.psk) && !std::strstr(logs, "654321"));
  std::puts(json); // runner parses this synthetic, secret-free CONFIG document
}

bool any_serial(const char *) { return true; }
bool no_batch(const char *, const char *, const char *) { return false; }

// The runtime holds no sensor rule of its own. It defers to the board hook.
void board_policy_hooks() {
  accept("sensor.vendor=\nsensor.model=\nsensor.serial=");
  config::set_sensor_policy({"Vendor", "Model", nullptr, nullptr});
  accept("sensor.vendor=Vendor\nsensor.model=Model");
  reject("sensor.serial=ANY-1", "sensor.serial");
  reject("sensor.batch_candidate=1", "sensor.batch_candidate");
  config::set_sensor_policy({"Vendor", "Model", any_serial, no_batch});
  accept("sensor.serial=ANY-1");
  reject("sensor.batch_candidate=1", "sensor.batch_candidate");
  accept("sensor.vendor=\nsensor.model=\nsensor.serial=");
  config::set_sensor_policy({});
  reject("sensor.vendor=Plantower\nsensor.model=PMS5003T", "sensor.vendor");
  config::set_sensor_policy(kPms5003t);
}

void sensor_identity() {
  config::set_sensor_policy(kPms5003t);
  assert(config::load(false));
  reject("sensor.vendor=Other\nsensor.model=PMS5003T", "sensor.vendor");
  reject("sensor.vendor=Plantower\nsensor.model=PMSA003", "sensor.model");
  reject("sensor.serial=bad barcode", "sensor.serial");
  reject("sensor.vendor=Plantower\nsensor.model=PMS5003T\n"
         "sensor.serial=PMS5003T-202602291332",
         "sensor.serial");
  reject("sensor.vendor=Plantower\nsensor.model=PMS5003T\n"
         "sensor.serial=PMS5003T-202613011332",
         "sensor.serial");
  reject("sensor.vendor=Plantower\nsensor.model=PMS5003T\n"
         "sensor.serial=PMS5003T-20260408",
         "sensor.serial");
  reject("sensor.vendor=Plantower\nsensor.model=PMS5003T\n"
         "sensor.serial=PMS5003T-202605011332\nsensor.batch_candidate=1",
         "sensor.batch_candidate");
  accept("sensor.vendor=Plantower\nsensor.model=PMS5003T\n"
         "sensor.serial=PMS5003T-202604081332\nsensor.batch_candidate=1");
  const auto selected = config::get();
  assert(selected.sensor_batch_candidate &&
         std::strcmp(selected.sensor_serial, "PMS5003T-202604081332") == 0);
  assert(config::load(false) && same(selected, config::get()));
  config::WifiView wifi{"connected", "192.0.2.1",  -40, "00:00:00:00:00:01",
                        1,           "aq-fixture", 2};
  char json[config::kSensorSerialMax + 480]{};
  const auto length = config::build_json(json, 480, wifi);
  assert(length && length < 480);
  assert(std::strstr(json, "\"serial\":\"PMS5003T-202604081332\""));
  assert(std::strstr(json, "\"batch_candidate\":1"));
  accept("sensor.serial=PMS5003T-20260408123456789\nwifi.ssid=" +
         std::string(32, '\\'));
  config::WifiView widest{
      "connecting", "255.255.255.255",       -2147483647, "ff:ff:ff:ff:ff:ff",
      4294967295U,  "aq-123456789012345678", 4294967295U};
  assert(config::build_json(json, 480, widest) != 0);
  accept("sensor.serial=PMS5003T-202605011332");
  assert(!config::get().sensor_batch_candidate);
  reject("sensor.batch_candidate=1", "sensor.batch_candidate");
  accept("sensor.serial=PMS5003T-2026040813327\nsensor.batch_candidate=1");
  assert(config::get().sensor_batch_candidate);
  accept("sensor.batch_candidate=0");
  accept("sensor.serial=PMS5003T-202402291");
  accept("sensor.serial=");
  assert(!config::get().sensor_serial[0]);
  fake_platform::nvs.fail_write_key = "cfg";
  const auto before = config::get();
  config::Actions actions;
  char bad_key[64];
  assert(!apply("sensor.serial=PMS5003T-202604081332", actions, bad_key));
  assert(std::strcmp(bad_key, "nvs") == 0 && same(before, config::get()));
  fake_platform::nvs.fail_write_key.clear();
  board_policy_hooks();
}

void persistence_failure() {
  assert(config::load(false));
  accept("wifi.ssid=fixture-network\nwifi.psk=fixture-password");
  const auto before = config::get();
  const auto stored = cfg();
  // Every setting shares one record, so one failed blob write or commit
  // rejects the whole save and the previous record stays readable.
  for (const auto *change :
       {"wifi.ssid=", "wifi.psk=", "ble.pin=000001", "lan.rotate_token=1"}) {
    for (const bool commit_fails : {false, true}) {
      (commit_fails ? fake_platform::nvs.fail_commit_key
                    : fake_platform::nvs.fail_write_key) = "cfg";
      const auto writes = fake_platform::nvs.writes;
      config::Actions actions;
      char bad_key[64];
      assert(!apply(change, actions, bad_key));
      assert(std::strcmp(bad_key, "nvs") == 0);
      assert(same(before, config::get()));
      // The save and the write back of the previous record.
      assert(fake_platform::nvs.writes == writes + 2);
      assert(cfg() == stored);
      fake_platform::nvs.fail_write_key.clear();
      fake_platform::nvs.fail_commit_key.clear();
    }
  }
  assert(config::load(false));
  assert(same(before, config::get()));
  // The loop above covers a failed lan.rotate_token=1, this one saves.
  accept("lan.rotate_token=1");
  assert(std::memcmp(before.token, config::get().token, config::kTokenBytes) !=
         0);
  fake_platform::nvs.fail_open = true;
  const auto rotated = config::get();
  config::Actions actions;
  char bad_key[64];
  assert(!apply("lan.on=false", actions, bad_key));
  assert(std::strcmp(bad_key, "nvs") == 0);
  assert(same(rotated, config::get()));
}

// Bytes logged since boot, a mark for log_count().
std::uint32_t log_mark() {
  char unused[1];
  std::uint32_t total = 0;
  oslog.tail(unused, 0, total);
  return total;
}

// Counts `needle` in what was logged after `mark`. The ring must still hold
// all of it.
std::size_t log_count(std::uint32_t mark, const std::string &needle) {
  static char ring[DebugLog::kFallbackRingBytes + 1];
  std::uint32_t total = 0;
  const auto copied = oslog.tail(ring, sizeof(ring) - 1, total);
  assert(total >= mark && total - mark <= copied);
  const std::string logs(ring + copied - (total - mark), total - mark);
  std::size_t count = 0;
  for (auto at = logs.find(needle); at != std::string::npos;
       at = logs.find(needle, at + needle.size()))
    ++count;
  return count;
}

// A stored record from an image that accepted other resolutions (#139). The
// record keeps its layout and stays in flash. Boot reads it as res 8 with one
// line, and the next row carries h3_resolution 8.
void stored_resolution_reset() {
  constexpr std::uint64_t kAt8 = 0x8828308281fffffULL;
  std::uint32_t mark = 0;
  const auto boot = [&mark](const char *record) {
    set_field(kLocationAt, 40, record);
    reseal();
    const auto stored = cfg();
    const auto writes = fake_platform::nvs.writes;
    mark = log_mark();
    assert(config::load(false));
    // Boot writes nothing back, the old text stays until the next save.
    assert(cfg() == stored && fake_platform::nvs.writes == writes);
    return config::location_settings();
  };
  const auto logged = [&mark](const char *needle) {
    return log_count(mark, needle);
  };
  auto location = boot("8828308281fffff|7|EG|fixed");
  assert(location.cell == kAt8 && location.resolution == 8);
  assert(std::strcmp(location.country, "EG") == 0);
  assert(logged("CONFIG location.resolution=7 reset to 8\n") == 1);
  assert(logged("CONFIG location.resolution=") == 1);
  assert(logged("invalid-stored-profile") == 0);
  // The next row of a fixed Thing has the res 8 cell and h3_resolution 8.
  oslogger::LocationSample<opensensor::NumericSample<4>> row{};
  row.location = config::sample_labels().location;
  const auto chosen =
      opensensor::gnss::select_location(row.location, std::nullopt, 1);
  assert(chosen.source == telemetry::LocationSource::Configured);
  assert(oslogger::apply_cell(row, chosen.cell, chosen.source, {0, 1, 2, 3}));
  assert(row.valid[0] && row.data[0] == static_cast<std::int64_t>(kAt8));
  assert(row.valid[1] && row.data[1] == 8);
  // The next save writes res 8 into the same record, and boot is quiet.
  accept("location.country=EG");
  assert(std::string(field(kLocationAt)) == "8828308281fffff|8|EG|fixed");
  mark = log_mark();
  assert(config::load(false));
  assert(logged("CONFIG location.resolution=") == 0);
  // A finer stored cell is published as its res 8 parent.
  location = boot("8928308280fffff|9|EG|moving");
  assert(location.cell == kAt8 && location.resolution == 8 &&
         location.mobility == config::Mobility::Moving);
  assert(logged("CONFIG location.resolution=9 reset to 8\n") == 1);
  location = boot("8928308280fffff|15|");
  assert(location.cell == kAt8 && location.resolution == 8);
  assert(logged("CONFIG location.resolution=15 reset to 8\n") == 1);
  location = boot("|0||fixed");
  assert(location.cell == 0 && location.resolution == 8);
  assert(logged("CONFIG location.resolution=0 reset to 8\n") == 1 &&
         logged("invalid-stored-profile") == 0);
  // A coarser stored cell cannot be restored, so the location is unset. The
  // refused record logs only the invalid line, never the reset line too.
  location = boot("85283473fffffff|5|EG|fixed");
  assert(location.cell == 0 && location.resolution == 8 &&
         !location.country[0]);
  assert(logged("CONFIG location.resolution=") == 0 &&
         logged("CONFIG LOCATION invalid-stored-profile=true") == 1);
  location = boot("85283473fffffff|8|EG|fixed");
  assert(location.cell == 0 && !location.country[0]);
  assert(logged("CONFIG location.resolution=") == 0 &&
         logged("CONFIG LOCATION invalid-stored-profile=true") == 1);
  // A malformed record prints only the invalid line.
  location = boot("8828308281fffff|16|EG|fixed");
  assert(location.cell == 0);
  assert(logged("CONFIG location.resolution=") == 0 &&
         logged("CONFIG LOCATION invalid-stored-profile=true") == 1);
}

void time_and_location() {
  assert(config::load(false));
  assert(config::get().ntp.enabled && config::get().ntp.dhcp &&
         config::get().ntp.public_fallback);
  assert(config::get().location.cell == 0 &&
         config::get().location.resolution == 8 &&
         config::get().location.mobility == config::Mobility::Fixed);
  char json[480];
  assert(config::build_page(json, sizeof(json), 1));
  assert(std::strstr(json, "\"lat\":null"));
  assert(std::strstr(json, "\"thing\":{\"mobility\":\"fixed\",\"setup\":1}"));
  reject("thing.mobility=walking", "thing.mobility");
  reject("thing.mobility=", "thing.mobility");
  reject("thing.mobility=Fixed", "thing.mobility");
  reject("location.cell=fffffffffffffff", "location.cell");
  // Res 8 is the only published resolution (#139). Every other value, from
  // BLE, LAN or the provisioning file, is refused as ERROR 12 with this key.
  for (const char *value :
       {"0", "5", "7", "9", "15", "16", "", "8 ", "-8", "4294967304"})
    reject(std::string("location.resolution=") + value, "location.resolution");
  // parse_uint takes leading zeros for every numeric key, so 08 is 8.
  accept("location.resolution=08");
  // A cell coarser than res 8 cannot be restored to res 8, so it is refused.
  reject("location.cell=85283473fffffff", "location.cell");
  accept("location.resolution=8");
  assert(config::get().location.resolution == 8);
  reject("location.country=ZZ", "location.country");
  reject("location.country=eg", "location.country");
  reject("ntp.interval_s=59", "ntp.interval_s");
  reject("ntp.interval_s=42949672960", "ntp.interval_s");
  reject("ntp.server1=https://time.cloudflare.com", "ntp.server1");
  reject("ntp.server1=host|injection", "ntp.server1");
  reject("ntp.server1=host:123", "ntp.server1");
  reject("ntp.server1=0.0.0.0", "ntp.server1");
  reject("ntp.server1=" + std::string(64, 'a'), "ntp.server1");
  accept("ntp.server2=2001:db8::1");
  reject("ntp.server1=" + std::string(254, 'a'), "ntp.server1");
  config::Actions actions;
  char bad_key[64];
  assert(apply("location.cell=8928308280fffff\nlocation.resolution=8\nlocation."
               "country=EG",
               actions, bad_key));
  assert(actions.location_changed && !actions.ntp_changed &&
         !actions.wifi_changed);
  // The res 9 cell is published as its res 8 parent.
  const auto coarse = config::get().location.cell;
  assert(coarse == 0x8828308281fffffULL);
  auto country_change = config::get().location;
  country_change.country[0] = 'U';
  country_change.country[1] = 'S';
  assert(!config::same_location(country_change, config::get().location));
  // A mobility change splits the open file like any other location change.
  auto moving = config::get().location;
  moving.mobility = config::Mobility::Moving;
  assert(!config::same_location(moving, config::get().location));
  assert(apply("thing.mobility=moving", actions, bad_key));
  assert(actions.location_changed);
  assert(config::get().location.mobility == config::Mobility::Moving);
  assert(std::string(field(kLocationAt)).ends_with("|EG|moving"));
  assert(config::build_page(json, sizeof(json), 1));
  assert(std::strstr(json, "\"mobility\":\"moving\""));
  assert(config::load(false));
  assert(config::get().location.mobility == config::Mobility::Moving);
  accept("thing.mobility=fixed");
  assert(config::get().location.mobility == config::Mobility::Fixed);
  assert(opensensor::location::resolution(coarse) == 8 &&
         coarse != 0x8928308280fffffULL);
  assert(std::string(field(kLocationAt)).find("8928308280fffff") ==
         std::string::npos);
  assert(config::build_page(json, sizeof(json), 1));
  assert(std::strstr(json, "\"country\":\"EG\""));
  assert(config::load(false));
  assert(config::get().location.cell == coarse);
  // A refused resolution leaves the whole request unapplied, whatever the
  // patch order.
  reject("location.resolution=9\nlocation.cell=8928308280fffff",
         "location.resolution");
  reject("location.cell=8928308280fffff\nlocation.resolution=5",
         "location.resolution");
  assert(config::get().location.cell == coarse);
  assert(apply("ntp.dhcp=0\nntp.public_fallback=0\nntp.server1=ntp.local\nntp."
               "server2=192.168.1.1\nntp.interval_s=7200",
               actions, bad_key));
  assert(actions.ntp_changed && !actions.wifi_changed);
  assert(config::load(false));
  const auto ntp = config::get().ntp;
  assert(!ntp.dhcp && !ntp.public_fallback && ntp.interval_s == 7200);
  assert(std::strcmp(ntp.servers[0], "ntp.local") == 0);
  assert(config::build_page(json, sizeof(json), 2));
  assert(config::build_page(json, sizeof(json), 3));
  assert(std::strstr(json, "ntp.local"));
  // Maximum supported DNS name remains readable within the wire JSON limit.
  accept("ntp.server1=" + std::string(63, 'a') + "." + std::string(63, 'b') +
         "." + std::string(63, 'c') + "." + std::string(61, 'd'));
  assert(config::build_page(json, sizeof(json), 3));
  assert(config::load(false));
  assert(std::strlen(config::get().ntp.servers[0]) == 253);
  assert(!config::build_page(json, 8, 1) && json[0] == '\0');
  assert(!config::build_page(json, sizeof(json), config::kLastConfigPage + 1));
  const auto before_failure = config::get();
  fake_platform::nvs.fail_write_key = "cfg";
  assert(!apply("location.country=US", actions, bad_key));
  assert(std::strcmp(bad_key, "nvs") == 0);
  assert(same(before_failure, config::get()));
  fake_platform::nvs.fail_write_key.clear();
  fake_platform::nvs.fail_commit_key = "cfg";
  assert(!apply("ntp.public_fallback=1", actions, bad_key));
  assert(std::strcmp(bad_key, "nvs") == 0);
  assert(same(before_failure, config::get()));
  fake_platform::nvs.fail_commit_key.clear();
  assert(config::load(false));
  assert(same(before_failure, config::get()));
  // A sealed record still passes the same validation as a SET_CONFIG value.
  // A record from before #83 has no mobility and reads as a fixed Thing.
  accept("thing.mobility=moving");
  set_field(kLocationAt, 40, "|8|EG");
  reseal();
  assert(config::load(false));
  assert(config::get().location.resolution == 8 &&
         std::strcmp(config::get().location.country, "EG") == 0 &&
         config::get().location.mobility == config::Mobility::Fixed);
  stored_resolution_reset();
  set_field(kLocationAt, 40, "|7|EG|walking");
  reseal();
  assert(config::load(false));
  assert(config::get().location.resolution == 8 &&
         !config::get().location.country[0]);
  set_field(kLocationAt, 40, "broken|15|EG");
  set_field(kNtpAt, 560, "broken");
  reseal();
  assert(config::load(false));
  assert(config::get().location.cell == 0);
  assert(!config::get().ntp.enabled && !config::get().ntp.public_fallback);
  accept("location.cell=\nlocation.country=");
  assert(config::get().location.cell == 0);
}

// location.phone_cell (#140) is RAM only. A request that holds only it writes
// no NVS record, a mixed request stores the other keys without it, and a
// reboot clears it. Coarse and invalid cells answer ERROR 12 with the key.
void phone_cell() {
  constexpr std::uint64_t kAt8 = 0x8828308281fffffULL;
  constexpr std::uint64_t kOther = 0x88283082a3fffffULL;
  static_assert(config::effective_cell({.cell = kAt8}) == kAt8);
  static_assert(config::effective_cell({.cell = kAt8, .phone_cell = kOther}) ==
                kOther);
  static_assert(config::effective_cell({.phone_cell = kOther}) == kOther);
  fake_platform::nvs = {};
  assert(config::load(false));
  accept("location.cell=8828308281fffff\nlocation.country=EG");
  const auto stored = cfg();
  const std::string stored_location = field(kLocationAt);
  const auto writes = fake_platform::nvs.writes;
  const auto commits = fake_platform::nvs.commits;
  const auto unchanged_flash = [&] {
    assert(cfg() == stored && fake_platform::nvs.writes == writes &&
           fake_platform::nvs.commits == commits);
  };
  char json[480];
  const auto page_has = [&json](const char *needle) {
    assert(config::build_page(json, sizeof(json), 1));
    return std::strstr(json, needle) != nullptr;
  };
  assert(page_has("\"country\":\"EG\",\"phone_cell\":\"\"},"));
  // Set. A res 9 cell is kept as its res 8 parent and written nowhere.
  config::Actions actions;
  char bad_key[64];
  assert(apply("location.phone_cell=8928308280fffff", actions, bad_key));
  assert(bad_key[0] == '\0' && actions.location_changed);
  assert(config::location_settings().phone_cell == kAt8);
  assert(config::get().location.cell == kAt8);
  unchanged_flash();
  assert(page_has("\"phone_cell\":\"8828308281fffff\""));
  // Update, uppercase hex too, as location.cell takes.
  accept("location.phone_cell=88283082A3FFFFF");
  assert(config::location_settings().phone_cell == kOther);
  unchanged_flash();
  assert(page_has("\"phone_cell\":\"88283082a3fffff\""));
  // Coarser than res 8, not a cell, or malformed. Nothing changes.
  for (const char *value :
       {"85283473fffffff", "fffffffffffffff", "8828308281ffff",
        "8828308281fffffa", "8828308281fffg f", " 8828308281fffff", "0", "1"})
    reject(std::string("location.phone_cell=") + value, "location.phone_cell");
  assert(config::location_settings().phone_cell == kOther);
  // A refused key elsewhere in the request keeps the old phone cell.
  reject("location.phone_cell=8828308281fffff\nlocation.country=ZZ",
         "location.country");
  assert(config::location_settings().phone_cell == kOther);
  unchanged_flash();
  // A mixed request stores the other keys and never the phone cell.
  accept("location.phone_cell=8828308281fffff\nlocation.country=US");
  assert(config::location_settings().phone_cell == kAt8);
  assert(fake_platform::nvs.writes == writes + 1);
  assert(std::string(field(kLocationAt)) == "8828308281fffff|8|US|fixed");
  // A failed save refuses the whole request, phone cell included.
  fake_platform::nvs.fail_write_key = "cfg";
  assert(!apply("location.phone_cell=88283082a3fffff\nlocation.country=EG",
                actions, bad_key));
  fake_platform::nvs.fail_write_key.clear();
  assert(std::strcmp(bad_key, "nvs") == 0);
  assert(config::location_settings().phone_cell == kAt8);
  // A phone cell only request still writes the record a failed write back
  // left unsure (#151), and that record never holds the phone cell.
  fake_platform::nvs.corrupt_read_key = "cfg";
  assert(!apply("location.country=EG", actions, bad_key));
  fake_platform::nvs.corrupt_read_key.clear();
  assert(std::strcmp(bad_key, "nvs") == 0);
  const auto unsure_writes = fake_platform::nvs.writes;
  accept("location.phone_cell=88283082a3fffff");
  assert(fake_platform::nvs.writes == unsure_writes + 1);
  assert(std::string(field(kLocationAt)) == "8828308281fffff|8|US|fixed");
  // Clear.
  const auto clear_writes = fake_platform::nvs.writes;
  accept("location.phone_cell=");
  assert(config::location_settings().phone_cell == 0);
  assert(fake_platform::nvs.writes == clear_writes);
  assert(page_has("\"phone_cell\":\"\""));
  // A reboot clears it. Nothing in flash brings it back.
  accept("location.phone_cell=88283082a3fffff");
  assert(config::load(false));
  assert(config::location_settings().phone_cell == 0);
  assert(config::location_settings().cell == kAt8);
  assert(page_has("\"phone_cell\":\"\""));
  // The location record has no room for it and holds four fields only.
  assert(std::string(field(kLocationAt)).find("88283082a3fffff") ==
         std::string::npos);
}

// #147. "Setup needed" is derived from the location settings, never stored.
// Page 1 carries it as thing.setup and, on a board with a receiver, a fresh
// res 8 GNSS cell as location.gnss_cell.
void setup_state() {
  using config::Mobility;
  constexpr std::uint64_t kCell = 0x8828308281fffffULL; // res 8
  static_assert(config::setup_needed({}));
  static_assert(!config::setup_needed({.cell = kCell}));
  static_assert(!config::setup_needed({.mobility = Mobility::Moving}));
  static_assert(
      !config::setup_needed({.cell = kCell, .mobility = Mobility::Moving}));
  assert(config::load(false));
  assert(config::setup_needed(config::location_settings()));
  char json[480];
  const auto page = [&](const config::PageInputs &inputs = {}) {
    const auto length = config::build_page(json, sizeof(json), 1, inputs);
    assert(length && length == std::strlen(json));
    return length;
  };
  page();
  assert(std::strstr(json, "\"thing\":{\"mobility\":\"fixed\",\"setup\":1}}"));
  // A board without a receiver omits the key.
  assert(!std::strstr(json, "gnss_cell"));
  page({.gnss = true});
  assert(std::strstr(json, "\"gnss_cell\":\"\""));
  page({.gnss = true, .gnss_cell = kCell});
  assert(std::strstr(json, "\"gnss_cell\":\"8828308281fffff\""));
  // An invalid cell is never printed.
  page({.gnss = true, .gnss_cell = 1});
  assert(std::strstr(json, "\"gnss_cell\":\"\""));
  // Setting a cell clears it, clearing the cell raises it again.
  accept("location.cell=8828308281fffff");
  assert(!config::setup_needed(config::location_settings()));
  page();
  assert(std::strstr(json, "\"setup\":0}"));
  accept("location.cell=");
  page();
  assert(std::strstr(json, "\"setup\":1}"));
  // A moving Thing never needs setup, with or without a cell.
  accept("thing.mobility=moving");
  page();
  assert(std::strstr(json, "\"thing\":{\"mobility\":\"moving\",\"setup\":0}}"));
  // Worst case. Every field at its widest, the fixed res 8 cap and cell
  // resolution (#139), an 11 and a 12 character centre, a country, "moving",
  // a phone cell (#140) and a GNSS cell. The frame header takes 2 of the 482
  // byte budget.
  opensensor::location::Cell finest = 0, gnss = 0;
  assert(opensensor::location::from_degrees(-45.1234567, -170.1234567, 15,
                                            finest));
  assert(
      opensensor::location::from_degrees(-45.1234567, -170.1234567, 8, gnss));
  char text[opensensor::location::kCellTextBytes];
  assert(opensensor::location::format_cell(finest, text, sizeof(text)));
  accept(std::string("location.resolution=8\nlocation.country=EG\n"
                     "location.cell=") +
         text + "\nlocation.phone_cell=" + text);
  const auto worst = page({.gnss = true, .gnss_cell = gnss});
  assert(std::strstr(json, "\"resolution\":8,\"cell_resolution\":8,"
                           "\"lat\":-45.") &&
         std::strstr(json, ",\"lon\":-170."));
  assert(std::strstr(json, "\"phone_cell\":\"") &&
         !std::strstr(json, "\"phone_cell\":\"\""));
  assert(std::strlen(text) == 15 && worst < 480);
  std::printf("%zu\n", worst);
}

void blob_round_trip() {
  config::set_sensor_policy(kPms5003t);
  assert(config::load(false));
  const auto writes = fake_platform::nvs.writes;
  const auto commits = fake_platform::nvs.commits;
  accept("wifi.ssid=round-trip\nwifi.psk=round-trip-password\nwifi.on=1\n"
         "ble.pair=none\nble.pin=024680\nlan.on=0\n"
         "sensor.vendor=Plantower\nsensor.model=PMS5003T\n"
         "sensor.serial=PMS5003T-202604081332\nsensor.batch_candidate=1\n"
         "location.cell=8928308280fffff\nlocation.resolution=8\n"
         "location.country=EG\nntp.dhcp=0\nntp.server1=ntp.local\n"
         "ntp.server2=192.168.1.1\nntp.interval_s=900");
  // One complete SET_CONFIG is one blob write and one commit.
  assert(fake_platform::nvs.writes == writes + 1);
  assert(fake_platform::nvs.commits == commits + 1);
  check_blob_header();
  assert(!has_old_key());
  const auto saved = config::get();
  assert(std::strcmp(field(kPskAt), "round-trip-password") == 0);
  assert(payload()[kPairAt] == 2 && payload()[kWifiAt] == 1 &&
         payload()[kLanAt] == 0 && payload()[kDisplayAt] == 0);
  assert(config::load(false));
  assert(same(saved, config::get()));
  assert(config::get().pair == config::PairMode::None &&
         config::get().pin == 24680);
  assert(config::get().location.cell == 0x8828308281fffffULL &&
         config::get().location.resolution == 8);
  assert(config::get().ntp.interval_s == 900 && !config::get().ntp.dhcp);
  const auto logs = log_text();
  assert(logs.find("round-trip-password") == std::string::npos);
  assert(logs.find("024680") == std::string::npos);
  assert(logs.find("CONFIG BLOB") == std::string::npos);
}

// No backward compatibility (#111). Pre-blob keys without a `cfg` record are
// a first boot. Defaults are seeded and the old keys are neither read nor
// erased.
void old_keys_ignored() {
  config::set_sensor_policy(kPms5003t);
  for (const bool records : {true, false}) {
    for (const bool display : {true, false}) {
      seed_old_keys(records);
      const fake_platform::Values old = fake_platform::nvs;
      assert(config::load(display));
      const auto seeded = config::get();
      check_defaults(seeded, display);
      // One seed write and one commit, the same as a clean first boot.
      assert(fake_platform::nvs.writes == 1 && fake_platform::nvs.commits == 1);
      check_blob_header();
      check_old_keys_kept(old);
      assert(log_text().find("CONFIG BLOB") == std::string::npos);
      assert(config::load(!display));
      assert(same(seeded, config::get()));
      accept("wifi.on=0");
      check_old_keys_kept(old);
    }
  }
}

void torn_blob() {
  config::set_sensor_policy(kPms5003t);
  seed_old_keys(true);
  assert(config::load(false));
  accept("wifi.ssid=blob-net\nwifi.psk=blob-password");
  const auto good = cfg();
  const auto blob_settings = config::get();
  using Damage = void (*)(std::vector<std::uint8_t> &);
  const Damage damages[] = {
      [](std::vector<std::uint8_t> &b) { b[kHeaderBytes + kSsidAt] ^= 1; },
      [](std::vector<std::uint8_t> &b) { b[8] ^= 0x80; },
      [](std::vector<std::uint8_t> &b) { b.pop_back(); },
      [](std::vector<std::uint8_t> &b) { b.resize(kHeaderBytes); },
      [](std::vector<std::uint8_t> &b) { b.resize(3); },
      [](std::vector<std::uint8_t> &b) { b.push_back(0); },
      [](std::vector<std::uint8_t> &b) { b[0] = 'X'; },
      [](std::vector<std::uint8_t> &b) { b[4] = 2; },
      [](std::vector<std::uint8_t> &b) { b[6] = 0x30; },
      [](std::vector<std::uint8_t> &b) {
        // A sealed record whose fixed text field lost its terminator.
        std::memset(b.data() + kHeaderBytes + kSsidAt, 's',
                    config::kSsidMax + 1);
      },
  };
  unsigned index = 0;
  for (const auto damage : damages) {
    cfg() = good;
    damage(cfg());
    if (index == 9) // reseal so the terminator check is reached on its own
      reseal();
    const auto writes = fake_platform::nvs.writes;
    assert(config::load(false));
    // A damaged record is a first boot. Old keys are not a fallback.
    check_defaults(config::get(), false);
    assert(fake_platform::nvs.writes == writes + 1); // one reseed write
    check_blob_header();
    assert(log_text().find("CONFIG BLOB invalid=true fallback=defaults") !=
           std::string::npos);
    cfg() = good;
    assert(config::load(false));
    assert(same(blob_settings, config::get()));
    ++index;
  }
  // A type mismatch on `cfg` is also a torn record.
  fake_platform::nvs.blobs.erase("cfg");
  fake_platform::nvs.strings["cfg"] = "not-a-blob";
  fake_platform::nvs.types["cfg"] = 8;
  assert(config::load(false));
  check_defaults(config::get(), false);
  // A damaged record with no old keys seeds a first-boot profile again.
  fake_platform::nvs = {};
  assert(config::load(false));
  cfg()[kHeaderBytes] ^= 1;
  assert(config::load(false));
  assert(config::get().pair == config::PairMode::Fixed);
  check_blob_header();
  assert(!has_old_key());
  // Invalid sensor text inside a sealed record clears only the sensor profile.
  cfg() = good;
  set_field(kSensorAt, 80, "Other|PMS5003T|PMS5003T-202604081332|1");
  reseal();
  assert(config::load(false));
  assert(!config::get().sensor_vendor[0] &&
         !config::get().sensor_batch_candidate);
  assert(std::strcmp(config::get().ssid, "blob-net") == 0);
}

// Owner site configuration (#148). Pinned `owner` record contract, separate
// from `cfg`. A layout change must bump the version.
constexpr std::size_t kOwnerPayloadBytes = 421;
constexpr std::size_t kTitleAt = 0, kDescriptionAt = 65, kProviderAt = 226,
                      kUrlAt = 291, kDeploymentAt = 388;
std::vector<std::uint8_t> &owner_blob() {
  return fake_platform::nvs.blobs.at("owner");
}
char *owner_field(std::size_t offset) {
  return reinterpret_cast<char *>(owner_blob().data() + kHeaderBytes + offset);
}
void reseal_owner() {
  put_le32(owner_blob().data() + 8,
           crc32(owner_blob().data() + kHeaderBytes, kOwnerPayloadBytes));
}
void check_owner_header() {
  const auto &blob = owner_blob();
  assert(fake_platform::nvs.types.at("owner") == 9);
  assert(blob.size() == kHeaderBytes + kOwnerPayloadBytes);
  assert(std::memcmp(blob.data(), "OSOW", 4) == 0);
  assert(blob[4] == 1 && blob[5] == 0);
  assert(blob[6] == (kOwnerPayloadBytes & 0xff) &&
         blob[7] == (kOwnerPayloadBytes >> 8));
  assert(le32(blob.data() + 8) ==
         crc32(blob.data() + kHeaderBytes, kOwnerPayloadBytes));
}
bool owner_empty() {
  const auto owner = config::owner_settings();
  return !owner.title[0] && !owner.description[0] && !owner.provider[0] &&
         !owner.url[0] && !owner.deployment.text[0] &&
         !config::deployment_id().text[0];
}
std::size_t count(const std::string &text, const std::string &needle) {
  std::size_t found = 0;
  for (auto at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + 1))
    ++found;
  return found;
}
std::string repeat(const std::string &unit, std::size_t times) {
  std::string out;
  for (std::size_t i = 0; i < times; ++i)
    out += unit;
  return out;
}

void owner_configuration() {
  constexpr const char *kInvalid = "CONFIG OWNER invalid-stored-profile=true";
  // A first boot seeds only `cfg`. A missing `owner` record is all empty
  // and prints nothing.
  assert(config::load(false));
  assert(owner_empty());
  assert(!fake_platform::nvs.blobs.count("owner"));
  assert(fake_platform::nvs.writes == 1);
  assert(log_text().find("CONFIG OWNER") == std::string::npos);

  // Bounds per key, maximum accepted and one byte more refused.
  struct Bound {
    const char *key;
    std::size_t max;
    char fill;
  };
  for (const auto &bound :
       {Bound{"owner.title", 64, 't'}, Bound{"owner.description", 160, 'd'},
        Bound{"owner.provider", 64, 'p'},
        Bound{"thing.deployment_id", 32, 'x'}}) {
    accept(std::string(bound.key) + "=" + std::string(bound.max, bound.fill));
    reject(std::string(bound.key) + "=" +
               std::string(bound.max + 1, bound.fill),
           bound.key);
  }
  const std::string url_max = "https://" + std::string(88, 'u');
  accept("owner.url=" + url_max);
  reject("owner.url=" + url_max + "u", "owner.url");
  auto owner = config::owner_settings();
  assert(std::strlen(owner.title.data()) == 64 &&
         std::strlen(owner.description.data()) == 160 &&
         std::strlen(owner.provider.data()) == 64 &&
         owner.url.data() == url_max &&
         std::strlen(config::deployment_id().text.data()) == 32);
  // Bounds count bytes, not characters.
  accept("owner.title=" + repeat("\xc3\xa9", 32));
  reject("owner.title=" + repeat("\xc3\xa9", 32) + "a", "owner.title");
  accept("owner.provider=" + repeat("\xe2\x82\xac", 21) + "a");
  accept("owner.description=" + repeat("\xf0\x9f\x8c\x8d", 40));
  reject("owner.description=" + repeat("\xf0\x9f\x8c\x8d", 40) + "a",
         "owner.description");

  // Empty clears each key.
  accept("owner.title=\nowner.description=\nowner.provider=\nowner.url=\n"
         "thing.deployment_id=");
  assert(owner_empty());

  // Invalid UTF-8 and control characters.
  for (const char *bad :
       {"\xff", "a\xc3", "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80",
        "\xf0\x80\x80\x80", "\xf0\x8f\xbf\xbf", "\xf4\x90\x80\x80",
        "\xf8\x88\x80\x80\x80", "\x80", "tab\there", "bell\x07", "del\x7f",
        "nel\xc2\x85", "csi\xc2\x9b", "cr\rinside"}) {
    reject(std::string("owner.title=") + bad, "owner.title");
    reject(std::string("owner.description=") + bad, "owner.description");
    reject(std::string("owner.provider=") + bad, "owner.provider");
  }
  accept("owner.title=Cairo \xc2\xb7 \xd8\xa7\xd9\x84\xd9\x82\xd8\xa7\xd9"
         "\x87\xd8\xb1\xd8\xa9 \xc2\xa0");
  // owner.url needs https:// and a host that starts with a letter or digit,
  // and only RFC 3986 characters.
  for (const char *bad :
       {"http://example.org", "HTTPS://example.org", "https://", "https:///x",
        "https://?q", "https://@", "https://@example.org",
        "https://:", "https://:443", "https://-x.org", "https://.org",
        "https://%41", "ftp://example.org", "example.org", "https://a b",
        "https://a\"b", "https://a\\b", "https://a<b>", "https://a\x01",
        "https://caf\xc3\xa9.org",
        // Any user info, an @ before the first /, ? or #, is refused because
        // page 5 echoes the link and STAC publishes it (#151, #152).
        "https://a@", "https://a@/x", "https://a@?q", "https://a@#top",
        "https://a@:443", "https://a@-x.org", "https://a@b@",
        "https://a@example.org/x", "https://a:b@example.org:8443/x",
        "https://a:b@example.org", "https://example.org@evil.example/x"})
    reject(std::string("owner.url=") + bad, "owner.url");
  accept("owner.url=https://example.org/air?site=7&a=b%20c#top");
  // The authority ends at the first /, ? or #, so an @ after it is fine.
  accept("owner.url=https://medium.com/@name");
  accept("owner.url=https://example.org/@a/b");
  accept("owner.url=https://example.org?to=@a#@b");
  accept("owner.url=https://example.org?to=a@");
  accept("owner.url=https://example.org#a@");
  accept("owner.url=https://1.example.org");
  // thing.deployment_id is [a-z0-9-] only, a letter or digit at each end and
  // never "unknown", the footer fallback.
  for (const char *bad :
       {"Cairo-07", "cairo_07", "cairo 07", "cairo.07", "cair\xc3\xb6",
        "cairo/07", "-cairo", "cairo-", "-", "---", "unknown"})
    reject(std::string("thing.deployment_id=") + bad, "thing.deployment_id");
  for (const char *good : {"a", "7", "a-b", "a--b", "unknown-07", "unknowns"})
    accept(std::string("thing.deployment_id=") + good);
  accept("thing.deployment_id=cairo-07");
  assert(std::strcmp(config::deployment_id().text.data(), "cairo-07") == 0);

  // One bad owner key refuses the whole request, valid keys included.
  reject("owner.title=New\nowner.provider=New Org\nowner.url=http://bad\n"
         "wifi.ssid=other",
         "owner.url");
  reject("thing.deployment_id=giza-01\nowner.title=" + std::string(65, 't'),
         "owner.title");
  reject("owner.title=New\nble.pin=12", "ble.pin");
  reject("owner.email=owner@example.org", "owner.email");
  assert(std::strcmp(config::deployment_id().text.data(), "cairo-07") == 0);

  // Round trip through the record, with escaping characters.
  const auto cfg_before = cfg();
  const auto settings_before = config::get();
  const auto writes = fake_platform::nvs.writes;
  config::Actions actions;
  char bad_key[64];
  assert(apply("owner.title=Cairo \"Air\" \\ Net\nowner.description=Rooftop "
               "sensors\nowner.provider=Walkthru\nowner.url=https://"
               "walkthru.earth\nthing.deployment_id=cairo-08",
               actions, bad_key));
  assert(!actions.location_changed && !actions.wifi_changed &&
         !actions.ntp_changed && !actions.ble_changed);
  // An owner only request writes the owner record once and leaves `cfg`.
  assert(fake_platform::nvs.writes == writes + 1);
  assert(cfg() == cfg_before);
  assert(same(settings_before, config::get()));
  check_owner_header();
  assert(std::strcmp(owner_field(kTitleAt), "Cairo \"Air\" \\ Net") == 0);
  assert(std::strcmp(owner_field(kDescriptionAt), "Rooftop sensors") == 0);
  assert(std::strcmp(owner_field(kProviderAt), "Walkthru") == 0);
  assert(std::strcmp(owner_field(kUrlAt), "https://walkthru.earth") == 0);
  assert(std::strcmp(owner_field(kDeploymentAt), "cairo-08") == 0);
  const auto saved = config::owner_settings();
  assert(config::load(false));
  assert(same_owner(saved, config::owner_settings()));
  assert(same(settings_before, config::get()));
  char json[480];
  assert(config::build_page(json, sizeof(json), 5));
  assert(std::strcmp(json, "{\"owner\":{\"title\":\"Cairo \\\"Air\\\" \\\\ "
                           "Net\",\"provider\":\"Walkthru\",\"url\":\"https:"
                           "//walkthru.earth\"},\"thing\":{\"deployment_id\":"
                           "\"cairo-08\"}}") == 0);
  assert(config::build_page(json, sizeof(json), 6));
  assert(std::strcmp(json,
                     "{\"owner\":{\"description\":\"Rooftop sensors\"}}") == 0);
  // Re-sending the same values writes nothing.
  accept("thing.deployment_id=cairo-08");
  assert(fake_platform::nvs.writes == writes + 1);
  // A mixed request saves both records.
  accept("owner.provider=Walkthru Earth\nlocation.country=EG");
  assert(fake_platform::nvs.writes == writes + 3);
  assert(std::strcmp(config::get().location.country, "EG") == 0);
  assert(config::load(false));
  assert(std::strcmp(config::owner_settings().provider.data(),
                     "Walkthru Earth") == 0);

  // A failed owner save changes nothing, live or stored. The previous record
  // is written back, and when that fails too one line names the rollback.
  constexpr const char *kRollback = "CONFIG ERROR operation=owner-rollback";
  constexpr const char *kCfgRollback = "CONFIG ERROR operation=cfg-rollback";
  const auto stored = owner_blob();
  const auto live = config::owner_settings();
  for (const bool on_commit : {false, true}) {
    (on_commit ? fake_platform::nvs.fail_commit_key
               : fake_platform::nvs.fail_write_key) = "owner";
    const auto rollbacks = count(log_text(), kRollback);
    assert(!apply("thing.deployment_id=giza-01", actions, bad_key));
    assert(std::strcmp(bad_key, "nvs") == 0);
    assert(count(log_text(), kRollback) == rollbacks + 1);
    assert(same_owner(live, config::owner_settings()));
    assert(owner_blob() == stored);
    fake_platform::nvs.fail_write_key.clear();
    fake_platform::nvs.fail_commit_key.clear();
  }
  // The unsure record is written again by the next request, even one with no
  // owner key in it.
  const auto unsure_writes = fake_platform::nvs.writes;
  accept("location.country=EG");
  assert(fake_platform::nvs.writes == unsure_writes + 2);
  assert(owner_blob() == stored);
  accept("location.country=EG");
  assert(fake_platform::nvs.writes == unsure_writes + 3);
  // A failed `cfg` save after the owner save puts both records back.
  const auto settings_live = config::get();
  const auto cfg_live = cfg();
  fake_platform::nvs.fail_write_key = "cfg";
  assert(
      !apply("thing.deployment_id=giza-01\nwifi.ssid=other", actions, bad_key));
  assert(std::strcmp(bad_key, "nvs") == 0);
  fake_platform::nvs.fail_write_key.clear();
  assert(same_owner(live, config::owner_settings()));
  assert(same(settings_live, config::get()));
  assert(owner_blob() == stored);
  assert(cfg() == cfg_live);
  assert(config::load(false));
  assert(same_owner(live, config::owner_settings()));
  // A `cfg` readback that fails after its commit puts both records back too,
  // so a reboot never pairs the new location with the old deployment id
  // (#151). The write back commits but its readback fails as well, so `cfg`
  // stays unsure and one line names it.
  const auto cfg_rollbacks = count(log_text(), kCfgRollback);
  fake_platform::nvs.corrupt_read_key = "cfg";
  assert(!apply("thing.deployment_id=giza-01\nlocation.country=US", actions,
                bad_key));
  fake_platform::nvs.corrupt_read_key.clear();
  assert(std::strcmp(bad_key, "nvs") == 0);
  assert(count(log_text(), kCfgRollback) == cfg_rollbacks + 1);
  assert(same_owner(live, config::owner_settings()));
  assert(same(settings_live, config::get()));
  assert(owner_blob() == stored);
  assert(cfg() == cfg_live);
  // The unsure `cfg` is written again by the next request, even one that
  // changes nothing.
  const auto cfg_unsure_writes = fake_platform::nvs.writes;
  accept(std::string("thing.deployment_id=") + live.deployment.text.data());
  assert(fake_platform::nvs.writes == cfg_unsure_writes + 1);
  assert(cfg() == cfg_live);
  accept(std::string("thing.deployment_id=") + live.deployment.text.data());
  assert(fake_platform::nvs.writes == cfg_unsure_writes + 1);
  assert(config::load(false));
  assert(same_owner(live, config::owner_settings()));
  assert(same(settings_live, config::get()));
  // When both write backs fail, the reply is still ERROR nvs, one line names
  // each rollback and the live snapshot keeps the whole previous state. The
  // owner record holds the new values, and a boot before any other request
  // loads them.
  const auto rollbacks = count(log_text(), kRollback);
  const auto cfg_rollbacks_before = count(log_text(), kCfgRollback);
  fake_platform::nvs.fail_write_key = "cfg";
  fake_platform::nvs.fail_write_at = fake_platform::nvs.writes + 4;
  assert(
      !apply("thing.deployment_id=giza-01\nwifi.ssid=other", actions, bad_key));
  assert(std::strcmp(bad_key, "nvs") == 0);
  assert(fake_platform::nvs.writes == fake_platform::nvs.fail_write_at);
  fake_platform::nvs.fail_write_key.clear();
  fake_platform::nvs.fail_write_at = 0;
  assert(count(log_text(), kRollback) == rollbacks + 1);
  assert(count(log_text(), kCfgRollback) == cfg_rollbacks_before + 1);
  assert(same_owner(live, config::owner_settings()));
  assert(same(settings_live, config::get()));
  assert(std::strcmp(owner_field(kDeploymentAt), "giza-01") == 0);
  assert(config::load(false));
  assert(std::strcmp(config::deployment_id().text.data(), "giza-01") == 0);
  assert(same(settings_live, config::get()));
  owner_blob() = stored;
  assert(config::load(false));
  assert(same_owner(live, config::owner_settings()));
  // Without a reboot the next request writes the unsure record again, even
  // when it asks for the value the live snapshot already holds (#151). Here
  // the owner readback fails and so does the write back, which leaves the
  // refused value in flash.
  fake_platform::nvs.corrupt_read_key = "owner";
  fake_platform::nvs.fail_write_at = fake_platform::nvs.writes + 2;
  assert(!apply("thing.deployment_id=giza-01", actions, bad_key));
  fake_platform::nvs.corrupt_read_key.clear();
  fake_platform::nvs.fail_write_at = 0;
  assert(std::strcmp(owner_field(kDeploymentAt), "giza-01") == 0);
  assert(same_owner(live, config::owner_settings()));
  const auto reconcile_writes = fake_platform::nvs.writes;
  accept(std::string("thing.deployment_id=") + live.deployment.text.data());
  assert(fake_platform::nvs.writes == reconcile_writes + 1);
  assert(owner_blob() == stored);
  assert(config::load(false));
  assert(same_owner(live, config::owner_settings()));
  // A readback that differs from the written record fails the save. The
  // previous record is committed again, `cfg` is never written for an owner
  // only request and the live snapshot does not move.
  const auto cfg_kept = cfg();
  const auto readback_writes = fake_platform::nvs.writes;
  fake_platform::nvs.corrupt_read_key = "owner";
  assert(!apply("thing.deployment_id=giza-01", actions, bad_key));
  fake_platform::nvs.corrupt_read_key.clear();
  assert(std::strcmp(bad_key, "nvs") == 0);
  assert(fake_platform::nvs.writes == readback_writes + 2);
  assert(same_owner(live, config::owner_settings()));
  assert(owner_blob() == stored);
  assert(cfg() == cfg_kept);
  // A mixed request stops before `cfg` when the owner readback fails.
  fake_platform::nvs.corrupt_read_key = "owner";
  assert(
      !apply("thing.deployment_id=giza-01\nwifi.ssid=other", actions, bad_key));
  fake_platform::nvs.corrupt_read_key.clear();
  assert(std::strcmp(bad_key, "nvs") == 0);
  assert(fake_platform::nvs.writes == readback_writes + 4);
  assert(cfg() == cfg_kept);
  assert(owner_blob() == stored);
  assert(same(settings_live, config::get()));
  assert(config::load(false));
  assert(same_owner(live, config::owner_settings()));

  // A damaged record reads as all empty with one line, and stays in NVS.
  auto good = owner_blob();
  for (const auto damage : {0, 1, 2, 3, 4}) {
    owner_blob() = good;
    if (damage == 0)
      owner_blob()[kHeaderBytes + kTitleAt] ^= 1; // CRC mismatch
    else if (damage == 1)
      owner_blob()[0] = 'X'; // magic
    else if (damage == 2)
      owner_blob()[4] = 2; // version
    else if (damage == 3)
      owner_blob().pop_back(); // size
    else {
      // Sealed, but the value fails the SET_CONFIG rule.
      std::memcpy(owner_field(kDeploymentAt), "Cairo", 5);
      reseal_owner();
    }
    const auto lines = count(log_text(), kInvalid);
    assert(config::load(false));
    assert(owner_empty());
    assert(count(log_text(), kInvalid) == lines + 1);
    assert(fake_platform::nvs.blobs.count("owner"));
    // The rest of the configuration is untouched.
    assert(std::strcmp(config::get().location.country, "EG") == 0);
  }
  // A record of another type is damaged too, and never erased.
  fake_platform::nvs.blobs.erase("owner");
  fake_platform::nvs.strings["owner"] = "not-a-blob";
  fake_platform::nvs.types["owner"] = 8;
  assert(config::load(false));
  assert(owner_empty());
  assert(fake_platform::nvs.strings.at("owner") == "not-a-blob");
  // A damaged `cfg` is a first boot but keeps a good owner record.
  fake_platform::nvs.strings.erase("owner");
  fake_platform::nvs.types.erase("owner");
  fake_platform::nvs.blobs["owner"] = good;
  fake_platform::nvs.types["owner"] = 9;
  cfg()[kHeaderBytes] ^= 1;
  assert(config::load(false));
  assert(same_owner(live, config::owner_settings()));

  // Worst case pages. Every field at its maximum, every escapable character
  // escaped. The test driver parses both pages and checks their size.
  accept("owner.title=" + std::string(64, '"') + "\nowner.description=" +
         std::string(160, '\\') + "\nowner.provider=" + std::string(64, '\\') +
         "\nowner.url=" + url_max +
         "\nthing.deployment_id=" + std::string(32, 'z'));
  const auto page5 = config::build_page(json, sizeof(json), 5);
  assert(page5 == 458);
  std::printf("%s\n", json);
  const auto page6 = config::build_page(json, sizeof(json), 6);
  assert(page6 == 348);
  std::printf("%s\n", json);
  // A buffer one byte short of the page fails cleanly.
  assert(!config::build_page(json, page5, 5) && json[0] == '\0');

  // The split rule compares the deployment text only.
  config::DeploymentId a{}, b{};
  std::snprintf(a.text.data(), a.text.size(), "cairo-07");
  std::snprintf(b.text.data(), b.text.size(), "cairo-07");
  assert(config::same_deployment(a, b));
  b.text[7] = '8';
  assert(!config::same_deployment(a, b));
  assert(!config::same_deployment(a, config::DeploymentId{}));

  // No owner value ever reaches the serial log.
  const auto logs = log_text();
  for (const char *value : {"Cairo", "Rooftop", "Walkthru", "walkthru.earth",
                            "cairo-0", "giza", "example.org", "zzzz", "uuuu"})
    assert(logs.find(value) == std::string::npos);
}

void owner_unavailable() {
  fake_platform::nvs.fail_open = true;
  assert(!config::load(false));
  assert(owner_empty());
  accept("thing.deployment_id=ram-only\nowner.title=RAM");
  assert(std::strcmp(config::deployment_id().text.data(), "ram-only") == 0);
  assert(fake_platform::nvs.writes == 0);
}

void unavailable() {
  fake_platform::nvs.fail_open = true;
  assert(!config::load(false));
  assert(config::get().pair == config::PairMode::Fixed);
  assert(!config::get().display);
  assert(fake_platform::nvs.writes == 0);
  accept("wifi.ssid=ram-only\nwifi.on=true");
  assert(config::get().wifi_on && fake_platform::nvs.writes == 0);
}
} // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string test = argv[1];
  if (test == "headless")
    first_boot(false);
  else if (test == "screen")
    first_boot(true);
  else if (test == "migration")
    migration();
  else if (test == "validation")
    validation();
  else if (test == "credentials")
    credentials();
  else if (test == "sensor")
    sensor_identity();
  else if (test == "nvs")
    persistence_failure();
  else if (test == "time-location")
    time_and_location();
  else if (test == "setup")
    setup_state();
  else if (test == "phone-cell")
    phone_cell();
  else if (test == "blob")
    blob_round_trip();
  else if (test == "old-keys")
    old_keys_ignored();
  else if (test == "torn")
    torn_blob();
  else if (test == "unavailable")
    unavailable();
  else if (test == "owner")
    owner_configuration();
  else if (test == "owner-unavailable")
    owner_unavailable();
  else
    assert(false);
}
