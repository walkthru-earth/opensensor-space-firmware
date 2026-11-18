#pragma once

// CoreS3 power columns from raw AXP2101 and AW9523 register bytes (#166).
// Pure. Host tested by tools/telemetry_contract_fixture.cpp.
//
// M5Unified folds a failed I2C read to 0 (readRegister8 is value_or(0) and
// readRegister14 ignores the read status), so its power getters cannot tell a
// bus fault from "no USB, empty battery". The board reads each register
// through the status returning readRegister and passes the bytes here. A
// failed read leaves its column null. Decoding follows the pinned M5Unified
// AXP2101 and CoreS3 paths. A good read gives the same value as before, except
// battery_mv, which is now the exact ADC count. The M5Unified getter went
// through float volts and truncated, so it could read 1 mV lower.

#include <cstdint>
#include <optional>

namespace telemetry::power {

// AXP2101 registers, at I2C address 0x34.
inline constexpr std::uint8_t kAxpStatus = 0x00;  // bit 5 VBUS good
inline constexpr std::uint8_t kAxpBattery = 0x34; // 14 bit, mV
inline constexpr std::uint8_t kAxpVbus = 0x38;    // 14 bit, mV
inline constexpr std::uint8_t kAxpLevel = 0xA4;   // fuel gauge, percent
inline constexpr std::uint8_t kVbusGood = 0x20;
// AW9523 output port 0, at I2C address 0x58, as M5Unified reads it.
inline constexpr std::uint8_t kAw9523Address = 0x58;
inline constexpr std::uint8_t kAwPort0 = 0x02;
inline constexpr std::uint8_t kExternal5vBit = 0x02; // BUS EN
inline constexpr std::uint8_t kUsbOutputBit = 0x20;  // USB OTG EN
// A 14 bit ADC channel with nothing on it reads near full scale. M5Unified
// reports such a VBUS reading as 0 mV, and so does this decoder.
inline constexpr std::uint16_t kAdcFullScale = 0x3FFF;
inline constexpr std::uint16_t kAdcInvalidMin = kAdcFullScale - 32;
// battery_mv plausible range from telemetry_semantics.inc. A reading outside
// it is not a single Li-ion cell (for example no battery fitted), so the
// column stays null. The fixture checks these against the .inc.
inline constexpr std::int32_t kBatteryMinMv = 2500;
inline constexpr std::int32_t kBatteryMaxMv = 4500;

struct Byte {
  bool ok = false;
  std::uint8_t value = 0;
};

struct Word14 {
  bool ok = false;
  std::uint8_t high = 0;
  std::uint8_t low = 0;
};

struct Registers {
  Byte status;
  Word14 vbus;
  Word14 battery;
  Byte level;
  Byte port0;
};

struct Reading {
  std::optional<std::int32_t> vbus_mv;
  std::optional<std::int32_t> battery_mv;
  std::optional<std::int32_t> battery_percent;
  std::optional<bool> external_5v;
  std::optional<bool> usb_output;
};

[[nodiscard]] constexpr std::int32_t raw14(const Word14 &word) {
  return static_cast<std::int32_t>((word.high & 0x3F) << 8 | word.low);
}

[[nodiscard]] constexpr std::optional<std::int32_t>
vbus_mv_of(const Byte &status, const Word14 &vbus) {
  if (!status.ok)
    return std::nullopt;
  if ((status.value & kVbusGood) == 0)
    return 0; // the PMIC reports no USB input
  if (!vbus.ok)
    return std::nullopt;
  const auto raw = raw14(vbus);
  return raw >= kAdcInvalidMin ? 0 : raw;
}

[[nodiscard]] constexpr std::optional<std::int32_t>
battery_mv_of(const Word14 &battery) {
  if (!battery.ok)
    return std::nullopt;
  const auto raw = raw14(battery);
  if (raw < kBatteryMinMv || raw > kBatteryMaxMv)
    return std::nullopt;
  return raw;
}

[[nodiscard]] constexpr std::optional<std::int32_t>
battery_percent_of(const Byte &level) {
  if (!level.ok)
    return std::nullopt;
  const auto percent = static_cast<std::int32_t>(level.value);
  if (percent > 100)
    return std::nullopt;
  return percent;
}

[[nodiscard]] constexpr std::optional<bool> port_bit(const Byte &port,
                                                     std::uint8_t bit) {
  if (!port.ok)
    return std::nullopt;
  return (port.value & bit) != 0;
}

[[nodiscard]] constexpr Reading decode(const Registers &registers) {
  return {
      .vbus_mv = vbus_mv_of(registers.status, registers.vbus),
      .battery_mv = battery_mv_of(registers.battery),
      .battery_percent = battery_percent_of(registers.level),
      .external_5v = port_bit(registers.port0, kExternal5vBit),
      .usb_output = port_bit(registers.port0, kUsbOutputBit),
  };
}

} // namespace telemetry::power
