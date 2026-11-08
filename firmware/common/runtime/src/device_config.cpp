#include "device_config.h"
#include "debug_log.h"
#include "os_location.h"

#ifdef ESP_PLATFORM
#include <lwip/sockets.h>
#else
#include <arpa/inet.h>
#endif
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <nvs.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>
#include <type_traits>

namespace config {
namespace {
constexpr const char *kNamespace = "oscfg";
portMUX_TYPE settings_mutex = portMUX_INITIALIZER_UNLOCKED;
Settings current;
Settings booted; // pair/pin the running BLE stack was started with
bool persistent = false;
// A record whose save failed and whose write back of the previous value failed
// too, so flash may hold the refused value. The next SET_CONFIG writes it again
// from the live snapshot, even when its keys did not change (#151). `load`
// clears both, because the live snapshot then matches flash.
bool cfg_unsure = false;
bool owner_unsure = false;
SensorPolicy sensor_policy; // installed by the board before load()

std::uint32_t random_pin() { return esp_random() % 1000000U; }

bool committed(nvs_handle_t store, esp_err_t result) {
  return result == ESP_OK && nvs_commit(store) == ESP_OK;
}

bool sensor_serial_chars(const char *value) {
  if (std::strlen(value) > kSensorSerialMax)
    return false;
  for (const char *p = value; *p; ++p)
    if (!(*p >= 'A' && *p <= 'Z') && !(*p >= '0' && *p <= '9') && *p != '-')
      return false;
  return true;
}

const char *hardware_vendor() {
  return sensor_policy.vendor ? sensor_policy.vendor : "";
}
const char *hardware_model() {
  return sensor_policy.model ? sensor_policy.model : "";
}
// Empty clears the serial. Without a board rule no serial is accepted.
bool serial_allowed(const char *serial) {
  return !*serial ||
         (sensor_policy.valid_serial && sensor_policy.valid_serial(serial));
}
bool batch_allowed(const Settings &settings) {
  return sensor_policy.batch_eligible &&
         sensor_policy.batch_eligible(settings.sensor_vendor,
                                      settings.sensor_model,
                                      settings.sensor_serial);
}

// Sensor identity and opt-in share one NVS value so a failed multi-key save
// cannot pair an old opt-in with a replacement unit's new serial on reboot.
void encode_sensor(char *out, std::size_t size, const Settings &settings) {
  std::snprintf(out, size, "%s|%s|%s|%u", settings.sensor_vendor,
                settings.sensor_model, settings.sensor_serial,
                settings.sensor_batch_candidate ? 1U : 0U);
}

bool decode_sensor(char *value, Settings &settings) {
  char *vendor = value;
  char *first = std::strchr(vendor, '|');
  if (!first)
    return false;
  *first++ = '\0';
  char *model = first;
  char *second = std::strchr(model, '|');
  if (!second)
    return false;
  *second++ = '\0';
  char *serial = second;
  char *third = std::strchr(serial, '|');
  if (!third)
    return false;
  *third++ = '\0';
  if (std::strchr(third, '|') ||
      std::strlen(vendor) >= sizeof(settings.sensor_vendor) ||
      std::strlen(model) >= sizeof(settings.sensor_model) ||
      !sensor_serial_chars(serial) || !serial_allowed(serial) ||
      (std::strcmp(third, "0") != 0 && std::strcmp(third, "1") != 0))
    return false;
  if (*vendor && (std::strcmp(vendor, hardware_vendor()) != 0 ||
                  std::strcmp(model, hardware_model()) != 0))
    return false;
  if ((*vendor == '\0') != (*model == '\0') || (*serial && !*vendor))
    return false;
  std::memcpy(settings.sensor_vendor, vendor, std::strlen(vendor) + 1);
  std::memcpy(settings.sensor_model, model, std::strlen(model) + 1);
  std::memcpy(settings.sensor_serial, serial, std::strlen(serial) + 1);
  settings.sensor_batch_candidate = std::strcmp(third, "1") == 0;
  if (settings.sensor_batch_candidate && !batch_allowed(settings))
    return false;
  return true;
}

bool parse_uint(const char *value, std::uint32_t minimum, std::uint32_t maximum,
                std::uint32_t &out) {
  if (!*value)
    return false;
  std::uint32_t number = 0;
  for (const char *p = value; *p; ++p) {
    if (*p < '0' || *p > '9')
      return false;
    const auto digit = static_cast<std::uint32_t>(*p - '0');
    if (number > maximum / 10 ||
        (number == maximum / 10 && digit > maximum % 10))
      return false;
    number = number * 10 + digit;
  }
  if (number < minimum)
    return false;
  out = number;
  return true;
}

bool valid_server(const char *value) {
  const auto length = std::strlen(value);
  if (length > kNtpServerMax)
    return false;
  if (!length)
    return true;
  unsigned char address[16]{};
  if (inet_pton(AF_INET, value, address) == 1)
    return address[0] || address[1] || address[2] || address[3];
  if (inet_pton(AF_INET6, value, address) == 1)
    return std::any_of(address, address + sizeof(address),
                       [](unsigned char byte) { return byte != 0; });
  // DNS names only: reject URLs, ports, record delimiters and invalid labels.
  unsigned label_length = 0;
  for (std::size_t i = 0; i < length; ++i) {
    const char c = value[i];
    if (c == '.') {
      if (!label_length || value[i - 1] == '-')
        return false;
      label_length = 0;
    } else {
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || (c == '-' && label_length)))
        return false;
      if (++label_length > 63)
        return false;
    }
  }
  return value[length - 1] != '-';
}

bool split_record(char *value, char **fields, std::size_t count) {
  fields[0] = value;
  for (std::size_t i = 1; i < count; ++i) {
    char *separator = std::strchr(fields[i - 1], '|');
    if (!separator)
      return false;
    *separator = '\0';
    fields[i] = separator + 1;
  }
  return std::strchr(fields[count - 1], '|') == nullptr;
}

void encode_location(char *out, std::size_t size,
                     const LocationSettings &location) {
  char cell[opensensor::location::kCellTextBytes]{};
  if (location.cell)
    opensensor::location::format_cell(location.cell, cell, sizeof(cell));
  std::snprintf(out, size, "%s|%u|%s|%s", cell, unsigned(location.resolution),
                location.country, mobility_name(location.mobility));
}

bool parse_mobility(const char *value, Mobility &mobility) {
  if (std::strcmp(value, "fixed") == 0)
    mobility = Mobility::Fixed;
  else if (std::strcmp(value, "moving") == 0)
    mobility = Mobility::Moving;
  else
    return false;
  return true;
}

// `cell|resolution|country|mobility`. A record written before #83 has no
// mobility field and reads as a fixed Thing, so a reflash keeps the location.
bool decode_location(char *record, LocationSettings &location) {
  char *fields[4];
  std::uint32_t resolution;
  LocationSettings decoded;
  const bool legacy =
      std::count(record, record + std::strlen(record), '|') == 2;
  if (!split_record(record, fields, legacy ? 3 : 4) ||
      (!legacy && !parse_mobility(fields[3], decoded.mobility)) ||
      !parse_uint(fields[1], 0, 15, resolution) ||
      (*fields[0] &&
       !opensensor::location::parse_cell(fields[0], decoded.cell)) ||
      (*fields[2] && !opensensor::location::valid_country(fields[2])))
    return false;
  decoded.resolution = static_cast<std::uint8_t>(resolution);
  if (decoded.cell &&
      !opensensor::location::coarsen(decoded.cell, resolution, decoded.cell))
    return false;
  std::snprintf(decoded.country, sizeof(decoded.country), "%s", fields[2]);
  location = decoded;
  return true;
}

void encode_ntp(char *out, std::size_t size, const NtpSettings &ntp) {
  std::snprintf(out, size, "%u|%u|%u|%lu|%s|%s", ntp.enabled ? 1U : 0U,
                ntp.dhcp ? 1U : 0U, ntp.public_fallback ? 1U : 0U,
                static_cast<unsigned long>(ntp.interval_s), ntp.servers[0],
                ntp.servers[1]);
}

bool decode_ntp(char *record, NtpSettings &ntp) {
  char *fields[6];
  NtpSettings decoded;
  std::uint32_t enabled, dhcp, fallback;
  if (!split_record(record, fields, 6) ||
      !parse_uint(fields[0], 0, 1, enabled) ||
      !parse_uint(fields[1], 0, 1, dhcp) ||
      !parse_uint(fields[2], 0, 1, fallback) ||
      !parse_uint(fields[3], 60, 86400, decoded.interval_s) ||
      !valid_server(fields[4]) || !valid_server(fields[5]))
    return false;
  decoded.enabled = enabled != 0;
  decoded.dhcp = dhcp != 0;
  decoded.public_fallback = fallback != 0;
  for (std::size_t i = 0; i < kNtpServerCount; ++i)
    std::snprintf(decoded.servers[i], sizeof(decoded.servers[i]), "%s",
                  fields[4 + i]);
  ntp = decoded;
  return true;
}

bool same_ntp(const NtpSettings &a, const NtpSettings &b) {
  return a.enabled == b.enabled && a.dhcp == b.dhcp &&
         a.public_fallback == b.public_fallback &&
         a.interval_s == b.interval_s &&
         std::strcmp(a.servers[0], b.servers[0]) == 0 &&
         std::strcmp(a.servers[1], b.servers[1]) == 0;
}

// The complete configuration is one NVS blob under `cfg`. A single
// nvs_set_blob replaces the whole record or leaves the previous one, and the
// CRC rejects anything else. A missing or damaged `cfg` is a first boot and
// gets fresh defaults (#111, no backward compatibility). No per-key layout is
// read, and nothing in the namespace is erased.
constexpr const char *kBlobKey = "cfg";
constexpr std::uint8_t kBlobMagic[4] = {'A', 'Q', 'C', 'F'};
constexpr std::uint16_t kBlobVersion = 1;
constexpr std::size_t kSensorRecordBytes = 80;
constexpr std::size_t kLocationRecordBytes = 40;
constexpr std::size_t kNtpRecordBytes = 560;

// Byte arrays only, so the layout has no padding and no host byte order.
struct BlobPayload {
  std::uint8_t pair;
  std::uint8_t pin[4];
  std::uint8_t display;
  std::uint8_t wifi_on;
  std::uint8_t lan_on;
  char ssid[kSsidMax + 1];
  char psk[kPskMax + 1];
  std::uint8_t token[kTokenBytes];
  char sensor[kSensorRecordBytes];
  char location[kLocationRecordBytes];
  char ntp[kNtpRecordBytes];
};
struct BlobHeader {
  std::uint8_t magic[4];
  std::uint8_t version[2];
  std::uint8_t length[2];
  std::uint8_t crc32[4]; // IEEE CRC-32 over the payload bytes
};
struct Blob {
  BlobHeader header;
  BlobPayload payload;
};
static_assert(alignof(Blob) == 1, "blob layout must not contain padding");
static_assert(sizeof(BlobHeader) == 12, "blob header is pinned");
static_assert(sizeof(BlobPayload) == 817, "blob version 1 payload is pinned");
static_assert(sizeof(Blob) == sizeof(BlobHeader) + sizeof(BlobPayload),
              "blob header and payload are contiguous");
static_assert(sizeof(BlobPayload) <= 0xffff, "payload length fits u16");
static_assert(sizeof(BlobPayload::ssid) == sizeof(Settings::ssid) &&
                  sizeof(BlobPayload::psk) == sizeof(Settings::psk) &&
                  sizeof(BlobPayload::token) == sizeof(Settings::token),
              "blob credentials mirror the settings fields");
static_assert(sizeof(Settings::sensor_vendor) + sizeof(Settings::sensor_model) +
                      sizeof(Settings::sensor_serial) + 2 <=
                  kSensorRecordBytes,
              "sensor record fits its field");
// Cell text with its NUL, two digit resolution, country, "moving" and the
// three separators.
static_assert(opensensor::location::kCellTextBytes + 2 + 2 + 6 + 3 <=
                  kLocationRecordBytes,
              "location record fits its field");
static_assert(kNtpServerCount * (kNtpServerMax + 1) + 16 <= kNtpRecordBytes,
              "NTP record fits its field");

static_assert(LocationSettings{}.resolution ==
                  opensensor::location::kDefaultResolution,
              "oscfg defaults to the shared H3 resolution");

// Byte arrays only, like Blob, so the layout has no padding.
struct OwnerPayload {
  char title[kOwnerTitleMax + 1];
  char description[kOwnerDescriptionMax + 1];
  char provider[kOwnerProviderMax + 1];
  char url[kOwnerUrlMax + 1];
  char deployment[kDeploymentIdMax + 1];
};
struct OwnerBlob {
  BlobHeader header;
  OwnerPayload payload;
};
static_assert(alignof(OwnerBlob) == 1, "owner layout must not contain padding");
static_assert(sizeof(OwnerPayload) == 421, "owner version 1 payload is pinned");
static_assert(sizeof(OwnerBlob) == sizeof(BlobHeader) + sizeof(OwnerPayload),
              "owner header and payload are contiguous");

// The staged record and its readback can share static storage, because
// saves never overlap. After startup only the storage worker saves, one
// command at a time, and no command source exists before `load` returns. The
// cfg and owner records take turns in the same two buffers, because every
// save and load finishes one record before it touches the other (#148).
union BlobBuffer {
  Blob cfg;
  OwnerBlob owner;
};
// The owner record rides in the cfg buffers, so it must never be the larger.
static_assert(sizeof(OwnerBlob) <= sizeof(Blob),
              "owner record fits the shared cfg buffers");
BlobBuffer staged_buffer;
BlobBuffer readback_buffer;
Blob &staged_blob = staged_buffer.cfg;
Blob &readback_blob = readback_buffer.cfg;
OwnerBlob &owner_staged = staged_buffer.owner;
OwnerBlob &owner_readback = readback_buffer.owner;

std::uint32_t crc32(const std::uint8_t *data, std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

void put_le(std::uint8_t *out, std::uint32_t value, std::size_t bytes) {
  for (std::size_t i = 0; i < bytes; ++i)
    out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::uint32_t get_le(const std::uint8_t *in, std::size_t bytes) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < bytes; ++i)
    value |= static_cast<std::uint32_t>(in[i]) << (8 * i);
  return value;
}

template <std::size_t N> bool terminated(const char (&field)[N]) {
  return std::memchr(field, '\0', N) != nullptr;
}

void encode_blob(Blob &blob, const Settings &settings) {
  std::memset(&blob, 0, sizeof(blob));
  BlobPayload &payload = blob.payload;
  payload.pair = static_cast<std::uint8_t>(settings.pair);
  put_le(payload.pin, settings.pin, sizeof(payload.pin));
  payload.display = settings.display ? 1 : 0;
  payload.wifi_on = settings.wifi_on ? 1 : 0;
  payload.lan_on = settings.lan_on ? 1 : 0;
  std::memcpy(payload.ssid, settings.ssid, sizeof(payload.ssid));
  std::memcpy(payload.psk, settings.psk, sizeof(payload.psk));
  std::memcpy(payload.token, settings.token, sizeof(payload.token));
  encode_sensor(payload.sensor, sizeof(payload.sensor), settings);
  // Each policy stays one coherent record, including the privacy cap.
  encode_location(payload.location, sizeof(payload.location),
                  settings.location);
  encode_ntp(payload.ntp, sizeof(payload.ntp), settings.ntp);
  std::memcpy(blob.header.magic, kBlobMagic, sizeof(kBlobMagic));
  put_le(blob.header.version, kBlobVersion, sizeof(blob.header.version));
  put_le(blob.header.length, sizeof(BlobPayload), sizeof(blob.header.length));
  put_le(
      blob.header.crc32,
      crc32(reinterpret_cast<const std::uint8_t *>(&payload), sizeof(payload)),
      sizeof(blob.header.crc32));
}

// Structural checks only. A record that passes still goes through the same
// value validation as a SET_CONFIG value.
bool blob_intact(const Blob &blob, std::size_t size) {
  const BlobPayload &payload = blob.payload;
  return size == sizeof(Blob) &&
         std::memcmp(blob.header.magic, kBlobMagic, sizeof(kBlobMagic)) == 0 &&
         get_le(blob.header.version, sizeof(blob.header.version)) ==
             kBlobVersion &&
         get_le(blob.header.length, sizeof(blob.header.length)) ==
             sizeof(BlobPayload) &&
         get_le(blob.header.crc32, sizeof(blob.header.crc32)) ==
             crc32(reinterpret_cast<const std::uint8_t *>(&payload),
                   sizeof(payload)) &&
         terminated(payload.ssid) && terminated(payload.psk) &&
         terminated(payload.sensor) && terminated(payload.location) &&
         terminated(payload.ntp);
}

bool save_locked(const Settings &settings) {
  nvs_handle_t store;
  if (nvs_open(kNamespace, NVS_READWRITE, &store) != ESP_OK)
    return false;
  encode_blob(staged_blob, settings);
  // One write and one commit.
  bool ok = committed(
      store, nvs_set_blob(store, kBlobKey, &staged_blob, sizeof(staged_blob)));
  std::size_t length = sizeof(readback_blob);
  ok = ok && nvs_get_blob(store, kBlobKey, &readback_blob, &length) == ESP_OK &&
       length == sizeof(staged_blob) &&
       std::memcmp(&readback_blob, &staged_blob, sizeof(staged_blob)) == 0;
  nvs_close(store);
  return ok;
}

// Saves `cfg`. On failure the previous record is written back, best effort,
// because the failed save may have committed before its readback failed. When
// that fails too the record stays unsure until a later save of it succeeds.
[[nodiscard]] bool save_cfg(const Settings &next, const Settings &previous) {
  if (save_locked(next)) {
    cfg_unsure = false;
    return true;
  }
  cfg_unsure = !save_locked(previous);
  if (cfg_unsure)
    oslog.println("CONFIG ERROR operation=cfg-rollback");
  return false;
}

void clear_sensor(Settings &settings) {
  settings.sensor_vendor[0] = '\0';
  settings.sensor_model[0] = '\0';
  settings.sensor_serial[0] = '\0';
  settings.sensor_batch_candidate = false;
  oslog.println("CONFIG SENSOR invalid-stored-profile=true");
}

// Invalid records are disabled or unset rather than silently enabling
// external traffic or location.
void apply_location(bool valid, char *record, Settings &settings) {
  if (!valid || !decode_location(record, settings.location)) {
    settings.location = LocationSettings{};
    oslog.println("CONFIG LOCATION invalid-stored-profile=true");
  }
}

void apply_ntp(bool valid, char *record, Settings &settings) {
  if (!valid || !decode_ntp(record, settings.ntp)) {
    settings.ntp = NtpSettings{};
    settings.ntp.enabled = false;
    settings.ntp.public_fallback = false;
    oslog.println("CONFIG NTP invalid-stored-profile=true");
  }
}

PairMode stored_pair_mode(std::uint8_t pair) {
  return pair > 2 ? PairMode::Random : static_cast<PairMode>(pair);
}

// Decodes an intact record in place. Its text fields are scratch afterwards.
void load_blob(BlobPayload &payload, Settings &settings) {
  settings.pair = stored_pair_mode(payload.pair);
  settings.pin = get_le(payload.pin, sizeof(payload.pin)) % 1000000U;
  settings.display = payload.display != 0;
  settings.wifi_on = payload.wifi_on != 0;
  settings.lan_on = payload.lan_on != 0;
  std::memcpy(settings.ssid, payload.ssid, sizeof(settings.ssid));
  std::memcpy(settings.psk, payload.psk, sizeof(settings.psk));
  std::memcpy(settings.token, payload.token, sizeof(settings.token));
  if (!decode_sensor(payload.sensor, settings))
    clear_sensor(settings);
  apply_location(true, payload.location, settings);
  apply_ntp(true, payload.ntp, settings);
}

void publish(const Settings &settings) {
  portENTER_CRITICAL(&settings_mutex);
  current = settings;
  portEXIT_CRITICAL(&settings_mutex);
}

bool parse_bool(const char *value, bool &out) {
  if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0) {
    out = false;
    return true;
  }
  if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0) {
    out = true;
    return true;
  }
  return false;
}

bool parse_pin(const char *value, std::uint32_t &out) {
  if (std::strlen(value) != 6)
    return false;
  std::uint32_t pin = 0;
  for (const char *p = value; *p; ++p) {
    if (*p < '0' || *p > '9')
      return false;
    pin = pin * 10 + static_cast<std::uint32_t>(*p - '0');
  }
  out = pin;
  return true;
}

bool printable(const char *value) {
  for (const char *p = value; *p; ++p)
    if (static_cast<unsigned char>(*p) < 32 || *p == 127)
      return false;
  return true;
}

std::size_t json_escape(char *out, std::size_t size, const char *text) {
  std::size_t used = 0;
  for (const char *p = text; *p && used + 7 < size; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == '"' || c == '\\') {
      out[used++] = '\\';
      out[used++] = static_cast<char>(c);
    } else if (c < 32) {
      used += static_cast<std::size_t>(
          std::snprintf(out + used, size - used, "\\u%04x", c));
    } else {
      out[used++] = static_cast<char>(c);
    }
  }
  out[used] = '\0';
  return used;
}
} // namespace

// Owner site configuration (#148). Its own record under `owner`, beside `cfg`
// and never inside it, so the `cfg` layout, key and type stay as they are. A
// missing record means every owner key is unset. A damaged one is unset too,
// with one serial line that carries no value.
namespace {
constexpr const char *kOwnerKey = "owner";
constexpr std::array<std::uint8_t, 4> kOwnerMagic = {'O', 'S', 'O', 'W'};
constexpr std::uint16_t kOwnerVersion = 1;

struct OwnerKeyInfo {
  std::string_view name;
  OwnerKey key = OwnerKey::Title;
  std::size_t max = 0;
};
static constexpr std::array<OwnerKeyInfo, 5> kOwnerKeys = {{
    {.name = "owner.title", .key = OwnerKey::Title, .max = kOwnerTitleMax},
    {.name = "owner.description",
     .key = OwnerKey::Description,
     .max = kOwnerDescriptionMax},
    {.name = "owner.provider",
     .key = OwnerKey::Provider,
     .max = kOwnerProviderMax},
    {.name = "owner.url", .key = OwnerKey::Url, .max = kOwnerUrlMax},
    {.name = "thing.deployment_id",
     .key = OwnerKey::Deployment,
     .max = kDeploymentIdMax},
}};

OwnerSettings owner_current;

// The text field of one key, in settings or in a stored payload.
template <typename Owner> auto owner_field(Owner &owner, OwnerKey key) {
  using Char = std::conditional_t<std::is_const_v<Owner>, const char, char>;
  switch (key) {
  case OwnerKey::Title:
    return std::span<Char>(owner.title);
  case OwnerKey::Description:
    return std::span<Char>(owner.description);
  case OwnerKey::Provider:
    return std::span<Char>(owner.provider);
  case OwnerKey::Url:
    return std::span<Char>(owner.url);
  case OwnerKey::Deployment:
    break;
  }
  if constexpr (std::is_same_v<std::remove_const_t<Owner>, OwnerPayload>)
    return std::span<Char>(owner.deployment);
  else
    return std::span<Char>(owner.deployment.text);
}

std::string_view text_of(std::span<const char> field) {
  const auto end = std::find(field.begin(), field.end(), '\0');
  return {field.data(), static_cast<std::size_t>(end - field.begin())};
}

// Copies a checked value and clears the rest, so equal values are equal
// byte for byte.
void store_text(std::span<char> field, std::string_view value) {
  std::fill(field.begin(), field.end(), '\0');
  std::copy_n(value.begin(), std::min(value.size(), field.size() - 1),
              field.begin());
}

// Strict UTF-8 (no overlong forms, surrogates or code points above
// U+10FFFF) without C0, DEL or C1 control characters.
[[nodiscard]] std::expected<void, OwnerValueError>
check_text(std::string_view value) {
  std::size_t i = 0;
  while (i < value.size()) {
    const auto lead = static_cast<unsigned char>(value[i]);
    if (lead < 0x80) {
      if (lead < 0x20 || lead == 0x7f)
        return std::unexpected(OwnerValueError::Control);
      ++i;
      continue;
    }
    std::size_t length = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xbf;
    if (lead >= 0xc2 && lead <= 0xdf) {
      length = 2;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      length = 3;
      if (lead == 0xe0)
        low = 0xa0;
      else if (lead == 0xed)
        high = 0x9f;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      length = 4;
      if (lead == 0xf0)
        low = 0x90;
      else if (lead == 0xf4)
        high = 0x8f;
    } else {
      return std::unexpected(OwnerValueError::Encoding);
    }
    if (value.size() - i < length)
      return std::unexpected(OwnerValueError::Encoding);
    for (std::size_t k = 1; k < length; ++k) {
      const auto next = static_cast<unsigned char>(value[i + k]);
      if (next < (k == 1 ? low : 0x80) || next > (k == 1 ? high : 0xbf))
        return std::unexpected(OwnerValueError::Encoding);
    }
    // U+0080..U+009F, the C1 controls, are C2 80..C2 9F.
    if (lead == 0xc2 && static_cast<unsigned char>(value[i + 1]) < 0xa0)
      return std::unexpected(OwnerValueError::Control);
    i += length;
  }
  return {};
}

constexpr bool ascii_alnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

// RFC 3986 characters only, so a link never needs JSON escaping.
constexpr bool url_char(char c) {
  if (ascii_alnum(c))
    return true;
  return std::string_view("-._~:/?#[]@!$&'()*+,;=%").contains(c);
}

[[nodiscard]] std::expected<void, OwnerValueError>
check_url(std::string_view value) {
  constexpr std::string_view kScheme = "https://";
  if (std::any_of(value.begin(), value.end(), [](char c) {
        return static_cast<unsigned char>(c) < 0x20 || c == 0x7f;
      }))
    return std::unexpected(OwnerValueError::Control);
  // The host starts with a letter or digit. That refuses https://@,
  // https://: and https:///x.
  if (!value.starts_with(kScheme) || value.size() == kScheme.size() ||
      !ascii_alnum(value[kScheme.size()]))
    return std::unexpected(OwnerValueError::Scheme);
  // The authority ends at the first /, ? or #, and an @ inside it starts user
  // info. Page 5 echoes the link and STAC publishes it, so user info is never
  // stored. An @ in the path, query or fragment is fine, as in
  // https://medium.com/@name.
  const auto rest = value.substr(kScheme.size());
  if (rest.substr(0, rest.find_first_of("/?#")).contains('@'))
    return std::unexpected(OwnerValueError::UserInfo);
  if (!std::all_of(value.begin(), value.end(), url_char))
    return std::unexpected(OwnerValueError::Charset);
  return {};
}

bool same_owner(const OwnerSettings &a, const OwnerSettings &b) {
  return std::all_of(kOwnerKeys.begin(), kOwnerKeys.end(),
                     [&](const OwnerKeyInfo &info) {
                       return text_of(owner_field(a, info.key)) ==
                              text_of(owner_field(b, info.key));
                     });
}

void encode_owner(OwnerBlob &blob, const OwnerSettings &owner) {
  std::memset(&blob, 0, sizeof(blob));
  for (const auto &info : kOwnerKeys)
    store_text(owner_field(blob.payload, info.key),
               text_of(owner_field(owner, info.key)));
  std::copy(kOwnerMagic.begin(), kOwnerMagic.end(), blob.header.magic);
  put_le(blob.header.version, kOwnerVersion, sizeof(blob.header.version));
  put_le(blob.header.length, sizeof(OwnerPayload), sizeof(blob.header.length));
  put_le(blob.header.crc32,
         crc32(reinterpret_cast<const std::uint8_t *>(&blob.payload),
               sizeof(blob.payload)),
         sizeof(blob.header.crc32));
}

// Why a stored owner record was refused. Every case loads as empty.
enum class [[nodiscard]] OwnerDecodeError : std::uint8_t {
  Header, // size, magic, version or payload length is wrong
  Crc,    // the payload does not match its CRC
  Text,   // a field has no terminator or fails the SET_CONFIG rule
};

// Structure and CRC, then the same value rules as SET_CONFIG.
[[nodiscard]] std::expected<OwnerSettings, OwnerDecodeError>
decode_owner(const OwnerBlob &blob, std::size_t size) {
  if (size != sizeof(OwnerBlob) ||
      !std::equal(kOwnerMagic.begin(), kOwnerMagic.end(), blob.header.magic) ||
      get_le(blob.header.version, sizeof(blob.header.version)) !=
          kOwnerVersion ||
      get_le(blob.header.length, sizeof(blob.header.length)) !=
          sizeof(OwnerPayload))
    return std::unexpected(OwnerDecodeError::Header);
  if (get_le(blob.header.crc32, sizeof(blob.header.crc32)) !=
      crc32(reinterpret_cast<const std::uint8_t *>(&blob.payload),
            sizeof(blob.payload)))
    return std::unexpected(OwnerDecodeError::Crc);
  OwnerSettings decoded;
  for (const auto &info : kOwnerKeys) {
    const auto field = owner_field(blob.payload, info.key);
    if (std::find(field.begin(), field.end(), '\0') == field.end())
      return std::unexpected(OwnerDecodeError::Text);
    const auto value = text_of(field);
    if (!check_owner_value(info.key, value))
      return std::unexpected(OwnerDecodeError::Text);
    store_text(owner_field(decoded, info.key), value);
  }
  return decoded;
}

// One write and one commit, then a readback, like save_locked.
[[nodiscard]] bool save_owner(const OwnerSettings &owner) {
  nvs_handle_t store;
  if (nvs_open(kNamespace, NVS_READWRITE, &store) != ESP_OK)
    return false;
  encode_owner(owner_staged, owner);
  bool ok = committed(store, nvs_set_blob(store, kOwnerKey, &owner_staged,
                                          sizeof(owner_staged)));
  std::size_t length = sizeof(owner_readback);
  ok = ok &&
       nvs_get_blob(store, kOwnerKey, &owner_readback, &length) == ESP_OK &&
       length == sizeof(owner_staged) &&
       std::memcmp(&owner_readback, &owner_staged, sizeof(owner_staged)) == 0;
  nvs_close(store);
  return ok;
}

// Reads the record through an open handle. Never erases it.
OwnerSettings load_owner(nvs_handle_t store) {
  std::size_t size = sizeof(owner_readback);
  const auto result = nvs_get_blob(store, kOwnerKey, &owner_readback, &size);
  if (result == ESP_ERR_NVS_NOT_FOUND)
    return OwnerSettings{};
  if (result == ESP_OK) {
    const auto decoded = decode_owner(owner_readback, size);
    if (decoded)
      return *decoded;
  }
  oslog.println("CONFIG OWNER invalid-stored-profile=true");
  return OwnerSettings{};
}

void publish_owner(const OwnerSettings &owner) {
  portENTER_CRITICAL(&settings_mutex);
  owner_current = owner;
  portEXIT_CRITICAL(&settings_mutex);
}

// Settings and owner change together, so a sample never sees one half of a
// request.
void publish_with_owner(const Settings &settings, const OwnerSettings &owner) {
  portENTER_CRITICAL(&settings_mutex);
  current = settings;
  owner_current = owner;
  portEXIT_CRITICAL(&settings_mutex);
}

// A failed save may still have committed before its readback failed, so the
// previous value is written back, best effort. A record whose write back fails
// too stays unsure until a later save of it succeeds.
void restore_owner(const OwnerSettings &previous) {
  owner_unsure = !save_owner(previous);
  if (owner_unsure)
    oslog.println("CONFIG ERROR operation=owner-rollback");
}

// Saves the owner record first, then `cfg`. A record is written when the
// request changed it or when it is unsure. The two writes are not atomic
// across a power cut. When either save fails, every record this request wrote
// gets its previous value back, so flash never pairs one record from this
// request with the other from before it. On any failure the caller replies
// ERROR and publishes nothing, so the live snapshot never holds half a request.
[[nodiscard]] bool save_request(bool owner_changed,
                                const OwnerSettings &previous_owner,
                                const OwnerSettings &next_owner,
                                bool settings_touched, const Settings &previous,
                                const Settings &next) {
  if (!persistent)
    return true;
  const bool write_owner = owner_changed || owner_unsure;
  if (write_owner) {
    if (!save_owner(next_owner)) {
      restore_owner(previous_owner);
      return false;
    }
    owner_unsure = false;
  }
  if (settings_touched || cfg_unsure) {
    if (!save_cfg(next, previous)) {
      if (write_owner)
        restore_owner(previous_owner);
      return false;
    }
  }
  return true;
}

// Page 5 holds title, provider, url and thing.deployment_id, page 6 the
// description. At every maximum length with every character escaped, page 5
// is 458 bytes and page 6 is 348, both under the 480 byte CONFIG limit.
int build_owner_page(char *out, std::size_t size, std::uint8_t page) {
  const OwnerSettings owner = owner_settings();
  if (page == 6) {
    std::array<char, 2 * kOwnerDescriptionMax + 8> description{};
    json_escape(description.data(), description.size(),
                owner.description.data());
    return std::snprintf(out, size, "{\"owner\":{\"description\":\"%s\"}}",
                         description.data());
  }
  std::array<char, 2 * kOwnerTitleMax + 8> title{};
  std::array<char, 2 * kOwnerProviderMax + 8> provider{};
  std::array<char, 2 * kOwnerUrlMax + 8> url{};
  std::array<char, 2 * kDeploymentIdMax + 8> deployment{};
  json_escape(title.data(), title.size(), owner.title.data());
  json_escape(provider.data(), provider.size(), owner.provider.data());
  json_escape(url.data(), url.size(), owner.url.data());
  json_escape(deployment.data(), deployment.size(),
              owner.deployment.text.data());
  return std::snprintf(out, size,
                       "{\"owner\":{\"title\":\"%s\",\"provider\":\"%s\","
                       "\"url\":\"%s\"},\"thing\":{\"deployment_id\":\"%s\"}}",
                       title.data(), provider.data(), url.data(),
                       deployment.data());
}
} // namespace

std::optional<OwnerKey> owner_key(std::string_view key) {
  const auto info = std::find_if(
      kOwnerKeys.begin(), kOwnerKeys.end(),
      [key](const OwnerKeyInfo &entry) { return entry.name == key; });
  if (info == kOwnerKeys.end())
    return std::nullopt;
  return info->key;
}

std::expected<void, OwnerValueError> check_owner_value(OwnerKey key,
                                                       std::string_view value) {
  const auto info = std::find_if(
      kOwnerKeys.begin(), kOwnerKeys.end(),
      [key](const OwnerKeyInfo &entry) { return entry.key == key; });
  if (info == kOwnerKeys.end() || value.size() > info->max)
    return std::unexpected(OwnerValueError::TooLong);
  if (value.empty())
    return {};
  if (key == OwnerKey::Url)
    return check_url(value);
  if (key == OwnerKey::Deployment) {
    const bool allowed = std::all_of(value.begin(), value.end(), [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
    if (!allowed)
      return std::unexpected(OwnerValueError::Charset);
    // A letter or digit at each end. The footer writes "unknown" when the id
    // is unset, so a stored "unknown" could not be told apart from it.
    if (value.front() == '-' || value.back() == '-' || value == "unknown")
      return std::unexpected(OwnerValueError::Reserved);
    return {};
  }
  return check_text(value);
}

OwnerSettings owner_settings() {
  OwnerSettings copy;
  portENTER_CRITICAL(&settings_mutex);
  copy = owner_current;
  portEXIT_CRITICAL(&settings_mutex);
  return copy;
}

DeploymentId deployment_id() {
  DeploymentId copy;
  portENTER_CRITICAL(&settings_mutex);
  copy = owner_current.deployment;
  portEXIT_CRITICAL(&settings_mutex);
  return copy;
}

SampleLabels sample_labels() {
  SampleLabels copy;
  portENTER_CRITICAL(&settings_mutex);
  copy.location = current.location;
  copy.deployment = owner_current.deployment;
  portEXIT_CRITICAL(&settings_mutex);
  return copy;
}

bool same_deployment(const DeploymentId &a, const DeploymentId &b) {
  return text_of(a.text) == text_of(b.text);
}

void set_sensor_policy(const SensorPolicy &policy) { sensor_policy = policy; }

const char *pair_name(PairMode mode) {
  switch (mode) {
  case PairMode::Fixed:
    return "fixed";
  case PairMode::None:
    return "none";
  default:
    return "random";
  }
}

bool load(bool display_detected) {
  Settings settings;
  persistent = false;
  cfg_unsure = false;
  owner_unsure = false;
  nvs_handle_t store;
  if (nvs_open(kNamespace, NVS_READWRITE, &store) != ESP_OK) {
    oslog.println("CONFIG ERROR operation=nvs-open persistent=false");
    settings.display = display_detected;
    settings.pair = display_detected ? PairMode::Random : PairMode::Fixed;
    settings.pin = random_pin();
    esp_fill_random(settings.token, kTokenBytes);
    publish(settings);
    publish_owner(OwnerSettings{});
    booted = settings;
    return false;
  }
  std::size_t blob_size = sizeof(readback_blob);
  const auto blob_result =
      nvs_get_blob(store, kBlobKey, &readback_blob, &blob_size);
  const bool from_blob =
      blob_result == ESP_OK && blob_intact(readback_blob, blob_size);
  if (blob_result != ESP_ERR_NVS_NOT_FOUND && !from_blob)
    oslog.println("CONFIG BLOB invalid=true fallback=defaults");
  const bool first_boot = !from_blob;
  bool migrated_pin = false;
  if (first_boot) {
    settings.display = display_detected;
    settings.pair = display_detected ? PairMode::Random : PairMode::Fixed;
    settings.pin = random_pin();
    esp_fill_random(settings.token, kTokenBytes);
  } else {
    load_blob(readback_blob.payload, settings);
  }
  if (!first_boot && settings.pin == kFactoryDefaultPin) {
    settings.pin = random_pin();
    migrated_pin = true;
    oslog.println("CONFIG SECURITY legacy_pin_rotated=true");
  }
  // Owner keys have their own record. A `cfg` first boot keeps them.
  publish_owner(load_owner(store));
  nvs_close(store);
  persistent = true;
  // The live snapshot keeps the new values, so a failed seed leaves `cfg`
  // unsure and the next SET_CONFIG writes it.
  if ((first_boot || migrated_pin) && !save_locked(settings)) {
    cfg_unsure = true;
    oslog.println("CONFIG ERROR operation=nvs-seed-or-migrate");
  }
  publish(settings);
  booted = settings;
  oslog.printf("CONFIG LOADED first_boot=%s display=%s pair=%s pin_default=%s "
               "wifi_on=%s ssid_set=%s lan_on=%s\n",
               first_boot ? "true" : "false",
               settings.display ? "true" : "false", pair_name(settings.pair),
               settings.pin == kFactoryDefaultPin ? "true" : "false",
               settings.wifi_on ? "true" : "false",
               settings.ssid[0] ? "true" : "false",
               settings.lan_on ? "true" : "false");
  return true;
}

Settings get() {
  Settings copy;
  portENTER_CRITICAL(&settings_mutex);
  copy = current;
  portEXIT_CRITICAL(&settings_mutex);
  return copy;
}

LocationSettings location_settings() {
  LocationSettings copy;
  portENTER_CRITICAL(&settings_mutex);
  copy = current.location;
  portEXIT_CRITICAL(&settings_mutex);
  return copy;
}

bool reboot_required() {
  const Settings now = get();
  return now.pair != booted.pair || now.pin != booted.pin;
}

bool apply_lines(const char *text, std::size_t length, char *bad_key,
                 std::size_t bad_key_size, Actions &actions) {
  const Settings previous = get();
  Settings next = previous;
  const OwnerSettings previous_owner = owner_settings();
  OwnerSettings next_owner = previous_owner;
  bool settings_touched = false;
  Actions pending;
  bad_key[0] = '\0';
  std::size_t position = 0;
  bool any = false;
  bool sensor_identity_changed = false;
  bool sensor_candidate_explicit = false;
  while (position < length) {
    // One line: key '=' value, terminated by '\n' or end of text.
    std::size_t end = position;
    while (end < length && text[end] != '\n')
      ++end;
    std::size_t line_length = end - position;
    if (line_length && text[position + line_length - 1] == '\r')
      --line_length;
    if (line_length == 0) {
      position = end + 1;
      continue;
    }
    if (std::memchr(text + position, '\0', line_length)) {
      std::snprintf(bad_key, bad_key_size, "malformed");
      return false;
    }
    char line[kNtpServerMax + 32];
    if (line_length >= sizeof(line)) {
      std::snprintf(bad_key, bad_key_size, "line-too-long");
      return false;
    }
    std::memcpy(line, text + position, line_length);
    line[line_length] = '\0';
    position = end + 1;
    char *equals = std::strchr(line, '=');
    if (!equals) {
      std::snprintf(bad_key, bad_key_size, "%s", line);
      return false;
    }
    *equals = '\0';
    const char *key = line;
    const char *value = equals + 1;
    auto reject = [&]() {
      std::snprintf(bad_key, bad_key_size, "%s", key);
      return false;
    };
    any = true;
    // Owner keys (#148) are checked here with the rest of the request and
    // stored in their own record.
    if (const auto owner = owner_key(key)) {
      if (!check_owner_value(*owner, value))
        return reject();
      store_text(owner_field(next_owner, *owner), value);
      continue;
    }
    settings_touched = true;
    if (std::strcmp(key, "ble.pair") == 0) {
      PairMode mode;
      if (std::strcmp(value, "random") == 0)
        mode = PairMode::Random;
      else if (std::strcmp(value, "fixed") == 0)
        mode = PairMode::Fixed;
      else if (std::strcmp(value, "none") == 0)
        mode = PairMode::None;
      else
        return reject();
      pending.ble_changed = pending.ble_changed || mode != next.pair;
      next.pair = mode;
    } else if (std::strcmp(key, "ble.pin") == 0) {
      std::uint32_t pin;
      if (!parse_pin(value, pin))
        return reject();
      pending.ble_changed = pending.ble_changed || pin != next.pin;
      next.pin = pin;
    } else if (std::strcmp(key, "ble.clear_bonds") == 0) {
      bool flag;
      if (!parse_bool(value, flag))
        return reject();
      pending.clear_bonds = flag;
    } else if (std::strcmp(key, "wifi.on") == 0) {
      bool flag;
      if (!parse_bool(value, flag))
        return reject();
      pending.wifi_changed = pending.wifi_changed || flag != next.wifi_on;
      next.wifi_on = flag;
    } else if (std::strcmp(key, "wifi.ssid") == 0) {
      if (std::strlen(value) > kSsidMax || !printable(value))
        return reject();
      pending.wifi_changed =
          pending.wifi_changed || std::strcmp(value, next.ssid) != 0;
      std::snprintf(next.ssid, sizeof(next.ssid), "%s", value);
    } else if (std::strcmp(key, "wifi.psk") == 0) {
      const std::size_t psk_length = std::strlen(value);
      if (psk_length > kPskMax || (psk_length && psk_length < 8) ||
          !printable(value))
        return reject();
      pending.wifi_changed =
          pending.wifi_changed || std::strcmp(value, next.psk) != 0;
      std::snprintf(next.psk, sizeof(next.psk), "%s", value);
    } else if (std::strcmp(key, "lan.on") == 0) {
      bool flag;
      if (!parse_bool(value, flag))
        return reject();
      pending.wifi_changed = pending.wifi_changed || flag != next.lan_on;
      next.lan_on = flag;
    } else if (std::strcmp(key, "lan.rotate_token") == 0) {
      bool flag;
      if (!parse_bool(value, flag))
        return reject();
      pending.rotate_token = flag;
    } else if (std::strcmp(key, "ntp.on") == 0) {
      if (!parse_bool(value, next.ntp.enabled))
        return reject();
    } else if (std::strcmp(key, "ntp.dhcp") == 0) {
      if (!parse_bool(value, next.ntp.dhcp))
        return reject();
    } else if (std::strcmp(key, "ntp.public_fallback") == 0) {
      if (!parse_bool(value, next.ntp.public_fallback))
        return reject();
    } else if (std::strcmp(key, "ntp.interval_s") == 0) {
      if (!parse_uint(value, 60, 86400, next.ntp.interval_s))
        return reject();
    } else if (std::strcmp(key, "ntp.server1") == 0 ||
               std::strcmp(key, "ntp.server2") == 0) {
      if (!valid_server(value))
        return reject();
      const std::size_t index = std::strcmp(key, "ntp.server1") == 0 ? 0 : 1;
      std::snprintf(next.ntp.servers[index], sizeof(next.ntp.servers[index]),
                    "%s", value);
    } else if (std::strcmp(key, "location.cell") == 0) {
      next.location.cell = 0;
      if (*value &&
          !opensensor::location::parse_cell(value, next.location.cell))
        return reject();
    } else if (std::strcmp(key, "location.resolution") == 0) {
      std::uint32_t resolution;
      if (!parse_uint(value, 0, 15, resolution))
        return reject();
      next.location.resolution = static_cast<std::uint8_t>(resolution);
    } else if (std::strcmp(key, "location.country") == 0) {
      if (*value && !opensensor::location::valid_country(value))
        return reject();
      std::snprintf(next.location.country, sizeof(next.location.country), "%s",
                    value);
    } else if (std::strcmp(key, "thing.mobility") == 0) {
      if (!parse_mobility(value, next.location.mobility))
        return reject();
    } else if (std::strcmp(key, "sensor.vendor") == 0) {
      if (std::strlen(value) >= sizeof(next.sensor_vendor) ||
          (value[0] && std::strcmp(value, hardware_vendor()) != 0))
        return reject();
      sensor_identity_changed = sensor_identity_changed ||
                                std::strcmp(value, next.sensor_vendor) != 0;
      std::snprintf(next.sensor_vendor, sizeof(next.sensor_vendor), "%s",
                    value);
    } else if (std::strcmp(key, "sensor.model") == 0) {
      if (std::strlen(value) >= sizeof(next.sensor_model) ||
          (value[0] && std::strcmp(value, hardware_model()) != 0))
        return reject();
      sensor_identity_changed =
          sensor_identity_changed || std::strcmp(value, next.sensor_model) != 0;
      std::snprintf(next.sensor_model, sizeof(next.sensor_model), "%s", value);
    } else if (std::strcmp(key, "sensor.serial") == 0) {
      if (!sensor_serial_chars(value))
        return reject();
      sensor_identity_changed = sensor_identity_changed ||
                                std::strcmp(value, next.sensor_serial) != 0;
      std::snprintf(next.sensor_serial, sizeof(next.sensor_serial), "%s",
                    value);
    } else if (std::strcmp(key, "sensor.batch_candidate") == 0) {
      bool flag;
      if (!parse_bool(value, flag))
        return reject();
      sensor_candidate_explicit = true;
      next.sensor_batch_candidate = flag;
    } else {
      return reject();
    }
  }
  if (!any) {
    std::snprintf(bad_key, bad_key_size, "empty");
    return false;
  }
  if (next.wifi_on && !next.ssid[0]) {
    std::snprintf(bad_key, bad_key_size, "wifi.ssid");
    return false;
  }
  if (sensor_identity_changed && !sensor_candidate_explicit)
    next.sensor_batch_candidate = false;
  if ((next.sensor_vendor[0] == '\0') != (next.sensor_model[0] == '\0') ||
      (next.sensor_serial[0] && !next.sensor_vendor[0])) {
    std::snprintf(bad_key, bad_key_size, "sensor.identity");
    return false;
  }
  if (!serial_allowed(next.sensor_serial)) {
    std::snprintf(bad_key, bad_key_size, "sensor.serial");
    return false;
  }
  if (next.sensor_batch_candidate && !batch_allowed(next)) {
    std::snprintf(bad_key, bad_key_size, "sensor.batch_candidate");
    return false;
  }
  if (next.location.cell &&
      !opensensor::location::coarsen(
          next.location.cell, next.location.resolution, next.location.cell)) {
    std::snprintf(bad_key, bad_key_size, "location.cell");
    return false;
  }
  pending.ntp_changed = !same_ntp(previous.ntp, next.ntp);
  pending.location_changed = !same_location(previous.location, next.location);
  if (pending.rotate_token)
    esp_fill_random(next.token, kTokenBytes);
  if (!save_request(!same_owner(previous_owner, next_owner), previous_owner,
                    next_owner, settings_touched, previous, next)) {
    std::snprintf(bad_key, bad_key_size, "nvs");
    return false;
  }
  publish_with_owner(next, next_owner);
  actions = pending;
  oslog.printf(
      "CONFIG SET pair=%s pin_default=%s wifi_on=%s ssid_set=%s "
      "lan_on=%s clear_bonds=%s rotate_token=%s reboot_required=%s\n",
      pair_name(next.pair), next.pin == kFactoryDefaultPin ? "true" : "false",
      next.wifi_on ? "true" : "false", next.ssid[0] ? "true" : "false",
      next.lan_on ? "true" : "false", pending.clear_bonds ? "true" : "false",
      pending.rotate_token ? "true" : "false",
      reboot_required() ? "true" : "false");
  return true;
}

std::size_t build_json(char *out, std::size_t size, const WifiView &wifi) {
  const Settings settings = get();
  char ssid[2 * kSsidMax + 8];
  json_escape(ssid, sizeof(ssid), settings.ssid);
  const int written = std::snprintf(
      out, size,
      "{\"ble\":{\"pair\":\"%s\",\"pin_set\":%u,\"pin_default\":%u,\"bonds\":%"
      "u,"
      "\"display\":%u},\"wifi\":{\"on\":%u,\"ssid\":\"%s\",\"psk_set\":%u,"
      "\"state\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"mac\":\"%s\"},"
      "\"lan\":{\"on\":%u,\"port\":%u,\"host\":\"%s\",\"clients\":%u},"
      "\"sensor\":{\"vendor\":\"%s\",\"model\":\"%s\",\"serial\":\"%s\","
      "\"batch_candidate\":%u}}",
      pair_name(settings.pair), settings.pin <= 999999U ? 1U : 0U,
      settings.pin == kFactoryDefaultPin ? 1U : 0U, wifi.bonds,
      settings.display ? 1U : 0U, settings.wifi_on ? 1U : 0U, ssid,
      settings.psk[0] ? 1U : 0U, wifi.state, wifi.ip, wifi.rssi, wifi.mac,
      settings.lan_on ? 1U : 0U, unsigned(kLanPort), wifi.host, wifi.clients,
      settings.sensor_vendor, settings.sensor_model, settings.sensor_serial,
      settings.sensor_batch_candidate ? 1U : 0U);
  return written > 0 && std::size_t(written) < size ? std::size_t(written) : 0;
}
bool same_location(const LocationSettings &a, const LocationSettings &b) {
  return a.cell == b.cell && a.resolution == b.resolution &&
         std::strcmp(a.country, b.country) == 0 && a.mobility == b.mobility;
}

const char *mobility_name(Mobility mobility) {
  return mobility == Mobility::Moving ? "moving" : "fixed";
}

std::size_t build_page(char *out, std::size_t size, std::uint8_t page,
                       const PageInputs &inputs) {
  const Settings settings = get();
  int written = -1;
  if (page == 1) {
    char cell[opensensor::location::kCellTextBytes]{};
    char center[128] = "\"cell_resolution\":null,\"lat\":null,\"lon\":null";
    double latitude, longitude;
    if (settings.location.cell &&
        opensensor::location::format_cell(settings.location.cell, cell,
                                          sizeof(cell)) &&
        opensensor::location::center_degrees(settings.location.cell, latitude,
                                             longitude))
      std::snprintf(center, sizeof(center),
                    "\"cell_resolution\":%d,\"lat\":%.7f,\"lon\":%.7f",
                    opensensor::location::resolution(settings.location.cell),
                    latitude, longitude);
    // Present only on a board with a receiver, "" without a fresh cell.
    char gnss[sizeof(",\"gnss_cell\":\"\"") +
              opensensor::location::kCellTextBytes]{};
    if (inputs.gnss) {
      char text[opensensor::location::kCellTextBytes]{};
      if (inputs.gnss_cell && !opensensor::location::format_cell(
                                  inputs.gnss_cell, text, sizeof(text)))
        text[0] = '\0';
      std::snprintf(gnss, sizeof(gnss), ",\"gnss_cell\":\"%s\"", text);
    }
    written = std::snprintf(
        out, size,
        "{\"location\":{\"cell\":\"%s\",\"resolution\":%u,%s,\"country\":"
        "\"%s\"%s},\"thing\":{\"mobility\":\"%s\",\"setup\":%u}}",
        cell, unsigned(settings.location.resolution), center,
        settings.location.country, gnss,
        mobility_name(settings.location.mobility),
        setup_needed(settings.location) ? 1U : 0U);
  } else if (page == 2) {
    written = std::snprintf(
        out, size,
        "{\"ntp\":{\"on\":%u,\"dhcp\":%u,\"public_fallback\":%u,\"interval_s\":"
        "%lu}}",
        settings.ntp.enabled ? 1U : 0U, settings.ntp.dhcp ? 1U : 0U,
        settings.ntp.public_fallback ? 1U : 0U,
        static_cast<unsigned long>(settings.ntp.interval_s));
  } else if (page == 3 || page == 4) {
    written = std::snprintf(out, size, "{\"ntp\":{\"server%u\":\"%s\"}}",
                            unsigned(page - 2), settings.ntp.servers[page - 3]);
  } else if (page == 5 || page == 6) {
    written = build_owner_page(out, size, page);
  }
  if (written <= 0 || static_cast<std::size_t>(written) >= size) {
    if (size)
      out[0] = '\0';
    return 0;
  }
  return static_cast<std::size_t>(written);
}
} // namespace config
