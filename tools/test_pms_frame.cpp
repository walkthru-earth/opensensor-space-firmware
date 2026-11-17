#include "../firmware/common/sensors/src/pms_frame.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

namespace {
std::array<std::uint8_t, 32> frame(std::uint16_t word11, std::uint16_t word12) {
  std::array<std::uint8_t, 32> bytes{};
  bytes[0] = 0x42;
  bytes[1] = 0x4d;
  bytes[3] = 28;
  for (int i = 0; i < 10; ++i)
    bytes[4 + 2 * i + 1] = static_cast<std::uint8_t>(i + 1);
  bytes[24] = static_cast<std::uint8_t>(word11 >> 8);
  bytes[25] = static_cast<std::uint8_t>(word11);
  bytes[26] = static_cast<std::uint8_t>(word12 >> 8);
  bytes[27] = static_cast<std::uint8_t>(word12);
  bytes[28] = 7;
  std::uint16_t sum = 0;
  for (int i = 0; i < 30; ++i)
    sum += bytes[i];
  bytes[30] = static_cast<std::uint8_t>(sum >> 8);
  bytes[31] = static_cast<std::uint8_t>(sum);
  return bytes;
}
} // namespace

int main() {
  const auto bytes = frame(251, 634);
  plantower::Parser temp_parser{plantower::Model::Pms5003t};
  plantower::Frame result{};
  for (std::size_t i = 0; i < 31; ++i)
    assert(!temp_parser.push(bytes[i], result));
  assert(temp_parser.push(bytes[31], result));
  assert(result.particle_count_fields == 4);
  assert(result.particle_counts[3] == 10);
  assert(result.particle_counts[4] == 0 && result.particle_counts[5] == 0);
  assert(result.has_temperature_humidity);
  assert(result.temperature_deci_c == 251);
  assert(result.humidity_deci_percent == 634);
  assert(result.atmospheric_pm25 == 5);
  plantower::Parser particle_parser{plantower::Model::Pmsa003};
  for (std::uint8_t byte : bytes)
    particle_parser.push(byte, result);
  assert(result.particle_count_fields == 6);
  assert(result.particle_counts[4] == 251);
  assert(result.particle_counts[5] == 634);
  assert(!result.has_temperature_humidity);
  auto bad = bytes;
  bad[12] ^= 1;
  for (std::uint8_t byte : bad)
    assert(!temp_parser.push(byte, result));
  assert(temp_parser.checksum_failures() == 1);
  for (std::uint8_t byte : bytes)
    temp_parser.push(byte, result);
  assert(result.has_temperature_humidity && result.temperature_deci_c == 251);

  // A cut frame, then a header with a bad length, then a good frame (#178).
  // The checksum failure moves the bad header to the front with its length
  // word already there. It counts as a length failure, not a second checksum
  // failure, and the good frame still parses on its last byte.
  plantower::Parser resync{plantower::Model::Pms5003t};
  std::vector<std::uint8_t> stream(bytes.begin(), bytes.begin() + 14);
  for (std::uint8_t byte : {0x42, 0x4d, 0x00, 0x04})
    stream.push_back(byte);
  stream.insert(stream.end(), bytes.begin(), bytes.end());
  std::size_t parsed = 0, at = 0;
  for (std::uint8_t byte : stream) {
    ++at;
    if (resync.push(byte, result))
      parsed = at;
  }
  assert(parsed == stream.size());
  assert(resync.checksum_failures() == 1 && resync.length_failures() == 1);
  assert(result.temperature_deci_c == 251 && result.atmospheric_pm25 == 5);
}
