#pragma once

#include <cstdint>

namespace opensensor {
namespace utc {
// Codes shared by sync/status consumers. The board supplies any RTC adapter;
// these helpers neither invent an anchor nor change a captured row's epoch.
enum Source : std::int32_t { None = 0, Host = 1, Rtc = 2, Network = 3 };
constexpr std::int64_t kMinEpochSeconds = 1577836800LL; // 2020-01-01 UTC
constexpr std::int64_t kMaxEpochSeconds = 4102444800LL; // 2100-01-01 UTC

constexpr bool supported_epoch(std::int64_t seconds) {
  return seconds >= kMinEpochSeconds && seconds <= kMaxEpochSeconds;
}

// Validate before multiplying; the supported range fits signed nanoseconds.
constexpr bool supported_anchor(std::int64_t seconds, std::uint32_t micros) {
  return supported_epoch(seconds) && micros < 1000000 &&
         (seconds < kMaxEpochSeconds || micros == 0);
}

// No source may set the clock earlier than this source date (#173). It is the
// same date as the phone floor in the app's SmartSyncPolicy, so an RTC that
// lost its date, a stale serial value or a forged SNTP reply from years ago
// cannot start a file. Move it forward in a release, never back.
constexpr std::int64_t kAnchorFloorSeconds = 1793232000LL; // 2026-10-29 UTC

// The check every new anchor passes, from any source.
constexpr bool accepted_anchor(std::int64_t seconds, std::uint32_t micros) {
  return supported_anchor(seconds, micros) && seconds >= kAnchorFloorSeconds;
}

constexpr std::int64_t anchor_ns(std::int64_t seconds, std::uint32_t micros) {
  return seconds * 1000000000LL + static_cast<std::int64_t>(micros) * 1000;
}

constexpr std::int64_t estimate_ns(std::int64_t now_mono_us,
                                   std::int64_t anchor_mono_us,
                                   std::int64_t anchor_utc_ns) {
  return anchor_utc_ns + (now_mono_us - anchor_mono_us) * 1000;
}

// Telemetry rows store UTC as microseconds (Parquet TIMESTAMP(MICROS, UTC),
// the Iceberg timestamptz unit in v1 to v3). Anchors are built from whole
// microseconds, so dividing the nanosecond clock is exact and loses nothing.
constexpr std::int64_t ns_to_us(std::int64_t utc_ns) { return utc_ns / 1000; }
constexpr std::int64_t estimate_us(std::int64_t now_mono_us,
                                   std::int64_t anchor_mono_us,
                                   std::int64_t anchor_utc_ns) {
  return ns_to_us(estimate_ns(now_mono_us, anchor_mono_us, anchor_utc_ns));
}

// Whole-second legacy host requests and transport latency can make harmless
// refreshes differ slightly. Keep the discontinuity threshold below the
// ten-second sampling period; each future row still records its exact anchor.
constexpr std::int64_t kEpochDiscontinuityNs = 2000000000LL;
constexpr bool starts_new_epoch(std::int32_t previous_source,
                                std::int32_t next_source,
                                std::int64_t skew_ns) {
  return previous_source == None ||
         (previous_source == Rtc &&
          (next_source == Host || next_source == Network)) ||
         skew_ns > kEpochDiscontinuityNs || skew_ns < -kEpochDiscontinuityNs;
}

// A move of more than one day against an anchor already held (#173). RTC
// drift and phone or SNTP error are seconds or minutes, never days. The
// logger still takes the anchor, so a wrong RTC can be corrected, but it
// logs the jump and does not copy it to the RTC. A later anchor within a
// day of it is not a jump, and that one is saved.
constexpr std::int64_t kClockJumpNs = 86400LL * 1000000000LL;
constexpr bool clock_jump(std::int32_t previous_source, std::int64_t skew_ns) {
  return previous_source != None &&
         (skew_ns > kClockJumpNs || skew_ns < -kClockJumpNs);
}

// Host and network anchors are copied to the RTC unless they are a jump.
constexpr bool saves_to_rtc(std::int32_t previous_source,
                            std::int32_t next_source, std::int64_t skew_ns) {
  return (next_source == Host || next_source == Network) &&
         !clock_jump(previous_source, skew_ns);
}

constexpr const char *source_name(std::int32_t source) {
  return source == Host      ? "host"
         : source == Rtc     ? "rtc"
         : source == Network ? "network"
                             : "none";
}

// Days since 1970-01-01 for a valid proleptic Gregorian calendar date.
// Howard Hinnant's days_from_civil algorithm; independent of process TZ.
constexpr std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
} // namespace utc
} // namespace opensensor
