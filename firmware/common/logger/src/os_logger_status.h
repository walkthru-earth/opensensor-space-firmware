#pragma once

#include <cstddef>
#include <cstdint>
#include <parquet_writer.h>
#include <storage_governor.h>
#include <string_view>

namespace oslogger {
// Pure snapshot/formatter shared by the runtime and host boundary tests.
struct StatusSnapshot {
  std::uint32_t uptime_seconds = 0;
  std::uint32_t interval_seconds = 0;
  std::uint32_t buffered = 0;
  std::uint32_t finalized = 0;
  std::uint32_t dropped = 0;
  std::uint32_t errors = 0;
  std::int64_t missed = 0;
  bool failed = false;
  telemetry::Codec codec = telemetry::Codec::Uncompressed;
  bool utc = false;
  std::int32_t generation = 0;
  // Small codes (clk 0 to 3, rtc 0 to 2) printed as u8 so the worst-case
  // object keeps headroom under ble::kMaxJson.
  std::uint8_t clock_source = 0;
  std::uint8_t rtc_state = 0;
  std::int64_t utc_ms = 0;
  std::int64_t clock_age_seconds = 0;
  bool storage_ok = false;
  storage::State sd_state = storage::State::Retry;
  std::uint32_t total_kib = 0;
  std::uint32_t used_kib = 0;
  std::uint32_t heap_free = 0;
  std::uint32_t partials = 0;
  std::uint32_t quarantined = 0;
  std::uint64_t quarantine_bytes = 0;
  std::uint32_t open_rows = 0;
  std::uint32_t open_groups = 0;
  // Sessions closed since boot (BLE and LAN, saturating like the DIAG counts)
  // and the newest ble::DisconnectReason code; "dr" is null until the first
  // close. A u16 keeps the worst-case object within ble::kMaxJson.
  // Contract in docs/shared/ble-sync-protocol.md, "Protocol 3 contract"
  // (#protocol-3-contract), and the `status` table (#status-read-notify).
  std::uint16_t disconnects = 0;
  std::uint8_t last_disconnect = 0;
  // "ls", the LIST_SINCE mount epoch (FAT volume serial, 0 when unknown).
  // The key's presence tells the phone the board serves LIST_SINCE.
  std::uint32_t mount_epoch = 0;
};
// Returns zero on insufficient capacity, never reports truncated JSON as valid.
std::size_t format_status(const StatusSnapshot &, char *, std::size_t capacity);

// Whether rows reach the card, for a board screen (#165). The inputs are the
// same state as STATUS "ok", "fail" and "sd". `started` is a successful
// begin(). The result is empty while rows are written, else why they are not.
struct LoggingState {
  bool started = false;
  bool storage_ok = false;
  bool failed = false;
  storage::State sd_state = storage::State::Retry;
};
[[nodiscard]] constexpr std::string_view
logging_stopped_reason(const LoggingState &state) {
  return !state.started                            ? "not started"
         : !state.storage_ok                       ? "no card"
         : state.sd_state == storage::State::Full  ? "card full"
         : state.sd_state == storage::State::Retry ? "card retry"
         : state.failed                            ? "writer stopped"
                                                   : "";
}
} // namespace oslogger
