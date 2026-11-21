#pragma once

#include <cstdint>

// Where the storage worker creates its next archive file (decision record
// 0004, items 4 and 6). Pure, so the host test covers every input. The worker
// asks once per new file, and a file keeps its root until it is finalized.
// Nothing here touches a file, and no answer ever deletes one to make room.
namespace opensensor::logger::archive_target {

enum class [[nodiscard]] Target : std::uint8_t {
  Card,  // the card archive root
  Flash, // the internal flash archive root
  None,  // no file, rows stay in the RAM batch and drop when it is full
};

// One full file must still fit on the flash volume before a new one starts
// there. A 15 minute CoreS3 window measured 49,839 bytes (decision record
// 0002), so 128 KiB covers a window and its sidecar with room to spare. A
// caller may pass its own reserve in Inputs.
inline constexpr std::uint64_t kDefaultFlashReserve = 128ULL * 1024ULL;

struct Inputs {
  // The card may take files, that is it is mounted and the space governor
  // says writable (Ready or Degraded). Degraded still writes to the card.
  bool card_ready = false;
  // The flash volume is mounted and its archive root exists.
  bool flash_mounted = false;
  // Free bytes on the flash volume, measured by the caller.
  std::uint64_t flash_free_bytes = 0;
  std::uint64_t flash_reserve_bytes = kDefaultFlashReserve;
};

// The card while it may take files, otherwise the flash volume while it is
// mounted and holds at least the reserve (and at least one byte), otherwise
// no file.
[[nodiscard]] constexpr Target choose(const Inputs &in) {
  if (in.card_ready)
    return Target::Card;
  if (in.flash_mounted && in.flash_free_bytes > 0 &&
      in.flash_free_bytes >= in.flash_reserve_bytes)
    return Target::Flash;
  return Target::None;
}

// True when the flash volume is the only root left and has fallen below its
// reserve. The worker logs the change into and out of this condition.
[[nodiscard]] constexpr bool flash_below_reserve(const Inputs &in) {
  return !in.card_ready && in.flash_mounted && choose(in) == Target::None;
}

[[nodiscard]] constexpr const char *target_name(Target target) {
  switch (target) {
  case Target::Card:
    return "card";
  case Target::Flash:
    return "flash";
  case Target::None:
    return "none";
  }
  return "none";
}

} // namespace opensensor::logger::archive_target
