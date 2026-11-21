#pragma once

#include "sha256_stream.h"
#include "sync_codec.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <sys/stat.h>

// Copy back of one flash archive file to the card (decision record 0004,
// items 8 and 9). The storage worker owns a Copier and calls step() once per
// pass while the card is Ready, so a copy never holds the worker for more
// than one chunk. Each step reads at most kChunkBytes.
//
// One file moves as follows. The copy goes to "<final>.copy" on the card, is
// synced and closed, then read back. Its size, CRC32 and SHA-256 must match
// what was read from flash. Only then is it renamed to the final name, which
// must not exist, and only then is the flash file removed. A final name that
// already exists is compared byte for byte by hash. Identical frees the flash
// file, anything else is a conflict and both files stay.
//
// The only files this ever deletes are the flash source after an identical
// verified copy is on the card, and a temporary "<final>.copy" on the card
// while its source is still on flash and the final name is absent. A
// temporary whose source is gone stays where it is. A temporary next to a
// final file is a crosslink suspect, since a power cut inside the FAT rename
// can leave both entries sharing one cluster chain (os_logger_directory.h).
// Removing either would free the clusters of the other, so such a pair stays
// as it is, with its source, and the copy ends with CrosslinkSuspect. Nothing
// is overwritten, the final name is never replaced.
//
// The Copier is a plain value. File handles, the scratch buffer and the hash
// state belong to the IO, which provides
//   int inspect(const char *path, struct stat *info)   // 0, or -1 and errno
//   bool make_directories(const char *path)
//   bool open_read(const char *path)                   // the one reader
//   bool open_write(const char *path)                  // the one writer, new
//   std::ptrdiff_t read(std::span<std::uint8_t> out)   // 0 at EOF, -1 error
//   bool write(std::span<const std::uint8_t> bytes)
//   bool sync()                                        // the writer
//   bool close_read()
//   bool close_write()
//   std::span<std::uint8_t> scratch()
//   int unlink(const char *path)                       // 0, or -1 and errno
//   int rename_no_replace(const char *from, const char *to) // EEXIST if taken
//   std::expected<void, hash::Sha256Error> hash_begin()
//   void hash_update(std::span<const std::uint8_t> bytes)
//   std::expected<hash::Sha256Digest, hash::Sha256Error> hash_finish()
// At most one reader and one writer are open at once, which is the copy back
// budget of two files (archive_files.h, kCopyBackFiles).
namespace opensensor::logger::flash_copy {

inline constexpr std::size_t kChunkBytes = 4096;
// Same bounds as the archive walk, a full path and a relative name.
inline constexpr std::size_t kPathBytes = 416;
inline constexpr std::size_t kRelativeBytes = 384;
inline constexpr char kTempSuffix[] = ".copy";

enum class [[nodiscard]] Outcome : std::uint8_t {
  InProgress,       // call step() again on a later pass
  Copied,           // the card holds a verified copy, the flash file is gone
  AlreadyIdentical, // the card already held the same bytes, flash file gone
  Conflict,         // the card holds other bytes under the name, both stay
  IoError,          // see failure() and error(), the flash file stays
};

// Why a copy ended with IoError. Error only, for the log line.
enum class [[nodiscard]] Failure : std::uint8_t {
  Idle,          // step() without a copy in progress
  Scratch,       // the IO has no scratch buffer
  SourceMissing, // the flash file is not there
  SourceStat,    // the flash file could not be inspected or is not a file
  SourceOpen,
  SourceRead,
  SourceSize, // the flash file read back a different size than inspected
  TempRemove, // a stale temporary could not be removed
  CardStat,   // the final name could not be inspected
  Directory,  // the card directory could not be created
  TempOpen,
  TempWrite,
  TempSync,
  TempClose,
  TempRead,
  CardRead, // an existing final file could not be read for comparison
  Hash,
  Verify,       // the readback differs from what was read from flash
  Rename,       // the final name was refused, EEXIST when it appeared meanwhile
  SourceUnlink, // copied, but the flash file could not be removed yet
  // "<final>.copy" and "<final>" both exist, possibly sharing clusters after
  // a power cut inside the rename. Nothing was deleted, the source stays.
  CrosslinkSuspect,
};

[[nodiscard]] constexpr const char *failure_name(Failure failure) {
  switch (failure) {
  case Failure::Idle:
    return "idle";
  case Failure::Scratch:
    return "scratch";
  case Failure::SourceMissing:
    return "source-missing";
  case Failure::SourceStat:
    return "source-stat";
  case Failure::SourceOpen:
    return "source-open";
  case Failure::SourceRead:
    return "source-read";
  case Failure::SourceSize:
    return "source-size";
  case Failure::TempRemove:
    return "temp-remove";
  case Failure::CardStat:
    return "card-stat";
  case Failure::Directory:
    return "directory";
  case Failure::TempOpen:
    return "temp-open";
  case Failure::TempWrite:
    return "temp-write";
  case Failure::TempSync:
    return "temp-sync";
  case Failure::TempClose:
    return "temp-close";
  case Failure::TempRead:
    return "temp-read";
  case Failure::CardRead:
    return "card-read";
  case Failure::Hash:
    return "hash";
  case Failure::Verify:
    return "verify";
  case Failure::Rename:
    return "rename";
  case Failure::SourceUnlink:
    return "source-unlink";
  case Failure::CrosslinkSuspect:
    return "crosslink-suspect";
  }
  return "unknown";
}

enum class [[nodiscard]] BeginError : std::uint8_t {
  Busy,    // a copy is in progress
  BadName, // a missing root, or a relative name that is not a plain path
  TooLong, // a path, the temporary included, would not fit kPathBytes
};

namespace detail {
[[nodiscard]] constexpr bool ends_with(const char *name, std::size_t length,
                                       const char *suffix) {
  const std::size_t tail = std::char_traits<char>::length(suffix);
  return length >= tail && std::char_traits<char>::compare(name + length - tail,
                                                           suffix, tail) == 0;
}

// A relative name of plain components, no empty, "." or ".." component, so
// it cannot leave either root.
[[nodiscard]] constexpr bool plain_relative(const char *name,
                                            std::size_t length) {
  std::size_t start = 0;
  for (std::size_t at = 0; at <= length; ++at) {
    if (at < length && name[at] != '/')
      continue;
    const std::size_t part = at - start;
    if (part == 0 || (part == 1 && name[start] == '.') ||
        (part == 2 && name[start] == '.' && name[start + 1] == '.'))
      return false;
    start = at + 1;
  }
  return true;
}
} // namespace detail

// The kind of a relative name on the flash root, nullopt for anything that is
// not copied (a file still being written, a temporary, anything else).
enum class Kind : std::uint8_t { Parquet, Sidecar, Quarantined };

[[nodiscard]] constexpr std::optional<Kind> copy_kind(const char *relative) {
  if (relative == nullptr || *relative == '\0')
    return std::nullopt;
  const std::size_t length = std::char_traits<char>::length(relative);
  if (detail::ends_with(relative, length, kTempSuffix))
    return std::nullopt;
  constexpr char kQuarantine[] = "quarantine/";
  constexpr std::size_t kQuarantineLength = sizeof(kQuarantine) - 1;
  if (length > kQuarantineLength &&
      std::char_traits<char>::compare(relative, kQuarantine,
                                      kQuarantineLength) == 0)
    return Kind::Quarantined;
  if (detail::ends_with(relative, length, ".parquet"))
    return Kind::Parquet;
  if (detail::ends_with(relative, length, ".json"))
    return Kind::Sidecar;
  return std::nullopt;
}

// Copy order. A Parquet file goes before any sidecar ("<stem>.json"), so a
// sidecar never reaches the card ahead of the file it describes. Quarantined
// files go after both, names that are not copied go last. Ties sort by name.
[[nodiscard]] constexpr bool copy_before(const char *a, const char *b) {
  const auto rank = [](const char *name) {
    const auto kind = copy_kind(name);
    return kind ? static_cast<int>(*kind) : 3;
  };
  const int left = rank(a), right = rank(b);
  if (left != right)
    return left < right;
  if (a == nullptr || b == nullptr)
    return a != nullptr; // a missing name sorts last
  return std::char_traits<char>::compare(
             a, b,
             std::min(std::char_traits<char>::length(a),
                      std::char_traits<char>::length(b)) +
                 1) < 0;
}

class Copier {
public:
  // Starts a copy of <flash_root>/<relative> to <card_root>/<relative>. A
  // quarantined file keeps its relative path, so it lands in the card's
  // quarantine. The roots are copied into the paths here.
  [[nodiscard]] std::expected<void, BeginError>
  begin(const char *flash_root, const char *card_root, const char *relative) {
    if (stage_ != Stage::Idle)
      return std::unexpected(BeginError::Busy);
    if (flash_root == nullptr || *flash_root == '\0' || card_root == nullptr ||
        *card_root == '\0' || relative == nullptr || *relative == '\0')
      return std::unexpected(BeginError::BadName);
    const std::size_t length = ::strnlen(relative, kRelativeBytes);
    if (length >= kRelativeBytes)
      return std::unexpected(BeginError::TooLong);
    if (!detail::plain_relative(relative, length) ||
        detail::ends_with(relative, length, kTempSuffix))
      return std::unexpected(BeginError::BadName);
    const int source = std::snprintf(source_.data(), source_.size(), "%s/%s",
                                     flash_root, relative);
    const int final_length = std::snprintf(final_.data(), final_.size(),
                                           "%s/%s", card_root, relative);
    const int temp = std::snprintf(temp_.data(), temp_.size(), "%s/%s%s",
                                   card_root, relative, kTempSuffix);
    if (source < 0 || final_length < 0 || temp < 0 ||
        static_cast<std::size_t>(source) >= source_.size() ||
        static_cast<std::size_t>(temp) >= temp_.size()) {
      source_[0] = final_[0] = temp_[0] = '\0';
      relative_at_ = 0;
      return std::unexpected(BeginError::TooLong);
    }
    relative_at_ = static_cast<std::size_t>(source) - length;
    reset();
    stage_ = Stage::Start;
    return {};
  }

  // One bounded step. InProgress until the copy ends. Any other outcome
  // leaves the Copier idle with every file closed.
  template <class IO> Outcome step(IO &io) {
    switch (stage_) {
    case Stage::Idle:
      failure_ = Failure::Idle;
      error_ = 0;
      return Outcome::IoError;
    case Stage::Start:
      return start(io);
    case Stage::Copy:
      return copy(io);
    case Stage::Seal:
      return seal(io);
    case Stage::Verify:
      return verify(io);
    case Stage::Commit:
      return commit(io);
    case Stage::HashSource:
      return hash_source(io);
    case Stage::HashFinal:
      return hash_final(io);
    }
    return fail(io, Failure::Idle, 0);
  }

  // Stops a copy in progress, for example when the card leaves Ready. Closes
  // what is open and removes this attempt's temporary while its source is
  // still on flash. Does nothing when idle.
  template <class IO> void cancel(IO &io) {
    release(io);
    stage_ = Stage::Idle;
  }

  [[nodiscard]] bool active() const { return stage_ != Stage::Idle; }
  // The relative name of the current or last copy.
  [[nodiscard]] const char *relative() const {
    return source_.data() + relative_at_;
  }
  [[nodiscard]] std::optional<Failure> failure() const { return failure_; }
  // errno of the failed call, 0 when the failure is not a call.
  [[nodiscard]] int error() const { return error_; }
  // Bytes read from flash, and their CRC32 once the source is read through.
  [[nodiscard]] std::uint64_t bytes() const { return bytes_; }
  [[nodiscard]] std::uint32_t crc() const { return crc_; }

private:
  enum class Stage : std::uint8_t {
    Idle,
    Start,
    Copy,       // flash to "<final>.copy", one chunk per step
    Seal,       // sync and close the temporary, open it to read back
    Verify,     // read the temporary back, one chunk per step
    Commit,     // rename without replace, then free the flash file
    HashSource, // a final name exists with the same size, hash the source
    HashFinal,  // then hash the final file and compare
  };

  void reset() {
    reading_ = writing_ = own_temp_ = false;
    size_ = bytes_ = check_bytes_ = 0;
    crc_ = 0;
    running_ = check_ = 0xffffffffU;
    digest_ = {};
    failure_.reset();
    error_ = 0;
  }

  template <class IO> std::span<std::uint8_t> chunk(IO &io) {
    const std::span<std::uint8_t> scratch = io.scratch();
    return scratch.first(std::min(scratch.size(), kChunkBytes));
  }

  template <class IO> bool close_reader(IO &io) {
    reading_ = false;
    return io.close_read();
  }

  // Closes what is open. This attempt's temporary goes only while its source
  // is still on flash, so a copy is never the last one of its rows, and only
  // while the final name is confirmed absent. A rename that failed may still
  // have left a final entry sharing the temporary's clusters, and unlinking
  // the temporary would then free them under the final file.
  template <class IO> void release(IO &io) {
    if (reading_)
      (void)close_reader(io);
    if (writing_) {
      writing_ = false;
      (void)io.close_write();
    }
    if (own_temp_) {
      own_temp_ = false;
      struct stat info{};
      if (io.inspect(source_.data(), &info) == 0 &&
          io.inspect(final_.data(), &info) != 0 && errno == ENOENT)
        (void)io.unlink(temp_.data());
    }
  }

  template <class IO> Outcome fail(IO &io, Failure failure, int error) {
    failure_ = failure;
    error_ = error;
    release(io);
    stage_ = Stage::Idle;
    return Outcome::IoError;
  }

  template <class IO> Outcome end(IO &io, Outcome outcome) {
    release(io);
    stage_ = Stage::Idle;
    return outcome;
  }

  template <class IO> bool hash_begin(IO &io, std::uint32_t &crc) {
    crc = 0xffffffffU;
    return io.hash_begin().has_value();
  }

  template <class IO> Outcome start(IO &io) {
    if (chunk(io).empty())
      return fail(io, Failure::Scratch, 0);
    struct stat source{};
    if (io.inspect(source_.data(), &source) != 0) {
      const int error = errno;
      return fail(
          io, error == ENOENT ? Failure::SourceMissing : Failure::SourceStat,
          error);
    }
    if (!S_ISREG(source.st_mode) || source.st_size < 0)
      return fail(io, Failure::SourceStat, 0);
    size_ = static_cast<std::uint64_t>(source.st_size);
    // The final name is inspected before the temporary is touched.
    struct stat target{};
    const bool final_exists = io.inspect(final_.data(), &target) == 0;
    if (!final_exists && errno != ENOENT)
      return fail(io, Failure::CardStat, errno);
    if (final_exists) {
      // A temporary next to the final file may share its clusters. Leave
      // both, and the source, for the owner.
      struct stat temp{};
      if (io.inspect(temp_.data(), &temp) == 0)
        return fail(io, Failure::CrosslinkSuspect, 0);
      if (errno != ENOENT)
        return fail(io, Failure::CardStat, errno);
      if (!S_ISREG(target.st_mode) ||
          static_cast<std::uint64_t>(target.st_size) != size_)
        return end(io, Outcome::Conflict);
      if (!io.open_read(source_.data()))
        return fail(io, Failure::SourceOpen, errno);
      reading_ = true;
      if (!hash_begin(io, running_))
        return fail(io, Failure::Hash, 0);
      stage_ = Stage::HashSource;
      return Outcome::InProgress;
    }
    // A temporary left by a reset or a failed attempt, with no final name
    // next to it. Its source is on flash, checked above, so it is only ever
    // a partial copy.
    if (io.unlink(temp_.data()) != 0 && errno != ENOENT)
      return fail(io, Failure::TempRemove, errno);
    // The parent of the final name, written in place for the call.
    char *const slash = std::strrchr(final_.data(), '/');
    *slash = '\0';
    const bool made = io.make_directories(final_.data());
    const int made_error = errno;
    *slash = '/';
    if (!made)
      return fail(io, Failure::Directory, made_error);
    if (!io.open_read(source_.data()))
      return fail(io, Failure::SourceOpen, errno);
    reading_ = true;
    if (!io.open_write(temp_.data()))
      return fail(io, Failure::TempOpen, errno);
    writing_ = own_temp_ = true;
    if (!hash_begin(io, running_))
      return fail(io, Failure::Hash, 0);
    stage_ = Stage::Copy;
    return Outcome::InProgress;
  }

  // Ends the source read. Its size must be what start() inspected.
  template <class IO> std::optional<Failure> finish_source(IO &io) {
    if (!close_reader(io))
      return Failure::SourceRead;
    if (bytes_ != size_)
      return Failure::SourceSize;
    const auto digest = io.hash_finish();
    if (!digest)
      return Failure::Hash;
    digest_ = *digest;
    crc_ = running_ ^ 0xffffffffU;
    return std::nullopt;
  }

  // Ends a second read and says whether it matched the source.
  template <class IO> std::expected<bool, Failure> finish_check(IO &io) {
    const auto digest = io.hash_finish();
    if (!digest)
      return std::unexpected(Failure::Hash);
    return check_bytes_ == bytes_ && (check_ ^ 0xffffffffU) == crc_ &&
           *digest == digest_;
  }

  template <class IO> Outcome copy(IO &io) {
    const std::span<std::uint8_t> buffer = chunk(io);
    const std::ptrdiff_t got = io.read(buffer);
    if (got < 0 || static_cast<std::size_t>(got) > buffer.size())
      return fail(io, Failure::SourceRead, errno);
    if (got == 0) {
      if (const auto failure = finish_source(io))
        return fail(io, *failure, *failure == Failure::SourceRead ? errno : 0);
      stage_ = Stage::Seal;
      return Outcome::InProgress;
    }
    const auto part = buffer.first(static_cast<std::size_t>(got));
    running_ = sync::crc32_update(running_, part.data(), part.size());
    io.hash_update(part);
    bytes_ += part.size();
    if (!io.write(part))
      return fail(io, Failure::TempWrite, errno);
    return Outcome::InProgress;
  }

  template <class IO> Outcome seal(IO &io) {
    if (!io.sync())
      return fail(io, Failure::TempSync, errno);
    writing_ = false;
    if (!io.close_write())
      return fail(io, Failure::TempClose, errno);
    if (!io.open_read(temp_.data()))
      return fail(io, Failure::TempRead, errno);
    reading_ = true;
    check_bytes_ = 0;
    if (!hash_begin(io, check_))
      return fail(io, Failure::Hash, 0);
    stage_ = Stage::Verify;
    return Outcome::InProgress;
  }

  // Reads one chunk of a second file into the check sums. Returns the bytes
  // read, 0 at EOF, -1 on a read error.
  template <class IO>
  std::ptrdiff_t check_chunk(IO &io, std::span<std::uint8_t> buffer) {
    const std::ptrdiff_t got = io.read(buffer);
    if (got <= 0 || static_cast<std::size_t>(got) > buffer.size())
      return got > 0 ? -1 : got;
    const auto part = buffer.first(static_cast<std::size_t>(got));
    check_ = sync::crc32_update(check_, part.data(), part.size());
    io.hash_update(part);
    check_bytes_ += part.size();
    return got;
  }

  template <class IO> Outcome verify(IO &io) {
    const std::ptrdiff_t got = check_chunk(io, chunk(io));
    if (got < 0)
      return fail(io, Failure::TempRead, errno);
    if (got > 0)
      return check_bytes_ > bytes_ ? fail(io, Failure::Verify, 0)
                                   : Outcome::InProgress;
    if (!close_reader(io))
      return fail(io, Failure::TempRead, errno);
    const auto same = finish_check(io);
    if (!same)
      return fail(io, same.error(), 0);
    if (!*same)
      return fail(io, Failure::Verify, 0);
    stage_ = Stage::Commit;
    return Outcome::InProgress;
  }

  template <class IO> Outcome commit(IO &io) {
    if (io.rename_no_replace(temp_.data(), final_.data()) != 0)
      return fail(io, Failure::Rename, errno);
    own_temp_ = false;
    return free_source(io, Outcome::Copied);
  }

  // The card holds the bytes, so the flash file may go. When it cannot, the
  // next attempt finds the identical final file and frees it then.
  template <class IO> Outcome free_source(IO &io, Outcome outcome) {
    if (io.unlink(source_.data()) != 0)
      return fail(io, Failure::SourceUnlink, errno);
    return end(io, outcome);
  }

  template <class IO> Outcome hash_source(IO &io) {
    const std::span<std::uint8_t> buffer = chunk(io);
    const std::ptrdiff_t got = io.read(buffer);
    if (got < 0 || static_cast<std::size_t>(got) > buffer.size())
      return fail(io, Failure::SourceRead, errno);
    if (got > 0) {
      const auto part = buffer.first(static_cast<std::size_t>(got));
      running_ = sync::crc32_update(running_, part.data(), part.size());
      io.hash_update(part);
      bytes_ += part.size();
      return Outcome::InProgress;
    }
    if (const auto failure = finish_source(io))
      return fail(io, *failure, *failure == Failure::SourceRead ? errno : 0);
    if (!io.open_read(final_.data()))
      return fail(io, Failure::CardRead, errno);
    reading_ = true;
    check_bytes_ = 0;
    if (!hash_begin(io, check_))
      return fail(io, Failure::Hash, 0);
    stage_ = Stage::HashFinal;
    return Outcome::InProgress;
  }

  template <class IO> Outcome hash_final(IO &io) {
    const std::ptrdiff_t got = check_chunk(io, chunk(io));
    if (got < 0)
      return fail(io, Failure::CardRead, errno);
    if (got > 0)
      return check_bytes_ > bytes_ ? end(io, Outcome::Conflict)
                                   : Outcome::InProgress;
    if (!close_reader(io))
      return fail(io, Failure::CardRead, errno);
    const auto same = finish_check(io);
    if (!same)
      return fail(io, same.error(), 0);
    if (!*same)
      return end(io, Outcome::Conflict);
    return free_source(io, Outcome::AlreadyIdentical);
  }

  std::array<char, kPathBytes> source_{}; // <flash_root>/<relative>
  std::array<char, kPathBytes> final_{};  // <card_root>/<relative>
  std::array<char, kPathBytes> temp_{};   // <card_root>/<relative>.copy
  std::size_t relative_at_ = 0;           // the relative name in source_
  hash::Sha256Digest digest_{};           // of the source
  std::uint64_t size_ = 0;                // the source as inspected
  std::uint64_t bytes_ = 0;               // read from the source
  std::uint64_t check_bytes_ = 0;         // read from the temporary or final
  std::uint32_t running_ = 0xffffffffU;   // CRC32 state of the source
  std::uint32_t check_ = 0xffffffffU;     // CRC32 state of the second read
  std::uint32_t crc_ = 0;                 // finished CRC32 of the source
  int error_ = 0;
  std::optional<Failure> failure_;
  Stage stage_ = Stage::Idle;
  bool reading_ = false;  // the IO reader is open for this copy
  bool writing_ = false;  // the IO writer is open for this copy
  bool own_temp_ = false; // this attempt created the temporary
};

} // namespace opensensor::logger::flash_copy
