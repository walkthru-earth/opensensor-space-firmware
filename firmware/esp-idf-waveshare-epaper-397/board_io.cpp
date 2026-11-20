#include "board_io.h"
#include <array>
#include <atomic>
#include <ctime>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <fat_volume.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sd_transfer_buffer.h>
#include <sdmmc_cmd.h>
#include <utc_clock.h>

namespace board {
namespace {
// Per transaction. The SEN6x clock stretches for no command, so 50 ms covers
// a 48 byte string read at 100 kHz with room to spare.
constexpr int kI2cTimeoutMs = 50;
// A probe of an absent address ends at the NACK, well inside this.
constexpr int kProbeTimeoutMs = 20;

i2c_master_bus_handle_t sensor_bus = nullptr;
i2c_master_dev_handle_t sensor = nullptr;
std::uint8_t sensor_address = 0;
esp_err_t sensor_error = ESP_OK;

i2c_master_bus_handle_t internal_bus = nullptr;
i2c_master_dev_handle_t pmic = nullptr;
i2c_master_dev_handle_t rtc = nullptr;

sdmmc_card_t *card = nullptr;
opensensor::storage::SdTransferBuffer sd_transfer_buffer;
std::atomic<bool> mounted{false};
std::atomic<std::uint64_t> free_bytes_cache{0};
std::uint64_t cached_total_bytes = 0;
std::uint64_t cached_used_bytes = 0;

// AXP2101 register map, which the TG28 follows for these registers.
namespace pmic_reg {
constexpr std::uint8_t kStatus1 = 0x00; // bit 5 VBUS good, bit 3 battery
constexpr std::uint8_t kChipId = 0x03;
constexpr std::uint8_t kAdcEnable = 0x30;  // bit 0 VBAT, bit 2 VBUS
constexpr std::uint8_t kVbatHigh = 0x34;   // 6 bits, then 0x35 low 8, in mV
constexpr std::uint8_t kLdoEnable = 0x90;  // bit 2 ALDO3
constexpr std::uint8_t kAldo3Volts = 0x94; // bits 4:0, 0.5 V + 0.1 V steps
constexpr std::uint8_t kBatteryPercent = 0xA4;
} // namespace pmic_reg
constexpr std::uint8_t kAldo3Bit = 1U << 2;
constexpr std::uint8_t kAldo3_3v3 = (3300 - 500) / 100;

// PCF85063 registers.
namespace rtc_reg {
constexpr std::uint8_t kControl1 = 0x00; // bit 5 STOP, bit 1 12 hour mode
constexpr std::uint8_t kSeconds = 0x04;  // bit 7 oscillator stopped
} // namespace rtc_reg

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

i2c_master_bus_handle_t new_bus(i2c_port_num_t port, int sda, int scl) {
  i2c_master_bus_config_t config{};
  config.i2c_port = port;
  config.sda_io_num = static_cast<gpio_num_t>(sda);
  config.scl_io_num = static_cast<gpio_num_t>(scl);
  config.clk_source = I2C_CLK_SRC_DEFAULT;
  config.glitch_ignore_cnt = 7;
  config.flags.enable_internal_pullup = true;
  i2c_master_bus_handle_t created = nullptr;
  return i2c_new_master_bus(&config, &created) == ESP_OK ? created : nullptr;
}

i2c_master_dev_handle_t add_device(i2c_master_bus_handle_t bus,
                                   std::uint8_t address, std::uint32_t hz) {
  i2c_device_config_t device{};
  device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  device.device_address = address;
  device.scl_speed_hz = hz;
  i2c_master_dev_handle_t created = nullptr;
  return i2c_master_bus_add_device(bus, &device, &created) == ESP_OK ? created
                                                                     : nullptr;
}

std::expected<std::uint8_t, PmicError> pmic_read(std::uint8_t reg) {
  if (!pmic)
    return std::unexpected(PmicError::NotStarted);
  std::uint8_t value = 0;
  if (i2c_master_transmit_receive(pmic, &reg, 1, &value, 1, kI2cTimeoutMs) !=
      ESP_OK)
    return std::unexpected(PmicError::I2c);
  return value;
}

std::expected<void, PmicError> pmic_write(std::uint8_t reg,
                                          std::uint8_t value) {
  if (!pmic)
    return std::unexpected(PmicError::NotStarted);
  const std::array<std::uint8_t, 2> frame{reg, value};
  if (i2c_master_transmit(pmic, frame.data(), frame.size(), kI2cTimeoutMs) !=
      ESP_OK)
    return std::unexpected(PmicError::I2c);
  return {};
}

// Read, change the bits in `mask` to `bits`, write back when they differ.
std::expected<void, PmicError> pmic_update(std::uint8_t reg, std::uint8_t mask,
                                           std::uint8_t bits) {
  const auto current = pmic_read(reg);
  if (!current)
    return std::unexpected(current.error());
  const auto next =
      static_cast<std::uint8_t>((*current & ~mask) | (bits & mask));
  if (next == *current)
    return {};
  return pmic_write(reg, next);
}

constexpr std::uint8_t to_bcd(int value) {
  return static_cast<std::uint8_t>(((value / 10) << 4) | (value % 10));
}
constexpr int from_bcd(std::uint8_t value) {
  return (value >> 4) * 10 + (value & 0x0F);
}

bool rtc_registers(std::uint8_t reg, std::span<std::uint8_t> out) {
  return rtc &&
         i2c_master_transmit_receive(rtc, &reg, 1, out.data(), out.size(),
                                     kI2cTimeoutMs) == ESP_OK;
}

// A remount keeps the DMA buffer reserved: once radios run, internal DMA
// memory may be too fragmented to reserve it again.
bool mount(bool keep_buffer) {
  if (card)
    return true;
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  // IDF treats any request above 20 MHz as a high-speed mode switch. Stay at
  // default speed until a card on this board is qualified faster.
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;
  // FatFs buffers can live in PSRAM while SDMMC still needs an internal DMA
  // staging buffer. Reserve it before Wi-Fi and TCP compete for internal RAM.
  if (!sd_transfer_buffer.reserve())
    return false;
  host.dma_aligned_buffer = sd_transfer_buffer.data();
  host.unaligned_multi_block_rw_max_chunk_size =
      opensensor::storage::SdTransferBuffer::kSectors;
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 4;
  slot.clk = static_cast<gpio_num_t>(kSdClk);
  slot.cmd = static_cast<gpio_num_t>(kSdCmd);
  slot.d0 = static_cast<gpio_num_t>(kSdData0);
  slot.d1 = static_cast<gpio_num_t>(kSdData1);
  slot.d2 = static_cast<gpio_num_t>(kSdData2);
  slot.d3 = static_cast<gpio_num_t>(kSdData3);
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
  const esp_vfs_fat_sdmmc_mount_config_t config =
      opensensor::storage::archive_mount_config();
  sdmmc_card_t *created = nullptr;
  const esp_err_t result =
      esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &config, &created);
  // The IDF mount helper releases its host and FAT resources on failure.
  if (result != ESP_OK) {
    if (!keep_buffer)
      sd_transfer_buffer.release();
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
  i2c_master_bus_handle_t created_bus =
      sensor_bus ? sensor_bus : new_bus(I2C_NUM_1, kSensorSda, kSensorScl);
  if (!created_bus)
    return false;
  sensor_bus = created_bus;
  sensor = add_device(sensor_bus, address, kSensorI2cHz);
  if (!sensor)
    return false;
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

bool start_internal_bus() {
  if (internal_bus)
    return pmic && rtc;
  internal_bus = new_bus(I2C_NUM_0, kInternalSda, kInternalScl);
  if (!internal_bus)
    return false;
  pmic = add_device(internal_bus, kPmicAddress, kInternalI2cHz);
  rtc = add_device(internal_bus, kRtcAddress, kInternalI2cHz);
  return pmic && rtc;
}

std::size_t scan(Bus bus, std::span<std::uint8_t> found) {
  const auto handle = bus == Bus::Internal ? internal_bus : sensor_bus;
  if (!handle)
    return 0;
  std::size_t count = 0;
  for (std::uint16_t address = 0x08; address < 0x78; ++address) {
    if (i2c_master_probe(handle, address, kProbeTimeoutMs) != ESP_OK)
      continue;
    if (count < found.size())
      found[count] = static_cast<std::uint8_t>(address);
    ++count;
  }
  return count;
}

const char *to_string(PmicError e) {
  switch (e) {
  case PmicError::NotStarted:
    return "not-started";
  case PmicError::I2c:
    return "i2c";
  }
  return "unknown";
}

std::expected<std::uint8_t, PmicError> pmic_chip_id() {
  return pmic_read(pmic_reg::kChipId);
}

std::expected<std::uint8_t, PmicError> pmic_ldo_enables() {
  return pmic_read(pmic_reg::kLdoEnable);
}

std::expected<void, PmicError> pmic_start_adc() {
  constexpr std::uint8_t kVbatVbus = (1U << 0) | (1U << 2);
  return pmic_update(pmic_reg::kAdcEnable, kVbatVbus, kVbatVbus);
}

std::expected<void, PmicError> panel_power(bool on) {
  if (on) {
    const auto volts = pmic_update(pmic_reg::kAldo3Volts, 0x1F, kAldo3_3v3);
    if (!volts)
      return volts;
  }
  return pmic_update(pmic_reg::kLdoEnable, kAldo3Bit, on ? kAldo3Bit : 0);
}

std::expected<PowerStatus, PmicError> read_power() {
  const auto status = pmic_read(pmic_reg::kStatus1);
  if (!status)
    return std::unexpected(status.error());
  PowerStatus out{.vbus_good = (*status & (1U << 5)) != 0,
                  .battery_present = (*status & (1U << 3)) != 0,
                  .battery_mv = std::nullopt,
                  .battery_percent = std::nullopt};
  if (!out.battery_present)
    return out;
  const auto high = pmic_read(pmic_reg::kVbatHigh);
  const auto low = pmic_read(pmic_reg::kVbatHigh + 1);
  if (high && low)
    out.battery_mv = static_cast<std::uint16_t>(((*high & 0x3F) << 8) | *low);
  const auto percent = pmic_read(pmic_reg::kBatteryPercent);
  if (percent && *percent <= 100)
    out.battery_percent = *percent;
  return out;
}

bool rtc_read(std::int64_t &seconds, void *) {
  std::array<std::uint8_t, 1> control{};
  std::array<std::uint8_t, 7> time{};
  if (!rtc_registers(rtc_reg::kControl1, control) ||
      !rtc_registers(rtc_reg::kSeconds, time))
    return false;
  // STOP or 12 hour mode, or the oscillator stopped since the last write.
  if ((control[0] & ((1U << 5) | (1U << 1))) != 0 || (time[0] & 0x80) != 0)
    return false;
  const int second = from_bcd(time[0] & 0x7F);
  const int minute = from_bcd(time[1] & 0x7F);
  const int hour = from_bcd(time[2] & 0x3F);
  const int day = from_bcd(time[3] & 0x3F);
  const int month = from_bcd(time[5] & 0x1F);
  const int year = 2000 + from_bcd(time[6]);
  if (year < 2020 || month < 1 || month > 12 || hour > 23 || minute > 59 ||
      second > 59)
    return false;
  static constexpr std::array<int, 12> kDays{31, 28, 31, 30, 31, 30,
                                             31, 31, 30, 31, 30, 31};
  const int max_day = kDays[static_cast<std::size_t>(month - 1)] +
                      (month == 2 && year % 4 == 0 ? 1 : 0);
  if (day < 1 || day > max_day)
    return false;
  seconds =
      opensensor::utc::days_from_civil(year, unsigned(month), unsigned(day)) *
          86400 +
      hour * 3600 + minute * 60 + second;
  return opensensor::utc::supported_epoch(seconds);
}

bool rtc_write(std::int64_t seconds, void *) {
  if (!rtc || !opensensor::utc::supported_epoch(seconds))
    return false;
  const auto value = static_cast<std::time_t>(seconds);
  std::tm utc{};
  if (!::gmtime_r(&value, &utc) || utc.tm_year < 100 || utc.tm_year > 199)
    return false;
  // 24 hour mode with the clock running, the other control bits as they are.
  std::array<std::uint8_t, 1> control{};
  if (!rtc_registers(rtc_reg::kControl1, control))
    return false;
  if ((control[0] & ((1U << 5) | (1U << 1))) != 0) {
    const std::array<std::uint8_t, 2> run{
        rtc_reg::kControl1,
        static_cast<std::uint8_t>(control[0] & ~((1U << 5) | (1U << 1)))};
    if (i2c_master_transmit(rtc, run.data(), run.size(), kI2cTimeoutMs) !=
        ESP_OK)
      return false;
  }
  // Writing the seconds register with bit 7 clear also clears the oscillator
  // stop flag.
  const std::array<std::uint8_t, 8> frame{
      rtc_reg::kSeconds,      to_bcd(utc.tm_sec),
      to_bcd(utc.tm_min),     to_bcd(utc.tm_hour),
      to_bcd(utc.tm_mday),    static_cast<std::uint8_t>(utc.tm_wday),
      to_bcd(utc.tm_mon + 1), to_bcd(utc.tm_year - 100)};
  if (i2c_master_transmit(rtc, frame.data(), frame.size(), kI2cTimeoutMs) !=
      ESP_OK)
    return false;
  std::int64_t readback = 0;
  return rtc_read(readback, nullptr) && readback >= seconds &&
         readback - seconds <= 1;
}

bool mount_card() { return mount(false); }
bool remount_card() {
  if (card && esp_vfs_fat_sdcard_unmount("/sd", card) != ESP_OK)
    return false;
  card = nullptr;
  mounted.store(false, std::memory_order_release);
  return mount(true);
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
} // namespace board
