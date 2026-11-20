#include "../board_io.h"
#include "display_profile.h"
#include "epaper_ui/canvas.h"
#include "epaper_ui/history.h"
#include "epaper_ui/orientation.h"
#include "epaper_ui/pages.h"
#include "epaper_ui/presenter.h"
#include "epaper_ui/settings.h"
#include "epaper_ui/ui_model.h"
#include "epd_ssd1677.h"
#include "sensor_modules.h"
#include "telemetry_contract.h"
#include <algorithm>
#include <array>
#include <ble_sync.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <debug_log.h>
#include <device_config.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_psram.h>
#include <esp_timer.h>
#include <expected>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <limits>
#include <log_persist.h>
#include <new>
#include <nvs.h>
#include <nvs_flash.h>
#include <optional>
#include <os_console.h>
#include <os_logger.h>
#include <os_sen6x.h>
#include <os_stac_item.h>
#include <power.h>
#include <span>
#include <string_view>
#include <utility>
#include <wifi_link.h>

#ifndef CONFIG_SPIRAM
#error The ePaper-3.97 logger requires the qualified 8 MB OPI PSRAM target.
#endif

namespace {
using namespace telemetry::contract;
namespace epd = opensensor::epd;
namespace ui = opensensor::ui;
using board_modules::Sen63cReading;
using board_modules::Sen63cSnapshot;
using opensensor::Sen6x;
using opensensor::Sen6xError;

// ---------------------------------------------------------------------------
// SEN63C. The board task owns the sensor, the snapshot and the I2C bus.

constexpr opensensor::Sen6xIo kSensorIo{.context = nullptr,
                                        .write = board::sensor_write,
                                        .read = board::sensor_read,
                                        .delay_ms = board::sensor_delay_ms};
constinit Sen6x sen6x{kSensorIo};
constinit Sen63cSnapshot snapshot{};

// The sensor modules of board.json (#17 step 5).
constinit board_modules::Sources sources{.sensor = &snapshot};
static constexpr auto kModules = board_modules::table(sources);

// The SEN63C makes one result per second. Polling twice as often keeps the
// newest result well inside kSen6xStaleAfterUs.
constexpr std::int64_t kSensorPollUs = 500000;
// The device status is read every 10th poll, every 5 s.
constexpr unsigned kStatusEveryPolls = 10;
// Consecutive failed transactions before the sensor counts as gone.
constexpr unsigned kLostAfterFailures = 5;
// Probe and start retries back off from 1 s to 60 s.
constexpr std::int64_t kRetryMinUs = 1000000;
constexpr std::int64_t kRetryMaxUs = 60000000;
// A running sensor with no new result for this long is stopped and started
// again. The start still honours the 24 s CO2 conditioning rule.
constexpr std::int64_t kRestartAfterUs = 10000000;

// Sensor settings from the panel. The UI task hands them over under
// feed_mutex and the board task applies them. Altitude and CO2 self
// calibration take idle mode only and the sensor forgets all three on a
// reset, so they go in before every start.
struct SensorSettings {
  std::uint16_t altitude_m = 0;
  bool co2_asc = true;
  std::int16_t temp_offset_dc = 0; // tenths of a degree C
  bool operator==(const SensorSettings &) const = default;
};
struct SensorRequests {
  SensorSettings settings{};
  std::uint32_t fan_clean_seq = 0; // each new value asks for one cleaning
};
SensorRequests sensor_requests; // feed_mutex once it exists

struct SensorControl {
  std::int64_t next_poll_us = 0;
  std::int64_t next_attempt_us = 0;
  std::int64_t retry_us = kRetryMinUs;
  unsigned failures = 0;
  unsigned polls = 0;
  bool absent_logged = false;
  bool status_logged = false;
  std::uint32_t logged_status = 0;
  std::array<char, 33> serial{};
  std::array<char, 8> version{};
  SensorSettings wanted{};
  SensorSettings applied{};
  bool configured = false; // `applied` is in the sensor since the last stop
  // A stop the sensor did not take. It is sent again before the next
  // configure, fan cleaning or start, which all need idle mode.
  bool stop_pending = false;
  std::uint32_t fan_clean_seen = 0;
  std::int64_t cleaning_until_us = 0; // 0 when no fan cleaning runs
};
SensorControl control;
SemaphoreHandle_t feed_mutex = nullptr;

std::uint32_t now_ms(std::int64_t now_us) {
  return static_cast<std::uint32_t>(now_us / 1000);
}

void back_off(std::int64_t now) {
  control.next_attempt_us = now + control.retry_us;
  control.retry_us = std::min(control.retry_us * 2, kRetryMaxUs);
}

// Counts one failed transaction. After kLostAfterFailures in a row the
// sensor is gone, and the next probe has to find it again.
void sensor_failed(const char *operation, Sen6xError error, std::int64_t now) {
  if (++control.failures < kLostAfterFailures)
    return;
  oslog.printf("OS SEN6X lost operation=%s error=%s i2c_error=%d\n", operation,
               opensensor::sen6x_error_name(error), board::sensor_last_error());
  snapshot.present = false;
  snapshot.measuring = false;
  snapshot.has_values = false;
  control.configured = false;
  control.cleaning_until_us = 0;
  control.failures = 0;
  control.absent_logged = false;
  back_off(now);
}

// Reads the device status. A SEN63C error bit is sticky, so the board reads
// and clears it, logs it once and latches it for the next row.
void read_status(std::int64_t now) {
  auto status = sen6x.read_device_status();
  if (!status) {
    sensor_failed("read-status", status.error(), now);
    return;
  }
  std::uint32_t raw = status->raw;
  std::uint32_t latest = raw;
  bool cleared = false;
  if (status->sen63c_error()) {
    const auto clear = sen6x.read_and_clear_device_status();
    if (clear) {
      raw |= clear->raw;
      latest = clear->raw & ~opensensor::Sen6xStatus::kSen63cErrors;
      cleared = true;
    } else {
      sensor_failed("clear-status", clear.error(), now);
    }
  }
  if (!control.status_logged || raw != control.logged_status) {
    const opensensor::Sen6xStatus bits{.raw = raw};
    oslog.printf("OS SEN6X status=0x%08lx rht_error=%u co2_error=%u "
                 "pm_error=%u fan_error=%u fan_speed_warning=%u cleared=%u\n",
                 static_cast<unsigned long>(raw), bits.rht_error(),
                 bits.co2_error(), bits.pm_error(), bits.fan_error(),
                 bits.fan_speed_warning(), cleared);
    control.logged_status = raw;
    control.status_logged = true;
  }
  snapshot.device_status |= raw;
  snapshot.device_status_latest = latest;
  snapshot.has_device_status = true;
}

// Finds the sensor. Stops any measurement a warm reset left running, then
// reads its identity. An absent sensor is logged once per loss.
void probe(std::int64_t now) {
  static_cast<void>(sen6x.stop_measurement());
  const auto name = sen6x.product_name();
  if (!name) {
    if (!control.absent_logged) {
      oslog.printf("OS SEN6X absent error=%s i2c_error=%d sda=%d scl=%d "
                   "addr=0x%02x\n",
                   opensensor::sen6x_error_name(name.error()),
                   board::sensor_last_error(), board::kSensorSda,
                   board::kSensorScl, Sen6x::kAddress);
      control.absent_logged = true;
    }
    back_off(now);
    return;
  }
  const auto serial = sen6x.serial_number();
  const auto version = sen6x.version();
  control.serial = {};
  control.version = {};
  if (serial)
    std::snprintf(control.serial.data(), control.serial.size(), "%s",
                  serial->c_str());
  if (version)
    std::snprintf(control.version.data(), control.version.size(), "%u.%u",
                  static_cast<unsigned>(version->major),
                  static_cast<unsigned>(version->minor));
  const bool match = name->view() == "SEN63C";
  oslog.printf("OS SEN6X present product=%s serial=%s version=%s match=%u\n",
               name->c_str(), serial ? serial->c_str() : "unknown",
               version ? control.version.data() : "unknown", match);
  snapshot.present = true;
  control.configured = false;
  control.stop_pending = false; // the stop above covers it
  control.failures = 0;
  control.absent_logged = false;
  control.next_attempt_us = now;
  read_status(now);
}

opensensor::Sen6xTemperatureOffset offset_of(const SensorSettings &s) {
  // Tenths of a degree to the 1/200 C the sensor takes.
  return {.offset_x200 = static_cast<std::int16_t>(s.temp_offset_dc * 20),
          .slope_x10000 = 0,
          .time_constant_s = 0,
          .slot = 0};
}

void log_settings(const char *when) {
  oslog.printf("OS SEN6X config when=%s altitude_m=%u co2_asc=%u "
               "temp_offset_dc=%d\n",
               when, static_cast<unsigned>(control.applied.altitude_m),
               control.applied.co2_asc,
               static_cast<int>(control.applied.temp_offset_dc));
}

// Idle mode only. Writes every sensor setting, so a sensor that reset puts
// them back too.
bool configure(std::int64_t now) {
  const auto &s = control.wanted;
  auto done = sen6x.set_sensor_altitude(s.altitude_m);
  const char *operation = "set-altitude";
  if (done) {
    done = sen6x.set_co2_auto_calibration(s.co2_asc, now_ms(now));
    operation = "set-co2-asc";
  }
  if (done) {
    done = sen6x.set_temperature_offset(offset_of(s));
    operation = "set-temp-offset";
  }
  if (!done) {
    oslog.printf("OS SEN6X config-failed operation=%s error=%s "
                 "i2c_error=%d\n",
                 operation, opensensor::sen6x_error_name(done.error()),
                 board::sensor_last_error());
    sensor_failed(operation, done.error(), now);
    back_off(now);
    return false;
  }
  control.applied = s;
  control.configured = true;
  log_settings("start");
  return true;
}

// Stops a running measurement, the idle commands need it. Rows go null at
// once, but the sensor counts as idle only after a stop it took. A failed
// stop stays pending and the next fan cleaning or start sends it again.
[[nodiscard]] std::expected<void, Sen6xError> stop(std::int64_t now) {
  if (!snapshot.measuring && !control.stop_pending)
    return {};
  snapshot.measuring = false;
  snapshot.has_values = false;
  control.stop_pending = true;
  const auto stopped = sen6x.stop_measurement();
  if (!stopped) {
    control.configured = false;
    sensor_failed("stop", stopped.error(), now);
    return stopped;
  }
  control.stop_pending = false;
  return {};
}

void start(std::int64_t now) {
  if (control.stop_pending) {
    if (!stop(now)) {
      back_off(now);
      return;
    }
    now = esp_timer_get_time(); // the stop waited 1400 ms
  }
  // Datasheet 4.8.1. After a stop inside the 24 s CO2 conditioning window
  // the sensor must not take the CO2 self calibration write, and the start
  // has to wait for the same window, so the whole configure waits for it.
  if (!control.configured) {
    if (const auto left_ms = sen6x.conditioning_left_ms(now_ms(now));
        left_ms != 0) {
      control.next_attempt_us = now + static_cast<std::int64_t>(left_ms) * 1000;
      return;
    }
    if (!configure(now))
      return;
  }
  const auto started = sen6x.start_continuous_measurement(now_ms(now));
  if (started) {
    snapshot.measuring = true;
    snapshot.start_mono_us = now;
    snapshot.has_values = false;
    control.retry_us = kRetryMinUs;
    control.failures = 0;
    oslog.printf("OS SEN6X started mono_us=%lld pm_warmup_us=%lld "
                 "co2_warmup_us=%lld\n",
                 static_cast<long long>(now),
                 static_cast<long long>(kSen6xPmWarmupUs),
                 static_cast<long long>(kSen6xCo2WarmupUs));
    return;
  }
  if (started.error() == Sen6xError::RestartTooSoon) {
    control.next_attempt_us = now + kRetryMinUs;
    return;
  }
  oslog.printf("OS SEN6X start-failed error=%s i2c_error=%d\n",
               opensensor::sen6x_error_name(started.error()),
               board::sensor_last_error());
  sensor_failed("start", started.error(), now);
  back_off(now);
}

void measure(std::int64_t now) {
  const auto ready = sen6x.data_ready();
  if (!ready) {
    sensor_failed("data-ready", ready.error(), now);
    return;
  }
  if (*ready) {
    const auto values = sen6x.read_sen63c_values();
    if (!values) {
      sensor_failed("read-values", values.error(), now);
      return;
    }
    snapshot.values = *values;
    snapshot.has_values = true;
    snapshot.received_mono_us = now;
  }
  control.failures = 0;
  if (++control.polls % kStatusEveryPolls == 0)
    read_status(now);
  if (!snapshot.measuring)
    return;
  const std::int64_t since =
      snapshot.has_values ? snapshot.received_mono_us : snapshot.start_mono_us;
  if (now - since > kRestartAfterUs) {
    oslog.printf("OS SEN6X stale age_us=%lld action=restart\n",
                 static_cast<long long>(now - since));
    static_cast<void>(stop(now));
    // No new result can mean the sensor reset and lost its settings.
    control.configured = false;
    if (snapshot.present)
      control.next_attempt_us = now;
  }
}

SensorRequests take_requests() {
  if (!feed_mutex)
    return sensor_requests;
  xSemaphoreTake(feed_mutex, portMAX_DELAY);
  const SensorRequests requests = sensor_requests;
  xSemaphoreGive(feed_mutex);
  return requests;
}

// Datasheet 4.8.23. Idle mode only, the fan runs at full speed for 10 s and
// the next start waits for it. Rows stay null while it runs. False while the
// stop it needs failed, so the request waits for the next poll. A lost sensor
// drops it.
bool fan_clean(std::int64_t now) {
  if (!snapshot.present || control.cleaning_until_us != 0)
    return true;
  if (!stop(now))
    return !snapshot.present;
  const auto cleaning = sen6x.start_fan_cleaning();
  if (!cleaning) {
    oslog.printf("OS SEN6X fan-cleaning-failed error=%s i2c_error=%d\n",
                 opensensor::sen6x_error_name(cleaning.error()),
                 board::sensor_last_error());
    sensor_failed("fan-cleaning", cleaning.error(), now);
    control.next_attempt_us = now;
    return true;
  }
  control.cleaning_until_us =
      esp_timer_get_time() +
      static_cast<std::int64_t>(Sen6x::kFanCleaningMs) * 1000;
  control.next_attempt_us = control.cleaning_until_us;
  oslog.printf("OS SEN6X fan-cleaning started duration_ms=%lu\n",
               static_cast<unsigned long>(Sen6x::kFanCleaningMs));
  return true;
}

// Takes the requested settings. A new altitude or CO2 calibration needs
// idle mode, so the measurement stops and the next start writes them. The
// temperature offset works while measuring and goes in at once.
void apply_requests(std::int64_t now) {
  const auto requests = take_requests();
  control.wanted = requests.settings;
  if (requests.fan_clean_seq != control.fan_clean_seen && fan_clean(now))
    control.fan_clean_seen = requests.fan_clean_seq;
  if (!snapshot.present || !control.configured ||
      control.wanted == control.applied)
    return;
  if (control.wanted.altitude_m != control.applied.altitude_m ||
      control.wanted.co2_asc != control.applied.co2_asc) {
    oslog.println("OS SEN6X config-change action=restart");
    // A failed stop stays pending, start() sends it before configure().
    static_cast<void>(stop(now));
    control.configured = false;
    control.next_attempt_us = std::max(control.next_attempt_us, now);
    return;
  }
  const auto set = sen6x.set_temperature_offset(offset_of(control.wanted));
  if (!set) {
    sensor_failed("set-temp-offset", set.error(), now);
    return;
  }
  control.applied.temp_offset_dc = control.wanted.temp_offset_dc;
  log_settings("change");
}

void poll_sensor(std::int64_t now) {
  if (now < control.next_poll_us)
    return;
  control.next_poll_us = now + kSensorPollUs;
  apply_requests(now);
  if (control.cleaning_until_us != 0 && now >= control.cleaning_until_us) {
    control.cleaning_until_us = 0;
    oslog.println("OS SEN6X fan-cleaning done");
  }
  if (!snapshot.present) {
    if (now >= control.next_attempt_us)
      probe(now);
    return;
  }
  if (!snapshot.measuring) {
    if (now >= control.next_attempt_us)
      start(esp_timer_get_time());
    return;
  }
  measure(now);
}

// ---------------------------------------------------------------------------
// Row hook. Runs on the board task, so the snapshot needs no lock.

std::uint32_t row_count = 0;
bool sample_pending = false;
std::int64_t sample_mono_us = 0;

void collect(oslogger::Row &row, std::int64_t now, std::int64_t, void *) {
  board_modules::collect(kModules, row, now);
  ++row_count;
  sample_pending = true;
  sample_mono_us = now;
}

// The STAC File Item statistics columns (#142, #184), the numeric measurement
// columns of the board template in template order. stac-item-test proves the
// Item against the board template.
constexpr auto kStatColumns =
    opensensor::stac::stat_columns<opensensor::stac::stat_count(
        kFields, std::span(kSemantics))>(kFields, std::span(kSemantics));
static_assert(opensensor::stac::stat_columns_valid(kStatColumns, kFields,
                                                   std::span(kSemantics)),
              "every STAC statistics column must have a gate the Item reads");

// OpenSensor BLE name prefix, so the board advertises OS-xxxx.
constexpr char kBleNamePrefix[] = "OS-";
const oslogger::Config kLoggerConfig{
    .fields = kFields,
    .column_count = field_count,
    .schema_version = kSchemaVersion,
    .schema_name = kSchemaName,
    .firmware = kFirmware,
    .created_by = kCreatedBy,
    .dictionary_version = kDictionaryVersion,
    .dictionary_uri = kDictionaryUri,
    .dictionary_sha256 = kDictionarySha256,
    .configuration_id = kConfigurationId,
    .configuration = kConfiguration,
    .time_semantics = kTimeSemantics,
    .board = kBoard,
    .output_directory = "/sd/output",
    .ble_name_prefix = kBleNamePrefix,
    .descriptor = kTemplateJson,
    .descriptor_bytes = sizeof(kTemplateJson) - 1,
    .descriptor_sha256 = kDescriptorSha256,
    .template_href = kTemplateHref,
    .self_test_field = "pm25_mass_ug_m3",
    .stac_columns = kStatColumns.data(),
    .stac_column_count = kStatColumns.size(),
};
const oslogger::Hooks kHooks{
    .collect = collect,
    .storage_total_bytes = [](void *) { return board::storage_total_bytes(); },
    .storage_used_bytes = [](void *) { return board::storage_used_bytes(); },
    .storage_free_bytes =
        [](std::uint64_t &free_bytes, void *) {
          return board::storage_free_bytes(free_bytes);
        },
    .storage_cluster_bytes = [](void *) { return board::cluster_bytes(); },
    .storage_volume_serial = [](void *) { return board::volume_serial(); },
    .storage_remount =
        [](void *) {
          const bool ok = board::remount_card();
          if (!ok && board::sd_buffer_unavailable_once())
            oslog.println("OS TF remount=false reason=dma_buffer_unavailable");
          return ok;
        },
    .storage_present = [](void *) { return board::card_present(); },
    .rtc_read = board::rtc_read,
    .rtc_write = board::rtc_write,
};

bool logging = false;
bool links_started = false;

// ---------------------------------------------------------------------------
// UI feed. The board task publishes, the UI task copies under the mutex.

using Text = std::array<char, 48>;
struct UiFeed {
  std::uint32_t version = 0;
  std::uint32_t sample_seq = 0;
  std::uint32_t sample_mono_s = 0;
  std::array<float, ui::kMetricCount> sample{};
  Sen63cReading reading{};
  bool sensor_present = false;
  std::optional<std::int64_t> utc_s;
  bool sd_ok = false;
  std::uint64_t sd_free = 0;
  ui::Link ble = ui::Link::Off;
  ui::Link wifi = ui::Link::Off;
  std::uint32_t samples = 0;
  std::uint32_t uptime_s = 0;
  std::optional<std::uint32_t> passkey;
  bool wifi_configured = false;
  bool wifi_enabled = false;
  bool fan_cleaning = false;
  Text firmware{};
  Text thing{};
  Text sensor_serial{};
  Text sensor_fw{};
  Text device_name{};
};
UiFeed feed;
QueueHandle_t key_queue = nullptr;

// The feed refreshes at least this often without a row, so the clock and the
// uptime move while logging is off.
constexpr std::int64_t kFeedTickUs = 10000000;
// Link and pairing changes are checked this often.
constexpr std::int64_t kLinkCheckUs = 250000;

void copy_text(Text &to, const char *from) {
  std::snprintf(to.data(), to.size(), "%s", from ? from : "");
}

float or_nan(std::optional<float> v) {
  return v ? *v : std::numeric_limits<float>::quiet_NaN();
}

// The row values in ui::Metric order, NaN for a null cell.
std::array<float, ui::kMetricCount> metrics_of(const Sen63cReading &r) {
  std::array<float, ui::kMetricCount> out{};
  out[ui::index(ui::Metric::Pm1)] = or_nan(r.pm1_ug_m3);
  out[ui::index(ui::Metric::Pm25)] = or_nan(r.pm25_ug_m3);
  out[ui::index(ui::Metric::Pm4)] = or_nan(r.pm4_ug_m3);
  out[ui::index(ui::Metric::Pm10)] = or_nan(r.pm10_ug_m3);
  out[ui::index(ui::Metric::Co2)] =
      r.co2_ppm ? static_cast<float>(*r.co2_ppm)
                : std::numeric_limits<float>::quiet_NaN();
  out[ui::index(ui::Metric::Temp)] = or_nan(r.temperature_c);
  out[ui::index(ui::Metric::Rh)] = or_nan(r.relative_humidity_percent);
  return out;
}

ui::Link ble_link() {
  if (!links_started)
    return ui::Link::Off;
  const auto link = ble::link();
  if (!link.enabled)
    return ui::Link::Off;
  return link.connected && link.authenticated ? ui::Link::Connected
                                              : ui::Link::Waiting;
}

ui::Link wifi_link() {
  if (!links_started)
    return ui::Link::Off;
  const char *state = lan::status().state;
  if (std::strcmp(state, "off") == 0)
    return ui::Link::Off;
  return std::strcmp(state, "connected") == 0 ? ui::Link::Connected
                                              : ui::Link::Waiting;
}

std::optional<std::uint32_t> pairing_passkey() {
  if (!links_started)
    return std::nullopt;
  const auto pairing = ble::pairing();
  if (!pairing.active)
    return std::nullopt;
  return pairing.passkey;
}

struct WifiSetup {
  bool configured = false;
  bool on = false;
};

// The settings copy holds the Wi-Fi PSK, so only two flags leave here.
WifiSetup wifi_setup() {
  if (!links_started)
    return {};
  const auto settings = config::get();
  return {.configured = settings.ssid[0] != '\0', .on = settings.wifi_on};
}

// Board task only.
void publish(std::int64_t now, bool sample) {
  UiFeed next;
  const auto clock = oslogger::clock_view();
  if (clock.epoch != 0)
    next.utc_s = clock.utc_ns / 1000000000;
  next.sd_ok = board::card_mounted();
  next.sd_free = board::cached_free_bytes();
  next.ble = ble_link();
  next.wifi = wifi_link();
  next.samples = row_count;
  next.uptime_s = static_cast<std::uint32_t>(now / 1000000);
  next.passkey = pairing_passkey();
  const auto wifi = wifi_setup();
  next.wifi_configured = wifi.configured;
  next.wifi_enabled = wifi.on;
  next.fan_cleaning = control.cleaning_until_us != 0;
  next.reading = sources.last;
  next.sensor_present = snapshot.present;
  copy_text(next.firmware, oslogger::firmware_text_id());
  copy_text(next.thing, oslogger::thing_text_id());
  copy_text(next.sensor_serial, snapshot.present ? control.serial.data() : "");
  copy_text(next.sensor_fw, snapshot.present ? control.version.data() : "");
  copy_text(next.device_name, links_started ? ble::local_name() : "");
  xSemaphoreTake(feed_mutex, portMAX_DELAY);
  next.version = feed.version + 1;
  next.sample_seq = feed.sample_seq;
  next.sample_mono_s = feed.sample_mono_s;
  next.sample = feed.sample;
  if (sample) {
    ++next.sample_seq;
    next.sample_mono_s = static_cast<std::uint32_t>(sample_mono_us / 1000000);
    next.sample = metrics_of(sources.last);
  }
  feed = next;
  xSemaphoreGive(feed_mutex);
}

struct FeedState {
  std::int64_t last_publish_us = std::numeric_limits<std::int64_t>::min() / 2;
  std::int64_t next_link_check_us = 0;
  std::uint32_t ble_generation = 0;
  std::uint32_t lan_generation = 0;
  bool pairing_active = false;
  bool cleaning = false;
  bool present = false;
};
FeedState feed_state;

void poll_feed(std::int64_t now) {
  if (!feed_mutex)
    return;
  bool due = false;
  bool sample = false;
  if (sample_pending) {
    sample_pending = false;
    due = sample = true;
  } else if (!logging && now - feed_state.last_publish_us >= kFeedTickUs) {
    // No rows without logging, so the screen samples on its own and restarts
    // the status latch the way collect_sen63c() does for a row.
    sources.last = board_modules::reading_of(snapshot, now);
    snapshot.device_status = snapshot.device_status_latest;
    sample_mono_us = now;
    due = sample = true;
  } else if (now - feed_state.last_publish_us >= kFeedTickUs) {
    due = true;
  }
  if (links_started && now >= feed_state.next_link_check_us) {
    feed_state.next_link_check_us = now + kLinkCheckUs;
    const auto ble_generation = ble::ui_generation();
    const auto lan_generation = lan::ui_generation();
    const bool pairing_active = ble::pairing().active;
    if (ble_generation != feed_state.ble_generation ||
        lan_generation != feed_state.lan_generation ||
        pairing_active != feed_state.pairing_active)
      due = true;
    feed_state.ble_generation = ble_generation;
    feed_state.lan_generation = lan_generation;
    feed_state.pairing_active = pairing_active;
  }
  // The popup shows the fan cleaning and the sensor row, so both publish at
  // once.
  const bool cleaning = control.cleaning_until_us != 0;
  if (cleaning != feed_state.cleaning || snapshot.present != feed_state.present)
    due = true;
  feed_state.cleaning = cleaning;
  feed_state.present = snapshot.present;
  if (!due)
    return;
  feed_state.last_publish_us = now;
  publish(now, sample);
}

// ---------------------------------------------------------------------------
// Keys. A 10 ms esp_timer scan debounces them and queues each new press.
// board::kKeys lists them in ui::Input order, so bit i is input i. BOOT is
// Menu and there is no Exit key, see board_io.h.

constexpr std::uint64_t kKeyScanUs = 10000;
constexpr unsigned kStableScans = 3;
static_assert(board::kKeys.size() <= 8, "keys_held() is one byte");

void scan_keys(void *) {
  static std::uint8_t last_raw = 0;
  static std::uint8_t stable = 0;
  static unsigned count = 0;
  const std::uint8_t raw = board::keys_held();
  if (raw != last_raw) {
    last_raw = raw;
    count = 0;
    return;
  }
  if (count >= kStableScans || ++count < kStableScans)
    return;
  const auto pressed = static_cast<std::uint8_t>(raw & ~stable);
  stable = raw;
  for (std::size_t i = 0; i < board::kKeys.size(); ++i) {
    if ((pressed & (1U << i)) == 0)
      continue;
    const auto input = static_cast<ui::Input>(i);
    static_cast<void>(xQueueSend(key_queue, &input, 0));
  }
}

bool start_key_scan() {
  if (!board::start_keys())
    return false;
  key_queue = xQueueCreate(8, sizeof(ui::Input));
  if (!key_queue)
    return false;
  const esp_timer_create_args_t args{.callback = scan_keys,
                                     .arg = nullptr,
                                     .dispatch_method = ESP_TIMER_TASK,
                                     .name = "os-keys",
                                     .skip_unhandled_events = true};
  esp_timer_handle_t timer = nullptr;
  return esp_timer_create(&args, &timer) == ESP_OK &&
         esp_timer_start_periodic(timer, kKeyScanUs) == ESP_OK;
}

// ---------------------------------------------------------------------------
// UI task. Owns the panel, the history, the three frames and the orientation
// tracker after init_board.

constexpr std::uint32_t kUiStackBytes = 8192;
constexpr UBaseType_t kUiPriority = 1;
constexpr BaseType_t kUiCore = 1;
// A history slot with no row for this long becomes a gap.
constexpr std::uint32_t kHistoryGapS = 20;
// A failed panel update waits this long before the next attempt.
constexpr TickType_t kDisplayRetryTicks = pdMS_TO_TICKS(5000);

constexpr const ui::DisplayProfile &kProfile =
    opensensor::epaper397::kDisplayProfile;
static_assert(kProfile.width == epd::kWidth &&
              kProfile.height == epd::kHeight &&
              kProfile.frame_bytes() == epd::kFrameBytes);
// The QMI8658A makes Auto a Rotation choice, and the default.
constexpr bool kOrientationSource = kProfile.orientation.present;
static_assert(kOrientationSource);

epd::Ssd1677 panel;
ui::History *history = nullptr;
// Pages draw the logical frame at the screen's rotation, the presenter turns
// it into the panel frame and keeps the frame on the glass, all in PSRAM.
std::optional<ui::Presenter> presenter;

ui::Model model_of(const UiFeed &f) {
  ui::Model m{};
  const auto &r = f.reading;
  const auto values = metrics_of(r);
  for (std::size_t i = 0; i < ui::kMetricCount; ++i) {
    const auto metric = static_cast<ui::Metric>(i);
    m.now[i] = ui::quality_for(metric,
                               std::isfinite(values[i])
                                   ? std::optional<float>{values[i]}
                                   : std::nullopt,
                               r.since_start_s);
  }
  m.status = {.utc_s = f.utc_s,
              .sd_ok = f.sd_ok,
              .sd_free_bytes = f.sd_free,
              .ble = f.ble,
              .wifi = f.wifi,
              .samples = f.samples,
              .uptime_s = f.uptime_s,
              .firmware = f.firmware.data(),
              .thing = f.thing.data(),
              .sensor_model = "SEN63C",
              .sensor_serial = f.sensor_serial.data(),
              .sensor_fw = f.sensor_fw.data(),
              .sensor_status = r.device_status,
              .sensor_present = f.sensor_present,
              .pairing_passkey = f.passkey,
              .device_name = f.device_name.data()};
  return m;
}

// Panel settings live in their own NVS namespace, apart from `oscfg` and
// `parquet`, the same blob as the CrowPanel. The UI task alone reads and
// writes them after init_board. This board declares no phone keys yet, so
// the knob is the only writer.
constexpr const char *kPanelNamespace = "ospanel";
constexpr const char *kPanelKey = "ui";
// A change is saved once the knob has rested this long, so turning through
// values costs one flash write. A failed save is tried again later.
constexpr std::int64_t kSaveAfterUs = 5000000;
constexpr std::int64_t kSaveRetryUs = 60000000;
// A data redraw may come this much early, the rows do not land exactly on
// the 10 s grid.
constexpr std::int64_t kRedrawSlackUs = 1000000;

ui::Settings panel_settings;

SensorSettings sensor_settings_of(const ui::Settings &s) {
  return {.altitude_m = s.altitude_m,
          .co2_asc = s.co2_asc,
          .temp_offset_dc = s.temp_offset_dc};
}

const char *rotation_text(ui::ScreenRotation r) {
  switch (r) {
  case ui::ScreenRotation::R0:
    return "0";
  case ui::ScreenRotation::R90:
    return "90";
  case ui::ScreenRotation::R180:
    return "180";
  case ui::ScreenRotation::R270:
    return "270";
  case ui::ScreenRotation::Auto:
    return "auto";
  }
  return "unknown";
}

unsigned degrees_of(ui::Rotation r) { return 90U * std::to_underlying(r); }

void log_panel_settings(const char *operation, esp_err_t error) {
  const auto sleep_s = ui::sleep_after_seconds(panel_settings);
  oslog.printf("OS PANEL settings operation=%s error=%d refresh_s=%lu "
               "start_page=%u sleep_s=%lu altitude_m=%u co2_asc=%u "
               "temp_offset_dc=%d theme=%s rotation=%s\n",
               operation, error,
               static_cast<unsigned long>(ui::refresh_seconds(panel_settings)),
               static_cast<unsigned>(ui::start_page(panel_settings)),
               static_cast<unsigned long>(sleep_s ? *sleep_s : 0),
               static_cast<unsigned>(panel_settings.altitude_m),
               panel_settings.co2_asc,
               static_cast<int>(panel_settings.temp_offset_dc),
               panel_settings.theme == ui::Theme::Dark ? "dark" : "light",
               rotation_text(panel_settings.rotation));
}

// A missing namespace or key is a first boot, so defaults and no error, and
// the default rotation is Auto here. Nothing is erased.
void load_panel_settings() {
  panel_settings = ui::defaults(kOrientationSource);
  nvs_handle_t handle = 0;
  esp_err_t error = nvs_open(kPanelNamespace, NVS_READONLY, &handle);
  if (error == ESP_OK) {
    ui::SettingsBlob blob{};
    std::size_t size = blob.size();
    error = nvs_get_blob(handle, kPanelKey, blob.data(), &size);
    nvs_close(handle);
    if (error == ESP_OK)
      panel_settings =
          ui::decode(std::span{blob.data(), size}, kOrientationSource);
  }
  if (error == ESP_ERR_NVS_NOT_FOUND)
    error = ESP_OK;
  log_panel_settings("load", error);
}

esp_err_t save_panel_settings() {
  nvs_handle_t handle = 0;
  esp_err_t error = nvs_open(kPanelNamespace, NVS_READWRITE, &handle);
  if (error == ESP_OK) {
    const auto blob = ui::encode(panel_settings);
    error = nvs_set_blob(handle, kPanelKey, blob.data(), blob.size());
    if (error == ESP_OK)
      error = nvs_commit(handle);
    nvs_close(handle);
  }
  log_panel_settings("save", error);
  return error;
}

// Hands the sensor part of a change, or a fan cleaning, to the board task.
void request_sensor(bool clean_fan) {
  xSemaphoreTake(feed_mutex, portMAX_DELAY);
  sensor_requests.settings = sensor_settings_of(panel_settings);
  if (clean_fan)
    ++sensor_requests.fan_clean_seq;
  xSemaphoreGive(feed_mutex);
}

// What the shown frame was drawn from, for changes that skip the refresh
// throttle.
struct DrawnState {
  bool sensor_present = false;
  bool fan_cleaning = false;
  bool has_values = false;
};

DrawnState drawn_state_of(const UiFeed &f) {
  const auto values = metrics_of(f.reading);
  return {.sensor_present = f.sensor_present,
          .fan_cleaning = f.fan_cleaning,
          .has_values = std::ranges::any_of(
              values, [](float v) { return std::isfinite(v); })};
}

ui::SettingsInfo settings_info_of(const UiFeed &f) {
  return {.wifi_configured = f.wifi_configured,
          .wifi_on = f.wifi_enabled,
          .sensor_present = f.sensor_present,
          .fan_cleaning = f.fan_cleaning,
          .orientation_source = kOrientationSource};
}

// The raw counts already point up at rest, the ui::Gravity convention.
ui::Gravity gravity_of(const board::Accel &a) {
  return {.x = static_cast<float>(a.x),
          .y = static_cast<float>(a.y),
          .z = static_cast<float>(a.z)};
}

// Auto rotation. The board task reads the QMI8658A every 500 ms and caches
// the raw counts, so the UI task never touches the I2C bus. Every pass of
// the UI loop, at most 200 ms apart and never during a refresh, feeds the
// cached read to the shared tracker while the setting is Auto. A fixed
// setting leaves the tracker alone. The tracker turns the picture once a new
// pose has held for 2 s, and the presenter makes that redraw full.
struct Orientation {
  ui::OrientationTracker tracker{kProfile.orientation.axes, kProfile.mount};
  bool was_auto = false;
  std::optional<ui::Rotation> logged;
};

ui::Rotation sense_rotation(Orientation &o, std::int64_t now) {
  const bool is_auto = panel_settings.rotation == ui::ScreenRotation::Auto;
  if (is_auto) {
    // Switching to Auto starts from the picture on the glass, so it only
    // turns when the device really sits another way.
    if (!o.was_auto)
      o.tracker = ui::OrientationTracker{kProfile.orientation.axes,
                                         o.logged.value_or(kProfile.mount)};
    const auto accel = board::last_accel();
    static_cast<void>(o.tracker.update(
        accel ? std::optional<ui::Gravity>{gravity_of(*accel)} : std::nullopt,
        static_cast<std::uint32_t>(now / 1000)));
  }
  o.was_auto = is_auto;
  const auto rotation =
      ui::effective_rotation(panel_settings, kProfile, o.tracker.current());
  if (o.logged != rotation) {
    oslog.printf("OS PANEL rotation=%u source=%s\n", degrees_of(rotation),
                 is_auto ? "auto" : "setting");
    o.logged = rotation;
  }
  return rotation;
}

void ui_task(void *) {
  ui::UiState state{.page = ui::start_page(panel_settings)};
  UiFeed local{};
  Orientation orientation{};
  std::uint32_t seen_version = 0;
  std::uint32_t seen_seq = 0;
  bool shown = false;
  bool panel_asleep = false;
  bool page_changed = false;
  bool forced = false;
  std::optional<std::uint32_t> shown_passkey;
  ui::Rotation shown_rotation = kProfile.mount;
  DrawnState drawn{};
  std::int64_t last_input_us = esp_timer_get_time();
  std::int64_t save_due_us = 0; // 0 when nothing waits to be saved
  std::int64_t next_redraw_us = 0;
  for (;;) {
    bool input = false;
    ui::Input key{};
    TickType_t wait = pdMS_TO_TICKS(200);
    // Without keys the screen still shows readings and the pairing passkey.
    if (!key_queue)
      vTaskDelay(wait);
    while (key_queue && xQueueReceive(key_queue, &key, wait) == pdTRUE) {
      const auto out =
          ui::handle(state, panel_settings, settings_info_of(local), key);
      page_changed = page_changed || out.page_changed;
      forced = forced || out.full_refresh;
      const auto now = esp_timer_get_time();
      if (out.effect == ui::Effect::Changed) {
        save_due_us = now + kSaveAfterUs;
        request_sensor(false);
      } else if (out.effect == ui::Effect::FanClean) {
        request_sensor(true);
      }
      last_input_us = now;
      input = true;
      wait = 0;
    }
    const auto now = esp_timer_get_time();
    if (save_due_us != 0 && now >= save_due_us)
      save_due_us = save_panel_settings() == ESP_OK ? 0 : now + kSaveRetryUs;
    xSemaphoreTake(feed_mutex, portMAX_DELAY);
    const bool changed = feed.version != seen_version;
    if (changed)
      local = feed;
    xSemaphoreGive(feed_mutex);
    if (changed) {
      seen_version = local.version;
      // The history keeps every row whatever the screen shows.
      if (local.sample_seq != seen_seq) {
        seen_seq = local.sample_seq;
        history->push(local.sample_mono_s, local.sample);
      } else if (seen_seq != 0 && !history->empty() &&
                 local.uptime_s >= local.sample_mono_s + kHistoryGapS) {
        history->advance(local.uptime_s);
      }
    }
    // A pairing phone wakes the screen, its passkey must show at once.
    const bool pairing_changed = local.passkey != shown_passkey;
    if (local.passkey) {
      if (state.asleep) {
        state.asleep = false;
        forced = true;
      }
      last_input_us = now;
    }
    bool fell_asleep = false;
    const auto sleep_s = ui::sleep_after_seconds(panel_settings);
    if (!state.asleep && sleep_s &&
        now - last_input_us >= static_cast<std::int64_t>(*sleep_s) * 1000000) {
      ui::fall_asleep(state);
      fell_asleep = forced = true;
    }
    // The user's turn on top of the mount, or the sensed one under Auto. A
    // new turn differs from the glass, so the presenter shows it at once
    // with a full refresh, the asleep screen too.
    const auto rotation = sense_rotation(orientation, now);
    const bool turned = rotation != shown_rotation;
    // The asleep screen holds no data, so only a key, a phone or a turn
    // redraws it. The refresh setting throttles new readings only. The
    // sensor coming or going, a fan cleaning and the first values after a
    // start show at once, and so does anything while the popup is open.
    const auto current = drawn_state_of(local);
    const bool urgent = current.sensor_present != drawn.sensor_present ||
                        current.fan_cleaning != drawn.fan_cleaning ||
                        (current.has_values && !drawn.has_values) ||
                        state.popup.open;
    const bool data_due =
        changed && !state.asleep && (urgent || now >= next_redraw_us);
    if (shown && !input && !pairing_changed && !fell_asleep && !data_due &&
        !turned)
      continue;
    auto canvas = presenter->canvas(rotation);
    if (!canvas) {
      // start_display validated the profile and sized the logical frame for
      // every rotation, so this does not happen. Never draw out of bounds.
      vTaskDelay(kDisplayRetryTicks);
      continue;
    }
    ui::render_screen(*canvas, state, panel_settings, settings_info_of(local),
                      model_of(local), *history);
    if (!presenter->compose() && shown && !input && !fell_asleep)
      continue;
    const auto plan = presenter->plan(page_changed, forced);
    const std::int64_t started = esp_timer_get_time();
    const auto result = presenter->present(panel, plan);
    oslog.printf(
        "OS DISPLAY refresh=%s ok=%u ms=%lld error=%s\n",
        epd::to_string(plan.mode), result.has_value(),
        static_cast<long long>((esp_timer_get_time() - started) / 1000),
        result ? "none" : epd::to_string(result.error()));
    if (!result) {
      oslog.printf("OS ERROR operation=display-show error=%s mode=%s\n",
                   epd::to_string(result.error()), epd::to_string(plan.mode));
      shown = false;
      vTaskDelay(kDisplayRetryTicks);
      continue;
    }
    shown = true;
    shown_passkey = local.passkey;
    shown_rotation = rotation;
    drawn = current;
    page_changed = false;
    forced = false;
    next_redraw_us =
        now +
        static_cast<std::int64_t>(ui::refresh_seconds(panel_settings)) *
            1000000 -
        kRedrawSlackUs;
    // Deep sleep and the ALDO3 supply off keep the asleep screen on the
    // glass. The next show() starts the controller again with a full
    // refresh.
    if (state.asleep && !panel_asleep) {
      panel.sleep();
      panel_asleep = true;
    } else if (!state.asleep) {
      panel_asleep = false;
    }
  }
}

// Panel, history and frames come before the radios, which take most of the
// internal DMA memory the panel transfer buffer needs.
bool start_display() {
  constexpr std::size_t kLogical = kProfile.logical_frame_bytes();
  constexpr std::size_t kFrame = kProfile.frame_bytes();
  feed_mutex = xSemaphoreCreateMutex();
  void *history_memory = heap_caps_aligned_alloc(
      alignof(ui::History), sizeof(ui::History), MALLOC_CAP_SPIRAM);
  auto *logical = static_cast<std::uint8_t *>(
      heap_caps_malloc(kLogical, MALLOC_CAP_SPIRAM));
  auto *frame =
      static_cast<std::uint8_t *>(heap_caps_malloc(kFrame, MALLOC_CAP_SPIRAM));
  auto *shown =
      static_cast<std::uint8_t *>(heap_caps_malloc(kFrame, MALLOC_CAP_SPIRAM));
  if (!feed_mutex || !history_memory || !logical || !frame || !shown) {
    oslog.printf(
        "OS ERROR operation=allocate mem=psram bytes=%u "
        "purpose=display\n",
        static_cast<unsigned>(sizeof(ui::History) + kLogical + 2 * kFrame));
    return false;
  }
  history = new (history_memory) ui::History();
  auto created =
      ui::Presenter::create(kProfile, std::span{logical, kLogical},
                            std::span{frame, kFrame}, std::span{shown, kFrame});
  const auto begun =
      created ? panel.begin() : std::expected<void, epd::Error>{};
  const char *error = "none";
  if (!created)
    error = ui::to_string(created.error());
  else if (!begun)
    error = epd::to_string(begun.error());
  const bool ready = created.has_value() && begun.has_value();
  oslog.printf("OS DISPLAY ready=%u panel=ssd1677 width=%u height=%u "
               "sck=11 mosi=12 cs=10 dc=9 rst=46 busy=3 power=tg28-aldo3 "
               "screen=ui history_bytes=%u error=%s\n",
               ready, static_cast<unsigned>(kProfile.width),
               static_cast<unsigned>(kProfile.height),
               static_cast<unsigned>(sizeof(ui::History)), error);
  if (created)
    presenter.emplace(*created);
  return ready;
}

bool start_ui_task() {
  return xTaskCreatePinnedToCore(ui_task, "os-ui", kUiStackBytes, nullptr,
                                 kUiPriority, nullptr, kUiCore) == pdPASS;
}

// ---------------------------------------------------------------------------
// Bring-up evidence. Each bus is scanned once at boot.

void log_scan(const char *name, board::Bus bus) {
  std::array<std::uint8_t, 16> found{};
  const std::size_t count = board::scan(bus, found);
  std::array<char, 96> list{};
  std::size_t used = 0;
  for (std::size_t i = 0; i < count && i < found.size(); ++i) {
    const int n = std::snprintf(list.data() + used, list.size() - used,
                                "%s0x%02x", i ? "," : "", found[i]);
    if (n < 0 || static_cast<std::size_t>(n) >= list.size() - used)
      break;
    used += static_cast<std::size_t>(n);
  }
  oslog.printf("OS I2C scan bus=%s count=%u addresses=%s\n", name,
               static_cast<unsigned>(count), count ? list.data() : "none");
}

void log_power() {
  const bool bus_ok = board::start_internal_bus();
  oslog.printf("OS PMIC bus ready=%u sda=%d scl=%d hz=%lu pmic=0x%02x "
               "rtc=0x%02x\n",
               bus_ok, board::kInternalSda, board::kInternalScl,
               static_cast<unsigned long>(board::kInternalI2cHz),
               board::kPmicAddress, board::kRtcAddress);
  const auto id = board::pmic_chip_id();
  const auto ldo = board::pmic_ldo_enables();
  const auto adc = board::pmic_start_adc();
  oslog.printf("OS PMIC chip=tg28 found=%u chip_id=0x%02x ldo_enables=0x%02x "
               "adc=%s error=%s\n",
               id.has_value(), id ? *id : 0, ldo ? *ldo : 0,
               adc ? "on" : "failed",
               id ? "none" : board::to_string(id.error()));
  const auto power = board::read_power();
  if (!power) {
    oslog.printf("OS POWER error=%s\n", board::to_string(power.error()));
    return;
  }
  std::array<char, 8> mv{};
  std::array<char, 8> percent{};
  std::snprintf(mv.data(), mv.size(), "%s", "null");
  std::snprintf(percent.data(), percent.size(), "%s", "null");
  if (power->battery_mv)
    std::snprintf(mv.data(), mv.size(), "%u",
                  static_cast<unsigned>(*power->battery_mv));
  if (power->battery_percent)
    std::snprintf(percent.data(), percent.size(), "%u",
                  static_cast<unsigned>(*power->battery_percent));
  oslog.printf("OS POWER vbus=%u battery=%u battery_mv=%s battery_percent=%s\n",
               power->vbus_good, power->battery_present, mv.data(),
               percent.data());
}

// QMI8658 orientation evidence. The raw axes go with every quarter turn
// change and every 30 s, so the owner test can map the IMU axes to the
// panel from these lines alone.
constexpr std::int64_t kImuLogUs = 30000000;
struct ImuLog {
  bool logged = false;
  std::optional<std::uint8_t> turn;
  std::int64_t next_us = 0;
};
constinit ImuLog imu_log{};

void log_turn(const board::Accel &a, std::optional<std::uint8_t> turn) {
  std::array<char, 8> quarter{};
  std::snprintf(quarter.data(), quarter.size(), "%s", "null");
  if (turn)
    std::snprintf(quarter.data(), quarter.size(), "%u",
                  static_cast<unsigned>(*turn));
  oslog.printf("OS IMU ax=%d ay=%d az=%d quarter=%s\n", a.x, a.y, a.z,
               quarter.data());
}

void start_orientation() {
  const auto info = board::start_imu();
  oslog.printf("OS IMU chip=qmi8658 found=%u addr=0x%02x who_am_i=0x%02x "
               "revision=0x%02x range_g=2 odr_hz=3 mode=low-power gyro=off "
               "error=%s\n",
               info.has_value(), board::kImuAddress, info ? info->who_am_i : 0,
               info ? info->revision : 0,
               info ? "none" : board::to_string(info.error()));
}

void poll_orientation(std::int64_t now) {
  const auto read = board::poll_orientation(now);
  if (!read)
    return;
  if (!*read) {
    if (imu_log.logged || now >= imu_log.next_us)
      oslog.printf("OS IMU error=%s\n", board::to_string(read->error()));
    imu_log = {
        .logged = false, .turn = std::nullopt, .next_us = now + kImuLogUs};
    return;
  }
  const auto turn = board::quarter_turn(**read);
  if (imu_log.logged && turn == imu_log.turn && now < imu_log.next_us)
    return;
  log_turn(**read, turn);
  imu_log = {.logged = true, .turn = turn, .next_us = now + kImuLogUs};
}
} // namespace

bool init_board() {
  const auto nvs_result = nvs_flash_init();
  if (nvs_result != ESP_OK) {
    oslog.printf("OS ERROR operation=nvs-init error=%d logging=false\n",
                 nvs_result);
    return false;
  }
  opensensor::log::report_coredump();
  const bool keys_ok = start_key_scan();
  oslog.printf("OS BOARD waveshare-esp32-s3-epaper-397 firmware=%s "
               "sensor=SEN63C display=ssd1677-800x480 battery=tg28 "
               "rtc=pcf85063 imu=qmi8658 console=usb-serial-jtag keys=%u\n",
               kFirmware, keys_ok);
  const bool psram_ok =
      esp_psram_is_initialized() && esp_psram_get_size() == 8 * 1024 * 1024;
  std::uint32_t flash_bytes = 0;
  const esp_err_t flash_result = esp_flash_get_size(nullptr, &flash_bytes);
  oslog.printf(
      "OS MEMORY flash_bytes=%lu flash_error=%d psram_physical_bytes=%lu "
      "psram_heap_bytes=%lu psram_ok=%u\n",
      static_cast<unsigned long>(flash_bytes), flash_result,
      static_cast<unsigned long>(esp_psram_get_size()),
      static_cast<unsigned long>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)),
      psram_ok);
  if (!psram_ok) {
    oslog.println("OS ERROR operation=psram logging=false");
    return false;
  }
  load_panel_settings();
  sensor_requests.settings = sensor_settings_of(panel_settings);
  control.wanted = sensor_requests.settings;
  // The PMIC first, the panel supply hangs off it.
  log_power();
  log_scan("internal", board::Bus::Internal);
  start_orientation();
  const bool display_ok = start_display();
  const bool bus_ok = board::start_sensor_bus(Sen6x::kAddress);
  board::sensor_delay_ms(nullptr, Sen6x::kPowerUpMs);
  oslog.printf("OS SEN6X bus ready=%u sda=%d scl=%d addr=0x%02x hz=%lu "
               "pullups=internal external_10k_required=1\n",
               bus_ok, board::kSensorSda, board::kSensorScl, Sen6x::kAddress,
               static_cast<unsigned long>(board::kSensorI2cHz));
  if (bus_ok) {
    log_scan("sensor", board::Bus::Sensor);
    const auto now = esp_timer_get_time();
    probe(now);
    if (snapshot.present)
      start(esp_timer_get_time());
    control.next_poll_us = esp_timer_get_time() + kSensorPollUs;
  }
  const bool mounted = board::mount_card();
  oslog.printf("OS TF mounted=%u size_bytes=%llu dma_buffer=%u bus=sdmmc "
               "width=4 clk=%d cmd=%d d0=%d d1=%d d2=%d d3=%d "
               "format_attempted=false\n",
               mounted, static_cast<unsigned long long>(board::card_bytes()),
               board::sd_buffer_reserved(), board::kSdClk, board::kSdCmd,
               board::kSdData0, board::kSdData1, board::kSdData2,
               board::kSdData3);
  logging = oslogger::begin(kLoggerConfig, kHooks, mounted);
  const bool ui_ok = display_ok && start_ui_task();
  if (display_ok && !ui_ok)
    oslog.println("OS ERROR operation=ui-task-create");
  if (logging) {
    links_started = oslogger::start_links(display_ok);
    if (!links_started)
      oslog.println("OS ERROR operation=sync-start");
  }
  oslog.printf("OS READY logging=%u mounted=%u sample_ms=10000 display=%u "
               "sensor=%u\n",
               logging, mounted, ui_ok, snapshot.present);
  if (ui_ok)
    publish(esp_timer_get_time(), false);
  return true;
}

void poll_board() {
  const auto now = esp_timer_get_time();
  poll_sensor(now);
  poll_orientation(now);
  if (logging)
    oslogger::poll();
  poll_feed(esp_timer_get_time());
  const auto ticks = pdMS_TO_TICKS(2);
  vTaskDelay(ticks ? ticks : 1);
}

namespace {
void board_task(void *) {
  if (!init_board()) {
    vTaskDelete(nullptr);
    return;
  }
  for (;;)
    poll_board();
}
} // namespace

extern "C" void app_main() {
  if (!opensensor::console::begin())
    return;
  opensensor::log::attach_persistent(); // PSRAM log, the internal ring
                                        // otherwise
  opensensor::power::configure_dfs();
  if (xTaskCreate(board_task, "os-board", 12288, nullptr,
                  oslogger::kBoardTaskPriority, nullptr) != pdPASS)
    oslog.println("OS ERROR operation=board-task-create logging=false");
}
