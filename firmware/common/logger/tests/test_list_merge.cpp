#include "os_logger_list_merge.h"

#include <cassert>
#include <cerrno>
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {
namespace lm = opensensor::logger::list_merge;
using lm::FlashName;

constexpr const char *kCard = "/sd/output";
constexpr const char *kLegacy = "/sd/parquet";
constexpr const char *kName =
    "thing=t1/year=2026/month=11/day=21/a-1-2.parquet";

// Answers stat from a map of paths to modes, and records each probe so the
// test sees which roots were looked at.
struct Probe {
  std::map<std::string, mode_t> entries;
  std::string fail;
  int fail_errno = EIO;
  std::vector<std::string> probed;

  int inspect(const char *path, struct stat *info) {
    probed.emplace_back(path);
    if (fail == path) {
      errno = fail_errno;
      return -1;
    }
    const auto entry = entries.find(path);
    if (entry == entries.end()) {
      errno = ENOENT;
      return -1;
    }
    *info = {};
    info->st_mode = entry->second;
    return 0;
  }
};

std::string on_card(const char *name) {
  return std::string(kCard) + "/" + name;
}

// A name already on the card is listed once, from the card walk.
void duplicate() {
  Probe probe;
  probe.entries[on_card(kName)] = S_IFREG;
  const FlashName name = lm::flash_name(probe, kCard, kName);
  assert(name == FlashName::OnCard && !lm::emits(name));
  assert(probe.probed == std::vector<std::string>{on_card(kName)});
}

// A name only on flash is listed under the relative name it will have on the
// card.
void unique() {
  Probe probe;
  probe.entries[on_card("thing=t1/year=2026/month=11/day=21/other.parquet")] =
      S_IFREG;
  const FlashName name = lm::flash_name(probe, kCard, kName);
  assert(name == FlashName::Emit && lm::emits(name));
  // A directory under the name on the card is not the file.
  probe.entries[on_card(kName)] = S_IFDIR;
  assert(lm::flash_name(probe, kCard, kName) == FlashName::Emit);
}

// The legacy root is its own namespace. A file of the same name there does
// not hide the flash file, only the card root is probed. A flash name under
// the legacy prefix is never listed, since OPEN routes that prefix to the
// legacy root.
void legacy_untouched() {
  Probe probe;
  probe.entries[std::string(kLegacy) + "/" + kName] = S_IFREG;
  assert(lm::flash_name(probe, kCard, kName) == FlashName::Emit);
  assert(probe.probed == std::vector<std::string>{on_card(kName)});

  Probe legacy;
  const std::string prefixed = std::string(lm::kLegacyPrefix) + "a.parquet";
  assert(lm::flash_name(legacy, kCard, prefixed.c_str()) ==
         FlashName::NotListed);
  assert(lm::flash_name(legacy, nullptr, prefixed.c_str()) ==
         FlashName::NotListed);
  assert(legacy.probed.empty());
  // A name that only starts like the prefix is an ordinary name.
  assert(lm::flash_name(legacy, kCard, "legacy-parquet.parquet") ==
         FlashName::Emit);
}

// No card root means nothing to compare with, so every flash name is listed.
// A probe that fails lists the name too, since leaving out a file the peer
// may not have is worse than listing it twice.
void card_unusable() {
  Probe probe;
  assert(lm::flash_name(probe, nullptr, kName) == FlashName::Emit);
  assert(lm::flash_name(probe, "", kName) == FlashName::Emit);
  assert(probe.probed.empty());
  probe.fail = on_card(kName);
  const FlashName name = lm::flash_name(probe, kCard, kName);
  assert(name == FlashName::Unprobed && lm::emits(name));
  // A path too long to build is not probed and is listed.
  const std::string root(lm::kPathBytes, 'r');
  Probe other;
  assert(lm::flash_name(other, root.c_str(), kName) == FlashName::Unprobed);
  assert(other.probed.empty());
}

void bad_names() {
  Probe probe;
  assert(lm::flash_name(probe, kCard, nullptr) == FlashName::NotListed);
  assert(lm::flash_name(probe, kCard, "") == FlashName::NotListed);
  assert(probe.probed.empty());
  assert(!lm::emits(FlashName::NotListed));
}

// LIST_SINCE stays incremental only while flash holds no file and no copy
// has moved a file onto the card since the peer's cursor.
void since_rule() {
  static_assert(!lm::since_needs_full(false, false));
  static_assert(lm::since_needs_full(true, false));
  static_assert(lm::since_needs_full(false, true));
  static_assert(lm::since_needs_full(true, true));
}
} // namespace

int main() {
  duplicate();
  unique();
  legacy_untouched();
  card_unusable();
  bad_names();
  since_rule();
  return 0;
}
