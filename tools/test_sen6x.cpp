#include "../firmware/common/sensors/src/os_sen6x.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <span>
#include <vector>

namespace {
using opensensor::Sen63cValues;
using opensensor::Sen6x;
using opensensor::Sen6xError;
using opensensor::Sen6xIo;

// Datasheet 4.6 example, CRC of 0xBEEF is 0x92.
constexpr std::array<std::uint8_t, 2> kBeef{0xBE, 0xEF};
static_assert(opensensor::sen6x_crc8(kBeef) == 0x92);
constexpr std::array<std::uint8_t, 2> kZero{0x00, 0x00};
static_assert(opensensor::sen6x_crc8(kZero) == 0x81);

constexpr std::array<std::uint8_t, 3> kGoodWord{0xBE, 0xEF, 0x92};
static_assert(Sen6x::decode_word(kGoodWord).has_value());
static_assert(*Sen6x::decode_word(kGoodWord) == 0xBEEF);
constexpr std::array<std::uint8_t, 3> kBadWord{0xBE, 0xEF, 0x93};
static_assert(!Sen6x::decode_word(kBadWord).has_value());
static_assert(Sen6x::decode_word(kBadWord).error() == Sen6xError::Crc);

// Decode runs at compile time. 123 is 12.3 ug/m3, 4500 is 45 %RH, 5000 is
// 25 C, 0x7FFF is the unknown CO2 of the first 24 s.
constexpr Sen63cValues kWarm =
    Sen6x::decode_sen63c({123, 0xFFFF, 0, 6000, 4500, 5000, 0x7FFF});
static_assert(kWarm.pm1_ug_m3.has_value() && *kWarm.pm1_ug_m3 == 12.3f);
static_assert(!kWarm.pm25_ug_m3.has_value());
static_assert(kWarm.pm4_ug_m3.has_value() && *kWarm.pm4_ug_m3 == 0.0f);
static_assert(*kWarm.pm10_ug_m3 == 600.0f);
static_assert(*kWarm.relative_humidity_percent == 45.0f);
static_assert(*kWarm.temperature_c == 25.0f);
static_assert(!kWarm.co2_ppm.has_value());

// Negative temperature is two's complement, -10 C is -2000.
constexpr Sen63cValues kCold = Sen6x::decode_sen63c(
    {0, 0, 0, 0, 0x7FFF, static_cast<std::uint16_t>(-2000), 415});
static_assert(*kCold.temperature_c == -10.0f);
static_assert(!kCold.relative_humidity_percent.has_value());
static_assert(*kCold.co2_ppm == 415);

static_assert(Sen6x::restart_allowed(0, 24000));
static_assert(!Sen6x::restart_allowed(0, 23999));
static_assert(Sen6x::restart_allowed(0xFFFFF000u, 24000 - 0x1000));
static_assert(!Sen6x::restart_allowed(0xFFFFF000u, 100));

void put_word(std::vector<std::uint8_t> &out, std::uint16_t word) {
  const std::array<std::uint8_t, 2> data{static_cast<std::uint8_t>(word >> 8),
                                         static_cast<std::uint8_t>(word)};
  out.push_back(data[0]);
  out.push_back(data[1]);
  out.push_back(opensensor::sen6x_crc8(data));
}

struct FakeBus {
  std::vector<std::uint16_t> commands;           // every command id written
  std::vector<std::vector<std::uint8_t>> writes; // every write, whole
  std::vector<std::uint32_t> delays;             // every delay asked for
  std::deque<std::vector<std::uint8_t>> replies;
  bool nack_write = false;
  bool nack_read = false;
  int corrupt_byte = -1; // flips one byte of the next reply

  Sen6xIo io() { return {this, write, read, delay}; }

  void reply(std::initializer_list<std::uint16_t> words) {
    std::vector<std::uint8_t> bytes;
    for (const auto word : words)
      put_word(bytes, word);
    replies.push_back(bytes);
  }

  static bool write(void *context, std::uint8_t address,
                    std::span<const std::uint8_t> data) {
    auto &bus = *static_cast<FakeBus *>(context);
    assert(address == Sen6x::kAddress);
    assert(data.size() >= 2 && (data.size() - 2) % 3 == 0);
    bus.writes.emplace_back(data.begin(), data.end());
    bus.commands.push_back(
        static_cast<std::uint16_t>((data[0] << 8) | data[1]));
    return !bus.nack_write;
  }
  static bool read(void *context, std::uint8_t address,
                   std::span<std::uint8_t> data) {
    auto &bus = *static_cast<FakeBus *>(context);
    assert(address == Sen6x::kAddress);
    if (bus.nack_read)
      return false;
    assert(!bus.replies.empty());
    auto bytes = bus.replies.front();
    bus.replies.pop_front();
    assert(bytes.size() == data.size());
    if (bus.corrupt_byte >= 0) {
      bytes[static_cast<std::size_t>(bus.corrupt_byte)] ^= 0x01;
      bus.corrupt_byte = -1;
    }
    std::memcpy(data.data(), bytes.data(), bytes.size());
    return true;
  }
  static void delay(void *context, std::uint32_t milliseconds) {
    static_cast<FakeBus *>(context)->delays.push_back(milliseconds);
  }
};

void measured_values() {
  FakeBus bus;
  Sen6x sensor(bus.io());
  bus.reply({0x0001});
  const auto ready = sensor.data_ready();
  assert(ready && *ready);
  bus.reply({0x0000});
  const auto not_ready = sensor.data_ready();
  assert(not_ready && !*not_ready);

  bus.reply({52, 85, 101, 112, 5123, 4321, 612});
  const auto values = sensor.read_sen63c_values();
  assert(values);
  assert(*values->pm1_ug_m3 == 5.2f && *values->pm25_ug_m3 == 8.5f);
  assert(*values->pm4_ug_m3 == 10.1f && *values->pm10_ug_m3 == 11.2f);
  assert(*values->relative_humidity_percent == 51.23f);
  assert(*values->temperature_c == 21.605f);
  assert(*values->co2_ppm == 612);
  assert((bus.commands == std::vector<std::uint16_t>{0x0202, 0x0202, 0x0471}));
  assert((bus.delays == std::vector<std::uint32_t>{20, 20, 20}));

  // Every sentinel at once, as before the first result.
  bus.reply({0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x7FFF, 0x7FFF, 0x7FFF});
  const auto unknown = sensor.read_sen63c_values();
  assert(unknown);
  assert(!unknown->pm1_ug_m3 && !unknown->pm25_ug_m3 && !unknown->pm4_ug_m3);
  assert(!unknown->pm10_ug_m3 && !unknown->relative_humidity_percent);
  assert(!unknown->temperature_c && !unknown->co2_ppm);
  assert(unknown->raw[6] == 0x7FFF);

  bus.reply({30, 0xFFFF, 45, 46, 47});
  const auto counts = sensor.read_number_concentrations();
  assert(counts && *counts->pm0p5_per_cm3 == 3.0f);
  assert(!counts->pm1_per_cm3 && *counts->pm10_per_cm3 == 4.7f);
  assert(bus.commands.back() == 0x0316);
}

void crc_and_nack() {
  FakeBus bus;
  Sen6x sensor(bus.io());
  // A CRC mismatch in the last word fails the whole read.
  bus.reply({1, 2, 3, 4, 5, 6, 7});
  bus.corrupt_byte = 20;
  const auto crc = sensor.read_sen63c_values();
  assert(!crc && crc.error() == Sen6xError::Crc);
  // A data byte flip is caught too.
  bus.reply({1, 2});
  bus.corrupt_byte = 3;
  const auto status = sensor.read_device_status();
  assert(!status && status.error() == Sen6xError::Crc);

  // A write NACK reads nothing and waits nothing.
  bus.nack_write = true;
  const auto delays = bus.delays.size();
  const auto busy = sensor.read_sen63c_values();
  assert(!busy && busy.error() == Sen6xError::WriteNack);
  assert(bus.delays.size() == delays);
  assert(!sensor.stop_measurement().has_value());
  bus.nack_write = false;

  bus.nack_read = true;
  const auto silent = sensor.data_ready();
  assert(!silent && silent.error() == Sen6xError::ReadNack);
  bus.nack_read = false;
  assert(bus.replies.empty());
}

void status_bits() {
  FakeBus bus;
  Sen6x sensor(bus.io());
  // Fan speed warning (bit 21), CO2-1 (12) and fan error (4).
  bus.reply({0x0020, 0x1010});
  const auto status = sensor.read_and_clear_device_status();
  assert(status);
  assert(status->raw == 0x00201010u);
  assert(status->fan_speed_warning() && status->co2_error());
  assert(status->fan_error() && !status->pm_error() && !status->rht_error());
  assert(status->sen63c_error());
  assert(bus.commands.back() == 0xD210);

  // PM (11) and RH&T (6) alone.
  bus.reply({0x0000, 0x0840});
  const auto other = sensor.read_device_status();
  assert(other && other->pm_error() && other->rht_error());
  assert(!other->co2_error() && !other->fan_speed_warning());
  assert(bus.commands.back() == 0xD206);

  // A warning or a SEN66-only bit is not a SEN63C error.
  constexpr opensensor::Sen6xStatus kWarnOnly{.raw = (1u << 21) | (1u << 9)};
  static_assert(!kWarnOnly.sen63c_error() && kWarnOnly.fan_speed_warning());
}

void identity() {
  FakeBus bus;
  Sen6x sensor(bus.io());
  // "SEN63C" then NUL padding, 16 words.
  bus.reply({0x5345, 0x4E36, 0x3343, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  const auto name = sensor.product_name();
  assert(name && name->view() == "SEN63C");
  assert(std::strcmp(name->c_str(), "SEN63C") == 0);

  // 32 characters with no NUL still fit, and c_str stays terminated.
  bus.reply({0x4141, 0x4141, 0x4141, 0x4141, 0x4141, 0x4141, 0x4141, 0x4141,
             0x4141, 0x4141, 0x4141, 0x4141, 0x4141, 0x4141, 0x4141, 0x4142});
  const auto serial = sensor.serial_number();
  assert(serial && serial->size == 32 && serial->view().back() == 'B');
  assert(std::strlen(serial->c_str()) == 32);

  // A control byte is refused.
  bus.reply({0x3078, 0x0A00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  const auto type = sensor.product_type();
  assert(!type && type.error() == Sen6xError::BadText);
  assert((bus.commands == std::vector<std::uint16_t>{0xD014, 0xD033, 0xD002}));

  bus.reply({0x0102});
  const auto version = sensor.version();
  assert(version && version->major == 1 && version->minor == 2);
}

void lifecycle() {
  FakeBus bus;
  Sen6x sensor(bus.io());
  assert(sensor.start_continuous_measurement(1000).has_value());
  assert(sensor.stop_measurement().has_value());
  // A restart inside 24 s of the last start sends nothing.
  const auto commands = bus.commands.size();
  const auto early = sensor.start_continuous_measurement(24999);
  assert(!early && early.error() == Sen6xError::RestartTooSoon);
  assert(bus.commands.size() == commands);
  assert(sensor.start_continuous_measurement(25000).has_value());

  // A failed start still opens the window, the sensor may have taken it.
  bus.nack_write = true;
  assert(!sensor.start_continuous_measurement(60000).has_value());
  bus.nack_write = false;
  const auto retry = sensor.start_continuous_measurement(61000);
  assert(!retry && retry.error() == Sen6xError::RestartTooSoon);
  assert(sensor.start_continuous_measurement(84000).has_value());

  assert(sensor.device_reset().has_value());
  assert(sensor.start_fan_cleaning().has_value());
  assert((bus.commands == std::vector<std::uint16_t>{0x0021, 0x0104, 0x0021,
                                                     0x0021, 0x0021, 0xD304,
                                                     0x5607}));
  assert(
      (bus.delays == std::vector<std::uint32_t>{50, 1400, 50, 50, 1200, 20}));
}

void unset_io() {
  FakeBus bus;
  Sen6xIo io = bus.io();
  io.delay_ms = nullptr;
  Sen6x sensor(io);
  const auto start = sensor.start_continuous_measurement(0);
  assert(!start && start.error() == Sen6xError::IoUnset);
  const auto values = sensor.read_sen63c_values();
  assert(!values && values.error() == Sen6xError::IoUnset);
  // No start reached the bus, so the window stays closed.
  assert(bus.commands.empty());
  Sen6x empty(Sen6xIo{});
  assert(empty.data_ready().error() == Sen6xError::IoUnset);
  assert(std::strcmp(opensensor::sen6x_error_name(Sen6xError::Crc), "crc") ==
         0);
}
// Datasheet 4.8.14, 4.8.35 and 4.8.39. Each data word goes MSB first with
// its CRC, and an out of range argument never reaches the bus.
void configuration_writes() {
  FakeBus bus;
  Sen6x sensor(bus.io());
  // -1.5 C is -300 in units of 1/200 C, 0xFED4.
  assert(sensor
             .set_temperature_offset({.offset_x200 = -300,
                                      .slope_x10000 = 0,
                                      .time_constant_s = 0,
                                      .slot = 0})
             .has_value());
  // No start yet, so no conditioning window.
  assert(sensor.set_co2_auto_calibration(false, 0).has_value());
  assert(sensor.set_co2_auto_calibration(true, 0).has_value());
  assert(sensor.set_sensor_altitude(350).has_value());
  const auto crc = [](std::uint16_t word) {
    const std::array<std::uint8_t, 2> data{static_cast<std::uint8_t>(word >> 8),
                                           static_cast<std::uint8_t>(word)};
    return opensensor::sen6x_crc8(data);
  };
  using Bytes = std::vector<std::uint8_t>;
  assert((bus.writes[0] == Bytes{0x60, 0xB2, 0xFE, 0xD4, crc(0xFED4), 0x00,
                                 0x00, crc(0x0000), 0x00, 0x00, crc(0x0000),
                                 0x00, 0x00, crc(0x0000)}));
  assert((bus.writes[1] == Bytes{0x67, 0x11, 0x00, 0x00, crc(0x0000)}));
  assert((bus.writes[2] == Bytes{0x67, 0x11, 0x00, 0x01, crc(0x0001)}));
  assert((bus.writes[3] == Bytes{0x67, 0x36, 0x01, 0x5E, crc(0x015E)}));
  assert((bus.delays == std::vector<std::uint32_t>{20, 20, 20, 20}));
  // The datasheet example word 0xBEEF has CRC 0x92.
  static_assert(Sen6x::encode_write(
                    0x6736, std::array<std::uint16_t, 1>{0xBEEF})[4] == 0x92);

  const auto high = sensor.set_sensor_altitude(3001);
  assert(!high && high.error() == Sen6xError::OutOfRange);
  assert(sensor.set_sensor_altitude(3000).has_value());
  const auto slot = sensor.set_temperature_offset({.slot = 5});
  assert(!slot && slot.error() == Sen6xError::OutOfRange);
  assert(bus.writes.size() == 5);
  assert(std::strcmp(opensensor::sen6x_error_name(Sen6xError::OutOfRange),
                     "out-of-range") == 0);
  bus.nack_write = true;
  const auto nack = sensor.set_co2_auto_calibration(true, 0);
  assert(!nack && nack.error() == Sen6xError::WriteNack);
}

// Datasheet 4.8.1. After a stop inside the 24 s CO2 conditioning window the
// ASC write is refused with no bus traffic. Altitude and the temperature
// offset are not restricted.
void conditioning_window() {
  FakeBus bus;
  Sen6x sensor(bus.io());
  assert(sensor.conditioning_left_ms(5000) == 0);
  assert(sensor.start_continuous_measurement(1000).has_value());
  assert(sensor.stop_measurement().has_value());
  assert(sensor.conditioning_left_ms(1000) == 24000);
  assert(sensor.conditioning_left_ms(24999) == 1);
  const auto commands = bus.commands.size();
  const auto early = sensor.set_co2_auto_calibration(true, 24999);
  assert(!early && early.error() == Sen6xError::ConditioningWindow);
  assert(bus.commands.size() == commands);
  assert(std::strcmp(opensensor::sen6x_error_name(early.error()),
                     "conditioning-window") == 0);
  assert(sensor.set_sensor_altitude(100).has_value());
  assert(sensor.set_temperature_offset({.slot = 0}).has_value());
  assert(sensor.conditioning_left_ms(25000) == 0);
  assert(sensor.set_co2_auto_calibration(true, 25000).has_value());
  assert((bus.commands ==
          std::vector<std::uint16_t>{0x0021, 0x0104, 0x6736, 0x60B2, 0x6711}));

  // A failed start still opens the window, the sensor may have taken it.
  bus.nack_write = true;
  assert(!sensor.start_continuous_measurement(60000).has_value());
  bus.nack_write = false;
  const auto after_nack = sensor.set_co2_auto_calibration(false, 70000);
  assert(!after_nack && after_nack.error() == Sen6xError::ConditioningWindow);
  assert(sensor.set_co2_auto_calibration(false, 84000).has_value());
}
} // namespace

int main() {
  measured_values();
  crc_and_nack();
  status_bits();
  identity();
  lifecycle();
  unset_io();
  configuration_writes();
  conditioning_window();
  return 0;
}
