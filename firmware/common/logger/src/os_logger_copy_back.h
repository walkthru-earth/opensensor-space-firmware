#pragma once

#include "os_logger_flash_copy.h"

#include <cstdint>

// Copy back of the flash fallback root to the card (decision record 0004,
// item 8), in its own translation unit since #215. The storage worker alone
// calls it, one step per idle pass while the card is Ready. The state lives
// in PSRAM, so no task stack holds it.
namespace oslogger::copy_back {

// os_logger_copy_back.cpp.

// Allocates the copy back state in PSRAM once the flash root is up. On a
// failed allocation it logs and copy back stays off for the boot.
void start();
// A copy runs only while the card takes writes and nothing else needs the
// card: no LIST job, no emergency close.
[[nodiscard]] bool allowed(bool storage_ready);
// True while a file is being copied.
[[nodiscard]] bool copying();
// Ends a copy in progress, the copier removes only its own temporary.
void cancel(const char *reason);
// A card that just mounted gets its copy pass now, not after the wait.
void run_now();
// The flash root may hold other names: a flash commit made, renamed or
// quarantined a file. The next step walks the root again.
void flash_changed();
// One copy step: the next file, or one chunk of the current one. True when
// it did work, so the loop yields a tick instead of sleeping.
[[nodiscard]] bool service(bool storage_ready);

// What copy back takes from the storage worker, os_logger.cpp.
namespace worker {
[[nodiscard]] const char *flash_root();
[[nodiscard]] const char *card_root();
// The worker side of allowed(): flash root up and holding files, the card
// Ready, no LIST, no emergency close.
[[nodiscard]] bool may_copy(bool storage_ready);
// The last walk found no file left on flash.
void flash_empty();
void count_error();
// Moves the stored copy count up before a file moves. False, after the one
// error line of the boot, when NVS refused.
[[nodiscard]] bool reserve_copy_count();
// A file reached the card, `copied` when this copy wrote it: the copy count,
// the mount epoch and the card write state follow.
void file_moved(bool copied);
// After the move: flash free space, and card free space for the governor
// when this copy wrote it (#213).
void measure_after_move(bool copied, std::int64_t now);
// A copy that failed on a card that never passed a write test fails it
// (#218).
void copy_failed(opensensor::logger::flash_copy::Failure failure, int error,
                 std::int64_t now);
[[nodiscard]] std::int64_t uptime_seconds();
void stamp_heartbeat();
[[nodiscard]] bool make_directories(const char *path);
} // namespace worker

} // namespace oslogger::copy_back
