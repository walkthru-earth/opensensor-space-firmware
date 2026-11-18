#include "board_io.h"
#include <atomic>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <esp_vfs_fat.h>
#include <fat_volume.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <numeric>
#include <sd_transfer_buffer.h>
#include <sdmmc_cmd.h>

namespace board {
namespace {
// Per transaction. The SEN6x clock stretches for no command, so 50 ms covers
// a 48 byte string read at 100 kHz with room to spare.
constexpr int kI2cTimeoutMs = 50;
constexpr spi_host_device_t kSdHost = SPI3_HOST;

i2c_master_bus_handle_t sensor_bus = nullptr;
i2c_master_dev_handle_t sensor = nullptr;
std::uint8_t sensor_address = 0;
esp_err_t sensor_error = ESP_OK;

sdmmc_card_t *card = nullptr;
opensensor::storage::SdTransferBuffer sd_transfer_buffer;
bool sd_bus_ready = false;
std::atomic<bool> mounted{false};
std::atomic<std::uint64_t> free_bytes_cache{0};
std::uint64_t cached_total_bytes = 0;
std::uint64_t cached_used_bytes = 0;
bool led_ready = false;

void refresh_capacity() {
  std::uint64_t total = 0, free = 0;
  if (card && esp_vfs_fat_info("/sd", &total, &free) == ESP_OK &&
      free <= total) {
    cached_total_bytes = total;
    cached_used_bytes = total - free;
  } else {
    cached_total_bytes = cached_used_bytes = 0;
  }
}

// Card supply on, then SPI3 once. The bus stays up across remounts, an
// unmount only removes the SDSPI device.
bool start_sd_bus() {
  if (sd_bus_ready)
    return true;
  const gpio_config_t power{.pin_bit_mask = 1ULL << kSdPower,
                            .mode = GPIO_MODE_OUTPUT,
                            .pull_up_en = GPIO_PULLUP_DISABLE,
                            .pull_down_en = GPIO_PULLDOWN_DISABLE,
                            .intr_type = GPIO_INTR_DISABLE};
  if (gpio_config(&power) != ESP_OK ||
      gpio_set_level(static_cast<gpio_num_t>(kSdPower), 1) != ESP_OK)
    return false;
  vTaskDelay(pdMS_TO_TICKS(10));
  // The pin fields sit in anonymous unions, so assign them by name.
  spi_bus_config_t bus{};
  bus.mosi_io_num = kSdMosi;
  bus.miso_io_num = kSdMiso;
  bus.sclk_io_num = kSdClk;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.data4_io_num = -1;
  bus.data5_io_num = -1;
  bus.data6_io_num = -1;
  bus.data7_io_num = -1;
  bus.max_transfer_sz =
      static_cast<int>(opensensor::storage::SdTransferBuffer::kBytes);
  sd_bus_ready = spi_bus_initialize(kSdHost, &bus, SPI_DMA_CH_AUTO) == ESP_OK;
  return sd_bus_ready;
}

// The DMA buffer stays reserved after a failed mount, so a card inserted
// later still mounts once radios have fragmented internal DMA memory (#168).
bool mount() {
  if (card)
    return true;
  if (!start_sd_bus())
    return false;
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = kSdHost;
  // IDF treats any request above 20 MHz as a high-speed mode switch. Stay at
  // default speed until a card on this board is qualified faster.
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;
  // FatFs buffers may live in PSRAM. Reserve the DMA bounce buffer before
  // Wi-Fi and TCP compete for internal RAM. The SDK's ALLOC_ALIGNED_BUF flag
  // only allocates automatically for non-SPI hosts.
  if (!sd_transfer_buffer.reserve())
    return false;
  host.dma_aligned_buffer = sd_transfer_buffer.data();
  host.unaligned_multi_block_rw_max_chunk_size =
      opensensor::storage::SdTransferBuffer::kSectors;
  sdspi_device_config_t device = SDSPI_DEVICE_CONFIG_DEFAULT();
  device.host_id = kSdHost;
  device.gpio_cs = static_cast<gpio_num_t>(kSdCs);
  const esp_vfs_fat_sdmmc_mount_config_t config =
      opensensor::storage::archive_mount_config();
  sdmmc_card_t *created = nullptr;
  const esp_err_t result =
      esp_vfs_fat_sdspi_mount("/sd", &host, &device, &config, &created);
  // The IDF mount helper releases its device and FAT resources on failure.
  if (result != ESP_OK) {
    return false;
  }
  card = created;
  mounted.store(true, std::memory_order_release);
  return true;
}
} // namespace

bool start_sensor_bus(std::uint8_t address) {
  if (sensor_bus && sensor)
    return sensor_address == address;
  i2c_master_bus_config_t config{};
  config.i2c_port = I2C_NUM_0;
  config.sda_io_num = static_cast<gpio_num_t>(kSensorSda);
  config.scl_io_num = static_cast<gpio_num_t>(kSensorScl);
  config.clk_source = I2C_CLK_SRC_DEFAULT;
  config.glitch_ignore_cnt = 7;
  config.flags.enable_internal_pullup = true;
  i2c_master_bus_handle_t created_bus = nullptr;
  if (i2c_new_master_bus(&config, &created_bus) != ESP_OK)
    return false;
  i2c_device_config_t device{};
  device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  device.device_address = address;
  device.scl_speed_hz = kSensorI2cHz;
  i2c_master_dev_handle_t created = nullptr;
  if (i2c_master_bus_add_device(created_bus, &device, &created) != ESP_OK) {
    i2c_del_master_bus(created_bus);
    return false;
  }
  sensor_bus = created_bus;
  sensor = created;
  sensor_address = address;
  return true;
}
bool sensor_write(void *, std::uint8_t address,
                  std::span<const std::uint8_t> data) {
  sensor_error = !sensor ? ESP_ERR_INVALID_STATE
                 : address != sensor_address || data.empty()
                     ? ESP_ERR_INVALID_ARG
                     : i2c_master_transmit(sensor, data.data(), data.size(),
                                           kI2cTimeoutMs);
  return sensor_error == ESP_OK;
}
bool sensor_read(void *, std::uint8_t address, std::span<std::uint8_t> data) {
  sensor_error =
      !sensor ? ESP_ERR_INVALID_STATE
      : address != sensor_address || data.empty()
          ? ESP_ERR_INVALID_ARG
          : i2c_master_receive(sensor, data.data(), data.size(), kI2cTimeoutMs);
  return sensor_error == ESP_OK;
}
// vTaskDelay(n) wakes on the n-th tick boundary, which can come up to one
// tick early because the current tick is already partly gone. Rounding up and
// one extra tick make the wait at least `milliseconds`, so a SEN6x command has
// finished before the next transaction. Zero still yields for one tick.
void sensor_delay_ms(void *, std::uint32_t milliseconds) {
  const auto ticks = static_cast<TickType_t>(
      (static_cast<std::uint64_t>(milliseconds) * configTICK_RATE_HZ + 999) /
      1000);
  vTaskDelay(ticks + 1);
}
int sensor_last_error() { return sensor_error; }

bool mount_card() { return mount(); }
bool sd_buffer_reserved() { return sd_transfer_buffer.data() != nullptr; }
bool sd_buffer_unavailable_once() {
  return sd_transfer_buffer.report_unavailable();
}
bool remount_card() {
  if (card && esp_vfs_fat_sdcard_unmount("/sd", card) != ESP_OK)
    return false;
  card = nullptr;
  mounted.store(false, std::memory_order_release);
  return mount();
}
bool card_mounted() { return mounted.load(std::memory_order_acquire); }
std::uint32_t cluster_bytes() {
  return opensensor::storage::fat_cluster_bytes(card);
}
std::uint32_t volume_serial() {
  return opensensor::storage::fat_volume_serial(card);
}
bool storage_free_bytes(std::uint64_t &free_bytes) {
  const bool ok =
      card && opensensor::storage::fat_free_bytes("/sd", free_bytes);
  if (ok)
    free_bytes_cache.store(free_bytes, std::memory_order_relaxed);
  return ok;
}
std::uint64_t cached_free_bytes() {
  return free_bytes_cache.load(std::memory_order_relaxed);
}
std::uint64_t card_bytes() {
  return card ? static_cast<std::uint64_t>(card->csd.capacity) *
                    card->csd.sector_size
              : 0;
}
std::uint64_t storage_total_bytes() {
  refresh_capacity();
  return cached_total_bytes;
}
std::uint64_t storage_used_bytes() {
  refresh_capacity();
  return cached_used_bytes;
}

bool start_indicator() {
  if (led_ready)
    return true;
  // Latch low before the pin becomes an output, so it never flashes on.
  gpio_set_level(static_cast<gpio_num_t>(kIndicatorLed), 0);
  const gpio_config_t pin{.pin_bit_mask = 1ULL << kIndicatorLed,
                          .mode = GPIO_MODE_OUTPUT,
                          .pull_up_en = GPIO_PULLUP_DISABLE,
                          .pull_down_en = GPIO_PULLDOWN_DISABLE,
                          .intr_type = GPIO_INTR_DISABLE};
  led_ready = gpio_config(&pin) == ESP_OK;
  return led_ready;
}
void indicator(bool on) {
  if (led_ready)
    gpio_set_level(static_cast<gpio_num_t>(kIndicatorLed), on ? 1 : 0);
}

bool start_keys() {
  const auto mask = std::accumulate(
      kKeys.begin(), kKeys.end(), std::uint64_t{0},
      [](std::uint64_t bits, int key) { return bits | (1ULL << key); });
  const gpio_config_t pins{.pin_bit_mask = mask,
                           .mode = GPIO_MODE_INPUT,
                           .pull_up_en = GPIO_PULLUP_ENABLE,
                           .pull_down_en = GPIO_PULLDOWN_DISABLE,
                           .intr_type = GPIO_INTR_DISABLE};
  return gpio_config(&pins) == ESP_OK;
}
std::uint8_t keys_held() {
  std::uint8_t held = 0;
  for (std::size_t i = 0; i < kKeys.size(); ++i)
    if (gpio_get_level(static_cast<gpio_num_t>(kKeys[i])) == 0)
      held = static_cast<std::uint8_t>(held | (1U << i));
  return held;
}
} // namespace board
