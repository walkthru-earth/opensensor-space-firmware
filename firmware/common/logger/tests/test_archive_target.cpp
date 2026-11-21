#include "os_logger_archive_target.h"
#include "storage_governor.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
using namespace opensensor::logger::archive_target;

constexpr std::uint64_t KiB = 1024;

// The card wins whenever it may take files, whatever the flash volume holds.
void card_first() {
  for (const bool mounted : {false, true})
    for (const std::uint64_t free :
         {std::uint64_t{0}, kDefaultFlashReserve, std::uint64_t{4096} * KiB}) {
      const Inputs in{.card_ready = true,
                      .flash_mounted = mounted,
                      .flash_free_bytes = free};
      assert(choose(in) == Target::Card);
      assert(!flash_below_reserve(in));
    }
}

// Without a card, a mounted flash volume with room takes the file.
void flash_when_no_card() {
  const Inputs in{.card_ready = false,
                  .flash_mounted = true,
                  .flash_free_bytes = 3 * 1024 * KiB};
  assert(choose(in) == Target::Flash);
  assert(!flash_below_reserve(in));
}

// No card and no flash volume leaves the rows in the RAM batch.
void none_without_either_root() {
  for (const std::uint64_t free :
       {std::uint64_t{0}, std::uint64_t{4096} * KiB}) {
    const Inputs in{
        .card_ready = false, .flash_mounted = false, .flash_free_bytes = free};
    assert(choose(in) == Target::None);
    // An unmounted volume is not full, it is absent.
    assert(!flash_below_reserve(in));
  }
}

// The reserve is one full file. At exactly the reserve a file still goes to
// flash, one byte below it none does, and room returning resumes it.
void reserve_boundary() {
  static_assert(kDefaultFlashReserve == 128 * KiB);
  Inputs in{.card_ready = false,
            .flash_mounted = true,
            .flash_free_bytes = kDefaultFlashReserve};
  assert(choose(in) == Target::Flash && !flash_below_reserve(in));
  in.flash_free_bytes = kDefaultFlashReserve - 1;
  assert(choose(in) == Target::None && flash_below_reserve(in));
  in.flash_free_bytes = 0;
  assert(choose(in) == Target::None && flash_below_reserve(in));
  in.flash_free_bytes = kDefaultFlashReserve + 1;
  assert(choose(in) == Target::Flash && !flash_below_reserve(in));
  // A card returning while flash is full takes the file at once.
  in.flash_free_bytes = 0;
  in.card_ready = true;
  assert(choose(in) == Target::Card && !flash_below_reserve(in));
}

// A caller may set its own reserve. A zero reserve still needs one free byte.
void reserve_override() {
  Inputs in{.card_ready = false,
            .flash_mounted = true,
            .flash_free_bytes = 64 * KiB,
            .flash_reserve_bytes = 64 * KiB};
  assert(choose(in) == Target::Flash);
  in.flash_free_bytes = 64 * KiB - 1;
  assert(choose(in) == Target::None && flash_below_reserve(in));
  in.flash_reserve_bytes = 0;
  in.flash_free_bytes = 1;
  assert(choose(in) == Target::Flash);
  in.flash_free_bytes = 0;
  assert(choose(in) == Target::None && flash_below_reserve(in));
}

// Every combination of the three inputs, with free space below, at and above
// the default reserve.
void every_combination() {
  const std::array<std::uint64_t, 3> frees = {
      kDefaultFlashReserve - 1, kDefaultFlashReserve, kDefaultFlashReserve + 1};
  for (const bool card : {false, true})
    for (const bool mounted : {false, true})
      for (const std::uint64_t free : frees) {
        const Inputs in{.card_ready = card,
                        .flash_mounted = mounted,
                        .flash_free_bytes = free};
        const Target want = card ? Target::Card
                            : mounted && free >= kDefaultFlashReserve
                                ? Target::Flash
                                : Target::None;
        assert(choose(in) == want);
        assert(flash_below_reserve(in) ==
               (!card && mounted && free < kDefaultFlashReserve));
      }
}

void names_are_stable() {
  assert(std::strcmp(target_name(Target::Card), "card") == 0);
  assert(std::strcmp(target_name(Target::Flash), "flash") == 0);
  assert(std::strcmp(target_name(Target::None), "none") == 0);
  static_assert(choose(Inputs{.card_ready = true}) == Target::Card);
}
// The probe write plan (#218, #219). The rows test the card, an open flash
// file closes only for a card that will take files and did not just fail.
void probe_plan() {
  using enum ProbeAction;
  const ProbeInputs fresh{.space_ok = true,
                          .card_writable = true,
                          .rows_waiting = true,
                          .flash_open = true,
                          .flash_takes_rows = true};
  assert(probe_action(fresh) == FinalizeWrite);
  // Below the reserve nothing is written, whatever else holds.
  for (const auto writes :
       {CardWrites::Untested, CardWrites::Passed, CardWrites::Failed}) {
    ProbeInputs in = fresh;
    in.space_ok = false;
    in.card_writable = false;
    in.writes = writes;
    assert(probe_action(in) == Measure);
  }
  // A Full card in the band above the reserve keeps the flash file and its
  // rows on flash (#219). Without a flash root the rows still go to the
  // card above its reserve, as before.
  ProbeInputs band = fresh;
  band.card_writable = false;
  assert(probe_action(band) == Measure);
  band.flash_open = false;
  assert(probe_action(band) == Measure);
  band.flash_takes_rows = false;
  assert(probe_action(band) == Write);
  band.rows_waiting = false;
  assert(probe_action(band) == Measure);
  // No rows waiting writes nothing and keeps the flash file open. A card
  // whose last test failed is not called writable on its mount (#218).
  ProbeInputs idle = fresh;
  idle.rows_waiting = false;
  assert(probe_action(idle) == Measure);
  idle.writes = CardWrites::Passed;
  assert(probe_action(idle) == Measure);
  idle.writes = CardWrites::Failed;
  assert(probe_action(idle) == Defer);
  idle.flash_open = false;
  assert(probe_action(idle) == Defer);
  // Rows and no flash file open test the card at no cost, even after a
  // failed test.
  ProbeInputs gap = fresh;
  gap.flash_open = false;
  gap.writes = CardWrites::Failed;
  assert(probe_action(gap) == Write);
  // After a failed test the open flash file waits, at most
  // kRetestFinalizeSeconds.
  ProbeInputs failed = fresh;
  failed.writes = CardWrites::Failed;
  failed.failed_age_s = 0;
  assert(probe_action(failed) == Defer);
  failed.failed_age_s = kRetestFinalizeSeconds - 1;
  assert(probe_action(failed) == Defer);
  failed.failed_age_s = kRetestFinalizeSeconds;
  assert(probe_action(failed) == FinalizeWrite);
  failed.writes = CardWrites::Passed;
  failed.failed_age_s = 0;
  assert(probe_action(failed) == FinalizeWrite);
  static_assert(probe_action(ProbeInputs{}) == Measure);
}

// A refused copy write fails the card only while it has not passed a write
// since its last probe (#218).
void copy_failure_rule() {
  assert(copy_fails_card(CardWrites::Untested, true));
  assert(copy_fails_card(CardWrites::Failed, true));
  assert(!copy_fails_card(CardWrites::Passed, true));
  for (const auto writes :
       {CardWrites::Untested, CardWrites::Passed, CardWrites::Failed})
    assert(!copy_fails_card(writes, false));
}

// One simulated day of the storage worker with a card that mounts but cannot
// take a write, or that sits Full in the band above its reserve (#219). The
// model samples every 10 s, commits a row group of up to 90 rows, closes a
// file at its last row group or at the window change, and runs the governor
// probe and the Full remount probe as the worker schedules them. It counts
// files and dropped rows, under the rule before #219 and the rule now.
enum class Rule : std::uint8_t { Before, Now };
enum class Card : std::uint8_t { CannotWrite, FullBand };
struct Day {
  unsigned flash_files = 0;
  unsigned card_files = 0;
  unsigned dropped = 0;
  unsigned tests = 0;
};

Day simulate_day(Rule rule, Card card, std::int64_t rotation_s) {
  namespace st = oslogger::storage;
  constexpr std::int64_t kTick = 10, kDay = 24 * 3600;
  constexpr std::size_t kBatch = 90;
  constexpr std::uint64_t MiB = 1024 * 1024;
  constexpr std::uint64_t kTotal = 64 * 1024 * MiB;
  const auto group_rows = std::min<std::size_t>(
      static_cast<std::size_t>(rotation_s / kTick), kBatch);
  const auto groups_per_file =
      static_cast<unsigned>(rotation_s / kTick) / group_rows;
  st::Governor governor;
  governor.configure(kTotal);
  const st::Thresholds limits = governor.limits();
  // Reserve 32 MiB, band up to 48 MiB.
  const std::uint64_t free = card == Card::CannotWrite ? 1024 * MiB : 40 * MiB;
  if (card == Card::CannotWrite)
    governor.write_failed(0);
  else
    governor.observe_free(31 * MiB, 0); // Full, then the band above it
  enum class Open : std::uint8_t { None, Flash, Card } open = Open::None;
  unsigned groups = 0;
  std::size_t count = 0;
  std::int64_t window = 0;
  CardWrites writes = CardWrites::Untested;
  std::int64_t failed_at = 0;
  Day day;
  const auto close = [&] {
    open = Open::None;
    groups = 0;
  };
  // Appends the batch to the open file, or to a new one on `to_card`.
  const auto append = [&](bool to_card) {
    if (to_card && card == Card::CannotWrite)
      return false;
    if (open == Open::None) {
      open = to_card ? Open::Card : Open::Flash;
      ++(to_card ? day.card_files : day.flash_files);
    }
    count = 0;
    if (++groups >= groups_per_file)
      close();
    return true;
  };
  const auto can_commit = [&] {
    return open != Open::Card || governor.writable();
  };
  const auto commit = [&] {
    const bool to_card =
        open == Open::Card || (open == Open::None && governor.writable());
    if (!append(to_card))
      governor.write_failed(0);
  };
  const auto probe = [&](std::int64_t now) {
    bool ok = true;
    if (rule == Rule::Before) {
      if (free >= limits.reserve) {
        if (open == Open::Flash)
          close();
        if (count) {
          ++day.tests;
          ok = append(true);
        }
      }
    } else {
      const auto plan =
          probe_action({.space_ok = free >= limits.reserve,
                        .card_writable = st::writable(
                            st::probe_state(governor.state(), free, limits)),
                        .rows_waiting = count != 0,
                        .flash_open = open == Open::Flash,
                        .flash_takes_rows = open != Open::Card,
                        .writes = writes,
                        .failed_age_s = now - failed_at});
      using enum ProbeAction;
      if (plan == FinalizeWrite)
        close();
      if (plan == Write || plan == FinalizeWrite) {
        ++day.tests;
        if (append(true)) {
          writes = CardWrites::Passed;
        } else {
          writes = CardWrites::Failed;
          failed_at = now;
          ok = false;
        }
      } else if (plan == Defer) {
        ok = false;
      } else if (writes != CardWrites::Failed) {
        writes = CardWrites::Untested;
      }
    }
    if (ok)
      governor.probe_succeeded(free, now);
    else
      governor.probe_failed(now);
  };
  for (std::int64_t now = kTick; now < kDay; now += kTick) {
    const std::int64_t row_window = now / rotation_s;
    if (row_window != window && count) {
      if (!can_commit()) {
        ++day.dropped; // the new row, as admit_unfitted_row drops it
        continue;
      }
      commit();
      close();
    } else if (row_window != window && open != Open::None) {
      close();
    }
    window = row_window;
    if (count == kBatch)
      ++day.dropped;
    else
      ++count;
    if (count >= group_rows && can_commit())
      commit();
    const bool full_remount = governor.state() == st::State::Full &&
                              governor.recheck_due(now) && open != Open::Card;
    if (governor.probe_due(now) || full_remount)
      probe(now);
    else if (governor.recheck_due(now))
      governor.observe_free(free, now);
  }
  return day;
}

// The measurement #219 asked for. A card that mounts but cannot write used
// to close the open flash file at each probe, and a Full card in the band
// above its reserve took the waiting rows into a card file that then held
// every later row off flash until they dropped.
void bad_card_day() {
  for (const std::int64_t rotation : {600, 900, 1800, 3600}) {
    const auto windows = static_cast<unsigned>(24 * 3600 / rotation);
    const Day before = simulate_day(Rule::Before, Card::CannotWrite, rotation);
    const Day now = simulate_day(Rule::Now, Card::CannotWrite, rotation);
    std::printf("cannot-write rotation=%llds windows=%u flash_files "
                "before=%u now=%u card_tests before=%u now=%u\n",
                static_cast<long long>(rotation), windows, before.flash_files,
                now.flash_files, before.tests, now.tests);
    assert(before.dropped == 0 && now.dropped == 0);
    assert(before.card_files == 0 && now.card_files == 0);
    // At most one extra flash file per kRetestFinalizeSeconds, and the
    // card is still tested at least once a window.
    assert(now.flash_files <= windows + 24 + 1);
    assert(now.flash_files <= before.flash_files);
    assert(now.tests >= windows / 2);
    const Day band_before =
        simulate_day(Rule::Before, Card::FullBand, rotation);
    const Day band_now = simulate_day(Rule::Now, Card::FullBand, rotation);
    std::printf("full-band rotation=%llds flash_files before=%u now=%u "
                "card_files before=%u now=%u dropped before=%u now=%u\n",
                static_cast<long long>(rotation), band_before.flash_files,
                band_now.flash_files, band_before.card_files,
                band_now.card_files, band_before.dropped, band_now.dropped);
    assert(band_now.dropped == 0 && band_now.card_files == 0);
    assert(band_now.flash_files <= windows + 1);
  }
  // The longest rotation is where the probe cut files before.
  const Day before = simulate_day(Rule::Before, Card::CannotWrite, 3600);
  const Day now = simulate_day(Rule::Now, Card::CannotWrite, 3600);
  assert(before.flash_files > 2 * now.flash_files);
}

} // namespace

int main() {
  card_first();
  flash_when_no_card();
  none_without_either_root();
  reserve_boundary();
  reserve_override();
  every_combination();
  names_are_stable();
  probe_plan();
  copy_failure_rule();
  bad_card_day();
  return 0;
}
