#include "../board_io.h"
#include "canvas.h"
#include "epd_uc8276c.h"
#include "history.h"
#include "pages.h"
#include "sensor_modules.h"
#include "telemetry_contract.h"
#include "ui_model.h"
#include <array>
#include <ble_sync.h>
#include <cmath>
#include <cstring>
#include <debug_log.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_psram.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <limits>
#include <log_persist.h>
#include <new>
#include <nvs_flash.h>
#include <os_console.h>
#include <os_logger.h>
#include <os_sen6x.h>
#include <power.h>
#include <wifi_link.h>

#ifndef CONFIG_SPIRAM
#error The CrowPanel logger requires the qualified 8 MB OPI PSRAM target.
#endif

namespace {
using namespace telemetry::contract;
namespace ui = opensensor::ui;
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
};
SensorControl control;

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
  control.failures = 0;
  control.absent_logged = false;
  control.next_attempt_us = now;
  read_status(now);
}

void start(std::int64_t now) {
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
    static_cast<void>(sen6x.stop_measurement());
    snapshot.measuring = false;
    snapshot.has_values = false;
    control.next_attempt_us = now;
  }
}

void poll_sensor(std::int64_t now) {
  if (now < control.next_poll_us)
    return;
  control.next_poll_us = now + kSensorPollUs;
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

// No STAC File Item sidecar yet (#142). stac-item-test proves every board that
// sets Config::stac_columns against a stac-opensensor example, and that repo
// has no CrowPanel example. Opt in once it does.

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
  Text firmware{};
  Text thing{};
  Text sensor_serial{};
  Text sensor_fw{};
  Text device_name{};
};
UiFeed feed;
SemaphoreHandle_t feed_mutex = nullptr;
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
    // No rows without logging, so the screen samples on its own.
    sources.last = board_modules::reading_of(snapshot, now);
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
  if (!due)
    return;
  feed_state.last_publish_us = now;
  publish(now, sample);
}

// ---------------------------------------------------------------------------
// Keys. A 10 ms esp_timer scan debounces them and queues each new press.

constexpr std::uint64_t kKeyScanUs = 10000;
constexpr unsigned kStableScans = 3;

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
// UI task. Owns the panel, the history and both frames after init_board.

constexpr std::uint32_t kUiStackBytes = 8192;
constexpr UBaseType_t kUiPriority = 1;
constexpr BaseType_t kUiCore = 1;
// A history slot with no row for this long becomes a gap.
constexpr std::uint32_t kHistoryGapS = 20;
// A failed panel update waits this long before the next attempt.
constexpr TickType_t kDisplayRetryTicks = pdMS_TO_TICKS(5000);

epd::Panel panel;
ui::History *history = nullptr;
std::uint8_t *draw_frame = nullptr;
std::uint8_t *shown_frame = nullptr;

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
              .sensor_serial = f.sensor_serial.data(),
              .sensor_fw = f.sensor_fw.data(),
              .sensor_status = r.device_status,
              .sensor_present = f.sensor_present,
              .pairing_passkey = f.passkey,
              .device_name = f.device_name.data()};
  return m;
}

void ui_task(void *) {
  std::uint8_t page = ui::kDashboardPage;
  ui::RefreshPolicy policy;
  UiFeed local{};
  std::uint32_t seen_version = 0;
  std::uint32_t seen_seq = 0;
  bool shown = false;
  bool page_changed = false;
  bool forced = false;
  const ui::Frame draw{draw_frame, ui::kFrameBytes};
  for (;;) {
    bool input = false;
    ui::Input key{};
    TickType_t wait = pdMS_TO_TICKS(200);
    while (xQueueReceive(key_queue, &key, wait) == pdTRUE) {
      const auto nav = ui::navigate(page, key);
      page_changed = page_changed || nav.page != page;
      forced = forced || nav.full_refresh;
      page = nav.page;
      input = true;
      wait = 0;
    }
    xSemaphoreTake(feed_mutex, portMAX_DELAY);
    const bool changed = feed.version != seen_version;
    if (changed)
      local = feed;
    xSemaphoreGive(feed_mutex);
    if (!changed && !input && shown)
      continue;
    seen_version = local.version;
    if (local.sample_seq != seen_seq) {
      seen_seq = local.sample_seq;
      history->push(local.sample_mono_s, local.sample);
    } else if (!history->empty() &&
               local.uptime_s >= local.sample_mono_s + kHistoryGapS) {
      history->advance(local.uptime_s);
    }
    ui::Canvas canvas{draw};
    ui::render(canvas, page, model_of(local), *history);
    if (shown && !input &&
        std::memcmp(draw_frame, shown_frame, ui::kFrameBytes) == 0)
      continue;
    const auto mode = policy.next(page_changed, forced) == ui::Refresh::Full
                          ? epd::Mode::Full
                          : epd::Mode::Partial;
    const auto result =
        panel.show(epd::Frame{draw_frame, ui::kFrameBytes}, mode);
    if (!result) {
      oslog.printf("OS ERROR operation=display-show error=%s mode=%s\n",
                   epd::to_string(result.error()),
                   mode == epd::Mode::Full ? "full" : "partial");
      shown = false;
      vTaskDelay(kDisplayRetryTicks);
      continue;
    }
    std::memcpy(shown_frame, draw_frame, ui::kFrameBytes);
    shown = true;
    page_changed = false;
    forced = false;
  }
}

// Panel, history and frames come before the radios, which take most of the
// internal DMA memory the panel needs.
bool start_display() {
  feed_mutex = xSemaphoreCreateMutex();
  void *history_memory = heap_caps_aligned_alloc(
      alignof(ui::History), sizeof(ui::History), MALLOC_CAP_SPIRAM);
  draw_frame = static_cast<std::uint8_t *>(
      heap_caps_malloc(ui::kFrameBytes, MALLOC_CAP_SPIRAM));
  shown_frame = static_cast<std::uint8_t *>(
      heap_caps_malloc(ui::kFrameBytes, MALLOC_CAP_SPIRAM));
  if (!feed_mutex || !history_memory || !draw_frame || !shown_frame) {
    oslog.printf(
        "OS ERROR operation=allocate mem=psram bytes=%u "
        "purpose=display\n",
        static_cast<unsigned>(sizeof(ui::History) + 2 * ui::kFrameBytes));
    return false;
  }
  history = new (history_memory) ui::History();
  const auto begun = panel.begin();
  const epd::Pins pins{};
  oslog.printf("OS DISPLAY ready=%u panel=uc8276c width=%d height=%d "
               "sck=%d mosi=%d cs=%d dc=%d rst=%d busy=%d power=%d "
               "history_bytes=%u error=%s\n",
               begun.has_value(), ui::kWidth, ui::kHeight, pins.sck, pins.mosi,
               pins.cs, pins.dc, pins.rst, pins.busy, pins.power,
               static_cast<unsigned>(sizeof(ui::History)),
               begun ? "none" : epd::to_string(begun.error()));
  return begun.has_value();
}

bool start_ui_task() {
  return xTaskCreatePinnedToCore(ui_task, "os-ui", kUiStackBytes, nullptr,
                                 kUiPriority, nullptr, kUiCore) == pdPASS;
}

// ---------------------------------------------------------------------------
// Indicator. Off without logging or a card, on while the sensor is valid,
// 1 Hz blink while it warms up or reports a fault.

void update_indicator(std::int64_t now) {
  const bool card = board::card_mounted();
  const bool valid =
      board_modules::status_of(snapshot, now) == ossensor::sen63c::kStatusValid;
  const bool on = logging && card && (valid || (now / 500000) % 2 == 0);
  static int previous = -1;
  if (static_cast<int>(on) != previous) {
    board::indicator(on);
    previous = on;
  }
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
  board::start_indicator();
  const bool keys_ok = start_key_scan();
  oslog.printf("OS BOARD elecrow-crowpanel-epaper-42 firmware=%s "
               "sensor=SEN63C display=uc8276c-400x300 battery=absent "
               "rtc=absent keys=%u\n",
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
  const bool display_ok = start_display();
  const bool bus_ok = board::start_sensor_bus(Sen6x::kAddress);
  vTaskDelay(pdMS_TO_TICKS(Sen6x::kPowerUpMs));
  oslog.printf("OS SEN6X bus ready=%u sda=%d scl=%d addr=0x%02x hz=%lu "
               "pullups=internal external_10k_required=1\n",
               bus_ok, board::kSensorSda, board::kSensorScl, Sen6x::kAddress,
               static_cast<unsigned long>(board::kSensorI2cHz));
  if (bus_ok) {
    const auto now = esp_timer_get_time();
    probe(now);
    if (snapshot.present)
      start(esp_timer_get_time());
    control.next_poll_us = esp_timer_get_time() + kSensorPollUs;
  }
  const bool mounted = board::mount_card();
  oslog.printf("OS TF mounted=%u size_bytes=%llu mosi=%d miso=%d clk=%d "
               "cs=%d power=%d bus=spi3 format_attempted=false\n",
               mounted, static_cast<unsigned long long>(board::card_bytes()),
               board::kSdMosi, board::kSdMiso, board::kSdClk, board::kSdCs,
               board::kSdPower);
  logging = oslogger::begin(kLoggerConfig, kHooks, mounted);
  if (logging) {
    links_started = oslogger::start_links(display_ok);
    if (!links_started)
      oslog.println("OS ERROR operation=sync-start");
  }
  const bool ui_ok = display_ok && start_ui_task();
  if (display_ok && !ui_ok)
    oslog.println("OS ERROR operation=ui-task-create");
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
  if (logging)
    oslogger::poll();
  poll_feed(esp_timer_get_time());
  update_indicator(now);
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
