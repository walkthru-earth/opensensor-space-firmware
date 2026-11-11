#include "os_logger_archive_reset.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
fs::path card;
std::string mapped(const char *path) {
  assert(std::strncmp(path, "/sd/", 4) == 0);
  return (card / (path + 4)).string();
}
int inspect(const char *path, struct stat *info) {
  return ::lstat(mapped(path).c_str(), info);
}
DIR *open(const char *path) { return ::opendir(mapped(path).c_str()); }
int remove_file(const char *path) { return ::unlink(mapped(path).c_str()); }
int remove_directory(const char *path) { return ::rmdir(mapped(path).c_str()); }
const opensensor::logger::detail::ArchiveResetIo io{
    inspect, open, ::readdir, ::closedir, remove_file, remove_directory};
void write(const fs::path &path) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path);
  stream << "retained evidence";
  assert(stream.good());
}
dirent *read_failure(DIR *) {
  errno = EIO;
  return nullptr;
}
int delete_failure(const char *) {
  errno = EACCES;
  return -1;
}
unsigned progress_calls = 0;
void count_progress() { ++progress_calls; }

// Records every removal in order, to check children go before their parent.
std::vector<std::string> removed;
int record_file(const char *path) {
  removed.emplace_back(path);
  return remove_file(path);
}
int record_directory(const char *path) {
  removed.emplace_back(std::string(path) + "/");
  return remove_directory(path);
}

// Like FAT, rmdir refuses a directory that still has an open handle.
std::vector<std::pair<DIR *, std::string>> open_directories;
DIR *tracked_open(const char *path) {
  DIR *directory = open(path);
  if (directory)
    open_directories.emplace_back(directory, path);
  return directory;
}
int tracked_close(DIR *directory) {
  for (auto it = open_directories.begin(); it != open_directories.end(); ++it)
    if (it->first == directory) {
      open_directories.erase(it);
      return ::closedir(directory);
    }
  assert(false);
  return -1;
}
int busy_rmdir(const char *path) {
  for (const auto &entry : open_directories)
    if (entry.second == path) {
      errno = EBUSY;
      return -1;
    }
  return remove_directory(path);
}

// A virtual "/sd/flat" holding `flat_files` files, for the entry cap without
// creating tens of thousands of real files.
std::uint32_t flat_files = 0, flat_read = 0, flat_removed = 0;
unsigned flat_open = 0;
int flat_handle = 0;
dirent flat_entry{};
int flat_inspect(const char *path, struct stat *info) {
  *info = {};
  info->st_mode = std::strcmp(path, "/sd/flat") == 0 ? S_IFDIR : S_IFREG;
  return 0;
}
DIR *flat_opendir(const char *path) {
  assert(std::strcmp(path, "/sd/flat") == 0);
  ++flat_open;
  flat_read = 0;
  return reinterpret_cast<DIR *>(&flat_handle);
}
dirent *flat_next(DIR *) {
  if (flat_read == flat_files)
    return nullptr;
  std::snprintf(flat_entry.d_name, sizeof(flat_entry.d_name), "f%u",
                static_cast<unsigned>(flat_read++));
  return &flat_entry;
}
int flat_closedir(DIR *) {
  --flat_open;
  return 0;
}
int flat_unlink(const char *) {
  ++flat_removed;
  return 0;
}
int flat_rmdir(const char *) { return 0; }
const opensensor::logger::detail::ArchiveResetIo flat_io{
    flat_inspect, flat_opendir, flat_next, flat_closedir,
    flat_unlink,  flat_rmdir,   nullptr};

// The root counts as one entry, so kMaximumEntries - 1 files fit and one
// more is refused in the check pass with nothing deleted.
// The depth cap is exact. kMaximumDepth levels below the root are erased,
// one more entry fails the check pass with nothing gone, and every removal
// comes before the directory that holds it.
void check_depth_cap() {
  namespace fs = std::filesystem;
  using opensensor::logger::detail::archive_reset::kMaximumDepth;
  std::uint32_t files = 0, directories = 0;
  const auto erase = [&](const char *root,
                         const opensensor::logger::detail::ArchiveResetIo &x) {
    return opensensor::logger::detail::erase_archive(root, root, nullptr, files,
                                                     directories, x);
  };
  fs::path deep = card / "caps";
  for (unsigned i = 0; i + 1 < kMaximumDepth; ++i)
    deep /= "d";
  write(deep / "at-cap.parquet");
  write(deep / "d/past-cap.parquet");
  assert(!erase("/sd/caps", io));
  assert(errno == EOVERFLOW && files == 0 && directories == 0);
  assert(fs::exists(deep / "at-cap.parquet"));
  fs::remove(deep / "d/past-cap.parquet");
  auto record_io = io;
  record_io.remove_file = record_file;
  record_io.remove_directory = record_directory;
  assert(erase("/sd/caps", record_io));
  assert(files == 1 && directories == kMaximumDepth + 1);
  assert(!fs::exists(card / "caps"));
  // Post order. No removal lies below a directory removed before it.
  assert(removed.size() == kMaximumDepth + 2);
  for (std::size_t i = 0; i < removed.size(); ++i)
    for (std::size_t j = i + 1; j < removed.size(); ++j)
      assert(removed[j].rfind(removed[i], 0) != 0);
  assert(removed.back() == "/sd/caps/");
}

// Each directory is closed before it is removed, so a FAT card that refuses
// rmdir on an open directory still erases the whole tree.
void check_close_before_rmdir() {
  std::uint32_t files = 0, directories = 0;
  write(card / "busy/thing=a/day/x.parquet");
  write(card / "busy/thing=b/y.parquet");
  auto busy_io = io;
  busy_io.open = tracked_open;
  busy_io.close = tracked_close;
  busy_io.remove_directory = busy_rmdir;
  assert(opensensor::logger::detail::erase_archive(
      "/sd/busy", "/sd/busy", nullptr, files, directories, busy_io));
  assert(files == 2 && directories == 4);
  assert(open_directories.empty() && !fs::exists(card / "busy"));
}

void check_entry_cap() {
  using opensensor::logger::detail::archive_reset::kMaximumEntries;
  std::uint32_t files = 0, directories = 0;
  flat_files = kMaximumEntries;
  flat_removed = 0;
  assert(!opensensor::logger::detail::erase_archive(
      "/sd/flat", "/sd/flat", nullptr, files, directories, flat_io));
  assert(errno == EOVERFLOW && flat_removed == 0 && flat_open == 0);
  assert(files == 0 && directories == 0);
  flat_files = kMaximumEntries - 1;
  assert(opensensor::logger::detail::erase_archive(
      "/sd/flat", "/sd/flat", nullptr, files, directories, flat_io));
  assert(files == kMaximumEntries - 1 && directories == 1);
  assert(flat_removed == kMaximumEntries - 1 && flat_open == 0);
}
} // namespace

int main() {
  char temporary[] = "/tmp/os-archive-reset-XXXXXX";
  assert(::mkdtemp(temporary));
  card = temporary;
  using opensensor::logger::detail::archive_root_valid;
  std::uint32_t files = 0, directories = 0;
  // Roots come from board config. Only the configured output root and the
  // optional configured legacy root are accepted.
  const char *output = "/sd/output", *legacy = "/sd/parquet";
  const auto erase_archive =
      [&](const char *root, std::uint32_t &f, std::uint32_t &d,
          const opensensor::logger::detail::ArchiveResetIo &x) {
        return opensensor::logger::detail::erase_archive(root, output, legacy,
                                                         f, d, x);
      };
  for (const char *path : {"/sd", "/sd/", "/sd/output/", "/sd/output/../",
                           "/sd/other", "output", "/sd/bench"}) {
    assert(!erase_archive(path, files, directories, io));
    assert(errno == EINVAL && files == 0 && directories == 0);
  }
  assert(!erase_archive(nullptr, files, directories, io));
  for (const char *path :
       {"/", "/sd", "/sd/", "sd/output", "/sd//output", "/sd/./output",
        "/sd/../output", "/sd/output/..", "/sd/output/", ""})
    assert(!archive_root_valid(path));
  assert(archive_root_valid("/sd/output") && archive_root_valid("/card/a/b"));
  // Invalid configured roots never authorize anything, even on an exact match.
  for (const char *bad : {"/sd", "/", "/sd/../sd"}) {
    assert(!opensensor::logger::detail::erase_archive(bad, bad, nullptr, files,
                                                      directories, io));
    assert(errno == EINVAL && files == 0 && directories == 0);
  }
  assert(erase_archive("/sd/output", files, directories, io));
  assert(files == 0 && directories == 0);
  write(card / "output");
  assert(!erase_archive("/sd/output", files, directories, io));
  assert(errno == ENOTDIR && fs::is_regular_file(card / "output"));
  fs::remove(card / "output");

  write(card / "output/retained.partial");
  auto broken_io = io;
  broken_io.next = read_failure;
  assert(!erase_archive("/sd/output", files, directories, broken_io));
  assert(errno == EIO && files == 0 && directories == 0);
  assert(fs::exists(card / "output/retained.partial"));
  broken_io = io;
  broken_io.remove_file = delete_failure;
  assert(!erase_archive("/sd/output", files, directories, broken_io));
  assert(errno == EACCES && files == 0 && directories == 0);
  assert(fs::exists(card / "output/retained.partial"));
  fs::remove_all(card / "output");

  write(card / "unrelated.txt");
  write(card / "output/thing=old/year=2026/month=10/day=10/part.parquet");
  write(card / "output/quarantine/interrupted.partial");
  write(card / "output/boot-0.partial");
  write(card / "parquet/legacy.parquet");
  fs::create_symlink(card / "unrelated.txt", card / "output/escape");
  assert(!erase_archive("/sd/output", files, directories, io));
  assert(files == 0 && directories == 0);
  assert(fs::exists(card / "output/boot-0.partial"));
  fs::remove(card / "output/escape");
  fs::create_directory_symlink(card, card / "output/escape");
  assert(!erase_archive("/sd/output", files, directories, io));
  assert(files == 0 && directories == 0);
  fs::remove(card / "output/escape");

  // Reject excessive depth in preflight, without partially deleting siblings.
  fs::path deep = card / "output";
  for (unsigned i = 0; i < 9; ++i)
    deep /= "deep";
  fs::create_directories(deep);
  assert(!erase_archive("/sd/output", files, directories, io));
  assert(errno == EOVERFLOW && files == 0 && directories == 0);
  fs::remove_all(card / "output/deep");
  // Every visited entry reports progress in both the preflight and the
  // delete pass, so the worker heartbeat moves through a large erase.
  auto progress_io = io;
  progress_io.progress = count_progress;
  assert(erase_archive("/sd/output", files, directories, progress_io));
  assert(files == 3 && directories == 6);
  assert(progress_calls == 2 * (3 + 6));
  assert(!fs::exists(card / "output"));
  assert(fs::exists(card / "parquet/legacy.parquet"));
  // A board without a legacy root never erases one, even at the old path.
  assert(!opensensor::logger::detail::erase_archive(
      "/sd/parquet", output, nullptr, files, directories, io));
  assert(errno == EINVAL && files == 3 && directories == 6);
  assert(fs::exists(card / "parquet/legacy.parquet"));
  assert(erase_archive("/sd/parquet", files, directories, io));
  assert(files == 4 && directories == 7);
  assert(fs::exists(card / "unrelated.txt"));
  assert(erase_archive("/sd/output", files, directories, io));
  assert(files == 4 && directories == 7);

  // Custom configured output root without a legacy root.
  write(card / "custom/archive/thing=a/x.parquet");
  write(card / "output/kept.parquet");
  assert(opensensor::logger::detail::erase_archive(
      "/sd/custom/archive", "/sd/custom/archive", nullptr, files, directories,
      io));
  assert(files == 5 && directories == 9);
  assert(fs::exists(card / "custom") && !fs::exists(card / "custom/archive"));
  assert(!opensensor::logger::detail::erase_archive(
      "/sd/output", "/sd/custom/archive", nullptr, files, directories, io));
  assert(fs::exists(card / "output/kept.parquet"));
  fs::remove_all(card / "output");
  fs::remove_all(card / "custom");

  fs::create_directory_symlink(card, card / "output");
  assert(!erase_archive("/sd/output", files, directories, io));
  assert(fs::exists(card / "unrelated.txt"));
  check_depth_cap();
  check_close_before_rmdir();
  fs::remove_all(card);
  check_entry_cap();
}
