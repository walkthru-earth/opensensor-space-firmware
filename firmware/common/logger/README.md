# OSLogger

Opt-in native ESP-IDF logger engine shared by ESP32-S3 board adapters. Add the
common component directories to the board CMake project and declare
`REQUIRES common runtime connectivity logger location`. ESP-IDF and managed dependency
versions are pinned by the shared toolchain lock and component manifests.

## Adapter contract

Include `os_logger.h`. Define a static `oslogger::Field[]` dictionary and fill
`Config` with its schema/dictionary identity, digest, acquisition configuration,
board identity and optional board metadata. The runtime requires `sequence`,
`monotonic_us` and `event_time_utc_us` as INT64, and `clock_epoch` as INT32. Other
standard runtime fields are mapped by name and set only when present with the
matching type. Field units `us_since_unix_epoch` receive TIMESTAMP(MICROS, UTC),
which Apache Iceberg maps to `timestamptz` (v1, v2 and v3). Every column gets Parquet
`field_id` = its 1-based position in the dictionary, so dictionaries are append
only. Never reorder, remove or reuse a position.

Call `begin(config, hooks, sd_mounted)` once after board/storage initialization,
then `start_links(display_detected)` and `poll()` on every main-loop pass.
Descriptors and callbacks must remain alive. `poll()` performs at most one sample
when due, counts skipped deadlines and advances the sequence accordingly. The
hardware acquisition callback fills a zero-initialized fixed-capacity `Row`;
unavailable measurements stay null. It can use board enum indices or the engine's
absent/type-safe `integer`, `counter` and `number` helpers.

The board supplies mounted filesystem capacity callbacks. It may supply paired
bus mutex callbacks, or use the engine's `lock_bus`/`unlock_bus` around shared
transactions; complete display DMA before unlocking. No filesystem mounting,
formatting, peripheral or controller initialization happens here. Output defaults
to `/sd/output`; `legacy_directory` is opt-in for previous archives.

`Config::sample_interval_ms` sets the sample period and the footer
`acquisition.sample_interval_ms`. The default is 10 s, which both boards keep.
It must pass `cadence::valid_sample_interval()` in `os_logger_cadence.h`, 1 s
to 300 s and a divisor of 300 s, so every rotation window stays whole. Row
groups hold one rotation window of rows, at most `cadence::kMaxBatchRows` (90),
which is also the footer `file.row_group_rows_max`. `Config::calibration_id`
fills `acquisition.calibration_id` and defaults to `unknown`. The footer
`thing.deployment_id` is the owner's stored `thing.deployment_id`, snapshotted
per sample in `LocationSample::deployment` and kept per file like the
location. `same_file_labels()` in `os_logger_location.h` closes the file when
either changes. `Config::deployment_id` (default `unknown`) is written while
the stored value is unset (#148). The footer sample
period always comes from the live config value, never a build time default.
`footer::build` in `os_logger_footer.h` writes the one `opensensor` key, see
[mobile-migration.md](../../../docs/shared/mobile-migration.md). `Config::ble_name_prefix` is a required
board constant (`OS-` on both boards) that starts the BLE name, and the mDNS
host is that name in lower case. `begin()` refuses a period, id or
prefix that does not pass these checks. The shared caps a new board must check
are listed in [`firmware/common/README.md`](../README.md#shared-caps-a-new-board-must-check).

Battery health is optional. A board that runs `opensensor::battery::Monitor` from
`firmware/common/power` calls `report_battery(state, mv)` from its collect
hook. The runtime prints `POWER STATE state=<name> mv=<n>` once per change and,
on a critical entry while armed, appends ` action=finalize` and raises the
emergency close described in Battery emergency close below.
`Hooks::power_shed` is an optional no-op by default, called on the main loop
after each change. Nothing here resets, sleeps or turns off radios.

Optional RTC callbacks run only in `begin()` and `poll()` on the calling main
task. Read returns UTC seconds only after rejecting invalid/unset/voltage-low
state; write must verify readback. With no RTC, state is 2 (unusable/absent), UTC
stays null until a real host or network anchor, and no RTC write occurs. A host sync only
sets an atomic pending request on the worker. The main loop writes near the next
whole-second boundary, with a bounded late fallback.

## Archive and transport behavior

One worker owns the filesystem, and receives copied nonblocking control requests
from BLE/LAN. Card data is the origin: finalized files can be copied over serial,
BLE or token-authenticated local LAN without any cloud service. The runtime uses
the existing protocol UUIDs, settings, Thing NVS key and archive sessions.
BLE and up to three authenticated LAN clients retain independent file handles;
OPEN, CLOSE and disconnect affect only the requesting connection. Queued stale
commands and responses are rejected by connection generation. Each open handle
keeps one read-only descriptor with a 4 KiB PSRAM stdio buffer from OPEN until
CLOSE, a replacing OPEN, an archive erase, a remount or the first request on any
link after that peer's connection generation changes. A disconnect alone does not
release it. READ windows seek within it. Both board
mounts reserve these four descriptors (`archive_files.h`), and a storage remount
releases them so the next READ reopens by path. OPEN takes the CRC-32 from a
64-entry PSRAM cache filled at finalize (path, size and mtime must match) and
reads the whole file only on a miss. The boot `PARQUET BEGIN` line reports
`crc32_check=ok` when the ROM CRC routine returns the standard check value.
LIVE emits the `Field::live_key` of every field that declares one, in
field-table order, through `os_logger_live.h`. Absent, invalid and non-finite
measurements are sent as null. Floats print with `Field::decimals` digits after
the point. `begin()` rejects malformed or duplicate keys and decimals above 6.

The RAM batch holds at most 90 rows. Each completed row group is fsynced and a
file holds at most eight row groups. Clock epochs and UTC rotation windows never
mix; Thing location snapshots never mix either. Unknown UTC uses the `unsynced`
Hive tree and rotates on the same window measured on the boot's monotonic clock.
An empty `unsynced/ringless=<boot>/` folder marks a boot whose NVS boot ring
append failed, so a later `LIST_SINCE` still lists that boot's unsynced files.
The logger removes it, best effort, when a later append in the same boot
succeeds. Otherwise it stays until the archive is erased.
Finalization writes/syncs the
footer, checks structure and CRC, then renames to immutable `.parquet`. Interrupted
`.partial` files are retained under `quarantine/<thing>/<partition>/`, at boot
and right after a same-boot failure; a partial whose finalized sibling exists is
left in place as a crosslink suspect. The boot walk is iterative and opens
directories six levels deep. A deeper directory is not entered and prints
`PARQUET ERROR operation=quarantine-depth`. An entry that cannot be inspected
or opened prints `operation=quarantine-entry` and is skipped, and only a
directory read error ends the walk with `operation=quarantine-walk`. Automated logging and sync never repair,
delete or rewrite owner archives, and never format or upload. The separate
physical serial [archive reset](../../../docs/shared/development.md#owner-maintenance)
requires an explicitly requested destructive reset and is unavailable over radio.

The SD space governor (`src/storage_governor.h`) is pure and header-only. Ready
and degraded write. Full finalizes the open file and raises
`PARQUET ALARM storage=full` below the reserve, normally 32 MiB with smaller-card
scaling. Full and retry stop normal card writes but retain a bounded RAM batch. New
rows drop when that batch is full or a partition/window change cannot be committed.
Retry follows any write, close,
rename or mount failure and probes on a 10 s to 10 min backoff, remounting through
the optional `storage_remount` hook. Boards supply `storage_free_bytes`,
`storage_cluster_bytes` and `storage_remount`; `firmware/common/src/fat_volume.h`
has the FatFs helpers. STATUS carries `sd_state` (0 to 3). RAM batches can be lost on reset;
footerless completed row groups need explicit host recovery. fsync is not proof
of power-cut durability.

The optional [location component](../location/README.md) supplies validated H3
cells and their grid centers. OSLogger snapshots cell, resolution (always 8
since #139) and optional declared country at acquisition, copies them through the queue and
uses the first row's snapshot for file metadata. Changing any of those settings
closes the previous file before adding new-location rows, including while a RAM
batch is waiting. Four nullable fields are mapped only when the board dictionary
contains all four: `h3_cell_id` (INT64), `h3_resolution` (INT32),
`location_source` (UINT8) and `geometry` (GEOMETRY, a 21 byte WKB Point at the
cell centre, null exactly when the cell is null). Raw GPS never
enters the logger. Since #88 the `opensensor` footer holds no location, country
or precision. The `geo` footer key gives the bbox of the located rows, and is
left without one when no row has a location.
See [time and location](../../../docs/shared/time-and-location.md) for owner policy.

### GNSS hook contract

`Hooks::gnss_fix` is optional and board-neutral. Its type is
`bool (*)(opensensor::gnss::Fix &fix, void *context)`, with the `Fix` type
from the [gnss component](../gnss/README.md). OSLogger calls it on the main
loop once per row when the board dictionary has the four location fields. It
also calls it on the storage worker for GET_CONFIG page 1
`location.gnss_cell`, so the hook must be safe from any task, and it is never
called from an ISR. It must
copy the newest fix and return true, or return false when there is none. It
must not block, allocate or touch the card. `opensensor::gnss::latest` from
`gnss_reader` already has this shape.

`collect` applies `opensensor::gnss::select_location`. A fixed Thing keeps the
configured cell with `location_source` 1 and ignores the fix. A moving
Thing gets the resolution 8 fix cell with
`location_source` 2 when the fix is at most `kFixMaxAgeUs` (15 s) old, and four
location nulls otherwise. A moving Thing with no hook writes nulls on every
row. The fix cell is written only to the row, never to the location snapshot,
so a GNSS cell change never splits a batch or file. A change of mobility, cell
or country still does.

Serial commands remain `parquet status`, `schema`, `list`, `get`, `time`, `flush`,
`interval` (600/900/1800/3600), `codec none`, `codec lz4`, `codec-test` and `mem`.
`parquet mem` prints the `DIAG tasks` stack high water marks and the `DIAG heap`
line at once, from the board task (see the board README's stack and heap budgets). The
codec choice is saved as the u8 key `codec` in the `parquet` NVS namespace, holding
the Parquet codec value (0 UNCOMPRESSED, 7 LZ4_RAW), and the reply reads
`PARQUET CONFIG codec=... persistent=true`. A failed save keeps the running choice,
prints `PARQUET ERROR operation=codec-store reason=...` and replies
`persistent=false`. At boot `PARQUET CODEC codec=... source=nvs` reports the saved
codec. With no key, or a key that cannot be read, the logger uses LZ4_RAW and prints
`source=default reason=missing|open|read|corrupt`. A bad value is left in place
until the next `codec` command replaces it. The interval is still not saved. Codec
comparison files remain separately labeled duplicate-row benchmark artifacts under
`Config::benchmark_directory` (`/sd/bench`), outside the listed archive. Their 4 KiB
staging buffer is borrowed from PSRAM only while the command runs.

Startup scans archive entries/stats and retained partials on the storage worker,
then emits one bounded `PARQUET SCAN` summary. It does not print every archived
path or `PARQUET LIST END` at boot: an archive-sized USB dump can block sampling
storage until a serial reader drains it. An explicit `parquet list` request
retains full `PARQUET FILE` entries, partial summary and `PARQUET LIST END`.
While a BLE or LAN `LIST` runs, `parquet list` answers
`PARQUET ERROR operation=list reason=busy-list` instead, because the card
reserves directory handles for one walk only. Run it again once the radio
`LIST` ends. `parquet get` reads one file and is not refused.
No filesystem ownership, retention or sampling-clock policy changes with this
output choice.

`parquet owner-pin` is a physical serial-only owner command for a headless
fixed-PIN device. It emits `OS OWNER_PIN` directly to the physical console, bypassing the
shared debug ring and BLE/LAN LOG_TAIL. Random pairing mode instead asks the
owner to use the pairing display. There is no corresponding radio opcode,
CONFIG secret field, or boot PIN print. Host tooling must consume/redact the
response rather than save it in captures or repository artifacts.

The physical serial `parquet wifi-profile` command exports only SSID/PSK bytes
as hex plus Wi-Fi/LAN enable flags in one raw `OS WIFI_PROFILE` line. It bypasses
DebugLog and has no network opcode. `parquet config-hex <hex>` imports up to 511
bytes of whole `key=value` lines through the real shared SET_CONFIG handler:
existing validation, NVS persistence, bond clearing, token-session dropping and
Wi-Fi reconfiguration all keep their established behavior. Its reply is a
secret-free `OS CONFIG ok=1 reboot_required=<0/1>` acknowledgement. Errors expose
only allowlisted key identifiers. Embedded NULs, invalid hex, oversize input and
invalid settings are rejected. Hosts must handle credential output in memory and
exclude it from captures and artifacts.

## Battery emergency close

A critical battery asks the storage worker to finish the open Parquet file
so the rows already buffered survive a brownout. The pure parts live in
`os_logger_power.h` (when to ask and the main loop report),
`os_logger_emergency_close.h` (the intent, outcomes, retry, the worker pass
and its log line) and `os_logger_transfer_gate.h` (how a due close stops a
transfer). `pixi run logger-power-test` covers the first two, and
`archive-sync-test` and `wifi-link-test` drive the gate through the real
archive session and LAN transport.

- One close per discharge. After a critical entry asks for a close, nothing
  re-arms it until the battery recovers to ok, which already needs the low
  threshold plus hysteresis, or to external power. A rebound to low, an
  unknown read or a settling gauge does not re-arm it. Settling also covers
  a gauge reset at runtime, so a recovery in that window re-arms only once
  settling ends. A fresh boot starts armed, so a cut before the close asks
  again on the next critical report.
- `report_battery()` on the main loop only raises an atomic intent and wakes
  the worker. It never touches the card and there is no queue to fill.
- The worker checks the intent at the top of every pass, before samples and
  queued commands. A due close also stops a BLE or LAN LIST or READ at its
  next data frame with the existing busy error, and a serial `parquet get`
  between blocks with `PARQUET ERROR operation=get reason=power-critical
  offset=<n>`. A data frame already waiting for ring space stops waiting at
  the next wait step too, so a peer that stopped draining cannot hold the
  close back for the slow peer bound. Terminal frames are never cut short.
  Clients retry after the close.
- With storage in Retry, the attempt's writable check runs the governor
  probe. That probe finalizes the open telemetry file, remounts the card
  and commits the buffered rows to a new file, which the close then
  finishes. The remount reaches the archive session, so every open READ
  handle is rechecked before its next CHUNK and a changed file ends with
  bad handle instead of stale bytes (#41).
- Each attempt reports one outcome, and `files=<f>` counts every telemetry
  file finalized during it, including one the probe closed. `closed` logs
  `PARQUET FLUSH reason=power-critical result=closed attempt=<n> files=<f>`.
  `nothing-open` (no rows buffered, no file open) is quiet on a first
  attempt. After a retry it logs `result=nothing-open attempt=<n> files=<f>`,
  because an earlier probe may have closed the file. `busy-retry` (storage
  not writable) and `failed` (the finalize write failed) log `PARQUET FLUSH
  reason=power-critical result=<outcome> storage=<state> attempt=<n>
  files=<f> retry_ms=<ms>` and back off 1, 2, 4, 8, 16, 30 and 30 s. After
  eight attempts the worker logs `PARQUET ERROR operation=power-close
  reason=<outcome> attempts=8 files=<f> closed=0`, where `closed=0` says the
  close itself never finished, and drops the intent. A finalize that wrote
  but whose free space reading then failed still counts as closed, and the
  governor records the reading failure. Rows still queued go to the next
  file.

```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> Requested: critical entry while armed
    Requested --> Attempt: worker pass, a stopped transfer or wait
    Attempt --> Probe: storage in Retry
    Probe --> Attempt: finalize, remount, new file, READs rechecked
    Attempt --> Idle: closed or nothing-open
    Attempt --> Backoff: busy-retry or failed
    Backoff --> Attempt: retry due
    Attempt --> Idle: eighth attempt fails, closed=0
```

The retry bound is not yet tied to measured energy. It moves with the
discharge calibration in issue #28.

## Radio LIST job

A BLE or LAN `LIST` runs as a job of bounded steps on the storage worker
(`archive_sync.h`). The worker glue around it lives in
`os_logger_list_job.h` as pure decisions that `archive-sync-test` drives with
the real session and storage governor. It covers three things. Periodic
remounts wait for the job, so a full card is measured without a remount and
a card in Retry is probed without one, and the skipped remount runs after the
job. One request from the job's own link and peer waits in a parked slot
outside the command queue, so requests from other links behind it still run.
Serial `parquet list` is refused while the job runs. A due critical battery
close first ends a running job with ERROR busy "preempted"
(`preempt_for_close`), so the close's Retry remount never reaches a live
walk.

## Worker watchdog

The `parquet-sd` worker stamps a heartbeat on every loop pass, at each BLE/LAN
frame, per directory entry in every archive walk (startup scan, partial
quarantine, `parquet list`, each bounded step of a BLE or LAN `LIST`,
ERASE_ARCHIVE preflight and delete), on both sides
of the board free-space, capacity and remount hooks, and once per second while
paused after an erase. The writer stamps only on confirmed progress, after each
block its output sink writes, after each encoded row group and after each
completed fsync. A sink call that never returns therefore still trips the
watchdog, while a slow write that keeps landing blocks does not. Each stamp
also feeds the worker's task watchdog user. A BLE or LAN frame wait stamps
between its steps, so each transport bounds it. BLE gives up after twice
`progress_timeout_ms` and LAN after twice `slow_peer_ms` without progress
(30 s each by default), so a stuck radio task cannot keep a worker that
writes nothing looking alive.

- `begin()` seeds the heartbeat just before it creates the worker task, so a
  worker that blocks before its first stamp ages out like any other stall.
  The new worker first waits for a task notification that `begin()` gives
  right after it prints `PARQUET BEGIN`, so BEGIN always precedes every
  worker line of that boot. No return sits between the task create and that
  notification, and the wait is bounded at 5 s, below the shortest stall
  timeout. A waiting worker is blocked, so idle tasks run and the supervisor
  keeps feeding its user from the seeded heartbeat. If the wait ever runs
  out, the worker starts anyway and first logs `PARQUET ERROR
  operation=worker-start-wait waited_ms=5000 order=unordered`, since its
  lines may then precede BEGIN.
- `poll()` keeps the passive warning. After 5 s without a stamp it logs
  `PARQUET ERROR operation=worker-stall seconds=N state=... stack_free=...
  watchdog=armed|warn|off`, at most every 30 s.
- With `CONFIG_OS_LOGGER_WORKER_WDT=y` (default) `begin()` registers the worker
  with the ESP-IDF task watchdog through `esp_task_wdt_add_user("parquet-sd")`.
  The task watchdog has one period for all users and the idle tasks
  (`CONFIG_ESP_TASK_WDT_TIMEOUT_S`, 5 s), so a 1 s `esp_timer` supervisor also
  feeds the user on the worker's behalf while the heartbeat is younger than
  `CONFIG_OS_LOGGER_WORKER_WDT_TIMEOUT_S` (default 60 s). A single SD call
  between two stamps, such as a first FAT free-cluster count or a remount, may
  therefore take up to that timeout without a false reset.
- When the heartbeat is older than the timeout, the supervisor stops feeding,
  marks a pending stall in RTC no-init memory and switches the task watchdog to
  panic. Within one more watchdog period the IDF prints
  `task_wdt: ... - parquet-sd (CPU 0/1)`, since a watchdog user has no task
  and so no core of its own, panics, saves the core dump and resets. The panic
  restarts through a software CPU reset, so the boot banner shows
  `rst:0xc (RTC_SW_CPU_RST)`, not a watchdog reset. `esp_reset_reason()`
  still reports `ESP_RST_TASK_WDT`, printed as `reset=6` on the `LOG ring=`
  line, and that value is what the next boot counts as a stall reset. Every
  board table reserves the 64 KiB `coredump` partition, each board
  application requires the `espcoredump` component, and the shared defaults
  enable `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` with a 2660 byte dump stack
  (2560 plus 100 bytes that IDF 6.1 always adds). The board build
  fails when the image lacks the dump, see `tools/check_idf_target.py`. A
  bench task watchdog reset on the Waveshare board with Wi-Fi off wrote a
  24992 byte dump that `coredump-info` decoded. A dump larger than the
  partition is not saved at all, and the size with Wi-Fi, BLE and LAN running
  is not measured yet (issue #49). The first boot after a new dump also
  writes one `COREDUMP` line to the LOG ring, see
  `firmware/common/runtime/AGENTS.md`. Later boots stay quiet for the same
  dump, and the dump stays in flash.
  Idle tasks keep their print-only behavior until a worker stall is declared.
- If the worker stamps again before that period ends, the supervisor recovers.
  It withdraws the pending stall, restores the task watchdog config the IDF
  started with (from `CONFIG_ESP_TASK_WDT_*`, so idle tasks and other users get
  their own panic policy back) and resumes feeding. `poll()` then logs
  `PARQUET WATCHDOG mode=armed consecutive=N reason=recovered`. A later stall
  trips again, and a later task watchdog reset that this firmware did not arm
  is not counted as a worker stall.
- The supervisor never leaves a user armed without a feeder. When the timer
  cannot be created or started, `begin()` deletes the user again and logs
  `mode=warn ... supervised=0`. A refused panic switch withdraws the pending
  stall and retries on the next tick. A refused restore keeps feeding and
  retries until the original config is back.
- The next boot logs `PARQUET ERROR operation=worker-watchdog-reset
  consecutive=N`. Every boot logs one `PARQUET WATCHDOG mode=armed|warn
  timeout_s=... consecutive=... max_consecutive=3 healthy_s=... supervised=1`.
  After three stall resets without a healthy period in between, the worker is
  warn-only: no reset, only the `worker-stall` warnings. A plain reboot does not
  clear the count. `CONFIG_OS_LOGGER_WORKER_WDT_HEALTHY_S` (default 3600 s) of
  continuous liveness clears it, re-arms a warn-only worker and logs
  `PARQUET WATCHDOG mode=armed consecutive=0 reason=healthy`. Power loss also
  clears the RTC record.
- The supervisor decision (`watchdog::supervise`, which returns Nothing, Feed,
  Arm, Trip or Recover), its unwinding (`watchdog::failed`), the guard and the
  expiry rule are pure in `os_logger_watchdog.h`. The start steps
  (`watchdog::prepare`, `watchdog::launch`) and the tick that applies each
  decision (`watchdog::tick`, `watchdog::apply`) live in
  `os_logger_watchdog_apply.h` behind an injected task watchdog and timer, and
  `os_logger.cpp` only wraps the ESP-IDF calls. `pixi run logger-watchdog-test`
  runs that same code with a fake clock and a fake task watchdog that keeps the
  IDF feed rule, where the timer is fed only once every entry, the two idle
  tasks included, has reset. `pixi run logger-work-queue-test` keeps the guard
  and expiry cases. The count `poll()` logs is a copy the supervisor publishes
  each tick, since the guard belongs to the supervisor timer. A forced stall on a bench board is still required
  before claiming the reset, recovery and core dump on hardware.

Read a saved core dump with `tools/idf.sh -C <trial> -B <build dir>
coredump-info` against the ELF that was flashed. Reading the partition does not
write the board.

### Bench stall command

`CONFIG_OS_LOGGER_BENCH_STALL` (default n) adds one serial command to the
worker, `parquet bench-stall <s>`. It prints `PARQUET BENCH stall_s=<s>` and
then sleeps in 100 ms steps without stamping the heartbeat, so the supervisor
sees a real stall. `0` blocks forever, which ends in the task watchdog reset
and core dump above. `1` to `3600` is a returning stall that prints
`PARQUET BENCH stall-end`. Anything else gets
`PARQUET ERROR operation=bench-stall reason=invalid-seconds` or
`reason=seconds-out-of-range` and no stall. The parser and the deadline are
pure in `os_logger_bench_stall.h`, and `pixi run logger-bench-stall-test` runs
them with sanitizers. It also fails when the Kconfig default is not n, when
any shipped `sdkconfig.defaults` sets the option, or when bench code in
`os_logger.cpp` sits outside `#if CONFIG_OS_LOGGER_BENCH_STALL`. With the
option off the compiled `os_logger.cpp` object is the same as before the
command existed.

Any serial user can stop logging with it, so it never ships. Build a bench
image only through the overlay `tools/idf-bench-stall-sdkconfig.defaults`,
which `OS_BENCH=stall` appends after the board defaults. The image lands in
its own `build/<variant>-bench-stall` directory, so the shipped
`build/<variant>` sdkconfig never picks it up, and the flash script only
writes it with the same `OS_BENCH=stall`.

```sh
OS_BENCH=stall pixi run waveshare-build logger
OS_BENCH=stall pixi run waveshare-flash <checked port> <full backup> logger
OS_BENCH=stall pixi run cores3-build
```

Keep the bench ELF, since a core dump decodes only against the exact ELF
(`coredump-info -B <trial>/build/<variant>-bench-stall`). Opening the
Waveshare CH343 console resets the board, so send the command on the same
open that logs the run.

```sh
pixi run capture --port <checked port> --seconds 900 \
  --send-on "PARQUET ROW" "parquet bench-stall 0" --out <artifacts>/stall.log
```

`--send-on PATTERN COMMAND` waits for PATTERN, sends COMMAND once and can
repeat in order. It refuses secret commands and `parquet erase-archive`, and
exits 130 when stopped with Ctrl-C. Flash a shipped build back after the run, then power cycle
once so the RTC stall count returns to `consecutive=0`.

## Verification

`pixi run logger-build-test` compiles the minimal generic ESP32-S3
fixture with all real native shared implementations and rejects accidental M5 library
ownership. It does not flash hardware. The fixture chooses OPI PSRAM purely for
compilation; the board adapter owns the physical board's verified PSRAM mode.
`pixi run python tools/test_logger_status.py --sanitize` tests the actual shared
STATUS formatter with every unsigned counter at its maximum and signed values
at their longest representation. All fields fit the 480-byte JSON limit (476
bytes uncompressed, 471 bytes LZ4); insufficient buffers fail rather than report
truncated JSON as valid. Shared BLE/LAN cached snapshot capacities derive from
that limit, and frames retain the 512-byte cap. BLE snapshots exceeding
negotiated MTU − 3 are cached but not notified; long reads retain the complete
JSON value. The same host gate checks the publishers' actual fit policy at
MTU 23/247/483/517 and payload boundaries.

`pixi run python tools/test_logger_provision.py --sanitize` exercises the real
configuration validator and common control handler with synthetic credentials,
UTF-8/hex round trips, malformed inputs, action dispatch and log-ring secrecy.

Writer/codec/measurement changes still require their sanitizer and contract gates,
and board readback evidence remains in that board's bench record.

`pixi run location-test`, `telemetry-contract-test --sanitize` and
`waveshare-contract-test --sanitize` compile the pinned official H3 sources and
exercise published vectors, privacy coarsening, captured settings, invalid/null
mapping and file metadata. PyArrow and DuckDB agree on located/unset, anchored/
unanchored and uncompressed/LZ4 fixtures for both current board schemas. These
host results do not establish hardware NTP reachability or SD durability.
