#pragma once

// Driver for the 400x300 UC8276C panel on the ELECROW CrowPanel ESP32-S3
// 4.2 inch board v1.2. ESP-IDF only. The pages and the refresh policy live
// in firmware/common/epaper_ui, this driver meets its epd::Panel concept.
//
// Board v1.0 has an SSD1683 instead. This driver detects it and refuses it,
// no v1.0 board has been checked here.
//
// One task owns the panel. Calls are not serialised.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "epaper_ui/panel.h"

namespace opensensor::epd {

struct Pins {
  int power = 7; // Switches the panel ground, high is on. Stays high.
  int sck = 12;
  int mosi = 11;
  int cs = 45;
  int dc = 46;
  int rst = 47;
  int busy = 48; // Active low on the UC8276C.
};

inline constexpr int kWidth = 400;
inline constexpr int kHeight = 300;
// The frame in the epaper_ui convention, 50 bytes per row.
inline constexpr std::size_t kFrameBytes =
    static_cast<std::size_t>(kWidth / 8) * kHeight;

class Uc8276c {
public:
  // Configures the pins, adds the panel to SPI2 at 10 MHz and allocates two
  // 15000 B frames in DMA capable internal RAM. Resets the panel and checks
  // the controller. Leaves it in deep sleep, the first show() is full. A
  // failure frees the frames and releases the device and SPI2 again.
  [[nodiscard]] std::expected<void, Error> begin(const Pins &pins = {});

  // Refreshes the glass and leaves the controller in standby with the
  // booster off. A partial refresh uses the DU waveform against the last
  // frame shown. The first show() after begin(), sleep() or an error is
  // full whatever `mode` says. Blocks for the refresh, up to about 4 s for
  // a full one. The UC8276C has no fast waveform here and refreshes only
  // the whole frame, so Fast is Unsupported and `window` must be the whole
  // frame. A frame of another size is BadFrame.
  [[nodiscard]] std::expected<void, Error>
  show(std::span<const std::uint8_t> frame, Refresh mode, Rect window);

  // Deep sleep. The image stays on the glass, the next show() resets the
  // controller first.
  void sleep();

  [[nodiscard]] bool ready() const { return ready_; }

private:
  enum class Mode : std::uint8_t { Full, Partial };
  enum class State : std::uint8_t { Deep, Standby, On };

  [[nodiscard]] std::expected<void, Error> wait_idle(int timeout_ms);
  [[nodiscard]] std::expected<void, Error>
  send(bool data, std::span<const std::uint8_t> bytes);
  [[nodiscard]] std::expected<void, Error>
  cmd(std::uint8_t c, std::span<const std::uint8_t> data = {});
  [[nodiscard]] std::expected<void, Error>
  luts(std::span<const std::uint8_t, 35> lut);
  [[nodiscard]] std::expected<void, Error> init();
  [[nodiscard]] std::expected<void, Error> wake();
  [[nodiscard]] std::expected<void, Error> refresh(Mode mode);
  void hw_reset();
  void enter_deep();

  Pins pins_{};
  void *dev_ = nullptr; // spi_device_handle_t, kept opaque here.
  std::uint8_t *tx_ = nullptr;
  std::uint8_t *prev_ = nullptr;
  State state_ = State::Deep;
  bool have_prev_ = false;
  bool ready_ = false;
  bool started_ = false;
};

static_assert(Panel<Uc8276c>);

} // namespace opensensor::epd
