#pragma once

#include "ble_sync.h"

#include <cstdint>

namespace telemetry {
struct PmsSnapshot {
  std::uint16_t values[12]{};
  std::int64_t received_mono_us = 0;
  std::uint32_t frames = 0;
  std::uint32_t checksum_errors = 0;
  std::uint32_t length_errors = 0;
  std::uint8_t firmware = 0;
  std::uint8_t error = 0;
  bool present = false;
};

// Called once after M5.begin, SD mount and the internal flash mount. One
// worker owns all subsequent SD IO. `fallback_directory` is the flash archive
// root, or nullptr when that volume is off. False when the logger could not
// start, so no row is ever written.
[[nodiscard]] bool begin_logger(bool sd_mounted,
                                const char *fallback_directory);
// The row's PMS status (telemetry_contract.h pms_status_of) for the screen and
// the MEAS line, so neither shows a value the row nulls (#165). `status`
// follows the pms_status codes, 4 is valid. `age_ms` is the saturated
// pms_age_ms.
struct PmsReading {
  std::int32_t status = 0;
  const char *name = "missing";
  std::uint32_t age_ms = 0;
};
PmsReading pms_reading(bool present, std::int64_t now_us,
                       std::int64_t received_mono_us,
                       std::uint8_t sensor_error);
// SD volume owned by main.cpp. Storage worker only, with the display lock
// held. remount unmounts and mounts again, never formats.
bool remount_sd();
std::uint32_t sd_cluster_bytes(); // 0 when unknown
std::uint32_t sd_volume_serial(); // FAT volume serial, 0 when unknown
// False once the card stops answering SD CMD13, as a pulled card does.
bool sd_present();
bool sd_free_bytes(std::uint64_t &free_bytes);
// Call on every application loop; takes one snapshot when the 10s deadline is
// due.
void poll_logger(const PmsSnapshot &pms);
// Serialize display transactions with the storage worker on the shared SPI bus.
void lock_display();
void unlock_display();
// Load device settings and start the sync links
// (docs/shared/ble-sync-protocol.md): BLE with the stored pairing mode, and the
// Wi-Fi/LAN task (radio stays off until configured). `display_detected` seeds
// the first-boot pairing default. Call after begin_logger(); false when BLE
// could not start.
bool start_links(bool display_detected);
// Called from the NimBLE host task or the LAN task: queue a control request
// for the storage worker (Wi-Fi scans are diverted to the LAN task). False
// when the command queue is full (the caller reports busy on its link).
bool enqueue_request(const ble::ControlRequest &request);
// Identities for mDNS TXT records; valid after begin_logger().
const char *thing_text_id();
const char *device_text_id();
const char *firmware_text_id();

// Snapshot of the device clock for the display. `utc_ns` is the current UTC
// estimate (0 when no anchor); `source` follows the `clock_status` codes
// (0 none, 1 host, 2 RTC-restored, 3 SNTP); `rtc_state` is 0 unread, 1 in
// use/written, 2 unusable. UTC only: local time is a reader/phone concern.
struct ClockView {
  std::int64_t utc_ns = 0;
  std::int32_t epoch = 0;
  std::int32_t source = 0;
  std::int32_t rtc_state = 0;
  const char *source_name = "none";
};
ClockView clock_view();
} // namespace telemetry
