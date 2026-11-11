#pragma once
#include <cstddef>
#include <cstdint>

// This adapter owns V2 UART1, SDMMC and the optional gauge/RGB controllers.
// It is deliberately local to the Waveshare trial; shared services borrow
// hooks.
namespace board {
// V2 pins, the one copy. board_io.cpp configures them and each app prints
// them in its boot lines.
inline constexpr int kSensorRx = 1; // sensor TX -> host RX
inline constexpr int kSensorTx = 2; // sensor RX <- host TX; V1 gauge conflict
inline constexpr int kSdClk = 5;
inline constexpr int kSdCmd = 4;
inline constexpr int kSdData0 = 6;
inline constexpr int kGaugeSda = 15;
inline constexpr int kGaugeScl = 16;
inline constexpr int kIndicatorLed = 38;

bool start_sensor();
int read_sensor();
bool mount_card();
// Storage worker only, under the logger bus lock. Unmount and mount again so
// a reinserted card or space freed elsewhere is seen. Never formats.
bool remount_card();
std::uint32_t cluster_bytes(); // 0 when unknown
std::uint32_t volume_serial(); // FAT volume serial, 0 when unknown
bool storage_free_bytes(std::uint64_t &free_bytes);
std::uint64_t card_bytes();
std::uint64_t storage_total_bytes();
std::uint64_t storage_used_bytes();
bool start_gauge();
// Max17048Io::read adapter for the gauge device on I2C_NUM_0. `address` must
// be the device address bound in start_gauge. Keeps the last esp_err_t.
bool gauge_read(void *context, std::uint8_t address, std::uint8_t reg,
                std::uint8_t *data, std::size_t size);
// Max17048Io::write adapter, same device and error tracking. Only the
// shared driver's reset-indicator clear uses it.
bool gauge_write(void *context, std::uint8_t address, std::uint8_t reg,
                 const std::uint8_t *data, std::size_t size);
int gauge_last_error();
bool start_indicator();
void indicator(std::uint8_t red, std::uint8_t green, std::uint8_t blue);
} // namespace board
