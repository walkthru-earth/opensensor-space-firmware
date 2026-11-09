#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <utility>

namespace opensensor::logger::detail {

using DirectoryStat = int (*)(const char *, struct stat *);
using DirectoryMkdir = int (*)(const char *, mode_t);

// The caller owns filesystem/bus serialization. FatFs rejects mkdir at its
// mounted root with EINVAL rather than EEXIST; inspect existing paths first.
inline bool ensure_directory(const char *path, DirectoryStat inspect = ::stat,
                             DirectoryMkdir create = ::mkdir) {
  struct stat info{};
  if (inspect(path, &info) == 0) {
    if (S_ISDIR(info.st_mode))
      return true;
    errno = ENOTDIR;
    return false;
  }
  if (errno != ENOENT)
    return false;
  if (create(path, 0700) == 0)
    return true;
  if (errno != EEXIST)
    return false;
  // Do not accept a regular file or a failed lookup after an EEXIST race.
  if (inspect(path, &info) != 0)
    return false;
  if (S_ISDIR(info.st_mode))
    return true;
  errno = ENOTDIR;
  return false;
}

// Resumable, allocation-free archive traversal. An explicit stack of open
// directories replaces recursion, so step() can visit a bounded number of
// entries and a later step() continues where it stopped. The order is the
// same pre-order, depth-first order a recursive walk produces.
// IO supplies open/read/inspect/close with any required bus locking. A null
// read is EOF only when errno is zero. FatFs otherwise reports an SD read
// failure through that same result. Any inaccessible entry fails the whole
// snapshot rather than publishing the visited prefix as complete.
template <class IO> class DirectoryWalk {
public:
  // Deepest directory level the walk opens, the archive root being level 0.
  static constexpr unsigned kMaxDepth = 6;
  // More: entries remain. Done: every directory was read and closed.
  // Failed: an IO call or a name check failed, errno says why. Stopped: the
  // visitor returned false. After any status but More nothing stays open.
  enum class Status : std::uint8_t { More, Done, Failed, Stopped };

  DirectoryWalk() = default;
  DirectoryWalk(const DirectoryWalk &) = delete;
  DirectoryWalk &operator=(const DirectoryWalk &) = delete;
  ~DirectoryWalk() { cancel(); }

  // Opens "<base>/<relative>" as a walk at `depth`. Drops any earlier walk.
  // `base` must outlive the walk. False with errno set when nothing opened.
  bool begin(IO &io, const char *base, const char *relative = "",
             unsigned depth = 0) {
    cancel();
    io_ = &io;
    base_ = base;
    if (depth > kMaxDepth) {
      errno = EOVERFLOW;
      return false;
    }
    const std::size_t length = std::strlen(relative);
    if (length >= sizeof(relative_)) {
      errno = ENAMETOOLONG;
      return false;
    }
    std::memcpy(relative_, relative, length + 1);
    first_depth_ = depth;
    return push();
  }

  // Reads at most `budget` directory entries (at least one) and passes each
  // file to visit(name, info), where name is relative to `base`.
  template <class Visitor> Status step(unsigned budget, Visitor &visit) {
    auto every = [](const char *) { return true; };
    return step(budget, visit, every);
  }

  // As above, but descends into a directory only when enter(name) is true,
  // with name relative to `base`. A skipped directory costs its one entry.
  template <class Visitor, class Enter>
  Status step(unsigned budget, Visitor &visit, Enter &enter) {
    auto never = [](const char *, int) { return false; };
    return step(budget, visit, enter, never);
  }

  // As above, but an entry whose path is too long, whose inspect fails or
  // whose directory cannot be opened goes to skip(name, error), with name
  // relative to `base` or the bare entry name when the path did not fit.
  // When skip returns true the walk moves on to the next entry, otherwise it
  // fails. A directory read or close error always fails the walk.
  template <class Visitor, class Enter, class Skip>
  Status step(unsigned budget, Visitor &visit, Enter &enter, Skip &skip) {
    if (!open_)
      return Status::Done;
    if (budget == 0)
      budget = 1;
    for (; budget; --budget) {
      Handle directory = handles_[open_ - 1];
      errno = 0;
      const auto *entry = io_->read(directory);
      if (!entry) {
        if (errno != 0)
          return fail(Status::Failed);
        --open_;
        if (io_->close(directory) != 0)
          return fail(Status::Failed);
        if (!open_)
          return Status::Done;
        relative_[lengths_[open_ - 1]] = '\0';
        continue;
      }
      if (entry->d_name[0] == '.')
        continue;
      const std::size_t parent = lengths_[open_ - 1];
      int length = std::snprintf(relative_ + parent, sizeof(relative_) - parent,
                                 "%s%s", parent ? "/" : "", entry->d_name);
      if (length < 0 ||
          parent + static_cast<std::size_t>(length) >= sizeof(relative_)) {
        relative_[parent] = '\0';
        if (skip(static_cast<const char *>(entry->d_name), ENAMETOOLONG))
          continue;
        errno = ENAMETOOLONG;
        return fail(Status::Failed);
      }
      // Hands the entry to skip(). True when the walk goes on past it.
      const auto skipped = [&](int error) {
        const bool next = skip(static_cast<const char *>(relative_), error);
        relative_[parent] = '\0';
        errno = error;
        return next;
      };
      char path[416];
      length = std::snprintf(path, sizeof(path), "%s/%s", base_, relative_);
      if (length < 0 || static_cast<std::size_t>(length) >= sizeof(path)) {
        if (skipped(ENAMETOOLONG))
          continue;
        return fail(Status::Failed);
      }
      struct stat info{};
      if (io_->inspect(path, &info) != 0) {
        if (skipped(errno))
          continue;
        return fail(Status::Failed);
      }
      if (S_ISDIR(info.st_mode)) {
        if (!enter(static_cast<const char *>(relative_))) {
          relative_[parent] = '\0';
          continue;
        }
        if (first_depth_ + open_ > kMaxDepth) {
          errno = EOVERFLOW;
          return fail(Status::Failed);
        }
        if (!push()) {
          if (skipped(errno))
            continue;
          return fail(Status::Failed);
        }
        continue;
      }
      const bool keep = visit(static_cast<const char *>(relative_), info);
      relative_[parent] = '\0';
      if (!keep)
        return fail(Status::Stopped);
    }
    return Status::More;
  }

  // Closes every open directory. False when a close failed.
  bool cancel() {
    bool closed = true;
    while (open_)
      if (io_->close(handles_[--open_]) != 0)
        closed = false;
    return closed;
  }

  bool active() const { return open_ != 0; }

private:
  using Handle = decltype(std::declval<IO &>().open(""));

  // Opens "<base>/<relative_>" as the new innermost level.
  bool push() {
    char path[416];
    const int length =
        std::snprintf(path, sizeof(path), "%s/%s", base_, relative_);
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(path)) {
      errno = ENAMETOOLONG;
      return false;
    }
    Handle directory = io_->open(path);
    if (!directory)
      return false;
    handles_[open_] = directory;
    lengths_[open_] = std::strlen(relative_);
    ++open_;
    return true;
  }

  // Closes everything, keeping the errno that ended the walk unless a close
  // reports one when none was set.
  Status fail(Status status) {
    const int failure = errno;
    if (!cancel() && status == Status::Stopped)
      status = Status::Failed;
    if (failure)
      errno = failure;
    return status;
  }

  IO *io_ = nullptr;
  const char *base_ = "";
  Handle handles_[kMaxDepth + 1]{};
  std::size_t lengths_[kMaxDepth + 1]{};
  unsigned open_ = 0;
  unsigned first_depth_ = 0;
  // Relative path of the innermost open directory, extended in place by the
  // entry being visited.
  char relative_[384]{};
};

// The whole walk in one call. True only when every entry was read.
template <class IO, class Visitor>
bool walk_directory(IO &io, const char *base, const char *relative,
                    unsigned depth, Visitor &visit) {
  DirectoryWalk<IO> walk;
  if (!walk.begin(io, base, relative, depth))
    return false;
  using Status = typename DirectoryWalk<IO>::Status;
  Status status;
  while ((status = walk.step(1024, visit)) == Status::More) {
  }
  return status == Status::Done;
}

inline bool has_suffix(const char *name, const char *suffix) {
  const std::size_t length = std::strlen(name), tail = std::strlen(suffix);
  return length > tail && std::strcmp(name + length - tail, suffix) == 0;
}

// Hands every ".partial" below `root` to move(relative), except those in the
// top level "quarantine" directory. move() may rename the file it is given.
// A directory deeper than DirectoryWalk::kMaxDepth is not entered. It goes
// to too_deep(relative) instead, so a deep tree is reported and never
// skipped silently. An entry that cannot be named, inspected or opened goes
// to unreadable(relative, error) and the walk moves on, so one damaged entry
// does not hide the partials after it. Only a directory read or close error
// ends the walk. True when every directory was read to its end.
template <class IO, class Move, class TooDeep, class Unreadable>
bool find_partials(IO &io, const char *root, Move &move, TooDeep &too_deep,
                   Unreadable &unreadable) {
  using Walk = DirectoryWalk<IO>;
  auto visit = [&](const char *name, const struct stat &) {
    if (has_suffix(name, ".partial"))
      move(name);
    return true;
  };
  auto enter = [&](const char *name) {
    if (std::strcmp(name, "quarantine") == 0)
      return false;
    unsigned depth = 1;
    for (const char *c = name; *c; ++c)
      depth += *c == '/' ? 1U : 0U;
    if (depth <= Walk::kMaxDepth)
      return true;
    too_deep(name);
    return false;
  };
  auto skip = [&](const char *name, int error) {
    unreadable(name, error);
    return true;
  };
  Walk walk;
  if (!walk.begin(io, root))
    return false;
  typename Walk::Status status;
  while ((status = walk.step(1024, visit, enter, skip)) == Walk::Status::More) {
  }
  return status == Walk::Status::Done;
}

// Quarantine keeps provenance. A ".partial" at `relative` below `root` moves
// to "<root>/quarantine/<thing>/<partition>/<name>-<boot>-<counter>.partial"
// where <thing> is the "thing=..." segment (or "no-thing") and the
// other directory segments are joined with '_' (or "root" when none). Both
// outputs are length checked; nothing is truncated.
inline bool quarantine_target(const char *root, const char *relative,
                              const char *boot, std::uint32_t counter,
                              char *directory, std::size_t directory_size,
                              char *path, std::size_t path_size) {
  if (!root || !relative || !boot || !directory || !path ||
      !has_suffix(relative, ".partial") || relative[0] == '/') {
    errno = EINVAL;
    return false;
  }
  const char *base = std::strrchr(relative, '/');
  base = base ? base + 1 : relative;
  char thing[96] = "no-thing";
  char partition[192] = "";
  std::size_t used = 0;
  for (const char *segment = relative; segment < base;) {
    const char *end = std::strchr(segment, '/');
    const std::size_t length = static_cast<std::size_t>(end - segment);
    if (length == 0 || (length == 1 && segment[0] == '.') ||
        (length == 2 && segment[0] == '.' && segment[1] == '.')) {
      errno = EINVAL;
      return false;
    }
    if (std::strncmp(segment, "thing=", 6) == 0 &&
        std::strcmp(thing, "no-thing") == 0) {
      if (length >= sizeof(thing)) {
        errno = ENAMETOOLONG;
        return false;
      }
      std::memcpy(thing, segment, length);
      thing[length] = '\0';
    } else {
      const std::size_t needed = used + (used ? 1 : 0) + length;
      if (needed >= sizeof(partition)) {
        errno = ENAMETOOLONG;
        return false;
      }
      if (used)
        partition[used++] = '_';
      std::memcpy(partition + used, segment, length);
      used += length;
      partition[used] = '\0';
    }
    segment = end + 1;
  }
  const int stem = static_cast<int>(std::strlen(base) - 8); // drop .partial
  int length = std::snprintf(directory, directory_size, "%s/quarantine/%s/%s",
                             root, thing, used ? partition : "root");
  if (length < 0 || static_cast<std::size_t>(length) >= directory_size) {
    errno = ENAMETOOLONG;
    return false;
  }
  length = std::snprintf(path, path_size, "%s/%.*s-%s-%08lx.partial", directory,
                         stem, base, boot, static_cast<unsigned long>(counter));
  if (length < 0 || static_cast<std::size_t>(length) >= path_size) {
    errno = ENAMETOOLONG;
    return false;
  }
  return true;
}

// The writer names "<P>-<attempt>.partial" and renames it to
// "<P>-<last>-<attempt>.parquet". A power cut inside the FAT rename can leave
// both entries sharing clusters, so such a pair is a crosslink suspect.
inline bool crosslink_pair(const char *partial, const char *parquet) {
  if (!has_suffix(partial, ".partial") || !has_suffix(parquet, ".parquet"))
    return false;
  const std::size_t partial_length = std::strlen(partial) - 8;
  const char *dash = nullptr;
  for (std::size_t i = partial_length; i-- > 0;)
    if (partial[i] == '-') {
      dash = partial + i;
      break;
    }
  if (!dash || dash == partial)
    return false;
  const std::size_t prefix = static_cast<std::size_t>(dash - partial);
  const char *attempt = dash + 1;
  const std::size_t attempt_length = partial_length - prefix - 1;
  if (attempt_length == 0)
    return false;
  for (std::size_t i = 0; i < attempt_length; ++i)
    if (attempt[i] < '0' || attempt[i] > '9')
      return false;
  // parquet = <prefix>-<last>-<attempt>.parquet
  if (std::strncmp(parquet, partial, prefix) != 0 || parquet[prefix] != '-')
    return false;
  const char *last = parquet + prefix + 1;
  const std::size_t rest = std::strlen(last) - 8; // drop .parquet
  if (rest < attempt_length + 2)
    return false;
  const char *tail = last + rest - attempt_length;
  if (std::strncmp(tail, attempt, attempt_length) != 0 || tail[-1] != '-')
    return false;
  const char *digit = last;
  if (*digit == '-')
    ++digit;
  if (digit >= tail - 1)
    return false;
  for (; digit < tail - 1; ++digit)
    if (*digit < '0' || *digit > '9')
      return false;
  return true;
}

// The STAC sidecar (#142) is written as "<F>.json.partial" and renamed to
// "<F>.json". A power cut inside that rename leaves the same kind of pair.
inline bool sidecar_crosslink_pair(const char *partial, const char *json) {
  if (!has_suffix(partial, ".json.partial") || !has_suffix(json, ".json"))
    return false;
  const std::size_t length = std::strlen(partial) - 8; // drop .partial
  return std::strlen(json) == length &&
         std::strncmp(partial, json, length) == 0;
}

// True when `directory` holds the finalized sibling of `partial`, or when the
// directory cannot be read completely. Callers leave suspects untouched.
template <class IO>
bool crosslink_suspect(IO &io, const char *directory, const char *partial) {
  auto *handle = io.open(directory);
  if (!handle)
    return true;
  bool suspect = false;
  for (;;) {
    errno = 0;
    const auto *entry = io.read(handle);
    if (!entry) {
      suspect = errno != 0;
      break;
    }
    if (crosslink_pair(partial, entry->d_name) ||
        sidecar_crosslink_pair(partial, entry->d_name)) {
      suspect = true;
      break;
    }
  }
  if (io.close(handle) != 0)
    suspect = true;
  return suspect;
}

// Moves one ".partial" into its provenance quarantine directory. IO supplies
// make_directories(path), exists(path) and rename(from, to) with any bus
// locking. An existing destination is never replaced. Nothing is deleted.
template <class IO>
bool quarantine_partial(IO &io, const char *root, const char *relative,
                        const char *boot, std::uint32_t counter,
                        char *destination, std::size_t destination_size) {
  char directory[416], source[416];
  if (!quarantine_target(root, relative, boot, counter, directory,
                         sizeof(directory), destination, destination_size))
    return false;
  const int length =
      std::snprintf(source, sizeof(source), "%s/%s", root, relative);
  if (length < 0 || static_cast<std::size_t>(length) >= sizeof(source)) {
    errno = ENAMETOOLONG;
    return false;
  }
  if (!io.make_directories(directory))
    return false;
  if (io.exists(destination)) {
    errno = EEXIST;
    return false;
  }
  return io.rename(source, destination) == 0;
}

} // namespace opensensor::logger::detail
