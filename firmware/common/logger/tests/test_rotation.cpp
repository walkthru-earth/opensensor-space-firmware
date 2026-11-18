#include <os_logger_rotation.h>
#include <utc_clock.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

int main() {
  using namespace oslogger;
  assert(same_rotation_window(2, true, 1, 2, true, 1));
  assert(!same_rotation_window(3, true, 1, 2, true, 1));
  assert(!same_rotation_window(2, true, 2, 2, true, 1));
  assert(!same_rotation_window(1, true, 0, 0, false, 0));
  // Unsynced rows now rotate by monotonic window too.
  assert(same_rotation_window(0, false, 7, 0, false, 7));
  assert(!same_rotation_window(0, false, 8, 0, false, 7));
  assert(rotation_window(899, 900) == 0 && rotation_window(900, 900) == 1);
  assert(rotation_window(12345, 0) == 0);
  assert(finalize_after_group(true, 1, 8));
  assert(finalize_after_group(false, 8, 8));
  for (std::size_t groups = 0; groups < 8; ++groups)
    assert(!finalize_after_group(false, groups, 8));

  // A row from a new window meets a batch from the old one (#178). The model
  // keeps what fits_batch reads: the RAM batch and whether a file of the old
  // window is still open. Each step reports what the commit did.
  enum class Outcome {
    NotRun,
    Written,
    AppendFailed,
    FinalizeFailed,
    FreeFailed
  };
  auto intake = [](bool writable, Outcome outcome) {
    std::size_t count = 5;
    bool old_file_open = true, called = false;
    const bool kept = admit_unfitted_row(
        writable,
        [&] {
          called = true;
          if (outcome == Outcome::AppendFailed)
            return false;
          // The group was appended and the file closed, finished or not.
          count = 0;
          old_file_open = false;
          return outcome == Outcome::Written;
        },
        [&] { return !old_file_open && count == 0; });
    assert(called == writable);
    return kept;
  };
  assert(!intake(false, Outcome::NotRun));
  assert(intake(true, Outcome::Written));
  assert(!intake(true, Outcome::AppendFailed));
  assert(intake(true, Outcome::FinalizeFailed));
  assert(intake(true, Outcome::FreeFailed));

  // Repeated small refreshes of an already external clock retain one window
  // even when a two-second backward correction lets a 91st/361st row fit.
  for (const std::int64_t interval : {900, 3600}) {
    std::int32_t epoch = 1;
    std::size_t groups = 0, buffered = 0, finalized = 0;
    constexpr auto group_rows = 90U;
    for (std::int64_t mono_seconds = 0; mono_seconds <= interval;
         mono_seconds += 10) {
      const auto correction = mono_seconds >= interval / 2 ? -2 : 0;
      if (mono_seconds == interval / 2 || mono_seconds % 60 == 0)
        epoch += opensensor::utc::starts_new_epoch(opensensor::utc::Host,
                                                   opensensor::utc::Host,
                                                   correction * 1000000000LL);
      const auto window = (mono_seconds + correction) / interval;
      assert(same_rotation_window(epoch, true, window, 1, true, 0));
      if (++buffered == group_rows) {
        ++groups;
        buffered = 0;
        finalized += finalize_after_group(false, groups, 8);
      }
    }
    assert(finalized == 0 && buffered == 1);
    assert(!same_rotation_window(epoch, true, 1, 1, true, 0));
  }

  // A 24 hour unsynced run at 10 s cadence fills RAM batches and row groups
  // like dated rows. No file is closed after six rows; each closes on its
  // monotonic window or the row-group bound.
  for (const std::int64_t interval : {600, 900, 1800, 3600}) {
    const std::size_t group_rows =
        static_cast<std::size_t>(interval / 10 < 90 ? interval / 10 : 90);
    std::size_t groups = 0, buffered = 0, file_rows = 0, files = 0;
    std::size_t smallest = SIZE_MAX;
    std::int64_t open_window = -1;
    for (std::int64_t mono_seconds = 0; mono_seconds < 86400;
         mono_seconds += 10) {
      const auto window = rotation_window(mono_seconds, interval);
      if (open_window >= 0 &&
          !same_rotation_window(0, false, window, 0, false, open_window)) {
        groups += buffered != 0;
        buffered = 0;
        smallest = file_rows < smallest ? file_rows : smallest;
        ++files;
        file_rows = 0;
        groups = 0;
      }
      open_window = window;
      ++buffered;
      ++file_rows;
      if (buffered == group_rows) {
        buffered = 0;
        if (finalize_after_group(false, ++groups, 8)) {
          smallest = file_rows < smallest ? file_rows : smallest;
          ++files;
          file_rows = 0;
          groups = 0;
          open_window = -1;
        }
      }
    }
    assert(files == static_cast<std::size_t>(86400 / interval) - 1);
    assert(smallest == static_cast<std::size_t>(interval / 10));
    assert(smallest > 6);
  }
}
