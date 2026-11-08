#pragma once

// Owner-editable device settings, protocol v2
// (docs/shared/ble-sync-protocol.md, "Device configuration"). Stored in the NVS
// namespace `oscfg`, separate from the `parquet` namespace that holds the
// Thing identity. Loaded once at boot on the main task; later reads and
// writes come from the storage worker (the only task that executes control
// requests), so the copy handed out by get() is guarded by a spinlock and
// everything else is single-writer.

#include <cstddef>
#include <cstdint>

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
  std::uint8_t resolution = 8; // Maximum permitted precision, not an upsample.
  char country[3]{};           // Optional owner-declared ISO 3166-1 alpha-2.
  Mobility mobility = Mobility::Fixed;
};

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

// Replaces the LAN token with fresh random bytes and saves.
bool rotate_token();

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
// 1 = location; 2 = NTP policy; 3/4 = one configured NTP server.
std::size_t build_page(char *out, std::size_t size, std::uint8_t page,
                       const PageInputs &inputs = {});
bool same_location(const LocationSettings &, const LocationSettings &);

const char *pair_name(PairMode mode);
// "fixed" or "moving", the SET_CONFIG and footer word.
const char *mobility_name(Mobility mobility);
} // namespace config
