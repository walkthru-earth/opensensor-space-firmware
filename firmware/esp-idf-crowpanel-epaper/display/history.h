#pragma once

// Fixed rings of recent readings for the dashboard sparklines and the 24 h
// charts. One slot per 10 s for the last 2 h and one per minute for the last
// day, for each metric. Slots are keyed by monotonic seconds, never UTC, so
// the charts work before the clock is set. A slot with no sample is NaN and
// draws as a gap, nothing is interpolated.
//
// Rows of earlier boots can be seeded before the first live push. Each boot
// is a segment of one virtual timeline. A seeded boot starts at its first
// row, the live boot at its own monotonic 0, and every segment after the
// first starts one slot after the previous segment's last row in each ring.
// That slot is a restart point, NaN with a restart bit, because the time
// between two boots is unknown and the chart must not claim a length for it.
// Inside a segment slots follow that boot's monotonic seconds exactly, so
// real gaps in a boot still draw at their true length and a 60 s mean never
// mixes two boots.
//
// The object is about 70 KB. The board allocates it in PSRAM, it does no
// allocation of its own and has no locks, so one task owns it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "series.h"
#include "ui_model.h"

namespace opensensor::ui {

enum class Window : std::uint8_t { Recent, Day };

class History {
public:
  static constexpr std::size_t kRecentSlots = 720; // 10 s each, the last 2 h.
  static constexpr std::size_t kDaySlots = 1440;   // 60 s each.
  static constexpr std::uint32_t kRecentStep = 10;
  static constexpr std::uint32_t kDayStep = 60;

  History() { reset(); }

  // Forgets every sample, seeded or live, and every restart point.
  void reset();

  // Earlier boots, before the first push() of this boot. Call seed_boot()
  // once per earlier boot, oldest boot first, then seed() for that boot's rows
  // in rising mono_s order (mono_s is that boot's own monotonic seconds).
  // finish_seed() closes seeding. Restart markers separate each seeded boot
  // from the next and the last one from this boot. Gap length across a restart
  // is unknown, so a marker never claims a duration.
  //
  // Caller rules. Seeding runs on the task that owns the object, or before
  // that task starts, like every other call. Seed only boots that ended before
  // this one, never this boot's own rows, which come through push(). A seeded
  // row older than the previous row of its boot is ignored, like push(). A boot
  // with no seeded rows adds no restart point. seed() before any seed_boot()
  // opens the first boot. seed_boot() and seed() after finish_seed(), push() or
  // advance() are ignored. push() and advance() close seeding themselves when
  // finish_seed() was not called. Without seeded rows nothing changes, push()
  // and advance() behave as if no seeding call was made.
  void seed_boot();
  void seed(std::uint32_t mono_s, std::span<const float, kMetricCount> values);
  void finish_seed();

  // Records one sample taken at `mono_s`, this boot's monotonic seconds. NaN
  // marks a missing value. Slots skipped since the previous push become NaN.
  // A sample in the same slot as the previous one replaces it in the recent
  // ring and joins its mean in the day ring. A time before the previous push
  // is ignored.
  void push(std::uint32_t mono_s, std::span<const float, kMetricCount> values);

  // Moves "now" to `mono_s`, this boot's monotonic seconds, without a sample,
  // so a stopped sensor shows as a growing gap instead of a frozen chart.
  // After seeding, an advance() before the first push() only grows the gap
  // after the restart point, the live boot starts at its own second 0, so
  // the gap is the real time this boot has run without a row. Seeded rows
  // make empty() false, so the caller may now advance before the first live
  // row. As without seeding, a later push() into a slot older than the
  // newest one is ignored, so the caller advances only to a time no row
  // still in flight can predate, today 20 s after the last row.
  void advance(std::uint32_t mono_s);

  // The metric's points, oldest first, with the window's restart bits.
  [[nodiscard]] Series series(Metric m, Window w) const;
  [[nodiscard]] bool empty() const { return !started_; }

private:
  // Where a segment sits on the virtual timeline. A row at boot second
  // `mono_s` lands in recent slot `recent + (mono_s / 10 - first_s / 10)`
  // and day slot `day + (mono_s / 60 - first_s / 60)`. All zero, the state
  // without seeding, maps a time to its plain slot.
  struct Anchor {
    std::uint32_t first_s = 0;
    std::uint32_t recent = 0;
    std::uint32_t day = 0;
  };

  void advance_recent(std::uint32_t slot);
  void advance_day(std::uint32_t slot);
  void move_to(std::uint32_t recent, std::uint32_t day);
  void record(std::uint32_t mono_s,
              std::span<const float, kMetricCount> values);
  // Places a restart point one slot after the newest entry in each ring and
  // anchors the next segment one slot after it, its time `first_s` there.
  void restart(std::uint32_t first_s);

  std::array<std::array<float, kRecentSlots>, kMetricCount> recent_{};
  std::array<std::array<float, kDaySlots>, kMetricCount> day_{};
  // One restart bit per slot, cleared when the ring reuses the slot.
  std::array<std::uint8_t, kRecentSlots / 8> recent_restarts_{};
  std::array<std::uint8_t, kDaySlots / 8> day_restarts_{};
  std::array<float, kMetricCount> day_sum_{};
  std::array<std::uint16_t, kMetricCount> day_count_{};
  Anchor anchor_{};
  std::uint32_t recent_slot_ = 0; // Virtual slot of the newest recent entry.
  std::uint32_t day_slot_ = 0;    // Virtual minute of the newest day entry.
  std::optional<std::uint32_t> last_push_s_; // Newest row of this segment.
  bool started_ = false;
  bool sealed_ = false;     // Seeding closed, live rows only.
  bool boot_opened_ = true; // The next seeded row starts a new segment.
};

static_assert(History::kRecentSlots % 8 == 0 && History::kDaySlots % 8 == 0);

} // namespace opensensor::ui
