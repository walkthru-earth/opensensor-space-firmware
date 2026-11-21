#include "os_logger_flash_copy.h"

#include "sha256_stream.h"
#include "sync_codec.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {
namespace fc = opensensor::logger::flash_copy;
using Bytes = std::vector<std::uint8_t>;
using fc::Failure;
using fc::Outcome;

constexpr const char *kFlash = "/flash/output";
constexpr const char *kCard = "/sd/output";
constexpr const char *kDay = "thing=t1/year=2026/month=11/day=21";
const std::string kName = std::string(kDay) + "/a-1-2.parquet";
const std::string kSource = std::string(kFlash) + "/" + kName;
const std::string kFinal = std::string(kCard) + "/" + kName;
const std::string kTemp = kFinal + ".copy";

Bytes pattern(std::size_t length, std::uint32_t seed = 0x12345678) {
  Bytes out(length);
  for (auto &byte : out) {
    seed = seed * 1664525U + 1013904223U;
    byte = static_cast<std::uint8_t>(seed >> 24);
  }
  return out;
}

std::uint32_t crc_of(const Bytes &bytes) {
  return opensensor::sync::crc32_update(0xffffffffU, bytes.data(),
                                        bytes.size()) ^
         0xffffffffU;
}

// The real SHA-256 stream, copied fresh so a fake can be returned by value.
struct Hasher {
  opensensor::hash::Sha256Stream stream;
  Hasher() = default;
  Hasher(const Hasher &) {}
  Hasher &operator=(const Hasher &) { return *this; }
};

// An in memory pair of FAT volumes. It owns the one reader and the one
// writer the copy may hold, and checks the machine never opens a second.
struct FakeIO {
  std::map<std::string, Bytes> files;
  std::set<std::string> dirs = {kFlash, kCard};
  bool reader_open = false, writer_open = false;
  std::string reader_path, writer_path;
  std::size_t reader_pos = 0;
  std::vector<std::uint8_t> buffer = std::vector<std::uint8_t>(8192);
  Hasher sha;

  // Failure injection.
  std::string fail_inspect, fail_open_read, fail_open_write, fail_unlink;
  std::string fail_read;         // a read of this open file fails with EIO
  unsigned fail_hash_finish = 0; // the hash_finish call to fail, 1 based
  int fail_inspect_errno = EIO;
  bool fail_dirs = false, fail_sync = false, fail_hash_begin = false;
  bool fail_close_write = false;
  int fail_rename = 0; // errno to fail rename with, 0 for none
  // A rename that fails after writing the new entry, as a power cut inside a
  // FAT rename can. Both names then hold the bytes (a crosslink on a card).
  bool rename_leaves_both = false;
  std::optional<std::size_t> corrupt_at;

  // What happened.
  unsigned data_reads = 0, syncs = 0, hash_finishes = 0;
  std::size_t largest_read = 0;
  std::vector<std::string> unlinked, made;

  void add_dirs(const std::string &path) {
    for (std::size_t at = path.find('/', 1); at != std::string::npos;
         at = path.find('/', at + 1))
      dirs.insert(path.substr(0, at));
    dirs.insert(path);
  }

  // A reset closes every handle and keeps what was written.
  void reset() {
    reader_open = writer_open = false;
    reader_path.clear();
    writer_path.clear();
  }

  int inspect(const char *path, struct stat *info) {
    if (fail_inspect == path) {
      errno = fail_inspect_errno;
      return -1;
    }
    *info = {};
    if (const auto file = files.find(path); file != files.end()) {
      info->st_mode = S_IFREG;
      info->st_size = static_cast<off_t>(file->second.size());
      return 0;
    }
    if (dirs.contains(path)) {
      info->st_mode = S_IFDIR;
      return 0;
    }
    errno = ENOENT;
    return -1;
  }

  bool make_directories(const char *path) {
    if (fail_dirs) {
      errno = EIO;
      return false;
    }
    made.emplace_back(path);
    add_dirs(path);
    return true;
  }

  bool open_read(const char *path) {
    assert(!reader_open);
    if (fail_open_read == path) {
      errno = EIO;
      return false;
    }
    if (!files.contains(path)) {
      errno = ENOENT;
      return false;
    }
    reader_open = true;
    reader_path = path;
    reader_pos = 0;
    return true;
  }

  bool open_write(const char *path) {
    assert(!writer_open);
    const std::string name = path;
    if (fail_open_write == name) {
      errno = EIO;
      return false;
    }
    if (files.contains(name) || dirs.contains(name)) {
      errno = EEXIST;
      return false;
    }
    if (!dirs.contains(name.substr(0, name.rfind('/')))) {
      errno = ENOENT;
      return false;
    }
    files[name] = {};
    writer_open = true;
    writer_path = name;
    return true;
  }

  std::span<std::uint8_t> scratch() { return buffer; }

  std::ptrdiff_t read(std::span<std::uint8_t> out) {
    assert(reader_open);
    const auto file = files.find(reader_path);
    if (file == files.end() || fail_read == reader_path) {
      errno = EIO;
      return -1;
    }
    ++data_reads;
    const std::size_t count =
        std::min(out.size(), file->second.size() - reader_pos);
    std::copy_n(file->second.begin() + static_cast<std::ptrdiff_t>(reader_pos),
                count, out.begin());
    reader_pos += count;
    largest_read = std::max(largest_read, count);
    return static_cast<std::ptrdiff_t>(count);
  }

  bool write(std::span<const std::uint8_t> bytes) {
    assert(writer_open);
    auto &file = files.at(writer_path);
    const std::size_t start = file.size();
    file.insert(file.end(), bytes.begin(), bytes.end());
    if (corrupt_at && *corrupt_at >= start && *corrupt_at < file.size())
      file[*corrupt_at] ^= 0x5a;
    return true;
  }

  bool sync() {
    assert(writer_open);
    ++syncs;
    if (fail_sync) {
      errno = EIO;
      return false;
    }
    return true;
  }

  bool close_read() {
    assert(reader_open);
    reader_open = false;
    return true;
  }

  bool close_write() {
    assert(writer_open);
    writer_open = false;
    if (fail_close_write) {
      errno = EIO;
      return false;
    }
    return true;
  }

  int unlink(const char *path) {
    assert(!(reader_open && reader_path == path));
    assert(!(writer_open && writer_path == path));
    if (fail_unlink == path) {
      errno = EIO;
      return -1;
    }
    if (files.erase(path) == 0) {
      errno = ENOENT;
      return -1;
    }
    unlinked.emplace_back(path);
    return 0;
  }

  int rename_no_replace(const char *from, const char *to) {
    assert(!(writer_open && writer_path == from));
    if (rename_leaves_both) {
      files[to] = files.at(from);
      errno = EIO;
      return -1;
    }
    if (fail_rename) {
      errno = fail_rename;
      return -1;
    }
    if (files.contains(to) || dirs.contains(to)) {
      errno = EEXIST;
      return -1;
    }
    const auto node = files.extract(from);
    if (node.empty()) {
      errno = ENOENT;
      return -1;
    }
    files[to] = std::move(node.mapped());
    return 0;
  }

  std::expected<void, opensensor::hash::Sha256Error> hash_begin() {
    if (fail_hash_begin)
      return std::unexpected(opensensor::hash::Sha256Error::Setup);
    return sha.stream.begin();
  }
  void hash_update(std::span<const std::uint8_t> bytes) {
    sha.stream.update(bytes);
  }
  std::expected<opensensor::hash::Sha256Digest, opensensor::hash::Sha256Error>
  hash_finish() {
    // The stream is finished either way, as a failed finish leaves it.
    const auto digest = sha.stream.finish();
    if (++hash_finishes == fail_hash_finish)
      return std::unexpected(opensensor::hash::Sha256Error::Finish);
    return digest;
  }
};

FakeIO with_source(const Bytes &bytes, const std::string &name = kName) {
  FakeIO io;
  const std::string source = std::string(kFlash) + "/" + name;
  io.add_dirs(source.substr(0, source.rfind('/')));
  io.files[source] = bytes;
  return io;
}

void begin(fc::Copier &copier, const std::string &name = kName) {
  const auto begun = copier.begin(kFlash, kCard, name.c_str());
  assert(begun.has_value());
  assert(copier.active());
}

struct Run {
  Outcome outcome = Outcome::InProgress;
  unsigned steps = 0;
};

// Steps until the copy ends, or `limit` steps. Every step reads at most one
// chunk of at most kChunkBytes.
Run run(fc::Copier &copier, FakeIO &io, unsigned limit = 10000) {
  Run out;
  while (out.steps < limit) {
    io.data_reads = 0;
    out.outcome = copier.step(io);
    ++out.steps;
    assert(io.data_reads <= 1);
    assert(io.largest_read <= fc::kChunkBytes);
    if (out.outcome != Outcome::InProgress) {
      assert(!copier.active());
      assert(!io.reader_open && !io.writer_open);
      break;
    }
  }
  return out;
}

// Runs the copy and checks how it ended. The run stays outside assert(), so
// a build without asserts still runs every copy.
void expect_outcome(fc::Copier &copier, FakeIO &io, Outcome want,
                    unsigned limit = 10000) {
  [[maybe_unused]] const Run result = run(copier, io, limit);
  assert(result.outcome == want);
}

void happy_path() {
  for (const std::size_t length : {std::size_t{0}, std::size_t{100}}) {
    const Bytes bytes = pattern(length);
    FakeIO io = with_source(bytes);
    fc::Copier copier;
    begin(copier);
    const Run result = run(copier, io);
    assert(result.outcome == Outcome::Copied);
    assert(!copier.failure().has_value());
    assert(io.files.at(kFinal) == bytes);
    assert(!io.files.contains(kSource));
    assert(!io.files.contains(kTemp));
    assert(io.syncs == 1);
    assert(io.made.size() == 1 &&
           io.made[0] == std::string(kCard) + "/" + kDay);
    assert(io.unlinked == std::vector<std::string>{kSource});
    assert(copier.bytes() == length && copier.crc() == crc_of(bytes));
    assert(std::strcmp(copier.relative(), kName.c_str()) == 0);
  }
}

// Each step moves one bounded chunk. 3 x 4096 + 17 bytes take one start
// step, five source reads (four chunks and EOF), one seal step, five verify
// reads and one commit step.
void multi_chunk() {
  const Bytes bytes = pattern(3 * fc::kChunkBytes + 17);
  FakeIO io = with_source(bytes);
  fc::Copier copier;
  begin(copier);
  const Run result = run(copier, io);
  assert(result.outcome == Outcome::Copied);
  assert(result.steps == 13);
  assert(io.largest_read == fc::kChunkBytes);
  assert(io.files.at(kFinal) == bytes);

  // A scratch buffer smaller than a chunk bounds the read instead.
  FakeIO small = with_source(bytes);
  small.buffer.resize(1000);
  fc::Copier again;
  begin(again);
  expect_outcome(again, small, Outcome::Copied);
  assert(small.largest_read == 1000);
  assert(small.files.at(kFinal) == bytes);

  // No scratch at all is an IO error before anything is created.
  FakeIO none = with_source(bytes);
  none.buffer.clear();
  fc::Copier empty;
  begin(empty);
  expect_outcome(empty, none, Outcome::IoError);
  assert(empty.failure() == Failure::Scratch);
  assert(none.files.contains(kSource) && !none.files.contains(kTemp));
}

// A reset mid copy leaves a partial temporary on the card. The next attempt
// removes it, because its source is still on flash, and copies again.
void reset_mid_copy() {
  const Bytes bytes = pattern(5 * fc::kChunkBytes);
  for (const unsigned stop : {3U, 8U, 12U}) {
    FakeIO io = with_source(bytes);
    {
      fc::Copier first;
      begin(first);
      const Run partial = run(first, io, stop);
      assert(partial.outcome == Outcome::InProgress);
      assert(io.files.contains(kTemp) && io.files.contains(kSource));
      io.reset();
    }
    fc::Copier second;
    begin(second);
    const Run result = run(second, io);
    assert(result.outcome == Outcome::Copied);
    assert(io.unlinked.front() == kTemp);
    assert(io.files.at(kFinal) == bytes);
    assert(!io.files.contains(kSource) && !io.files.contains(kTemp));
  }
}

// A byte that differs on the card fails the readback. The temporary goes,
// the source stays and no final name appears.
void verify_mismatch() {
  const Bytes bytes = pattern(2 * fc::kChunkBytes + 5);
  FakeIO io = with_source(bytes);
  io.corrupt_at = fc::kChunkBytes + 3;
  fc::Copier copier;
  begin(copier);
  const Run result = run(copier, io);
  assert(result.outcome == Outcome::IoError);
  assert(copier.failure() == Failure::Verify);
  assert(io.files.at(kSource) == bytes);
  assert(!io.files.contains(kTemp) && !io.files.contains(kFinal));
  assert(io.unlinked == std::vector<std::string>{kTemp});
  // A step while idle keeps the cause and errno of the failed copy for the
  // caller's log line.
  const int failed_errno = copier.error();
  [[maybe_unused]] const Outcome idle = copier.step(io);
  assert(idle == Outcome::IoError);
  assert(copier.failure() == Failure::Verify);
  assert(copier.error() == failed_errno);
  assert(std::strcmp(copier.relative(), kName.c_str()) == 0);
}

// The card already holds the same bytes under the final name, with no
// temporary next to it. The source is freed and the final file is not
// touched.
void existing_identical() {
  const Bytes bytes = pattern(2 * fc::kChunkBytes + 9);
  FakeIO io = with_source(bytes);
  io.add_dirs(std::string(kCard) + "/" + kDay);
  io.files[kFinal] = bytes;
  fc::Copier copier;
  begin(copier);
  const Run result = run(copier, io);
  assert(result.outcome == Outcome::AlreadyIdentical);
  assert(io.files.at(kFinal) == bytes);
  assert(!io.files.contains(kSource));
  assert(io.unlinked == std::vector<std::string>{kSource});
  assert(io.syncs == 0 && io.made.empty());
}

// A temporary next to a final file is the shape a power cut inside the FAT
// rename leaves, both entries possibly on one cluster chain. Unlinking the
// temporary would free the final file's clusters, and the identical hash
// would then free the source too. So nothing is deleted, the source stays,
// and the copy ends as a crosslink suspect, whatever the final file holds.
void crosslink_suspect() {
  const Bytes bytes = pattern(2 * fc::kChunkBytes + 9);
  const std::array<Bytes, 3> finals = {bytes, pattern(bytes.size(), 5),
                                       pattern(7, 5)};
  for (const Bytes &final_bytes : finals) {
    for (const bool same_temp : {true, false}) {
      FakeIO io = with_source(bytes);
      io.add_dirs(std::string(kCard) + "/" + kDay);
      io.files[kFinal] = final_bytes;
      const Bytes temp = same_temp ? final_bytes : pattern(10, 7);
      io.files[kTemp] = temp;
      fc::Copier copier;
      begin(copier);
      const Run result = run(copier, io);
      assert(result.outcome == Outcome::IoError && result.steps == 1);
      assert(copier.failure() == Failure::CrosslinkSuspect);
      assert(std::strcmp(fc::failure_name(Failure::CrosslinkSuspect),
                         "crosslink-suspect") == 0);
      assert(io.unlinked.empty());
      assert(io.files.at(kSource) == bytes);
      assert(io.files.at(kFinal) == final_bytes && io.files.at(kTemp) == temp);
      // Every later pass finds the same pair and still deletes nothing.
      begin(copier);
      expect_outcome(copier, io, Outcome::IoError);
      assert(copier.failure() == Failure::CrosslinkSuspect);
      assert(io.unlinked.empty() && io.files.contains(kSource));
    }
  }
  // A temporary that cannot be inspected next to a final file is not
  // touched either.
  FakeIO io = with_source(bytes);
  io.add_dirs(std::string(kCard) + "/" + kDay);
  io.files[kFinal] = bytes;
  io.fail_inspect = kTemp;
  fc::Copier copier;
  begin(copier);
  expect_outcome(copier, io, Outcome::IoError);
  assert(copier.failure() == Failure::CardStat && copier.error() == EIO);
  assert(io.unlinked.empty() && io.files.contains(kSource));
}

// The card holds other bytes under the final name. Both stay, nothing is
// deleted or overwritten.
void existing_different() {
  const Bytes bytes = pattern(2 * fc::kChunkBytes + 9);
  const std::array<Bytes, 3> others = {
      pattern(bytes.size(), 99),    // same size, other bytes
      pattern(bytes.size() + 1, 1), // other size
      [&] {                         // one byte differs at the end
        Bytes last = bytes;
        last.back() ^= 1;
        return last;
      }()};
  for (const Bytes &other : others) {
    FakeIO io = with_source(bytes);
    io.add_dirs(std::string(kCard) + "/" + kDay);
    io.files[kFinal] = other;
    fc::Copier copier;
    begin(copier);
    const Run result = run(copier, io);
    assert(result.outcome == Outcome::Conflict);
    assert(io.files.at(kFinal) == other && io.files.at(kSource) == bytes);
    assert(io.unlinked.empty());
  }
  // A size mismatch needs no read at all.
  FakeIO io = with_source(bytes);
  io.add_dirs(std::string(kCard) + "/" + kDay);
  io.files[kFinal] = pattern(3);
  fc::Copier copier;
  begin(copier);
  const Run quick = run(copier, io);
  assert(quick.outcome == Outcome::Conflict && quick.steps == 1);
  // A directory under the final name is a conflict too.
  FakeIO folder = with_source(bytes);
  folder.add_dirs(kFinal);
  fc::Copier other;
  begin(other);
  expect_outcome(other, folder, Outcome::Conflict);
  assert(folder.files.at(kSource) == bytes && folder.unlinked.empty());
}

// A source missing at the start touches nothing on the card, not even a
// temporary, since only a temporary whose source is on flash may go. A
// source that vanishes mid copy keeps the temporary for the same reason.
void source_vanished() {
  FakeIO io;
  io.add_dirs(std::string(kCard) + "/" + kDay);
  io.files[kTemp] = pattern(10);
  fc::Copier copier;
  begin(copier);
  const Run result = run(copier, io);
  assert(result.outcome == Outcome::IoError);
  assert(copier.failure() == Failure::SourceMissing);
  assert(copier.error() == ENOENT);
  assert(io.files.contains(kTemp) && io.unlinked.empty());

  const Bytes bytes = pattern(3 * fc::kChunkBytes);
  FakeIO later = with_source(bytes);
  fc::Copier mid;
  begin(mid);
  expect_outcome(mid, later, Outcome::InProgress, 2);
  later.files.erase(kSource);
  const Run gone = run(mid, later);
  assert(gone.outcome == Outcome::IoError);
  assert(mid.failure() == Failure::SourceRead);
  assert(later.files.contains(kTemp) && !later.files.contains(kFinal));
  assert(later.unlinked.empty());

  // A source that cannot be inspected is not treated as missing.
  FakeIO broken = with_source(bytes);
  broken.fail_inspect = kSource;
  fc::Copier stat_error;
  begin(stat_error);
  expect_outcome(stat_error, broken, Outcome::IoError);
  assert(stat_error.failure() == Failure::SourceStat);
  assert(broken.unlinked.empty());
}

// The rename never replaces. A refused rename keeps the source. It removes
// the temporary only when the final name is confirmed absent, so a final
// name that appeared meanwhile stays as it is and so does the temporary.
void rename_refused() {
  const Bytes bytes = pattern(fc::kChunkBytes + 1);
  FakeIO io = with_source(bytes);
  io.fail_rename = EACCES;
  fc::Copier copier;
  begin(copier);
  const Run result = run(copier, io);
  assert(result.outcome == Outcome::IoError);
  assert(copier.failure() == Failure::Rename && copier.error() == EACCES);
  assert(io.files.at(kSource) == bytes);
  assert(!io.files.contains(kTemp) && !io.files.contains(kFinal));

  FakeIO race = with_source(bytes);
  fc::Copier racer;
  begin(racer);
  // Start, two source reads and EOF, seal, two verify reads and EOF.
  expect_outcome(racer, race, Outcome::InProgress, 8);
  const Bytes other = pattern(4, 3);
  race.files[kFinal] = other;
  const Run raced = run(racer, race);
  assert(raced.outcome == Outcome::IoError && raced.steps == 1);
  assert(racer.failure() == Failure::Rename && racer.error() == EEXIST);
  assert(race.files.at(kFinal) == other && race.files.at(kSource) == bytes);
  assert(race.files.contains(kTemp) && race.unlinked.empty());
  // The next pass finds the pair and leaves it for the owner.
  begin(racer);
  expect_outcome(racer, race, Outcome::IoError);
  assert(racer.failure() == Failure::CrosslinkSuspect);
  assert(race.unlinked.empty() && race.files.at(kSource) == bytes);

  // A rename that fails after the new entry was written leaves both names,
  // which on a card may share clusters. The temporary is not unlinked.
  FakeIO both = with_source(bytes);
  both.rename_leaves_both = true;
  fc::Copier half;
  begin(half);
  const Run partial = run(half, both);
  assert(partial.outcome == Outcome::IoError);
  assert(half.failure() == Failure::Rename && half.error() == EIO);
  assert(both.files.at(kFinal) == bytes && both.files.at(kTemp) == bytes);
  assert(both.files.at(kSource) == bytes && both.unlinked.empty());
  both.rename_leaves_both = false;
  begin(half);
  expect_outcome(half, both, Outcome::IoError);
  assert(half.failure() == Failure::CrosslinkSuspect);
  assert(both.unlinked.empty() && both.files.at(kSource) == bytes);
}

// Every other IO failure keeps the source and removes only the temporary
// this attempt made.
void io_failures() {
  const Bytes bytes = pattern(fc::kChunkBytes + 1);
  struct Case {
    void (*arm)(FakeIO &);
    Failure failure;
    bool temp_made;
  };
  const std::array<Case, 8> cases = {{
      {[](FakeIO &io) { io.fail_dirs = true; }, Failure::Directory, false},
      {[](FakeIO &io) { io.fail_open_read = kSource; }, Failure::SourceOpen,
       false},
      {[](FakeIO &io) { io.fail_open_write = kTemp; }, Failure::TempOpen,
       false},
      {[](FakeIO &io) { io.fail_hash_begin = true; }, Failure::Hash, true},
      {[](FakeIO &io) { io.fail_sync = true; }, Failure::TempSync, true},
      {[](FakeIO &io) { io.fail_close_write = true; }, Failure::TempClose,
       true},
      {[](FakeIO &io) {
         io.fail_inspect = kFinal;
         io.fail_inspect_errno = EIO;
       },
       Failure::CardStat, false},
      {[](FakeIO &io) {
         io.add_dirs(std::string(kCard) + "/" + kDay);
         io.files[kTemp] = pattern(3);
         io.fail_unlink = kTemp;
       },
       Failure::TempRemove, false},
  }};
  for (const Case &c : cases) {
    FakeIO io = with_source(bytes);
    c.arm(io);
    fc::Copier copier;
    begin(copier);
    const Run result = run(copier, io);
    assert(result.outcome == Outcome::IoError);
    assert(copier.failure() == c.failure);
    assert(std::strlen(fc::failure_name(c.failure)) > 0);
    assert(io.files.at(kSource) == bytes);
    assert(!io.files.contains(kFinal));
    if (c.failure != Failure::TempRemove)
      assert(!io.files.contains(kTemp));
    assert(c.temp_made ==
           (std::count(io.unlinked.begin(), io.unlinked.end(), kTemp) == 1));
  }

  // The copy is on the card but the source cannot be freed. The next pass
  // finds the identical final file and frees it then.
  FakeIO io = with_source(bytes);
  io.fail_unlink = kSource;
  fc::Copier copier;
  begin(copier);
  expect_outcome(copier, io, Outcome::IoError);
  assert(copier.failure() == Failure::SourceUnlink);
  assert(io.files.at(kFinal) == bytes && io.files.contains(kSource));
  io.fail_unlink.clear();
  begin(copier);
  expect_outcome(copier, io, Outcome::AlreadyIdentical);
  assert(!io.files.contains(kSource) && io.files.at(kFinal) == bytes);
}

// Quarantined files keep their relative path, so they land in the card's
// quarantine. A file at the top of the root needs no directory.
void same_relative_path() {
  const std::string name =
      "quarantine/thing=t1/year=2026_month=11_day=21/a-1-boot-00000001.partial";
  const Bytes bytes = pattern(77);
  FakeIO io = with_source(bytes, name);
  fc::Copier copier;
  begin(copier, name);
  expect_outcome(copier, io, Outcome::Copied);
  assert(io.files.at(std::string(kCard) + "/" + name) == bytes);
  assert(io.made.front() ==
         std::string(kCard) + "/quarantine/thing=t1/year=2026_month=11_day=21");

  FakeIO top = with_source(bytes, "top.parquet");
  fc::Copier flat;
  begin(flat, "top.parquet");
  expect_outcome(flat, top, Outcome::Copied);
  assert(top.made.size() == 1 && top.made[0] == kCard);
  assert(top.files.at(std::string(kCard) + "/top.parquet") == bytes);
}

// A conflict sets the flash file aside on flash, with its sidecar, under
// "flash-conflict/". The next copy takes it to that name on the card. No
// file is deleted or overwritten on the way (#212).
const std::string kAsideName = std::string(fc::kConflictPrefix) + kName;
const std::string kStem = kName.substr(0, kName.size() - 8);
const std::string kSidecar = std::string(kFlash) + "/" + kStem + ".json";
const std::string kAsideSource = std::string(kFlash) + "/" + kAsideName;
const std::string kAsideSidecar =
    std::string(kFlash) + "/" + fc::kConflictPrefix + kStem + ".json";

FakeIO conflicting(const Bytes &bytes, const Bytes &other) {
  FakeIO io = with_source(bytes);
  io.add_dirs(std::string(kCard) + "/" + kDay);
  io.files[kFinal] = other;
  return io;
}

void conflict_set_aside() {
  const Bytes bytes = pattern(2 * fc::kChunkBytes + 9);
  const Bytes other = pattern(bytes.size(), 99);
  const Bytes sidecar = pattern(40, 3);
  static_assert(fc::set_aside_name("flash-conflict/a.parquet"));
  static_assert(!fc::set_aside_name("flash-conflict/"));
  static_assert(!fc::set_aside_name("thing=t1/a.parquet"));
  static_assert(!fc::set_aside_name(nullptr));
  static_assert(fc::aside_retries(fc::AsideError::Rename) &&
                fc::aside_retries(fc::AsideError::Directory) &&
                fc::aside_retries(fc::AsideError::Stat) &&
                !fc::aside_retries(fc::AsideError::Taken) &&
                !fc::aside_retries(fc::AsideError::AlreadyAside) &&
                !fc::aside_retries(fc::AsideError::TooLong) &&
                !fc::aside_retries(fc::AsideError::NotCopied) &&
                !fc::aside_retries(fc::AsideError::NotConflict));
  assert(std::strcmp(fc::aside_error_name(fc::AsideError::Taken), "taken") ==
         0);

  FakeIO io = conflicting(bytes, other);
  io.files[kSidecar] = sidecar;
  fc::Copier copier;
  begin(copier);
  const Run first = run(copier, io);
  assert(first.outcome == Outcome::Conflict);
  const auto aside = copier.set_aside(io);
  assert(aside.has_value() && *aside);
  assert(std::strcmp(copier.relative(), kName.c_str()) == 0);
  assert(io.unlinked.empty());
  assert(!io.files.contains(kSource) && !io.files.contains(kSidecar));
  assert(io.files.at(kAsideSource) == bytes);
  assert(io.files.at(kAsideSidecar) == sidecar);
  assert(io.files.at(kFinal) == other);
  assert(io.made.back() ==
         std::string(kFlash) + "/" + fc::kConflictPrefix + kDay);
  // Once only, a second call has no conflict to act on.
  const auto again = copier.set_aside(io);
  assert(!again && again.error() == fc::AsideError::NotConflict);

  // The next copy takes the set aside file to the same name on the card.
  begin(copier, kAsideName);
  const Run moved = run(copier, io);
  assert(moved.outcome == Outcome::Copied);
  assert(io.files.at(std::string(kCard) + "/" + kAsideName) == bytes);
  assert(io.files.at(kFinal) == other);
  assert(io.unlinked == std::vector<std::string>{kAsideSource});

  // A second conflict under the set aside name stays where it is.
  FakeIO twice = with_source(bytes, kAsideName);
  twice.add_dirs(std::string(kCard) + "/" + fc::kConflictPrefix + kDay);
  twice.files[std::string(kCard) + "/" + kAsideName] = other;
  fc::Copier held;
  begin(held, kAsideName);
  const Run second = run(held, twice);
  assert(second.outcome == Outcome::Conflict);
  const auto stays = held.set_aside(twice);
  assert(!stays && stays.error() == fc::AsideError::AlreadyAside);
  assert(twice.files.at(kAsideSource) == bytes && twice.unlinked.empty());

  // A sidecar that conflicts on its own moves alone.
  FakeIO lone = with_source(sidecar, kStem + ".json");
  lone.add_dirs(std::string(kCard) + "/" + kDay);
  lone.files[std::string(kCard) + "/" + kStem + ".json"] = pattern(40, 4);
  fc::Copier json;
  begin(json, kStem + ".json");
  const Run json_run = run(json, lone);
  assert(json_run.outcome == Outcome::Conflict);
  const auto json_aside = json.set_aside(lone);
  assert(json_aside.has_value() && !*json_aside);
  assert(lone.files.at(kAsideSidecar) == sidecar);
  assert(!lone.files.contains(kSidecar) && lone.unlinked.empty());
}

// Every way a set aside can stop leaves both flash names as they were.
void set_aside_refused() {
  const Bytes bytes = pattern(300);
  const Bytes other = pattern(301, 9);
  const auto conflict = [&](FakeIO &io, fc::Copier &copier,
                            const std::string &name = kName) {
    begin(copier, name);
    const Run result = run(copier, io);
    assert(result.outcome == Outcome::Conflict);
  };

  // No conflict to act on.
  {
    FakeIO io = with_source(bytes);
    fc::Copier copier;
    const auto idle = copier.set_aside(io);
    assert(!idle && idle.error() == fc::AsideError::NotConflict);
    begin(copier);
    const Run copied = run(copier, io);
    assert(copied.outcome == Outcome::Copied);
    const auto after = copier.set_aside(io);
    assert(!after && after.error() == fc::AsideError::NotConflict);
  }
  // The sidecar's set aside name is taken. Nothing moves, not even the
  // Parquet file, so the sidecar never ends up apart from it.
  {
    FakeIO io = conflicting(bytes, other);
    io.files[kSidecar] = pattern(20);
    io.files[kAsideSidecar] = pattern(21);
    fc::Copier copier;
    conflict(io, copier);
    const auto taken = copier.set_aside(io);
    assert(!taken && taken.error() == fc::AsideError::Taken);
    assert(io.files.at(kSource) == bytes);
    assert(io.files.at(kSidecar) == pattern(20));
    assert(io.files.at(kAsideSidecar) == pattern(21));
    assert(!io.files.contains(kAsideSource) && io.unlinked.empty());
  }
  // A refused rename or folder is retried later, the file stays.
  for (const bool folder : {false, true}) {
    FakeIO io = conflicting(bytes, other);
    fc::Copier copier;
    conflict(io, copier);
    io.fail_rename = folder ? 0 : EBUSY;
    io.fail_dirs = folder;
    const auto refused = copier.set_aside(io);
    assert(!refused);
    assert(refused.error() ==
           (folder ? fc::AsideError::Directory : fc::AsideError::Rename));
    assert(errno == (folder ? EIO : EBUSY));
    assert(io.files.at(kSource) == bytes && io.unlinked.empty());
  }
  // A sidecar that cannot be inspected keeps the Parquet file in place.
  {
    FakeIO io = conflicting(bytes, other);
    fc::Copier copier;
    conflict(io, copier);
    io.fail_inspect = kSidecar;
    const auto unsure = copier.set_aside(io);
    assert(!unsure && unsure.error() == fc::AsideError::Stat);
    assert(io.files.at(kSource) == bytes);
  }
  // A quarantined file is never set aside.
  {
    const std::string name = "quarantine/thing=t1/x/a-1-boot-00000001.partial";
    FakeIO io = with_source(bytes, name);
    io.add_dirs(std::string(kCard) + "/quarantine/thing=t1/x");
    io.files[std::string(kCard) + "/" + name] = other;
    fc::Copier copier;
    conflict(io, copier, name);
    const auto quarantined = copier.set_aside(io);
    assert(!quarantined && quarantined.error() == fc::AsideError::NotCopied);
  }
  // A name that would not fit with the prefix stays.
  {
    const std::string name = std::string(375 - 8, 'a') + ".parquet";
    FakeIO io = with_source(bytes, name);
    io.files[std::string(kCard) + "/" + name] = other;
    fc::Copier copier;
    conflict(io, copier, name);
    const auto long_name = copier.set_aside(io);
    assert(!long_name && long_name.error() == fc::AsideError::TooLong);
    assert(io.files.at(std::string(kFlash) + "/" + name) == bytes);
  }
}

// Both flash names of one file, as a power cut inside the set aside rename
// can leave them, may share clusters. Neither is copied or freed.
void set_aside_pair() {
  const Bytes bytes = pattern(500);
  for (const bool from_aside : {false, true}) {
    FakeIO io = with_source(bytes);
    io.files[kAsideSource] = bytes;
    fc::Copier copier;
    begin(copier, from_aside ? kAsideName : kName);
    const Run held = run(copier, io);
    assert(held.outcome == Outcome::IoError && held.steps == 1);
    assert(copier.failure() == Failure::CrosslinkSuspect);
    assert(io.unlinked.empty() && io.made.empty());
    assert(io.files.at(kSource) == bytes && io.files.at(kAsideSource) == bytes);
    assert(!io.files.contains(kTemp));
  }
  // The other name cannot be inspected, the copy stops and is tried later.
  FakeIO io = with_source(bytes);
  io.fail_inspect = kAsideSource;
  fc::Copier copier;
  begin(copier);
  const Run unsure = run(copier, io);
  assert(unsure.outcome == Outcome::IoError);
  assert(copier.failure() == Failure::SourceStat && copier.error() == EIO);
  assert(io.files.at(kSource) == bytes && io.unlinked.empty());
  // A copy that is not paired still restores its temporary name.
  FakeIO clean = with_source(bytes);
  fc::Copier plain;
  begin(plain);
  const Run copied = run(plain, clean);
  assert(copied.outcome == Outcome::Copied);
  assert(clean.files.at(kFinal) == bytes && !clean.files.contains(kTemp));
}

void begin_rules() {
  fc::Copier copier;
  const auto bad = [&](const char *flash, const char *card, const char *name,
                       fc::BeginError want) {
    const auto begun = copier.begin(flash, card, name);
    assert(!begun.has_value() && begun.error() == want);
    assert(!copier.active());
  };
  using enum fc::BeginError;
  bad(kFlash, kCard, nullptr, BadName);
  bad(kFlash, kCard, "", BadName);
  bad(kFlash, kCard, "/abs.parquet", BadName);
  bad(kFlash, kCard, "a//b.parquet", BadName);
  bad(kFlash, kCard, "a/../b.parquet", BadName);
  bad(kFlash, kCard, "../b.parquet", BadName);
  bad(kFlash, kCard, "a/..", BadName);
  bad(kFlash, kCard, "a/./b.parquet", BadName);
  bad(kFlash, kCard, "dir/", BadName);
  bad(kFlash, kCard, "a.parquet.copy", BadName);
  bad(nullptr, kCard, "a.parquet", BadName);
  bad(kFlash, "", "a.parquet", BadName);
  const std::string longest(fc::kRelativeBytes, 'a');
  bad(kFlash, kCard, longest.c_str(), TooLong);
  const std::string root(fc::kPathBytes, 'r');
  bad(kFlash, root.c_str(), "a.parquet", TooLong);
  // A card root that makes the final path one byte too long, and one that
  // fills it exactly, are both refused, the second since the temporary is
  // longer. Neither leaves a truncated final name behind (#211).
  for (const std::size_t final_length : {fc::kPathBytes, fc::kPathBytes - 1}) {
    const std::string name = "a.parquet";
    const std::string card(final_length - 1 - name.size(), 'c');
    const auto begun = copier.begin(kFlash, card.c_str(), name.c_str());
    assert(!begun.has_value() && begun.error() == TooLong);
    assert(*copier.relative() == '\0' && !copier.active());
  }
  // The longest card root whose final path and temporary both still fit.
  {
    const std::string name = "a.parquet";
    const std::string card(fc::kPathBytes - 1 - name.size() - 1 -
                               (sizeof(fc::kTempSuffix) - 1),
                           'c');
    fc::Copier edge;
    const auto begun = edge.begin(kFlash, card.c_str(), name.c_str());
    assert(begun.has_value());
    assert(std::strcmp(edge.relative(), name.c_str()) == 0);
  }
  // A name that fits every path, the temporary included.
  const std::string fits(fc::kRelativeBytes - 1, 'b');
  const auto fit = copier.begin(kFlash, kCard, fits.c_str());
  assert(fit.has_value());
  const auto busy = copier.begin(kFlash, kCard, "a.parquet");
  assert(!busy.has_value() && busy.error() == Busy);
  assert(std::strcmp(copier.relative(), fits.c_str()) == 0);
  // A refused begin after a finished copy names no file, so its failure line
  // never names the file of the last copy.
  for (const char *refused : {longest.c_str(), "a/../b.parquet"}) {
    FakeIO io = with_source(pattern(10));
    fc::Copier done;
    begin(done);
    expect_outcome(done, io, Outcome::Copied);
    assert(std::strcmp(done.relative(), kName.c_str()) == 0);
    const auto again = done.begin(kFlash, kCard, refused);
    assert(!again.has_value());
    assert(*done.relative() == '\0' && !done.active());
  }
}

// The read and hash failures the io_failures table does not reach (#209).
// Each keeps the flash file and leaves the card as it was, apart from the
// temporary this attempt made.
void read_and_hash_failures() {
  const Bytes bytes = pattern(2 * fc::kChunkBytes + 9);
  const auto with_final = [&] {
    FakeIO io = with_source(bytes);
    io.add_dirs(std::string(kCard) + "/" + kDay);
    io.files[kFinal] = bytes;
    return io;
  };
  const auto expect_failure = [&](fc::Copier &copier, FakeIO &io,
                                  Failure failure, const char *name,
                                  int error) {
    expect_outcome(copier, io, Outcome::IoError);
    assert(copier.failure() == failure && copier.error() == error);
    assert(std::strcmp(fc::failure_name(failure), name) == 0);
    assert(io.files.at(kSource) == bytes && !io.files.contains(kTemp));
  };
  // The source read fails while it is hashed against an existing final
  // file of the same size (Stage::HashSource).
  {
    FakeIO io = with_final();
    io.fail_read = kSource;
    fc::Copier copier;
    begin(copier);
    expect_failure(copier, io, Failure::SourceRead, "source-read", EIO);
    assert(io.files.at(kFinal) == bytes && io.unlinked.empty());
  }
  // The existing final file cannot be opened, or read, for the comparison.
  for (const bool open_fails : {true, false}) {
    FakeIO io = with_final();
    (open_fails ? io.fail_open_read : io.fail_read) = kFinal;
    fc::Copier copier;
    begin(copier);
    expect_failure(copier, io, Failure::CardRead, "card-read", EIO);
    assert(io.files.at(kFinal) == bytes && io.unlinked.empty());
  }
  // A hash that cannot finish, for the source or for the file it is checked
  // against, on the comparison path. Nothing is removed.
  for (const unsigned call : {1U, 2U}) {
    FakeIO io = with_final();
    io.fail_hash_finish = call;
    fc::Copier copier;
    begin(copier);
    expect_failure(copier, io, Failure::Hash, "hash", 0);
    assert(io.hash_finishes == call);
    assert(io.files.at(kFinal) == bytes && io.unlinked.empty());
  }
  // The same on the copy path, after the source read or after the readback.
  // Only the temporary of this attempt goes and no final name appears.
  for (const unsigned call : {1U, 2U}) {
    FakeIO io = with_source(bytes);
    io.fail_hash_finish = call;
    fc::Copier copier;
    begin(copier);
    expect_failure(copier, io, Failure::Hash, "hash", 0);
    assert(io.hash_finishes == call && !io.files.contains(kFinal));
    assert(io.unlinked == std::vector<std::string>{kTemp});
  }
}

// cancel() closes what is open and removes the temporary of this attempt,
// since its source is still on flash.
void cancel_mid_copy() {
  const Bytes bytes = pattern(3 * fc::kChunkBytes);
  FakeIO io = with_source(bytes);
  fc::Copier copier;
  begin(copier);
  expect_outcome(copier, io, Outcome::InProgress, 3);
  assert(io.reader_open && io.writer_open);
  copier.cancel(io);
  assert(!copier.active() && !io.reader_open && !io.writer_open);
  assert(!io.files.contains(kTemp) && io.files.at(kSource) == bytes);
  // Idle cancel and step do nothing. A cancelled copy has no failure.
  copier.cancel(io);
  [[maybe_unused]] const Outcome idle = copier.step(io);
  assert(idle == Outcome::IoError);
  assert(!copier.failure().has_value());
  assert(io.files.at(kSource) == bytes);
}

// Within a directory Parquet goes before its sidecar, and only finalized
// files, their sidecars and quarantined files are copied.
void copy_order() {
  using fc::Kind;
  assert(fc::copy_kind("a/b.parquet") == Kind::Parquet);
  assert(fc::copy_kind("a/b.json") == Kind::Sidecar); // "<stem>.json"
  assert(fc::copy_kind("quarantine/t/p/x-1-00000001.partial") ==
         Kind::Quarantined);
  assert(fc::copy_kind("quarantine/t/p/x.json-1-00000001.partial") ==
         Kind::Quarantined);
  assert(!fc::copy_kind("a/b-1.partial").has_value()); // being written
  assert(!fc::copy_kind("a/b.json.partial").has_value());
  assert(!fc::copy_kind("a/b.parquet.copy").has_value()); // a temporary
  assert(!fc::copy_kind("quarantine/t/p/x.partial.copy").has_value());
  assert(!fc::copy_kind("a/notes.txt").has_value());
  assert(!fc::copy_kind("").has_value() && !fc::copy_kind(nullptr).has_value());

  std::vector<std::string> names = {"b.json", "c.txt", "a.json", "b.parquet",
                                    "a.parquet"};
  std::sort(names.begin(), names.end(),
            [](const std::string &a, const std::string &b) {
              return fc::copy_before(a.c_str(), b.c_str());
            });
  assert((names == std::vector<std::string>{"a.parquet", "b.parquet", "a.json",
                                            "b.json", "c.txt"}));
  assert(!fc::copy_before("a.parquet", "a.parquet"));
  assert(fc::copy_before("z.parquet", "a.json"));
}

// Only a write the card refused says the card cannot take writes (#218).
// Every other failure is about one file or the flash side.
void card_write_failures() {
  for (const Failure failure :
       {Failure::Directory, Failure::TempOpen, Failure::TempWrite,
        Failure::TempSync, Failure::TempClose}) {
    assert(fc::card_write_failure(failure, EIO));
    assert(fc::card_write_failure(failure, 0));
  }
  assert(fc::card_write_failure(Failure::Rename, EIO));
  assert(fc::card_write_failure(Failure::Rename, EROFS));
  assert(!fc::card_write_failure(Failure::Rename, EEXIST));
  for (const Failure failure :
       {Failure::Idle, Failure::Scratch, Failure::SourceMissing,
        Failure::SourceStat, Failure::SourceOpen, Failure::SourceRead,
        Failure::SourceSize, Failure::TempRemove, Failure::CardStat,
        Failure::TempRead, Failure::CardRead, Failure::Hash, Failure::Verify,
        Failure::SourceUnlink, Failure::CrosslinkSuspect})
    assert(!fc::card_write_failure(failure, EIO));
  static_assert(fc::card_write_failure(Failure::TempWrite, EIO));
}
} // namespace

int main() {
  happy_path();
  multi_chunk();
  reset_mid_copy();
  verify_mismatch();
  existing_identical();
  crosslink_suspect();
  existing_different();
  source_vanished();
  rename_refused();
  io_failures();
  same_relative_path();
  begin_rules();
  read_and_hash_failures();
  cancel_mid_copy();
  copy_order();
  card_write_failures();
  conflict_set_aside();
  set_aside_refused();
  set_aside_pair();
  return 0;
}
