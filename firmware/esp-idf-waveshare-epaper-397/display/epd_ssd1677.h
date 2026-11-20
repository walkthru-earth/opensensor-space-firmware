#pragma once

// Driver for the 800x480 SSD1677 panel on the Waveshare ESP32-S3-ePaper-3.97
// (SKU 33810). ESP-IDF only. The register sequences follow the Waveshare
// ESP-IDF demo (github.com/waveshareteam/ESP32-S3-ePaper-3.97 at 1ce1c424).
// The pages and the refresh policy live in firmware/common/epaper_ui, this
// driver meets its epd::Panel concept.
//
// The panel supply is the TG28 ALDO3 rail, switched through board_io.h, so
// board::start_internal_bus() must have succeeded before begin().
//
// One task owns the panel. Calls are not serialised.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <span>

#include "epaper_ui/panel.h"

namespace opensensor::epd {

inline constexpr int kWidth = 800;
inline constexpr int kHeight = 480;
inline constexpr std::size_t kStride = kWidth / 8;
// The frame in the epaper_ui convention, 100 bytes per row.
inline constexpr std::size_t kFrameBytes = kStride * kHeight;

class Ssd1677 {
public:
  // Switches the panel supply on, configures the pins, allocates a 4 KiB
  // transfer buffer in internal DMA RAM and adds the panel to SPI3 at
  // 20 MHz. The controller stays in reset until the first show(). A failure
  // releases what it took and switches the supply off again.
  [[nodiscard]] std::expected<void, Error> begin();

  // Full is the slow flashing waveform that clears ghosting. Fast is the
  // demo's quicker waveform (temperature register 0x1A = 0x6A). Both write
  // the whole frame into both RAM planes, so a later Partial has a base.
  // Partial updates only `window` against that base and is not verified on
  // the glass yet. `window` is in panel pixels, x and w multiples of 8. A
  // Partial with no base since begin(), sleep() or an error is a whole frame
  // Full instead. The driver inverts while it copies, the SSD1677 wants a
  // set bit white. The frame may sit in PSRAM. Blocks for the refresh, up to
  // about 4 s for Full.
  [[nodiscard]] std::expected<void, Error>
  show(std::span<const std::uint8_t> frame, Refresh mode, Rect window);

  // Deep sleep and panel supply off. The image stays on the glass. The next
  // show() powers up and resets the controller, and is Full or Fast.
  void sleep();

  [[nodiscard]] bool ready() const { return started_; }

private:
  // Controller states.
  //   Off      supply off or controller in deep sleep. The way out is a RST
  //            pulse and a full register init.
  //   Full     initialised for the full waveform.
  //   Fast     initialised for the fast waveform, temperature forced to 0x6A.
  //   Partial  after a partial refresh, which leaves a window and border
  //            setting that Full and Fast must init over again.
  enum class State : std::uint8_t { Off, Full, Fast, Partial };

  [[nodiscard]] static std::expected<void, Error> wait_idle(int timeout_ms);
  [[nodiscard]] std::expected<void, Error>
  transmit(std::span<const std::uint8_t> bytes);
  [[nodiscard]] std::expected<void, Error>
  cmd(std::uint8_t c, std::span<const std::uint8_t> data = {});
  [[nodiscard]] std::expected<void, Error>
  cmd(std::uint8_t c, std::initializer_list<std::uint8_t> data);
  [[nodiscard]] std::expected<void, Error>
  write_ram(std::uint8_t reg, std::span<const std::uint8_t> frame,
            const Rect &window);
  [[nodiscard]] std::expected<void, Error> power_on();
  [[nodiscard]] std::expected<void, Error> init_mode(State mode);
  [[nodiscard]] std::expected<void, Error> update(std::uint8_t sequence);
  [[nodiscard]] std::expected<void, Error>
  show_whole(std::span<const std::uint8_t> frame, State mode);
  [[nodiscard]] std::expected<void, Error>
  show_window(std::span<const std::uint8_t> frame, const Rect &window);
  static void hw_reset();
  void release();

  void *dev_ = nullptr;           // spi_device_handle_t, kept opaque here.
  std::uint8_t *chunk_ = nullptr; // Internal DMA RAM, the transfer buffer.
  State state_ = State::Off;
  bool started_ = false;
  bool powered_ = false;
  bool bus_owned_ = false;
  bool base_valid_ = false;
};

static_assert(Panel<Ssd1677>);

} // namespace opensensor::epd
