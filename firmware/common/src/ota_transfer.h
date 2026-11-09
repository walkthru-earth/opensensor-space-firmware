#pragma once

// Authenticated chunked LAN OTA, the board neutral rules (#40).
//
// Pure. The ESP-IDF OTA calls sit behind the borrowed Sink callbacks, so the
// host test can fake flash. The device glue lives in
// firmware/common/connectivity/src/ota_lan.cpp. The wire format is in
// docs/shared/ble-sync-protocol.md, section "LAN OTA".
//
// One upload at a time, owned by the LAN session that sent OTA_BEGIN.
// The rules.
//   - OTA_BEGIN declares the image size and its SHA-256. The size must be at
//     least kHeaderBytes and at most the inactive slot.
//   - OTA_WRITE chunks arrive in order with no gap and no overlap, and never
//     run past the declared size.
//   - The first chunk carries the whole image and app description header. Its
//     magic words, chip id and project name must match the running app, so a
//     CoreS3 image never lands on a Waveshare board. This is checked before
//     the first byte reaches flash.
//   - OTA_END needs every declared byte. ESP-IDF verifies the image first,
//     then the SHA-256 read back from flash must equal the declared one, then
//     the boot partition moves to the new slot.
//   - An owner that sends nothing for kStallMs while Receiving, or whose
//     session closed, loses the upload, so it cannot block other sessions.
//   - Any rule broken by the owner aborts the upload and returns to Idle.
//   - Nothing here touches NVS or the SD archive. Reboot is a separate REBOOT
//     request.

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace opensensor::ota {

// Request opcodes, phone to device. ble_sync.h re-exports them as ble::Op.
inline constexpr std::uint8_t kOpBegin = 0x12;
inline constexpr std::uint8_t kOpWrite = 0x13;
inline constexpr std::uint8_t kOpEnd = 0x14;
inline constexpr std::uint8_t kOpCancel = 0x15;
// Reply frame, device to phone. Every successful OTA request answers it.
inline constexpr std::uint8_t kFrameState = 0x50;

inline constexpr std::size_t kDigestBytes = 32;
// The largest control request the transports carry (ble::kMaxControlBytes).
inline constexpr std::size_t kMaxRequestBytes = 512;
// OTA_WRITE is op, offset u32 LE, then data.
inline constexpr std::size_t kWriteHeaderBytes = 1 + 4;
inline constexpr std::size_t kMaxChunk = kMaxRequestBytes - kWriteHeaderBytes;
// OTA_BEGIN body is size u32 LE, then the SHA-256.
inline constexpr std::size_t kBeginBodyBytes = 4 + kDigestBytes;
// STATE frame, type, phase, next offset u32, size u32, slot u32, max chunk u16.
inline constexpr std::size_t kStateBytes = 1 + 1 + 4 + 4 + 4 + 2;

// ESP-IDF image layout (esp_app_format/include/esp_app_format.h and
// esp_app_desc.h). esp_image_header_t is 24 bytes and the first segment
// header 8 more, so esp_app_desc_t starts at 32. esp_ota_ops.c reads the
// description at the same place in esp_ota_get_partition_description().
inline constexpr std::uint8_t kImageMagic = 0xE9;
inline constexpr std::size_t kChipIdOffset = 12;
inline constexpr std::size_t kAppDescOffset = 24 + 8;
inline constexpr std::uint32_t kAppDescMagic = 0xABCD5432;
inline constexpr std::size_t kProjectNameOffset = kAppDescOffset + 48;
inline constexpr std::size_t kProjectNameBytes = 32;
inline constexpr std::size_t kHeaderBytes =
    kProjectNameOffset + kProjectNameBytes;
static_assert(kHeaderBytes <= kMaxChunk, "first chunk must hold the header");

// Where a control request goes. OTA ops run only on an authenticated LAN
// session. BLE gets ERROR not-on-this-link, since 1.6 MB over BLE would hold
// the storage worker for minutes.
enum class Route : std::uint8_t { NotOta, LanOnly, Handle };
[[nodiscard]] constexpr bool is_ota_op(std::uint8_t op) {
  return op == kOpBegin || op == kOpWrite || op == kOpEnd || op == kOpCancel;
}
[[nodiscard]] constexpr Route route(std::uint8_t op, bool on_lan) {
  if (!is_ota_op(op))
    return Route::NotOta;
  return on_lan ? Route::Handle : Route::LanOnly;
}

// An owner silent this long while Receiving loses the upload. The host sends
// the next chunk as soon as the last STATE arrives, so this is far above any
// real gap, and well below the 300 s LAN idle close.
inline constexpr std::int64_t kStallMs = 30000;
enum class Keep : std::uint8_t { Keep, SessionClosed, Stalled };
// Whether the Receiving owner keeps its upload. `idle_ms` is the time since
// its last OTA request.
[[nodiscard]] constexpr Keep keep_owner(bool session_open,
                                        std::int64_t idle_ms) {
  if (!session_open)
    return Keep::SessionClosed;
  if (idle_ms >= kStallMs)
    return Keep::Stalled;
  return Keep::Keep;
}

using Digest = std::array<std::uint8_t, kDigestBytes>;

// The running app this upload must match.
struct Target {
  std::uint32_t slot_bytes = 0; // size of the inactive OTA partition
  std::uint16_t chip_id = 0;    // esp_chip_id_t, 0x0009 on ESP32-S3
  // esp_app_desc_t::project_name of the running app, without its NUL.
  std::span<const char> project_name{};
};

enum class Phase : std::uint8_t {
  Idle = 0,
  Receiving = 1,
  Ready = 2, // the boot partition points at the new image, reboot pending
};

struct Progress {
  Phase phase = Phase::Idle;
  std::uint32_t next_offset = 0;
  std::uint32_t size = 0;
  std::uint32_t slot_bytes = 0;
};

enum class Error : std::uint8_t {
  Malformed,     // request body has the wrong length
  UnknownOp,     // not an OTA opcode
  NoTarget,      // no inactive slot or no project name
  Busy,          // another session owns the upload
  RebootPending, // an image is already staged, REBOOT or OTA_CANCEL first
  NotActive,     // OTA_WRITE or OTA_END without OTA_BEGIN
  Size,          // declared size is below kHeaderBytes or above the slot
  Offset,        // chunk is not contiguous with the bytes already written
  Overrun,       // chunk runs past the declared size
  Header,        // image or app description magic is wrong
  Chip,          // image is built for another chip
  Project,       // image is built for another project
  Incomplete,    // OTA_END before every declared byte arrived
  Digest,        // SHA-256 read back from flash differs from the declared one
  Flash,         // the sink could not begin, write or read back
  Verify,        // esp_ota_end rejected the image
  Activate,      // the boot partition could not be set or restored
};

// Error to the wire ERROR frame. Codes match ble::Error, checked in ota_lan.
struct WireError {
  std::uint8_t code = 0;
  const char *detail = nullptr;
};
[[nodiscard]] WireError wire_error(Error error);

enum class SinkError : std::uint8_t { Failed };

// Borrowed device callbacks. Each one is required.
struct Sink {
  void *context = nullptr;
  // esp_ota_begin on the inactive slot.
  std::expected<void, SinkError> (*begin)(void *context,
                                          std::uint32_t size) = nullptr;
  // esp_ota_write of the next contiguous bytes.
  std::expected<void, SinkError> (*write)(
      void *context, std::span<const std::uint8_t> data) = nullptr;
  // SHA-256 of the first `size` bytes of the slot, read back from flash.
  std::expected<Digest, SinkError> (*digest)(void *context,
                                             std::uint32_t size) = nullptr;
  // esp_ota_end, which also verifies the image.
  std::expected<void, SinkError> (*finish)(void *context) = nullptr;
  // esp_ota_set_boot_partition on the new slot.
  std::expected<void, SinkError> (*activate)(void *context) = nullptr;
  // esp_ota_set_boot_partition back on the running slot.
  std::expected<void, SinkError> (*revert)(void *context) = nullptr;
  // esp_ota_abort. Safe to call with no open handle.
  void (*abort)(void *context) = nullptr;
};

// The upload owner. Any value that changes when the LAN session changes, for
// example the peer slot in the high bits and its connection generation below.
using Owner = std::uint64_t;

// The LAN owner layout ota_lan.cpp uses, peer slot in the high 32 bits and
// the session's connection generation in the low 32.
[[nodiscard]] constexpr Owner lan_owner(std::uint8_t peer,
                                        std::uint32_t generation) {
  return (Owner{peer} << 32) | generation;
}
[[nodiscard]] constexpr std::uint8_t owner_peer(Owner owner) {
  return static_cast<std::uint8_t>(owner >> 32);
}

// The owner peer slot's connection generation now, as stale_action() takes
// it. `generation_of` is the slot lookup, lan::connection_generation on the
// device. nullopt when the slot is at or past `slots`, and the lookup is never
// called for such a slot.
template <std::invocable<std::uint8_t> Lookup>
[[nodiscard]] constexpr std::optional<std::uint32_t>
owner_generation(Owner owner, std::uint8_t slots, Lookup &&generation_of) {
  const std::uint8_t peer = owner_peer(owner);
  if (peer >= slots)
    return std::nullopt;
  return static_cast<std::uint32_t>(generation_of(peer));
}

// Whether the Receiving upload of a lan_owner() keeps going, checked before
// each OTA request. `generation` is the owner peer slot's connection
// generation now, or nullopt when the slot is out of range. The session is
// open only while it still equals the owner's. `seen_us` is the time of the
// owner's last OTA request, from touch_seen(). Any other phase keeps.
[[nodiscard]] constexpr Keep
stale_action(const Progress &progress, Owner owner,
             std::optional<std::uint32_t> generation, std::int64_t now_us,
             std::int64_t seen_us) {
  if (progress.phase != Phase::Receiving)
    return Keep::Keep;
  const bool open = generation.has_value() &&
                    *generation == static_cast<std::uint32_t>(owner);
  return keep_owner(open, (now_us - seen_us) / 1000);
}

// The owner's last seen time after `requester` sent an OTA request at
// `now_us`. It moves only when the requester owns a Receiving upload after
// the request, so a refused session never refreshes another owner's upload.
[[nodiscard]] constexpr std::int64_t
touch_seen(const Progress &after, Owner current_owner, Owner requester,
           std::int64_t now_us, std::int64_t seen_us) {
  return after.phase == Phase::Receiving && current_owner == requester
             ? now_us
             : seen_us;
}

class Transfer {
public:
  Transfer(const Target &target, const Sink &sink)
      : target_(target), sink_(sink) {}

  // Decode and run one OTA request, op byte first. A rule broken by the
  // owner aborts the upload. A request from another session never disturbs
  // the owner's upload.
  [[nodiscard]] std::expected<Progress, Error>
  handle(std::span<const std::uint8_t> request, Owner owner);

  // Drop an upload whose owner is gone. Returns true when one was aborted.
  // A staged image (Ready) is kept, since its bytes are already verified.
  bool drop_owner(Owner owner);

  [[nodiscard]] Progress progress() const;
  [[nodiscard]] bool active() const { return phase_ != Phase::Idle; }
  [[nodiscard]] Owner owner() const { return owner_; }

private:
  std::expected<Progress, Error> begin(std::span<const std::uint8_t> body,
                                       Owner owner);
  std::expected<Progress, Error> write(std::span<const std::uint8_t> body);
  std::expected<Progress, Error> finish();
  std::expected<Progress, Error> cancel();
  [[nodiscard]] std::expected<void, Error>
  check_header(std::span<const std::uint8_t> first) const;
  Error fail(Error error);

  Target target_;
  Sink sink_;
  Phase phase_ = Phase::Idle;
  Owner owner_ = 0;
  std::uint32_t size_ = 0;
  std::uint32_t next_ = 0;
  Digest declared_{};
};

// Writes the STATE frame. Returns kStateBytes.
std::size_t encode_state(const Progress &progress,
                         std::span<std::uint8_t, kStateBytes> out);

} // namespace opensensor::ota
