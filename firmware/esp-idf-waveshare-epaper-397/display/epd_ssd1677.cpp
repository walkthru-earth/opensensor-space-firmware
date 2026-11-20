// SSD1677 controller states, tracked in `state`.
//   Off      supply off or controller in deep sleep. The way out is a RST
//            pulse and a full register init.
//   Full     initialised for the full waveform.
//   Fast     initialised for the fast waveform, temperature forced to 0x6A.
//   Partial  after a partial refresh, which leaves a window and border
//            setting that Full and Fast must init over again.
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

// A full refresh measured about 3 s in the demo. A reset settles in well
// under a second.
constexpr int kRefreshTimeoutMs = 10000;
constexpr int kResetTimeoutMs = 2000;

enum class State : std::uint8_t { Off, Full, Fast, Partial };

struct Driver {
  spi_device_handle_t dev = nullptr;
  std::uint8_t *chunk = nullptr; // Internal DMA RAM, kChunk bytes.
  State state = State::Off;
  bool started = false;
  bool powered = false;
  bool bus_owned = false;
  bool base_valid = false;
};
constinit Driver driver{};

void delay_ms(int ms) {
  const TickType_t ticks = pdMS_TO_TICKS(ms);
  vTaskDelay(ticks > 0 ? ticks : 1);
}

std::expected<void, Error> wait_idle(int timeout_ms) {
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

std::expected<void, Error> transmit(std::span<const std::uint8_t> bytes) {
  spi_transaction_t t{};
  t.length = bytes.size() * 8;
  t.tx_buffer = bytes.data();
  gpio_set_level(kCs, 0);
  const esp_err_t result = spi_device_polling_transmit(driver.dev, &t);
  gpio_set_level(kCs, 1);
  if (result != ESP_OK)
    return std::unexpected(Error::Spi);
  return {};
}

// Command byte, then up to kChunk data bytes, both staged in the DMA buffer
// so nothing is read from flash or PSRAM by the SPI DMA.
std::expected<void, Error> cmd(std::uint8_t c,
                               std::span<const std::uint8_t> data = {}) {
  driver.chunk[0] = c;
  gpio_set_level(kDc, 0);
  if (auto r = transmit({driver.chunk, 1}); !r)
    return r;
  if (data.empty())
    return {};
  for (std::size_t i = 0; i < data.size(); ++i)
    driver.chunk[i] = data[i];
  gpio_set_level(kDc, 1);
  return transmit({driver.chunk, data.size()});
}

std::expected<void, Error> cmd(std::uint8_t c,
                               std::initializer_list<std::uint8_t> data) {
  return cmd(c, std::span<const std::uint8_t>{data.begin(), data.size()});
}

constexpr std::uint8_t lo(unsigned v) { return static_cast<std::uint8_t>(v); }
constexpr std::uint8_t hi(unsigned v) {
  return static_cast<std::uint8_t>(v >> 8);
}

// Writes the rows of `window` from `frame` into RAM register `reg`,
// inverted, in kChunk transfers. The controller address counters must
// already point at the window's first byte.
std::expected<void, Error> write_ram(std::uint8_t reg,
                                     std::span<const std::uint8_t> frame,
                                     const Window &window) {
  if (auto r = cmd(reg); !r)
    return r;
  gpio_set_level(kDc, 1);
  const std::size_t row_bytes = window.w / 8;
  const std::size_t first = window.x / 8;
  std::size_t fill = 0;
  for (std::size_t y = window.y; y < std::size_t{window.y} + window.h; ++y) {
    const auto row = frame.subspan(y * kStride + first, row_bytes);
    for (const std::uint8_t byte : row) {
      driver.chunk[fill++] = static_cast<std::uint8_t>(~byte);
      if (fill == kChunk) {
        if (auto r = transmit({driver.chunk, fill}); !r)
          return r;
        fill = 0;
      }
    }
  }
  if (fill == 0)
    return {};
  return transmit({driver.chunk, fill});
}

void hw_reset() {
  gpio_set_level(kRst, 1);
  delay_ms(50);
  gpio_set_level(kRst, 0);
  delay_ms(2);
  gpio_set_level(kRst, 1);
  delay_ms(50);
}

std::expected<void, Error> power_on() {
  if (driver.powered)
    return {};
  if (!board::panel_power(true))
    return std::unexpected(Error::PanelPower);
  driver.powered = true;
  // The rail feeds the panel through Q3 and Q2, let it settle.
  delay_ms(10);
  return {};
}

// Reset and full screen window, the demo's EPD_Init and EPD_Init_Fast.
std::expected<void, Error> init_mode(State mode) {
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
       .data = {lo(kHeight - 1), hi(kHeight - 1), 0x02}}, // Gate lines.
  }};
  for (const Reg &reg : kBooster)
    if (auto r = cmd(reg.cmd, std::span{reg.data}.first(reg.len)); !r)
      return r;
  if (mode == State::Full)
    if (auto r = cmd(0x3C, {0x01}); !r) // Border waveform.
      return r;
  static constexpr std::array<Reg, 5> kWindow{{
      {.cmd = 0x11, .len = 1, .data = {0x01}}, // X increment, Y decrement.
      {.cmd = 0x44,
       .len = 4,
       .data = {0x00, 0x00, lo(kWidth - 1), hi(kWidth - 1)}},
      {.cmd = 0x45,
       .len = 4,
       .data = {lo(kHeight - 1), hi(kHeight - 1), 0x00, 0x00}},
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
  driver.state = mode;
  return {};
}

std::expected<void, Error> update(std::uint8_t sequence) {
  if (auto r = cmd(0x22, {sequence}); !r)
    return r;
  if (auto r = cmd(0x20); !r)
    return r;
  return wait_idle(kRefreshTimeoutMs);
}

std::expected<void, Error> show_whole(std::span<const std::uint8_t> frame,
                                      State mode) {
  if (driver.state != mode)
    if (auto r = init_mode(mode); !r)
      return r;
  static constexpr Window kAll{.x = 0, .y = 0, .w = kWidth, .h = kHeight};
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
  driver.base_valid = true;
  return {};
}

// The demo's EPD_Display_Partial. Only a hardware reset, so RAM 0x26 keeps
// the base image, then the window and 0x24.
std::expected<void, Error> show_window(std::span<const std::uint8_t> frame,
                                       const Window &w) {
  const unsigned x_end = w.x + w.w - 8; // First pixel of the last byte.
  const unsigned y_end = w.y + w.h - 1;
  hw_reset();
  driver.state = State::Partial;
  if (auto r = cmd(0x18, {0x80}); !r)
    return r;
  if (auto r = cmd(0x3C, {0x80}); !r)
    return r;
  if (auto r = cmd(0x44, {lo(w.x), hi(w.x), lo(x_end), hi(x_end)}); !r)
    return r;
  if (auto r = cmd(0x45, {lo(y_end), hi(y_end), lo(w.y), hi(w.y)}); !r)
    return r;
  auto origin = [&w]() -> std::expected<void, Error> {
    if (auto r = cmd(0x4E, {lo(w.x), hi(w.x)}); !r)
      return r;
    return cmd(0x4F, {lo(w.y), hi(w.y)});
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

void release() {
  if (driver.dev) {
    spi_bus_remove_device(driver.dev);
    driver.dev = nullptr;
  }
  if (driver.bus_owned) {
    spi_bus_free(kHost);
    driver.bus_owned = false;
  }
  if (driver.chunk) {
    heap_caps_free(driver.chunk);
    driver.chunk = nullptr;
  }
  if (driver.powered && board::panel_power(false))
    driver.powered = false;
}

} // namespace

const char *to_string(Error e) {
  switch (e) {
  case Error::NotStarted:
    return "not-started";
  case Error::PanelPower:
    return "panel-power";
  case Error::Gpio:
    return "gpio";
  case Error::SpiBus:
    return "spi-bus";
  case Error::SpiDevice:
    return "spi-device";
  case Error::NoMemory:
    return "no-memory";
  case Error::BusyTimeout:
    return "busy-timeout";
  case Error::Spi:
    return "spi";
  case Error::BadFrame:
    return "bad-frame";
  case Error::BadWindow:
    return "bad-window";
  case Error::NoBase:
    return "no-base";
  }
  return "unknown";
}

std::expected<void, Error> init() {
  if (driver.started)
    return {};
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
  driver.chunk = static_cast<std::uint8_t *>(
      heap_caps_malloc(kChunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  if (!driver.chunk) {
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
  driver.bus_owned = true;
  spi_device_interface_config_t device{};
  device.mode = 0;
  device.clock_speed_hz = kSpiHz;
  device.spics_io_num = -1;
  device.queue_size = 1;
  if (spi_bus_add_device(kHost, &device, &driver.dev) != ESP_OK) {
    release();
    return std::unexpected(Error::SpiDevice);
  }
  driver.state = State::Off;
  driver.base_valid = false;
  driver.started = true;
  return {};
}

std::expected<void, Error> show(std::span<const std::uint8_t> frame,
                                Refresh refresh, std::optional<Window> window) {
  if (!driver.started)
    return std::unexpected(Error::NotStarted);
  if (frame.size() != kFrameBytes)
    return std::unexpected(Error::BadFrame);
  std::expected<void, Error> result;
  switch (refresh) {
  case Refresh::Full:
    result = show_whole(frame, State::Full);
    break;
  case Refresh::Fast:
    result = show_whole(frame, State::Fast);
    break;
  case Refresh::Partial: {
    if (!window || window->w == 0 || window->h == 0 || window->x % 8 != 0 ||
        window->w % 8 != 0 || window->x + window->w > kWidth ||
        window->y + window->h > kHeight)
      return std::unexpected(Error::BadWindow);
    if (!driver.base_valid)
      return std::unexpected(Error::NoBase);
    if (auto r = power_on(); !r)
      return r;
    result = show_window(frame, *window);
    break;
  }
  }
  if (!result) {
    // The controller state is unknown, start from a reset next time.
    driver.state = State::Off;
    driver.base_valid = false;
  }
  return result;
}

void sleep() {
  if (!driver.started)
    return;
  if (driver.state != State::Off && cmd(0x10, {0x01})) // Deep sleep.
    delay_ms(10);
  // Low pins before the supply drops, so nothing back powers the panel.
  gpio_set_level(kRst, 0);
  gpio_set_level(kCs, 0);
  gpio_set_level(kDc, 0);
  delay_ms(10);
  if (driver.powered && board::panel_power(false))
    driver.powered = false;
  else if (driver.powered)
    ESP_LOGW(kTag, "panel supply stayed on");
  driver.state = State::Off;
  driver.base_valid = false;
}

} // namespace opensensor::epd
