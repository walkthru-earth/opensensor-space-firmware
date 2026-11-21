#pragma once

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

// LIST over both archive roots (decision record 0004, item 7). LIST walks
// the card root, the legacy root and then the flash root. A flash file is
// listed under the relative name it will have on the card, so the phone sees
// one file wherever it sits. These helpers decide, for one flash name, whether
// the flash walk lists it. They only inspect, they never change a file.
//
// The probe is any type with
//   int inspect(const char *path, struct stat *info)   // 0, or -1 and errno
namespace opensensor::logger::list_merge {

// The wire prefix of names on the legacy root. OPEN routes it there.
inline constexpr char kLegacyPrefix[] = "legacy-parquet/";
// Same bound as the archive walk paths.
inline constexpr std::size_t kPathBytes = 416;

enum class [[nodiscard]] FlashName : std::uint8_t {
  Emit,      // only on flash, list it
  OnCard,    // the card root holds a file of this name, the card walk lists it
  Unprobed,  // the card could not be inspected, list it anyway
  NotListed, // empty, or under the legacy prefix, which OPEN would misroute
};

// True when the flash walk lists the name. A name whose card copy could not
// be checked is listed, since a duplicate entry is better than leaving out a
// file the peer may not have.
[[nodiscard]] constexpr bool emits(FlashName name) {
  return name == FlashName::Emit || name == FlashName::Unprobed;
}

// Decides one flash relative name. card_root is null or empty while the card
// is not usable, then there is nothing to compare with and the name is
// listed. Only the card root is probed. The legacy root has its own prefixed
// names and never hides a flash file.
template <class Probe>
[[nodiscard]] FlashName flash_name(Probe &probe, const char *card_root,
                                   const char *relative) {
  if (relative == nullptr || *relative == '\0' ||
      std::strncmp(relative, kLegacyPrefix, sizeof(kLegacyPrefix) - 1) == 0)
    return FlashName::NotListed;
  if (card_root == nullptr || *card_root == '\0')
    return FlashName::Emit;
  std::array<char, kPathBytes> path{};
  const int length =
      std::snprintf(path.data(), path.size(), "%s/%s", card_root, relative);
  if (length < 0 || static_cast<std::size_t>(length) >= path.size())
    return FlashName::Unprobed;
  struct stat info{};
  if (probe.inspect(path.data(), &info) == 0)
    return S_ISREG(info.st_mode) ? FlashName::OnCard : FlashName::Emit;
  return errno == ENOENT ? FlashName::Emit : FlashName::Unprobed;
}

// LIST_SINCE never leaves out a file the peer may not have seen. A flash file
// carries no card mount epoch, so while flash holds one the answer is the
// full listing. A copy puts files on the card that are new to it but old to
// the peer. The worker covers that by moving its mount epoch after each copy,
// which makes every older cursor full (os_logger_list_since.h).
[[nodiscard]] constexpr bool since_needs_full(bool flash_holds_files) {
  return flash_holds_files;
}

} // namespace opensensor::logger::list_merge
