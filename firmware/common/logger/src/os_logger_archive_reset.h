#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace opensensor::logger::detail {

struct ArchiveResetIo {
#ifdef ESP_PLATFORM
  // These roots are exclusively on the board's mounted FAT volume. IDF 6.1.1
  // FatFs exposes only file/directory types and has no symbolic-link/lstat
  // implementation (components/fatfs/vfs/vfs_fat.c). Never reuse this default
  // on another filesystem. Host filesystems must inspect links themselves.
  int (*inspect)(const char *, struct stat *) = ::stat;
#else
  int (*inspect)(const char *, struct stat *) = ::lstat;
#endif
  DIR *(*open)(const char *) = ::opendir;
  dirent *(*next)(DIR *) = ::readdir;
  int (*close)(DIR *) = ::closedir;
  int (*remove_file)(const char *) = ::unlink;
  int (*remove_directory)(const char *) = ::rmdir;
  // Optional liveness callback, called once per visited entry.
  void (*progress)() = nullptr;
};

namespace archive_reset {
constexpr unsigned kMaximumDepth = 8;
constexpr std::uint32_t kMaximumEntries = 65536;
constexpr std::size_t kPathBytes = 512;

// One pass over the tree at `root`, root at depth 0. Only ordinary files and
// directories are allowed. Host lstat rejects links, and board FAT cannot
// contain them. With `remove` false it only checks the tree. With `remove`
// true it deletes each file when it is read and each directory right after
// its last entry was read and it was closed, so children always go before
// their directory. The walk is iterative. It keeps one open directory per
// level in a fixed array and builds every path in one buffer, so its stack
// does not grow with the tree. An entry deeper than kMaximumDepth, or more
// than kMaximumEntries entries, fail the pass with EOVERFLOW.
// The caller has stopped every producer and holds exclusive filesystem access.
inline bool walk(const char *root, bool remove, std::uint32_t &files,
                 std::uint32_t &directories, const ArchiveResetIo &io) {
  std::uint32_t entries = 0;
  auto count = [&] {
    if (++entries > kMaximumEntries) {
      errno = EOVERFLOW;
      return false;
    }
    if (io.progress)
      io.progress();
    return true;
  };
  char path[kPathBytes];
  const std::size_t root_length = std::strlen(root);
  if (root_length >= sizeof(path)) {
    errno = ENAMETOOLONG;
    return false;
  }
  std::memcpy(path, root, root_length + 1);
  if (!count())
    return false;
  struct stat info{};
  if (io.inspect(path, &info) != 0)
    return errno == ENOENT;
  if (S_ISREG(info.st_mode)) {
    errno = ENOTDIR;
    return false;
  }
  if (!S_ISDIR(info.st_mode)) {
    errno = EINVAL;
    return false;
  }
  // open[i] is the directory at depth i, and lengths[i] is the length of its
  // path in `path`.
  DIR *open[kMaximumDepth + 1]{};
  std::size_t lengths[kMaximumDepth + 1]{};
  unsigned levels = 0;
  // Closes every open directory and keeps the errno that ended the pass.
  auto fail = [&] {
    const int error = errno;
    while (levels)
      io.close(open[--levels]);
    errno = error;
    return false;
  };
  if (!(open[0] = io.open(path)))
    return false;
  lengths[0] = root_length;
  levels = 1;
  while (levels) {
    const std::size_t parent = lengths[levels - 1];
    path[parent] = '\0';
    errno = 0;
    const dirent *entry = io.next(open[levels - 1]);
    if (!entry) {
      if (errno != 0)
        return fail();
      if (io.close(open[--levels]) != 0)
        return fail();
      if (!remove)
        continue;
      if (directories == std::numeric_limits<std::uint32_t>::max()) {
        errno = EOVERFLOW;
        return fail();
      }
      if (io.remove_directory(path) != 0)
        return fail();
      ++directories;
      continue;
    }
    if (std::strcmp(entry->d_name, ".") == 0 ||
        std::strcmp(entry->d_name, "..") == 0)
      continue;
    if (!entry->d_name[0] || std::strchr(entry->d_name, '/')) {
      errno = EINVAL;
      return fail();
    }
    const int length = std::snprintf(path + parent, sizeof(path) - parent,
                                     "/%s", entry->d_name);
    if (length < 0 ||
        parent + static_cast<std::size_t>(length) >= sizeof(path)) {
      errno = ENAMETOOLONG;
      return fail();
    }
    // The entry sits one level below the directory it was read from.
    if (levels > kMaximumDepth) {
      errno = EOVERFLOW;
      return fail();
    }
    if (!count())
      return fail();
    if (io.inspect(path, &info) != 0)
      return fail();
    if (S_ISREG(info.st_mode)) {
      if (!remove)
        continue;
      if (files == std::numeric_limits<std::uint32_t>::max()) {
        errno = EOVERFLOW;
        return fail();
      }
      if (io.remove_file(path) != 0)
        return fail();
      ++files;
      continue;
    }
    if (!S_ISDIR(info.st_mode)) {
      errno = EINVAL;
      return fail();
    }
    if (!(open[levels] = io.open(path)))
      return fail();
    lengths[levels] = parent + static_cast<std::size_t>(length);
    ++levels;
  }
  return true;
}
} // namespace archive_reset

// An archive root must be an absolute path below a mount point, with no
// empty, "." or ".." segment and no trailing slash. "/sd" alone is rejected.
inline bool archive_root_valid(const char *root) {
  if (!root || root[0] != '/')
    return false;
  unsigned segments = 0;
  for (const char *segment = root + 1;;) {
    const char *end = std::strchr(segment, '/');
    const std::size_t length =
        end ? static_cast<std::size_t>(end - segment) : std::strlen(segment);
    if (length == 0 || (length == 1 && segment[0] == '.') ||
        (length == 2 && segment[0] == '.' && segment[1] == '.'))
      return false;
    ++segments;
    if (!end)
      break;
    segment = end + 1;
  }
  return segments >= 2 && std::strlen(root) < archive_reset::kPathBytes;
}

// Physical owner-authorized maintenance only. Never accept a caller-selected
// path, card root, radio command or a path derived from a file manifest.
// root must equal the configured output root or the optional configured
// legacy root. Counters accumulate across calls, including root dirs.
inline bool erase_archive(const char *root, const char *output_root,
                          const char *legacy_root, std::uint32_t &files_removed,
                          std::uint32_t &directories_removed,
                          const ArchiveResetIo &io = {}) {
  const bool output = archive_root_valid(output_root) && root &&
                      std::strcmp(root, output_root) == 0;
  const bool legacy = legacy_root && archive_root_valid(legacy_root) && root &&
                      std::strcmp(root, legacy_root) == 0;
  if (!output && !legacy) {
    errno = EINVAL;
    return false;
  }
  // Check the whole tree before deleting anything, then delete it.
  if (!archive_reset::walk(root, false, files_removed, directories_removed, io))
    return false;
  return archive_reset::walk(root, true, files_removed, directories_removed,
                             io);
}

} // namespace opensensor::logger::detail
