// UC8276C init values and the GC and DU waveforms come from Elecrow's v1.2
// demo driver as documented by
// ESPBoards/esphome-lvgl-crowpanel-epaper-5.79-4.2, by way of the bible-eink
// epd_ssd1683 component.
//
// Controller power states, tracked in `state_`.
//   Deep     deep sleep, registers and RAM lost, the way out is a RST pulse
//            and a full init. Also the state after begin() and after an error.
//   Standby  booster off, registers kept. Needs only 0x04 power on to
//            refresh again.
//   On       booster on, only inside show().
// Every show() ends in Standby by 0x02 power off. sleep() goes to Deep.
#include "epd_uc8276c.h"

#include <array>
#include <cstring>

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
constexpr std::size_t kBytes = ui::kFrameBytes;
// Under the 4096 B bus max_transfer_sz, word aligned.
constexpr std::size_t kChunk = 4092;

// Each LUT register holds six 7 byte phase groups, only the first is used
// and the rest are zero. Rows are VCOM, WW, BW, WB, BB.
static constexpr std::array<std::uint8_t, 35> kLutFull{
    0x01, 0x14, 0x0A, 0x14, 0x00, 0x01, 0x01, //
    0x01, 0x54, 0x0A, 0x94, 0x00, 0x01, 0x01, //
    0x01, 0x54, 0x0A, 0x94, 0x00, 0x01, 0x01, //
    0x01, 0x94, 0x0A, 0x54, 0x00, 0x01, 0x01, //
    0x01, 0x94, 0x0A, 0x54, 0x00, 0x01, 0x01, //
};
// DU waveform, unchanged pixels get an idle drive so only changes move.
static constexpr std::array<std::uint8_t, 35> kLutPart{
    0x01, 0x14, 0x00, 0x00, 0x00, 0x01, 0x00, //
    0x01, 0x14, 0x00, 0x00, 0x00, 0x01, 0x00, //
    0x01, 0x94, 0x00, 0x00, 0x00, 0x01, 0x00, //
    0x01, 0x54, 0x00, 0x00, 0x00, 0x01, 0x00, //
    0x01, 0x14, 0x00, 0x00, 0x00, 0x01, 0x00, //
};

void delay_ms(int ms) {
  vTaskDelay(pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : 1);
}

gpio_num_t pin(int p) { return static_cast<gpio_num_t>(p); }

spi_device_handle_t handle(void *dev) {
  return static_cast<spi_device_handle_t>(dev);
}

} // namespace

const char *to_string(Error e) {
  switch (e) {
  case Error::NotReady:
    return "not-ready";
  case Error::AlreadyStarted:
    return "already-started";
  case Error::Gpio:
    return "gpio";
  case Error::SpiBus:
    return "spi-bus";
  case Error::SpiDevice:
    return "spi-device";
  case Error::NoMemory:
    return "no-memory";
  case Error::UnsupportedController:
    return "unsupported-controller";
  case Error::BusyTimeout:
    return "busy-timeout";
  case Error::Spi:
    return "spi";
  }
  return "unknown";
}

std::expected<void, Error> Panel::wait_idle(int timeout_ms) {
  // BUSY is low while the controller works. A full refresh takes a few
  // seconds, a 10 ms poll costs nothing next to that.
  const std::int64_t end =
      esp_timer_get_time() + std::int64_t{timeout_ms} * 1000;
  while (gpio_get_level(pin(pins_.busy)) == 0) {
    if (esp_timer_get_time() >= end) {
      if (gpio_get_level(pin(pins_.busy)) != 0)
        return {};
      ESP_LOGE(kTag, "busy timeout after %d ms", timeout_ms);
      return std::unexpected(Error::BusyTimeout);
    }
    delay_ms(10);
  }
  return {};
}

std::expected<void, Error> Panel::send(bool data,
                                       std::span<const std::uint8_t> bytes) {
  gpio_set_level(pin(pins_.dc), data ? 1 : 0);
  while (!bytes.empty()) {
    const std::size_t n = bytes.size() < kChunk ? bytes.size() : kChunk;
    spi_transaction_t t{};
    t.length = n * 8;
    t.tx_buffer = bytes.data();
    if (spi_device_polling_transmit(handle(dev_), &t) != ESP_OK)
      return std::unexpected(Error::Spi);
    bytes = bytes.subspan(n);
  }
  return {};
}

std::expected<void, Error> Panel::cmd(std::uint8_t c,
                                      std::span<const std::uint8_t> data) {
  // A DMA transfer must not read from flash, so the command byte sits on the
  // stack and short data is copied there too.
  std::array<std::uint8_t, 8> buf{};
  buf[0] = c;
  if (auto r = send(false, std::span{buf}.first(1)); !r)
    return r;
  if (data.empty())
    return {};
  if (data.size() > buf.size())
    return send(true, data);
  std::memcpy(buf.data(), data.data(), data.size());
  return send(true, std::span{buf}.first(data.size()));
}

std::expected<void, Error> Panel::luts(std::span<const std::uint8_t, 35> lut) {
  std::array<std::uint8_t, 42> reg{};
  for (std::size_t i = 0; i < 5; ++i) {
    std::memcpy(reg.data(), lut.subspan(i * 7, 7).data(), 7);
    if (auto r = cmd(static_cast<std::uint8_t>(0x20 + i)); !r)
      return r;
    if (auto r = send(true, reg); !r)
      return r;
  }
  return {};
}

void Panel::hw_reset() {
  gpio_set_level(pin(pins_.rst), 0);
  delay_ms(20);
  gpio_set_level(pin(pins_.rst), 1);
  delay_ms(20);
}

// Reset and register init, out of Deep. Leaves the booster off.
std::expected<void, Error> Panel::init() {
  struct Reg {
    std::uint8_t cmd = 0;
    std::uint8_t len = 0;
    std::array<std::uint8_t, 5> data{};
  };
  static constexpr std::array<Reg, 9> kInit{{
      {.cmd = 0x00, .len = 2, .data = {0x3F, 0xCA}}, // LUT from registers.
      {.cmd = 0x01, .len = 5, .data = {0x03, 0x10, 0x3F, 0x3F, 0x03}}, // Power.
      {.cmd = 0x06,
       .len = 3,
       .data = {0xE7, 0xE7, 0x3D}}, // Booster soft start.
      {.cmd = 0x60, .len = 1, .data = {0x22}},
      {.cmd = 0x82, .len = 1, .data = {0x00}},
      {.cmd = 0x30, .len = 1, .data = {0x09}}, // 50 Hz frame rate.
      {.cmd = 0xE3, .len = 1, .data = {0x88}},
      {.cmd = 0x61, .len = 4, .data = {0x01, 0x90, 0x01, 0x2C}}, // 400 x 300.
      {.cmd = 0x50, .len = 1, .data = {0xB7}}, // White border.
  }};
  hw_reset();
  if (auto r = wait_idle(1000); !r)
    return r;
  for (const Reg &reg : kInit)
    if (auto r = cmd(reg.cmd, std::span{reg.data}.first(reg.len)); !r)
      return r;
  return luts(kLutFull);
}

std::expected<void, Error> Panel::wake() {
  const bool from_deep = state_ == State::Deep;
  if (from_deep) {
    if (auto r = init(); !r)
      return r;
    state_ = State::Standby;
  }
  ESP_LOGI(kTag, "refresh from %s", from_deep ? "deep sleep" : "standby");
  state_ = State::On;
  if (auto r = cmd(0x04); !r) // Power on.
    return r;
  return wait_idle(1000);
}

std::expected<void, Error> Panel::refresh(Mode mode) {
  if (auto r = luts(mode == Mode::Partial ? kLutPart : kLutFull); !r)
    return r;
  // Plane 0x10 is the old frame, 0x13 the new. A set bit is black here, the
  // same as the canvas, so nothing is inverted. Reset clears panel RAM and
  // the datasheet does not promise it across a booster power cycle, so the
  // base frame is resent every time.
  if (auto r = cmd(0x10); !r)
    return r;
  if (auto r = send(true, {mode == Mode::Partial ? prev_ : tx_, kBytes}); !r)
    return r;
  if (auto r = cmd(0x13); !r)
    return r;
  if (auto r = send(true, {tx_, kBytes}); !r)
    return r;
  if (auto r = cmd(0x12); !r)
    return r;
  delay_ms(10);
  return wait_idle(10000);
}

// Deep sleep is only entered from power off, so a booster that is still on
// goes off first. A failed power off still sends 0x07, the next show()
// resets either way. The reset loses panel RAM, so the next show() is full.
void Panel::enter_deep() {
  if (state_ == State::On && cmd(0x02))
    static_cast<void>(wait_idle(1000));
  static constexpr std::array<std::uint8_t, 1> kCheck{0xA5};
  static_cast<void>(cmd(0x07, kCheck));
  state_ = State::Deep;
  have_prev_ = false;
}

std::expected<void, Error> Panel::begin(const Pins &p) {
  if (started_)
    return std::unexpected(Error::AlreadyStarted);
  started_ = true;
  pins_ = p;
  // Latch the levels first so no pin glitches low when it becomes an output.
  gpio_set_level(pin(p.power), 1);
  gpio_set_level(pin(p.rst), 1);
  gpio_set_level(pin(p.dc), 1);
  const gpio_config_t out{
      .pin_bit_mask = (1ULL << p.power) | (1ULL << p.dc) | (1ULL << p.rst),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  const gpio_config_t in{
      .pin_bit_mask = 1ULL << p.busy,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  if (gpio_config(&out) != ESP_OK || gpio_config(&in) != ESP_OK)
    return std::unexpected(Error::Gpio);
  delay_ms(50);

  // Every field is named, in ESP-IDF v6.1 declaration order, so
  // -Wmissing-field-initializers stays quiet and a field the SDK adds shows.
  const spi_bus_config_t bus{
      .mosi_io_num = p.mosi,
      .miso_io_num = -1,
      .sclk_io_num = p.sck,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .data4_io_num = -1,
      .data5_io_num = -1,
      .data6_io_num = -1,
      .data7_io_num = -1,
      .data_io_default_level = false,
      .max_transfer_sz = 4096,
      .dma_burst_size = 0,
      .flags = 0,
      .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
      .intr_flags = 0,
  };
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK)
    return std::unexpected(Error::SpiBus);
  // Every later failure gives back the frames, the device and the bus, so
  // a board that cannot use the panel keeps its DMA memory and SPI2.
  const auto fail = [this](Error e) -> std::expected<void, Error> {
    heap_caps_free(tx_);
    heap_caps_free(prev_);
    tx_ = nullptr;
    prev_ = nullptr;
    if (dev_ != nullptr)
      static_cast<void>(spi_bus_remove_device(handle(dev_)));
    dev_ = nullptr;
    static_cast<void>(spi_bus_free(SPI2_HOST));
    return std::unexpected(e);
  };
  const spi_device_interface_config_t cfg{
      .command_bits = 0,
      .address_bits = 0,
      .dummy_bits = 0,
      .mode = 0,
      .clock_source = SPI_CLK_SRC_DEFAULT,
      .duty_cycle_pos = 0,
      .cs_ena_pretrans = 0,
      .cs_ena_posttrans = 0,
      .clock_speed_hz = 10 * 1000 * 1000,
      .input_delay_ns = 0,
      .sample_point = SPI_SAMPLING_POINT_PHASE_0,
      .spics_io_num = p.cs,
      .flags = 0,
      .queue_size = 1,
      .pre_cb = nullptr,
      .post_cb = nullptr,
  };
  spi_device_handle_t dev = nullptr;
  if (spi_bus_add_device(SPI2_HOST, &cfg, &dev) != ESP_OK)
    return fail(Error::SpiDevice);
  dev_ = dev;
  tx_ = static_cast<std::uint8_t *>(
      heap_caps_malloc(kBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  prev_ = static_cast<std::uint8_t *>(
      heap_caps_malloc(kBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  if (tx_ == nullptr || prev_ == nullptr)
    return fail(Error::NoMemory);

  // A reset here is the intended wake, the panel may be in deep sleep from
  // before a software restart. The UC8276C idles BUSY high, the SSD1683 of
  // board v1.0 low.
  hw_reset();
  delay_ms(80);
  if (gpio_get_level(pin(p.busy)) == 0) {
    ESP_LOGE(kTag, "controller SSD1683 (board v1.0) is not supported");
    return fail(Error::UnsupportedController);
  }
  ESP_LOGI(kTag, "controller UC8276C (board v1.2)");
  state_ = State::Deep;
  ready_ = true;
  return {};
}

std::expected<void, Error> Panel::show(Frame frame, Mode mode) {
  if (!ready_)
    return std::unexpected(Error::NotReady);
  auto result = wake();
  if (!result) {
    // A failed reset is already Deep, a failed power on is parked there.
    if (state_ != State::Deep)
      enter_deep();
    have_prev_ = false;
    return result;
  }
  std::memcpy(tx_, frame.data(), kBytes);
  if (!have_prev_)
    mode = Mode::Full;
  const std::int64_t t0 = esp_timer_get_time();
  result = refresh(mode);
  const std::int64_t t1 = esp_timer_get_time();
  if (result) {
    // 0x02 goes out either way. If it times out, enter_deep() below sends
    // only 0x07 instead of repeating it and waiting another second.
    state_ = State::Standby;
    result = cmd(0x02);
    if (result)
      result = wait_idle(1000);
  }
  if (!result) {
    // The controller state is unknown after an error, park it in deep sleep
    // so the next show() starts over with a reset.
    enter_deep();
  }
  std::memcpy(prev_, tx_, kBytes);
  have_prev_ = result.has_value();
  ESP_LOGI(kTag, "%s refresh %lld ms%s",
           mode == Mode::Partial ? "partial" : "full",
           static_cast<long long>((t1 - t0) / 1000), result ? "" : " failed");
  return result;
}

void Panel::sleep() {
  if (!ready_ || state_ == State::Deep)
    return;
  enter_deep();
}

} // namespace opensensor::epd
