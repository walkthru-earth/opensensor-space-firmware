#include "pms_frame.h"

#include <cstring>
#include <numeric>

namespace plantower {

std::uint16_t Parser::word_at(std::size_t offset) const {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(buffer_[offset]) << 8) | buffer_[offset + 1]);
}

void Parser::resynchronize() {
  // A header moved to the front already holds its length word when four or
  // more bytes follow it, and push() checks that word only as the fourth byte
  // arrives. Check it here and move on again while it is wrong (#178). Each
  // pass shortens the buffer, so the loop ends.
  shift_to_next_header();
  while (used_ >= 4 && word_at(2) != 28) {
    ++length_failures_;
    shift_to_next_header();
  }
}

void Parser::shift_to_next_header() {
  std::size_t next = used_;
  for (std::size_t index = 1; index + 1 < used_; ++index) {
    if (buffer_[index] == 0x42 && buffer_[index + 1] == 0x4d) {
      next = index;
      break;
    }
  }
  if (next == used_ && used_ != 0 && buffer_[used_ - 1] == 0x42) {
    buffer_[0] = 0x42;
    used_ = 1;
    return;
  }
  if (next < used_) {
    const std::size_t remaining = used_ - next;
    std::memmove(buffer_.data(), buffer_.data() + next, remaining);
    used_ = remaining;
    return;
  }
  used_ = 0;
}

bool Parser::push(std::uint8_t byte, Frame &frame) {
  if (used_ == 0 && byte != 0x42)
    return false;
  buffer_[used_++] = byte;
  if (used_ == 2 && buffer_[1] != 0x4d) {
    resynchronize();
    return false;
  }
  if (used_ == 4 && word_at(2) != 28) {
    ++length_failures_;
    resynchronize();
    return false;
  }
  if (used_ < buffer_.size())
    return false;
  const auto sum =
      std::accumulate(buffer_.begin(), buffer_.end() - 2, std::uint16_t{0},
                      [](std::uint16_t accumulated, std::uint8_t value) {
                        return static_cast<std::uint16_t>(accumulated + value);
                      });
  if (sum != word_at(30)) {
    ++checksum_failures_;
    resynchronize();
    return false;
  }
  frame = {};
  frame.cf1_pm1 = word_at(4);
  frame.cf1_pm25 = word_at(6);
  frame.cf1_pm10 = word_at(8);
  frame.atmospheric_pm1 = word_at(10);
  frame.atmospheric_pm25 = word_at(12);
  frame.atmospheric_pm10 = word_at(14);
  frame.particle_count_fields = model_ == Model::Pms5003t ? 4 : 6;
  for (std::size_t index = 0; index < frame.particle_count_fields; ++index)
    frame.particle_counts[index] = word_at(16 + index * 2);
  if (model_ == Model::Pms5003t) {
    frame.temperature_deci_c = static_cast<std::int16_t>(word_at(24));
    frame.humidity_deci_percent = word_at(26);
    frame.has_temperature_humidity = true;
  }
  frame.firmware_version = buffer_[28];
  frame.sensor_error = buffer_[29];
  used_ = 0;
  return true;
}

} // namespace plantower
