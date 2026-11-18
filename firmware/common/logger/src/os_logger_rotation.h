#pragma once

#include <cstddef>
#include <cstdint>

namespace oslogger {
// Dated rows use UTC seconds / rotation. Unsynced rows have no UTC, so they
// rotate on monotonic seconds / rotation within one boot and clock epoch.
constexpr std::int64_t rotation_window(std::int64_t seconds,
                                       std::int64_t rotation_seconds) {
  return rotation_seconds > 0 ? seconds / rotation_seconds : 0;
}

constexpr bool same_rotation_window(std::int32_t epoch, bool dated,
                                    std::int64_t window,
                                    std::int32_t reference_epoch,
                                    bool reference_dated,
                                    std::int64_t reference_window) {
  return epoch == reference_epoch && dated == reference_dated &&
         window == reference_window;
}

// Row count is a RAM/row-group bound, not a UTC file boundary. An offset
// refresh near a boundary can leave one extra sample in the same window.
constexpr bool finalize_after_group(bool explicit_finalize,
                                    std::size_t row_groups,
                                    std::size_t maximum_row_groups) {
  return explicit_finalize || row_groups >= maximum_row_groups;
}

// Whether a row that did not fit the RAM batch may join it. A batch from
// another window leaves RAM only through a write, tried only while the card
// can take one. A commit can report failure after its rows left RAM, when the
// finalize or the free space read failed, so the answer comes from the batch
// and file left afterwards, never from the commit result alone (#178).
template <typename Commit, typename Fits>
constexpr bool admit_unfitted_row(bool writable, Commit &&commit, Fits &&fits) {
  if (writable)
    static_cast<void>(commit());
  return fits();
}
} // namespace oslogger
