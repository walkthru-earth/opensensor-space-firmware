#pragma once

// Boot order for the chart history restore, newest boot first. Pure C++23
// with no ESP-IDF, so the host test builds the same code the board runs.
//
// Proofs. Ring boots keep NVS ring order and dated only boots keep UTC order.
// Between the two families a ring boot with a dated file proves the order,
// a boot that is newer or older than it in the ring is newer or older than
// a dated boot whose newest key lies on the other side of that file.
//
// Fallback. Only when the proofs give no answer for a ring boot and a dated
// boot, the card used bytes decide. Every row stores sd_used_bytes, and the
// firmware never deletes archive files, so card usage only grows across
// boots. Boot X is older than boot Y when X's largest used bytes are at or
// below Y's smallest. Overlapping or missing ranges stay unknown. The
// assumption breaks when the owner deletes files from a computer between
// boots, and the worst result is a misordered chart, never a data change.
// A fallback answer never replaces a proof, so it cannot contradict one.
//
// The first pair that neither proofs nor the fallback can order ends the
// list, and older boots stay out.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace opensensor::crowpanel::restore_order {

inline constexpr std::size_t kMaxBoots = 64;

// Card used bytes over one boot, from its oldest file's smallest and its
// newest file's largest sd_used_bytes chunk bound.
struct UsedRange {
  std::int64_t min = 0;
  std::int64_t max = 0;
};

struct BootKey {
  int ring_age = -1;         // Higher is newer, -1 when not in the ring.
  std::uint64_t utc_key = 0; // YYYYMMDDHHMM of its newest dated file, or 0.
  std::optional<UsedRange> used;
};

struct Result {
  std::size_t count = 0;    // boots written to the output, newest first
  bool cut = false;         // a boot that could not be placed ended the list
  std::size_t fallback = 0; // placements decided by used bytes
};

// Caller owned working memory, so the board keeps it off the task stack.
// After order() returns, ring and dated hold the indexes of each family
// newest first, the caller may read them.
struct Scratch {
  std::array<std::uint8_t, kMaxBoots> ring{};  // newest first
  std::array<std::uint8_t, kMaxBoots> dated{}; // newest first
  std::array<std::uint64_t, kMaxBoots> older_max{};
  std::array<std::uint64_t, kMaxBoots> newer_min{};
};

enum class Relation : std::uint8_t { Unknown, Newer, Older };

// How `a` relates to `b` by card used bytes alone.
[[nodiscard]] constexpr Relation by_used(const BootKey &a, const BootKey &b) {
  if (!a.used || !b.used)
    return Relation::Unknown;
  const bool a_older = a.used->max <= b.used->min;
  const bool b_older = b.used->max <= a.used->min;
  if (a_older == b_older)
    return Relation::Unknown;
  return a_older ? Relation::Older : Relation::Newer;
}

// Writes indexes into `boots` to `out`, newest first, until `out` is full or
// a boot cannot be placed. More than kMaxBoots boots places none.
[[nodiscard]] constexpr Result order(std::span<const BootKey> boots,
                                     std::span<std::uint8_t> out,
                                     Scratch &scratch) {
  if (boots.size() > kMaxBoots)
    return {.count = 0, .cut = true, .fallback = 0};
  std::size_t rings = 0, dated = 0;
  for (std::size_t i = 0; i < boots.size(); ++i)
    (boots[i].ring_age >= 0 ? scratch.ring[rings++] : scratch.dated[dated++]) =
        static_cast<std::uint8_t>(i);
  std::sort(scratch.ring.begin(), scratch.ring.begin() + rings,
            [&](std::uint8_t a, std::uint8_t b) {
              return boots[a].ring_age > boots[b].ring_age;
            });
  std::sort(scratch.dated.begin(), scratch.dated.begin() + dated,
            [&](std::uint8_t a, std::uint8_t b) {
              return boots[a].utc_key > boots[b].utc_key;
            });
  // older_max[k], the newest dated key of ring boot k or any older one.
  // newer_min[k], the oldest nonzero dated key of ring boot k or any newer
  // one, 0 when none has a key.
  for (std::size_t k = rings; k-- > 0;) {
    const std::uint64_t key = boots[scratch.ring[k]].utc_key;
    scratch.older_max[k] =
        std::max(key, k + 1 < rings ? scratch.older_max[k + 1] : 0);
  }
  for (std::size_t k = 0; k < rings; ++k) {
    const std::uint64_t key = boots[scratch.ring[k]].utc_key;
    const std::uint64_t before = k ? scratch.newer_min[k - 1] : 0;
    scratch.newer_min[k] = !key ? before : before ? std::min(before, key) : key;
  }
  Result result;
  std::size_t r = 0, d = 0;
  while (result.count < out.size() && (r < rings || d < dated)) {
    bool ring_next = d == dated;
    if (r < rings && d < dated) {
      const BootKey &ring_boot = boots[scratch.ring[r]];
      const BootKey &dated_boot = boots[scratch.dated[d]];
      const bool ring_newer = scratch.older_max[r] > dated_boot.utc_key;
      const bool dated_newer =
          scratch.newer_min[r] && scratch.newer_min[r] < dated_boot.utc_key;
      if (ring_newer != dated_newer) {
        ring_next = ring_newer;
      } else if (!ring_newer) {
        // No proof either way, so the used bytes may decide.
        const Relation relation = by_used(ring_boot, dated_boot);
        if (relation == Relation::Unknown) {
          result.cut = true;
          return result;
        }
        ring_next = relation == Relation::Newer;
        ++result.fallback;
      } else {
        // Proofs that disagree mean the card is inconsistent, stop here.
        result.cut = true;
        return result;
      }
    }
    out[result.count++] = ring_next ? scratch.ring[r++] : scratch.dated[d++];
  }
  return result;
}

} // namespace opensensor::crowpanel::restore_order
