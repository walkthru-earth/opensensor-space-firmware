#pragma once

#include "sync_codec.h"
#include "utc_clock.h"

namespace opensensor {
namespace sync {
struct TimeRequest {
  std::int64_t seconds = 0;
  std::uint32_t subsecond_us = 0;
  bool precision = false;
};
enum class TimeRequestResult { Ok, Malformed, InvalidEpoch };

// SET_TIME body excludes its opcode; legacy clients send only seconds.
inline TimeRequestResult decode_time_request(const std::uint8_t *body,
                                             std::size_t length,
                                             TimeRequest &request) {
  if (!body || (length != 8 && length != 12))
    return TimeRequestResult::Malformed;
  request.seconds = get_i64(body);
  request.precision = length == 12;
  request.subsecond_us = request.precision ? get_u32(body + 8) : 0;
  return utc::accepted_anchor(request.seconds, request.subsecond_us)
             ? TimeRequestResult::Ok
             : TimeRequestResult::InvalidEpoch;
}

// TIME_SET body excludes its frame byte. Preserve the exact legacy prefix.
inline std::size_t encode_time_ack(std::uint8_t *body,
                                   const TimeRequest &request,
                                   std::int64_t monotonic_us) {
  put_i64(body, request.seconds);
  put_i64(body + 8, monotonic_us);
  if (request.precision)
    put_u32(body + 16, request.subsecond_us);
  return request.precision ? 20 : 16;
}
} // namespace sync
} // namespace opensensor
