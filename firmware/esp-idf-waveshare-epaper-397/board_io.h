#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

// This adapter owns the two I2C buses, the TG28 PMIC, the PCF85063 RTC and
// the 4-bit SDMMC card of the Waveshare ESP32-S3-ePaper-3.97 (SKU 33810).
// The SSD1677 panel on SPI3 belongs to the display task through
// opensensor::epd, which switches its supply here. Shared services borrow
// hooks, nothing here is board-neutral.
namespace board {
// Waveshare ESP32-S3-ePaper-3.97 pins, the one copy. board_io.cpp configures
// them and the logger prints them in its boot lines.
//
// SEN63C on the GPIO43/44 header with 10 k pull-ups to 3V3, its own bus.
// The console is USB-Serial/JTAG because these two pins are UART0 by default.
inline constexpr int kSensorSda = 43;
inline constexpr int kSensorScl = 44;
inline constexpr std::uint32_t kSensorI2cHz = 100000;
// Internal bus with 4.7 k pull-ups. TG28 PMIC 0x34, PCF85063 RTC 0x51,
// SHTC3 0x70, ES8311 0x18 and QMI8658 0x6A or 0x6B. Only the PMIC and the
// RTC are used, the others are left in their reset state.
inline constexpr int kInternalSda = 41;
inline constexpr int kInternalScl = 42;
inline constexpr std::uint32_t kInternalI2cHz = 100000;
inline constexpr std::uint8_t kPmicAddress = 0x34;
inline constexpr std::uint8_t kRtcAddress = 0x51;
// microSD on SDMMC, 4 bit. Always powered, no card detect.
inline constexpr int kSdClk = 16;
inline constexpr int kSdCmd = 17;
inline constexpr int kSdData0 = 15;
inline constexpr int kSdData1 = 7;
inline constexpr int kSdData2 = 8;
inline constexpr int kSdData3 = 18;

// I2C_NUM_1 at kSensorI2cHz and the SEN63C bound at `address`. Board task
// only.
[[nodiscard]] bool start_sensor_bus(std::uint8_t address);
// Sen6xIo adapters for the bound device. One call is one I2C transaction.
bool sensor_write(void *context, std::uint8_t address,
                  std::span<const std::uint8_t> data);
bool sensor_read(void *context, std::uint8_t address,
                 std::span<std::uint8_t> data);
void sensor_delay_ms(void *context, std::uint32_t milliseconds);
int sensor_last_error(); // esp_err_t of the last transaction

// I2C_NUM_0 with the PMIC and RTC bound. Board task, before any other
// internal bus call. The ESP-IDF master driver serialises the bus, so the
// display task may switch the panel supply while the board task reads.
[[nodiscard]] bool start_internal_bus();

// Probes every 7 bit address on one bus and writes the ones that answer.
// Returns how many answered, which may exceed `found.size()`.
enum class Bus : std::uint8_t { Internal, Sensor };
std::size_t scan(Bus bus, std::span<std::uint8_t> found);

// TG28 PMIC, register compatible with the AXP2101 for what is used here.
enum class [[nodiscard]] PmicError : std::uint8_t { NotStarted, I2c };
[[nodiscard]] const char *to_string(PmicError e);
// Chip id register 0x03.
[[nodiscard]] std::expected<std::uint8_t, PmicError> pmic_chip_id();
// LDO enable register 0x90, bit 0 ALDO1 to bit 3 ALDO4.
[[nodiscard]] std::expected<std::uint8_t, PmicError> pmic_ldo_enables();
// Turns on the VBAT and VBUS ADC channels, leaves the rest as they are.
[[nodiscard]] std::expected<void, PmicError> pmic_start_adc();
// ALDO3 feeds the e-paper panel. On sets 3.3 V first.
[[nodiscard]] std::expected<void, PmicError> panel_power(bool on);
struct PowerStatus {
  bool vbus_good = false;
  bool battery_present = false;
  std::optional<std::uint16_t> battery_mv;
  std::optional<std::uint8_t> battery_percent;
};
[[nodiscard]] std::expected<PowerStatus, PmicError> read_power();

// PCF85063 RTC as UTC seconds since 1970. False while the oscillator stop
// flag is set, the clock is stopped, in 12 hour mode, or out of range.
bool rtc_read(std::int64_t &seconds, void *context);
// Writes UTC seconds and reads them back. Clears the oscillator stop flag.
bool rtc_write(std::int64_t seconds, void *context);

// Mounts FAT at /sd over 4-bit SDMMC. Never formats.
bool mount_card();
// Storage worker only. Unmount and mount again so a reinserted card or space
// freed elsewhere is seen. Never formats.
bool remount_card();
// Any task. True while a card is mounted.
bool card_mounted();
std::uint32_t cluster_bytes(); // 0 when unknown
std::uint32_t volume_serial(); // FAT volume serial, 0 when unknown
bool storage_free_bytes(std::uint64_t &free_bytes);
// Any task. The last value storage_free_bytes read, 0 before the first.
std::uint64_t cached_free_bytes();
std::uint64_t card_bytes();
std::uint64_t storage_total_bytes();
std::uint64_t storage_used_bytes();
} // namespace board
