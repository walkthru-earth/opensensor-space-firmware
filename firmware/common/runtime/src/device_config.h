#pragma once

// Owner-editable device settings, protocol v2
// (docs/shared/ble-sync-protocol.md, "Device configuration"). Stored in the NVS
// namespace `oscfg`, separate from the `parquet` namespace that holds the
// Thing identity. Loaded once at boot on the main task; later reads and
// writes come from the storage worker (the only task that executes control
// requests), so the copy handed out by get() is guarded by a spinlock and
// everything else is single-writer.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace config {
enum class PairMode : std::uint8_t { Random = 0, Fixed = 1, None = 2 };
// The factory default PIN that older images shipped. A saved record that still
// holds it is rotated on load. New devices receive a random per-device fixed
// PIN.
constexpr std::uint32_t kFactoryDefaultPin = 123456;
constexpr std::uint16_t kLanPort = 47390;
constexpr std::size_t kTokenBytes = 32;
constexpr std::size_t kSsidMax = 32;
constexpr std::size_t kPskMax = 63;
constexpr std::size_t kSensorSerialMax = 32;
constexpr std::size_t kNtpServerMax = 253;
constexpr std::size_t kNtpServerCount = 2;

struct NtpSettings {
  bool enabled = true;
  bool dhcp = true;
  bool public_fallback = true;
  std::uint32_t interval_s = 3600;
  char servers[kNtpServerCount][kNtpServerMax + 1]{};
};
// How the Thing moves, `thing.mobility`. The footer `thing.mobility` and the
// STAC `opensensor:mobility` carry the same word (#83, decision 31).
enum class Mobility : std::uint8_t { Fixed = 0, Moving = 1 };
struct LocationSettings {
  std::uint64_t cell = 0; // Only the published cell is retained, zero = unset.
  std::uint8_t resolution = 8; // Always 8, the published H3 resolution (#139).
  char country[3]{};           // Optional owner-declared ISO 3166-1 alpha-2.
  Mobility mobility = Mobility::Fixed;
  // location.phone_cell (#140), the res 8 cell of the phone's own fix, zero
  // when unset. RAM only. The `cfg` record never holds it, so a reboot clears
  // it. While it is set every row carries it with location_source 3.
  std::uint64_t phone_cell = 0;
};

// The cell a row carries when GNSS does not decide it. The phone cell while it
// is set, else the configured cell. A fixed Thing splits its file when this
// changes. A moving Thing never splits on it (#140).
[[nodiscard]] constexpr std::uint64_t
effective_cell(const LocationSettings &location) {
  return location.phone_cell ? location.phone_cell : location.cell;
}

// The Thing needs setup (#147). A fixed Thing without a cell needs one, a
// moving Thing never does. Derived on every read, never stored. Drives the
// advert `setup` bit, GET_CONFIG page 1 `thing.setup` and the CoreS3 screen.
[[nodiscard]] constexpr bool setup_needed(const LocationSettings &location) {
  return location.mobility == Mobility::Fixed && location.cell == 0;
}

struct Settings {
  PairMode pair = PairMode::Random;
  std::uint32_t pin = 0;
  bool display = true; // detected at first boot, then frozen
  bool wifi_on = false;
  char ssid[kSsidMax + 1]{};
  char psk[kPskMax + 1]{};
  bool lan_on = true;
  std::uint8_t token[kTokenBytes]{};
  char sensor_vendor[16]{};
  char sensor_model[16]{};
  char sensor_serial[kSensorSerialMax + 1]{};
  bool sensor_batch_candidate = false;
  NtpSettings ntp;
  LocationSettings location;
};

// One-shot actions requested through SET_CONFIG; not stored.
struct Actions {
  bool clear_bonds = false;
  bool rotate_token = false;
  bool wifi_changed = false; // ssid/psk/on changed: LAN task must reapply
  bool ble_changed = false;  // pair/pin changed: takes effect at next boot
  bool ntp_changed = false;
  bool location_changed = false;
};

// Loads settings, seeding defaults on first boot from `display_detected`
// (screen -> random passkey, no screen -> random per-device fixed PIN) and
// generating the LAN token. Legacy universal PINs are rotated on load. Returns
// false when NVS was unusable, in which
// case defaults are used and nothing persists.
bool load(bool display_detected);

// Board-owned sensor identity rules. The runtime stores whatever identity the
// owner provisions, but only the board knows which vendor and model are wired,
// what a valid sticker serial looks like and which batches the board can
// compute a candidate for. Strings and callbacks must have static lifetime.
// A null `valid_serial` refuses any non-empty serial and a null
// `batch_eligible` refuses the batch opt-in. Vendor and model are compared
// exactly. An unset policy therefore refuses all sensor identity provisioning.
struct SensorPolicy {
  const char *vendor = nullptr;
  const char *model = nullptr;
  bool (*valid_serial)(const char *serial) = nullptr;
  bool (*batch_eligible)(const char *vendor, const char *model,
                         const char *serial) = nullptr;
};

// Board adapter installs its policy before load/start_links.
void set_sensor_policy(const SensorPolicy &policy);

Settings get();
// Only the location settings, for tasks that must not copy credentials.
LocationSettings location_settings();
// Runtime view for JSON/UI: whether a `ble.*` change is waiting for a reboot.
bool reboot_required();

// Parses `key=value` lines (see the protocol table), validates all of them,
// then applies and saves. On failure nothing changes and `bad_key` names the
// culprit. `actions` reports what the caller must do next.
bool apply_lines(const char *text, std::size_t length, char *bad_key,
                 std::size_t bad_key_size, Actions &actions);

struct WifiView {
  const char *state; // off | connecting | connected | failed
  const char *ip;    // dotted quad or ""
  int rssi;
  const char *mac;
  unsigned clients;
  const char *host; // mDNS host label
  unsigned bonds;
};
// Builds the CONFIG JSON (<= 480 bytes). Never includes the PSK or token.
std::size_t build_json(char *out, std::size_t size, const WifiView &wifi);
// Board readings page 1 shows beside the stored settings, never stored.
struct PageInputs {
  bool gnss = false; // the board has a receiver, adds location.gnss_cell
  std::uint64_t gnss_cell = 0; // fresh res 8 cell, zero prints ""
};
// Optional GET_CONFIG pages keep the legacy document and 480-byte cap intact.
// 1 = location; 2 = NTP policy; 3/4 = one configured NTP server; 5 = owner
// title, provider, url and thing.deployment_id; 6 = owner description.
std::size_t build_page(char *out, std::size_t size, std::uint8_t page,
                       const PageInputs &inputs = {});
// True when two samples may share a file. Cell, resolution, country and
// mobility must match. A fixed Thing also needs the same effective_cell(), so a
// phone cell change splits it. A moving Thing ignores the phone cell, since
// one moving file holds many cells (#140).
bool same_location(const LocationSettings &, const LocationSettings &);

// Owner site configuration (#148). Free text the owner sets over SET_CONFIG for
// the STAC Collection and the footer, kept in its own CRC-checked NVS record
// `owner` beside `cfg`. Values are bounded UTF-8 without control characters.
// An empty value clears the key. The board never prints these values, they
// appear only in GET_CONFIG pages 5 and 6 and in the footer.
constexpr std::size_t kOwnerTitleMax = 64;
constexpr std::size_t kOwnerDescriptionMax = 160;
constexpr std::size_t kOwnerProviderMax = 64;
constexpr std::size_t kOwnerUrlMax = 96;
constexpr std::size_t kDeploymentIdMax = 32;
// The highest GET_CONFIG page. Pages 5 and 6 hold the owner keys.
constexpr std::uint8_t kLastConfigPage = 6;

enum class OwnerKey : std::uint8_t {
  Title,       // owner.title, the Collection title
  Description, // owner.description
  Provider,    // owner.provider, the producer and licensor provider name
  Url,         // owner.url, https only
  Deployment,  // thing.deployment_id, footer thing.deployment_id
};
// Why one owner value was refused. The request reply names the key only.
enum class [[nodiscard]] OwnerValueError : std::uint8_t {
  TooLong,  // more bytes than the key allows
  Encoding, // not valid UTF-8
  Control,  // a C0 or C1 control character or DEL
  Scheme,   // owner.url without https:// and a host that starts with a
            // letter or digit
  UserInfo, // owner.url with an @ after https:// and before the first /,
            // ? or # or the end
  Charset,  // a byte outside the key's character set
  Reserved, // thing.deployment_id with a hyphen at an end, or "unknown"
};

// Footer thing.deployment_id as the owner set it, empty when unset. Each
// sample carries a copy so a change splits the file and never relabels rows
// already buffered.
struct DeploymentId {
  std::array<char, kDeploymentIdMax + 1> text{};
};

struct OwnerSettings {
  std::array<char, kOwnerTitleMax + 1> title{};
  std::array<char, kOwnerDescriptionMax + 1> description{};
  std::array<char, kOwnerProviderMax + 1> provider{};
  std::array<char, kOwnerUrlMax + 1> url{};
  DeploymentId deployment{};
};

// The SET_CONFIG key for an owner field, or nullopt for any other key.
[[nodiscard]] std::optional<OwnerKey> owner_key(std::string_view key);
// The rules one SET_CONFIG value must pass, also applied to a stored record.
[[nodiscard]] std::expected<void, OwnerValueError>
check_owner_value(OwnerKey key, std::string_view value);
// Copies of the published owner settings, safe from any task.
OwnerSettings owner_settings();
DeploymentId deployment_id();
// The location and deployment id a sample carries, copied under one lock so a
// row never pairs one request's location with another's deployment id.
struct SampleLabels {
  LocationSettings location;
  DeploymentId deployment;
};
SampleLabels sample_labels();
// True when both name the same deployment, the file split rule.
bool same_deployment(const DeploymentId &, const DeploymentId &);

const char *pair_name(PairMode mode);
// "fixed" or "moving", the SET_CONFIG and footer word.
const char *mobility_name(Mobility mobility);
} // namespace config
