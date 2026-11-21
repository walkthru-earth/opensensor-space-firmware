#pragma once

// LIST_SINCE policy (docs/shared/ble-sync-protocol.md "LIST_SINCE"). Pure and
// allocation free, so host tests run it on a real directory tree. The logger
// supplies the clock, the boot ring and the IO.

#include "archive_sync.h"
#include "os_logger_directory.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <sys/stat.h>

namespace opensensor::logger::list_since {

using BootId = std::array<std::uint8_t, 16>;
inline constexpr std::size_t kRingBoots = 16;
// NVS blob "boots" in namespace "parquet". u32 LE append count, then the 16
// boot slots. Slot (count - 1) % 16 holds the newest boot.
inline constexpr std::size_t kRingBlobBytes = 4 + kRingBoots * 16;
// The cursor seq for a boot with nothing left to list.
inline constexpr std::uint64_t kSeqDone = UINT64_MAX;
// A boot id the ring never holds, so the next request walks the whole
// unsynced tree. Sent when the ring cannot vouch for every unsynced boot.
inline constexpr BootId kUnknownBoot = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                        0xff, 0xff, 0xff, 0xff};

constexpr bool is_zero(const BootId &boot) {
  return std::ranges::all_of(boot, [](std::uint8_t byte) { return byte == 0; });
}

// The 32 hex boot id text, as in "boot=<text>", to its 16 bytes in text
// order. False for any other length or a non hex digit.
constexpr bool parse_boot(const char *text, BootId &boot) {
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < 32; ++i)
    if (digit(text[i]) < 0)
      return false;
  if (text[32] != '\0')
    return false;
  for (std::size_t i = 0; i < 16; ++i)
    boot[i] = static_cast<std::uint8_t>(digit(text[2 * i]) * 16 +
                                        digit(text[2 * i + 1]));
  return true;
}

// Lower case, as the logger names boot folders.
constexpr void format_boot(const BootId &boot, std::span<char, 33> text) {
  constexpr char kHex[] = "0123456789abcdef";
  for (std::size_t i = 0; i < 16; ++i) {
    text[2 * i] = kHex[boot[i] >> 4];
    text[2 * i + 1] = kHex[boot[i] & 0x0f];
  }
  text[32] = '\0';
}

// The last kRingBoots boots that created an unsynced file, oldest first.
// Each boot appends itself once, before its first unsynced folder exists, so
// a held boot proves every later unsynced boot is held too.
class Ring {
public:
  std::uint32_t appended() const { return appended_; }
  std::size_t size() const {
    return appended_ < kRingBoots ? appended_ : kRingBoots;
  }
  // True once a boot aged out, so an empty-ring cursor no longer covers them.
  bool wrapped() const { return appended_ > kRingBoots; }
  // Age 0 is the oldest boot held.
  const BootId &at(std::size_t age) const {
    const std::size_t start = appended_ < kRingBoots ? 0 : appended_;
    return boots_[(start + age) % kRingBoots];
  }
  std::optional<std::size_t> find(const BootId &boot) const {
    for (std::size_t age = 0; age < size(); ++age)
      if (at(age) == boot)
        return age;
    return std::nullopt;
  }
  const BootId *newest() const { return size() ? &at(size() - 1) : nullptr; }
  bool holds_newest(const BootId &boot) const {
    return size() && *newest() == boot;
  }
  void append(const BootId &boot) {
    boots_[appended_ % kRingBoots] = boot;
    ++appended_;
  }
  void encode(std::span<std::uint8_t, kRingBlobBytes> blob) const {
    for (std::size_t i = 0; i < 4; ++i)
      blob[i] = static_cast<std::uint8_t>(appended_ >> (8 * i));
    for (std::size_t slot = 0; slot < kRingBoots; ++slot)
      std::memcpy(blob.data() + 4 + 16 * slot, boots_[slot].data(), 16);
  }
  // nullopt for a blob of any other size.
  static std::optional<Ring> decode(std::span<const std::uint8_t> blob) {
    if (blob.size() != kRingBlobBytes)
      return std::nullopt;
    Ring ring;
    for (std::size_t i = 0; i < 4; ++i)
      ring.appended_ |= static_cast<std::uint32_t>(blob[i]) << (8 * i);
    for (std::size_t slot = 0; slot < kRingBoots; ++slot)
      std::memcpy(ring.boots_[slot].data(), blob.data() + 4 + 16 * slot, 16);
    return ring;
  }

private:
  std::uint32_t appended_ = 0;
  std::array<BootId, kRingBoots> boots_{};
};

// Where the ring lives. load fills `blob` and its stored `length`, Missing
// when no ring was saved yet. save writes and commits the whole blob.
enum class Load : std::uint8_t { Found, Missing, Failed };
struct RingStore {
  void *context = nullptr;
  Load (*load)(void *, std::span<std::uint8_t, kRingBlobBytes> blob,
               std::size_t &length) = nullptr;
  bool (*save)(void *, std::span<const std::uint8_t, kRingBlobBytes>) = nullptr;
};

// The RAM copy of the ring for this boot. A store that fails never stops a
// file from being written. It only makes the next LIST_SINCE walk the whole
// unsynced tree. A blob of the wrong size is never overwritten, so a ring
// this firmware cannot read keeps every listing on that safe path.
class RingKeeper {
public:
  void set_current(const BootId &boot) { current_ = boot; }
  const BootId &current() const { return current_; }
  // Call before this boot's first unsynced folder is made, and again before
  // each later unsynced file until an append succeeded.
  void note_unsynced(const RingStore &store) {
    unsynced_ = true;
    if (held_ || !load(store))
      return;
    Ring next = ring_;
    next.append(current_);
    std::array<std::uint8_t, kRingBlobBytes> blob{};
    next.encode(blob);
    if (!store.save(store.context, blob))
      return;
    ring_ = next;
    held_ = true;
  }
  // The ring, or nullptr when it could not be read.
  const Ring *ring(const RingStore &store) {
    return load(store) ? &ring_ : nullptr;
  }
  // This boot made an unsynced folder the ring does not name.
  bool current_unlisted() const { return unsynced_ && !held_; }
  bool appended() const { return held_; }
  // The append failed and no ringless= marker names this boot yet. The
  // logger then makes "thing=<id>/unsynced/ringless=<boot>", so a later
  // boot's LIST_SINCE still finds this boot's folder.
  bool needs_marker() const { return current_unlisted() && !marked_; }
  void marker_made() { marked_ = true; }
  // A later append of this boot succeeded, so its marker is no longer
  // needed. The logger removes it, best effort. Otherwise a marker lasts
  // until the serial `parquet erase-archive CONFIRM`.
  bool stale_marker() const { return held_ && marked_; }
  void marker_removed() { marked_ = false; }

private:
  bool load(const RingStore &store) {
    if (loaded_)
      return true;
    std::array<std::uint8_t, kRingBlobBytes> blob{};
    std::size_t length = 0;
    switch (store.load(store.context, blob, length)) {
    case Load::Missing:
      ring_ = Ring{};
      loaded_ = true;
      return true;
    case Load::Found:
      if (length > blob.size())
        return false;
      if (const auto decoded = Ring::decode(
              std::span<const std::uint8_t>(blob.data(), length))) {
        ring_ = *decoded;
        loaded_ = true;
        return true;
      }
      return false;
    case Load::Failed:
      break;
    }
    return false;
  }

  Ring ring_;
  BootId current_{};
  bool loaded_ = false;
  bool unsynced_ = false;
  bool held_ = false;
  bool marked_ = false;
};

// Days since 1970-01-01 of a civil date (proleptic Gregorian).
constexpr std::int64_t days_from_civil(std::int64_t year, unsigned month,
                                       unsigned day) {
  year -= month <= 2;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const auto year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned day_of_year =
      (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

// YYYYMMDD of a day count. 0 for a year outside 1..9999.
constexpr std::uint32_t civil_day(std::int64_t days) {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto day_of_era = static_cast<unsigned>(days - era * 146097);
  const unsigned year_of_era = (day_of_era - day_of_era / 1460 +
                                day_of_era / 36524 - day_of_era / 146096) /
                               365;
  const unsigned day_of_year =
      day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const unsigned shifted = (5 * day_of_year + 2) / 153;
  const unsigned day = day_of_year - (153 * shifted + 2) / 5 + 1;
  const unsigned month = shifted < 10 ? shifted + 3 : shifted - 9;
  const std::int64_t year =
      static_cast<std::int64_t>(year_of_era) + era * 400 + (month <= 2);
  if (year < 1 || year > 9999)
    return 0;
  return static_cast<std::uint32_t>(year) * 10000 + month * 100 + day;
}

// The UTC day before the one holding `utc_ms`.
constexpr std::uint32_t day_before(std::int64_t utc_ms) {
  constexpr std::int64_t kDayMs = 86400000;
  const std::int64_t days =
      utc_ms >= 0 ? utc_ms / kDayMs : -((-utc_ms + kDayMs - 1) / kDayMs);
  return civil_day(days - 1);
}

// YYYYMMDD of the day= folder a dated file of `utc_us` lands in.
constexpr std::uint32_t day_of_utc_us(std::int64_t utc_us) {
  constexpr std::int64_t kDayUs = 86400000000LL;
  const std::int64_t days =
      utc_us >= 0 ? utc_us / kDayUs : -((-utc_us + kDayUs - 1) / kDayUs);
  return civil_day(days);
}

// The smaller of two days where 0 means unknown.
constexpr std::uint32_t min_known(std::uint32_t a, std::uint32_t b) {
  if (!a)
    return b;
  if (!b)
    return a;
  return a < b ? a : b;
}

// A floor the request may carry back. Only lowering a floor is safe, so a
// day before 2020 becomes 0 (no floor) and one after 2100 becomes its end.
constexpr std::uint32_t cursor_day(std::uint32_t day) {
  if (day < 20200101)
    return 0;
  if (day > 21001231)
    return 21001231;
  return day;
}

namespace detail {
// Parses `digits` decimal digits at text, then requires `end` ('/' or '\0').
constexpr bool number(const char *&text, unsigned digits, std::uint32_t &out) {
  out = 0;
  for (unsigned i = 0; i < digits; ++i, ++text) {
    if (*text < '0' || *text > '9')
      return false;
    out = out * 10 + static_cast<std::uint32_t>(*text - '0');
  }
  return *text == '\0' || *text == '/';
}
constexpr bool starts(const char *text, const char *prefix) {
  while (*prefix)
    if (*text++ != *prefix++)
      return false;
  return true;
}
} // namespace detail

// Whether the dated walk, started at "thing=<id>", enters `relative`
// (relative to the archive root). Never "thing=<id>/unsynced". A year=,
// month= or day= folder only when it can hold a day at or after `floor`, and
// any folder it does not recognise always, so a stray folder is never
// skipped. floor 0 enters every dated folder.
constexpr bool enter_dated(const char *relative, std::uint32_t floor) {
  const char *rest = relative;
  while (*rest && *rest != '/')
    ++rest;
  if (*rest != '/')
    return true;
  ++rest;
  if (detail::starts(rest, "unsynced") && (rest[8] == '\0' || rest[8] == '/'))
    return false;
  if (!floor)
    return true;
  std::uint32_t year, month, day;
  if (!detail::starts(rest, "year="))
    return true;
  rest += 5;
  if (!detail::number(rest, 4, year))
    return true;
  if (!*rest++)
    return year >= floor / 10000;
  if (!detail::starts(rest, "month="))
    return true;
  rest += 6;
  if (!detail::number(rest, 2, month))
    return true;
  if (!*rest++)
    return year * 100 + month >= floor / 100;
  if (!detail::starts(rest, "day="))
    return true;
  rest += 4;
  if (!detail::number(rest, 2, day))
    return true;
  return year * 10000 + month * 100 + day >= floor;
}

// First sequence in "<prefix>_<boot>_<first>-<rest>", the last component of
// `relative`. nullopt when the name does not follow that pattern.
constexpr std::optional<std::uint64_t> first_seq(const char *relative) {
  const char *name = relative;
  for (const char *c = relative; *c; ++c)
    if (*c == '/')
      name = c + 1;
  const char *dash = name;
  while (*dash && *dash != '-')
    ++dash;
  if (!*dash)
    return std::nullopt;
  const char *start = dash;
  while (start > name && start[-1] >= '0' && start[-1] <= '9')
    --start;
  if (start == dash || start == name || start[-1] != '_' || dash - start > 19)
    return std::nullopt;
  std::uint64_t value = 0;
  for (const char *c = start; c < dash; ++c)
    value = value * 10 + static_cast<std::uint64_t>(*c - '0');
  return value;
}

// What the logger knows when a LIST_SINCE starts.
struct Inputs {
  ossync::ListCursor cursor;
  // FAT volume serial of this mount, 0 when unknown.
  std::uint32_t mount_epoch = 0;
  // UTC day before today, 0 without UTC.
  std::uint32_t yesterday = 0;
  // Lowest day= folder this boot created a dated file in, 0 for none.
  std::uint32_t lowest_dated = 0;
  // nullptr when the ring could not be read.
  const Ring *ring = nullptr;
  BootId current{};
  // This boot created an unsynced file the ring does not hold, because its
  // append failed.
  bool current_unlisted = false;
  // Last sequence of this boot's newest finalized unsynced file.
  std::optional<std::uint64_t> current_last_seq;
  // Walk what plain LIST walks even for a matching cursor, while the flash
  // fallback root holds files the incremental walk never reads (decision
  // record 0004, list_merge::since_needs_full).
  bool force_full = false;
  // False while the card cannot be walked and the full walk reads the flash
  // root only. The cursor then carries epoch 0, so the card coming back with
  // the same epoch never takes it as current.
  bool card_listable = true;
};

// The epoch after a copy back moved files onto the card. Every cursor of the
// old epoch then gets the full listing, since a copied file may sit behind
// it. It skips 0, which means unknown, and 0 stays 0.
[[nodiscard]] constexpr std::uint32_t next_epoch(std::uint32_t epoch) {
  if (epoch == 0)
    return 0;
  return epoch == UINT32_MAX ? 1 : epoch + 1;
}

// The volume serial moved on by `bumps` copies, as next_epoch() applied that
// many times. A mount reads the serial and the worker shifts it by the
// stored count of files copied back (CopyCounter), so a cursor from before a
// copy stays stale across remounts and reboots.
[[nodiscard]] constexpr std::uint32_t shifted_epoch(std::uint32_t serial,
                                                    std::uint32_t bumps) {
  if (serial == 0)
    return 0;
  // The nonzero epochs form a cycle of UINT32_MAX values.
  const std::uint64_t index =
      (std::uint64_t{serial} - 1 + bumps) % std::uint64_t{UINT32_MAX};
  return static_cast<std::uint32_t>(index + 1);
}

// Copies one store write covers. A drain of the 16 MB boards' flash is about
// 150 files, so it writes the key about ten times.
inline constexpr std::uint32_t kCopyCountBlock = 16;

// Where the count of files copied back lives (NVS `osflash/copies`). load
// is Missing before the first copy of the board's life. save writes and
// commits the value.
struct CopyCountStore {
  void *context = nullptr;
  Load (*load)(void *, std::uint32_t &count) = nullptr;
  bool (*save)(void *, std::uint32_t count) = nullptr;
};

// The count of files copied back from flash over the life of the board. The
// mount epoch is the volume serial shifted by it. The store always holds a
// value above every count a boot used, written a block ahead before a copy,
// so a reset never brings back an epoch that a cursor from before a copy
// carries. Never erased, and it never goes down.
class CopyCounter {
public:
  // The epoch of a volume serial, 0 (unknown) while the count cannot be read.
  [[nodiscard]] std::uint32_t epoch(const CopyCountStore &store,
                                    std::uint32_t serial) {
    return load(store) ? shifted_epoch(serial, count_) : 0;
  }
  // Call before each copy moves a file. False when the count cannot be read
  // or the next block cannot be stored, and the copy then waits, so no file
  // moves without a count a reset cannot lose.
  [[nodiscard]] bool reserve(const CopyCountStore &store) {
    if (!load(store))
      return false;
    if (count_ < stored_)
      return true;
    if (count_ > UINT32_MAX - kCopyCountBlock)
      return false; // never reached, about 4e9 copies
    const std::uint32_t next = count_ + kCopyCountBlock;
    if (!store.save(store.context, next))
      return false;
    stored_ = next;
    return true;
  }
  // A copy moved a file, after reserve() returned true for it.
  void copied() {
    if (count_ < stored_)
      ++count_;
  }
  std::uint32_t count() const { return count_; }

private:
  bool load(const CopyCountStore &store) {
    if (loaded_)
      return true;
    std::uint32_t stored = 0;
    switch (store.load(store.context, stored)) {
    case Load::Missing:
      stored = 0;
      break;
    case Load::Found:
      break;
    case Load::Failed:
      return false;
    }
    count_ = stored_ = stored;
    loaded_ = true;
    return true;
  }

  std::uint32_t count_ = 0;
  std::uint32_t stored_ = 0;
  bool loaded_ = false;
};

struct Plan {
  // Walk what plain LIST walks. The cursor did not match this mount.
  bool full = false;
  // Dated walk floor, 0 for none.
  std::uint32_t floor = 0;
  // Walk the thing's whole unsynced tree, as the ring cannot place the
  // cursor boot.
  bool unsynced_full = false;
  // Ring ages to read, from first_age to the newest. The folder at
  // first_age keeps only names whose first sequence is at least first_seq.
  std::size_t first_age = 0;
  std::uint64_t first_seq = 0;
  ossync::ListSinceResult result;
};

constexpr Plan plan(const Inputs &in) {
  Plan out;
  out.full = in.force_full || !in.card_listable || !in.mount_epoch ||
             in.cursor.mount_epoch != in.mount_epoch;
  const std::uint32_t request_day = out.full ? 0 : in.cursor.day;
  const std::uint32_t floor =
      request_day
          ? min_known(request_day, min_known(in.yesterday, in.lowest_dated))
          : 0;
  out.floor = floor;
  auto &next = out.result.next;
  next.mount_epoch = in.card_listable ? in.mount_epoch : 0;
  next.day = cursor_day(in.yesterday ? min_known(in.yesterday, in.lowest_dated)
                                     : floor);
  const bool ring_trusted = in.ring && !in.current_unlisted;
  if (!ring_trusted) {
    next.boot = kUnknownBoot;
    next.seq = kSeqDone;
  } else if (const BootId *newest = in.ring->newest()) {
    next.boot = *newest;
    next.seq = *newest != in.current ? kSeqDone
               : in.current_last_seq ? *in.current_last_seq + 1
                                     : 0;
  } else {
    next.boot = BootId{};
    next.seq = kSeqDone;
  }
  if (out.full) {
    out.result.flags = ossync::kListSinceFull;
    return out;
  }
  if (!ring_trusted) {
    out.unsynced_full = true;
  } else if (is_zero(in.cursor.boot)) {
    // The ring was empty when the cursor was made. Every boot since is held
    // unless one aged out.
    out.unsynced_full = in.ring->wrapped();
  } else if (const auto age = in.ring->find(in.cursor.boot)) {
    out.first_age = *age;
    out.first_seq = in.cursor.seq;
    if (in.cursor.seq == kSeqDone) {
      out.first_age = *age + 1;
      out.first_seq = 0;
    }
  } else {
    out.unsynced_full = true;
  }
  if (out.unsynced_full)
    out.result.flags = ossync::kListSinceUnsyncedFull;
  return out;
}

// What this boot wrote that the next LIST_SINCE must know. RAM only, so a
// clock moved back across a reboot is caught by the periodic audit. The
// logger calls dated() before it creates a dated file, finalized() after a
// file was renamed into place, and inputs() when a LIST_SINCE starts.
class Tracker {
public:
  constexpr void dated(unsigned year, unsigned month, unsigned day) {
    lowest_dated_ =
        min_known(lowest_dated_,
                  static_cast<std::uint32_t>(year) * 10000 + month * 100 + day);
  }
  // Only an unsynced thing file moves the sequence the cursor resumes at.
  constexpr void finalized(bool benchmark, bool dated_file, std::int64_t last) {
    if (!benchmark && !dated_file && last >= 0)
      last_unsynced_seq_ = static_cast<std::uint64_t>(last);
  }
  Inputs inputs(const ossync::ListCursor &cursor, std::uint32_t mount_epoch,
                std::uint32_t yesterday, RingKeeper &keeper,
                const RingStore &store) const {
    Inputs in;
    in.cursor = cursor;
    in.mount_epoch = mount_epoch;
    in.yesterday = yesterday;
    in.lowest_dated = lowest_dated_;
    in.ring = keeper.ring(store);
    in.current = keeper.current();
    in.current_unlisted = keeper.current_unlisted();
    in.current_last_seq = last_unsynced_seq_;
    return in;
  }
  constexpr std::uint32_t lowest_dated() const { return lowest_dated_; }
  constexpr std::optional<std::uint64_t> last_unsynced_seq() const {
    return last_unsynced_seq_;
  }

private:
  std::uint32_t lowest_dated_ = 0;
  std::optional<std::uint64_t> last_unsynced_seq_;
};

// The folder name that marks a boot whose ring append failed.
inline constexpr char kRinglessPrefix[] = "ringless=";
// Ringless boots one walk reads by name. More widen it to the whole tree.
inline constexpr std::size_t kMaxRingless = 4;

// Writes "<directory>/thing=<thing>/unsynced/ringless=<boot>". False
// when it does not fit.
inline bool marker_path(std::span<char> out, const char *directory,
                        const char *thing, const char *boot) {
  if (out.empty())
    return false;
  const int length =
      std::snprintf(out.data(), out.size(), "%s/thing=%s/unsynced/%s%s",
                    directory, thing, kRinglessPrefix, boot);
  return length > 0 && static_cast<std::size_t>(length) < out.size();
}

// The incremental LIST_SINCE walk of one thing: the dated tree above the
// floor, then the unsynced tree, or the planned ring boot folders followed by
// one read of "unsynced/" for ringless= markers and the boot folders they
// name. Same step contract as DirectoryWalk. visit(name, info) sees every file,
// with name relative to the archive root, exactly as plain LIST names it.
template <class IO> class IncrementalWalk {
public:
  using Walk = opensensor::logger::detail::DirectoryWalk<IO>;
  using Status = typename Walk::Status;

  // `root` must outlive the walk. The plan and ring are copied.
  bool begin(IO &io, const char *root, const char *thing, const Plan &chosen,
             const Ring &ring) {
    cancel();
    io_ = &io;
    root_ = root;
    plan_ = chosen;
    ring_ = ring;
    const int length = std::snprintf(thing_, sizeof(thing_), "thing=%s", thing);
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(thing_)) {
      errno = ENAMETOOLONG;
      return false;
    }
    phase_ = Phase::Dated;
    age_ = chosen.first_age;
    widened_ = false;
    return open(walk_.begin(io, root, thing_, 1));
  }

  template <class Visitor> Status step(unsigned budget, Visitor &visit) {
    while (phase_ != Phase::Done && !walk_.active())
      if (!next_phase())
        return Status::Failed;
    if (phase_ == Phase::Done)
      return Status::Done;
    Status status;
    if (phase_ == Phase::Dated) {
      auto enter = [this](const char *relative) {
        return enter_dated(relative, plan_.floor);
      };
      status = walk_.step(budget, visit, enter);
    } else if (phase_ == Phase::Unsynced && widened_) {
      // The ring folders this walk already read are not read again.
      auto enter = [this](const char *relative) {
        return !read_in_boots(relative);
      };
      status = walk_.step(budget, visit, enter);
    } else if (phase_ == Phase::Scan) {
      auto skip = [](const char *, const struct stat &) { return true; };
      auto note = [this](const char *relative) {
        note_marker(relative);
        return false;
      };
      status = walk_.step(budget, skip, note);
    } else if (phase_ == Phase::Boots && age_ == plan_.first_age &&
               plan_.first_seq) {
      auto filtered = [&](const char *name, const struct stat &info) {
        const auto first = first_seq(name);
        return (first && *first < plan_.first_seq) || visit(name, info);
      };
      status = walk_.step(budget, filtered);
    } else {
      status = walk_.step(budget, visit);
    }
    if (status != Status::Done)
      return status;
    // The next folder opens now and is read on the next step.
    if (phase_ == Phase::Boots)
      ++age_;
    while (phase_ != Phase::Done && !walk_.active())
      if (!next_phase())
        return Status::Failed;
    return phase_ == Phase::Done ? Status::Done : Status::More;
  }

  void cancel() {
    walk_.cancel();
    phase_ = Phase::Done;
  }

  // True when ringless markers made the walk read the whole unsynced tree.
  bool widened() const { return widened_; }

private:
  enum class Phase : std::uint8_t {
    Dated,
    Unsynced,
    Boots,
    Scan,
    Ringless,
    Done
  };

  // Whether `relative` is a boot= folder the Boots phase read.
  bool read_in_boots(const char *relative) const {
    const char *name = std::strrchr(relative, '/');
    name = name ? name + 1 : relative;
    BootId boot{};
    if (!detail::starts(name, "boot=") || !parse_boot(name + 5, boot))
      return false;
    const auto age = ring_.find(boot);
    return age && *age >= plan_.first_age;
  }

  // Notes a ringless=<boot> folder of a boot the ring does not hold.
  void note_marker(const char *relative) {
    const char *name = std::strrchr(relative, '/');
    name = name ? name + 1 : relative;
    if (!detail::starts(name, kRinglessPrefix))
      return;
    BootId boot{};
    if (!parse_boot(name + sizeof(kRinglessPrefix) - 1, boot) ||
        ring_.find(boot))
      return;
    const auto known = std::span(ringless_).first(ringless_count_);
    if (std::ranges::any_of(known,
                            [&](const BootId &seen) { return seen == boot; }))
      return;
    if (ringless_count_ == ringless_.size()) {
      overflow_ = true;
      return;
    }
    ringless_[ringless_count_++] = boot;
  }

  bool open_unsynced(char (&relative)[160]) {
    std::snprintf(relative, sizeof(relative), "%s/unsynced", thing_);
    return open(walk_.begin(*io_, root_, relative, 2));
  }

  bool open_boot(const BootId &id, char (&relative)[160]) {
    char boot[33] = {};
    format_boot(id, boot);
    std::snprintf(relative, sizeof(relative), "%s/unsynced/boot=%s", thing_,
                  boot);
    return walk_.begin(*io_, root_, relative, 3);
  }

  // A missing folder is an empty one. Any other failure fails the walk.
  static bool open(bool opened) { return opened || errno == ENOENT; }

  // Opens the next folder after the current phase ran out. False on a
  // failure other than a missing folder.
  bool next_phase() {
    char relative[160];
    for (;;) {
      switch (phase_) {
      case Phase::Dated:
        if (plan_.unsynced_full) {
          phase_ = Phase::Unsynced;
          return open_unsynced(relative);
        }
        phase_ = Phase::Boots;
        continue;
      case Phase::Boots:
        if (age_ < ring_.size()) {
          if (open_boot(ring_.at(age_), relative))
            return true;
          if (errno != ENOENT)
            return false;
          ++age_;
          continue;
        }
        phase_ = Phase::Scan;
        ringless_count_ = 0;
        overflow_ = false;
        return open_unsynced(relative);
      case Phase::Scan:
        if (overflow_) {
          widened_ = true;
          phase_ = Phase::Unsynced;
          return open_unsynced(relative);
        }
        phase_ = Phase::Ringless;
        next_ringless_ = 0;
        continue;
      case Phase::Ringless:
        if (next_ringless_ < ringless_count_) {
          if (open_boot(ringless_[next_ringless_++], relative))
            return true;
          if (errno != ENOENT)
            return false;
          continue;
        }
        phase_ = Phase::Done;
        return true;
      case Phase::Unsynced:
      case Phase::Done:
        phase_ = Phase::Done;
        return true;
      }
    }
  }

  Walk walk_;
  IO *io_ = nullptr;
  const char *root_ = "";
  char thing_[48]{};
  Plan plan_{};
  Ring ring_{};
  Phase phase_ = Phase::Done;
  std::size_t age_ = 0;
  std::array<BootId, kMaxRingless> ringless_{};
  std::size_t ringless_count_ = 0;
  std::size_t next_ringless_ = 0;
  bool overflow_ = false;
  bool widened_ = false;
};

} // namespace opensensor::logger::list_since
