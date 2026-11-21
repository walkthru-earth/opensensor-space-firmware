#include "os_logger_flash_copy.h"
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

// The LIST merge and the copy back share one path bound, so a flash name the
// merge lists always fits the copy (#210).
static_assert(&lm::kPathBytes == &opensensor::logger::paths::kPathBytes);
static_assert(&opensensor::logger::flash_copy::kPathBytes ==
              &opensensor::logger::paths::kPathBytes);

constexpr const char *kCard = "/sd/output";
constexpr const char *kLegacy = "/sd/parquet";
constexpr const char *kName =
    "thing=t1/year=2026/month=11/day=21/a-1-2.parquet";

// Answers stat from a map of paths to modes, and records each probe so the
// test sees which roots were looked at.
struct Probe {
  std::map<std::string, mode_t> entries;
  std::map<std::string, off_t> sizes;
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
    const auto size = sizes.find(path);
    info->st_size = size == sizes.end() ? 100 : size->second;
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

// LIST_SINCE stays incremental only while flash holds no file. A copy onto
// the card moves the mount epoch instead (test_list_since.cpp).
void since_rule() {
  static_assert(!lm::since_needs_full(false));
  static_assert(lm::since_needs_full(true));
}
} // namespace

// A flash file set aside after a conflict (#212) is listed under its
// "flash-conflict/" name, which the card does not hold, so the peer can fetch
// it next to the card file of the old name. Once the card holds that name
// too, the card walk lists it.
void set_aside_listed() {
  const std::string aside = std::string("flash-conflict/") + kName;
  Probe probe;
  probe.entries[on_card(kName)] = S_IFREG;
  assert(lm::flash_name(probe, kCard, aside.c_str()) == FlashName::Emit);
  assert(probe.probed == std::vector<std::string>{on_card(aside.c_str())});
  probe.entries[on_card(aside.c_str())] = S_IFREG;
  assert(lm::flash_name(probe, kCard, aside.c_str()) == FlashName::OnCard);
}

constexpr const char *kFlash = "/flash/output";
constexpr const char *kSidecar = "thing=t1/year=2026/month=11/day=21/"
                                 "data_0910_0123456789abcdef0123456789abcdef_"
                                 "1-90-0.json";

std::string at(const char *root, const char *name) {
  return std::string(root) + "/" + name;
}

// The sidecar is found where OPEN finds it, the card first, then flash
// (#143). A flash sidecar is listed under the name it has on the card.
void sidecar_lookup() {
  using lm::SidecarMiss;
  Probe card;
  card.entries[at(kCard, kSidecar)] = S_IFREG;
  card.sizes[at(kCard, kSidecar)] = 900;
  auto found = lm::sidecar(card, kCard, kFlash, kSidecar, false);
  assert(found && *found == 900);
  assert(card.probed == std::vector<std::string>{at(kCard, kSidecar)});

  // Only on flash, with or without a card.
  Probe flash;
  flash.entries[at(kFlash, kSidecar)] = S_IFREG;
  for (const char *card_root : {kCard, static_cast<const char *>(nullptr)})
    for (const bool parquet_on_flash : {false, true}) {
      found = lm::sidecar(flash, card_root, kFlash, kSidecar, parquet_on_flash);
      assert(found && *found == 100);
    }
  // Flash not ready.
  found = lm::sidecar(flash, kCard, nullptr, kSidecar, false);
  assert(!found && found.error() == SidecarMiss::Absent);

  // A card sidecar beside a Parquet only on flash is some other file's.
  found = lm::sidecar(card, kCard, kFlash, kSidecar, true);
  assert(!found && found.error() == SidecarMiss::Split);
  static_assert(lm::sidecar_pairs(false, false));
  static_assert(lm::sidecar_pairs(true, false));
  static_assert(lm::sidecar_pairs(true, true));
  static_assert(!lm::sidecar_pairs(false, true));

  // No json beside the Parquet.
  Probe none;
  found = lm::sidecar(none, kCard, kFlash, kSidecar, false);
  assert(!found && found.error() == SidecarMiss::Absent);
  assert(none.probed.size() == 2);

  // A directory of that name, and sizes OPEN refuses.
  Probe odd;
  odd.entries[at(kCard, kSidecar)] = S_IFDIR;
  found = lm::sidecar(odd, kCard, kFlash, kSidecar, false);
  assert(!found && found.error() == SidecarMiss::NotFile);
  odd.entries[at(kCard, kSidecar)] = S_IFREG;
  for (const off_t size :
       {off_t(0), off_t(1), off_t(opensensor::sync::kMaxSidecarBytes) + 1,
        off_t(-1)}) {
    odd.sizes[at(kCard, kSidecar)] = size;
    found = lm::sidecar(odd, kCard, kFlash, kSidecar, false);
    assert(!found && found.error() == SidecarMiss::Size);
  }
  for (const off_t size :
       {off_t(2), off_t(opensensor::sync::kMaxSidecarBytes)}) {
    odd.sizes[at(kCard, kSidecar)] = size;
    found = lm::sidecar(odd, kCard, kFlash, kSidecar, false);
    assert(found && *found == static_cast<std::uint32_t>(size));
  }

  // A card that cannot be inspected ends the lookup, flash is not tried, so
  // LIST never names a file OPEN would not resolve to.
  Probe broken;
  broken.entries[at(kFlash, kSidecar)] = S_IFREG;
  broken.fail = at(kCard, kSidecar);
  found = lm::sidecar(broken, kCard, kFlash, kSidecar, false);
  assert(!found && found.error() == SidecarMiss::Unreadable && errno == EIO);
  assert(broken.probed.size() == 1);
}

int main() {
  duplicate();
  unique();
  legacy_untouched();
  card_unusable();
  bad_names();
  since_rule();
  set_aside_listed();
  sidecar_lookup();
  return 0;
}
