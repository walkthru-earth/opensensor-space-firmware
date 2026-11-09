# Agent map for opensensor-space-firmware

General OpenSensor firmware for ESP32-S3 Things. Boards plug in sensors through
logger hooks and a field dictionary, and air quality is the first sensor set.
CoreS3 and Waveshare V2 each have one active, self-contained native ESP-IDF
application. Do not add Arduino APIs or an Arduino component. Shared code lives
in `firmware/common/`. Device protocol and telemetry contracts live in
`docs/shared/`. Open only the relevant file in `docs/README.md` after this one.

## C++ standard and style

- Language is C++23. Firmware builds as `-std=gnu++23` through
  `os_pin_cxx_standard()` in `firmware/cmake/os_cxx_standard.cmake`, and host
  tests use `-std=c++23` from `tools/host_cxx.py`. Every component we own calls
  the helper right after `idf_component_register()`. Third-party components
  keep the SDK default. `tools/check_idf_target.py` fails a build when a repo
  `.cpp` misses the pin or the four places that name the standard disagree.
- Every component we own also calls `os_map_source_paths()` from
  `firmware/cmake/os_source_paths.cmake`, so the image holds no checkout path.
  The board build fails when a repo C or C++ file misses it, and the CI job
  `repro-paths` builds one board from two paths and compares the images.
- The dialect is pinned because ESP-IDF 6.1 defaults to `gnu++26` and a later
  SDK may move it again. An SDK upgrade must never change the dialect silently.
- Not C++26 yet. As of 2026-10 ISO has not published it, and GCC 15, the host clang and
  cppcheck support only part of it. Revisit when all three cover what we need.
- Prefer `std::span` over pointer plus length, with a fixed extent for wire
  fields. Prefer `std::bit_cast` over `memcpy` type punning.
- Constant tables are `static constexpr`, so they live in flash `.rodata`. Use
  `constinit` only for mutable globals that need static initialization, because
  those sit in DRAM. Use `consteval` for values that must be computed at build time.
- New fallible functions return `std::expected<T, E>` instead of `bool` plus an
  out parameter. `E` is a dedicated error-only `enum class` with no `Ok`, `More`
  or `Done` value, so the existing result enums do not qualify as they are.
- Never call `.value()` on an `expected` or `optional`. It aborts on device under
  `-fno-exceptions` and throws on host. Test `has_value()` or the object itself
  and then use `*`, or use `value_or()`. Other throwing paths such as `at()`,
  `std::stoi` and a failed `new` abort on device for the same reason.
- Put `[[nodiscard]]` on every result enum and on every function that returns an
  `expected` or a status. Use `enum class`, `std::to_underlying` for enum casts
  and `using enum` where it reads better.
- Use designated initializers in declaration order, without mixing in positional
  values. Nest them as `.flags = {.x = 1}`, never `.flags.x = 1`, per the
  [ESP-IDF C++ guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/cplusplus.html#designated-initializers).
  Prefer `std::array` over C arrays and use concepts only for real templates.
- Use `std::string_view` for non-owning text. It is not NUL terminated, so never
  pass `.data()` to `%s` or a C API that expects a C string. Never return a view
  of a temporary or of a local buffer.
- On device, avoid `std::format`, `std::print` and iostream. One
  `std::format_to_n` call cost about 341 KB of image and 5 KB of DIRAM on CoreS3
  (GCC 15.2, measured 2026-10-17), so keep `snprintf`.
- Exceptions and RTTI stay off. Use FreeRTOS tasks with explicit stack, priority
  and core, not `std::thread` or `std::jthread`.
- No heap in hot paths or ISRs. No ranges pipelines in hot code, because `-Og`
  keeps every adaptor layer. No coroutines and no modules.
- Wire and file formats use explicit shifts, never `std::byteswap` or host
  endianness.
- Existing code moves over gradually, one module per PR. These PRs never change
  wire or file formats.

## Task map

| Task | Open next |
| --- | --- |
| CoreS3 pins, build, current bench evidence | `firmware/esp-idf-cores3/AGENTS.md`, `docs/boards/m5stack-cores3/README.md` |
| Waveshare V2 pins, build, current bench evidence | `firmware/esp-idf-waveshare-sim7670g/AGENTS.md`, `docs/boards/waveshare-esp32-s3-sim7670g/README.md` |
| Shared drivers, writer, settings, radio, logger | `firmware/common/AGENTS.md`, then its scoped `AGENTS.md` |
| New ESP32-S3 board, logger hooks, sensor modules and plugin manifests | `docs/shared/board-porting.md`, `firmware/common/logger/README.md`, `docs/shared/plugins/README.md` |
| BLE/LAN and mobile compatibility | `docs/shared/ble-sync-protocol.md`, `../opensensor-space-mobile/AGENTS.md` |
| Telemetry schema, Parquet, UTC, archive paths | `docs/shared/telemetry-pipeline.md`, `docs/shared/table-and-observation-model.md` |
| Router/local NTP, H3 and country/privacy configuration | `docs/shared/time-and-location.md`, `firmware/common/location/AGENTS.md` |
| Moving Thing, GNSS receiver and the `gnss_fix` hook | `firmware/common/gnss/AGENTS.md`, `docs/shared/time-and-location.md` |
| Board STAC File Item and Thing Collection (decided in DR 0001, not built, #142 and #145) | `docs/shared/stac-contract/README.md`, `docs/shared/decisions/0001-standalone-thing-stac.md` |
| LAN OTA, boot self test and core dumps (#40) | `firmware/common/connectivity/AGENTS.md`, `firmware/common/runtime/AGENTS.md`, `docs/shared/security-hardening.md` |
| Host scripts and gates | `tools/AGENTS.md`, `docs/shared/development.md` |

## Code boundaries

- Board trials own pins, peripheral initialization, measurement schema and
  acquisition. Do not borrow sibling trial settings or start competing trials
  without a measured reason. Shared ESP-IDF components remain board-neutral and opt-in.
  Add capabilities through `oslogger::Hooks`, `ossync::ServiceConfig` and board
  sdkconfig defaults, never board-name conditionals in `firmware/common`.
- One owner per peripheral and controller. Do not guess GPIO from another
  board. CoreS3 uses Quad PSRAM. Waveshare V2 uses OPI PSRAM. Inspect each
  board's wiring and bench record before changing pins, power or memory.
- Preserve 10-second monotonic sampling, absent/null measurements and null UTC until
  a real time anchor exists. The storage worker alone accesses files after
  startup. Finalized Parquet is immutable. Retain failed `.partial` files in
  provenance quarantine and leave crosslink suspects in place. A failed STAC
  sidecar `.json.partial` (#142) holds no owner rows and can be rebuilt from its
  Parquet, so the worker removes it at runtime and quarantines one a reset left
  behind, see `firmware/common/logger/AGENTS.md`. The SD governor
  alarms and resumes after space or storage recovers, without automatic deletion. Do not
  silently repair, delete or rewrite owner data.
- The card is the origin. The local phone or laptop archive is the first copy.
  BLE/LAN sync works without internet. Cloud upload is optional. The uploader
  software landed in #129, off by default and not wired into any board yet.
  Wiring it into the storage worker is #144.
- Keep device PIN, Wi-Fi PSK and LAN bearer token out of logs, radio responses,
  captured artifacts and docs. Contract changes require firmware and mobile
  consumers to agree. `docs/shared/ble-sync-protocol.md` owns wire behavior.
  Firmware reports proto 3 and mobile rejects earlier protocols. Preserve
  per-peer generations, ABORT, u32 LIST_END counts and typed disconnect evidence.
  Only the LAN task writes sockets and only the NimBLE host sends BLE responses.
- ESP-IDF source/toolchain is pinned by `tools/idf-dependencies.lock`. Managed
  components use exact manifest pins. Use `pixi run idf-setup`, `cores3-build`
  and `waveshare-build [logger|diagnostic]`. Never erase NVS during migration.
  Retain config namespaces, keys, types and flash partition offsets. The
  Thing id lives in NVS `parquet/thing` (owner decision 31, 2026-10-26). The
  old `parquet/station` key stays in flash unread, never erase it.
- Prefer DHCP/router and configured local NTP before permitted public fallback.
  Never infer internet availability or UTC from Wi-Fi association. Preserve RTC
  and authenticated host anchors offline. Location starts unset. Persist only
  the privacy-coarsened H3 cell and owner-declared country. Snapshot location per
  sample/file and split on changes. Never relabel finalized files.
- Bench claims require dated board evidence with image identity and limits.
  Host tests do not prove SD, radio coexistence or power-cut durability.
- Only ESP32-S3 on the ESP-IDF pinned in `tools/idf-dependencies.lock` is
  verified. Do not claim C3, C6, P4 or other chips in READMEs, manifests or
  Kconfig until a board of that chip has completed the flash checklist below
  and has a dated bench record.
- The writer workspace lives in PSRAM with no internal RAM fallback, it cannot
  fit next to Wi-Fi and NimBLE. A failed allocation prints
  `PARQUET ERROR operation=allocate mem=psram bytes=N purpose=writer-state
  logging=false` and stops logging. Worker heartbeat and watchdog rules live in
  `firmware/common/logger/AGENTS.md`.

## Compatibility, licensing and reviews

- No backward compatibility for now (owner decision 14, 2026-10-26). The phone
  does not keep reading older wire contracts. Each firmware change ships with a
  matched app build and every board is reflashed with it. The contract tests and
  digest gates stay. Any change to LIVE keys, decimals, columns or field order
  ships under a new dictionary digest, and a digest is never reused for a
  different key set. Keep every `live_key` and `decimals` in the board
  `telemetry_fields.inc` so the digest covers them.
- Every change the phone can observe (columns, units, field ids, LIVE keys, log
  lines, versions, digests) gets a newest-first entry in
  `docs/shared/mobile-migration.md` saying what changed, why, and what the app
  must do. Bump only the identity strings the change touches. The dictionary
  string and digest move when `telemetry_fields.inc` changes. The schema string
  moves when columns, types or annotations change. The firmware identity moves
  with each release image.
- The footer `dictionary.uri` (`kDictionaryUri`), the template describedby
  link and the template href (`kTemplateHref`, in the footer from #88) are raw GitHub URLs at a
  full 40 hex commit on main, never `main` or a branch (#89).
  `pixi run pinned-uris-test` proves each commit is on HEAD and holds bytes that
  hash to `dict` or `desc`. A commit URL cannot name its own commit, so a change
  that moves `desc` takes a content commit and then a pin commit written by
  `pixi run pin-uris`. A change that moves `dict` pins twice, the steps are in
  `docs/shared/templates/README.md`. PRs that move dict or desc land with a
  merge commit, everything else squashes. A squash would drop the pinned commit
  from main.
- Parquet columns keep stable `field_id` values (1-based position in the board
  `telemetry_fields.inc`). From the Thing baseline (#83) the rule is append
  only, never reorder, remove or reuse a position (owner answer to bridge
  question 36, 2026-10-26). That baseline dropped the H3 centre columns
  (decision 32) and is a fresh numbering, readers never key on an id from an
  earlier dictionary. Parquet UTC columns (`*_utc_us`) use `TIMESTAMP(MICROS, UTC)` so
  files register in Iceberg v3 tables without rewrites. Do not switch them to
  NANOS, which maps only to v3 `timestamptz_ns` that Spark, Athena and Redshift
  cannot read. LIVE `utc` stays nanoseconds.
- Our code is CC-BY-4.0 with Walkthru.Earth attribution. Third-party code keeps
  its own LICENSE, NOTICE and headers. When a dependency, vendored file or
  pinned version changes, update `THIRD_PARTY_NOTICES.md` from the linker maps.
- Ask the owner before creating a new public repository.
- The merge gate is the local gates in the Host gate section below plus the
  coordinator's review on the PR. CodeRabbit is advisory. When it comments,
  check each inline comment against the code, fix it or explain why not, and
  reply in its thread. Never hold a merge waiting for CodeRabbit. Commit per
  major step and post progress on the tracking issue.
- No em dashes in new or edited docs, commits, PR text or replies.

## Before any physical flash write

Identify the exact board and checked serial port. Before each physical flash
write, run these commands in order.

1. `pixi run ports`
2. `pixi run chip --port <port>`
3. `pixi run flash-id --port <port>`
4. `pixi run efuse --port <port>`
5. `pixi run backup --board <board> --port <port>`

For CoreS3 and Waveshare V2, confirm the backup is **16,777,216 bytes** in
`backup/`. For a new board, extend the backup and flash helpers before supporting
a different capacity. Confirm its backup matches the full flash capacity
reported by `flash-id`. The first four
commands read flash/device state. `chip` can reset a running app on exit. Never
write eFuses or format the card. Never assume flash size from a product name.
CoreS3 native USB readback must use esptool's default baud. 921600 corrupted a
measured read. See the selected board's docs for recovery and power rules.
LAN OTA (#40) writes the inactive app slot over an authenticated LAN session,
and activation rewrites the otadata boot selection (`esp_ota_set_boot_partition`).
Images are not signed and rollback is off, so never claim either.

## Dependency selection

Before choosing or changing SDK/component versions, check the latest stable release and current official documentation. Pin exact verified versions and source identity. Record compatibility exceptions with evidence. Shared components must remain reusable across board applications. Each board owns pins, controller initialization, mount policy and hardware capabilities.

## Host gate

Use the pinned pixi environment (`pixi install`, `pixi run <task>`), not ad hoc
pip packages. Run `pixi run gate`, the same entry point CI runs. It covers
`fmt-check`, `lint` and every host contract task with sanitizers. For writer/schema or
archive changes run the relevant `pixi run parquet-test --sanitize`, `reader-test`,
`telemetry-contract-test --sanitize`, `archive-sync-test` and board contract
gates. Every firmware PR builds both boards. Perform proportional real-device readback
before claiming hardware behavior. Keep captures/exports/binaries in ignored
trial `artifacts/`, never `build/`. Full-card owner copies live in ignored
`exports/`. `docs/shared/development.md` has setup and exact task names.
