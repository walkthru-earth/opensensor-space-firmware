#include <array>
#include <ble_sync.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
#include <os_logger_status.h>

int main() {
  // Exercise the actual policy called by both snapshot publishers.
  assert(!ble::snapshot_notification_fits(0, 517));
  assert(!ble::snapshot_notification_fits(1, 0));
  assert(!ble::snapshot_notification_fits(1, 2));
  assert(ble::snapshot_notification_fits(20, 23));
  assert(!ble::snapshot_notification_fits(21, 23));
  assert(ble::snapshot_notification_fits(244, 247));
  assert(!ble::snapshot_notification_fits(245, 247));
  assert(ble::snapshot_notification_fits(480, 483));
  assert(!ble::snapshot_notification_fits(480, 482));
  assert(ble::snapshot_notification_fits(480, 517));
  assert(!ble::snapshot_notification_fits(481, 517));
  assert(!ble::snapshot_notification_fits(
      std::numeric_limits<std::size_t>::max(), 517));
  oslogger::StatusSnapshot view;
  const auto maximum = std::numeric_limits<std::uint32_t>::max();
  view.uptime_seconds = view.interval_seconds = view.buffered = view.finalized =
      view.dropped = view.errors = view.total_kib = view.used_kib =
          view.heap_free = view.partials = view.quarantined = view.open_rows =
              view.open_groups = view.mount_epoch = maximum;
  view.disconnects = std::numeric_limits<std::uint16_t>::max();
  view.last_disconnect = std::numeric_limits<std::uint8_t>::max();
  // The formatter clamps miss, utc_ms and clk_age_s, so the largest inputs
  // print their clamp bounds. The generation keeps its extra minus sign.
  view.missed = std::numeric_limits<std::int64_t>::max();
  view.generation = std::numeric_limits<std::int32_t>::min();
  view.clock_source = view.rtc_state = std::numeric_limits<std::uint8_t>::max();
  view.quarantine_bytes = std::numeric_limits<std::uint64_t>::max();
  view.utc_ms = view.clock_age_seconds =
      std::numeric_limits<std::int64_t>::max();
  view.failed = view.utc = view.storage_ok = true;
  view.sd_state = oslogger::storage::State::Retry;
  std::array<char, ble::kMaxJson + 16> buffer{};
  for (const auto codec :
       {telemetry::Codec::Uncompressed, telemetry::Codec::Lz4Raw}) {
    view.codec = codec;
    const auto length =
        oslogger::format_status(view, buffer.data(), buffer.size());
    // Keep room for a small key without crossing the 480-byte bound.
    constexpr std::size_t kHeadroom = 10;
    assert(length && length + kHeadroom <= ble::kMaxJson);
    assert(!ble::snapshot_notification_fits(length, 23));
    assert(!ble::snapshot_notification_fits(length, 247));
    assert(ble::snapshot_notification_fits(length, 517));
    assert(length == std::strlen(buffer.data()));
    assert(buffer[length - 1] == '}');
    std::cout << buffer.data() << '\n';
    const auto original = buffer;
    assert(oslogger::format_status(view, buffer.data(), length) == 0);
    assert(buffer[length - 1] == '\0');
    assert(oslogger::format_status(view, buffer.data(), length + 1) == length);
    assert(buffer == original);
    assert(oslogger::format_status(view, buffer.data(), 1) == 0);
    assert(buffer[0] == '\0');
    assert(oslogger::format_status(view, nullptr, 0) == 0);
  }
  view.utc = false;
  view.disconnects = 0;
  const auto length =
      oslogger::format_status(view, buffer.data(), buffer.size());
  assert(length && length <= ble::kMaxJson);
  assert(std::strstr(buffer.data(), "\"utc_ms\":null,\"clk_age_s\":null}"));
  assert(std::strstr(buffer.data(), "\"dc\":0,\"dr\":null,"));
  // Unreachable negative counts print 0 rather than a sign.
  view.utc = true;
  view.missed = view.utc_ms = view.clock_age_seconds = -5;
  view.mount_epoch = 0x1234abcdU;
  assert(oslogger::format_status(view, buffer.data(), buffer.size()));
  assert(std::strstr(buffer.data(), "\"miss\":0,"));
  assert(std::strstr(buffer.data(),
                     "\"ls\":305441741,\"utc_ms\":0,\"clk_age_s\":0}"));
  view.utc = false;
  view.mount_epoch = maximum;
  assert(oslogger::format_status(view, buffer.data(), buffer.size()));
  std::cout << buffer.data() << '\n';
  // sd_state is the documented wire number: 0 ready, 1 degraded, 2 full,
  // 3 retry.
  using oslogger::storage::State;
  const char *const wire[] = {"\"sd_state\":0,", "\"sd_state\":1,",
                              "\"sd_state\":2,", "\"sd_state\":3,"};
  const State states[] = {State::Ready, State::Degraded, State::Full,
                          State::Retry};
  for (std::size_t i = 0; i < 4; ++i) {
    view.sd_state = states[i];
    assert(oslogger::format_status(view, buffer.data(), buffer.size()));
    assert(std::strstr(buffer.data(), wire[i]));
  }

  // The board screen line (#165). A failed begin, no card, a full card, a
  // retrying card and a stopped writer each name why rows are not written.
  using oslogger::logging_stopped_reason;
  static_assert(logging_stopped_reason({}) == "not started");
  static_assert(
      logging_stopped_reason(
          {.started = false, .storage_ok = true, .sd_state = State::Ready}) ==
      "not started");
  static_assert(
      logging_stopped_reason(
          {.started = true, .storage_ok = false, .sd_state = State::Ready}) ==
      "no card");
  static_assert(logging_stopped_reason({.started = true,
                                        .storage_ok = true,
                                        .failed = true,
                                        .sd_state = State::Full}) ==
                "card full");
  static_assert(logging_stopped_reason({.started = true,
                                        .storage_ok = true,
                                        .failed = true,
                                        .sd_state = State::Retry}) ==
                "card retry");
  static_assert(logging_stopped_reason({.started = true,
                                        .storage_ok = true,
                                        .failed = true,
                                        .sd_state = State::Ready}) ==
                "writer stopped");
  // Degraded still writes rows.
  static_assert(
      logging_stopped_reason(
          {.started = true, .storage_ok = true, .sd_state = State::Degraded})
          .empty());
  static_assert(
      logging_stopped_reason(
          {.started = true, .storage_ok = true, .sd_state = State::Ready})
          .empty());
}
