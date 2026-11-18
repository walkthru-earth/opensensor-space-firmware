#pragma once

// The Plantower row status rule that every PMS board shares (#169, #165).
// Pure. Host tested by tools/test_waveshare_contract.cpp and the CoreS3
// tools/telemetry_contract_fixture.cpp. The board passes its own warm-up and
// staleness numbers, so policy stays in the board. The codes equal the
// pms_status classes of the pmsa003 and pms5003t modules. A board that can
// tell a model mismatch adds it after Valid.

#include <cstdint>
#include <limits>

namespace plantower {

enum class Reading : std::int32_t {
  Missing = 0,
  Warming = 1,
  Stale = 2,
  SensorError = 3,
  Valid = 4,
};

// Milliseconds from the frame to `now_us`, saturated to the uint32
// pms_age_ms column. A frame that has not arrived for 2^32 ms stays at the
// maximum and never wraps back to fresh. A frame stamped after `now_us` is
// 0 ms old.
[[nodiscard]] constexpr std::uint32_t frame_age_ms(std::int64_t now_us,
                                                   std::int64_t received_us) {
  if (now_us <= received_us)
    return 0;
  const auto age = (static_cast<std::uint64_t>(now_us) -
                    static_cast<std::uint64_t>(received_us)) /
                   1000U;
  constexpr auto most = std::numeric_limits<std::uint32_t>::max();
  return age > most ? most : static_cast<std::uint32_t>(age);
}

// True inside the warm-up window after the last sensor wake. A clock reading
// earlier than the wake counts as warming, so a row never reads valid before
// the window.
[[nodiscard]] constexpr bool warming(std::int64_t now_us, std::int64_t wake_us,
                                     std::int64_t warmup_us) {
  return now_us < wake_us || now_us - wake_us < warmup_us;
}

struct Sighting {
  bool present = false; // a checksum-valid frame arrived since boot
  bool warming = false;
  std::uint32_t age_ms = 0; // frame_age_ms at the row time
  std::uint8_t sensor_error = 0;
};

// Missing, then warming, then stale, then the sensor error byte.
[[nodiscard]] constexpr Reading classify(const Sighting &sighting,
                                         std::uint32_t stale_after_ms) {
  return !sighting.present                  ? Reading::Missing
         : sighting.warming                 ? Reading::Warming
         : sighting.age_ms > stale_after_ms ? Reading::Stale
         : sighting.sensor_error != 0       ? Reading::SensorError
                                            : Reading::Valid;
}

} // namespace plantower
