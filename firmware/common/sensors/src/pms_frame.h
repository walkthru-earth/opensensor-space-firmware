#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace plantower {

enum class Model : std::uint8_t { Pmsa003, Pms5003t };

struct Frame {
  std::uint16_t cf1_pm1 = 0;
  std::uint16_t cf1_pm25 = 0;
  std::uint16_t cf1_pm10 = 0;
  std::uint16_t atmospheric_pm1 = 0;
  std::uint16_t atmospheric_pm25 = 0;
  std::uint16_t atmospheric_pm10 = 0;
  std::array<std::uint16_t, 6> particle_counts{};
  std::uint8_t particle_count_fields = 0;
  std::int16_t temperature_deci_c = 0;
  std::uint16_t humidity_deci_percent = 0;
  bool has_temperature_humidity = false;
  std::uint8_t firmware_version = 0;
  std::uint8_t sensor_error = 0;
};

// Streaming 32-byte Plantower frame parser; accepts split/concatenated frames.
// PMSA003 and PMS5003T both advertise a 28-byte payload, but their final two
// data words differ. Model must be selected by the board trial, never guessed.
class Parser {
public:
  explicit Parser(Model model) : model_(model) {}
  bool push(std::uint8_t byte, Frame &frame);
  std::uint32_t checksum_failures() const { return checksum_failures_; }
  std::uint32_t length_failures() const { return length_failures_; }

private:
  std::uint16_t word_at(std::size_t offset) const;
  void resynchronize();
  void shift_to_next_header();
  Model model_;
  std::array<std::uint8_t, 32> buffer_{};
  std::size_t used_ = 0;
  std::uint32_t checksum_failures_ = 0;
  std::uint32_t length_failures_ = 0;
};

} // namespace plantower
