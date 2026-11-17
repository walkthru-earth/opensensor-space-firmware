#pragma once

// Driver for the 400x300 UC8276C panel on the ELECROW CrowPanel ESP32-S3
// 4.2 inch board v1.2. ESP-IDF only, the rest of display/ is host portable.
//
// Ported from the epd_ssd1683 component of github.com/yharby/bible-eink
// (CC-BY-4.0, Walkthru.Earth), which was proven on the same board. That
// driver also handles the SSD1683 of board v1.0. This one detects it and
// refuses it, no v1.0 board has been checked here.
//
// One task owns the panel. Calls are not serialised.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "canvas.h"

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

enum class Mode : std::uint8_t { Full, Partial };

enum class [[nodiscard]] Error : std::uint8_t {
  NotReady,              // begin() has not succeeded.
  AlreadyStarted,        // begin() ran before.
  Gpio,                  // A pin could not be configured.
  SpiBus,                // spi_bus_initialize failed, the host may be taken.
  SpiDevice,             // spi_bus_add_device failed.
  NoMemory,              // No DMA capable internal RAM for the two frames.
  UnsupportedController, // BUSY idles low, an SSD1683 board v1.0.
  BusyTimeout,           // The controller did not release BUSY in time.
  Spi,                   // A transfer failed.
};

[[nodiscard]] const char *to_string(Error e);

// The panel frame, the same layout as ui::Canvas. 1 bit per pixel, MSB is
// the left pixel, a set bit is black.
using Frame = std::span<const std::uint8_t, ui::kFrameBytes>;

class Panel {
public:
  // Configures the pins, adds the panel to SPI2 at 10 MHz and allocates two
  // 15000 B frames in DMA capable internal RAM. Resets the panel and checks
  // the controller. Leaves it in deep sleep, the first show() is full.
  [[nodiscard]] std::expected<void, Error> begin(const Pins &pins = {});

  // Refreshes the glass and leaves the controller in standby with the
  // booster off. A partial refresh uses the DU waveform against the last
  // frame shown. The first show() after begin(), sleep() or an error is
  // full whatever `mode` says. Blocks for the refresh, up to about 4 s for
  // a full one.
  [[nodiscard]] std::expected<void, Error> show(Frame frame, Mode mode);

  // Deep sleep. The image stays on the glass, the next show() resets the
  // controller first.
  void sleep();

  [[nodiscard]] bool ready() const { return ready_; }

private:
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

} // namespace opensensor::epd
