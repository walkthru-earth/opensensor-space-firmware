#include "os_logger_directory.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {
enum class Scenario {
  MountedRoot,
  ExistingFile,
  Missing,
  Denied,
  RaceDir,
  RaceFile
};
Scenario scenario;
unsigned inspections = 0;
unsigned creations = 0;

int inspect(const char *path, struct stat *info) {
  assert(std::strcmp(path, "/sd") == 0);
  ++inspections;
  if (scenario == Scenario::Denied) {
    errno = EACCES;
    return -1;
  }
  if (scenario == Scenario::Missing ||
      ((scenario == Scenario::RaceDir || scenario == Scenario::RaceFile) &&
       inspections == 1)) {
    errno = ENOENT;
    return -1;
  }
  info->st_mode =
      scenario == Scenario::ExistingFile || scenario == Scenario::RaceFile
          ? S_IFREG
          : S_IFDIR;
  return 0;
}

int create(const char *path, mode_t mode) {
  assert(std::strcmp(path, "/sd") == 0 && mode == 0700);
  ++creations;
  if (scenario == Scenario::MountedRoot) {
    // ESP-IDF FatFs mkdir('/') differs from POSIX mkdir of an existing dir.
    errno = EINVAL;
    return -1;
  }
  if (scenario == Scenario::RaceDir || scenario == Scenario::RaceFile) {
    errno = EEXIST;
    return -1;
  }
  assert(scenario == Scenario::Missing);
  return 0;
}

bool check(Scenario value) {
  scenario = value;
  inspections = creations = 0;
  errno = ERANGE; // Successful stat/mkdir need not clear a prior errno.
  return opensensor::logger::detail::ensure_directory("/sd", inspect, create);
}

struct FakeArchiveIO {
  struct Cursor {
    std::string path;
    unsigned index = 0;
    bool open = false;
  } cursors[2];
  struct dirent entry{};
  std::string fail_open, fail_read, fail_stat, fail_close;
  unsigned opened = 0, closed = 0;

  Cursor *open(const char *path) {
    if (path == fail_open) {
      errno = EIO;
      return nullptr;
    }
    const unsigned index = std::strcmp(path, "/sd/") == 0 ? 0 : 1;
    auto &cursor = cursors[index];
    assert(!cursor.open);
    cursor = {path, 0, true};
    ++opened;
    return &cursor;
  }
  const struct dirent *read(Cursor *cursor) {
    const char *names[2] = {"first.parquet", "day"};
    const bool root = cursor == &cursors[0];
    if (cursor->index == (root ? 2U : 1U)) {
      // The failed SD read happens after a valid file was returned.
      if (cursor->path == fail_read)
        errno = EIO;
      return nullptr;
    }
    std::snprintf(entry.d_name, sizeof(entry.d_name), "%s",
                  root ? names[cursor->index] : "latest.parquet");
    ++cursor->index;
    return &entry;
  }
  int inspect(const char *path, struct stat *info) {
    if (path == fail_stat) {
      errno = EIO;
      return -1;
    }
    info->st_mode = std::strcmp(path, "/sd/day") == 0 ? S_IFDIR : S_IFREG;
    // A successful POSIX call need not clear a previous unrelated errno.
    errno = ERANGE;
    return 0;
  }
  int close(Cursor *cursor) {
    assert(cursor->open);
    cursor->open = false;
    ++closed;
    if (cursor->path == fail_close) {
      errno = EIO;
      return -1;
    }
    return 0;
  }
};

void check_archive_walk() {
  using opensensor::logger::detail::walk_directory;
  for (unsigned failure = 0; failure < 6; ++failure) {
    FakeArchiveIO io;
    switch (failure) {
    case 1:
      io.fail_read = "/sd/day";
      break;
    case 2:
      io.fail_read = "/sd/";
      break;
    case 3:
      io.fail_open = "/sd/day";
      break;
    case 4:
      io.fail_stat = "/sd/day/latest.parquet";
      break;
    case 5:
      io.fail_close = "/sd/day";
      break;
    }
    std::vector<std::string> files;
    auto visit = [&](const char *name, const struct stat &) {
      files.emplace_back(name);
      return true;
    };
    errno = EACCES;
    const bool complete = walk_directory(io, "/sd", "", 0, visit);
    assert(complete == (failure == 0));
    if (failure)
      assert(errno == EIO);
    assert(io.opened == io.closed); // including recursive error unwinding
    assert(files.size() == (failure == 3 || failure == 4 ? 1U : 2U));
    assert(files[0] == "first.parquet");
    if (files.size() == 2)
      assert(files[1] == "day/latest.parquet");
  }
  FakeArchiveIO io;
  auto visit = [](const char *, const struct stat &) {
    errno = ENAMETOOLONG;
    return false;
  };
  assert(!walk_directory(io, "/sd", "", 0, visit));
  assert(errno == ENAMETOOLONG && io.opened == io.closed);
  assert(!walk_directory(io, "/sd", "", 7, visit));
  assert(errno == EOVERFLOW);
  const std::string long_base(416, 'a');
  assert(!walk_directory(io, long_base.c_str(), "", 0, visit));
  assert(errno == ENAMETOOLONG);
}
// An in-memory tree for the resumable walk, built once and shared read only
// by every case. Directories map to their entry names, and a trailing '/'
// marks a subdirectory. `kinds` indexes every entry by its full path, so
// inspect() costs O(log n) instead of a scan of the parent listing. The scan
// made each walk of the 10000 entry directory quadratic, and that walk ran six
// times (issue #50).
struct Tree {
  std::map<std::string, std::vector<std::string>, std::less<>> directories;
  std::map<std::string, mode_t, std::less<>> kinds;

  // "/sd/" holds a day tree with partials and one huge flat directory.
  explicit Tree(unsigned huge) {
    directories["/sd/"] = {"a.parquet", "thing=x/", "huge/", "z.parquet"};
    directories["/sd/thing=x"] = {"year=2026/", "b.partial", ".hidden"};
    directories["/sd/thing=x/year=2026"] = {"month=01/", "month=02/"};
    directories["/sd/thing=x/year=2026/month=01"] = {"c.parquet", "d.parquet"};
    directories["/sd/thing=x/year=2026/month=02"] = {};
    auto &flat = directories["/sd/huge"];
    for (unsigned i = 0; i < huge; ++i)
      flat.push_back("f" + std::to_string(i) +
                     (i % 10 ? ".parquet" : ".partial"));
    for (const auto &[directory, names] : directories) {
      // The root opens as "/sd/" while its entries stat as "/sd/<name>".
      const std::string parent = directory == "/sd/" ? "/sd" : directory;
      for (const auto &name : names) {
        const bool subdirectory = name.back() == '/';
        const std::string_view leaf(name.data(),
                                    name.size() - (subdirectory ? 1 : 0));
        kinds.emplace(parent + "/" + std::string(leaf),
                      subdirectory ? S_IFDIR : S_IFREG);
      }
    }
  }
};

// Per case IO state over a shared Tree, so cursors and counters start fresh.
struct TreeIO {
  struct Cursor {
    const std::vector<std::string> *names = nullptr;
    std::size_t index = 0;
  };
  const Tree &tree;
  std::vector<std::unique_ptr<Cursor>> cursors;
  struct dirent entry{};
  unsigned opened = 0, closed = 0, reads = 0;

  explicit TreeIO(const Tree &shared) : tree(shared) {}

  Cursor *open(const char *path) {
    const auto found = tree.directories.find(std::string_view(path));
    if (found == tree.directories.end()) {
      errno = ENOENT;
      return nullptr;
    }
    ++opened;
    cursors.push_back(
        std::make_unique<Cursor>(Cursor{.names = &found->second, .index = 0}));
    return cursors.back().get();
  }
  const struct dirent *read(Cursor *cursor) {
    ++reads;
    const auto &names = *cursor->names;
    if (cursor->index == names.size())
      return nullptr;
    const std::string &name = names[cursor->index++];
    const int length =
        static_cast<int>(name.size()) - (name.back() == '/' ? 1 : 0);
    std::snprintf(entry.d_name, sizeof(entry.d_name), "%.*s", length,
                  name.c_str());
    return &entry;
  }
  int inspect(const char *path, struct stat *info) {
    const auto found = tree.kinds.find(std::string_view(path));
    if (found == tree.kinds.end()) {
      errno = ENOENT;
      return -1;
    }
    info->st_mode = found->second;
    return 0;
  }
  int close(Cursor *) {
    ++closed;
    return 0;
  }
};

// A walk resumed in bounded steps visits exactly what one unbounded walk
// does, and no step reads more entries than its budget.
void check_resumable_walk() {
  using opensensor::logger::detail::DirectoryWalk;
  using opensensor::logger::detail::walk_directory;
  using Status = DirectoryWalk<TreeIO>::Status;
  const Tree tree(10000);
  std::vector<std::string> whole;
  {
    TreeIO io(tree);
    auto visit = [&](const char *name, const struct stat &) {
      whole.emplace_back(name);
      return true;
    };
    assert(walk_directory(io, "/sd", "", 0, visit));
    assert(io.opened == io.closed && io.opened == 6);
  }
  assert(whole.size() == 4 + 1 + 10000);
  assert(whole[0] == "a.parquet" &&
         whole[1] == "thing=x/year=2026/month=01/c.parquet" &&
         whole.back() == "z.parquet");
  for (const unsigned budget : {1U, 7U, 32U, 4096U}) {
    TreeIO io(tree);
    std::vector<std::string> stepped;
    auto visit = [&](const char *name, const struct stat &) {
      stepped.emplace_back(name);
      return true;
    };
    DirectoryWalk<TreeIO> walk;
    assert(walk.begin(io, "/sd"));
    unsigned steps = 0;
    Status status;
    do {
      const unsigned before = io.reads;
      status = walk.step(budget, visit);
      assert(io.reads - before <= budget);
      ++steps;
    } while (status == Status::More);
    assert(status == Status::Done && !walk.active());
    assert(stepped == whole && io.opened == io.closed);
    assert(steps >= io.reads / budget);
  }
  // A visitor that refuses stops the walk inside the step and closes every
  // open directory. Nothing is read afterwards.
  {
    TreeIO io(tree);
    unsigned visits = 0;
    auto visit = [&](const char *, const struct stat &) {
      return ++visits < 3;
    };
    DirectoryWalk<TreeIO> walk;
    assert(walk.begin(io, "/sd"));
    assert(walk.step(100000, visit) == Status::Stopped);
    assert(visits == 3 && !walk.active() && io.opened == io.closed);
    const unsigned reads = io.reads;
    assert(walk.step(100000, visit) == Status::Done && io.reads == reads);
  }
  // cancel() mid walk, and a destroyed walk, close what is open.
  {
    TreeIO io(tree);
    auto visit = [](const char *, const struct stat &) { return true; };
    {
      DirectoryWalk<TreeIO> walk;
      assert(walk.begin(io, "/sd"));
      assert(walk.step(3, visit) == Status::More && walk.active());
      assert(io.opened > io.closed);
    }
    assert(io.opened == io.closed);
    DirectoryWalk<TreeIO> walk;
    assert(walk.begin(io, "/sd"));
    assert(walk.step(5, visit) == Status::More);
    assert(walk.cancel() && !walk.active() && io.opened == io.closed);
    assert(!walk.begin(io, "/missing") && errno == ENOENT);
  }
}
void check_quarantine_names() {
  using opensensor::logger::detail::quarantine_target;
  char directory[416], path[416];
  assert(quarantine_target(
      "/sd/output",
      "thing=abc/year=2026/month=10/day=11/data_1200_b1_42-3.partial", "b2", 7,
      directory, sizeof(directory), path, sizeof(path)));
  assert(std::strcmp(directory, "/sd/output/quarantine/thing=abc/"
                                "year=2026_month=10_day=11") == 0);
  assert(std::strcmp(path, "/sd/output/quarantine/thing=abc/"
                           "year=2026_month=10_day=11/"
                           "data_1200_b1_42-3-b2-00000007.partial") == 0);
  assert(quarantine_target("/sd/output",
                           "thing=abc/unsynced/boot=b1/data_unsynced_b1_0-0."
                           "partial",
                           "b1", 0, directory, sizeof(directory), path,
                           sizeof(path)));
  assert(std::strcmp(path, "/sd/output/quarantine/thing=abc/unsynced_boot=b1/"
                           "data_unsynced_b1_0-0-b1-00000000.partial") == 0);
  assert(quarantine_target("/sd/output",
                           "benchmarks/boot=b1/lz4_raw_b1_0-1."
                           "partial",
                           "b1", 1, directory, sizeof(directory), path,
                           sizeof(path)));
  assert(std::strcmp(directory,
                     "/sd/output/quarantine/no-thing/benchmarks_boot=b1") == 0);
  assert(quarantine_target("/sd/output", "stray-0.partial", "b1", 2, directory,
                           sizeof(directory), path, sizeof(path)));
  assert(std::strcmp(path, "/sd/output/quarantine/no-thing/root/"
                           "stray-0-b1-00000002.partial") == 0);
  // A torn STAC sidecar (#142) keeps its provenance like any partial.
  assert(quarantine_target(
      "/sd/output",
      "thing=abc/year=2026/month=10/day=11/data_1200_b1_42-131-3.json.partial",
      "b2", 8, directory, sizeof(directory), path, sizeof(path)));
  assert(std::strcmp(path,
                     "/sd/output/quarantine/thing=abc/"
                     "year=2026_month=10_day=11/"
                     "data_1200_b1_42-131-3.json-b2-00000008.partial") == 0);
  // Rejections: not a partial, unsafe segments, absolute or too long.
  for (const char *bad :
       {"thing=a/x.parquet", "thing=a/../x.partial", "thing=a//x.partial",
        "/abs/x.partial", "./x.partial", ".partial"}) {
    errno = 0;
    assert(!quarantine_target("/sd/output", bad, "b1", 0, directory,
                              sizeof(directory), path, sizeof(path)));
  }
  assert(!quarantine_target("/sd/output", "thing=a/x-0.partial", "b1", 0,
                            directory, 24, path, sizeof(path)));
  assert(errno == ENAMETOOLONG);
  assert(!quarantine_target("/sd/output", "thing=a/x-0.partial", "b1", 0,
                            directory, sizeof(directory), path, 40));
  assert(errno == ENAMETOOLONG);
  const std::string long_thing =
      "thing=" + std::string(120, 's') + "/x-0.partial";
  assert(!quarantine_target("/sd/output", long_thing.c_str(), "b1", 0,
                            directory, sizeof(directory), path, sizeof(path)));
  assert(errno == ENAMETOOLONG);
}

struct PosixQuarantineIO {
  bool make_directories(const char *path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return !error;
  }
  bool exists(const char *path) { return ::access(path, F_OK) == 0; }
  int rename(const char *from, const char *to) { return ::rename(from, to); }
};

// A write failure quarantines its own .partial in the same boot, with
// provenance, and never replaces an existing quarantined fragment.
void check_same_boot_quarantine() {
  namespace fs = std::filesystem;
  char temporary[] = "/tmp/aq-quarantine-XXXXXX";
  assert(::mkdtemp(temporary));
  const fs::path root = fs::path(temporary) / "output";
  const char *relative =
      "thing=abc/year=2026/month=10/day=11/data_1200_b1_42-3.partial";
  fs::create_directories((root / relative).parent_path());
  {
    std::ofstream(root / relative) << "partial bytes";
  }
  PosixQuarantineIO io;
  char moved[416];
  assert(opensensor::logger::detail::quarantine_partial(
      io, root.c_str(), relative, "b1", 0, moved, sizeof(moved)));
  assert(!fs::exists(root / relative));
  assert(fs::exists(root / "thing=abc/year=2026/month=10/day=11"));
  const fs::path expected = root / "quarantine/thing=abc/"
                                   "year=2026_month=10_day=11/"
                                   "data_1200_b1_42-3-b1-00000000.partial";
  assert(moved == expected.string() && fs::file_size(expected) == 13);
  // Same counter again must refuse to overwrite the earlier fragment.
  {
    std::ofstream(root / relative) << "second";
  }
  assert(!opensensor::logger::detail::quarantine_partial(
      io, root.c_str(), relative, "b1", 0, moved, sizeof(moved)));
  assert(errno == EEXIST && fs::exists(root / relative));
  assert(fs::file_size(expected) == 13);
  assert(opensensor::logger::detail::quarantine_partial(
      io, root.c_str(), relative, "b1", 1, moved, sizeof(moved)));
  assert(!fs::exists(root / relative));
  // A missing source fails and creates nothing beyond the directory.
  assert(!opensensor::logger::detail::quarantine_partial(
      io, root.c_str(), relative, "b1", 2, moved, sizeof(moved)));
  fs::remove_all(temporary);
}

struct PosixDirectoryIO {
  DIR *open(const char *path) { return ::opendir(path); }
  const struct dirent *read(DIR *directory) { return ::readdir(directory); }
  int inspect(const char *path, struct stat *info) {
    return ::stat(path, info);
  }
  int close(DIR *directory) { return ::closedir(directory); }
};

// The startup quarantine walk is iterative. It finds partials down to the
// depth cap while they are moved out under it, never enters quarantine/,
// and reports a deeper directory instead of skipping it silently.
void check_find_partials() {
  namespace fs = std::filesystem;
  using opensensor::logger::detail::DirectoryWalk;
  using opensensor::logger::detail::find_partials;
  char temporary[] = "/tmp/aq-find-partials-XXXXXX";
  assert(::mkdtemp(temporary));
  const fs::path root = fs::path(temporary) / "output";
  const auto touch = [](const fs::path &path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "x";
  };
  touch(root / "top-0.partial");
  touch(root / "kept.parquet");
  touch(root / "quarantine/thing=a/day/old-0.partial");
  touch(root / "thing=a/year=2026/month=10/day=11/data-1.partial");
  touch(root / "thing=a/year=2026/month=10/day=11/data-1-2-3.parquet");
  // A sidecar torn by a reset is found like a Parquet partial (#142). A
  // finished sidecar is not.
  touch(root / "thing=a/year=2026/month=10/day=11/data-1-2-3.json");
  touch(root / "thing=a/year=2026/month=10/day=11/data-4-5-6.json.partial");
  // The deepest directory the walk opens sits at kMaxDepth, so a partial
  // there is found and one more level is reported.
  fs::path deep = root;
  for (unsigned i = 0; i < DirectoryWalk<PosixDirectoryIO>::kMaxDepth; ++i)
    deep /= "l" + std::to_string(i + 1);
  touch(deep / "at-cap.partial");
  touch(deep / "past/below-cap.partial");
  PosixDirectoryIO io;
  PosixQuarantineIO mover;
  std::vector<std::string> moved, reported;
  std::uint32_t counter = 0;
  auto move = [&](const char *name) {
    char destination[416];
    assert(opensensor::logger::detail::quarantine_partial(
        mover, root.c_str(), name, "b1", counter++, destination,
        sizeof(destination)));
    moved.emplace_back(name);
  };
  auto too_deep = [&](const char *name) { reported.emplace_back(name); };
  auto unreadable = [](const char *, int) { assert(false); };
  assert(find_partials(io, root.c_str(), move, too_deep, unreadable));
  std::sort(moved.begin(), moved.end());
  assert(moved.size() == 4);
  assert(moved[0] == "l1/l2/l3/l4/l5/l6/at-cap.partial");
  assert(moved[1] == "thing=a/year=2026/month=10/day=11/data-1.partial");
  assert(moved[2] ==
         "thing=a/year=2026/month=10/day=11/data-4-5-6.json.partial");
  assert(moved[3] == "top-0.partial");
  assert(reported.size() == 1 && reported[0] == "l1/l2/l3/l4/l5/l6/past");
  assert(fs::exists(deep / "past/below-cap.partial"));
  assert(fs::exists(root / "quarantine/thing=a/day/old-0.partial"));
  assert(fs::exists(root / "kept.parquet"));
  assert(
      fs::exists(root / "thing=a/year=2026/month=10/day=11/data-1-2-3.json"));
  // A second walk finds only what was left behind on purpose.
  moved.clear();
  reported.clear();
  assert(find_partials(io, root.c_str(), move, too_deep, unreadable));
  assert(moved.empty() && reported.size() == 1);
  // A missing root fails with its errno instead of looking empty.
  assert(!find_partials(io, (root / "missing").c_str(), move, too_deep,
                        unreadable));
  assert(errno == ENOENT);
  fs::remove_all(temporary);
}

struct FakeListing {
  std::vector<std::string> names;
  bool fail_open = false, fail_read = false, fail_close = false;
  unsigned index = 0;
  bool opened = false;
  struct dirent entry{};
  FakeListing *open(const char *) {
    if (fail_open) {
      errno = EIO;
      return nullptr;
    }
    opened = true;
    index = 0;
    return this;
  }
  const struct dirent *read(FakeListing *) {
    if (index == names.size()) {
      if (fail_read)
        errno = EIO;
      return nullptr;
    }
    std::snprintf(entry.d_name, sizeof(entry.d_name), "%s",
                  names[index++].c_str());
    return &entry;
  }
  int close(FakeListing *) {
    opened = false;
    return fail_close ? -1 : 0;
  }
};

void check_crosslink_guard() {
  using opensensor::logger::detail::crosslink_pair;
  using opensensor::logger::detail::crosslink_suspect;
  assert(crosslink_pair("data_1200_b1_42-3.partial",
                        "data_1200_b1_42-131-3.parquet"));
  assert(crosslink_pair("data_unsynced_b1_0-12.partial",
                        "data_unsynced_b1_0-5-12.parquet"));
  // Different attempt, different stem, missing last or not a number.
  assert(!crosslink_pair("data_1200_b1_42-3.partial",
                         "data_1200_b1_42-131-4.parquet"));
  assert(!crosslink_pair("data_1200_b1_42-3.partial",
                         "data_1200_b1_43-131-3.parquet"));
  assert(!crosslink_pair("data_1200_b1_42-3.partial",
                         "data_1200_b1_42--3.parquet"));
  assert(!crosslink_pair("data_1200_b1_42-3.partial",
                         "data_1200_b1_42-x1-3.parquet"));
  assert(!crosslink_pair("data_1200_b1_42-3.partial",
                         "data_1200_b1_42-131-13.parquet"));
  assert(!crosslink_pair("data_1200_b1_42-3.partial", "other.parquet"));
  assert(!crosslink_pair("nodash.partial", "nodash-1-2.parquet"));
  assert(!crosslink_pair("x-.partial", "x-1-.parquet"));
  // A STAC sidecar partial (#142) never pairs with a Parquet file, since its
  // last segment is not an attempt number.
  assert(!crosslink_pair("data_1200_b1_42-131-3.json.partial",
                         "data_1200_b1_42-131-3.parquet"));
  using opensensor::logger::detail::sidecar_crosslink_pair;
  assert(sidecar_crosslink_pair("data_1200_b1_42-131-3.json.partial",
                                "data_1200_b1_42-131-3.json"));
  assert(!sidecar_crosslink_pair("data_1200_b1_42-131-3.json.partial",
                                 "data_1200_b1_42-131-4.json"));
  assert(!sidecar_crosslink_pair("data_1200_b1_42-131-3.json.partial",
                                 "data_1200_b1_42-131-3.parquet"));
  assert(!sidecar_crosslink_pair("data_1200_b1_42-131-3.json.partial",
                                 "xdata_1200_b1_42-131-3.json"));
  assert(!sidecar_crosslink_pair("data_1200_b1_42-3.partial",
                                 "data_1200_b1_42-3"));
  assert(!sidecar_crosslink_pair(".json.partial", ".json"));

  FakeListing listing;
  listing.names = {"data_1200_b1_42-131-3.parquet", "data_1200_b1_42-3.partial",
                   "data_1215_b1_132-4.partial"};
  assert(crosslink_suspect(listing, "/sd/day", "data_1200_b1_42-3.partial"));
  assert(!listing.opened);
  assert(!crosslink_suspect(listing, "/sd/day", "data_1215_b1_132-4.partial"));
  // An unreadable directory is treated as suspect so nothing moves.
  listing.fail_read = true;
  assert(crosslink_suspect(listing, "/sd/day", "data_1215_b1_132-4.partial"));
  listing.fail_read = false;
  listing.fail_open = true;
  assert(crosslink_suspect(listing, "/sd/day", "data_1215_b1_132-4.partial"));
  listing.fail_open = false;
  listing.fail_close = true;
  assert(crosslink_suspect(listing, "/sd/day", "data_1215_b1_132-4.partial"));
  listing.fail_close = false;
  // A sidecar partial beside its final sidecar stays, one beside only its
  // Parquet file is torn and may move.
  listing.names = {
      "data_1200_b1_42-131-3.parquet", "data_1200_b1_42-131-3.json",
      "data_1200_b1_42-131-3.json.partial", "data_1215_b1_132-200-4.parquet",
      "data_1215_b1_132-200-4.json.partial"};
  assert(crosslink_suspect(listing, "/sd/day",
                           "data_1200_b1_42-131-3.json.partial"));
  assert(!crosslink_suspect(listing, "/sd/day",
                            "data_1215_b1_132-200-4.json.partial"));
}
} // namespace

// Fails inspect for names holding "bad", opendir for names holding
// "locked" and every read of a directory named "broken".
struct FaultyDirectoryIO : PosixDirectoryIO {
  DIR *broken = nullptr;
  DIR *open(const char *path) {
    if (std::strstr(path, "locked")) {
      errno = EACCES;
      return nullptr;
    }
    DIR *directory = ::opendir(path);
    if (directory && std::strstr(path, "broken"))
      broken = directory;
    return directory;
  }
  const struct dirent *read(DIR *directory) {
    if (directory == broken) {
      errno = EIO;
      return nullptr;
    }
    return ::readdir(directory);
  }
  int inspect(const char *path, struct stat *info) {
    if (std::strstr(path, "bad")) {
      errno = EIO;
      return -1;
    }
    return ::stat(path, info);
  }
};

// One entry that cannot be inspected or opened is reported and skipped, and
// the partials after it are still moved. A directory read error ends the
// walk and is reported as a failed walk, never as a complete one.
void check_find_partials_failures() {
  namespace fs = std::filesystem;
  using opensensor::logger::detail::find_partials;
  char temporary[] = "/tmp/aq-find-faults-XXXXXX";
  assert(::mkdtemp(temporary));
  const fs::path root = fs::path(temporary) / "output";
  const auto touch = [](const fs::path &path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "x";
  };
  touch(root / "a-bad.partial");
  touch(root / "b-locked/inside-0.partial");
  touch(root / "thing=a/day/c-0.partial");
  touch(root / "z-0.partial");
  FaultyDirectoryIO io;
  std::vector<std::string> moved, skipped;
  std::vector<int> errors;
  auto move = [&](const char *name) { moved.emplace_back(name); };
  auto too_deep = [](const char *) { assert(false); };
  auto unreadable = [&](const char *name, int error) {
    skipped.emplace_back(name);
    errors.push_back(error);
  };
  assert(find_partials(io, root.c_str(), move, too_deep, unreadable));
  std::sort(moved.begin(), moved.end());
  assert(moved.size() == 2);
  assert(moved[0] == "thing=a/day/c-0.partial" && moved[1] == "z-0.partial");
  assert(skipped.size() == 2);
  const bool bad_first = skipped[0] == "a-bad.partial";
  assert(skipped[bad_first ? 0 : 1] == "a-bad.partial");
  assert(skipped[bad_first ? 1 : 0] == "b-locked");
  assert(errors[bad_first ? 0 : 1] == EIO);
  assert(errors[bad_first ? 1 : 0] == EACCES);
  // A directory that fails mid read stops the walk with its errno.
  touch(root / "thing=a/broken/d-0.partial");
  moved.clear();
  skipped.clear();
  errno = 0;
  assert(!find_partials(io, root.c_str(), move, too_deep, unreadable));
  assert(errno == EIO);
  assert(std::find(moved.begin(), moved.end(), "thing=a/broken/d-0.partial") ==
         moved.end());
  fs::remove_all(temporary);
}

int main() {
  assert(check(Scenario::MountedRoot));
  assert(inspections == 1 && creations == 0);
  assert(!check(Scenario::ExistingFile));
  assert(errno == ENOTDIR && creations == 0);
  assert(check(Scenario::Missing));
  assert(creations == 1);
  assert(!check(Scenario::Denied));
  assert(errno == EACCES && creations == 0);
  assert(check(Scenario::RaceDir));
  assert(inspections == 2 && creations == 1);
  assert(!check(Scenario::RaceFile));
  assert(errno == ENOTDIR && inspections == 2 && creations == 1);
  check_archive_walk();
  check_resumable_walk();
  check_quarantine_names();
  check_same_boot_quarantine();
  check_find_partials();
  check_find_partials_failures();
  check_crosslink_guard();
}
