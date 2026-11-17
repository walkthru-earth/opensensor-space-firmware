#pragma once

// Sensirion SEN6x environmental sensor node over I2C, header only and bus
// agnostic. Written from the SEN6x datasheet, version 1.0 (October 2026),
// sections 4.3 to 4.8. No Sensirion driver code is copied.
//
// Framing (datasheet 4.6 and 4.7). A command is a 16-bit id sent MSB first
// with no CRC. Every data word is two bytes, MSB first, followed by a CRC-8
// (polynomial 0x31, init 0xFF, no final XOR). The controller writes the
// command, waits the execution time, then reads N words of 3 bytes. The
// sensor does not stretch the clock, it NACKs the address while busy.
//
// Only the SEN63C measured values layout (0x0471) is decoded. SEN66 (0x0300)
// and the other variants use different layouts and get their own decode when
// a board needs one, never a shared one.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>

namespace opensensor {

// One callback call is one whole I2C transaction to a 7-bit address. `write`
// sends `data` after the address header and `read` fills `data`. Both return
// false on any NACK or bus error. `delay_ms` blocks the calling task.
struct Sen6xIo {
  void *context = nullptr;
  bool (*write)(void *context, std::uint8_t address,
                std::span<const std::uint8_t> data) = nullptr;
  bool (*read)(void *context, std::uint8_t address,
               std::span<std::uint8_t> data) = nullptr;
  void (*delay_ms)(void *context, std::uint32_t milliseconds) = nullptr;
};

// Why a SEN6x call failed. Success is the value side of std::expected.
enum class [[nodiscard]] Sen6xError : std::uint8_t {
  IoUnset = 1,        // a callback is missing, no bus traffic at all
  WriteNack = 2,      // the command write failed, sensor busy or absent
  ReadNack = 3,       // the response read failed
  Crc = 4,            // a received word failed its CRC-8
  BadText = 5,        // a string reply holds a byte outside printable ASCII
  RestartTooSoon = 6, // start refused inside the 24 s CO2 conditioning window
  OutOfRange = 7,     // an argument outside the datasheet range, no bus traffic
  // A CO2 calibration write inside the 24 s CO2 conditioning window, no bus
  // traffic
  ConditioningWindow = 8,
};

constexpr const char *sen6x_error_name(Sen6xError error) {
  switch (error) {
  case Sen6xError::IoUnset:
    return "io-unset";
  case Sen6xError::WriteNack:
    return "write-nack";
  case Sen6xError::ReadNack:
    return "read-nack";
  case Sen6xError::Crc:
    return "crc";
  case Sen6xError::BadText:
    return "bad-text";
  case Sen6xError::RestartTooSoon:
    return "restart-too-soon";
  case Sen6xError::OutOfRange:
    return "out-of-range";
  case Sen6xError::ConditioningWindow:
    return "conditioning-window";
  }
  return "unknown";
}

// CRC-8 of one data word, datasheet 4.6. crc({0xBE, 0xEF}) is 0x92.
[[nodiscard]] constexpr std::uint8_t
sen6x_crc8(std::span<const std::uint8_t, 2> word) {
  std::uint8_t crc = 0xFF;
  for (const std::uint8_t byte : word) {
    crc = static_cast<std::uint8_t>(crc ^ byte);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80u) != 0 ? static_cast<std::uint8_t>((crc << 1) ^ 0x31u)
                               : static_cast<std::uint8_t>(crc << 1);
    }
  }
  return crc;
}

// One command id and its execution time from datasheet table 27.
struct Sen6xCommand {
  std::uint16_t id = 0;
  std::uint16_t execution_ms = 0;
};

// SEN63C measured values, 0x0471, datasheet table 32. Each quantity is empty
// when the sensor reports its unknown sentinel (0xFFFF for the uint16 PM
// words, 0x7FFF for the int16 RH, T and CO2 words). CO2 stays 0x7FFF for up
// to 24 s after a start. Raw words are kept for diagnostics.
struct Sen63cValues {
  std::array<std::uint16_t, 7> raw{};
  std::optional<float> pm1_ug_m3;
  std::optional<float> pm25_ug_m3;
  std::optional<float> pm4_ug_m3;
  std::optional<float> pm10_ug_m3;
  std::optional<float> relative_humidity_percent;
  std::optional<float> temperature_c;
  std::optional<std::int16_t> co2_ppm;
};

// Number concentrations, 0x0316, datasheet table 40, in particles per cm3.
// Each value is empty on the 0xFFFF sentinel.
struct Sen6xNumberConcentrations {
  std::array<std::uint16_t, 5> raw{};
  std::optional<float> pm0p5_per_cm3;
  std::optional<float> pm1_per_cm3;
  std::optional<float> pm2p5_per_cm3;
  std::optional<float> pm4_per_cm3;
  std::optional<float> pm10_per_cm3;
};

// Device status register, datasheet 4.3. Error bits are sticky until Read And
// Clear Device Status or a reset. The fan speed warning clears itself.
struct Sen6xStatus {
  std::uint32_t raw = 0;

  static constexpr std::uint32_t kFanSpeedWarning = 1u << 21; // not sticky
  static constexpr std::uint32_t kCo2Error1 = 1u << 12;       // SEN63C, SEN69C
  static constexpr std::uint32_t kPmError = 1u << 11;
  static constexpr std::uint32_t kHchoError = 1u << 10; // SEN68, SEN69C
  static constexpr std::uint32_t kCo2Error2 = 1u << 9;  // SEN66 only
  static constexpr std::uint32_t kGasError = 1u << 7;   // VOC and NOx
  static constexpr std::uint32_t kRhtError = 1u << 6;
  static constexpr std::uint32_t kFanError = 1u << 4;
  // Error bits a SEN63C can raise, datasheet 4.3.2 to 4.3.8.
  static constexpr std::uint32_t kSen63cErrors =
      kCo2Error1 | kPmError | kRhtError | kFanError;

  constexpr bool fan_speed_warning() const {
    return (raw & kFanSpeedWarning) != 0;
  }
  constexpr bool co2_error() const { return (raw & kCo2Error1) != 0; }
  constexpr bool pm_error() const { return (raw & kPmError) != 0; }
  constexpr bool rht_error() const { return (raw & kRhtError) != 0; }
  constexpr bool fan_error() const { return (raw & kFanError) != 0; }
  constexpr bool sen63c_error() const { return (raw & kSen63cErrors) != 0; }
};

// Set Temperature Offset Parameters, 0x60B2, datasheet 4.8.14 table 41. The
// compensated temperature is T + slope * T + offset, in the slot given. The
// five slots add up. Volatile, a reset puts every slot back to zero.
struct Sen6xTemperatureOffset {
  std::int16_t offset_x200 = 0;      // offset in C times 200
  std::int16_t slope_x10000 = 0;     // normalized slope times 10000
  std::uint16_t time_constant_s = 0; // 0 applies within the next second
  std::uint16_t slot = 0;            // 0 to 4
};

// Firmware version, 0xD100, datasheet table 48.
struct Sen6xVersion {
  std::uint8_t major = 0;
  std::uint8_t minor = 0;
};

// A string reply (product name, product type or serial number), up to 32
// printable ASCII characters. `chars` is always NUL terminated.
struct Sen6xText {
  std::array<char, 33> chars{};
  std::size_t size = 0;
  constexpr std::string_view view() const { return {chars.data(), size}; }
  constexpr const char *c_str() const { return chars.data(); }
};

class Sen6x {
public:
  static constexpr std::uint8_t kAddress = 0x6B;
  // Power on until I2C communication may start, datasheet table 1.
  static constexpr std::uint32_t kPowerUpMs = 100;
  // After a start the SEN63C conditions its CO2 sensor for 24 s, datasheet
  // 4.8.1. Inside that window it must not be started again, and after a stop
  // it must not take Set CO2 Sensor Automatic Self Calibration, Perform
  // Forced CO2 Recalibration or Perform CO2 Sensor Factory Reset. This
  // driver sends only the first of those three.
  static constexpr std::uint32_t kCo2ConditioningMs = 24000;
  // First results after a start take about 1.1 s, datasheet 4.8.1.
  static constexpr std::uint32_t kFirstResultMs = 1100;
  // The fan runs at full speed for 10 s after Start Fan Cleaning. Wait that
  // long before starting a measurement, datasheet 4.8.23.
  static constexpr std::uint32_t kFanCleaningMs = 10000;
  // PM start-up time until specifications are met, datasheet table 2.
  static constexpr std::uint32_t kPmStartupS = 30;

  // Datasheet table 27.
  static constexpr Sen6xCommand kStartContinuousMeasurement{0x0021, 50};
  static constexpr Sen6xCommand kStopMeasurement{0x0104, 1400};
  static constexpr Sen6xCommand kGetDataReady{0x0202, 20};
  static constexpr Sen6xCommand kReadMeasuredValuesSen63c{0x0471, 20};
  static constexpr Sen6xCommand kReadNumberConcentrations{0x0316, 20};
  static constexpr Sen6xCommand kGetProductType{0xD002, 20};
  static constexpr Sen6xCommand kGetProductName{0xD014, 20};
  static constexpr Sen6xCommand kGetSerialNumber{0xD033, 20};
  static constexpr Sen6xCommand kReadDeviceStatus{0xD206, 20};
  static constexpr Sen6xCommand kReadAndClearDeviceStatus{0xD210, 20};
  static constexpr Sen6xCommand kGetVersion{0xD100, 20};
  static constexpr Sen6xCommand kDeviceReset{0xD304, 1200};
  static constexpr Sen6xCommand kStartFanCleaning{0x5607, 20};
  static constexpr Sen6xCommand kSetTemperatureOffset{0x60B2, 20};
  static constexpr Sen6xCommand kSetCo2AutoCalibration{0x6711, 20};
  static constexpr Sen6xCommand kSetSensorAltitude{0x6736, 20};

  // Valid Set Sensor Altitude input, datasheet 4.8.39.
  static constexpr std::uint16_t kMaxAltitudeM = 3000;
  static constexpr std::uint16_t kTemperatureOffsetSlots = 5;

  static constexpr std::uint16_t kUnknownU16 = 0xFFFF;
  static constexpr std::uint16_t kUnknownI16 = 0x7FFF;

  explicit constexpr Sen6x(const Sen6xIo &io) : io_(io) {}

  // Milliseconds left in the CO2 conditioning window of the last start
  // attempt that reached the bus, 0 outside it or before any start.
  [[nodiscard]] constexpr std::uint32_t
  conditioning_left_ms(std::uint32_t now_ms) const {
    if (!last_start_ms_.has_value() || restart_allowed(*last_start_ms_, now_ms))
      return 0;
    return kCo2ConditioningMs - (now_ms - *last_start_ms_);
  }

  // Starts continuous measurement. `now_ms` is a monotonic millisecond clock.
  // A start inside kCo2ConditioningMs of the previous start attempt is
  // refused with no bus traffic, because a restart there raises a sticky CO2
  // error. Every attempt that reaches the bus opens a new window, even a
  // failed one, since the sensor may have taken the command.
  [[nodiscard]] std::expected<void, Sen6xError>
  start_continuous_measurement(std::uint32_t now_ms) {
    if (!io_ready())
      return std::unexpected(Sen6xError::IoUnset);
    if (conditioning_left_ms(now_ms) != 0)
      return std::unexpected(Sen6xError::RestartTooSoon);
    last_start_ms_ = now_ms;
    return send(kStartContinuousMeasurement);
  }

  // Stops measurement and waits the 1400 ms a new start needs.
  [[nodiscard]] std::expected<void, Sen6xError> stop_measurement() {
    return send(kStopMeasurement);
  }

  // True when new measured values are ready. False when none are, or when no
  // measurement runs. Reading the values clears the flag.
  [[nodiscard]] std::expected<bool, Sen6xError> data_ready() {
    const auto words = read_words<1>(kGetDataReady);
    if (!words)
      return std::unexpected(words.error());
    return ((*words)[0] & 0x00FFu) != 0;
  }

  [[nodiscard]] std::expected<Sen63cValues, Sen6xError> read_sen63c_values() {
    const auto words = read_words<7>(kReadMeasuredValuesSen63c);
    if (!words)
      return std::unexpected(words.error());
    return decode_sen63c(*words);
  }

  [[nodiscard]] std::expected<Sen6xNumberConcentrations, Sen6xError>
  read_number_concentrations() {
    const auto words = read_words<5>(kReadNumberConcentrations);
    if (!words)
      return std::unexpected(words.error());
    return decode_number_concentrations(*words);
  }

  [[nodiscard]] std::expected<Sen6xStatus, Sen6xError> read_device_status() {
    return read_status(kReadDeviceStatus);
  }

  // Returns the status as it was before clearing every flag.
  [[nodiscard]] std::expected<Sen6xStatus, Sen6xError>
  read_and_clear_device_status() {
    return read_status(kReadAndClearDeviceStatus);
  }

  [[nodiscard]] std::expected<Sen6xText, Sen6xError> product_name() {
    return read_text(kGetProductName);
  }
  // SEN63C answers "0x00085700", datasheet table 43.
  [[nodiscard]] std::expected<Sen6xText, Sen6xError> product_type() {
    return read_text(kGetProductType);
  }
  [[nodiscard]] std::expected<Sen6xText, Sen6xError> serial_number() {
    return read_text(kGetSerialNumber);
  }

  [[nodiscard]] std::expected<Sen6xVersion, Sen6xError> version() {
    const auto words = read_words<1>(kGetVersion);
    if (!words)
      return std::unexpected(words.error());
    return Sen6xVersion{.major = static_cast<std::uint8_t>((*words)[0] >> 8),
                        .minor = static_cast<std::uint8_t>((*words)[0])};
  }

  // Same effect as a power cycle. Idle mode only, waits 1200 ms.
  [[nodiscard]] std::expected<void, Sen6xError> device_reset() {
    return send(kDeviceReset);
  }

  // Idle mode only. The caller waits kFanCleaningMs before the next start.
  [[nodiscard]] std::expected<void, Sen6xError> start_fan_cleaning() {
    return send(kStartFanCleaning);
  }

  // Any mode, datasheet 4.8.14. A slot above 4 is refused with no bus
  // traffic, the sensor would ignore it.
  [[nodiscard]] std::expected<void, Sen6xError>
  set_temperature_offset(const Sen6xTemperatureOffset &offset) {
    if (offset.slot >= kTemperatureOffsetSlots)
      return std::unexpected(Sen6xError::OutOfRange);
    return send_words(kSetTemperatureOffset,
                      std::array<std::uint16_t, 4>{
                          std::bit_cast<std::uint16_t>(offset.offset_x200),
                          std::bit_cast<std::uint16_t>(offset.slope_x10000),
                          offset.time_constant_s, offset.slot});
  }

  // CO2 automatic self calibration on or off. Idle mode only and volatile,
  // the sensor turns it back on after a reset, datasheet 4.8.35. In the field
  // it must stay on. Inside the CO2 conditioning window of the last start it
  // is refused with no bus traffic, datasheet 4.8.1. `now_ms` is the clock
  // start_continuous_measurement() takes.
  [[nodiscard]] std::expected<void, Sen6xError>
  set_co2_auto_calibration(bool enabled, std::uint32_t now_ms) {
    if (!io_ready())
      return std::unexpected(Sen6xError::IoUnset);
    if (conditioning_left_ms(now_ms) != 0)
      return std::unexpected(Sen6xError::ConditioningWindow);
    return send_words(kSetCo2AutoCalibration,
                      std::array<std::uint16_t, 1>{
                          static_cast<std::uint16_t>(enabled ? 1 : 0)});
  }

  // Altitude in metres for the CO2 pressure compensation, 0 to 3000. Idle
  // mode only and volatile, a reset puts back 0, datasheet 4.8.39.
  [[nodiscard]] std::expected<void, Sen6xError>
  set_sensor_altitude(std::uint16_t metres) {
    if (metres > kMaxAltitudeM)
      return std::unexpected(Sen6xError::OutOfRange);
    return send_words(kSetSensorAltitude, std::array<std::uint16_t, 1>{metres});
  }

  // The bytes of a write with data, the command then each word MSB first
  // with its CRC, datasheet 4.7.
  template <std::size_t N>
  [[nodiscard]] static constexpr std::array<std::uint8_t, 2 + N * 3>
  encode_write(std::uint16_t command,
               const std::array<std::uint16_t, N> &words) {
    std::array<std::uint8_t, 2 + N * 3> bytes{};
    bytes[0] = static_cast<std::uint8_t>(command >> 8);
    bytes[1] = static_cast<std::uint8_t>(command & 0xFFu);
    for (std::size_t i = 0; i < N; ++i) {
      const std::array<std::uint8_t, 2> word{
          static_cast<std::uint8_t>(words[i] >> 8),
          static_cast<std::uint8_t>(words[i] & 0xFFu)};
      bytes[2 + i * 3] = word[0];
      bytes[3 + i * 3] = word[1];
      bytes[4 + i * 3] = sen6x_crc8(word);
    }
    return bytes;
  }

  // Unsigned subtraction keeps this right across one clock wrap.
  [[nodiscard]] static constexpr bool restart_allowed(std::uint32_t start_ms,
                                                      std::uint32_t now_ms) {
    return now_ms - start_ms >= kCo2ConditioningMs;
  }

  // One received word, its MSB, LSB and CRC.
  [[nodiscard]] static constexpr std::expected<std::uint16_t, Sen6xError>
  decode_word(std::span<const std::uint8_t, 3> bytes) {
    const std::array<std::uint8_t, 2> data{bytes[0], bytes[1]};
    if (sen6x_crc8(data) != bytes[2])
      return std::unexpected(Sen6xError::Crc);
    return static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
  }

  template <std::size_t N>
  [[nodiscard]] static constexpr std::expected<std::array<std::uint16_t, N>,
                                               Sen6xError>
  decode_words(std::span<const std::uint8_t, N * 3> bytes) {
    std::array<std::uint16_t, N> words{};
    for (std::size_t i = 0; i < N; ++i) {
      const auto word = decode_word(
          std::span<const std::uint8_t, 3>(bytes.subspan(i * 3, 3)));
      if (!word)
        return std::unexpected(word.error());
      words[i] = *word;
    }
    return words;
  }

  [[nodiscard]] static constexpr Sen63cValues
  decode_sen63c(const std::array<std::uint16_t, 7> &words) {
    return Sen63cValues{
        .raw = words,
        .pm1_ug_m3 = pm_mass(words[0]),
        .pm25_ug_m3 = pm_mass(words[1]),
        .pm4_ug_m3 = pm_mass(words[2]),
        .pm10_ug_m3 = pm_mass(words[3]),
        .relative_humidity_percent = scaled_i16(words[4], 100.0f),
        .temperature_c = scaled_i16(words[5], 200.0f),
        .co2_ppm = words[6] == kUnknownI16
                       ? std::nullopt
                       : std::optional<std::int16_t>(
                             std::bit_cast<std::int16_t>(words[6])),
    };
  }

  [[nodiscard]] static constexpr Sen6xNumberConcentrations
  decode_number_concentrations(const std::array<std::uint16_t, 5> &words) {
    return Sen6xNumberConcentrations{
        .raw = words,
        .pm0p5_per_cm3 = pm_mass(words[0]),
        .pm1_per_cm3 = pm_mass(words[1]),
        .pm2p5_per_cm3 = pm_mass(words[2]),
        .pm4_per_cm3 = pm_mass(words[3]),
        .pm10_per_cm3 = pm_mass(words[4]),
    };
  }

  // 16 words carry 32 characters, two per word, MSB first. The string ends
  // at the first NUL or after 32 characters.
  [[nodiscard]] static constexpr std::expected<Sen6xText, Sen6xError>
  decode_text(const std::array<std::uint16_t, 16> &words) {
    Sen6xText text;
    for (const std::uint16_t word : words) {
      for (const auto byte : {static_cast<std::uint8_t>(word >> 8),
                              static_cast<std::uint8_t>(word)}) {
        if (byte == 0)
          return text;
        if (byte < 0x20 || byte > 0x7E)
          return std::unexpected(Sen6xError::BadText);
        text.chars[text.size++] = static_cast<char>(byte);
      }
    }
    return text;
  }

private:
  // Value / 10 for the uint16 PM and number concentration words.
  static constexpr std::optional<float> pm_mass(std::uint16_t word) {
    if (word == kUnknownU16)
      return std::nullopt;
    return static_cast<float>(word) / 10.0f;
  }

  static constexpr std::optional<float> scaled_i16(std::uint16_t word,
                                                   float scale) {
    if (word == kUnknownI16)
      return std::nullopt;
    return static_cast<float>(std::bit_cast<std::int16_t>(word)) / scale;
  }

  constexpr bool io_ready() const {
    return io_.write != nullptr && io_.read != nullptr &&
           io_.delay_ms != nullptr;
  }

  // Writes the command and waits its execution time.
  std::expected<void, Sen6xError> send(Sen6xCommand command) {
    if (!io_ready())
      return std::unexpected(Sen6xError::IoUnset);
    const std::array<std::uint8_t, 2> bytes{
        static_cast<std::uint8_t>(command.id >> 8),
        static_cast<std::uint8_t>(command.id & 0xFFu)};
    if (!io_.write(io_.context, kAddress, bytes))
      return std::unexpected(Sen6xError::WriteNack);
    io_.delay_ms(io_.context, command.execution_ms);
    return {};
  }

  // Writes the command with N data words and waits its execution time.
  template <std::size_t N>
  std::expected<void, Sen6xError>
  send_words(Sen6xCommand command, const std::array<std::uint16_t, N> &words) {
    if (!io_ready())
      return std::unexpected(Sen6xError::IoUnset);
    const auto bytes = encode_write(command.id, words);
    if (!io_.write(io_.context, kAddress, bytes))
      return std::unexpected(Sen6xError::WriteNack);
    io_.delay_ms(io_.context, command.execution_ms);
    return {};
  }

  // Writes the command, waits its execution time, reads N words.
  template <std::size_t N>
  std::expected<std::array<std::uint16_t, N>, Sen6xError>
  read_words(Sen6xCommand command) {
    if (const auto sent = send(command); !sent)
      return std::unexpected(sent.error());
    std::array<std::uint8_t, N * 3> bytes{};
    if (!io_.read(io_.context, kAddress, bytes))
      return std::unexpected(Sen6xError::ReadNack);
    return decode_words<N>(std::span<const std::uint8_t, N * 3>(bytes));
  }

  std::expected<Sen6xStatus, Sen6xError> read_status(Sen6xCommand command) {
    const auto words = read_words<2>(command);
    if (!words)
      return std::unexpected(words.error());
    return Sen6xStatus{.raw = (static_cast<std::uint32_t>((*words)[0]) << 16) |
                              (*words)[1]};
  }

  std::expected<Sen6xText, Sen6xError> read_text(Sen6xCommand command) {
    const auto words = read_words<16>(command);
    if (!words)
      return std::unexpected(words.error());
    return decode_text(*words);
  }

  Sen6xIo io_;
  std::optional<std::uint32_t> last_start_ms_;
};

} // namespace opensensor
