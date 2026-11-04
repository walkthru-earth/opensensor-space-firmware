# Shared opt-in logger runtime

OSLogger owns the monotonic sampling deadline, command queue and single archive
worker for consumers that opt into this native ESP-IDF component. A board supplies a measurement
contract, acquisition callback, mounted filesystem and optional bus/RTC hooks.
The native runtime preserves the established logger contract; board pin/peripheral
initialization and each board's schema/provenance remain in its own trial.

- Never include a board trial, M5Unified or a board filesystem/controller initializer here. Storage is already
  mounted and capacity callbacks borrow the board's filesystem instance.
- Preserve the `parquet` NVS namespace, its `thing` string key (owner decision
  31, 2026-10-26, the old `station` key stays unread) and its `codec`
  u8 key (Parquet codec value, 0 or 7). A missing or unreadable `codec` boots
  LZ4_RAW and logs why. It is never erased, and only a codec command rewrites
  it. Radio initialization belongs to OSConnectivity; retain its shared
  identity and protocol contract.
- Only the worker accesses the filesystem after startup. Acquisition and RTC
  callbacks run only on the main task through begin/poll; a host clock command
  must enqueue an RTC request rather than performing hardware IO on the worker.
- Preserve 10-second monotonic sampling on both boards (the
  `Config::sample_interval_ms` default, which `begin()` rejects unless
  `cadence::valid_sample_interval()` passes), absent-field/type-safe runtime setters,
  null UTC before a real anchor, visible drop/deadline/error counts, and immutable
  finalized files. Keep 90-row RAM batches (`cadence::kMaxBatchRows`), rows per
  group from `cadence::rows_per_group()` for the rotation window, the
  eight-row-group writer bound (`kMaxRowGroups`) and fsync after each row group.
  Cadence rules stay pure in `os_logger_cadence.h`; `.partial` files are preserved/quarantined rather
  than repaired or deleted. Do not imply power-cut durability from normal resets.
- Task priorities: board task `kBoardTaskPriority` (2) < `parquet-sd` worker
  (`Config::worker_priority`, 3) < `os-lan` (4). The worker stack stays in
  internal RAM because it writes NVS. `poll()` prints `DIAG tasks` stack
  high-water marks and `DIAG heap` (internal, DMA, PSRAM free, minimum and
  largest block) 60 s after begin, then hourly, and at once on the serial
  `parquet mem` command, which `poll()` answers itself without the worker.
  Append new fields at the end of those lines. A task stack change updates
  `tools/stack_budget.json` (`pixi run stack-budget --write-budgets`).
- The worker is a task watchdog user (`CONFIG_OS_LOGGER_WORKER_WDT`, 60 s).
  A radio LIST runs as bounded `ArchiveSession::step()` calls from the worker
  loop, one per pass after samples and commits. Never walk a whole tree for a
  radio request in one call.
  Any new loop or walk on the worker calls `stamp_heartbeat()` per entry, and
  any new blocking board hook is stamped on both sides. Writer code stamps only
  after confirmed progress, never before a call that may hang. Keep the
  supervisor decision and boot-loop guard pure in `os_logger_watchdog.h`, and
  the start and apply steps in `os_logger_watchdog_apply.h` behind injected
  task watchdog and timer interfaces. The timer callback only wraps the IDF
  calls, and every arm or panic switch it applies has an unwind path. See the README section Worker watchdog.
  The bench only `parquet bench-stall` command stays inside
  `#if CONFIG_OS_LOGGER_BENCH_STALL` (default n), set only by
  `tools/idf-bench-stall-sdkconfig.defaults` through `OS_BENCH=stall`.
- Battery power policy stays pure in `os_logger_power.h`,
  `os_logger_emergency_close.h` and `firmware/common/power`.
  `report_battery()` runs on the main loop and only logs, calls the optional
  `power_shed` hook and raises the emergency close intent. Only the worker
  runs the close, at the top of each pass or after a transfer stops at a
  data frame checkpoint, never from a timer, ISR or the main loop. Only Ok or
  external power re-arms it. Never reset, sleep or drop rows from a power
  state.
- The SD space governor never deletes. A full card pauses and alarms; space
  returning or a successful probe resumes. Keep its logic pure in
  `storage_governor.h`; boards only supply the free, cluster and remount hooks.
- Startup archive scans must not print each filename. Retain a bounded summary
  so offline startup does not depend on a USB reader draining archive output.
  Explicit serial `parquet list` still emits entries and its END marker. It
  is refused while a radio `LIST` job runs, since the FATFS lock budget covers
  one walk. Keep the job's worker decisions pure in `os_logger_list_job.h`,
  and never start a periodic remount while a `LIST` job runs.
- Boot self test (#40). `Config::self_test_field` must hold a valid value,
  the SD card must be ready and BLE started before `self_test_deadline_ms`
  (180 s). A null `self_test_field` skips the sensor check, and a name that is
  not a board field fails it. Once decided, the logger calls
  `ossync::ota::confirm_running_app` first, then prints one SELFTEST line with
  the result and the mark. Keep the verdict pure in
  `firmware/common/src/boot_self_test.h`.
- `Hooks::gnss_fix` is optional and runs on the main loop once per row. It
  copies the newest H3 fix and never blocks or touches the receiver UART. A
  board without a receiver leaves it null, so a moving Thing writes location
  nulls. The per row source rule lives in `firmware/common/gnss`.
- `os_sensor_module.h` is the sensor module API. Generated `<id>_module.h`
  headers include only standard headers, and a board module table includes
  both. `system.module.json` is the
  `system` module manifest for logger owned columns, which gets no generated
  header. See `firmware/common/sensors/AGENTS.md`.
- Board metadata must describe its hardware honestly. PMS5003T has four particle
  bins and ambient temperature/humidity; do not synthesize CoreS3-only bins.
- Copy the coarsened location settings at acquisition into the queued sample.
  Split batches/files when cell, maximum resolution or declared country changes.
  File metadata comes from the file's first sample, never current settings at
  finalization. Preserve nullable location fields before explicit provisioning;
  raw GPS and a discarded finer H3 cell must never enter storage or logs.
- `parquet owner-pin` is physical serial-only and bypasses DebugLog. Never
  expose its response through BLE/LAN, CONFIG JSON, LOG_TAIL, boot output or
  retained host captures. `parquet wifi-profile` likewise writes only directly
  to the physical UART. `parquet config-hex` must reuse common SET_CONFIG
  validation/actions and emit only secret-free acknowledgements/allowlisted
  error keys; never log raw command bodies or decoded credentials.
- The compile fixture is generic ESP32-S3 with no M5 libraries. Compilation is
  not a board measurement. Host/board contract tests and real readback belong to
  the consuming trial's gates and board bench record.
