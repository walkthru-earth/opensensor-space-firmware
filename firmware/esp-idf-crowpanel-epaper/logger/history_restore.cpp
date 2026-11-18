#include "history_restore.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <expected>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <limits>
#include <new>
#include <nvs.h>
#include <optional>
#include <span>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include <debug_log.h>
#include <os_logger_cadence.h>
#include <os_logger_list_since.h>
#include <parquet_columns.h>
#include <parquet_reader.h>

#include "restore_order.h"

namespace opensensor::crowpanel {
namespace {

namespace list_since = opensensor::logger::list_since;
using list_since::BootId;
using ui::History;
using ui::kMetricCount;

constexpr std::uint32_t kBudgetS = 24 * 60 * 60;
constexpr std::int64_t kDeadlineUs = 8'000'000;
// The used bytes fallback reads footers for at most half the deadline, so
// rows can still be read after it.
constexpr std::int64_t kStatsUs = kDeadlineUs / 2;
// 24 h at the board's 10 s cadence.
constexpr std::size_t kMaxRows = kBudgetS / 10;
// The largest file the writer makes, kMaxRowGroups groups of at most
// kMaxBatchRows rows.
constexpr std::size_t kFileRows =
    telemetry::kMaxRowGroups * oslogger::cadence::kMaxBatchRows;
constexpr std::size_t kMaxBoots = restore_order::kMaxBoots;
constexpr std::size_t kMaxSeeded = 32;
// 24 h at the shortest 600 s rotation is 144 files.
constexpr std::size_t kMaxBootFiles = 160;
constexpr std::size_t kNameBytes = 96;
constexpr std::size_t kPathBytes = 256;
constexpr std::size_t kDays = 2;
constexpr std::size_t kThingBytes = 37;

// Chart metrics in ui::Metric order.
static constexpr std::array<std::string_view, kMetricCount> kMetricColumns{
    "pm1_mass_ug_m3",           "pm25_mass_ug_m3", "pm4_mass_ug_m3",
    "pm10_mass_ug_m3",          "co2_ppm",         "ambient_temperature_c",
    "relative_humidity_percent"};
static_assert(ui::index(ui::Metric::Pm1) == 0 &&
              ui::index(ui::Metric::Pm25) == 1 &&
              ui::index(ui::Metric::Pm4) == 2 &&
              ui::index(ui::Metric::Pm10) == 3 &&
              ui::index(ui::Metric::Co2) == 4 &&
              ui::index(ui::Metric::Temp) == 5 &&
              ui::index(ui::Metric::Rh) == 6);

enum class [[nodiscard]] Reason : std::uint8_t {
  NoThing,
  Memory,
  Path,
  NoBoots,
  Order,
  Deadline,
  NoRows,
};

const char *reason_name(Reason reason) {
  using enum Reason;
  switch (reason) {
  case NoThing:
    return "no-thing";
  case Memory:
    return "memory";
  case Path:
    return "path";
  case NoBoots:
    return "no-boots";
  case Order:
    return "order-unknown";
  case Deadline:
    return "deadline";
  case NoRows:
    return "no-rows";
  }
  return "unknown";
}

// Why the walk stopped before it ran out of boots.
enum class Cut : std::uint8_t { None, Budget, Rows, Order, Deadline };

const char *cut_name(Cut cut) {
  using enum Cut;
  switch (cut) {
  case None:
    return "none";
  case Budget:
    return "budget";
  case Rows:
    return "rows";
  case Order:
    return "order";
  case Deadline:
    return "deadline";
  }
  return "unknown";
}

enum class [[nodiscard]] FileError : std::uint8_t {
  Open,
  Footer,
  Shape,
  Column,
  Stats
};

struct FileRef {
  std::int64_t first = 0;
  std::uint8_t dir = 0; // 0 unsynced folder, 1 + i day folder i.
  std::array<char, kNameBytes> name{};
};

struct Boot {
  BootId id{};
  int ring_age = -1;         // Higher is newer, -1 when not in the ring.
  std::uint64_t utc_key = 0; // YYYYMMDDHHMM of its newest dated file, or 0.
  // Its oldest and newest finalized file seen, for the used bytes fallback.
  unsigned files = 0;
  bool complete = true; // false when a listing of its files was cut short
  FileRef oldest{};
  FileRef newest{};
  std::optional<restore_order::UsedRange> used;
};

struct Row {
  std::uint32_t mono_s = 0;
  std::array<float, kMetricCount> values{};
};

// All restore state, in PSRAM for the call only.
struct Workspace {
  telemetry::FileSummary summary{};
  std::array<std::uint8_t, 64 * 1024> footer{};
  std::array<std::uint8_t, 32 * 1024> page{};
  std::array<std::int64_t, kFileRows> int64s{};
  std::array<float, kFileRows> floats{};
  std::array<std::int32_t, kFileRows> int32s{};
  std::array<std::uint8_t, kFileRows> valid{};
  std::array<Row, kFileRows> file_rows{};
  std::array<std::uint8_t, kFileRows> file_ok{};
  std::array<Row, kMaxRows> rows{};
  std::array<Boot, kMaxBoots> boots{};
  std::size_t boot_count = 0;
  std::array<restore_order::BootKey, kMaxBoots> keys{};
  // keys[i] is boots[key_boot[i]], boots with no finalized file have no key.
  std::array<std::uint8_t, kMaxBoots> key_boot{};
  restore_order::Scratch scratch{};
  std::array<std::uint8_t, kMaxSeeded> order{}; // newest first
  std::array<std::size_t, kMaxSeeded> starts{}; // first row of each boot
  std::array<FileRef, kMaxBootFiles> files{};
  std::array<std::uint32_t, kDays> days{}; // YYYYMMDD, newest first, 0 none
  std::array<char, kPathBytes> root{};
  std::array<char, kPathBytes> path{};
};

// What the card held, logged on both OS HISTORY lines.
struct Seen {
  unsigned ring_boots = 0;  // boots of the NVS ring
  unsigned dated_boots = 0; // boots with only dated files
  unsigned placed = 0;      // boots the order placed
  unsigned fallback = 0;    // placements decided by used bytes
};

struct Summary {
  unsigned boots = 0;
  unsigned files = 0;
  unsigned skipped = 0;
  std::size_t rows = 0;
  std::uint64_t span_s = 0;
  Cut cut = Cut::None;
};

class Deadline {
public:
  Deadline(std::int64_t start, std::int64_t length) : end_(start + length) {}
  [[nodiscard]] bool expired() const { return esp_timer_get_time() >= end_; }
  // This deadline or `length` from now, whichever comes first.
  [[nodiscard]] Deadline within(std::int64_t length) const {
    Deadline out = *this;
    out.end_ = std::min(end_, esp_timer_get_time() + length);
    return out;
  }

private:
  std::int64_t end_;
};

// Consumes 1 to `max` decimal digits from the front of `text`.
std::optional<std::uint64_t> digits(std::string_view &text, std::size_t max) {
  std::size_t n = 0;
  std::uint64_t value = 0;
  while (n < text.size() && n < max && text[n] >= '0' && text[n] <= '9')
    value = value * 10 + static_cast<std::uint64_t>(text[n++] - '0');
  if (n == 0 || (n < text.size() && text[n] >= '0' && text[n] <= '9'))
    return std::nullopt;
  text.remove_prefix(n);
  return value;
}

bool take(std::string_view &text, std::string_view token) {
  if (!text.starts_with(token))
    return false;
  text.remove_prefix(token.size());
  return true;
}

struct FileName {
  BootId boot{};
  std::optional<std::uint16_t> hhmm; // dated files only
  std::int64_t first = 0;
};

// "data_unsynced_<boot>_<first>-<last>-<attempt>.parquet" or
// "data_HHMM_<boot>_<first>-<last>-<attempt>.parquet". Nothing else, so
// ".partial", ".json" and every other entry are skipped.
std::optional<FileName> parse_file_name(std::string_view text) {
  constexpr std::string_view kSuffix = ".parquet";
  if (!text.ends_with(kSuffix) || !take(text, "data_"))
    return std::nullopt;
  text.remove_suffix(kSuffix.size());
  FileName out;
  if (!take(text, "unsynced_")) {
    if (text.size() < 5 || text[4] != '_')
      return std::nullopt;
    std::string_view clock = text.substr(0, 4);
    const auto hhmm = digits(clock, 4);
    if (!hhmm || !clock.empty() || *hhmm / 100 > 23 || *hhmm % 100 > 59)
      return std::nullopt;
    out.hhmm = static_cast<std::uint16_t>(*hhmm);
    text.remove_prefix(5);
  }
  if (text.size() < 33 || text[32] != '_')
    return std::nullopt;
  std::array<char, 33> hex{};
  std::copy_n(text.data(), 32, hex.data());
  if (!list_since::parse_boot(hex.data(), out.boot))
    return std::nullopt;
  text.remove_prefix(33);
  const auto first = digits(text, 18);
  if (!first || !take(text, "-") || !digits(text, 18) || !take(text, "-") ||
      !digits(text, 10) || !text.empty())
    return std::nullopt;
  out.first = static_cast<std::int64_t>(*first);
  return out;
}

// "<key><width digits>", for the year=, month= and day= folders.
std::optional<unsigned> folder_number(std::string_view name,
                                      std::string_view key, std::size_t width) {
  if (!take(name, key) || name.size() != width)
    return std::nullopt;
  const auto value = digits(name, width);
  if (!value || !name.empty())
    return std::nullopt;
  return static_cast<unsigned>(*value);
}

template <class Visit> bool list_directory(const char *path, Visit &&visit) {
  DIR *dir = ::opendir(path);
  if (!dir)
    return false;
  while (const dirent *entry = ::readdir(dir))
    if (!visit(std::string_view{entry->d_name}))
      break;
  ::closedir(dir);
  return true;
}

template <class... Args>
bool format_path(std::array<char, kPathBytes> &out, const char *format,
                 Args... args) {
  const int length = std::snprintf(out.data(), out.size(), format, args...);
  return length > 0 && static_cast<std::size_t>(length) < out.size();
}

// Keeps the two largest values, largest first.
void keep_top(std::array<std::uint32_t, kDays> &top, std::uint32_t value) {
  if (value == top[0] || value == top[1])
    return;
  if (value > top[0]) {
    top[1] = top[0];
    top[0] = value;
  } else if (value > top[1]) {
    top[1] = value;
  }
}

std::array<std::uint32_t, kDays>
top_folders(const char *path, std::string_view key, std::size_t width) {
  std::array<std::uint32_t, kDays> top{};
  list_directory(path, [&](std::string_view name) {
    if (const auto value = folder_number(name, key, width); value && *value)
      keep_top(top, *value);
    return true;
  });
  return top;
}

// The two newest year=/month=/day= folders. They lie in the two newest
// months of the two newest years, so only those are listed.
void find_days(Workspace &ws) {
  ws.days = {};
  std::array<char, kPathBytes> path{};
  for (const std::uint32_t year : top_folders(ws.root.data(), "year=", 4)) {
    if (!year || !format_path(path, "%s/year=%04lu", ws.root.data(),
                              static_cast<unsigned long>(year)))
      continue;
    for (const std::uint32_t month : top_folders(path.data(), "month=", 2)) {
      std::array<char, kPathBytes> month_path{};
      if (!month || month > 12 ||
          !format_path(month_path, "%s/month=%02lu", path.data(),
                       static_cast<unsigned long>(month)))
        continue;
      for (const std::uint32_t day : top_folders(month_path.data(), "day=", 2))
        if (day && day <= 31)
          keep_top(ws.days, year * 10000 + month * 100 + day);
    }
  }
}

bool day_path(const Workspace &ws, std::uint32_t day,
              std::array<char, kPathBytes> &out) {
  return format_path(out, "%s/year=%04lu/month=%02lu/day=%02lu", ws.root.data(),
                     static_cast<unsigned long>(day / 10000),
                     static_cast<unsigned long>(day / 100 % 100),
                     static_cast<unsigned long>(day % 100));
}

bool unsynced_path(const Workspace &ws, const BootId &boot,
                   std::array<char, kPathBytes> &out) {
  std::array<char, 33> hex{};
  list_since::format_boot(boot, hex);
  return format_path(out, "%s/unsynced/boot=%s", ws.root.data(), hex.data());
}

Boot new_boot(const BootId &id, int ring_age, std::uint64_t utc_key) {
  return {.id = id,
          .ring_age = ring_age,
          .utc_key = utc_key,
          .files = 0,
          .complete = true,
          .oldest = {},
          .newest = {},
          .used = std::nullopt};
}

Boot *find_boot(Workspace &ws, const BootId &id) {
  for (std::size_t i = 0; i < ws.boot_count; ++i)
    if (ws.boots[i].id == id)
      return &ws.boots[i];
  return nullptr;
}

// Keeps the oldest and newest file of `boot` seen so far.
void note_file(Boot &boot, std::string_view name, std::int64_t first,
               std::uint8_t dir) {
  if (name.size() >= kNameBytes)
    return;
  const auto set = [&](FileRef &ref) {
    ref = {.first = first, .dir = dir, .name = {}};
    std::copy(name.begin(), name.end(), ref.name.begin());
  };
  if (boot.files == 0 || first < boot.oldest.first)
    set(boot.oldest);
  if (boot.files == 0 || first > boot.newest.first)
    set(boot.newest);
  ++boot.files;
}

// A dated file of `id` that starts at `key`. When the table is full the
// dated only boot with the oldest key gives way to a newer one.
void note_dated(Workspace &ws, const BootId &id, std::uint64_t key,
                std::string_view name, std::int64_t first, std::uint8_t dir) {
  Boot *boot = find_boot(ws, id);
  if (!boot && ws.boot_count < ws.boots.size()) {
    boot = &ws.boots[ws.boot_count++];
    *boot = new_boot(id, -1, key);
  }
  if (!boot) {
    Boot *oldest = nullptr;
    for (std::size_t i = 0; i < ws.boot_count; ++i)
      if (ws.boots[i].ring_age < 0 &&
          (!oldest || ws.boots[i].utc_key < oldest->utc_key))
        oldest = &ws.boots[i];
    if (!oldest || oldest->utc_key >= key)
      return;
    boot = oldest;
    *boot = new_boot(id, -1, key);
  }
  boot->utc_key = std::max(boot->utc_key, key);
  note_file(*boot, name, first, dir);
}

void scan_dated(Workspace &ws, const Deadline &deadline) {
  for (std::size_t i = 0; i < kDays; ++i) {
    const std::uint32_t day = ws.days[i];
    if (!day || !day_path(ws, day, ws.path))
      continue;
    list_directory(ws.path.data(), [&](std::string_view name) {
      if (const auto file = parse_file_name(name); file && file->hhmm)
        note_dated(ws, file->boot, std::uint64_t{day} * 10000 + *file->hhmm,
                   name, file->first, static_cast<std::uint8_t>(1 + i));
      return !deadline.expired();
    });
  }
}

// The unsynced files of every ring boot. A ring boot with no finalized file
// anywhere (a reset before its first rotation leaves only a ".partial")
// adds no rows, so the order leaves it out instead of stopping at it.
void scan_unsynced(Workspace &ws, const Deadline &deadline) {
  for (std::size_t i = 0; i < ws.boot_count; ++i) {
    Boot &boot = ws.boots[i];
    if (boot.ring_age < 0 || !unsynced_path(ws, boot.id, ws.path))
      continue;
    list_directory(ws.path.data(), [&](std::string_view name) {
      if (const auto file = parse_file_name(name);
          file && !file->hhmm && file->boot == boot.id)
        note_file(boot, name, file->first, 0);
      return !deadline.expired();
    });
    if (deadline.expired())
      boot.complete = false;
  }
}

// The newest kMaxBootFiles finalized files of one boot, newest first.
std::size_t boot_files(Workspace &ws, const Boot &boot,
                       const Deadline &deadline) {
  std::size_t count = 0;
  const auto keep = [&](std::string_view name, std::uint8_t dir) {
    const auto file = parse_file_name(name);
    if (!file || file->boot != boot.id || name.size() >= kNameBytes)
      return;
    FileRef *slot = nullptr;
    if (count < ws.files.size()) {
      slot = &ws.files[count++];
    } else {
      slot = &*std::min_element(
          ws.files.begin(), ws.files.end(),
          [](const FileRef &a, const FileRef &b) { return a.first < b.first; });
      if (slot->first >= file->first)
        return;
    }
    slot->first = file->first;
    slot->dir = dir;
    slot->name = {};
    std::copy(name.begin(), name.end(), slot->name.begin());
  };
  if (boot.ring_age >= 0 && unsynced_path(ws, boot.id, ws.path))
    list_directory(ws.path.data(), [&](std::string_view name) {
      keep(name, 0);
      return !deadline.expired();
    });
  for (std::size_t i = 0; boot.utc_key && i < kDays; ++i) {
    if (!ws.days[i] || !day_path(ws, ws.days[i], ws.path))
      continue;
    list_directory(ws.path.data(), [&](std::string_view name) {
      keep(name, static_cast<std::uint8_t>(1 + i));
      return !deadline.expired();
    });
  }
  std::sort(
      ws.files.begin(), ws.files.begin() + count,
      [](const FileRef &a, const FileRef &b) { return a.first > b.first; });
  return count;
}

bool file_path(Workspace &ws, const Boot &boot, const FileRef &file) {
  std::array<char, kPathBytes> directory{};
  const bool ok = file.dir == 0
                      ? unsynced_path(ws, boot.id, directory)
                      : day_path(ws, ws.days[file.dir - 1], directory);
  return ok &&
         format_path(ws.path, "%s/%s", directory.data(), file.name.data());
}

struct Source {
  int fd = -1;
  ~Source() {
    if (fd >= 0)
      ::close(fd);
  }
};

bool read_at(void *context, std::uint64_t offset, std::uint8_t *dest,
             std::size_t size) {
  const int fd = static_cast<Source *>(context)->fd;
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
      ::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0)
    return false;
  while (size) {
    const ssize_t got = ::read(fd, dest, size);
    if (got <= 0)
      return false;
    dest += got;
    size -= static_cast<std::size_t>(got);
  }
  return true;
}

float as_float(const Workspace &ws, telemetry::PhysicalType type,
               std::size_t i) {
  if (!ws.valid[i])
    return std::numeric_limits<float>::quiet_NaN();
  switch (type) {
  case telemetry::PhysicalType::Float:
    return ws.floats[i];
  case telemetry::PhysicalType::Int32:
    return static_cast<float>(ws.int32s[i]);
  case telemetry::PhysicalType::Int64:
    return static_cast<float>(ws.int64s[i]);
  default:
    return std::numeric_limits<float>::quiet_NaN();
  }
}

// Opens ws.path into `source` and decodes its footer into ws.summary.
std::expected<void, FileError> open_footer(Workspace &ws, Source &source) {
  source.fd = ::open(ws.path.data(), O_RDONLY);
  struct stat info{};
  if (source.fd < 0 || ::fstat(source.fd, &info) != 0 || info.st_size <= 0)
    return std::unexpected(FileError::Open);
  const auto footer = telemetry::read_parquet_footer(
      read_at, &source, static_cast<std::uint64_t>(info.st_size),
      ws.footer.data(), ws.footer.size(), ws.summary);
  if (!footer.ok)
    return std::unexpected(FileError::Footer);
  return {};
}

// The card used bytes over one file, from the sd_used_bytes chunk bounds in
// its footer. No page is read.
std::expected<restore_order::UsedRange, FileError>
used_range(Workspace &ws, const Boot &boot, const FileRef &file) {
  if (!file_path(ws, boot, file))
    return std::unexpected(FileError::Open);
  Source source;
  if (const auto opened = open_footer(ws, source); !opened)
    return std::unexpected(opened.error());
  const auto column = telemetry::find_column(ws.summary, "sd_used_bytes");
  if (!column ||
      ws.summary.columns[*column].type != telemetry::PhysicalType::Int64)
    return std::unexpected(FileError::Column);
  std::optional<restore_order::UsedRange> range;
  for (std::size_t g = 0; g < ws.summary.row_group_count; ++g) {
    const auto &chunk = ws.summary.row_groups[g].columns[*column];
    if (!chunk.has_bounds)
      continue;
    // PLAIN little endian INT64 bits, as the reader decoded them.
    const auto low = std::bit_cast<std::int64_t>(chunk.min);
    const auto high = std::bit_cast<std::int64_t>(chunk.max);
    if (!range)
      range = restore_order::UsedRange{.min = low, .max = high};
    range->min = std::min(range->min, low);
    range->max = std::max(range->max, high);
  }
  if (!range)
    return std::unexpected(FileError::Stats);
  return *range;
}

// The used bytes of one boot, the smallest of its oldest file and the
// largest of its newest file. Unset when either footer has no bounds.
void load_used(Workspace &ws, Boot &boot) {
  if (boot.files == 0)
    return;
  const auto oldest = used_range(ws, boot, boot.oldest);
  if (!oldest)
    return;
  if (boot.oldest.first == boot.newest.first) {
    boot.used = *oldest;
    return;
  }
  const auto newest = used_range(ws, boot, boot.newest);
  if (newest)
    boot.used =
        restore_order::UsedRange{.min = oldest->min, .max = newest->max};
}

// Orders the boots newest first into ws.order, see restore_order.h. Boots
// with no finalized file are left out. The used bytes footers are read only
// when the proofs alone stop early, newest boots of each family first.
restore_order::Result order_boots(Workspace &ws, const Deadline &deadline,
                                  Seen &seen) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < ws.boot_count; ++i) {
    const Boot &boot = ws.boots[i];
    (boot.ring_age >= 0 ? seen.ring_boots : seen.dated_boots) += 1;
    if (boot.files == 0 && boot.complete)
      continue;
    ws.keys[count] = {.ring_age = boot.ring_age,
                      .utc_key = boot.utc_key,
                      .used = std::nullopt};
    ws.key_boot[count++] = static_cast<std::uint8_t>(i);
  }
  const std::span<const restore_order::BootKey> keys{ws.keys.data(), count};
  auto result = restore_order::order(keys, ws.order, ws.scratch);
  if (result.cut) {
    // After order(), scratch.ring and scratch.dated list each family newest
    // first, so a short stats deadline still covers the newest boots.
    std::size_t rings = 0;
    for (std::size_t i = 0; i < count; ++i)
      rings += ws.keys[i].ring_age >= 0 ? 1 : 0;
    const std::size_t dated = count - rings;
    const Deadline stats = deadline.within(kStatsUs);
    for (std::size_t k = 0; k < std::max(rings, dated) && !stats.expired(); ++k)
      for (const bool ring : {true, false}) {
        if (k >= (ring ? rings : dated))
          continue;
        vTaskDelay(1); // let the idle tasks feed the task watchdog
        if (stats.expired())
          break;
        const std::uint8_t key =
            ring ? ws.scratch.ring[k] : ws.scratch.dated[k];
        Boot &boot = ws.boots[ws.key_boot[key]];
        load_used(ws, boot);
        ws.keys[key].used = boot.used;
      }
    result = restore_order::order(keys, ws.order, ws.scratch);
  }
  for (std::size_t k = 0; k < result.count; ++k)
    ws.order[k] = ws.key_boot[ws.order[k]];
  seen.placed = static_cast<unsigned>(result.count);
  seen.fallback = static_cast<unsigned>(result.fallback);
  return result;
}

// Decodes the monotonic time and chart metrics of one file into
// ws.file_rows, in file order. A missing metric column reads as NaN.
std::expected<std::size_t, FileError> read_file(Workspace &ws) {
  Source source;
  if (const auto opened = open_footer(ws, source); !opened)
    return std::unexpected(opened.error());
  const auto mono = telemetry::find_column(ws.summary, "monotonic_us");
  if (!mono ||
      ws.summary.columns[*mono].type != telemetry::PhysicalType::Int64 ||
      ws.summary.num_rows > kFileRows)
    return std::unexpected(FileError::Shape);
  std::array<std::optional<std::size_t>, kMetricCount> metrics{};
  for (std::size_t m = 0; m < kMetricCount; ++m)
    metrics[m] = telemetry::find_column(ws.summary, kMetricColumns[m]);
  const telemetry::ColumnOutput out{.floats = ws.floats,
                                    .int64s = ws.int64s,
                                    .int32s = ws.int32s,
                                    .valid = ws.valid};
  std::size_t offset = 0;
  for (std::size_t g = 0; g < ws.summary.row_group_count; ++g) {
    const std::size_t rows = ws.summary.row_groups[g].num_rows;
    if (rows > kFileRows - offset)
      return std::unexpected(FileError::Shape);
    if (!telemetry::read_column(read_at, &source, ws.summary, g, *mono, ws.page,
                                out))
      return std::unexpected(FileError::Column);
    for (std::size_t i = 0; i < rows; ++i) {
      const std::int64_t us = ws.int64s[i];
      ws.file_ok[offset + i] = ws.valid[i] && us >= 0;
      ws.file_rows[offset + i].mono_s =
          static_cast<std::uint32_t>(std::min<std::int64_t>(
              us / 1'000'000, std::numeric_limits<std::uint32_t>::max()));
    }
    for (std::size_t m = 0; m < kMetricCount; ++m) {
      const bool present =
          metrics[m] && telemetry::read_column(read_at, &source, ws.summary, g,
                                               *metrics[m], ws.page, out);
      for (std::size_t i = 0; i < rows; ++i)
        ws.file_rows[offset + i].values[m] =
            present ? as_float(ws, ws.summary.columns[*metrics[m]].type, i)
                    : std::numeric_limits<float>::quiet_NaN();
    }
    offset += rows;
  }
  return offset;
}

bool read_identity(std::array<char, kThingBytes> &thing,
                   std::optional<list_since::Ring> &ring) {
  nvs_handle_t settings;
  if (nvs_open("parquet", NVS_READONLY, &settings) != ESP_OK)
    return false;
  std::size_t length = thing.size();
  const bool found =
      nvs_get_str(settings, "thing", thing.data(), &length) == ESP_OK;
  std::array<std::uint8_t, list_since::kRingBlobBytes> blob{};
  std::size_t blob_length = blob.size();
  if (nvs_get_blob(settings, "boots", blob.data(), &blob_length) == ESP_OK)
    ring = list_since::Ring::decode(std::span{blob.data(), blob_length});
  nvs_close(settings);
  // The logger's lower case UUID, so the path can hold nothing else.
  return found && length == thing.size() &&
         std::all_of(thing.begin(), thing.end() - 1, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-';
         });
}

// Reads the newest rows of the boots in ws.order, newest boot first, and
// stores them at the end of ws.rows in time order. ws.starts[k] is the first
// row of the k-th boot.
Summary read_boots(Workspace &ws, std::size_t ordered,
                   const Deadline &deadline) {
  Summary summary;
  std::size_t pos = ws.rows.size();
  std::int64_t remaining = kBudgetS;
  for (std::size_t k = 0; k < ordered && summary.cut == Cut::None; ++k) {
    const Boot &boot = ws.boots[ws.order[k]];
    const std::size_t end = pos;
    std::optional<std::uint32_t> last_s;
    std::uint32_t cutoff = 0;
    bool done = false;
    const std::size_t files = boot_files(ws, boot, deadline);
    for (std::size_t f = 0; f < files && !done; ++f) {
      vTaskDelay(1); // let the idle tasks feed the task watchdog
      if (deadline.expired()) {
        summary.cut = Cut::Deadline;
        break;
      }
      if (!file_path(ws, boot, ws.files[f])) {
        ++summary.skipped;
        continue;
      }
      const auto rows = read_file(ws);
      if (!rows) {
        ++summary.skipped;
        continue;
      }
      ++summary.files;
      for (std::size_t i = *rows; i-- > 0;) {
        if (!ws.file_ok[i])
          continue;
        const std::uint32_t mono_s = ws.file_rows[i].mono_s;
        if (!last_s) {
          last_s = mono_s;
          cutoff = mono_s > remaining
                       ? static_cast<std::uint32_t>(mono_s - remaining)
                       : 0;
        }
        if (mono_s < cutoff) {
          done = true;
          continue;
        }
        if (pos == 0) {
          summary.cut = Cut::Rows;
          done = true;
          break;
        }
        ws.rows[--pos] = ws.file_rows[i];
      }
    }
    if (pos == end)
      continue;
    const std::uint32_t span =
        *last_s > ws.rows[pos].mono_s ? *last_s - ws.rows[pos].mono_s : 0;
    ws.starts[summary.boots++] = pos;
    summary.span_s += span;
    remaining -= static_cast<std::int64_t>(span) + History::kDayStep;
    if (summary.cut == Cut::None && remaining <= 0)
      summary.cut = Cut::Budget;
  }
  summary.rows = ws.rows.size() - pos;
  return summary;
}

std::expected<Summary, Reason> restore(Workspace &ws, History &history,
                                       const char *output_directory,
                                       const Deadline &deadline, Seen &seen) {
  std::array<char, kThingBytes> thing{};
  std::optional<list_since::Ring> ring;
  if (!read_identity(thing, ring))
    return std::unexpected(Reason::NoThing);
  if (!format_path(ws.root, "%s/thing=%s", output_directory, thing.data()))
    return std::unexpected(Reason::Path);
  if (ring)
    for (std::size_t age = 0; age < ring->size(); ++age) {
      const BootId &id = ring->at(age);
      if (list_since::is_zero(id) || id == list_since::kUnknownBoot ||
          find_boot(ws, id))
        continue;
      ws.boots[ws.boot_count++] = new_boot(id, static_cast<int>(age), 0);
    }
  find_days(ws);
  scan_dated(ws, deadline);
  scan_unsynced(ws, deadline);
  if (ws.boot_count == 0)
    return std::unexpected(Reason::NoBoots);
  const auto ordered = order_boots(ws, deadline, seen);
  if (ordered.count == 0)
    return std::unexpected(ordered.cut ? Reason::Order : Reason::NoRows);
  Summary summary = read_boots(ws, ordered.count, deadline);
  if (summary.cut == Cut::None && ordered.cut)
    summary.cut = Cut::Order;
  if (summary.rows == 0)
    return std::unexpected(summary.cut == Cut::Deadline ? Reason::Deadline
                                                        : Reason::NoRows);
  for (std::size_t k = summary.boots; k-- > 0;) {
    const std::size_t end = k ? ws.starts[k - 1] : ws.rows.size();
    history.seed_boot();
    for (std::size_t i = ws.starts[k]; i < end; ++i)
      history.seed(ws.rows[i].mono_s, ws.rows[i].values);
  }
  history.finish_seed();
  return summary;
}

} // namespace

void restore_history(History &history, const char *output_directory) {
  const std::int64_t started = esp_timer_get_time();
  const Deadline deadline{started, kDeadlineUs};
  void *memory = heap_caps_malloc(sizeof(Workspace), MALLOC_CAP_SPIRAM);
  std::expected<Summary, Reason> result = std::unexpected(Reason::Memory);
  Seen seen;
  if (memory) {
    auto *ws = new (memory) Workspace();
    result = restore(*ws, history, output_directory, deadline, seen);
    ws->~Workspace();
    heap_caps_free(memory);
  }
  const auto ms =
      static_cast<unsigned long>((esp_timer_get_time() - started) / 1000);
  if (!result) {
    oslog.printf("OS HISTORY restore=none reason=%s ms=%lu ring_boots=%u "
                 "dated_boots=%u placed=%u fallback=%u\n",
                 reason_name(result.error()), ms, seen.ring_boots,
                 seen.dated_boots, seen.placed, seen.fallback);
    return;
  }
  oslog.printf("OS HISTORY restored boots=%u files=%u rows=%u span_s=%lu "
               "ms=%lu skipped=%u cut=%s ring_boots=%u dated_boots=%u "
               "placed=%u fallback=%u\n",
               result->boots, result->files,
               static_cast<unsigned>(result->rows),
               static_cast<unsigned long>(result->span_s), ms, result->skipped,
               cut_name(result->cut), seen.ring_boots, seen.dated_boots,
               seen.placed, seen.fallback);
}

} // namespace opensensor::crowpanel
