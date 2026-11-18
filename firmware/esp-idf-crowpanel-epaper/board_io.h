#pragma once
#include <array>
#include <cstdint>
#include <span>

// This adapter owns the CrowPanel I2C bus for the SEN63C, the SDSPI card on
// SPI3, the status LED and the five keys. The e-paper panel on SPI2 belongs
// to the UI task through opensensor::epd::Panel. Shared services borrow
// hooks, nothing here is board-neutral.
namespace board {
// CrowPanel ESP32-S3 4.2 inch v1.2 pins, the one copy. board_io.cpp
// configures them and the logger prints them in its boot lines.
// SEN63C on the side header. The sensor has no pull-ups and the internal
// ones are weak, so 10 k to 3V3 on SDA and SCL is required.
inline constexpr int kSensorSda = 8;
inline constexpr int kSensorScl = 9;
inline constexpr std::uint32_t kSensorI2cHz = 100000;
// microSD on SPI3. GPIO42 switches the card supply, high is on.
inline constexpr int kSdMosi = 40;
inline constexpr int kSdMiso = 13;
inline constexpr int kSdClk = 39;
inline constexpr int kSdCs = 10;
inline constexpr int kSdPower = 42;
// Red status LED, active high.
inline constexpr int kIndicatorLed = 41;
// Keys, active low. Up, Down and Press are the rotary switch, Exit and Menu
// the two side keys.
inline constexpr int kKeyUp = 6;
inline constexpr int kKeyDown = 4;
inline constexpr int kKeyPress = 5;
inline constexpr int kKeyExit = 1;
inline constexpr int kKeyMenu = 2;

// I2C_NUM_0 at kSensorI2cHz with internal pull-ups on and the SEN63C bound
// at `address`. Board task only.
[[nodiscard]] bool start_sensor_bus(std::uint8_t address);
// Sen6xIo adapters for the bound device. One call is one I2C transaction.
bool sensor_write(void *context, std::uint8_t address,
                  std::span<const std::uint8_t> data);
bool sensor_read(void *context, std::uint8_t address,
                 std::span<std::uint8_t> data);
void sensor_delay_ms(void *context, std::uint32_t milliseconds);
int sensor_last_error(); // esp_err_t of the last transaction

// Powers the card, starts SPI3 and mounts FAT at /sd. Never formats.
bool mount_card();
// Storage worker only. Unmount and mount again so a reinserted card or space
// freed elsewhere is seen. Never formats.
bool remount_card();
// True while the SD DMA buffer is reserved. Once reserved it stays, also with
// no card, so a card inserted later can mount after radios start (#168).
bool sd_buffer_reserved();
// True once, the first time no buffer is reserved when a remount fails.
bool sd_buffer_unavailable_once();
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

bool start_indicator();
void indicator(bool on);

// Configures the keys as inputs with pull-ups.
bool start_keys();
// In the order of opensensor::ui::Input.
inline constexpr std::array<int, 5> kKeys{kKeyUp, kKeyDown, kKeyPress, kKeyMenu,
                                          kKeyExit};
// Bit i is set while key kKeys[i] is held. Any task.
std::uint8_t keys_held();
} // namespace board
