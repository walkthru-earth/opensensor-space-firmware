#pragma once

// Bluetooth LE sync service, protocol v3: docs/shared/ble-sync-protocol.md.
//
// Threading contract. NimBLE callbacks run on the host task and never touch the
// SD card or the display. `control` writes become ControlRequest values that
// the storage worker dequeues and executes; the worker queues `response` frames
// with deliver(), which waits on notify credits the host task returns. The
// sampling loop publishes `live`/`status` JSON.
// The display loop reads pairing()/link() and draws; nothing here draws.

#include "ble_policy.h"
#include "device_config.h"
#include "disconnect.h"

#include <cstddef>
#include <cstdint>
#include <iterator>

namespace ble {
constexpr std::uint8_t kProtocolVersion = 3;
constexpr std::size_t kMaxControlBytes = 512;
constexpr std::uint8_t kMaxLanClients = 3;
constexpr std::uint32_t kMaxRead = 16384;
constexpr std::uint16_t kMaxMtu = 517;
// Largest frame ever notified: the GATT attribute-value limit, not MTU - 3.
constexpr std::uint16_t kMaxFrame = 512;
// Leave room for framing and bounded cached snapshots below the 512-byte
// GATT attribute cap. The larger limit retains all STATUS counter fields.
constexpr std::size_t kMaxJson = 480;
static_assert(kMaxJson + 16 <= kMaxFrame,
              "JSON characteristic exceeds GATT cap");
// info JSON buffer, NUL included. The BLE info characteristic and the LAN
// HELLO frame (4 + kMaxInfo) both size from it, see wire::build_info().
constexpr std::size_t kMaxInfo = 400;

// Shared snapshot notification policy. The full value remains available by
// long read when it cannot fit the negotiated ATT notification payload.
constexpr bool snapshot_notification_fits(std::size_t length,
                                          std::uint16_t negotiated_mtu) {
  return length > 0 && length <= kMaxJson && negotiated_mtu >= 3 &&
         length <= static_cast<std::size_t>(negotiated_mtu - 3);
}

enum Op : std::uint8_t {
  kOpList = 0x01,
  kOpOpen = 0x02,
  kOpRead = 0x03,
  kOpClose = 0x04,
  kOpSetTime = 0x05,
  kOpFlush = 0x06,
  kOpStatus = 0x07,
  kOpGetConfig = 0x08,
  kOpSetConfig = 0x09,
  kOpWifiScan = 0x0a,
  kOpReboot = 0x0b,
  kOpLogTail = 0x0c,
  kOpGetToken = 0x0d,
  kOpGetDiag = 0x0e,
  kOpAbort = 0x0f,
  // Opcodes and frame types are separate number spaces. LIST_SINCE shares
  // 0x10 with the FILE frame, which is never sent to the device.
  kOpListSince = 0x10,
  // Read-only board template download, one chunk per request
  // (descriptor_chunk.h). The reply is DESCRIPTOR_CHUNK, never unsolicited.
  kOpGetDescriptor = 0x11,
  // LAN only app update into the inactive OTA slot (ota_lan.h, rules in
  // ota_transfer.h). Every success answers OTA_STATE. Reboot stays REBOOT.
  kOpOtaBegin = 0x12,
  kOpOtaWrite = 0x13,
  kOpOtaEnd = 0x14,
  kOpOtaCancel = 0x15,
};
enum Frame : std::uint8_t {
  kFrameFile = 0x10,
  kFrameListEnd = 0x11,
  kFrameOpened = 0x20,
  kFrameChunk = 0x21,
  kFrameReadEnd = 0x22,
  kFrameClosed = 0x23,
  kFrameTimeSet = 0x30,
  kFrameFlushed = 0x31,
  kFrameConfig = 0x40,
  kFrameWifiAp = 0x41,
  kFrameWifiScanEnd = 0x42,
  kFrameRebooting = 0x43,
  kFrameLog = 0x44,
  kFrameLogEnd = 0x45,
  kFrameToken = 0x46,
  kFrameHello = 0x47,
  kFrameStatus = 0x48,
  kFrameLive = 0x49,
  kFrameConfigChunk = 0x4a,
  kFrameBye = 0x4b,
  kFrameDiag = 0x4c,
  kFrameAborted = 0x4d,
  kFrameListSinceEnd = 0x4e,
  kFrameDescriptorChunk = 0x4f,
  kFrameOtaState = 0x50,
  kFrameError = 0x7f,
};
// Every frame type the device sends, for the uniqueness check below.
inline constexpr Frame kFrames[] = {
    kFrameFile,     kFrameListEnd, kFrameOpened,       kFrameChunk,
    kFrameReadEnd,  kFrameClosed,  kFrameTimeSet,      kFrameFlushed,
    kFrameConfig,   kFrameWifiAp,  kFrameWifiScanEnd,  kFrameRebooting,
    kFrameLog,      kFrameLogEnd,  kFrameToken,        kFrameHello,
    kFrameStatus,   kFrameLive,    kFrameConfigChunk,  kFrameBye,
    kFrameDiag,     kFrameAborted, kFrameListSinceEnd, kFrameDescriptorChunk,
    kFrameOtaState, kFrameError,
};
consteval bool frames_unique() {
  for (std::size_t i = 0; i < std::size(kFrames); ++i)
    for (std::size_t j = i + 1; j < std::size(kFrames); ++j)
      if (kFrames[i] == kFrames[j])
        return false;
  return true;
}
static_assert(frames_unique(), "two frame types share a number");

enum Error : std::uint8_t {
  kErrMalformed = 1,
  kErrInvalidName = 2,
  kErrNotFinalized = 3,
  kErrOpenFailed = 4,
  kErrBadHandle = 5,
  kErrRange = 6,
  kErrBusy = 7,
  kErrStorage = 8,
  kErrInvalidEpoch = 9,
  kErrNothingToFlush = 10,
  kErrUnknownOp = 11,
  kErrInvalidConfig = 12,
  kErrNotOnThisLink = 13,
  kErrAuth = 14,
  kErrWifiUnavailable = 15,
  kErrAborted = 16,
  // GET_DESCRIPTOR on a board built without a descriptor.
  kErrNoDescriptor = 17,
  // An OTA image whose header, chip, project, SHA-256 or ESP-IDF
  // verification does not match. The upload is aborted.
  kErrImageRejected = 18,
};

// Advertising service data (protocol v2.1, docs/shared/ble-sync-protocol.md
// "Advertising payload"). Ten bytes under the service UUID in the ADV PDU so a
// phone's offloaded scan filter can wake its app on a flag bit; the name and
// the 128-bit UUID list ride in the scan response. Little-endian.
constexpr std::uint8_t kAdvertVersion = 1;
constexpr std::size_t kAdvertPayloadBytes = 10;
enum AdvertFlag : std::uint8_t {
  kAdvNoUtc = 1U << 0,         // no UTC anchor this boot: rows go to `unsynced`
  kAdvNewFiles = 1U << 1,      // a file finalized since the last completed LIST
  kAdvSd = 1U << 2,            // card mounted, output directory exists
  kAdvLan = 1U << 3,           // Wi-Fi connected and the LAN server is up
  kAdvFail = 1U << 4,          // storage worker stopped after an error
  kAdvClockRestored = 1U << 5, // clock_status == 2: dated from the RTC only
  kAdvSetup = 1U << 6,         // config::setup_needed(), open the setup wizard
};
struct AdvertState {
  std::uint8_t flags = 0;
  std::uint32_t finalized = 0; // same counter as status.fin
};

// Which transport a control request arrived on. The storage worker answers on
// the same link (ble::send_response or lan::send_response) and a file handle
// is only honoured on the link that opened it.
enum class Link : std::uint8_t { Ble = 0, Lan = 1 };

struct ControlRequest {
  std::uint8_t bytes[kMaxControlBytes]{};
  std::uint16_t length = 0;
  std::uint16_t conn_handle = 0;
  std::int64_t received_mono_us = 0;
  Link link = Link::Ble;
  std::uint8_t peer = 0; // LAN slot; BLE always uses zero
  std::uint32_t link_generation = 0;
  // The link's ABORT sequence when this request arrived (abort_signal.h).
  std::uint32_t abort_seq = 0;
};

using RequestHandler = bool (*)(const ControlRequest &request);

struct Identity {
  const char *thing = nullptr;
  const char *device = nullptr;
  const char *boot = nullptr;
  const char *schema = nullptr;
  unsigned columns = 0;
  const char *dictionary_sha256 = nullptr;
  const char *firmware = nullptr;
  // desc, the 64 hex SHA-256 of the board template. info carries the first
  // 16 hex; GET_DESCRIPTOR serves the bytes it hashes.
  const char *descriptor_sha256 = nullptr;
  // Board constant that starts the BLE name. The name is the prefix plus the
  // last four device id characters (OS-6b40 on CoreS3), and the mDNS host is
  // the same text in lower case. Required, see wire::valid_name_prefix().
  const char *name_prefix = nullptr;
};

struct PairingState {
  bool active = false;
  std::uint32_t passkey = 0;
  std::int64_t deadline_us = 0;
};
struct LinkState {
  bool enabled = false;
  bool advertising = false;
  bool connected = false;
  bool authenticated = false;
  std::uint16_t mtu = 0;
  std::uint32_t bonds = 0;
  std::uint8_t advert_flags = 0; // last advertised AdvertFlag bits
};

// Call once after the firmware command queue and config::load(); identities
// must outlive the program. `mode`/`fixed_pin` are the pairing settings the
// stack starts with (they cannot change until the next boot). `options` sets
// advertising, link parameters and notify credits and must pass
// valid_options(). Returns false when the stack could not start (logging
// continues).
bool begin(const Identity &identity, config::PairMode mode,
           std::uint32_t fixed_pin, RequestHandler handler,
           const Options &options = Options{});
const char *local_name();
const char *info_json();
config::PairMode pair_mode();
// Forgets every bonded peer (ble.clear_bonds). Safe from any task.
void clear_bonds();
// Ends the current link and records `reason` as its disconnect event. Safe
// from any task; a no-op without a connection. link().connected turns false
// only after the event is recorded.
void disconnect(DisconnectReason reason);
// disconnect(), then waits up to timeout_ms for the close. On timeout the
// event is recorded here so a restart that follows still leaves it.
void disconnect_and_wait(DisconnectReason reason, std::uint32_t timeout_ms);

// From the sampling loop, after the row was queued. Payloads <= kMaxJson.
// Cache the complete value; omit notifications exceeding negotiated MTU - 3.
void publish_live(const char *json, std::size_t length);
void publish_status(const char *json, std::size_t length);
// Alongside publish_status(): refreshes the advertising service data when the
// flags or the finalized-file counter changed (one HCI command), otherwise a
// compare. Safe from any task; a no-op until begin() succeeded.
void publish_advert(const AdvertState &state);

// Advertises fast for Options::fast_window_ms, for example after a user wake.
// Safe from any task; a no-op until begin() succeeded. A documented hook for a
// board wake gesture (button or touch); no board calls it yet.
void wake_advertising();

// From the storage worker. Queues one response frame, waiting for a notify
// credit in Options::wait_step_ms steps and calling `between_steps` after
// each; it returns false to stop waiting. False when the frame was not queued:
// the peer is gone, the stream broke, the wait was stopped or no credit came
// back for twice the progress timeout. Frames leave in order.
using WaitStep = bool (*)(void *context);
bool deliver(const std::uint8_t *frame, std::size_t length,
             std::uint32_t expected_generation, WaitStep between_steps,
             void *context);
bool deliver_error(Op op, Error code, const char *detail,
                   std::uint32_t expected_generation, WaitStep between_steps,
                   void *context);
// From any other task: deliver() with no wait step and a 4 s budget. On the
// host task it tries once without waiting.
bool send_response(const std::uint8_t *frame, std::size_t length,
                   std::uint32_t expected_generation = 0);
bool send_error(Op op, Error code, const char *detail,
                std::uint32_t expected_generation = 0);
// Largest `response` payload for the current link (MTU - 3), or 0 if none.
std::uint16_t payload_max();
// Increments on every connect/disconnect so the worker can drop stale handles.
std::uint32_t connection_generation();

PairingState pairing();
LinkState link();
// Increments whenever pairing()/link() changes; cheap to poll from the UI.
std::uint32_t ui_generation();
} // namespace ble
