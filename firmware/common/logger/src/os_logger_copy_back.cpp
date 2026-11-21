#include "os_logger_copy_back.h"

#include "debug_log.h"
#include "os_logger.h"
#include "os_logger_copy_candidates.h"
#include "os_logger_directory.h"
#include "os_logger_flash_copy.h"
#include "os_logger_paths.h"
#include "sha256_stream.h"

#include <esp_heap_caps.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <expected>
#include <fcntl.h>
#include <new>
#include <span>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace oslogger::copy_back {
namespace {
namespace flash_copy = opensensor::logger::flash_copy;
namespace paths = opensensor::logger::paths;
using worker::flash_root;
using worker::stamp_heartbeat;
using worker::uptime_seconds;

class BusLock {
public:
  BusLock() { lock_bus(); }
  ~BusLock() { unlock_bus(); }
};

// The archive walk IO of os_logger.cpp, one bus lock per call. Each read is
// progress for the worker heartbeat.
struct ArchiveDirectoryIO {
  static DIR *open(const char *path) {
    BusLock lock;
    return ::opendir(path);
  }
  static const struct dirent *read(DIR *directory) {
    stamp_heartbeat(); // archive walks are progress, not a stall
    BusLock lock;
    errno = 0;
    return ::readdir(directory);
  }
  static int inspect(const char *path, struct stat *info) {
    BusLock lock;
    return ::stat(path, info);
  }
  static int close(DIR *directory) {
    BusLock lock;
    return ::closedir(directory);
  }
};

// One reader and one writer, the kCopyBackFiles budget of archive_files.h,
// with their own SHA-256 stream so the STAC writer's never changes hands.
// Every call takes the bus lock and each chunk read stamps the heartbeat.
struct CopyIO : ArchiveDirectoryIO {
  int reader = -1;
  int writer = -1;
  std::array<std::uint8_t, flash_copy::kChunkBytes> buffer{};
  opensensor::hash::Sha256Stream sha;

  static bool make_directories(const char *path) {
    return worker::make_directories(path);
  }
  bool open_read(const char *path) {
    BusLock lock;
    reader = ::open(path, O_RDONLY);
    return reader >= 0;
  }
  // A new file only, so a name that appeared meanwhile is never truncated.
  bool open_write(const char *path) {
    BusLock lock;
    writer = ::open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    return writer >= 0;
  }
  std::ptrdiff_t read(std::span<std::uint8_t> out) {
    stamp_heartbeat();
    BusLock lock;
    return ::read(reader, out.data(), out.size());
  }
  bool write(std::span<const std::uint8_t> bytes) {
    BusLock lock;
    while (!bytes.empty()) {
      const ssize_t wrote = ::write(writer, bytes.data(), bytes.size());
      if (wrote <= 0) {
        if (wrote == 0)
          errno = EIO;
        return false;
      }
      bytes = bytes.subspan(static_cast<std::size_t>(wrote));
    }
    return true;
  }
  bool sync() {
    stamp_heartbeat();
    BusLock lock;
    return ::fsync(writer) == 0;
  }
  bool close_read() {
    BusLock lock;
    const int closed = ::close(reader);
    reader = -1;
    return closed == 0;
  }
  bool close_write() {
    BusLock lock;
    const int closed = ::close(writer);
    writer = -1;
    return closed == 0;
  }
  std::span<std::uint8_t> scratch() { return buffer; }
  static int unlink(const char *path) {
    BusLock lock;
    return ::unlink(path);
  }
  // FatFs refuses a rename onto an existing name with EEXIST, so the plain
  // rename never replaces a card file.
  static int rename_no_replace(const char *from, const char *to) {
    BusLock lock;
    return ::rename(from, to);
  }
  std::expected<void, opensensor::hash::Sha256Error> hash_begin() {
    return sha.begin();
  }
  void hash_update(std::span<const std::uint8_t> bytes) { sha.update(bytes); }
  std::expected<opensensor::hash::Sha256Digest, opensensor::hash::Sha256Error>
  hash_finish() {
    return sha.finish();
  }
};

// The copy back state, about 11 KiB, in PSRAM so no task stack holds it.
struct FlashDrain {
  // Names that stay for the owner this boot: a conflict, a crosslink
  // suspect, a name the copier refused. `failed` holds names that failed
  // with an IO error and is cleared before the next pass over the root.
  static constexpr std::size_t kNames = 32;
  struct Names {
    std::array<std::uint32_t, kNames> hashes{};
    std::size_t count = 0;
    bool contains(std::uint32_t hash) const {
      return std::find(hashes.begin(), hashes.begin() + count, hash) !=
             hashes.begin() + count;
    }
    // False when full, the caller then waits a pass instead.
    bool add(std::uint32_t hash) {
      if (contains(hash))
        return true;
      if (count == hashes.size())
        return false;
      hashes[count++] = hash;
      return true;
    }
  };
  // The files of the last walk that copy next (#215). One walk serves up to
  // kCandidates copies.
  static constexpr std::size_t kCandidates = 16;
  CopyIO io;
  flash_copy::Copier copier;
  opensensor::logger::detail::DirectoryWalk<ArchiveDirectoryIO> walk;
  opensensor::logger::copy_candidates::List<kCandidates> candidates;
  std::array<char, paths::kRelativeBytes> best{};
  Names held;
  Names failed;
  std::uint32_t moved = 0;
  std::uint32_t left_logged = UINT32_MAX;
  std::int64_t next_at = 0; // uptime seconds of the next pass
  // A Parquet file moved since the walk, so a sidecar the walk saw waiting
  // may go now, ahead of the listed sidecars and quarantined files.
  bool parquet_moved = false;
};
FlashDrain *flash_drain = nullptr;
constexpr std::int64_t kCopyWaitSeconds = 60;

std::uint32_t name_hash(const char *name) {
  std::uint32_t hash = 2166136261U;
  for (; *name; ++name)
    hash = (hash ^ static_cast<std::uint8_t>(*name)) * 16777619U;
  return hash;
}

// Waits one pass. The pass after it walks the root again, since names may
// have changed meanwhile and a name a full list could not hold must come
// back.
void wait_a_pass(FlashDrain &drain, std::int64_t now) {
  drain.next_at = now + kCopyWaitSeconds;
  drain.candidates.clear();
}

// True when `root` holds "<prefix><stem>.parquet". Sets `unsure` when the
// path does not fit or the inspect fails for another reason than ENOENT.
bool parquet_on(const char *root, std::string_view prefix,
                std::string_view stem, bool &unsure) {
  char parquet[paths::kPathBytes];
  const int length =
      std::snprintf(parquet, sizeof(parquet), "%s/%.*s%.*s.parquet", root,
                    static_cast<int>(prefix.size()), prefix.data(),
                    static_cast<int>(stem.size()), stem.data());
  if (length < 0 || static_cast<std::size_t>(length) >= sizeof(parquet)) {
    unsure = true;
    return false;
  }
  struct stat info{};
  if (ArchiveDirectoryIO::inspect(parquet, &info) == 0)
    return true;
  unsure = unsure || errno != ENOENT;
  return false;
}

// True when the flash file `relative` holds the owner's data on its own:
// a sidecar waits until its Parquet left flash and is on the card. A set
// aside sidecar (#212) belongs to its Parquet under either name, since only
// the sidecar may have conflicted.
bool sidecar_waits(const char *relative) {
  constexpr std::string_view kJson = ".json";
  std::string_view stem{relative};
  stem.remove_suffix(kJson.size());
  const std::array<std::string_view, 2> prefixes{"",
                                                 flash_copy::kConflictPrefix};
  std::size_t names = 1;
  if (flash_copy::set_aside_name(relative)) {
    stem.remove_prefix(flash_copy::kConflictPrefixLength);
    names = 2;
  }
  bool unsure = false, on_card = false;
  for (std::size_t at = 0; at < names; ++at) {
    if (parquet_on(flash_root(), prefixes[at], stem, unsure))
      return true;
    on_card =
        parquet_on(worker::card_root(), prefixes[at], stem, unsure) || on_card;
  }
  return unsure || !on_card;
}

// Removes the empty directories above a freed flash file, up to the flash
// root. The root's quarantine folder stays for same-boot quarantine moves.
void remove_empty_flash_directories(const char *relative) {
  char path[paths::kPathBytes];
  const int length =
      std::snprintf(path, sizeof(path), "%s/%s", flash_root(), relative);
  if (length < 0 || static_cast<std::size_t>(length) >= sizeof(path))
    return;
  const std::size_t root_length = std::strlen(flash_root());
  for (;;) {
    char *slash = std::strrchr(path, '/');
    if (!slash || static_cast<std::size_t>(slash - path) <= root_length)
      return;
    *slash = '\0';
    if (std::strcmp(path + root_length, "/quarantine") == 0)
      return;
    BusLock lock;
    if (::rmdir(path) != 0)
      return; // not empty, or gone
  }
}

// Takes the first listed file that may still move into `best`. The checks
// of the walk run again, since a copy may have failed or the card changed.
// A list whose next entry is no Parquet file is dropped once a Parquet file
// moved after the walk, so the copy order stays the one of a walk per step.
bool take_listed(FlashDrain &drain) {
  auto &list = drain.candidates;
  while (const char *name = list.front()) {
    const std::uint32_t hash = name_hash(name);
    const auto kind = flash_copy::copy_kind(name);
    if (!kind || drain.held.contains(hash) || drain.failed.contains(hash)) {
      list.pop_front();
      continue;
    }
    if (*kind != flash_copy::Kind::Parquet && drain.parquet_moved) {
      list.clear();
      return false;
    }
    if (*kind == flash_copy::Kind::Sidecar && sidecar_waits(name)) {
      list.pop_front();
      continue;
    }
    std::strcpy(drain.best.data(), name);
    return true;
  }
  return false;
}

// Picks the next flash file. The list of the last walk serves first. When it
// is empty, one complete walk that closes every directory, so nothing stays
// open between passes, fills it again. False when no copy began.
bool copy_back_next() {
  FlashDrain &drain = *flash_drain;
  drain.best[0] = '\0';
  std::uint32_t files = 0;
  if (!take_listed(drain)) {
    drain.candidates.clear();
    drain.parquet_moved = false;
    auto visit = [&](const char *name, const struct stat &info) {
      if (!S_ISREG(info.st_mode))
        return true;
      ++files;
      const auto kind = flash_copy::copy_kind(name);
      if (!kind || std::strlen(name) >= drain.best.size())
        return true;
      const std::uint32_t hash = name_hash(name);
      if (drain.held.contains(hash) || drain.failed.contains(hash))
        return true;
      if (!drain.candidates.would_keep(name))
        return true;
      if (*kind == flash_copy::Kind::Sidecar && sidecar_waits(name))
        return true;
      drain.candidates.offer(name);
      return true;
    };
    using Walk = opensensor::logger::detail::DirectoryWalk<ArchiveDirectoryIO>;
    Walk::Status status = Walk::Status::Failed;
    if (drain.walk.begin(drain.io, flash_root())) {
      do
        status = drain.walk.step(64, visit);
      while (status == Walk::Status::More);
    }
    const std::int64_t now = uptime_seconds();
    if (status != Walk::Status::Done) {
      drain.walk.cancel();
      worker::count_error();
      oslog.printf("OS FLASH ERROR operation=copy-walk errno=%d\n", errno);
      wait_a_pass(drain, now);
      return false;
    }
    if (const char *first = drain.candidates.front())
      std::strcpy(drain.best.data(), first);
  }
  const std::int64_t now = uptime_seconds();
  if (!drain.best[0]) {
    drain.failed.count = 0;
    if (!files) {
      worker::flash_empty();
      drain.left_logged = UINT32_MAX;
      oslog.printf("OS FLASH copy done files=%lu\n",
                   static_cast<unsigned long>(drain.moved));
      drain.moved = 0;
      return false;
    }
    if (files != drain.left_logged) {
      drain.left_logged = files;
      oslog.printf("OS FLASH copy held files=%lu\n",
                   static_cast<unsigned long>(files));
    }
    wait_a_pass(drain, now);
    return false;
  }
  // The count goes up in NVS before a file moves, so a reset never brings
  // back an epoch from before the copy. Without it the file waits on flash,
  // where LIST still finds it.
  if (!worker::reserve_copy_count()) {
    wait_a_pass(drain, now);
    return false;
  }
  const auto begun =
      drain.copier.begin(flash_root(), worker::card_root(), drain.best.data());
  // The copier owns the name from here, or holds it after a refusal.
  drain.candidates.pop_front();
  if (!begun) {
    worker::count_error();
    oslog.printf("OS FLASH copy name=%s bytes=0 result=error reason=%s\n",
                 drain.best.data(),
                 begun.error() == flash_copy::BeginError::TooLong   ? "too-long"
                 : begun.error() == flash_copy::BeginError::BadName ? "bad-name"
                                                                    : "busy");
    if (!drain.held.add(name_hash(drain.best.data())))
      wait_a_pass(drain, now);
    return false;
  }
  return true;
}

} // namespace

void start() {
  void *memory = heap_caps_calloc(1, sizeof(FlashDrain),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!memory) {
    worker::count_error();
    oslog.printf("OS FLASH ERROR operation=allocate mem=psram bytes=%u "
                 "purpose=copy-back\n",
                 unsigned(sizeof(FlashDrain)));
    return;
  }
  flash_drain = new (memory) FlashDrain{};
}

bool allowed(bool storage_ready) {
  return flash_drain && worker::may_copy(storage_ready);
}

bool copying() { return flash_drain && flash_drain->copier.active(); }

void cancel(const char *reason) {
  if (!copying())
    return;
  oslog.printf("OS FLASH copy name=%s bytes=%llu result=cancelled reason=%s\n",
               flash_drain->copier.relative(),
               static_cast<unsigned long long>(flash_drain->copier.bytes()),
               reason);
  flash_drain->copier.cancel(flash_drain->io);
  // The file stays on flash and goes first again, after a new walk.
  flash_drain->candidates.clear();
}

void now() {
  if (flash_drain) {
    flash_drain->next_at = 0;
    flash_drain->failed.count = 0;
    flash_drain->candidates.clear();
  }
}

void flash_changed() {
  if (flash_drain)
    flash_drain->candidates.clear();
}

bool service(bool storage_ready) {
  if (!allowed(storage_ready)) {
    cancel("paused");
    return false;
  }
  FlashDrain &drain = *flash_drain;
  if (!drain.copier.active()) {
    if (uptime_seconds() < drain.next_at)
      return false;
    return copy_back_next();
  }
  const auto outcome = drain.copier.step(drain.io);
  if (outcome == flash_copy::Outcome::InProgress)
    return true;
  const char *relative = drain.copier.relative();
  const auto bytes = static_cast<unsigned long long>(drain.copier.bytes());
  const std::uint32_t hash = name_hash(relative);
  const std::int64_t now = uptime_seconds();
  switch (outcome) {
  case flash_copy::Outcome::Copied:
  case flash_copy::Outcome::AlreadyIdentical: {
    oslog.printf(
        "OS FLASH copy name=%s bytes=%llu result=%s\n", relative, bytes,
        outcome == flash_copy::Outcome::Copied ? "copied" : "identical");
    if (drain.moved != UINT32_MAX)
      ++drain.moved;
    const bool copied = outcome == flash_copy::Outcome::Copied;
    if (flash_copy::copy_kind(relative) == flash_copy::Kind::Parquet)
      drain.parquet_moved = true;
    // A LIST_SINCE cursor from before the move never reads as current.
    worker::file_moved(copied);
    remove_empty_flash_directories(relative);
    // A committed copy took card space, so the governor sees it now and not
    // at the next batch commit (#213).
    worker::measure_after_move(copied, now);
    break;
  }
  case flash_copy::Outcome::Conflict: {
    oslog.printf("OS FLASH copy name=%s bytes=%llu result=conflict\n", relative,
                 bytes);
    // Both files stay. The flash one moves to its set aside name, which the
    // card does not hold, so LIST names it and the next pass copies it (#212).
    const auto aside = drain.copier.set_aside(drain.io);
    if (aside) {
      oslog.printf("OS FLASH conflict name=%s moved=%s%s sidecar=%d\n",
                   relative, flash_copy::kConflictPrefix, relative,
                   *aside ? 1 : 0);
      remove_empty_flash_directories(relative);
      // The set aside names are new to the list.
      drain.candidates.clear();
      break;
    }
    oslog.printf("OS FLASH conflict name=%s moved=none reason=%s errno=%d\n",
                 relative, flash_copy::aside_error_name(aside.error()),
                 aside.error() == flash_copy::AsideError::AlreadyAside ? 0
                                                                       : errno);
    if (flash_copy::aside_retries(aside.error())) {
      if (!drain.failed.add(hash))
        wait_a_pass(drain, now);
    } else if (!drain.held.add(hash)) {
      wait_a_pass(drain, now);
    }
    break;
  }
  default: {
    worker::count_error();
    const auto failure =
        drain.copier.failure().value_or(flash_copy::Failure::Idle);
    oslog.printf("OS FLASH copy name=%s bytes=%llu result=error reason=%s "
                 "errno=%d\n",
                 relative, bytes, flash_copy::failure_name(failure),
                 drain.copier.error());
    // A card that reached Ready without a write test fails it here, so it
    // leaves Ready like after a failed commit (#218). The flash file stays.
    worker::copy_failed(failure, drain.copier.error(), now);
    if (failure == flash_copy::Failure::CrosslinkSuspect) {
      // Both card entries and the flash source stay for the owner, once
      // per file and boot.
      oslog.printf("PARQUET RECOVERY crosslink-suspect file=%s action=none\n",
                   relative);
      if (!drain.held.add(hash))
        wait_a_pass(drain, now);
    } else if (!drain.failed.add(hash)) {
      wait_a_pass(drain, now);
    }
    break;
  }
  }
  return true;
}

} // namespace oslogger::copy_back
