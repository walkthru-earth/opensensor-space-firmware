#include "os_logger_archive_target.h"

#include <array>
#include <cassert>
#include <cstdint>
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
} // namespace

int main() {
  card_first();
  flash_when_no_card();
  none_without_either_root();
  reserve_boundary();
  reserve_override();
  every_combination();
  names_are_stable();
  return 0;
}
