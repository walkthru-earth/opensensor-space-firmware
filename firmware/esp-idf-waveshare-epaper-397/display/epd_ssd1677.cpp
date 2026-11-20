// SSD1677 controller states are tracked in `state_`, see the header.
//
// RAM 0x24 holds the new image and 0x26 the previous one. A set RAM bit is
// white, the opposite of the frame, so every write inverts.
#include "epd_ssd1677.h"

#include <array>

#include "board_io.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace opensensor::epd {
namespace {

constexpr const char *kTag = "epd";

// Panel pins from the Waveshare schematic and demo.
constexpr gpio_num_t kBusy = GPIO_NUM_3; // High while the controller works.
constexpr gpio_num_t kRst = GPIO_NUM_46;
constexpr gpio_num_t kDc = GPIO_NUM_9;
constexpr gpio_num_t kCs = GPIO_NUM_10; // Driven here, like the demo.
constexpr gpio_num_t kSck = GPIO_NUM_11;
constexpr gpio_num_t kMosi = GPIO_NUM_12;
constexpr spi_host_device_t kHost = SPI3_HOST;
constexpr int kSpiHz = 20 * 1000 * 1000;
constexpr std::size_t kChunk = 4096; // Also the bus max_transfer_sz.

// A full refresh measured 2531 ms on the owner unit. A reset settles in well
// under a second.
constexpr int kRefreshTimeoutMs = 10000;
constexpr int kResetTimeoutMs = 2000;

// The panel size for register arguments.
constexpr unsigned kW = kWidth;
constexpr unsigned kH = kHeight;

void delay_ms(int ms) {
  const TickType_t ticks = pdMS_TO_TICKS(ms);
  vTaskDelay(ticks > 0 ? ticks : 1);
}

spi_device_handle_t handle(void *dev) {
  return static_cast<spi_device_handle_t>(dev);
}

constexpr std::uint8_t lo(unsigned v) { return static_cast<std::uint8_t>(v); }
constexpr std::uint8_t hi(unsigned v) {
  return static_cast<std::uint8_t>(v >> 8);
}

// A window the controller can take, inside the panel and byte aligned in x.
constexpr bool window_ok(const Rect &w) {
  return w.x >= 0 && w.y >= 0 && w.w > 0 && w.h > 0 && w.x % 8 == 0 &&
         w.w % 8 == 0 && w.right() <= kWidth && w.bottom() <= kHeight;
}
constexpr Rect kAll{.x = 0, .y = 0, .w = kWidth, .h = kHeight};
static_assert(window_ok(kAll));
static_assert(!window_ok({.x = 4, .y = 0, .w = 8, .h = 1}));
static_assert(!window_ok({.x = 792, .y = 0, .w = 16, .h = 1}));
static_assert(!window_ok({.x = 0, .y = 0, .w = 0, .h = 0}));

} // namespace

std::expected<void, Error> Ssd1677::wait_idle(int timeout_ms) {
  // The demo waits 100 ms before it looks at BUSY, the controller may not
  // have raised it yet right after a command.
  delay_ms(100);
  const std::int64_t end =
      esp_timer_get_time() + std::int64_t{timeout_ms} * 1000;
  while (gpio_get_level(kBusy) != 0) {
    if (esp_timer_get_time() >= end) {
      if (gpio_get_level(kBusy) == 0)
        return {};
      ESP_LOGE(kTag, "busy timeout after %d ms", timeout_ms);
      return std::unexpected(Error::BusyTimeout);
    }
    delay_ms(20);
  }
  return {};
}

std::expected<void, Error>
Ssd1677::transmit(std::span<const std::uint8_t> bytes) {
  spi_transaction_t t{};
  t.length = bytes.size() * 8;
  t.tx_buffer = bytes.data();
  gpio_set_level(kCs, 0);
  const esp_err_t result = spi_device_polling_transmit(handle(dev_), &t);
  gpio_set_level(kCs, 1);
  if (result != ESP_OK)
    return std::unexpected(Error::Spi);
  return {};
}

// Command byte, then up to kChunk data bytes, both staged in the DMA buffer
// so nothing is read from flash or PSRAM by the SPI DMA.
std::expected<void, Error> Ssd1677::cmd(std::uint8_t c,
                                        std::span<const std::uint8_t> data) {
  chunk_[0] = c;
  gpio_set_level(kDc, 0);
  if (auto r = transmit({chunk_, 1}); !r)
    return r;
  if (data.empty())
    return {};
  for (std::size_t i = 0; i < data.size(); ++i)
    chunk_[i] = data[i];
  gpio_set_level(kDc, 1);
  return transmit({chunk_, data.size()});
}

std::expected<void, Error>
Ssd1677::cmd(std::uint8_t c, std::initializer_list<std::uint8_t> data) {
  return cmd(c, std::span<const std::uint8_t>{data.begin(), data.size()});
}

// Writes the rows of `window` from `frame` into RAM register `reg`,
// inverted, in kChunk transfers. The controller address counters must
// already point at the window's first byte.
std::expected<void, Error>
Ssd1677::write_ram(std::uint8_t reg, std::span<const std::uint8_t> frame,
                   const Rect &window) {
  if (auto r = cmd(reg); !r)
    return r;
  gpio_set_level(kDc, 1);
  const auto row_bytes = static_cast<std::size_t>(window.w / 8);
  const auto first = static_cast<std::size_t>(window.x / 8);
  const auto top = static_cast<std::size_t>(window.y);
  const auto bottom = static_cast<std::size_t>(window.bottom());
  std::size_t fill = 0;
  for (std::size_t y = top; y < bottom; ++y) {
    const auto row = frame.subspan(y * kStride + first, row_bytes);
    for (const std::uint8_t byte : row) {
      chunk_[fill++] = static_cast<std::uint8_t>(~byte);
      if (fill == kChunk) {
        if (auto r = transmit({chunk_, fill}); !r)
          return r;
        fill = 0;
      }
    }
  }
  if (fill == 0)
    return {};
  return transmit({chunk_, fill});
}

void Ssd1677::hw_reset() {
  gpio_set_level(kRst, 1);
  delay_ms(50);
  gpio_set_level(kRst, 0);
  delay_ms(2);
  gpio_set_level(kRst, 1);
  delay_ms(50);
}

std::expected<void, Error> Ssd1677::power_on() {
  if (powered_)
    return {};
  if (const auto on = board::panel_power(true); !on) {
    ESP_LOGE(kTag, "panel supply on failed, pmic %s",
             board::to_string(on.error()));
    return std::unexpected(Error::NotReady);
  }
  powered_ = true;
  // The rail feeds the panel through Q3 and Q2, let it settle.
  delay_ms(10);
  return {};
}

// Reset and full screen window, the demo's EPD_Init and EPD_Init_Fast.
std::expected<void, Error> Ssd1677::init_mode(State mode) {
  if (auto r = power_on(); !r)
    return r;
  if (mode == State::Fast)
    delay_ms(500); // As the demo, before the reset.
  hw_reset();
  if (auto r = wait_idle(kResetTimeoutMs); !r)
    return r;
  if (auto r = cmd(0x12); !r) // Software reset.
    return r;
  if (auto r = wait_idle(kResetTimeoutMs); !r)
    return r;
  if (mode == State::Full)
    if (auto r = cmd(0x18, {0x80}); !r) // Internal temperature sensor.
      return r;
  struct Reg {
    std::uint8_t cmd = 0;
    std::uint8_t len = 0;
    std::array<std::uint8_t, 5> data{};
  };
  static constexpr std::array<Reg, 2> kBooster{{
      {.cmd = 0x0C, .len = 5, .data = {0xAE, 0xC7, 0xC3, 0xC0, 0x80}},
      {.cmd = 0x01,
       .len = 3,
       .data = {lo(kH - 1), hi(kH - 1), 0x02}}, // Gate lines.
  }};
  for (const Reg &reg : kBooster)
    if (auto r = cmd(reg.cmd, std::span{reg.data}.first(reg.len)); !r)
      return r;
  if (mode == State::Full)
    if (auto r = cmd(0x3C, {0x01}); !r) // Border waveform.
      return r;
  static constexpr std::array<Reg, 5> kWindow{{
      {.cmd = 0x11, .len = 1, .data = {0x01}}, // X increment, Y decrement.
      {.cmd = 0x44, .len = 4, .data = {0x00, 0x00, lo(kW - 1), hi(kW - 1)}},
      {.cmd = 0x45, .len = 4, .data = {lo(kH - 1), hi(kH - 1), 0x00, 0x00}},
      {.cmd = 0x4E, .len = 2, .data = {0x00, 0x00}},
      {.cmd = 0x4F, .len = 2, .data = {0x00, 0x00}},
  }};
  for (const Reg &reg : kWindow)
    if (auto r = cmd(reg.cmd, std::span{reg.data}.first(reg.len)); !r)
      return r;
  if (auto r = wait_idle(kResetTimeoutMs); !r)
    return r;
  if (mode == State::Fast) {
    if (auto r = cmd(0x3C, {0x01}); !r)
      return r;
    if (auto r = cmd(0x18, {0x80}); !r)
      return r;
    if (auto r = cmd(0x1A, {0x6A}); !r) // Fast waveform, about 1.5 s.
      return r;
  }
  state_ = mode;
  return {};
}

std::expected<void, Error> Ssd1677::update(std::uint8_t sequence) {
  if (auto r = cmd(0x22, {sequence}); !r)
    return r;
  if (auto r = cmd(0x20); !r)
    return r;
  return wait_idle(kRefreshTimeoutMs);
}

std::expected<void, Error>
Ssd1677::show_whole(std::span<const std::uint8_t> frame, State mode) {
  if (state_ != mode)
    if (auto r = init_mode(mode); !r)
      return r;
  for (const std::uint8_t reg : {std::uint8_t{0x24}, std::uint8_t{0x26}}) {
    if (auto r = cmd(0x4E, {0x00, 0x00}); !r)
      return r;
    if (auto r = cmd(0x4F, {0x00, 0x00}); !r)
      return r;
    if (auto r = write_ram(reg, frame, kAll); !r)
      return r;
  }
  if (auto r = update(mode == State::Full ? 0xF7 : 0xD7); !r)
    return r;
  base_valid_ = true;
  return {};
}

// The demo's EPD_Display_Partial. Only a hardware reset, so RAM 0x26 keeps
// the base image, then the window and 0x24.
std::expected<void, Error>
Ssd1677::show_window(std::span<const std::uint8_t> frame, const Rect &w) {
  const auto x = static_cast<unsigned>(w.x);
  const auto y = static_cast<unsigned>(w.y);
  // First pixel of the last byte, and the last row.
  const unsigned x_end = x + static_cast<unsigned>(w.w) - 8;
  const unsigned y_end = y + static_cast<unsigned>(w.h) - 1;
  hw_reset();
  state_ = State::Partial;
  if (auto r = cmd(0x18, {0x80}); !r)
    return r;
  if (auto r = cmd(0x3C, {0x80}); !r)
    return r;
  if (auto r = cmd(0x44, {lo(x), hi(x), lo(x_end), hi(x_end)}); !r)
    return r;
  if (auto r = cmd(0x45, {lo(y_end), hi(y_end), lo(y), hi(y)}); !r)
    return r;
  auto origin = [this, x, y]() -> std::expected<void, Error> {
    if (auto r = cmd(0x4E, {lo(x), hi(x)}); !r)
      return r;
    return cmd(0x4F, {lo(y), hi(y)});
  };
  if (auto r = origin(); !r)
    return r;
  if (auto r = write_ram(0x24, frame, w); !r)
    return r;
  if (auto r = update(0xFF); !r)
    return r;
  // The window is now on the glass, make it the base for the next partial.
  if (auto r = origin(); !r)
    return r;
  return write_ram(0x26, frame, w);
}

void Ssd1677::release() {
  if (dev_) {
    spi_bus_remove_device(handle(dev_));
    dev_ = nullptr;
  }
  if (bus_owned_) {
    spi_bus_free(kHost);
    bus_owned_ = false;
  }
  if (chunk_) {
    heap_caps_free(chunk_);
    chunk_ = nullptr;
  }
  if (powered_ && board::panel_power(false))
    powered_ = false;
}

std::expected<void, Error> Ssd1677::begin() {
  if (started_)
    return std::unexpected(Error::AlreadyStarted);
  if (auto r = power_on(); !r)
    return r;
  const gpio_config_t outputs{
      .pin_bit_mask = (1ULL << kRst) | (1ULL << kDc) | (1ULL << kCs),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  const gpio_config_t busy{
      .pin_bit_mask = 1ULL << kBusy,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  if (gpio_config(&outputs) != ESP_OK || gpio_config(&busy) != ESP_OK) {
    release();
    return std::unexpected(Error::Gpio);
  }
  gpio_set_level(kCs, 1);
  gpio_set_level(kRst, 1);
  chunk_ = static_cast<std::uint8_t *>(
      heap_caps_malloc(kChunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  if (!chunk_) {
    release();
    return std::unexpected(Error::NoMemory);
  }
  spi_bus_config_t bus{};
  bus.mosi_io_num = kMosi;
  bus.miso_io_num = -1;
  bus.sclk_io_num = kSck;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = kChunk;
  if (spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
    release();
    return std::unexpected(Error::SpiBus);
  }
  bus_owned_ = true;
  spi_device_interface_config_t device{};
  device.mode = 0;
  device.clock_speed_hz = kSpiHz;
  device.spics_io_num = -1;
  device.queue_size = 1;
  spi_device_handle_t dev = nullptr;
  if (spi_bus_add_device(kHost, &device, &dev) != ESP_OK) {
    release();
    return std::unexpected(Error::SpiDevice);
  }
  dev_ = dev;
  state_ = State::Off;
  base_valid_ = false;
  started_ = true;
  return {};
}

std::expected<void, Error> Ssd1677::show(std::span<const std::uint8_t> frame,
                                         Refresh mode, Rect window) {
  if (!started_)
    return std::unexpected(Error::NotReady);
  if (frame.size() != kFrameBytes)
    return std::unexpected(Error::BadFrame);
  if (mode == Refresh::Partial && !window_ok(window))
    return std::unexpected(Error::BadWindow);
  // No base in controller RAM after begin(), sleep() or an error. A partial
  // now would refresh against stale RAM, so the frame goes out whole with
  // the clean waveform instead.
  if (mode == Refresh::Partial && !base_valid_)
    mode = Refresh::Full;
  std::expected<void, Error> result;
  switch (mode) {
  case Refresh::Full:
    result = show_whole(frame, State::Full);
    break;
  case Refresh::Fast:
    result = show_whole(frame, State::Fast);
    break;
  case Refresh::Partial:
    result = power_on();
    if (result)
      result = show_window(frame, window);
    break;
  }
  if (!result) {
    // The controller state is unknown, start from a reset next time.
    state_ = State::Off;
    base_valid_ = false;
  }
  return result;
}

void Ssd1677::sleep() {
  if (!started_)
    return;
  if (state_ != State::Off && cmd(0x10, {0x01})) // Deep sleep.
    delay_ms(10);
  // Low pins before the supply drops, so nothing back powers the panel.
  gpio_set_level(kRst, 0);
  gpio_set_level(kCs, 0);
  gpio_set_level(kDc, 0);
  delay_ms(10);
  if (powered_ && board::panel_power(false))
    powered_ = false;
  else if (powered_)
    ESP_LOGW(kTag, "panel supply stayed on");
  state_ = State::Off;
  base_valid_ = false;
}

} // namespace opensensor::epd
