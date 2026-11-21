#include "ble_sync.h"
#include "debug_log.h"
#include "device_config.h"
#include "log_persist.h"
#include "power.h"
#include "screen_header.h"
#include "telemetry_logger.h"
#include "wifi_link.h"
#include <M5Unified.h>
#include <pms_frame.h>
#include <sd_transfer_buffer.h>

#include "os_console.h"
#include <driver/sdspi_host.h>
#include <driver/uart.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_psram.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <fat_volume.h>
#include <flash_volume.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <os_logger.h>
#include <sdkconfig.h>
#include <sdmmc_cmd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <numeric>
#include <string_view>

namespace {

constexpr std::uint32_t kI2cScanFrequency = 100000;
constexpr std::uint32_t kPmsProbeDurationMs = 5000;
constexpr std::uint32_t kDisplayIntervalMs = 10000;
constexpr std::uint8_t kScreenPageCount = 4;
constexpr bool kEnableBle = true;
constexpr int kMinimumSwipeDistance = 40;
constexpr int kSdSck = 36;
constexpr int kSdMiso = 35;
constexpr int kSdMosi = 37;
constexpr int kSdCs = 4;
constexpr int kPmsRx = 18;
constexpr int kPmsTx = 17;

constexpr uart_port_t kPmsPort = UART_NUM_1;
bool pms_uart_ready = false;
sdmmc_card_t *sd_card = nullptr;
opensensor::storage::SdTransferBuffer sd_transfer_buffer;

std::uint32_t monotonic_ms() {
  return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

void wait_ms(std::uint32_t milliseconds) {
  const auto ticks = pdMS_TO_TICKS(milliseconds);
  vTaskDelay(ticks ? ticks : 1);
}

int read_pms_byte() {
  if (!pms_uart_ready)
    return -1;
  std::uint8_t byte;
  return uart_read_bytes(kPmsPort, &byte, 1, 0) == 1 ? byte : -1;
}

using PmsFrame = plantower::Frame;
plantower::Parser pms_parser{plantower::Model::Pmsa003};
PmsFrame latest_pms_frame{};
bool has_pms_frame = false;
bool sd_mounted = false;
std::int64_t latest_pms_mono_us = 0;
std::uint32_t pms_frame_count = 0;
std::uint32_t last_display_ms = 0;
std::uint8_t screen_page = 0;
std::uint32_t last_ble_ui_generation = 0;
bool ble_started = false;

const char *board_name(m5::board_t board) {
  switch (board) {
  case m5::board_t::board_M5StackCoreS3:
    return "M5Stack CoreS3";
  case m5::board_t::board_M5StackCoreS3SE:
    return "M5Stack CoreS3 SE";
  default:
    return "unexpected board";
  }
}

const char *flash_mode_name() {
#if CONFIG_ESPTOOLPY_FLASHMODE_QIO
  return "qio";
#elif CONFIG_ESPTOOLPY_FLASHMODE_QOUT
  return "qout";
#elif CONFIG_ESPTOOLPY_FLASHMODE_DIO
  return "dio";
#elif CONFIG_ESPTOOLPY_FLASHMODE_DOUT
  return "dout";
#else
  return "unknown";
#endif
}

const char *reset_reason_name(esp_reset_reason_t reason) {
  switch (reason) {
  case ESP_RST_POWERON:
    return "power-on";
  case ESP_RST_EXT:
    return "external-pin";
  case ESP_RST_SW:
    return "software";
  case ESP_RST_PANIC:
    return "panic";
  case ESP_RST_INT_WDT:
    return "interrupt-watchdog";
  case ESP_RST_TASK_WDT:
    return "task-watchdog";
  case ESP_RST_WDT:
    return "other-watchdog";
  case ESP_RST_DEEPSLEEP:
    return "deep-sleep";
  case ESP_RST_BROWNOUT:
    return "brownout";
  case ESP_RST_SDIO:
    return "sdio";
  case ESP_RST_USB:
    return "usb";
  case ESP_RST_JTAG:
    return "jtag";
  case ESP_RST_EFUSE:
    return "efuse";
  case ESP_RST_PWR_GLITCH:
    return "power-glitch";
  case ESP_RST_CPU_LOCKUP:
    return "cpu-lockup";
  default:
    return "unknown";
  }
}

// Same rule as the charging_status column, so DIAG, the device page and the
// header battery mark agree on charging.
bool is_charging(m5::charge_state_t state) {
  return m5::charge_states_any_charging.contains(state);
}

// Charging, discharging and unknown names for DIAG and the screen.
const char *charging_name(m5::charge_state_t state) {
  if (is_charging(state))
    return "charging";
  if (m5::charge_states_any_not_charging.contains(state))
    return "discharging";
  return "unknown";
}

const char *i2c_device_name(std::uint8_t address) {
  switch (address) {
  case 0x21:
    return "GC0308-camera";
  case 0x23:
    return "LTR553-proximity";
  case 0x34:
    return "AXP2101-power";
  case 0x36:
    return "AW88298-amplifier";
  case 0x38:
    return "FT6336U-touch";
  case 0x40:
    return "ES7210-codec-or-SHT20-address-collision";
  case 0x51:
    return "BM8563-RTC";
  case 0x58:
    return "AW9523B-expander";
  case 0x69:
    return "BMI270-IMU";
  default:
    return "unexpected";
  }
}

void report_chip() {
  esp_chip_info_t chip{};
  esp_chip_info(&chip);
  std::uint8_t mac[6]{};
  const esp_err_t mac_result = esp_read_mac(mac, ESP_MAC_WIFI_STA);

  oslog.printf(
      "DIAG chip model=%s revision=%u cores=%u features=0x%08lx reset=%s(%d) "
      "board=%s(%d)\n",
      "ESP32-S3", static_cast<unsigned>(chip.revision),
      static_cast<unsigned>(chip.cores),
      static_cast<unsigned long>(chip.features),
      reset_reason_name(esp_reset_reason()),
      static_cast<int>(esp_reset_reason()), board_name(M5.getBoard()),
      static_cast<int>(M5.getBoard()));
  if (mac_result == ESP_OK) {
    oslog.printf("DIAG identity wifi_sta_mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  } else {
    oslog.printf("DIAG identity wifi_sta_mac=unavailable error=%d\n",
                 mac_result);
  }
  std::uint32_t flash_bytes = 0;
  const esp_err_t flash_result = esp_flash_get_size(nullptr, &flash_bytes);
  oslog.printf("DIAG flash bytes=%lu size_error=%d configured_speed=%s "
               "configured_mode=%s\n",
               static_cast<unsigned long>(flash_bytes), flash_result,
               CONFIG_ESPTOOLPY_FLASHFREQ, flash_mode_name());
}

void report_memory() {
  oslog.printf(
      "DIAG memory heap_total=%lu heap_free=%lu heap_min_free=%lu "
      "heap_largest_internal=%lu psram_found=%s psram_total=%lu psram_free=%lu "
      "psram_largest=%lu\n",
      static_cast<unsigned long>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL)),
      static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
      static_cast<unsigned long>(
          heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
      static_cast<unsigned long>(heap_caps_get_largest_free_block(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      esp_psram_is_initialized() ? "true" : "false",
      static_cast<unsigned long>(esp_psram_get_size()),
      static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
      static_cast<unsigned long>(
          heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

void report_dma_memory(const char *stage) {
  constexpr auto caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
  oslog.printf(
      "DIAG dma stage=%s free=%lu largest=%lu min_free=%lu "
      "sd_buffer_bytes=%u\n",
      stage, static_cast<unsigned long>(heap_caps_get_free_size(caps)),
      static_cast<unsigned long>(heap_caps_get_largest_free_block(caps)),
      static_cast<unsigned long>(heap_caps_get_minimum_free_size(caps)),
      sd_transfer_buffer.data()
          ? static_cast<unsigned>(opensensor::storage::SdTransferBuffer::kBytes)
          : 0);
}

void report_i2c() {
  unsigned found = 0;
  unsigned unexpected = 0;
  // M5Unified excludes reserved addresses 0x00-0x07 and 0x78-0x7f because
  // probing the low range can stop the ESP32-S3 I2C controller.
  for (std::uint8_t address = 8; address < 0x78; ++address) {
    if (!M5.In_I2C.scanID(address, kI2cScanFrequency)) {
      continue;
    }
    ++found;
    const char *name = i2c_device_name(address);
    if (std::strcmp(name, "unexpected") == 0) {
      ++unexpected;
    }
    oslog.printf("DIAG i2c address=0x%02x device=%s\n", address, name);
  }
  oslog.printf(
      "DIAG i2c_summary sda=12 scl=11 frequency_hz=%lu found=%u unexpected=%u "
      "note_bmm150=behind_bmi270_aux_bus\n",
      static_cast<unsigned long>(kI2cScanFrequency), found, unexpected);
}

void report_power() {
  // Status returning reads, so a failed read prints null, not 0 (#199).
  const auto supply = telemetry::read_power_now();
  const auto charging = M5.Power.getChargeState();
  namespace pw = telemetry::power;
  pw::IntText vbus_buffer;
  pw::IntText battery_buffer;
  pw::IntText percent_buffer;
  oslog.printf(
      "DIAG power pmic_type=%d external_5v=%s usb_output=%s vbus_mv=%s "
      "battery_mv=%s battery_percent=%s charging=%s battery_current_ma=%ld\n",
      static_cast<int>(M5.Power.getType()),
      pw::bool_text(supply.external_5v, "on", "off", "null"),
      pw::bool_text(supply.usb_output, "source", "input", "null"),
      pw::int_text(supply.vbus_mv, vbus_buffer, "null"),
      pw::int_text(supply.battery_mv, battery_buffer, "null"),
      pw::int_text(supply.battery_percent, percent_buffer, "null"),
      charging_name(charging), static_cast<long>(M5.Power.getBatteryCurrent()));
}

void report_rtc_imu_touch() {
  m5::rtc_datetime_t datetime{};
  if (M5.Rtc.isEnabled() && M5.Rtc.getDateTime(&datetime)) {
    oslog.printf(
        "DIAG rtc enabled=true date=%04d-%02d-%02d time=%02d:%02d:%02d\n",
        datetime.date.year, datetime.date.month, datetime.date.date,
        datetime.time.hours, datetime.time.minutes, datetime.time.seconds);
  } else {
    oslog.println("DIAG rtc enabled=false_or_read_failed");
  }

  M5.Imu.update();
  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;
  float gx = 0.0f;
  float gy = 0.0f;
  float gz = 0.0f;
  const bool accel_ok = M5.Imu.isEnabled() && M5.Imu.getAccel(&ax, &ay, &az);
  const bool gyro_ok = M5.Imu.isEnabled() && M5.Imu.getGyro(&gx, &gy, &gz);
  oslog.printf("DIAG imu enabled=%s type=%d accel_ok=%s accel_g=%.4f,%.4f,%.4f "
               "gyro_ok=%s gyro_dps=%.4f,%.4f,%.4f\n",
               M5.Imu.isEnabled() ? "true" : "false",
               static_cast<int>(M5.Imu.getType()), accel_ok ? "true" : "false",
               ax, ay, az, gyro_ok ? "true" : "false", gx, gy, gz);

  M5.update();
  oslog.printf("DIAG touch controller_ack=%s active_points=%u\n",
               M5.In_I2C.scanID(0x38, kI2cScanFrequency) ? "true" : "false",
               static_cast<unsigned>(M5.Touch.getCount()));
}

// SDSPI attach and FAT mount on the M5GFX-owned bus. Never formats. The DMA
// buffer stays reserved after a failed mount: once radios run, internal DMA
// memory may be too fragmented to reserve it again (#168).
esp_err_t mount_sd() {
  // M5GFX owns SPI2 initialization, power and the startup SPI-mode handshake.
  // GPIO35 is shared LCD DC/SD MISO; finish display DMA/transactions before
  // attaching SDSPI. Never independently initialize or free the shared bus.
  M5.Display.waitDMA();
  M5.Display.endWrite();
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SPI2_HOST;
  // IDF treats any request above 20 MHz as a card high-speed mode switch.
  // This shared LCD bus/card failed the subsequent CSD check at 25 MHz on
  // the first native flash; retain default-speed mode until qualified.
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;
  // FatFs metadata/file buffers may live in PSRAM. Reserve the DMA bounce
  // buffer now, before Wi-Fi/TCP bursts compete for internal RAM. The SDK's
  // ALLOC_ALIGNED_BUF flag only allocates automatically for non-SPI hosts.
  if (!sd_transfer_buffer.reserve())
    return ESP_ERR_NO_MEM;
  host.dma_aligned_buffer = sd_transfer_buffer.data();
  host.unaligned_multi_block_rw_max_chunk_size =
      opensensor::storage::SdTransferBuffer::kSectors;
  sdspi_device_config_t device = SDSPI_DEVICE_CONFIG_DEFAULT();
  device.host_id = SPI2_HOST;
  device.gpio_cs = static_cast<gpio_num_t>(kSdCs);
  const esp_vfs_fat_sdmmc_mount_config_t mount =
      opensensor::storage::archive_mount_config();
  return esp_vfs_fat_sdspi_mount("/sd", &host, &device, &mount, &sd_card);
}

bool report_sd() {
  if (sd_card)
    return true;
  const auto board = M5.getBoard();
  if (board != m5::board_t::board_M5StackCoreS3 &&
      board != m5::board_t::board_M5StackCoreS3SE) {
    oslog.println("DIAG sd mounted=false status=unexpected_board "
                  "format_attempted=false");
    return false;
  }
  if (!sd_transfer_buffer.reserve()) {
    oslog.println("DIAG sd mounted=false status=dma_buffer_unavailable "
                  "format_attempted=false");
    return false;
  }
  const esp_err_t result = mount_sd();
  if (result != ESP_OK) {
    oslog.printf("DIAG sd mounted=false status=missing_or_mount_failed "
                 "error=%d format_attempted=false\n",
                 result);
    return false;
  }
  std::uint64_t total = 0, free = 0;
  const esp_err_t capacity = esp_vfs_fat_info("/sd", &total, &free);
  oslog.printf(
      "DIAG sd mounted=true card_bytes=%llu total_bytes=%llu "
      "used_bytes=%llu capacity_error=%d cluster_bytes=%lu clock_hz=%d "
      "spi_host=SPI2 sck=%d miso=%d mosi=%d cs=%d dma_buffer_bytes=%u\n",
      static_cast<unsigned long long>(sd_card->csd.capacity) *
          sd_card->csd.sector_size,
      static_cast<unsigned long long>(total),
      static_cast<unsigned long long>(total >= free ? total - free : 0),
      capacity,
      static_cast<unsigned long>(
          opensensor::storage::fat_cluster_bytes(sd_card)),
      sd_card->real_freq_khz * 1000, kSdSck, kSdMiso, kSdMosi, kSdCs,
      static_cast<unsigned>(opensensor::storage::SdTransferBuffer::kBytes));
  return true;
}

bool start_pms() {
  uart_config_t uart{};
  uart.baud_rate = 9600;
  uart.data_bits = UART_DATA_8_BITS;
  uart.parity = UART_PARITY_DISABLE;
  uart.stop_bits = UART_STOP_BITS_1;
  uart.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart.source_clk = UART_SCLK_DEFAULT;
  const esp_err_t configured = uart_param_config(kPmsPort, &uart);
  const esp_err_t pins =
      configured == ESP_OK
          ? uart_set_pin(kPmsPort, kPmsTx, kPmsRx, UART_PIN_NO_CHANGE,
                         UART_PIN_NO_CHANGE)
          : configured;
  const esp_err_t installed =
      pins == ESP_OK ? uart_driver_install(kPmsPort, 2048, 0, 0, nullptr, 0)
                     : pins;
  if (installed != ESP_OK) {
    oslog.printf("DIAG pmsa003 status=uart_init_failed error=%d\n", installed);
    return false;
  }
  pms_uart_ready = true;
  const std::uint32_t started = monotonic_ms();
  bool received = false;

  while (static_cast<std::uint32_t>(monotonic_ms() - started) <
         kPmsProbeDurationMs) {
    for (;;) {
      const int value = read_pms_byte();
      if (value < 0)
        break;
      if (pms_parser.push(static_cast<std::uint8_t>(value), latest_pms_frame)) {
        has_pms_frame = true;
        latest_pms_mono_us = esp_timer_get_time();
        ++pms_frame_count;
        received = true;
        break;
      }
    }
    if (received) {
      break;
    }
    wait_ms(1);
  }
  if (!received) {
    oslog.printf(
        "DIAG pmsa003 status=no_valid_frame_within_timeout timeout_ms=%lu "
        "checksum_failures=%lu length_failures=%lu values_valid=false\n",
        static_cast<unsigned long>(kPmsProbeDurationMs),
        static_cast<unsigned long>(pms_parser.checksum_failures()),
        static_cast<unsigned long>(pms_parser.length_failures()));
    return false;
  }

  oslog.printf(
      "DIAG pmsa003 status=frame_received values_valid=%s sensor_error=%u "
      "firmware=%u atmospheric_pm1_ug_m3=%u atmospheric_pm25_ug_m3=%u "
      "atmospheric_pm10_ug_m3=%u cf1_pm1_ug_m3=%u cf1_pm25_ug_m3=%u "
      "cf1_pm10_ug_m3=%u counts_per_0_1l=%u,%u,%u,%u,%u,%u\n",
      latest_pms_frame.sensor_error == 0 ? "true" : "false",
      latest_pms_frame.sensor_error, latest_pms_frame.firmware_version,
      latest_pms_frame.atmospheric_pm1, latest_pms_frame.atmospheric_pm25,
      latest_pms_frame.atmospheric_pm10, latest_pms_frame.cf1_pm1,
      latest_pms_frame.cf1_pm25, latest_pms_frame.cf1_pm10,
      latest_pms_frame.particle_counts[0], latest_pms_frame.particle_counts[1],
      latest_pms_frame.particle_counts[2], latest_pms_frame.particle_counts[3],
      latest_pms_frame.particle_counts[4], latest_pms_frame.particle_counts[5]);
  return latest_pms_frame.sensor_error == 0;
}

void poll_pms() {
  for (;;) {
    const int value = read_pms_byte();
    if (value < 0)
      break;
    if (!pms_parser.push(static_cast<std::uint8_t>(value), latest_pms_frame)) {
      continue;
    }
    has_pms_frame = true;
    latest_pms_mono_us = esp_timer_get_time();
    ++pms_frame_count;
  }
}

// The row's status rule, so the screen and MEAS never show as valid a value
// the row nulls (#165).
constexpr std::int32_t kPmsValid = 4;
telemetry::PmsReading pms_reading() {
  return telemetry::pms_reading(has_pms_frame, esp_timer_get_time(),
                                latest_pms_mono_us,
                                latest_pms_frame.sensor_error);
}

void print_periodic_pms() {
  // Before the first frame the status is missing. Frame age and values stay
  // out of the line until a frame arrives.
  const auto reading = pms_reading();
  if (!has_pms_frame) {
    oslog.printf("MEAS pmsa003 values_valid=false status=%s interval_ms=%lu "
                 "checksum_failures=%lu length_failures=%lu\n",
                 reading.name, static_cast<unsigned long>(kDisplayIntervalMs),
                 static_cast<unsigned long>(pms_parser.checksum_failures()),
                 static_cast<unsigned long>(pms_parser.length_failures()));
    return;
  }

  oslog.printf(
      "MEAS pmsa003 values_valid=%s status=%s sensor_error=%u "
      "frame_age_ms=%lu "
      "atmospheric_pm1_ug_m3=%u atmospheric_pm25_ug_m3=%u "
      "atmospheric_pm10_ug_m3=%u cf1_pm1_ug_m3=%u cf1_pm25_ug_m3=%u "
      "cf1_pm10_ug_m3=%u counts_per_0_1l=%u,%u,%u,%u,%u,%u frames=%lu "
      "checksum_failures=%lu length_failures=%lu\n",
      reading.status == kPmsValid ? "true" : "false", reading.name,
      latest_pms_frame.sensor_error, static_cast<unsigned long>(reading.age_ms),
      latest_pms_frame.atmospheric_pm1, latest_pms_frame.atmospheric_pm25,
      latest_pms_frame.atmospheric_pm10, latest_pms_frame.cf1_pm1,
      latest_pms_frame.cf1_pm25, latest_pms_frame.cf1_pm10,
      latest_pms_frame.particle_counts[0], latest_pms_frame.particle_counts[1],
      latest_pms_frame.particle_counts[2], latest_pms_frame.particle_counts[3],
      latest_pms_frame.particle_counts[4], latest_pms_frame.particle_counts[5],
      static_cast<unsigned long>(pms_frame_count),
      static_cast<unsigned long>(pms_parser.checksum_failures()),
      static_cast<unsigned long>(pms_parser.length_failures()));
}

void draw_pm_triplet(std::uint16_t pm1, std::uint16_t pm25, std::uint16_t pm10,
                     int label_y, int value_y) {
  constexpr int x_positions[] = {8, 112, 216};
  constexpr const char *labels[] = {"PM1.0", "PM2.5", "PM10"};
  const std::uint16_t values[] = {pm1, pm25, pm10};

  M5.Display.setTextSize(1);
  for (std::size_t index = 0; index < 3; ++index) {
    M5.Display.setCursor(x_positions[index], label_y);
    M5.Display.print(labels[index]);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(x_positions[index], value_y);
    M5.Display.printf("%u", values[index]);
    M5.Display.setTextSize(1);
  }
}

std::uint16_t battery_colour(screen_header::Band band) {
  using enum screen_header::Band;
  switch (band) {
  case High:
    return TFT_GREEN;
  case Mid:
    return TFT_YELLOW;
  case Low:
    break;
  }
  return TFT_RED;
}

// Header battery icon and percent, from the same reading as the device page
// and the battery columns. Nothing is drawn without a cell in the plausible
// voltage range and a fuel gauge percent, so no battery means no icon.
void draw_header_battery(const telemetry::power::Reading &supply,
                         m5::charge_state_t charge) {
  if (!supply.battery_mv || !supply.battery_percent)
    return;
  namespace sh = screen_header;
  const auto percent = sh::clamp_percent(*supply.battery_percent);
  M5.Display.drawRect(sh::kBatteryX, sh::kBatteryY, sh::kBatteryWidth,
                      sh::kBatteryHeight, TFT_WHITE);
  M5.Display.fillRect(sh::kBatteryX + sh::kBatteryWidth,
                      sh::kBatteryY + (sh::kBatteryHeight - sh::kNubHeight) / 2,
                      sh::kNubWidth, sh::kNubHeight, TFT_WHITE);
  if (const int fill = sh::fill_px(percent); fill > 0)
    M5.Display.fillRect(sh::kBatteryX + sh::kFillInset,
                        sh::kBatteryY + sh::kFillInset, fill, sh::kFillHeight,
                        battery_colour(sh::band_of(percent)));
  M5.Display.setTextSize(1);
  M5.Display.setCursor(sh::kPercentX, sh::kPercentY);
  M5.Display.printf("%ld%%", static_cast<long>(percent));
  if (!is_charging(charge))
    return;
  // A lightning bolt from two triangles in a 7 x 10 box.
  constexpr int x = sh::kBoltX;
  constexpr int y = sh::kBatteryY;
  M5.Display.fillTriangle(x + 5, y, x, y + 6, x + 4, y + 5, TFT_YELLOW);
  M5.Display.fillTriangle(x + 2, y + 4, x + 6, y + 4, x + 1, y + 9, TFT_YELLOW);
}

void draw_screen_header(std::string_view title,
                        const telemetry::power::Reading &supply,
                        m5::charge_state_t charge) {
  namespace sh = screen_header;
  M5.Display.setTextSize(sh::kTitleSize);
  M5.Display.setCursor(sh::kTitleX, sh::kTitleY);
  M5.Display.printf("%.*s", static_cast<int>(title.size()), title.data());
  M5.Display.setTextSize(1);
  M5.Display.setCursor(sh::kPageX, sh::kRow1Y);
  M5.Display.printf("%u/%u", static_cast<unsigned>(screen_page + 1),
                    static_cast<unsigned>(kScreenPageCount));
  if (ble_started) {
    const auto link = ble::link();
    M5.Display.setTextColor(link.connected ? TFT_GREEN : TFT_DARKGREY,
                            TFT_BLACK);
    M5.Display.setCursor(sh::kBtX, sh::kRow1Y);
    M5.Display.print(link.connected ? (link.authenticated ? "BT ok" : "BT ..")
                                    : "BT adv");
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  }
  const auto wifi = lan::status();
  if (std::strcmp(wifi.state, "off") != 0) {
    const bool up = std::strcmp(wifi.state, "connected") == 0;
    M5.Display.setTextColor(up ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
    M5.Display.setCursor(sh::kWifiX, sh::kRow1Y);
    M5.Display.print(up ? (wifi.authenticated ? "WiFi ok" : "WiFi up")
                        : "WiFi ..");
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  }
  draw_header_battery(supply, charge);

  constexpr int bar_top = 31;
  constexpr int bar_height = 184;
  const int marker_height = bar_height / kScreenPageCount;
  M5.Display.fillRect(316, bar_top, 3, bar_height, TFT_DARKGREY);
  M5.Display.fillRect(316, bar_top + marker_height * screen_page, 3,
                      marker_height, TFT_WHITE);
}

// While the Thing needs setup (#147, the advert setup bit) the footer asks
// for the app on every page instead of the swipe hint. It clears on the next
// redraw after a SET_CONFIG sets a cell or switches to moving.
void draw_screen_footer() {
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 228);
  if (config::setup_needed(config::location_settings())) {
    M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Display.print("Setup needed, open the OpenSensor app");
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    return;
  }
  M5.Display.print("Swipe up/down or tap for pages");
}

// The PM pages show no number unless the row would store it (#165).
void draw_pms_not_valid(const telemetry::PmsReading &reading) {
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 48);
  M5.Display.print(reading.status == 1   ? "Sensor warming up"
                   : reading.status == 2 ? "No recent frame"
                   : reading.status == 3 ? "Sensor reports an error"
                                         : "Waiting for a valid frame");
  M5.Display.setTextSize(1);
  if (has_pms_frame) {
    M5.Display.setCursor(8, 78);
    M5.Display.printf("Status %s, frame %lu s ago, error %u", reading.name,
                      static_cast<unsigned long>(reading.age_ms / 1000),
                      latest_pms_frame.sensor_error);
  }
  M5.Display.setCursor(8, 94);
  M5.Display.printf("CRC failures: %lu  length failures: %lu",
                    static_cast<unsigned long>(pms_parser.checksum_failures()),
                    static_cast<unsigned long>(pms_parser.length_failures()));
}

void draw_mass_page(const telemetry::PmsReading &reading) {
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 31);
  M5.Display.print("Mass concentration in ug/m3");
  M5.Display.setCursor(8, 48);
  M5.Display.print("Atmospheric");
  draw_pm_triplet(latest_pms_frame.atmospheric_pm1,
                  latest_pms_frame.atmospheric_pm25,
                  latest_pms_frame.atmospheric_pm10, 61, 73);

  M5.Display.setCursor(8, 104);
  M5.Display.print("CF=1");
  draw_pm_triplet(latest_pms_frame.cf1_pm1, latest_pms_frame.cf1_pm25,
                  latest_pms_frame.cf1_pm10, 117, 129);

  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 171);
  M5.Display.printf("Status: %s  sensor error=%u", reading.name,
                    latest_pms_frame.sensor_error);
  M5.Display.setCursor(8, 187);
  M5.Display.printf("Latest frame: %lu ms ago  sensor FW: %u",
                    static_cast<unsigned long>(reading.age_ms),
                    latest_pms_frame.firmware_version);
  M5.Display.setCursor(8, 203);
  M5.Display.printf("Display and serial refresh every %lu s",
                    static_cast<unsigned long>(kDisplayIntervalMs / 1000));
}

void draw_count_pair(const char *left_label, std::uint16_t left_value,
                     const char *right_label, std::uint16_t right_value,
                     int label_y, int value_y) {
  constexpr int right_x = 168;
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, label_y);
  M5.Display.print(left_label);
  M5.Display.setCursor(right_x, label_y);
  M5.Display.print(right_label);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, value_y);
  M5.Display.printf("%u", left_value);
  M5.Display.setCursor(right_x, value_y);
  M5.Display.printf("%u", right_value);
}

void draw_counts_page(const telemetry::PmsReading &reading) {
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 31);
  M5.Display.print("Cumulative particle counts per 0.1 L");
  draw_count_pair(">0.3 um", latest_pms_frame.particle_counts[0], ">0.5 um",
                  latest_pms_frame.particle_counts[1], 49, 61);
  draw_count_pair(">1.0 um", latest_pms_frame.particle_counts[2], ">2.5 um",
                  latest_pms_frame.particle_counts[3], 91, 103);
  draw_count_pair(">5.0 um", latest_pms_frame.particle_counts[4], ">10 um",
                  latest_pms_frame.particle_counts[5], 133, 145);

  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 179);
  M5.Display.printf("Frames=%lu  CRC=%lu  length=%lu",
                    static_cast<unsigned long>(pms_frame_count),
                    static_cast<unsigned long>(pms_parser.checksum_failures()),
                    static_cast<unsigned long>(pms_parser.length_failures()));
  M5.Display.setCursor(8, 196);
  M5.Display.printf("Sensor FW=%u  error=%u  age=%lu ms",
                    latest_pms_frame.firmware_version,
                    latest_pms_frame.sensor_error,
                    static_cast<unsigned long>(reading.age_ms));
}

void draw_device_page(const telemetry::power::Reading &supply,
                      m5::charge_state_t charge) {
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 35);
  // Live from the logger, not the boot mount result (#165).
  const auto logging = oslogger::logging_view();
  if (logging.stopped.empty()) {
    M5.Display.printf("Logging on, rows dropped %lu",
                      static_cast<unsigned long>(logging.dropped));
  } else {
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.printf("Logging off, %.*s, dropped %lu",
                      static_cast<int>(logging.stopped.size()),
                      logging.stopped.data(),
                      static_cast<unsigned long>(logging.dropped));
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  }
  M5.Display.setCursor(8, 55);
  namespace pw = telemetry::power;
  pw::IntText vbus_buffer;
  pw::IntText battery_buffer;
  pw::IntText percent_buffer;
  const char *unit_vbus = supply.vbus_mv ? " mV" : "";
  const char *unit_battery = supply.battery_mv ? " mV" : "";
  const char *unit_percent = supply.battery_percent ? "%" : "";
  M5.Display.printf("VBUS: %s%s",
                    pw::int_text(supply.vbus_mv, vbus_buffer, "--"), unit_vbus);
  M5.Display.setCursor(168, 55);
  M5.Display.printf("Battery: %s%s",
                    pw::int_text(supply.battery_mv, battery_buffer, "--"),
                    unit_battery);
  M5.Display.setCursor(8, 75);
  M5.Display.printf("Battery: %s%s  %s",
                    pw::int_text(supply.battery_percent, percent_buffer, "--"),
                    unit_percent, charging_name(charge));

  M5.Display.setCursor(8, 103);
  M5.Display.printf(
      "Internal heap free: %lu bytes",
      static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  M5.Display.setCursor(8, 123);
  M5.Display.printf("Internal heap minimum: %lu bytes",
                    static_cast<unsigned long>(
                        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
  M5.Display.setCursor(8, 143);
  M5.Display.printf(
      "PSRAM free: %lu / %lu bytes",
      static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
      static_cast<unsigned long>(esp_psram_get_size()));

  M5.Display.setCursor(8, 171);
  M5.Display.printf("Uptime: %lu s",
                    static_cast<unsigned long>(monotonic_ms() / 1000));
  M5.Display.setCursor(168, 171);
  M5.Display.printf("PMS frames: %lu",
                    static_cast<unsigned long>(pms_frame_count));
  M5.Display.setCursor(8, 191);
  M5.Display.printf("Touch: %s  refresh: %lu s",
                    M5.Touch.isEnabled() ? "enabled" : "disabled",
                    static_cast<unsigned long>(kDisplayIntervalMs / 1000));
  // UTC by contract; the phone renders local time.
  const auto clock = telemetry::clock_view();
  M5.Display.setCursor(8, 211);
  if (clock.source == 0) {
    M5.Display.printf("Clock: no UTC yet (RTC %s), set from app",
                      clock.rtc_state == 2 ? "unusable" : "unset");
  } else {
    const std::time_t seconds =
        static_cast<std::time_t>(clock.utc_ns / 1000000000);
    std::tm utc{};
    if (::gmtime_r(&seconds, &utc))
      M5.Display.printf("Clock: %04d-%02d-%02d %02d:%02d:%02dZ %s e%ld",
                        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                        utc.tm_hour, utc.tm_min, utc.tm_sec, clock.source_name,
                        static_cast<long>(clock.epoch));
  }
}

void draw_bluetooth_page() {
  const auto link = ble::link();
  const auto wifi = lan::status();
  const auto settings = config::get();
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 35);
  M5.Display.print("Device name");
  M5.Display.setTextSize(3);
  M5.Display.setCursor(8, 48);
  M5.Display.print(ble_started ? ble::local_name() : "BLE off");
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 82);
  if (!ble_started)
    M5.Display.print("Bluetooth did not start; USB sync still works");
  else if (link.connected)
    M5.Display.printf("BT connected  %s  MTU %u",
                      link.authenticated ? "paired" : "pairing...",
                      static_cast<unsigned>(link.mtu));
  else
    M5.Display.print("BT advertising: open the OpenSensor Sync app to pair");
  M5.Display.setCursor(8, 96);
  M5.Display.printf("Pairing: %s   bonded phones: %lu",
                    config::pair_name(settings.pair),
                    static_cast<unsigned long>(link.bonds));
  if (config::reboot_required()) {
    M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Display.setCursor(8, 108);
    M5.Display.print("Pairing change waits for a reboot");
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  }
  M5.Display.setCursor(8, 128);
  if (!settings.wifi_on)
    M5.Display.print("Wi-Fi: off (set up from the app)");
  else if (std::strcmp(wifi.state, "connected") == 0)
    M5.Display.printf("Wi-Fi: %s  %s  %d dBm", settings.ssid, wifi.ip,
                      wifi.rssi);
  else
    M5.Display.printf("Wi-Fi: %s  %s", settings.ssid, wifi.state);
  M5.Display.setCursor(8, 142);
  if (settings.wifi_on && std::strcmp(wifi.state, "connected") == 0)
    M5.Display.printf("LAN sync: %s.local:%u  %s  sessions %lu", wifi.host,
                      static_cast<unsigned>(config::kLanPort),
                      wifi.authenticated ? "phone connected"
                                         : (wifi.mdns ? "waiting" : "no mDNS"),
                      static_cast<unsigned long>(wifi.sessions));
  else
    M5.Display.print("LAN sync: needs Wi-Fi");
  M5.Display.setCursor(8, 170);
  M5.Display.print("Files are copied to the phone, never");
  M5.Display.setCursor(8, 182);
  M5.Display.print("deleted here. Works offline.");
}

// Pairing takes over the whole screen so the six digits are unmistakable.
void draw_pairing_overlay(const ble::PairingState &pairing) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 20);
  M5.Display.print("Pair with phone");
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 50);
  M5.Display.print("Enter this code on the phone:");
  M5.Display.setTextSize(6);
  M5.Display.setCursor(52, 90);
  M5.Display.printf("%06lu", static_cast<unsigned long>(pairing.passkey));
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 170);
  M5.Display.printf("Device %s", ble::local_name());
  M5.Display.setCursor(8, 190);
  if (ble::pair_mode() == config::PairMode::Fixed)
    M5.Display.print("Per-device fixed PIN; configurable from the app.");
  else
    M5.Display.print("Only someone reading this screen can pair.");
}

// The Thing id did not load, so logging is off for the boot (#225). Red text
// like the NVS fault, with the stage and the ESP-IDF error. The stored value
// is never rewritten.
void draw_thing_id_fault(const oslogger::thing_id::Error &error) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_RED, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 48);
  M5.Display.print("Settings storage error");
  M5.Display.setCursor(8, 80);
  M5.Display.print("Logging is off");
  M5.Display.setCursor(8, 112);
  M5.Display.printf("thing-id stage=%s",
                    oslogger::thing_id::stage_name(error.stage));
  M5.Display.setCursor(8, 136);
  M5.Display.printf("error=%d", error.code);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 176);
  M5.Display.print("Read the USB serial log and see the board guide");
}

void show_pms_screen() {
  telemetry::lock_display();
  // M5GFX 0.3.0 renumbered rotation so 0 is the upright landscape panel.
  M5.Display.setRotation(0);
  const auto pairing = ble::pairing();
  if (ble_started && pairing.active) {
    draw_pairing_overlay(pairing);
    M5.Display.waitDMA();
    telemetry::unlock_display();
    return;
  }
  if (const auto fault = telemetry::thing_id_fault()) {
    draw_thing_id_fault(*fault);
    M5.Display.waitDMA();
    telemetry::unlock_display();
    return;
  }
  // One power read per redraw serves the header and the device page. It runs
  // before the clear, so the I2C reads do not lengthen the blank frame.
  const auto supply = telemetry::read_power_now();
  const auto charge = M5.Power.getChargeState();
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

  static_assert(screen_header::kTitles.size() == kScreenPageCount);
  draw_screen_header(screen_header::kTitles[screen_page], supply, charge);

  const auto reading = pms_reading();
  if (reading.status != kPmsValid && screen_page < 2) {
    draw_pms_not_valid(reading);
  } else if (screen_page == 0) {
    draw_mass_page(reading);
  } else if (screen_page == 1) {
    draw_counts_page(reading);
  } else if (screen_page == 2) {
    draw_device_page(supply, charge);
  } else {
    draw_bluetooth_page();
  }
  draw_screen_footer();
  M5.Display.waitDMA();
  telemetry::unlock_display();
}

void handle_touch_navigation() {
  if (!M5.Touch.isEnabled() || M5.Touch.getCount() == 0) {
    return;
  }

  const auto &touch = M5.Touch.getDetail();
  std::uint8_t next_page = screen_page;
  const char *gesture = nullptr;

  if (touch.wasClicked()) {
    next_page = static_cast<std::uint8_t>((screen_page + 1) % kScreenPageCount);
    gesture = "tap";
  } else if (touch.wasFlicked()) {
    const int distance_x = touch.distanceX();
    const int distance_y = touch.distanceY();
    const int absolute_x = distance_x < 0 ? -distance_x : distance_x;
    const int absolute_y = distance_y < 0 ? -distance_y : distance_y;

    if (absolute_y >= absolute_x && absolute_y >= kMinimumSwipeDistance &&
        distance_y < 0) {
      next_page =
          static_cast<std::uint8_t>((screen_page + 1) % kScreenPageCount);
      gesture = "forward-swipe";
    } else if (absolute_y >= absolute_x &&
               absolute_y >= kMinimumSwipeDistance && distance_y > 0) {
      next_page = static_cast<std::uint8_t>(
          (screen_page + kScreenPageCount - 1) % kScreenPageCount);
      gesture = "back-swipe";
    }
  }

  if (gesture == nullptr || next_page == screen_page) {
    return;
  }

  screen_page = next_page;
  show_pms_screen();
  oslog.printf("UI page=%u/%u gesture=%s\n",
               static_cast<unsigned>(screen_page + 1),
               static_cast<unsigned>(kScreenPageCount), gesture);
}

} // namespace

// Storage worker only, with the shared display/storage lock held.
bool telemetry::remount_sd() {
  if (sd_card && esp_vfs_fat_sdcard_unmount("/sd", sd_card) != ESP_OK)
    return false;
  sd_card = nullptr;
  const esp_err_t result = mount_sd();
  if (result != ESP_OK && sd_transfer_buffer.report_unavailable())
    oslog.println("DIAG sd remount=false status=dma_buffer_unavailable");
  return result == ESP_OK;
}
std::uint32_t telemetry::sd_cluster_bytes() {
  return opensensor::storage::fat_cluster_bytes(sd_card);
}
bool telemetry::sd_present() {
  return opensensor::storage::card_answers(sd_card);
}
std::uint32_t telemetry::sd_volume_serial() {
  return opensensor::storage::fat_volume_serial(sd_card);
}
bool telemetry::sd_free_bytes(std::uint64_t &free_bytes) {
  return sd_card && opensensor::storage::fat_free_bytes("/sd", free_bytes);
}

static void board_begin() {
  auto config = M5.config();
  config.clear_display = true;
  config.output_power = true;
  config.internal_imu = true;
  config.internal_rtc = true;
  config.internal_mic = false;
  config.internal_spk = false;
  config.external_imu = false;
  config.external_rtc = false;
  config.external_display_value = 0;
  config.fallback_board = m5::board_t::board_M5StackCoreS3;
  M5.begin(config);
  wait_ms(500);

  oslog.println("DIAG BEGIN schema=cores3-bringup-v1");
  report_chip();
  report_memory();
  report_i2c();
  report_power();
  report_rtc_imu_touch();
  sd_mounted = report_sd();
  report_dma_memory("sd-mounted");
  const char *fallback_directory = opensensor::storage::mount_flash_fallback(
      [](const char *text) { oslog.printf("%s", text); });
  start_pms();
  // os_logger prints the cause. The device page shows "not started".
  if (!telemetry::begin_logger(sd_mounted, fallback_directory))
    oslog.println("DIAG logger status=start_failed logging=false");
  if (kEnableBle) {
    ble_started = telemetry::start_links(M5.getDisplayCount() > 0);
    last_ble_ui_generation = ble::ui_generation() + lan::ui_generation();
    report_dma_memory("links-started");
  }
  show_pms_screen();
  last_display_ms = monotonic_ms();
  oslog.println("DIAG COMPLETE schema=cores3-bringup-v1");
}

static void board_poll() {
  M5.update();
  poll_pms();
  telemetry::PmsSnapshot sample;
  sample.present = has_pms_frame;
  sample.received_mono_us = latest_pms_mono_us;
  sample.frames = pms_frame_count;
  sample.checksum_errors = pms_parser.checksum_failures();
  sample.length_errors = pms_parser.length_failures();
  sample.firmware = latest_pms_frame.firmware_version;
  sample.error = latest_pms_frame.sensor_error;
  sample.values[0] = latest_pms_frame.cf1_pm1;
  sample.values[1] = latest_pms_frame.cf1_pm25;
  sample.values[2] = latest_pms_frame.cf1_pm10;
  sample.values[3] = latest_pms_frame.atmospheric_pm1;
  sample.values[4] = latest_pms_frame.atmospheric_pm25;
  sample.values[5] = latest_pms_frame.atmospheric_pm10;
  for (std::size_t index = 0; index < 6; ++index) {
    sample.values[index + 6] = latest_pms_frame.particle_counts[index];
  }
  telemetry::poll_logger(sample);
  handle_touch_navigation();
  const std::uint32_t now = monotonic_ms();
  const std::uint32_t link_generation =
      ble::ui_generation() + lan::ui_generation();
  if (ble_started && link_generation != last_ble_ui_generation) {
    last_ble_ui_generation = link_generation;
    show_pms_screen();
    last_display_ms = now;
  }
  if (static_cast<std::uint32_t>(now - last_display_ms) >= kDisplayIntervalMs) {
    show_pms_screen();
    print_periodic_pms();
    last_display_ms = now;
  }
  // A Thing id fault repeats its line every 30 s on the physical console
  // only, so the repeats never push the first line out of the log ring.
  static std::uint32_t last_fault_line_ms = now;
  if (const auto fault = telemetry::thing_id_fault();
      fault && static_cast<std::uint32_t>(now - last_fault_line_ms) >= 30000) {
    last_fault_line_ms = now;
    opensensor::console::printf(oslogger::thing_id::kFaultLine,
                                oslogger::thing_id::stage_name(fault->stage),
                                fault->code);
  }
  wait_ms(20);
}

namespace {
constexpr const char *kNvsFaultLine =
    "DIAG ERROR operation=nvs-init error=%d erased=false\n";

// NVS did not start, so the board has no settings and does not log. The
// screen needs no NVS, so the fault goes there in red, and the serial line
// repeats every 30 s. Never erase NVS here, it holds the owner's settings
// and the Thing id.
[[noreturn]] void storage_fault(esp_err_t error) {
  auto config = M5.config();
  config.clear_display = true;
  config.output_power = true;
  config.internal_imu = false;
  config.internal_rtc = false;
  config.internal_mic = false;
  config.internal_spk = false;
  config.external_imu = false;
  config.external_rtc = false;
  config.external_display_value = 0;
  config.fallback_board = m5::board_t::board_M5StackCoreS3;
  M5.begin(config);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_RED, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 48);
  M5.Display.print("Settings storage error");
  M5.Display.setCursor(8, 80);
  M5.Display.print("Logging is off");
  M5.Display.setCursor(8, 112);
  M5.Display.printf("nvs-init error=%d", error);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 152);
  M5.Display.print("Read the USB serial log and see the board guide");
  for (;;) {
    wait_ms(30000);
    opensensor::console::printf(kNvsFaultLine, error);
  }
}

// The parameter is the NVS init error, ESP_OK when NVS started. FreeRTOS
// fixes the callback signature.
// cppcheck-suppress constParameterCallback
void board_task(void *parameter) {
  const auto nvs =
      static_cast<esp_err_t>(reinterpret_cast<std::intptr_t>(parameter));
  if (nvs != ESP_OK)
    storage_fault(nvs);
  board_begin();
  for (;;)
    board_poll();
}
} // namespace

extern "C" void app_main() {
  if (!opensensor::console::begin())
    return;
  opensensor::log::attach_persistent(); // PSRAM log; the internal ring
                                        // otherwise
  const esp_err_t nvs = nvs_flash_init();
  if (nvs != ESP_OK) {
    oslog.printf(kNvsFaultLine, nvs);
  } else {
    opensensor::log::report_coredump();
    opensensor::power::configure_dfs();
  }
  // Without NVS the board task shows the fault on the screen and stops.
  if (xTaskCreate(board_task, "cores3-board", 16 * 1024,
                  reinterpret_cast<void *>(static_cast<std::intptr_t>(nvs)),
                  oslogger::kBoardTaskPriority, nullptr) != pdPASS)
    oslog.println("DIAG ERROR operation=board-task-create");
}
