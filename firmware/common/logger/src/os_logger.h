#pragma once

#include "os_logger_cadence.h"
#include <ble_sync.h>
#include <cstddef>
#include <cstdint>
#include <numeric_sample.h>
#include <os_battery_health.h>
#include <parquet_writer.h>
#include <sync_service.h>

namespace opensensor::gnss {
struct Fix;
} // namespace opensensor::gnss

namespace oslogger {
using Row = opensensor::NumericSample<telemetry::kMaxColumns>;
struct Field {
  const char *name = nullptr;
  telemetry::PhysicalType type = telemetry::PhysicalType::Int32;
  const char *procedure = nullptr;
  const char *unit = nullptr;
  const char *validity = nullptr;
  const char *property_uri = nullptr;
  // Short JSON key for the LIVE snapshot, or nullptr to keep the field out of
  // LIVE. Keys are [a-z0-9_]{1,15} and unique within the table.
  const char *live_key = nullptr;
  // Digits after the point when a Float prints in LIVE, 0..kMaxLiveDecimals.
  // Each board sets it per field from the sensor resolution. Integer fields
  // keep 0 and ignore it.
  std::uint8_t decimals = 0;
};
// All pointed-to descriptors and strings must outlive the runtime. The schema
// must include INT64 sequence, monotonic_us, event_time_utc_us and INT32
// clock_epoch. UTC values remain null until a supported clock anchor exists.
// INT64 fields with unit "us_since_unix_epoch" are written as
// TIMESTAMP(MICROS, UTC). Each leaf's Parquet field_id is its 1-based position
// in `fields`, so a board appends new fields and never reorders them.
struct Config {
  const Field *fields = nullptr;
  std::size_t column_count = 0;
  std::int32_t schema_version = 0;
  const char *schema_name = nullptr;
  const char *firmware = nullptr;
  // Passed to Writer.finish; nullptr keeps the historical writer default.
  const char *created_by = nullptr;
  const char *dictionary_version = nullptr;
  const char *dictionary_uri = nullptr;
  const char *dictionary_sha256 = nullptr;
  const char *configuration_id = nullptr;
  const char *configuration = nullptr;
  const char *time_semantics = nullptr;
  const char *board = nullptr;
  const char *output_directory = "/sd/output";
  // Optional legacy archive root; no board path is assumed.
  const char *legacy_directory = nullptr;
  // codec-test copies live here, outside the listed and synced archive.
  const char *benchmark_directory = "/sd/bench";
  // Sync service tuning; nullptr keeps the shared defaults.
  const ossync::ServiceConfig *sync = nullptr;
  // Storage worker task. Its stack stays in internal RAM: the worker commits
  // NVS (SET_CONFIG, thing id), and a flash write disables the cache that a
  // PSRAM stack needs. Size from the hourly DIAG tasks line plus 25 percent.
  unsigned worker_priority = 3;
  std::size_t worker_stack_bytes = 24576;
  // Sample period, also written as footer acquisition.sample_interval_ms. It
  // must pass cadence::valid_sample_interval(). Row groups hold the rows of
  // one rotation window, at most cadence::kMaxBatchRows.
  std::uint32_t sample_interval_ms = cadence::kDefaultSampleIntervalMs;
  // Footer thing.deployment_id and acquisition.calibration_id. Non empty
  // text, "unknown" until a board or deployment knows better.
  const char *deployment_id = "unknown";
  const char *calibration_id = "unknown";
  // Required board constant for ble::Identity::name_prefix. The mDNS host is
  // the BLE name in lower case. CoreS3 and Waveshare pass "OS-".
  const char *ble_name_prefix = nullptr;
  // Required stac-opensensor board template (#85), the generated kTemplateJson
  // of telemetry_template.h without its NUL, and desc, its SHA-256 from
  // telemetry_descriptor_digest.h. `parquet schema` reports both.
  const char *descriptor = nullptr;
  std::size_t descriptor_bytes = 0;
  const char *descriptor_sha256 = nullptr;
  // Required footer template.href (#88), where the template whose SHA-256 is
  // descriptor_sha256 is published. One board constant, kTemplateHref.
  const char *template_href = nullptr;
  // Boot self test (#40). A row field that must hold a valid value at least
  // once before the deadline. nullptr skips the sensor check. The SD card
  // must be ready and the BLE stack started by the same deadline, then the
  // logger prints one SELFTEST line and marks the running app valid.
  const char *self_test_field = nullptr;
  std::uint32_t self_test_deadline_ms = 180000;
};
// Shared default for each board's sampling and UI task: above the idle
// housekeeping, below the storage worker (3), the LAN task (4) and the BLE
// error task (4).
constexpr unsigned kBoardTaskPriority = 2;
struct Hooks {
  void *context = nullptr;
  // Main loop only. Set hardware fields; runtime supplies identity, clock,
  // memory/storage counters and collection completion after this callback.
  void (*collect)(Row &, std::int64_t now, std::int64_t scheduled,
                  void *context) = nullptr;
  // Storage worker only, after the board has mounted its filesystem, called
  // with the bus lock held. free returns false when the volume cannot answer.
  std::uint64_t (*storage_total_bytes)(void *) = nullptr;
  std::uint64_t (*storage_used_bytes)(void *) = nullptr;
  bool (*storage_free_bytes)(std::uint64_t &free_bytes, void *) = nullptr;
  std::uint32_t (*storage_cluster_bytes)(void *) = nullptr; // 0 when unknown
  // Optional. FAT volume serial of the mounted card, read after every
  // successful mount and remount with the bus lock held. 0 when unknown. It
  // is the LIST_SINCE mount epoch, so a swapped card gets a full listing.
  std::uint32_t (*storage_volume_serial)(void *) = nullptr;
  // Optional. Unmount and mount the card again so a reinserted card or space
  // freed on a PC becomes visible. Called by the storage worker with the bus
  // lock held and no file open. Never formats. Returns true when mounted.
  bool (*storage_remount)(void *) = nullptr;
  // Supply both or neither. Otherwise the engine owns its own bus mutex.
  void (*lock_bus)(void *) = nullptr;
  void (*unlock_bus)(void *) = nullptr;
  // Main loop only. read must reject unset/invalid RTC or voltage-low data;
  // write must verify readback. Values are UTC seconds, never local time.
  bool (*rtc_read)(std::int64_t &utc_seconds, void *) = nullptr;
  bool (*rtc_write)(std::int64_t utc_seconds, void *) = nullptr;
  // Optional, main loop only. Called once per battery state change reported
  // through report_battery(), after the POWER STATE line. The default is no
  // action. A board may switch off loads here. It must not block, reset or
  // sleep; the runtime already finalizes the open file on a critical entry.
  void (*power_shed)(opensensor::battery::State, void *) = nullptr;
  // Optional. Copy the newest GNSS fix into `fix` and return true, or
  // return false when there is none. The fix holds an H3 cell and its
  // monotonic time, never coordinates. Must not block or touch the receiver
  // UART. Called on the main loop once per row, and on the storage worker for
  // GET_CONFIG page 1 location.gnss_cell (#147), so it must be safe from any
  // task, as opensensor::gnss::latest() is. A moving Thing uses a fix no older
  // than opensensor::gnss::kFixMaxAgeUs, at H3 res 8 (#139), as
  // location_source 2, and writes location nulls otherwise. A set
  // location.phone_cell wins over the fix, as location_source 3 (#140). A
  // fixed Thing ignores both for rows. nullptr means the board has no receiver,
  // and page 1 then omits location.gnss_cell.
  bool (*gnss_fix)(opensensor::gnss::Fix &fix, void *) = nullptr;
};
// One singleton per firmware image; call once after hardware/storage setup.
bool begin(const Config &, const Hooks &, bool sd_mounted);
void poll();
// Main loop only, normally from the collect hook. Prints one POWER STATE
// line per change and, once per critical entry, raises the emergency close
// intent and wakes the storage worker, which closes the open file. Never
// touches the card, resets or sleeps.
void report_battery(opensensor::battery::State state, std::int32_t millivolts);
bool start_links(bool display_detected);
bool enqueue_request(const ble::ControlRequest &);
void lock_bus();
void unlock_bus();
const char *thing_text_id();
const char *device_text_id();
const char *firmware_text_id();
std::size_t field_index(const char *name); // column_count when absent
void integer(Row &, const char *name, std::int32_t value);
void counter(Row &, const char *name, std::int64_t value);
void number(Row &, const char *name, float value);
struct ClockView {
  std::int64_t utc_ns = 0;
  std::int32_t epoch = 0;
  std::int32_t source = 0;
  std::int32_t rtc_state = 0;
  const char *source_name = "none";
};
ClockView clock_view();
} // namespace oslogger
