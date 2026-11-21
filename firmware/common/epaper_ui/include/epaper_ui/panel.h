#pragma once

// What the shared UI asks of an e-paper driver. Each board keeps its own
// driver with its pins, bus and controller commands, and the driver meets
// the Panel concept below. A concept, not a virtual base, so a call costs no
// vtable and an image links only its own driver.
//
// Frame convention, the same for every panel and every DisplayProfile:
//   1 bit per pixel, rows top to bottom, each row left to right.
//   stride = width / 8 bytes per row, width is a multiple of 8.
//   The most significant bit of a byte is its leftmost pixel.
//   A set bit is black, a clear bit is white.
// A driver whose controller wants another order converts inside show().

#include <concepts>
#include <cstdint>
#include <expected>
#include <span>

#include "epaper_ui/geometry.h"

namespace opensensor::epd {

using Rect = ui::Rect;

// Full is the clean waveform with a flash. Fast is a quicker full screen
// waveform. Partial updates without a flash and leaves faint ghosts.
enum class Refresh : std::uint8_t { Full, Fast, Partial };

[[nodiscard]] const char *to_string(Refresh r);

enum class [[nodiscard]] Error : std::uint8_t {
  NotReady,              // begin() has not succeeded.
  AlreadyStarted,        // begin() ran before.
  Gpio,                  // A pin could not be configured.
  SpiBus,                // The SPI bus could not start, the host may be taken.
  SpiDevice,             // The panel could not join the bus.
  NoMemory,              // No memory for the driver frames.
  UnsupportedController, // The board carries a controller this driver refuses.
  BusyTimeout,           // The controller did not release BUSY in time.
  Spi,                   // A transfer failed.
  BadFrame,              // The frame size is not the panel's.
  BadWindow,             // A window outside the frame, not byte aligned, or
                         // one the panel cannot refresh on its own.
  Unsupported,           // A refresh mode the panel does not offer.
};

[[nodiscard]] const char *to_string(Error e);

// Shows `frame` with refresh `mode`. `window` is the part that changed, the
// whole frame unless the profile offers partial_window. The frame is always
// the whole screen. The shared refresh policy never asks for a mode the
// profile does not list, a driver still refuses one with Unsupported.
// sleep() puts the controller in its lowest power state, the image stays.
template <typename P>
concept Panel = requires(P &panel, std::span<const std::uint8_t> frame,
                         Refresh mode, Rect window) {
  {
    panel.show(frame, mode, window)
  } -> std::same_as<std::expected<void, Error>>;
  panel.sleep();
};

} // namespace opensensor::epd
