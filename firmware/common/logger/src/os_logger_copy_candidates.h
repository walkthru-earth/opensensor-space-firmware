#pragma once

#include "os_logger_flash_copy.h"

#include <array>
#include <cstddef>
#include <cstring>

// The short sorted list of flash files that copy back takes next (#215).
// One walk of the flash root offers every file that may move now. The list
// keeps the kCapacity first ones in copy order (flash_copy::copy_before), so
// the next copies need no walk. The storage worker walks again only when the
// list is empty or was cleared.
//
// The list is a cache of one walk. clear() drops it whenever the flash root
// may hold other names than that walk saw: a flash commit, a conflict set
// aside, a cancelled copy, a card remount or a wait of one pass. The first
// entry is taken when its copy begins, the copier deletes that flash file
// itself. A taken name that fails stays out, like a name the walk skips.
//
// Pure and host tested, no file access. It lives in the PSRAM copy back
// state, never on a task stack.
namespace opensensor::logger::copy_candidates {

template <std::size_t Capacity> class List {
  static_assert(Capacity > 0);

public:
  static constexpr std::size_t kCapacity = Capacity;
  static constexpr std::size_t kNameBytes = flash_copy::kRelativeBytes;

  // Starts the fill of one walk. Any earlier entries are gone.
  void clear() {
    count_ = 0;
    first_ = 0;
  }

  [[nodiscard]] bool empty() const { return first_ == count_; }
  [[nodiscard]] std::size_t size() const { return count_ - first_; }

  // True when offer() would keep `name`: it fits a relative name and the
  // list has room or `name` goes before the last entry. A caller checks
  // this before a costly test of the name, as the walk does for a sidecar.
  [[nodiscard]] bool would_keep(const char *name) const {
    if (name == nullptr || ::strnlen(name, kNameBytes) >= kNameBytes)
      return false;
    if (first_ != 0)
      return false; // a list being taken from is never refilled
    return count_ < Capacity ||
           flash_copy::copy_before(name, names_[count_ - 1].data());
  }

  // Keeps `name` in copy order when would_keep(name). The last entry drops
  // out when the list is full. False when `name` was not kept.
  bool offer(const char *name) {
    if (!would_keep(name))
      return false;
    std::size_t at = count_ < Capacity ? count_++ : Capacity - 1;
    // Shift later entries down by one, the last one falls off when full.
    while (at > 0 && flash_copy::copy_before(name, names_[at - 1].data())) {
      names_[at] = names_[at - 1];
      --at;
    }
    std::memcpy(names_[at].data(), name, std::strlen(name) + 1);
    return true;
  }

  // The next name to copy, nullptr when the list is empty.
  [[nodiscard]] const char *front() const {
    return empty() ? nullptr : names_[first_].data();
  }

  // Drops the first entry, after its copy began or when it may not move.
  void pop_front() {
    if (!empty())
      ++first_;
  }

private:
  std::array<std::array<char, kNameBytes>, Capacity> names_{};
  std::size_t count_ = 0;
  std::size_t first_ = 0;
};

} // namespace opensensor::logger::copy_candidates
