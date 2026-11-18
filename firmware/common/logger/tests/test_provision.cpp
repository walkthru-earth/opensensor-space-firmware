#include <ble_sync.h>
#include <cassert>
#include <cstring>
#include <debug_log.h>
#include <os_logger_provision.h>
#include <string>
#include <wifi_link.h>

namespace {
unsigned cleared = 0, dropped = 0, reapplied = 0;
std::string hex(const std::string &value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (const unsigned char byte : value) {
    result += digits[byte >> 4];
    result += digits[byte & 15];
  }
  return result;
}
oslogger::ProvisionResult apply(const std::string &lines) {
  return oslogger::apply_config_hex(hex(lines).c_str());
}
} // namespace
namespace ble {
LinkState link() { return {}; }
void clear_bonds() { ++cleared; }
} // namespace ble
namespace lan {
Status status() {
  Status value;
  value.state = "off";
  return value;
}
void drop_session(ble::DisconnectReason) { ++dropped; }
void apply_time_settings() {}
void apply_settings() { ++reapplied; }
} // namespace lan
int main() {
  assert(config::load(false));
  const auto token = config::get();
  const std::string ssid = "synthetic \xc3\xa9 SSID";
  const std::string psk = "synthetic secret = psk";
  auto result =
      apply("wifi.ssid=" + ssid + "\nwifi.psk=" + psk +
            "\nwifi.on=1\nlan.on=1\nble.clear_bonds=1\nlan.rotate_token=1\n");
  assert(result.ok && !result.reboot_required);
  assert(cleared == 1 && dropped == 1 && reapplied == 1);
  auto settings = config::get();
  assert(std::strcmp(settings.ssid, ssid.c_str()) == 0);
  assert(std::strcmp(settings.psk, psk.c_str()) == 0);
  assert(settings.wifi_on && settings.lan_on);
  assert(std::memcmp(token.token, settings.token, config::kTokenBytes) != 0);
  char profile[256]{};
  auto length =
      oslogger::format_wifi_profile(settings, profile, sizeof(profile));
  assert(length == std::strlen(profile));
  const auto expected = "OS WIFI_PROFILE ssid=" + hex(ssid) +
                        " psk=" + hex(psk) + " wifi_on=1 lan_on=1\n";
  assert(profile == expected);
  assert(oslogger::format_wifi_profile(settings, profile, length) == 0);
  assert(oslogger::format_wifi_profile(settings, profile, length + 1) ==
         length);
  assert(std::strstr(profile, "pin=") == nullptr &&
         std::strstr(profile, "token=") == nullptr);
  result = apply("ble.clear_bonds=1\nlan.rotate_token=1\nwifi.on=broken");
  assert(!result.ok && std::strcmp(result.bad_key, "wifi.on") == 0);
  assert(cleared == 1 && dropped == 1 && reapplied == 1);
  assert(std::memcmp(settings.token, config::get().token,
                     config::kTokenBytes) == 0);
  result = apply(std::string("wifi.ssid=changed\n") +
                 "wifi.psk=" + std::string("ab\0cdefgh", 9));
  assert(!result.ok && std::strcmp(result.bad_key, "malformed") == 0);
  assert(std::strcmp(config::get().ssid, ssid.c_str()) == 0);
  result = apply(psk); // malformed key content must not become an error log
  assert(!result.ok && std::strcmp(result.bad_key, "invalid-key") == 0);
  // A refused owner value names its key, an unknown owner.* key does not
  // (#178).
  result = apply("owner.url=http://plain.example");
  assert(!result.ok && std::strcmp(result.bad_key, "owner.url") == 0);
  result = apply("thing.deployment_id=Not_Allowed");
  assert(!result.ok && std::strcmp(result.bad_key, "thing.deployment_id") == 0);
  result = apply("owner.title=" + std::string(config::kOwnerTitleMax + 1, 't'));
  assert(!result.ok && std::strcmp(result.bad_key, "owner.title") == 0);
  result = apply("owner.secret=1");
  assert(!result.ok && std::strcmp(result.bad_key, "invalid-key") == 0);
  for (const auto *value : {"", "1", "0x", "gg"})
    assert(!oslogger::apply_config_hex(value).ok);
  assert(!oslogger::apply_config_hex(
              std::string(2 * ble::kMaxControlBytes, '0').c_str())
              .ok);
  std::string boundary = "lan.on=1\n";
  boundary.resize(ble::kMaxControlBytes - 1, '\n');
  assert(apply(boundary).ok); // maximum whole decoded command is accepted
  assert(!apply("wifi.psk=short").ok);
  assert(!apply(std::string(128, 'x')).ok);
  assert(cleared == 1 && dropped == 1 && reapplied == 1);
  result = apply("ble.pin=000001");
  assert(result.ok && result.reboot_required);
  std::strcpy(settings.ssid, std::string(config::kSsidMax, 's').c_str());
  std::strcpy(settings.psk, std::string(config::kPskMax, 'p').c_str());
  assert(oslogger::format_wifi_profile(settings, profile, sizeof(profile)) > 0);
  char logs[DebugLog::kFallbackRingBytes + 1]{};
  std::uint32_t total = 0;
  const auto count = oslog.tail(logs, sizeof(logs) - 1, total);
  logs[count] = '\0';
  assert(std::strstr(logs, ssid.c_str()) == nullptr);
  assert(std::strstr(logs, psk.c_str()) == nullptr);
  assert(std::strstr(logs, hex(psk).c_str()) == nullptr);
  assert(std::strstr(logs, "000001") == nullptr);
  assert(std::strstr(logs, "OS WIFI_PROFILE") == nullptr);
}

namespace opensensor::network_time {
Status status() { return Status{}; }
} // namespace opensensor::network_time
