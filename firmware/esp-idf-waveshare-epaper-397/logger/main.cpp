#include "../board_io.h"
#include "bringup_screen.h"
#include "epd_ssd1677.h"
#include "sensor_modules.h"
#include "telemetry_contract.h"
#include <algorithm>
#include <array>
#include <ble_sync.h>
#include <cmath>
#include <cstdio>
#include <debug_log.h>
#include <device_config.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_psram.h>
#include <esp_timer.h>
#include <expected>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <limits>
#include <log_persist.h>
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

// Sensor settings. This board has no settings screen or phone keys yet, so
// they stay at the defaults and the board task applies them. Altitude and CO2
// self calibration take idle mode only and the sensor forgets all three on a
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
    .storage_remount = [](void *) { return board::remount_card(); },
    .rtc_read = board::rtc_read,
    .rtc_write = board::rtc_write,
};

bool logging = false;
bool links_started = false;

// ---------------------------------------------------------------------------
// TEMPORARY bring-up screen until the board-neutral e-paper UI lands. The
// board task publishes the row values, the display task copies them under
// the mutex and redraws.

struct ScreenFeed {
  std::uint32_t version = 0;
  opensensor::bringup::Values values{};
};
ScreenFeed screen_feed;
std::uint8_t *screen_frame = nullptr; // PSRAM, epd::kFrameBytes

// The screen samples on its own this often while logging is off.
constexpr std::int64_t kFeedTickUs = 10000000;
std::int64_t last_publish_us = std::numeric_limits<std::int64_t>::min() / 2;

constexpr std::uint32_t kDisplayStackBytes = 6144;
constexpr UBaseType_t kDisplayPriority = 1;
constexpr BaseType_t kDisplayCore = 1;
// A Fast refresh at most this often, and a Full one every kFullEvery
// refreshes to clear the ghosting the fast waveform leaves.
constexpr TickType_t kRedrawTicks = pdMS_TO_TICKS(60000);
constexpr unsigned kFullEvery = 30;

// Board task only.
void publish(bool sample) {
  if (!feed_mutex)
    return;
  opensensor::bringup::Values next{};
  const Sen63cReading &r = sources.last;
  next.pm25_ug_m3 = r.pm25_ug_m3;
  if (r.co2_ppm)
    next.co2_ppm = static_cast<float>(*r.co2_ppm);
  next.temperature_c = r.temperature_c;
  next.relative_humidity_percent = r.relative_humidity_percent;
  next.sensor_present = snapshot.present;
  next.card_mounted = board::card_mounted();
  next.logging = logging;
  next.rows = row_count;
  xSemaphoreTake(feed_mutex, portMAX_DELAY);
  if (sample || screen_feed.version == 0)
    screen_feed.values = next;
  else
    screen_feed.values.card_mounted = next.card_mounted;
  ++screen_feed.version;
  xSemaphoreGive(feed_mutex);
}

void poll_feed(std::int64_t now) {
  if (sample_pending) {
    sample_pending = false;
    last_publish_us = now;
    publish(true);
  } else if (now - last_publish_us >= kFeedTickUs) {
    // No rows without logging, so the screen samples on its own and restarts
    // the status latch the way collect_sen63c() does for a row.
    if (!logging) {
      sources.last = board_modules::reading_of(snapshot, now);
      snapshot.device_status = snapshot.device_status_latest;
    }
    last_publish_us = now;
    publish(!logging);
  }
}

void display_task(void *) {
  std::uint32_t drawn_version = 0;
  unsigned refreshes = 0;
  bool shown = false;
  opensensor::bringup::Values drawn{};
  for (;;) {
    xSemaphoreTake(feed_mutex, portMAX_DELAY);
    const ScreenFeed copy = screen_feed;
    xSemaphoreGive(feed_mutex);
    const auto &v = copy.values;
    const bool changed =
        !shown || v.pm25_ug_m3 != drawn.pm25_ug_m3 ||
        v.co2_ppm != drawn.co2_ppm || v.temperature_c != drawn.temperature_c ||
        v.relative_humidity_percent != drawn.relative_humidity_percent ||
        v.sensor_present != drawn.sensor_present ||
        v.card_mounted != drawn.card_mounted || v.logging != drawn.logging;
    if (copy.version != drawn_version && changed) {
      opensensor::bringup::render(
          std::span<std::uint8_t, opensensor::bringup::kFrameBytes>(
              screen_frame, opensensor::bringup::kFrameBytes),
          v);
      const auto mode =
          refreshes % kFullEvery == 0 ? epd::Refresh::Full : epd::Refresh::Fast;
      const std::int64_t started = esp_timer_get_time();
      const auto result =
          epd::show({screen_frame, epd::kFrameBytes}, mode, std::nullopt);
      oslog.printf(
          "OS DISPLAY refresh=%s ok=%u ms=%lld error=%s\n",
          mode == epd::Refresh::Full ? "full" : "fast", result.has_value(),
          static_cast<long long>((esp_timer_get_time() - started) / 1000),
          result ? "none" : epd::to_string(result.error()));
      // The panel sleeps between refreshes, the image stays on the glass.
      epd::sleep();
      if (result) {
        ++refreshes;
        shown = true;
        drawn = v;
      }
      drawn_version = copy.version;
    }
    vTaskDelay(kRedrawTicks);
  }
}

// Before the radios, which take most of the internal DMA memory the panel
// transfer buffer needs.
bool start_display() {
  feed_mutex = xSemaphoreCreateMutex();
  screen_frame = static_cast<std::uint8_t *>(
      heap_caps_malloc(epd::kFrameBytes, MALLOC_CAP_SPIRAM));
  if (!feed_mutex || !screen_frame) {
    oslog.printf("OS ERROR operation=allocate mem=psram bytes=%u "
                 "purpose=display\n",
                 static_cast<unsigned>(epd::kFrameBytes));
    return false;
  }
  const auto begun = epd::init();
  oslog.printf("OS DISPLAY ready=%u panel=ssd1677 width=%u height=%u "
               "sck=11 mosi=12 cs=10 dc=9 rst=46 busy=3 power=tg28-aldo3 "
               "screen=bringup error=%s\n",
               begun.has_value(), static_cast<unsigned>(epd::kWidth),
               static_cast<unsigned>(epd::kHeight),
               begun ? "none" : epd::to_string(begun.error()));
  if (!begun)
    return false;
  return xTaskCreatePinnedToCore(display_task, "os-epd", kDisplayStackBytes,
                                 nullptr, kDisplayPriority, nullptr,
                                 kDisplayCore) == pdPASS;
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
} // namespace

bool init_board() {
  const auto nvs_result = nvs_flash_init();
  if (nvs_result != ESP_OK) {
    oslog.printf("OS ERROR operation=nvs-init error=%d logging=false\n",
                 nvs_result);
    return false;
  }
  opensensor::log::report_coredump();
  oslog.printf("OS BOARD waveshare-esp32-s3-epaper-397 firmware=%s "
               "sensor=SEN63C display=ssd1677-800x480 battery=tg28 "
               "rtc=pcf85063 console=usb-serial-jtag\n",
               kFirmware);
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
  // The PMIC first, the panel supply hangs off it.
  log_power();
  log_scan("internal", board::Bus::Internal);
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
  oslog.printf("OS TF mounted=%u size_bytes=%llu bus=sdmmc width=4 clk=%d "
               "cmd=%d d0=%d d1=%d d2=%d d3=%d format_attempted=false\n",
               mounted, static_cast<unsigned long long>(board::card_bytes()),
               board::kSdClk, board::kSdCmd, board::kSdData0, board::kSdData1,
               board::kSdData2, board::kSdData3);
  logging = oslogger::begin(kLoggerConfig, kHooks, mounted);
  if (logging) {
    links_started = oslogger::start_links(display_ok);
    if (!links_started)
      oslog.println("OS ERROR operation=sync-start");
  }
  oslog.printf("OS READY logging=%u mounted=%u sample_ms=10000 display=%u "
               "sensor=%u\n",
               logging, mounted, display_ok, snapshot.present);
  publish(true);
  return true;
}

void poll_board() {
  const auto now = esp_timer_get_time();
  poll_sensor(now);
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
