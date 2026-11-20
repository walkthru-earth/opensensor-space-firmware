#pragma once

// Driver for the 800x480 SSD1677 panel on the Waveshare ESP32-S3-ePaper-3.97
// (SKU 33810). ESP-IDF only. The register sequences follow the Waveshare
// ESP-IDF demo (github.com/waveshareteam/ESP32-S3-ePaper-3.97 at 1ce1c424).
//
// The panel supply is the TG28 ALDO3 rail, switched through board_io.h, so
// board::start_internal_bus() must have succeeded before init().
//
// One task owns the panel. Calls are not serialised.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace opensensor::epd {

inline constexpr std::uint16_t kWidth = 800;
inline constexpr std::uint16_t kHeight = 480;
inline constexpr std::size_t kStride = kWidth / 8;
inline constexpr std::size_t kFrameBytes = kStride * kHeight;

// Full is the slow flashing waveform that clears ghosting. Fast is the
// demo's 1.5 s waveform (temperature register 0x1A = 0x6A). Partial updates
// only `window` against the last Full or Fast frame and is not verified on
// the glass yet.
enum class Refresh : std::uint8_t { Full, Fast, Partial };

enum class [[nodiscard]] Error : std::uint8_t {
  NotStarted,  // init() has not succeeded.
  PanelPower,  // The PMIC did not switch ALDO3.
  Gpio,        // A pin could not be configured.
  SpiBus,      // spi_bus_initialize failed, SPI3 may be taken.
  SpiDevice,   // spi_bus_add_device failed.
  NoMemory,    // No internal DMA RAM for the 4 KiB transfer buffer.
  BusyTimeout, // The controller did not release BUSY in time.
  Spi,         // A transfer failed.
  BadFrame,    // The frame is not kFrameBytes long.
  BadWindow,   // Partial without a window, or one off the panel or unaligned.
  NoBase,      // Partial before a Full or Fast refresh since init or sleep.
};

[[nodiscard]] const char *to_string(Error e);

// A partial update area in panel pixels. `x` and `w` are multiples of 8.
struct Window {
  std::uint16_t x = 0;
  std::uint16_t y = 0;
  std::uint16_t w = 0;
  std::uint16_t h = 0;
};

// Switches the panel supply on, configures the pins and adds the panel to
// SPI3 at 20 MHz. The controller stays in reset until the first show(). A
// failure releases what it took and switches the supply off again.
[[nodiscard]] std::expected<void, Error> init();

// `frame` is 1 bit per pixel, row major, stride kStride, MSB the leftmost
// pixel, a set bit black, the shared e-paper canvas layout. The SSD1677
// wants a set bit white, so the driver inverts while it copies. The frame
// may sit in PSRAM. Full and Fast write both RAM planes so a later Partial
// has a base. Blocks for the refresh, up to about 4 s for Full.
[[nodiscard]] std::expected<void, Error>
show(std::span<const std::uint8_t> frame, Refresh refresh,
     std::optional<Window> window = std::nullopt);

// Deep sleep and panel supply off. The image stays on the glass. The next
// show() powers up and resets the controller, and needs Full or Fast first.
void sleep();

} // namespace opensensor::epd
