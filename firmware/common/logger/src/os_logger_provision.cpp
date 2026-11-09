#include "os_logger_provision.h"
#include <ble_sync.h>
#include <control_sync.h>
#include <cstdio>
#include <cstring>

namespace oslogger {
namespace {
int nibble(char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  return -1;
}
void safe_error(ProvisionResult &result, const char *key) {
  constexpr const char *keys[] = {"ble.pair",
                                  "ble.pin",
                                  "ble.clear_bonds",
                                  "wifi.on",
                                  "wifi.ssid",
                                  "wifi.psk",
                                  "lan.on",
                                  "lan.rotate_token",
                                  "ntp.on",
                                  "ntp.dhcp",
                                  "ntp.public_fallback",
                                  "ntp.interval_s",
                                  "ntp.server1",
                                  "ntp.server2",
                                  "location.cell",
                                  "location.resolution",
                                  "location.country",
                                  "location.phone_cell",
                                  "thing.mobility",
                                  "sensor.vendor",
                                  "sensor.model",
                                  "sensor.serial",
                                  "sensor.identity",
                                  "sensor.batch_candidate",
                                  "malformed",
                                  "line-too-long",
                                  "empty",
                                  "nvs"};
  const char *safe = "invalid-key";
  for (const auto *candidate : keys)
    if (key && std::strcmp(key, candidate) == 0)
      safe = candidate;
  std::snprintf(result.bad_key, sizeof(result.bad_key), "%s", safe);
}
} // namespace
std::size_t format_wifi_profile(const config::Settings &settings, char *out,
                                std::size_t capacity) {
  char ssid[2 * config::kSsidMax + 1]{}, psk[2 * config::kPskMax + 1]{};
  constexpr char digits[] = "0123456789abcdef";
  auto hex = [&](const char *text, char *encoded, std::size_t maximum) {
    std::size_t i = 0;
    while (i < maximum && text[i]) {
      const auto byte = static_cast<unsigned char>(text[i]);
      encoded[2 * i] = digits[byte >> 4];
      encoded[2 * i + 1] = digits[byte & 15];
      ++i;
    }
  };
  hex(settings.ssid, ssid, config::kSsidMax);
  hex(settings.psk, psk, config::kPskMax);
  const int written = std::snprintf(
      out, capacity, "OS WIFI_PROFILE ssid=%s psk=%s wifi_on=%u lan_on=%u\n",
      ssid, psk, settings.wifi_on ? 1U : 0U, settings.lan_on ? 1U : 0U);
  return written > 0 && std::size_t(written) < capacity ? std::size_t(written)
                                                        : 0;
}
ProvisionResult apply_config_hex(const char *hex) {
  ProvisionResult result;
  const std::size_t length = hex ? std::strlen(hex) : 0;
  if (!length || length % 2 || length / 2 > ble::kMaxControlBytes - 1) {
    std::strcpy(result.bad_key, "malformed-hex");
    return result;
  }
  ble::ControlRequest request{};
  request.bytes[0] = ble::kOpSetConfig;
  request.length = static_cast<std::uint16_t>(1 + length / 2);
  for (std::size_t i = 0; i < length; i += 2) {
    const int high = nibble(hex[i]), low = nibble(hex[i + 1]);
    if (high < 0 || low < 0) {
      std::strcpy(result.bad_key, "malformed-hex");
      return result;
    }
    request.bytes[1 + i / 2] = static_cast<std::uint8_t>((high << 4) | low);
  }
  const ossync::ControlReplies replies{
      &result, [](void *, ble::Link) { return ble::kMaxFrame; },
      [](void *context, ble::Link, const std::uint8_t *frame,
         std::size_t size) {
        if (size >= 2 && frame[0] == ble::kFrameConfig) {
          auto &response = *static_cast<ProvisionResult *>(context);
          response.ok = true;
          response.reboot_required = frame[1] != 0;
        }
        return true;
      },
      [](void *context, ble::Link, ble::Op, ble::Error, const char *detail) {
        safe_error(*static_cast<ProvisionResult *>(context), detail);
        return true;
      }};
  ossync::handle_common_control(request, replies);
  return result;
}
} // namespace oslogger
