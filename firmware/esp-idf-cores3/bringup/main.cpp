#include "ble_sync.h"
#include "debug_log.h"
#include "device_config.h"
#include "log_persist.h"
#include "power.h"
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
std::uint32_t latest_pms_ms = 0;
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

const char *charging_name(m5::Power_Class::is_charging_t charging) {
  switch (charging) {
  case m5::Power_Class::is_charging:
    return "charging";
  case m5::Power_Class::is_discharging:
    return "discharging";
  case m5::Power_Class::charge_unknown:
  default:
    return "unknown";
  }
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
  const auto charging = M5.Power.isCharging();
  oslog.printf(
      "DIAG power pmic_type=%d external_5v=%s usb_output=%s vbus_mv=%d "
      "battery_mv=%d battery_percent=%ld charging=%s battery_current_ma=%ld\n",
      static_cast<int>(M5.Power.getType()),
      M5.Power.getExtOutput() ? "on" : "off",
      M5.Power.getUsbOutput() ? "source" : "input",
      static_cast<int>(M5.Power.getVBUSVoltage()),
      static_cast<int>(M5.Power.getBatteryVoltage()),
      static_cast<long>(M5.Power.getBatteryLevel()), charging_name(charging),
      static_cast<long>(M5.Power.getBatteryCurrent()));
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

// SDSPI attach and FAT mount on the M5GFX-owned bus. Never formats. A
// remount keeps the DMA buffer: once radios run, internal DMA memory may be
// too fragmented to reserve it again.
esp_err_t mount_sd(bool keep_buffer) {
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
  const esp_err_t result =
      esp_vfs_fat_sdspi_mount("/sd", &host, &device, &mount, &sd_card);
  if (result != ESP_OK && !keep_buffer)
    sd_transfer_buffer.release();
  return result;
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
  const esp_err_t result = mount_sd(false);
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
        latest_pms_ms = monotonic_ms();
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
    latest_pms_ms = monotonic_ms();
    latest_pms_mono_us = esp_timer_get_time();
    ++pms_frame_count;
  }
}

void print_periodic_pms() {
  if (!has_pms_frame) {
    oslog.printf(
        "MEAS pmsa003 values_valid=false interval_ms=%lu checksum_failures=%lu "
        "length_failures=%lu\n",
        static_cast<unsigned long>(kDisplayIntervalMs),
        static_cast<unsigned long>(pms_parser.checksum_failures()),
        static_cast<unsigned long>(pms_parser.length_failures()));
    return;
  }

  oslog.printf(
      "MEAS pmsa003 values_valid=%s sensor_error=%u frame_age_ms=%lu "
      "atmospheric_pm1_ug_m3=%u atmospheric_pm25_ug_m3=%u "
      "atmospheric_pm10_ug_m3=%u cf1_pm1_ug_m3=%u cf1_pm25_ug_m3=%u "
      "cf1_pm10_ug_m3=%u counts_per_0_1l=%u,%u,%u,%u,%u,%u frames=%lu "
      "checksum_failures=%lu length_failures=%lu\n",
      latest_pms_frame.sensor_error == 0 ? "true" : "false",
      latest_pms_frame.sensor_error,
      static_cast<unsigned long>(monotonic_ms() - latest_pms_ms),
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

void draw_screen_header(const char *title) {
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 5);
  M5.Display.print(title);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(279, 10);
  M5.Display.printf("%u/%u", static_cast<unsigned>(screen_page + 1),
                    static_cast<unsigned>(kScreenPageCount));
  if (ble_started) {
    const auto link = ble::link();
    M5.Display.setTextColor(link.connected ? TFT_GREEN : TFT_DARKGREY,
                            TFT_BLACK);
    M5.Display.setCursor(236, 10);
    M5.Display.print(link.connected ? (link.authenticated ? "BT ok" : "BT ..")
                                    : "BT adv");
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  }
  const auto wifi = lan::status();
  if (std::strcmp(wifi.state, "off") != 0) {
    const bool up = std::strcmp(wifi.state, "connected") == 0;
    M5.Display.setTextColor(up ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
    M5.Display.setCursor(196, 10);
    M5.Display.print(up ? (wifi.authenticated ? "WiFi ok" : "WiFi up")
                        : "WiFi ..");
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  }

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

void draw_waiting_for_pms() {
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 48);
  M5.Display.print("Waiting for a valid frame");
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 78);
  M5.Display.printf("CRC failures: %lu  length failures: %lu",
                    static_cast<unsigned long>(pms_parser.checksum_failures()),
                    static_cast<unsigned long>(pms_parser.length_failures()));
}

void draw_mass_page() {
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
  M5.Display.printf("Status: %s  sensor error=%u",
                    latest_pms_frame.sensor_error == 0 ? "valid" : "sensor",
                    latest_pms_frame.sensor_error);
  M5.Display.setCursor(8, 187);
  M5.Display.printf("Latest frame: %lu ms ago  sensor FW: %u",
                    static_cast<unsigned long>(monotonic_ms() - latest_pms_ms),
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

void draw_counts_page() {
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
                    static_cast<unsigned long>(monotonic_ms() - latest_pms_ms));
}

void draw_device_page() {
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 35);
  M5.Display.printf("SD card: %s", sd_mounted ? "mounted" : "not mounted");
  M5.Display.setCursor(8, 55);
  M5.Display.printf("VBUS: %d mV", static_cast<int>(M5.Power.getVBUSVoltage()));
  M5.Display.setCursor(168, 55);
  M5.Display.printf("Battery: %d mV",
                    static_cast<int>(M5.Power.getBatteryVoltage()));
  M5.Display.setCursor(8, 75);
  M5.Display.printf("Battery: %ld%%  %s",
                    static_cast<long>(M5.Power.getBatteryLevel()),
                    charging_name(M5.Power.isCharging()));

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

void show_pms_screen() {
  telemetry::lock_display();
  M5.Display.setRotation(1);
  const auto pairing = ble::pairing();
  if (ble_started && pairing.active) {
    draw_pairing_overlay(pairing);
    M5.Display.waitDMA();
    telemetry::unlock_display();
    return;
  }
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

  constexpr const char *titles[kScreenPageCount] = {
      "PM mass", "Particle counts", "Device health", "Bluetooth"};
  draw_screen_header(titles[screen_page]);

  if (!has_pms_frame && screen_page < 2) {
    draw_waiting_for_pms();
  } else if (screen_page == 0) {
    draw_mass_page();
  } else if (screen_page == 1) {
    draw_counts_page();
  } else if (screen_page == 2) {
    draw_device_page();
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
  return mount_sd(true) == ESP_OK;
}
std::uint32_t telemetry::sd_cluster_bytes() {
  return opensensor::storage::fat_cluster_bytes(sd_card);
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
  start_pms();
  telemetry::begin_logger(sd_mounted);
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
  sample.age_ms = monotonic_ms() - latest_pms_ms;
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
  wait_ms(20);
}

namespace {
void board_task(void *) {
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
    oslog.printf("DIAG ERROR operation=nvs-init error=%d erased=false\n", nvs);
    return;
  }
  opensensor::log::report_coredump();
  opensensor::power::configure_dfs();
  if (xTaskCreate(board_task, "cores3-board", 16 * 1024, nullptr,
                  oslogger::kBoardTaskPriority, nullptr) != pdPASS)
    oslog.println("DIAG ERROR operation=board-task-create");
}
