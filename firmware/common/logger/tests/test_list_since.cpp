// LIST_SINCE policy on a real directory tree. Proves the incremental walk
// reads a bounded number of entries on a 5000+ file archive, and still finds
// every file a later boot or a moved clock adds.

#include <os_logger_list_merge.h>
#include <os_logger_list_since.h>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <sys/stat.h>

namespace ls = opensensor::logger::list_since;
namespace list_merge = opensensor::logger::list_merge;
using opensensor::logger::detail::DirectoryWalk;

namespace {

// POSIX IO that counts every directory entry read.
struct CountingIO {
  unsigned opened = 0, reads = 0;
  DIR *open(const char *path) {
    DIR *directory = opendir(path);
    if (directory)
      ++opened;
    return directory;
  }
  const struct dirent *read(DIR *directory) {
    ++reads;
    return readdir(directory);
  }
  int inspect(const char *path, struct stat *info) { return stat(path, info); }
  int close(DIR *directory) { return closedir(directory); }
};

std::filesystem::path root_path;
const char *kThing = "11111111-2222-3333-4444-555555555555";

ls::BootId boot_of(unsigned n) {
  ls::BootId boot{};
  boot[0] = 0xb0;
  boot[15] = static_cast<std::uint8_t>(n);
  return boot;
}

std::string hex(const ls::BootId &boot) {
  char text[33];
  ls::format_boot(boot, text);
  return text;
}

std::string thing_dir() { return std::string("thing=") + kThing; }

// Creates a file and returns its name relative to the archive root.
std::string touch(const std::string &relative) {
  const auto path = root_path / relative;
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path) << "x";
  return relative;
}

std::string dated(unsigned year, unsigned month, unsigned day,
                  const ls::BootId &boot, unsigned first) {
  char name[200];
  std::snprintf(name, sizeof(name),
                "%s/year=%04u/month=%02u/day=%02u/data_1200_%s_%u-%u-0.parquet",
                thing_dir().c_str(), year, month, day, hex(boot).c_str(), first,
                first);
  return name;
}

std::string unsynced(const ls::BootId &boot, unsigned first) {
  char name[200];
  std::snprintf(name, sizeof(name),
                "%s/unsynced/boot=%s/data_unsynced_%s_%u-%u-0.parquet",
                thing_dir().c_str(), hex(boot).c_str(), hex(boot).c_str(),
                first, first + 4);
  return name;
}

struct Listing {
  std::set<std::string> names;
  unsigned reads = 0;
  bool widened = false;
};

Listing run(const ls::Plan &plan, const ls::Ring &ring, unsigned budget = 32) {
  CountingIO io;
  ls::IncrementalWalk<CountingIO> walk;
  const std::string root = root_path.string();
  Listing out;
  assert(walk.begin(io, root.c_str(), kThing, plan, ring));
  auto visit = [&](const char *name, const struct stat &info) {
    if (S_ISREG(info.st_mode))
      assert(out.names.insert(name).second);
    return true;
  };
  using Status = ls::IncrementalWalk<CountingIO>::Status;
  for (;;) {
    const unsigned before = io.reads;
    const auto status = walk.step(budget, visit);
    assert(io.reads - before <= budget);
    if (status == Status::Done)
      break;
    assert(status == Status::More);
  }
  out.reads = io.reads;
  out.widened = walk.widened();
  return out;
}

// Everything plain LIST would name under this thing.
std::set<std::string> everything() {
  CountingIO io;
  DirectoryWalk<CountingIO> walk;
  const std::string root = root_path.string();
  std::set<std::string> names;
  assert(walk.begin(io, root.c_str(), thing_dir().c_str(), 1));
  auto visit = [&](const char *name, const struct stat &) {
    names.insert(name);
    return true;
  };
  while (walk.step(1000, visit) == DirectoryWalk<CountingIO>::Status::More) {
  }
  return names;
}

void check_names_and_dates() {
  static_assert(ls::days_from_civil(1970, 1, 1) == 0);
  static_assert(ls::civil_day(0) == 19700101);
  static_assert(ls::civil_day(ls::days_from_civil(2024, 2, 29)) == 20240229);
  static_assert(ls::civil_day(ls::days_from_civil(2100, 12, 31)) == 21001231);
  static_assert(ls::day_before(0) == 19691231);
  static_assert(ls::day_before(1709251200000LL) == 20240229); // 2024-03-01
  static_assert(ls::day_of_utc_us(1709251199999999LL) == 20240229);
  static_assert(ls::min_known(0, 5) == 5 && ls::min_known(5, 0) == 5 &&
                ls::min_known(4, 5) == 4 && ls::min_known(0, 0) == 0);
  static_assert(ls::cursor_day(20191231) == 0);
  static_assert(ls::cursor_day(21010101) == 21001231);
  static_assert(ls::cursor_day(20260101) == 20260101);
  static_assert(
      ossync::valid_list_day(20240229) && !ossync::valid_list_day(20230229) &&
      !ossync::valid_list_day(20191231) && ossync::valid_list_day(21001231) &&
      !ossync::valid_list_day(21010101) && !ossync::valid_list_day(20260431) &&
      !ossync::valid_list_day(20261301));

  static_assert(ls::enter_dated("thing=a/unsynced", 0) == false);
  static_assert(ls::enter_dated("thing=a/unsynced/boot=x", 20260101) == false);
  static_assert(ls::enter_dated("thing=a/unsynced2", 20260101));
  static_assert(ls::enter_dated("thing=a/year=2025", 0));
  static_assert(!ls::enter_dated("thing=a/year=2025", 20260101));
  static_assert(ls::enter_dated("thing=a/year=2026", 20260315));
  static_assert(!ls::enter_dated("thing=a/year=2026/month=02", 20260315));
  static_assert(ls::enter_dated("thing=a/year=2026/month=03", 20260315));
  static_assert(
      !ls::enter_dated("thing=a/year=2026/month=03/day=14", 20260315));
  static_assert(ls::enter_dated("thing=a/year=2026/month=03/day=15", 20260315));
  static_assert(ls::enter_dated("thing=a/year=2027/month=01/day=01", 20260315));
  // A folder it does not recognise is always entered.
  static_assert(ls::enter_dated("thing=a/stray", 20260315));
  static_assert(ls::enter_dated("thing=a/year=20x6", 20260315));
  static_assert(ls::enter_dated("thing=a/year=2026/odd", 20260315));

  static_assert(*ls::first_seq("a/b/data_unsynced_ab_17-20-0.parquet") == 17);
  static_assert(*ls::first_seq("data_1200_ab_0-3-1.parquet") == 0);
  static_assert(*ls::first_seq("x_9999999999999999999-1.parquet") ==
                9999999999999999999ULL);
  // Twenty digits could overflow, so the name does not follow the pattern.
  static_assert(!ls::first_seq("x_18446744073709551615-1.parquet"));
  static_assert(!ls::first_seq("a/data.parquet"));
  static_assert(!ls::first_seq("a/data_x-1.parquet"));
  static_assert(!ls::first_seq("a/data_12x-1.parquet"));

  ls::BootId boot{};
  assert(ls::parse_boot("00112233445566778899aabbccddeeff", boot));
  assert(hex(boot) == "00112233445566778899aabbccddeeff");
  assert(ls::parse_boot("00112233445566778899AABBCCDDEEFF", boot));
  assert(!ls::parse_boot("00112233445566778899aabbccddeef", boot));
  assert(!ls::parse_boot("00112233445566778899aabbccddeeff0", boot));
  assert(!ls::parse_boot("0011223344556677889-aabbccddeeff0", boot));

  ossync::ListCursor cursor{.mount_epoch = 0x01020304,
                            .day = 20260315,
                            .boot = boot,
                            .seq = 0x1122334455667788ULL};
  std::array<std::uint8_t, ossync::kListCursorBytes> wire{};
  ossync::encode_cursor(cursor, wire);
  assert(wire[0] == 0x04 && wire[3] == 0x01);
  assert(wire[8] == 0x00 && wire[9] == 0x11 && wire[23] == 0xff);
  assert(wire[24] == 0x88 && wire[31] == 0x11);
  const auto back = ossync::decode_cursor(wire);
  assert(back.mount_epoch == cursor.mount_epoch && back.day == cursor.day &&
         back.boot == cursor.boot && back.seq == cursor.seq);
}

void check_ring() {
  ls::Ring ring;
  assert(!ring.size() && !ring.newest() && !ring.wrapped());
  for (unsigned i = 0; i < 16; ++i)
    ring.append(boot_of(i));
  assert(ring.size() == 16 && !ring.wrapped());
  assert(*ring.find(boot_of(0)) == 0 && *ring.find(boot_of(15)) == 15);
  ring.append(boot_of(16));
  assert(ring.wrapped() && ring.size() == 16);
  assert(!ring.find(boot_of(0)));
  assert(*ring.find(boot_of(1)) == 0 && *ring.find(boot_of(16)) == 15);
  assert(ring.holds_newest(boot_of(16)));
  std::array<std::uint8_t, ls::kRingBlobBytes> blob{};
  ring.encode(blob);
  assert(blob[0] == 17 && blob[1] == 0);
  const auto copy = ls::Ring::decode(blob);
  assert(copy && copy->appended() == 17 && *copy->find(boot_of(5)) == 4);
  assert(!ls::Ring::decode(std::span<const std::uint8_t>(blob.data(), 259)));
}

// A fake NVS key.
struct FakeStore {
  ls::Load mode = ls::Load::Missing;
  std::array<std::uint8_t, ls::kRingBlobBytes> blob{};
  std::size_t length = 0;
  bool fail_save = false;
  unsigned saves = 0, loads = 0;

  ls::RingStore store() {
    return {.context = this,
            .load =
                [](void *context,
                   std::span<std::uint8_t, ls::kRingBlobBytes> out,
                   std::size_t &length) {
                  auto &self = *static_cast<FakeStore *>(context);
                  ++self.loads;
                  if (self.mode == ls::Load::Found) {
                    std::memcpy(out.data(), self.blob.data(), out.size());
                    length = self.length;
                  }
                  return self.mode;
                },
            .save =
                [](void *context,
                   std::span<const std::uint8_t, ls::kRingBlobBytes> in) {
                  auto &self = *static_cast<FakeStore *>(context);
                  ++self.saves;
                  if (self.fail_save)
                    return false;
                  std::memcpy(self.blob.data(), in.data(), in.size());
                  self.length = in.size();
                  self.mode = ls::Load::Found;
                  return true;
                }};
  }
};

void check_keeper() {
  // A boot appends itself once, and a later boot finds it.
  FakeStore nvs;
  {
    ls::RingKeeper keeper;
    keeper.set_current(boot_of(1));
    assert(!keeper.current_unlisted() && !keeper.needs_marker());
    keeper.note_unsynced(nvs.store());
    keeper.note_unsynced(nvs.store());
    assert(nvs.saves == 1 && keeper.appended() && !keeper.current_unlisted());
  }
  {
    ls::RingKeeper keeper;
    keeper.set_current(boot_of(2));
    const auto *ring = keeper.ring(nvs.store());
    assert(ring && ring->size() == 1 && ring->holds_newest(boot_of(1)));
    keeper.note_unsynced(nvs.store());
    assert(nvs.saves == 2 && keeper.ring(nvs.store())->size() == 2);
  }
  // A failing save never blocks, and retries on the next unsynced file.
  {
    ls::RingKeeper keeper;
    keeper.set_current(boot_of(3));
    nvs.fail_save = true;
    keeper.note_unsynced(nvs.store());
    assert(!keeper.appended() && keeper.current_unlisted());
    // The logger then marks the boot folder once.
    assert(keeper.needs_marker());
    keeper.marker_made();
    assert(!keeper.needs_marker());
    // The RAM ring keeps what NVS holds.
    assert(keeper.ring(nvs.store())->size() == 2);
    ls::Inputs in{
        .cursor = {.mount_epoch = 7, .day = 0, .boot = boot_of(2), .seq = 0},
        .mount_epoch = 7,
        .yesterday = 0,
        .lowest_dated = 0,
        .ring = keeper.ring(nvs.store()),
        .current = boot_of(3),
        .current_unlisted = keeper.current_unlisted(),
        .current_last_seq = 9};
    const auto plan = ls::plan(in);
    assert(!plan.full && plan.unsynced_full);
    assert(plan.result.flags == ossync::kListSinceUnsyncedFull);
    assert(plan.result.next.boot == ls::kUnknownBoot &&
           plan.result.next.seq == ls::kSeqDone);
    nvs.fail_save = false;
    keeper.note_unsynced(nvs.store());
    assert(keeper.appended() && !keeper.current_unlisted());
    assert(!keeper.needs_marker());
    assert(keeper.ring(nvs.store())->holds_newest(boot_of(3)));
    // The marker is stale once the append succeeds. A failed removal keeps
    // it stale, so the next unsynced file does not retry, and it lasts.
    assert(keeper.stale_marker());
    keeper.marker_removed();
    assert(!keeper.stale_marker() && !keeper.needs_marker());
  }
  // A boot that never needed a marker never has a stale one.
  {
    ls::RingKeeper keeper;
    keeper.set_current(boot_of(6));
    assert(!keeper.stale_marker());
    keeper.note_unsynced(nvs.store());
    assert(keeper.appended() && !keeper.stale_marker());
  }
  // A marked boot whose append never succeeded still needs its marker.
  {
    FakeStore failing;
    failing.fail_save = true;
    ls::RingKeeper keeper;
    keeper.set_current(boot_of(7));
    keeper.note_unsynced(failing.store());
    assert(!keeper.appended() && keeper.needs_marker());
    keeper.marker_made();
    assert(!keeper.needs_marker() && !keeper.stale_marker());
  }
  // The marker path, and a buffer too small for it.
  {
    char path[96];
    assert(ls::marker_path(path, "/sdcard/output", "ab", "0011"));
    assert(std::string(path) ==
           "/sdcard/output/thing=ab/unsynced/ringless=0011");
    char small[20];
    assert(!ls::marker_path(small, "/sdcard/output", "ab", "0011"));
    assert(!ls::marker_path(std::span<char>{}, "/sdcard/output", "ab", "0011"));
    // The path needs room for its terminator. A buffer exactly its length
    // fails and one byte more fits.
    constexpr std::size_t exact =
        sizeof("/sdcard/output/thing=ab/unsynced/ringless=0011") - 1;
    char edge[exact + 1];
    assert(!ls::marker_path(std::span<char>(edge, exact), "/sdcard/output",
                            "ab", "0011"));
    assert(ls::marker_path(std::span<char>(edge, exact + 1), "/sdcard/output",
                           "ab", "0011"));
  }
  // A failing read gives no ring, so LIST_SINCE walks every unsynced boot.
  {
    FakeStore broken;
    broken.mode = ls::Load::Failed;
    ls::RingKeeper keeper;
    keeper.set_current(boot_of(4));
    keeper.note_unsynced(broken.store());
    assert(broken.saves == 0 && keeper.current_unlisted());
    assert(!keeper.ring(broken.store()));
    ls::Inputs in{.cursor = {.mount_epoch = 7, .day = 0, .boot = {}, .seq = 0},
                  .mount_epoch = 7,
                  .yesterday = 0,
                  .lowest_dated = 0,
                  .ring = nullptr,
                  .current = boot_of(4),
                  .current_unlisted = true,
                  .current_last_seq = std::nullopt};
    assert(ls::plan(in).unsynced_full);
  }
  // A blob of another size is never overwritten.
  {
    FakeStore odd;
    odd.mode = ls::Load::Found;
    odd.length = 100;
    ls::RingKeeper keeper;
    keeper.set_current(boot_of(5));
    keeper.note_unsynced(odd.store());
    assert(odd.saves == 0 && !keeper.ring(odd.store()));
  }
}

// The state the logger keeps per boot and hands to plan().
void check_tracker() {
  ls::Tracker tracker;
  assert(tracker.lowest_dated() == 0 && !tracker.last_unsynced_seq());
  tracker.dated(2026, 3, 15);
  tracker.dated(2026, 3, 16);
  assert(tracker.lowest_dated() == 20260315);
  // A clock moved back lowers it.
  tracker.dated(2025, 12, 31);
  assert(tracker.lowest_dated() == 20251231);
  // Benchmark files, dated files and a negative sequence leave it alone.
  tracker.finalized(false, false, 40);
  tracker.finalized(true, false, 90);
  tracker.finalized(false, true, 91);
  tracker.finalized(false, false, -1);
  assert(tracker.last_unsynced_seq() == 40);
  tracker.finalized(false, false, 44);
  assert(tracker.last_unsynced_seq() == 44);

  FakeStore nvs;
  ls::RingKeeper keeper;
  keeper.set_current(boot_of(6));
  keeper.note_unsynced(nvs.store());
  const ossync::ListCursor cursor{
      .mount_epoch = 5, .day = 20260316, .boot = boot_of(6), .seq = 30};
  const auto in = tracker.inputs(cursor, 5, 20260317, keeper, nvs.store());
  assert(in.cursor.day == 20260316 && in.cursor.seq == 30);
  assert(in.mount_epoch == 5 && in.yesterday == 20260317);
  assert(in.lowest_dated == 20251231 && in.current_last_seq == 44);
  assert(in.ring && in.ring->holds_newest(boot_of(6)));
  assert(in.current == boot_of(6) && !in.current_unlisted);
  // The backdated day lowers the floor, and the cursor resumes after 44.
  const auto plan = ls::plan(in);
  assert(plan.floor == 20251231 && plan.first_seq == 30);
  assert(plan.result.next.boot == boot_of(6) && plan.result.next.seq == 45);
}

ls::Inputs inputs(const ossync::ListCursor &cursor, const ls::Ring *ring,
                  const ls::BootId &current) {
  return {.cursor = cursor,
          .mount_epoch = 0xcafe,
          .yesterday = 20260314,
          .lowest_dated = 20260315,
          .ring = ring,
          .current = current,
          .current_unlisted = false,
          .current_last_seq = std::nullopt};
}

void check_plan() {
  ls::Ring ring;
  ring.append(boot_of(1));
  ring.append(boot_of(2));
  const ossync::ListCursor zero{};
  // Epoch 0 never matches, so the first request is a full listing.
  auto in = inputs(zero, &ring, boot_of(2));
  in.mount_epoch = 0;
  assert(ls::plan(in).full);
  // A swapped card gives a full listing and the new epoch.
  in = inputs(
      {.mount_epoch = 0xbeef, .day = 20260310, .boot = boot_of(1), .seq = 3},
      &ring, boot_of(2));
  auto plan = ls::plan(in);
  assert(plan.full && plan.result.flags == ossync::kListSinceFull);
  assert(plan.result.next.mount_epoch == 0xcafe);
  assert(plan.result.next.day == 20260314);
  assert(plan.result.next.boot == boot_of(2) && plan.result.next.seq == 0);
  in.current_last_seq = 41;
  assert(ls::plan(in).result.next.seq == 42);
  // The newest ring boot is an older one, so it has nothing left.
  in.current = boot_of(9);
  assert(ls::plan(in).result.next.seq == ls::kSeqDone);
  // A matching epoch uses the cursor.
  in = inputs(
      {.mount_epoch = 0xcafe, .day = 20260310, .boot = boot_of(1), .seq = 3},
      &ring, boot_of(2));
  plan = ls::plan(in);
  assert(!plan.full && !plan.unsynced_full && plan.result.flags == 0);
  assert(plan.floor == 20260310 && plan.first_age == 0 && plan.first_seq == 3);
  // The floor drops to yesterday and to the lowest day this boot dated.
  in.cursor.day = 20260320;
  assert(ls::plan(in).floor == 20260314);
  in.lowest_dated = 20250101;
  assert(ls::plan(in).floor == 20250101);
  assert(ls::plan(in).result.next.day == 20250101);
  // Without UTC the next floor echoes this one.
  in.yesterday = 0;
  in.lowest_dated = 0;
  assert(ls::plan(in).floor == 20260320 &&
         ls::plan(in).result.next.day == 20260320);
  // Day 0 means no floor.
  in.cursor.day = 0;
  assert(ls::plan(in).floor == 0 && ls::plan(in).result.next.day == 0);
  // A finished boot moves the first folder on by one.
  in.cursor.seq = ls::kSeqDone;
  plan = ls::plan(in);
  assert(plan.first_age == 1 && plan.first_seq == 0);
  // An unknown boot walks the whole unsynced tree.
  in.cursor.boot = boot_of(7);
  plan = ls::plan(in);
  assert(plan.unsynced_full &&
         plan.result.flags == ossync::kListSinceUnsyncedFull);
  // A zero boot came from an empty ring, so every ring boot is new.
  in.cursor.boot = {};
  plan = ls::plan(in);
  assert(!plan.unsynced_full && plan.first_age == 0 && plan.first_seq == 0);
  // An empty ring sends the zero boot.
  const ls::Ring empty;
  in.ring = &empty;
  plan = ls::plan(in);
  assert(ls::is_zero(plan.result.next.boot) &&
         plan.result.next.seq == ls::kSeqDone);
  // Once a boot aged out a zero boot no longer covers every boot.
  ls::Ring wrapped;
  for (unsigned i = 0; i < 17; ++i)
    wrapped.append(boot_of(i));
  in.ring = &wrapped;
  assert(ls::plan(in).unsynced_full);
}

// Decision record 0004. Files on the flash root carry no card epoch and
// sit outside the incremental walk, so a matching cursor still gets the full
// listing while flash holds any. The next cursor keeps this mount's epoch, so
// once flash is empty the same cursor is incremental again. A copy back that
// moved files onto the card moves the epoch on, which makes every cursor
// taken before it full.
void check_flash_fallback() {
  ls::Ring ring;
  ring.append(boot_of(1));
  ring.append(boot_of(2));
  auto in = inputs(
      {.mount_epoch = 0xcafe, .day = 20260310, .boot = boot_of(1), .seq = 3},
      &ring, boot_of(2));
  assert(!ls::plan(in).full);
  in.force_full = list_merge::since_needs_full(true, false);
  auto plan = ls::plan(in);
  assert(plan.full && plan.result.flags == ossync::kListSinceFull);
  assert(plan.result.next.mount_epoch == 0xcafe);
  in.force_full = list_merge::since_needs_full(false, false);
  assert(!ls::plan(in).full);
  // A copy moved the epoch on. The cursor taken before it is full, the
  // cursor the full listing hands out is incremental.
  in.mount_epoch = ls::next_epoch(0xcafe);
  assert(in.mount_epoch == 0xcaff);
  plan = ls::plan(in);
  assert(plan.full && plan.result.next.mount_epoch == 0xcaff);
  in.cursor.mount_epoch = plan.result.next.mount_epoch;
  assert(!ls::plan(in).full);
  // The epoch never wraps to 0, which means unknown, and unknown stays
  // unknown.
  assert(ls::next_epoch(0xffffffffU) == 1);
  assert(ls::next_epoch(0) == 0);
}

void check_walks() {
  const auto old_boot = boot_of(1);
  const auto current = boot_of(2);
  // 2025 dated archive: 365 days x 14 files.
  unsigned files = 0;
  for (unsigned month = 1; month <= 12; ++month) {
    static constexpr unsigned kDays[] = {31, 28, 31, 30, 31, 30,
                                         31, 31, 30, 31, 30, 31};
    for (unsigned day = 1; day <= kDays[month - 1]; ++day)
      for (unsigned i = 0; i < 14; ++i, ++files)
        touch(dated(2025, month, day, old_boot, files));
  }
  for (unsigned i = 0; i < 40; ++i)
    touch(unsynced(old_boot, i * 5));
  for (unsigned i = 0; i < 10; ++i)
    touch(unsynced(current, i * 5));
  touch(thing_dir() + "/unsynced/boot=" + hex(current) + "/open.partial");
  // Other things and loose root files are not this thing's.
  touch("thing=other/year=2025/month=12/day=31/data_1200_x_1-1-0.parquet");
  touch("loose.parquet");
  const auto all = everything();
  assert(all.size() > 5000);
  ls::Ring ring;
  ring.append(old_boot);
  ring.append(current);

  // A cursor with no floor and the whole unsynced tree names what LIST does.
  ls::Inputs in =
      inputs({.mount_epoch = 0xcafe, .day = 0, .boot = boot_of(9), .seq = 0},
             &ring, current);
  auto plan = ls::plan(in);
  assert(plan.unsynced_full);
  auto listing = run(plan, ring);
  assert(listing.names == all);
  const unsigned full_reads = listing.reads;
  assert(full_reads > 5000);
  // Small budgets resume to the same answer.
  assert(run(plan, ring, 1).names == all);

  // The phone's steady state. Yesterday's floor and the current boot from
  // seq 50 read about a hundred entries, not five thousand.
  in = inputs(
      {.mount_epoch = 0xcafe, .day = 20251230, .boot = current, .seq = 50},
      &ring, current);
  in.yesterday = 20251230;
  in.lowest_dated = 20251231;
  in.current_last_seq = 49;
  plan = ls::plan(in);
  listing = run(plan, ring);
  assert(listing.reads < 120);
  assert(listing.reads * 40 < full_reads);
  std::set<std::string> expected;
  for (const auto &name : all)
    if (name.find("/year=2025/month=12/day=30/") != std::string::npos ||
        name.find("/year=2025/month=12/day=31/") != std::string::npos)
      expected.insert(name);
  // A name the seq filter cannot parse is always passed on, here the open
  // .partial file the logger counts.
  expected.insert(thing_dir() + "/unsynced/boot=" + hex(current) +
                  "/open.partial");
  assert(listing.names == expected);
  assert(plan.result.next.boot == current && plan.result.next.seq == 50);
  std::cout << "LIST_SINCE steady state read " << listing.reads
            << " entries for " << listing.names.size() << " files, plain LIST "
            << full_reads << " entries for " << all.size() << " files\n";

  // New files after that cursor. The current boot writes more unsynced files,
  // a new boot starts, and its clock lands on a day far behind the floor.
  const auto cursor = plan.result.next;
  std::set<std::string> added;
  added.insert(touch(unsynced(current, 50)));
  added.insert(touch(unsynced(current, 55)));
  const auto later = boot_of(3);
  ring.append(later);
  added.insert(touch(unsynced(later, 0)));
  added.insert(touch(dated(2025, 3, 4, later, 7)));
  added.insert(touch(dated(2025, 12, 31, later, 8)));
  in = inputs(cursor, &ring, later);
  in.yesterday = 20251230;
  in.lowest_dated = 20250304;
  in.current_last_seq = 0;
  plan = ls::plan(in);
  assert(plan.floor == 20250304 && plan.first_age == 1 && plan.first_seq == 50);
  listing = run(plan, ring);
  for (const auto &name : added)
    assert(listing.names.count(name));
  // Names below the cursor seq in the cursor boot stay out.
  assert(!listing.names.count(unsynced(current, 45)));
  assert(plan.result.next.boot == later && plan.result.next.seq == 1);

  // The cursor boot finished, so its folder is not read again.
  in.cursor.boot = later;
  in.cursor.seq = ls::kSeqDone;
  in.lowest_dated = 0;
  in.yesterday = 20251231;
  in.cursor.day = 20251231;
  plan = ls::plan(in);
  listing = run(plan, ring);
  assert(!listing.names.count(unsynced(later, 0)));
  assert(listing.names.count(dated(2025, 12, 31, later, 8)));

  // A boot whose ring append failed all boot is in no ring. Its ringless=
  // marker makes later walks read its whole folder. A marker for a boot the
  // ring holds is ignored, so nothing is listed twice.
  const auto ringless = boot_of(5);
  const std::string unsynced_dir = thing_dir() + "/unsynced/";
  std::filesystem::create_directories(root_path / unsynced_dir /
                                      ("ringless=" + hex(ringless)));
  std::filesystem::create_directories(root_path / unsynced_dir /
                                      ("ringless=" + hex(later)));
  std::set<std::string> hidden;
  hidden.insert(touch(unsynced(ringless, 0)));
  hidden.insert(touch(unsynced(ringless, 5)));
  plan = ls::plan(in);
  assert(!plan.unsynced_full);
  listing = run(plan, ring);
  for (const auto &name : hidden)
    assert(listing.names.count(name));
  // The ring boot `later` was fully listed, so its marker reads nothing.
  assert(!listing.names.count(unsynced(later, 0)));
  assert(!listing.widened);
  // Without the marker the boot stays hidden, the gap the marker closes.
  std::filesystem::remove(root_path / unsynced_dir /
                          ("ringless=" + hex(ringless)));
  assert(!run(plan, ring).names.count(unsynced(ringless, 0)));
  std::filesystem::create_directories(root_path / unsynced_dir /
                                      ("ringless=" + hex(ringless)));
  // More markers than one walk tracks widen it to the whole unsynced tree.
  for (unsigned i = 10; i < 10 + ls::kMaxRingless; ++i)
    std::filesystem::create_directories(root_path / unsynced_dir /
                                        ("ringless=" + hex(boot_of(i))));
  listing = run(plan, ring);
  assert(listing.widened);
  for (const auto &name : everything())
    if (name.find("/unsynced/") != std::string::npos)
      assert(listing.names.count(name));

  // A ring boot whose folder is gone is skipped, as is a missing thing.
  ring.append(boot_of(4));
  in.cursor.boot = later;
  in.cursor.seq = 0;
  plan = ls::plan(in);
  listing = run(plan, ring);
  assert(listing.names.count(unsynced(later, 0)));
  std::filesystem::remove_all(root_path / thing_dir());
  assert(run(plan, ring).names.empty());
}

} // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  root_path = std::filesystem::path(argv[1]) / "sd";
  std::filesystem::create_directories(root_path);
  check_names_and_dates();
  check_ring();
  check_keeper();
  check_tracker();
  check_plan();
  check_flash_fallback();
  check_walks();
}
