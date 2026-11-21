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

// What the card showed at its last write (decision record 0004, item 4).
// A probe that wrote nothing leaves Untested, so the next card write, a new
// card file or a copy back write, is the test.
enum class CardWrites : std::uint8_t {
  Untested, // no write since the last probe that wrote nothing
  Passed,   // a card write succeeded
  Failed,   // a probe write test failed, and no card write succeeded since
};

// After a failed write test an open flash file is finalized for another test
// at most once in this time. Without it a card that mounts but cannot write
// closed the flash file at every probe, about 150 extra files a day at the
// 600 s backoff cap. Between those tests the rows wait in RAM before each
// flash file opens, and a probe then tests the card without closing a file.
inline constexpr std::int64_t kRetestFinalizeSeconds = 3600;

struct ProbeInputs {
  // The probe measured at least the card reserve.
  bool space_ok = false;
  // The space band after this probe lets the card take files, that is
  // storage::probe_state() is Ready or Degraded. A Full card in the band
  // above the reserve stays Full and cannot.
  bool card_writable = false;
  bool rows_waiting = false;
  // The open telemetry file is on flash.
  bool flash_open = false;
  // The next commit goes to flash while the card cannot take it, so the
  // rows have a home without the card.
  bool flash_takes_rows = false;
  CardWrites writes = CardWrites::Untested;
  // Seconds since the failed write test, when writes is Failed.
  std::int64_t failed_age_s = 0;
};

enum class [[nodiscard]] ProbeAction : std::uint8_t {
  Measure,       // no card write, the probe succeeds on its mount and space
  Write,         // the waiting rows test the card, no flash file is open
  FinalizeWrite, // finalize the open flash file, then the rows test the card
  Defer,         // no card write, and the probe counts as failed
};

// The card write at a storage probe (#218, #219). The waiting rows are the
// write test. An open flash file is finalized for the test only when the
// card will take files after the probe and its last write test did not fail
// within kRetestFinalizeSeconds, so a bad or Full card never cuts the flash
// file into short pieces. With no rows waiting nothing is written and the
// flash file stays open. A card whose last write test failed is not called
// writable on its mount alone, the probe defers and the backoff grows.
[[nodiscard]] constexpr ProbeAction probe_action(const ProbeInputs &in) {
  if (!in.space_ok)
    return ProbeAction::Measure;
  if (!in.card_writable) {
    // The rows go to flash while the card stays Full. A board without a
    // flash root writes them to the card above its reserve, as before.
    if (in.flash_takes_rows || !in.rows_waiting)
      return ProbeAction::Measure;
    return ProbeAction::Write;
  }
  const bool failed = in.writes == CardWrites::Failed;
  if (!in.rows_waiting)
    return failed ? ProbeAction::Defer : ProbeAction::Measure;
  if (!in.flash_open)
    return ProbeAction::Write;
  if (failed && in.failed_age_s < kRetestFinalizeSeconds)
    return ProbeAction::Defer;
  return ProbeAction::FinalizeWrite;
}

// A copy back write the card refused while the card has not passed a write
// since its last probe. It counts as the write test, so the card leaves
// Ready (#218). After a passed write a refused copy is that file's problem
// and leaves the card state alone, so one bad path never stops logging.
[[nodiscard]] constexpr bool copy_fails_card(CardWrites writes,
                                             bool card_write_failure) {
  return card_write_failure && writes != CardWrites::Passed;
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
