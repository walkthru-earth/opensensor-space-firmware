# Shared development workflow

This repository targets ESP32-S3 with native **ESP-IDF 6.1**. Application source
uses no Arduino APIs or Arduino component. Board pins, PSRAM mode, power
sequencing, storage mounting and hardware evidence belong to the board adapter;
the eight opt-in components (OSCommon, OSSensors, OSRuntime, OSConnectivity,
OSLogger, OSLocation, `power` and `upload`) belong to [`firmware/common`](../../firmware/common/README.md).

## Host and toolchain

Host dependencies are pinned in root `pixi.toml` and `pixi.lock`. The manifest
sets `requires-pixi = ">=0.84.1"` and CI runs pixi **0.84.1**, so an older pixi
stops with a clear error. Start with
`pixi install`; run host and firmware commands through `pixi run`. Add host
packages with `pixi add`. The SDK is installed separately by the pinned
`tools/setup-idf.sh`; `tools/idf.sh` enters its own Python/compiler environment
and invokes `idf.py`, while host validators continue to use Pixi.

```sh
pixi install
pixi run idf-setup
pixi run cores3-build
pixi run waveshare-build
pixi run waveshare-build diagnostic
```

| Board adapter | Source | Configuration | Hardware scope |
| --- | --- | --- | --- |
| [CoreS3](../../firmware/esp-idf-cores3/README.md) | `bringup/main.cpp` and board telemetry adapter | 16 MB flash, Quad PSRAM, USB Serial/JTAG console, native M5 components and SDSPI on existing SPI2 | Short native USB Parquet/SD/network UTC check passed; phone transfers/endurance pending; [image evidence](../boards/m5stack-cores3/bench-verified.md#board-1-first-native-esp-idf-flash-and-readback) |
| [Waveshare V2](../../firmware/esp-idf-waveshare-sim7670g/README.md) | `logger/main.cpp` or `diagnostic/main.cpp`, board IO adapter | 16 MB flash, OPI PSRAM for logger, no PSRAM for diagnostic, UART0 console and one-bit SDMMC | Native qualification pending; historical Arduino images in the board bench record |

`tools/idf-dependencies.lock` pins ESP-IDF 6.1 and its exact source commit.
Component manifests pin M5Unified **0.3.0**, M5GFX **0.3.0**, mDNS **1.15.1**
and Waveshare led_strip **3.1.1**. `pixi run telemetry-contract-test` fails
when the CoreS3 M5 versions in `dependencies.env`, the `os_cores3` manifest,
the footer `kConfiguration` and `dependencies.lock` disagree. The location
component pins [embedded-h3-location](https://github.com/walkthru-earth/embedded-h3-location)
**v2.0.0** as a git dependency, and each board `dependencies.lock` records its
commit. The library vendors official H3 **4.5.0** with exact source and license
hashes in its own `vendor/h3/manifest.json`. Host tests build the same commit
through `tools/h3_library.py`, which clones it once per locked commit into
`${XDG_CACHE_HOME:-~/.cache}/opensensor-space-firmware/embedded-h3-location/<commit>/`
and fails when the board locks disagree. It reads the locks of every app in
the board registry.
Policy is to pin the latest stable release of every dependency. Rechecking on
**2026-11-09** confirmed mDNS **1.15.1**, led_strip **3.1.1**, ESP-IDF **6.1**
and H3 **4.5.0** as latest, and M5Unified and M5GFX **0.3.0** as their latest
releases. Dated device evidence records which component versions each
qualified image used. LZ4 is vendored **1.11.0** with checked hashes, the
[official latest release](https://github.com/lz4/lz4/releases/tag/v1.11.0)
when checked on 2026-11-09.
Commit each board’s generated `dependencies.lock` to pin transitive component
resolution. The Waveshare diagnostic variant keeps its own in
`diagnostic/dependencies.lock`, so building it leaves the logger lock alone. Downloaded
`managed_components/` and build outputs are ignored. Use the [ESP-IDF 6.1 programming guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/index.html),
[native NimBLE APIs](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/bluetooth/nimble/index.html)
and [netif/SNTP manual](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/network/esp_netif_programming.html)
when changing integrations.

`tools/idf-env.sh` derives every SDK path for `idf-setup` and the build
wrappers from values pixi sets (`PIXI_WORKSPACE_NAME`, `PIXI_WORKSPACE_ROOT`,
see the [pixi environment variables](https://pixi.sh/latest/reference/environment_variables/)).
Large downloads are shared by every checkout and worktree. State bound to one
checkout's Python stays in that checkout.

| State | Scope | Default | Override |
| --- | --- | --- | --- |
| ESP-IDF source clone | Shared | `${XDG_CACHE_HOME:-~/.cache}/opensensor-space-firmware/toolchains` | `OS_TOOLCHAIN_ROOT`, or `OS_IDF_PATH` for one checkout |
| SDK compilers and tools | Shared | `~/.espressif` | `OS_IDF_TOOLS_PATH` |
| Pixi host environment | Per checkout | `.pixi/envs/default` | pixi config |
| ESP-IDF Python venv | Per checkout | `.pixi/idf-python-env/idf<X.Y>_py<X.Y>_env` | `OS_IDF_PYTHON_ENV_PATH` |

The wrapper still enforces the exact source commit for any `OS_IDF_PATH`. A venv
links to the interpreter that created it, so each checkout owns one, and
`tools/idf.sh` rebuilds it with the SDK's own `idf_tools.py install-python-env`
when it is missing, its Python link is dangling, or its `pyvenv.cfg` home is
not the Pixi Python of the current checkout. A new worktree needs no manual
step. The venv path never reaches the image, `repro-paths` checks that.
Each board owns its sdkconfig defaults, partitions and manifest dependencies.
Sharing a download cache does not make a sibling board a dependency.

The `[cache]` table and `PIXI_CACHE_DIR` move only pixi's own package caches,
see [pixi configuration](https://pixi.sh/latest/reference/pixi_configuration/).
Users who need those elsewhere set either one in their own pixi config, never
in this repo. The ESP-IDF cache is not a pixi cache. It follows
`XDG_CACHE_HOME` or `OS_TOOLCHAIN_ROOT`. Defaults live in `tools/idf-env.sh`
rather than `[activation.env]`, because activation values outrank the outside
environment and would make these overrides impossible.

Board wrappers verify dictionary/vendor hashes and generated ESP32-S3 flash,
PSRAM and partition configuration. Build output is ephemeral under
`firmware/<board>/build/<variant>/`; retain reviewable binaries and image hashes
in `artifacts/` before a real hardware run. The backup-gated flash wrappers
require the checked character-device port and matching 16,777,216-byte backup,
and flash the verified image without an implicit rebuild or full-chip erase. They use
Pixi's pinned esptool with the validated generated `flash_args` response file;
ESP-IDF 6.1 has no `idf.py --no-deps` option. Only bootloader, partition table,
OTA metadata and app0 regions are written. NVS is outside this plan.

```sh
pixi run cores3-flash <checked-port> backup/m5stack-cores3-flash-<timestamp>.bin
pixi run waveshare-flash <checked-port> backup/waveshare-sim7670g-v2-flash-<timestamp>.bin
pixi run waveshare-flash <checked-port> backup/waveshare-sim7670g-v2-flash-<timestamp>.bin diagnostic
```

Follow [the full identification/backup sequence](../../AGENTS.md#before-any-physical-flash-write)
first. Never erase Thing/config NVS or format the card. The migration retains
NVS namespaces, keys and value types and existing flash partition offsets;
The logger configuration retains the earlier two-universal-MAC policy
(`CONFIG_ESP32S3_UNIVERSAL_MAC_ADDRESSES_TWO=y`), preserving Bluetooth base+1.
IDF's four-MAC default would shift Bluetooth to base+2 and fail to load the
existing singleton local IRK. Bonded client reconnect still needs qualification;
never erase keys to work around an identity mismatch.

If the checkout or managed environment moved and a tool reports an old
interpreter path, run `pixi install` to reconcile it. Use `python -m esptool` /
`python -m espefuse` through project tasks rather than stale entry-point shebangs.

### LAN OTA and core dump readback

A logger on the LAN takes a new app image without USB (#40). Build the image,
then push it with the LAN token file.

```sh
pixi run ble-sync --lan <host> --token-file <token file> ota firmware/<board>/build/<variant>/<app>.bin --reboot
pixi run ble-sync --lan <host> --token-file <token file> ota-cancel
```

`ota` checks the chip and project name in the file first, then sends it in
order and waits for the SHA-256 check. Without `--reboot` the image waits for
the next `REBOOT` or power cycle. `ota-cancel` drops an upload or a staged
image. After the reboot look for the `SELFTEST` line on serial. Rollback is
off, see [OTA](security-hardening.md#ota).

A crash saves a core dump to the `coredump` partition. Read it over USB only,
with the same build that crashed.

```sh
pixi run coredump-read cores3 <checked-port>
pixi run coredump-read waveshare <checked-port> [logger|diagnostic]
pixi run coredump-read cores3 <checked-port> --erase
```

It prints the crashed task and the backtraces and leaves the partition alone,
so the same crash can be read again. `--erase` clears that partition after a
good print. `--dry-run` prints the commands only. No raw dump stays on the
host, even when decoding fails. There is no LAN core dump on purpose.

### C++ dialect

Firmware and host tests are pinned to C++23. ESP-IDF 6.1 adds `-std=gnu++26`
for chip targets, so every repo-owned component calls
`os_pin_cxx_standard(${COMPONENT_LIB})` from
[`firmware/cmake/os_cxx_standard.cmake`](../../firmware/cmake/os_cxx_standard.cmake),
which appends `$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>`. Board and fixture
projects include that file before `project.cmake`. C sources such as vendored
H3 keep the SDK C dialect, and third-party components keep the SDK default.
The standard is named in four places. Change them together.

| Place | Spelling |
| --- | --- |
| `firmware/cmake/os_cxx_standard.cmake` | `-std=gnu++23` in the function body |
| `tools/host_cxx.py` | `CXX_STD = "-std=c++23"` for every `tools/test_*.py` |
| `pixi.toml` task `pms-frame-test` | `clang++ -std=c++23` |
| `tools/lint.sh` | cppcheck `--std=c++23` |

`python tools/check_idf_target.py --cxx-dialect` fails when the four disagree,
or when a `tools/test_*.py` hardcodes a C++ `-std` instead of importing
`CXX_STD`. C standards such as `-std=c99` for H3 stay allowed.
`tools/lint.sh` runs it first. Every board build runs `check_idf_target.py`,
which repeats that check and reads `build/<variant>/compile_commands.json`. It
fails when the last `-std` on any repo-owned `.cpp` is not the pinned GNU
dialect, or when a repo `.c` file gets a C++ standard. `managed_components` and
build outputs are skipped. The logger compile test, the connectivity
fixture and the PSRAM probe have no board target, so they run only that
per-file check with `check_idf_target.py --compile-dialect <build>`, and
`compile-dialect-test` fails if one of them stops calling it. Style rules live at the top of
[`AGENTS.md`](../../AGENTS.md#c-standard-and-style).

`tools/lint.sh` then runs `tools/check_board_conditionals.py`, which fails on
any `#if`, `#ifdef`, `#ifndef` or `#elif` in `firmware/common` (vendor code
skipped) whose condition names a board, a modem or a chip target, such as
`CORES3`, `WAVESHARE`, `SIM7670` or `CONFIG_IDF_TARGET_ESP32S3`, also inside a
longer macro name, on a continued line or past a block comment. A name in a
trailing comment does not count. `CONFIG_IDF_TARGET_ARCH_*` names a CPU family
and stays allowed. Lint passes only on exit 0, so a check that cannot run
fails it. `board-conditionals-test` covers the rule. Shared code takes board
capabilities from hooks, `ServiceConfig` and sdkconfig defaults instead.

`tools/check_retired_names.py` then fails on any tracked file that still names
a retired AQ wire, NVS, Kconfig or serial name from before #93, such as the old
mDNS service, LAN handshake, GATT base UUID, NVS namespaces, `AQ_` Kconfig
symbols or `AQ <WORD>` serial tags. The dated bench records, the migration log
and the #93 audit are history and stay exempt. No `sdkconfig.rename` shim maps
the old Kconfig names, so a saved sdkconfig value under an old name is dropped.
The old board component names are retired too (#111). The board components
are `os_cores3`, `os_waveshare_logger` and `os_waveshare_diagnostic`.
The old logger source file names are retired too (#111). The logger sources
are `firmware/common/logger/src/os_logger*`.
The old common source file names are retired too (#111). They are now
`os_location`, `os_battery_health`, `os_console`, `os_ltr553` and `os_max17048`.
`retired-names-test` covers the rule.

### Source paths

Two checkouts of one commit build the same image. Every repo-owned component
also calls `os_map_source_paths(${COMPONENT_LIB})` from
[`firmware/cmake/os_source_paths.cmake`](../../firmware/cmake/os_source_paths.cmake),
which adds `-ffile-prefix-map=<repo root>=.`. `__FILE__` in asserts and
`ESP_ERROR_CHECK` then reads `./firmware/common/...` instead of the checkout
path, because the SDK's `CONFIG_COMPILER_HIDE_PATHS_MACROS` maps only the board
project directory. Each board project also calls `os_map_debug_paths()`, a
`-fdebug-prefix-map` of the repo root for every component, since the app image
embeds the ELF SHA-256 and third-party debug info (managed components, the
NimBLE copy in the build directory) would otherwise still carry the path.
Neither flag changes code. The board build and `--compile-dialect` fail when a
repo C or C++ file misses the map. `pixi run repro-paths-check [board]
[variant]` builds HEAD twice and compares, see CI below. It needs the SDK and
the network for managed components, and it leaves uncommitted edits out.

Source paths in the ELF are relative to the repo root. Function and line
resolve from anywhere, but run `addr2line`, `gdb`, `idf.py monitor` and the
core dump tools from the repo root so they find the source files.

On 2026-10-17 one `std::format_to_n` call added to the CoreS3 image at `-Og`
with xtensa GCC 15.2.0 cost about 341 KB of flash image and 5 KB of DIRAM
against the same build with `snprintf`. Device code keeps `snprintf` until a
later toolchain shows a smaller cost.

### Editor setup

The repo root `.clangd` points clangd at the board compile databases, so build
each board once to create them. Run `pixi run cores3-build` and
`pixi run waveshare-build logger`. Run `pixi run waveshare-build diagnostic`
too before you edit the Waveshare diagnostic image, which has its own
database. Shared code under `firmware/common` borrows the CoreS3 database,
except `firmware/common/gnss_reader`, which only the Waveshare logger builds.
Never commit `compile_commands.json` or anything under `build/`.

Host tests under `tools/` need no build. Each `tools/test_*.py` passes its own
`-I` list to the host `clang++`, and `tools/host_cxx.py` holds only the
dialect flag. `.clangd` adds `-std=c++23` and the shared include folders most
tests use. The tests with a fakes folder of their own (`wifi_host_fakes`,
`sd_memory_host_fakes`, `logger_work_host_fakes`) get the exact list their
script passes. `partial_rebuild_fixture.cpp` reads the Waveshare field table
and `footer_fixture.h` reads the CoreS3 contract first, as the comments in
`.clangd` say. On 2026-11-09 every `.cpp` and `.h` directly in `tools/`
checked with 0 errors. A new test that adds an include folder or a fakes
folder needs the same line in `.clangd`.

Use the clangd that pixi pins (the `clang-tools` entry in `pixi.toml`) and let
it ask the xtensa compiler for its headers. In VS Code with the clangd
extension put this in your own `.vscode/settings.json`, which git ignores.

```json
{
  "clangd.path": "${workspaceFolder}/.pixi/envs/default/bin/clangd",
  "clangd.arguments": [
    "--query-driver=${userHome}/.espressif/tools/xtensa-esp-elf/**/bin/xtensa-esp32s3-elf-*"
  ]
}
```

If `OS_IDF_TOOLS_PATH` moves the SDK tools away from `~/.espressif`, change the
glob to match. clangd runs every compiler the glob matches, so keep it narrow
instead of the `**` the ESP-IDF extension docs show. The `esp-clangd` that
`pixi run idf-setup` installs also works as `clangd.path`.

Check a file from the repo root with the same flags.

```sh
.pixi/envs/default/bin/clangd --check=firmware/common/logger/src/os_logger.cpp \
  --check-locations=false \
  --query-driver="$HOME/.espressif/tools/xtensa-esp-elf/**/bin/xtensa-esp32s3-elf-*"
```

It ends with `All checks completed, 0 errors` once the databases exist. Spell
checkers that read `cspell.json` know the project words.

## Native migration validation 2026-10-09

The migration passed native board and generic component builds plus host
contracts. Those results establish compilation and logic compatibility.
A subsequent [dated CoreS3 native flash/readback](../boards/m5stack-cores3/bench-verified.md#board-1-first-native-esp-idf-flash-and-readback)
qualifies only its short SD/USB/sensor/time run. Waveshare native hardware
remains pending. Historical Arduino results retain their original image scope.
Proto 3 phone transfers, reconnect, coexistence, sustained arbitration and
power-cut durability need separate device qualification.

## Dependency and shared-code policy

Use current official documentation and verify the latest stable release before
choosing or changing an SDK/component version. Pin the exact version/source
identity and generated dependency resolution; document any older compatibility
choice with evidence. Do not silently track floating `latest`/`master` or add
Arduino APIs/components. SDK and host-tool environments remain separate.

All eight components (OSCommon, OSSensors, OSRuntime, OSConnectivity, OSLogger,
OSLocation, `power` and `upload`) stay board-neutral and reusable.
Each board owns pin maps, controller initialization, power/display/sensor/RTC
capabilities, filesystem mounting and its measurement/provenance dictionary.
Reuse board-neutral interfaces with borrowed IO callbacks rather than copying
a sibling's GPIO, PSRAM or mount policy.

## Adding a board

Use the [ESP32-S3 porting note](board-porting.md) for logger hooks, transport
configuration, hardware capabilities and the qualification sequence. The
[adapter contract](../../firmware/common/logger/README.md) owns logger lifecycle
and callback details. Shared logic stays in `firmware/common`, with no board-name
conditionals or sibling trial dependencies.

## Verification

Every firmware PR runs the host gate and builds both boards. Use cached
build directories and the pinned Pixi environment. Run additional gates for the
changed subsystem. Preserve vendored bytes and licenses. Generic fixtures never
get flashed.

```sh
pixi run gate
pixi run connectivity-build-test
pixi run cores3-build
pixi run waveshare-build
```

`pixi run gate` runs `tools/gate.sh`. It runs `fmt-check`, `lint` and every
host contract and fixture task in a fixed list, with ASan and UBSan wherever
the task supports `--sanitize`. It stops at the first failure and prints which
task failed. Pass `--keep-going` to run the whole list and see every failure.
Tasks that need a board, a serial port, Bluetooth, a LAN peer or the ESP-IDF
SDK are not in the gate. Add a new host test task to that list when you add it
to `pixi.toml`. The clang, clang-format and cppcheck used by the gate come from
exact Pixi pins on macOS and Linux, so local runs and CI see the same tools.

### Continuous integration

`.github/workflows/ci.yml` runs on every pull request, on pushes to `main`,
and by hand from the Actions tab (`workflow_dispatch`).

- **Workflow lint** runs `actionlint` (with `shellcheck`) and `zizmor` on
  `.github/` at exact conda-forge pins through `pixi exec`. Run the same
  commands locally before changing a workflow.
- **Host gate** runs `pixi run gate --keep-going` on Ubuntu with the locked
  Pixi environment. Each task runs under GNU `timeout` from the Pixi
  `coreutils` pin, 300 s by default (`OS_GATE_TASK_TIMEOUT` overrides it), and
  a stalled task is reported as `TIMEOUT` by name in the gate summary. The
  gate refuses to start when a `*-test` task in `pixi.toml` is missing from its
  list, except the two SDK fixtures the board job runs.
- **Board builds** run `pixi run idf-setup`, then `cores3-build`,
  `waveshare-build logger`, `waveshare-build diagnostic`, and the generic
  `connectivity-build-test` and `logger-build-test` fixtures. The SDK comes
  from the same `setup-idf.sh` path developers use, so CI builds exactly the
  commit in `tools/idf-dependencies.lock`. The SDK and ccache caches are
  restored on every run but written only by pushes to `main`, and the SDK is
  saved from the cores3 leg alone, so pull requests never evict the `main`
  entries. `waveshare-psram-build` stays a manual SDK fixture. A
  `bench-stall` leg compiles the CoreS3 and Waveshare logger bench images
  with `OS_BENCH=stall`, so the `CONFIG_OS_LOGGER_BENCH_STALL` code keeps
  building. It only compiles them and has no size or slot check.
- Each board job uploads its `idf.py size --format json2` report and
  `os-build-sha256.json` as the `size-<board>` artifact, and the ELF, map and
  `flash_args` as `firmware-<board>`. Both are kept 90 days for `main` builds
  and 7 days otherwise. The job summary shows a size table. For a pull request
  the change is measured against the green build of its base commit, else
  against the newest green `main` build, and the summary names which commit
  was used. A malformed baseline prints a warning and the absolute table.
- Each board job then runs `ota-slot-check`, which fails when the app image
  uses more than half of the smallest app slot in that variant's
  `partitions.csv`. The share and the measurements behind it live in
  `tools/ota_slot_check.py`.
- **Reproducible image paths** runs `repro-paths-check waveshare logger`. It
  builds that image in two git worktrees of the commit at paths of different
  length, with `SOURCE_DATE_EPOCH` set to the commit time, and fails unless
  every file in the two `os-build-sha256.json` manifests matches, the app,
  bootloader, partition table, OTA data, `sdkconfig` and flash arguments. The
  compiler database and project description hold absolute paths by design and
  are left out. On a failure it names the file, the count of differing bytes
  and whether a checkout path is in it. The job runs without ccache.
- **CI result** waits for every other job and fails unless each one passed.
  A skipped job passes only when its id is listed in `ALLOWED_SKIPS` in that
  job, which is empty today because no job has an `if:`. It is the single
  required check.
- `.github/workflows/release.yml` is a separate workflow for `v*` tags and
  is not part of `CI result`, see [Cutting a release](#cutting-a-release).
- `.github/workflows/scorecard.yml` runs the OpenSSF Scorecard weekly and on
  pushes to `main` and uploads its SARIF result to code scanning. It never
  runs on pull requests and is not part of `CI result`.
- Dependabot opens one grouped pull request a week for action pin updates,
  only for releases at least 7 days old (`.github/dependabot.yml`).

CI proves compilation and host logic only. It never flashes a board, and board
runs stay manual and dated in each bench record. Every action is pinned to a
full commit SHA. Check the latest stable release before changing a pin.

#### Required checks

Make only `CI result` required for `main`. In Settings, Rules, Rulesets, open
or create the `main` branch ruleset, enable Require status checks to pass, add
`CI result` with the source set to GitHub Actions, and save.

`CI result` needs every other job, so jobs and board matrix entries can be
added or renamed without a ruleset change. A new job must be added to its
`needs` list, or its failure will not block a merge. A job that gains an `if:`
must also be added to `ALLOWED_SKIPS`, or its skip blocks the merge. Keep the
job name `CI result` stable. If a merge queue is enabled later, add a
`merge_group` trigger to the workflow at the same time.

| Changed area | Additional Pixi tasks |
| --- | --- |
| Writer, codec, footer or schema | `parquet-test --sanitize`, `reader-test`, `waveshare-contract-test --sanitize` |
| Plantower, LTR553 or MAX17048 | `pms-frame-test`, `ltr553-test`, `max17048-test` |
| Battery health or logger power hooks | `battery-health-test`, `logger-power-test`, `waveshare-contract-test --sanitize` |
| Disconnects, LAN rings or BLE policy | `disconnect-test`, `frame-ring-test`, `ble-policy-test`, `ble-advert-test`, `ble-sync-test`, `lan-concurrency-bench` |
| Settings, time or location | `config-test`, `network-time-test`, `location-test`, `logger-provision-test` |
| Logs and persistence | `debug-log-test`, `disconnect-test` |
| Logger queues and lifecycle | `logger-build-test`, `logger-work-queue-test`, `logger-watchdog-test`, `logger-codec-store-test`, `logger-rotation-test`, `logger-footer-test`, `logger-directory-test`, `logger-archive-reset-test` |
| Storage governor, CRC cache or DMA buffer | `storage-governor-test`, `crc-cache-test`, `sd-transfer-buffer-test` |
| Backup tooling | `backup-test` with offline fake esptool |
| App signing or the tracked secrets guard | `release-sign-test`, `secrets-check-test` |
| Release workflow | `release-workflow-test`, then `actionlint` and `zizmor` as in CI |
| CMake helpers, compile flags or `sdkconfig` defaults | `repro-paths-check`, `compile-dialect-test` |
| Diagnostic or PSRAM configuration | `waveshare-build diagnostic`, `waveshare-psram-build` |

Task definitions in `pixi.toml` own exact commands and sanitizer defaults. The
archive, transport, settings, logger and geometry host fixtures compile actual
implementation code against platform fakes. `logger-directory-test` includes
FatFs mount-root handling through `stat`, since `mkdir` at the mounted root can
return `EINVAL`. `sd-transfer-buffer-test` covers the 4 KiB internal DMA buffer
reservation, failed-mount release/retry and lifetime, rather than SD timing.

The LAN task owns non-blocking socket IO. The storage worker services samples
between bounded ring/credit waits and consumes copied requests serially.
`logger-work-queue-test` covers queue-before-wakeup, sample priority and
saturation. None of these host gates proves throughput, radio coexistence,
physical persistence or power-cut durability. Use the
[LAN qualification procedure](lan-performance.md) and dated board evidence
before making those claims. Shared mount assertions enforce the
[descriptor budget](../../firmware/common/src/archive_files.h). Shared SDK
defaults enable fast seek and board target checks enforce radio settings. Do not carry old generated sdkconfig values into a new target.

## Local provisioning and client boundaries

The shared [sync protocol](ble-sync-protocol.md) retains its UUIDs/opcodes and
512-byte BLE frame cap. STATUS/LIVE JSON allows 480 bytes. Clients request MTU
517 where possible; MTU 483 carries a maximum snapshot in one notification.
Notifications exceeding negotiated MTU − 3 are omitted after the complete value
is cached; use characteristic long reads for complete snapshots.
LIVE `t`/`rh` are ambient only and CoreS3 sends IMU temperature as `imu_t`. A board
without an RTC hook has no retained clock anchor; UTC stays null after boot until
automatic host SET_TIME or successful Wi-Fi SNTP. Raw unsynchronized rows remain in the `unsynced` tree; the mobile archive reconciles derived partitions from same-boot anchors.

Physical UART owner commands (`owner-pin`, `wifi-profile`, `config-hex`) bypass
DebugLog for secret-bearing replies. Use `pixi run owner-pin` and
`pixi run provision-usb` with explicitly checked ports; host tools must keep
credential profiles in memory and exclude them from captures and artifacts.
They add no radio opcode or secret CONFIG JSON field. The real SET_CONFIG
validator/actions remain the single provisioning implementation.

## Local time and deployment location

The [time/location contract](time-and-location.md) describes default DHCP/router,
configured local and permitted public NTP order, offline RTC/host anchoring and
explicit mobile H3 provisioning. Use `pixi run network-time-test`, `location-test`,
`config-test` and `control-sync-test` for these changes, plus both schema reader
gates and affected board builds for logger location changes. These sanitizer
fixtures compile the pinned embedded-h3-location library, its official H3
core and real service/settings code. Existing
generated sdkconfig values override defaults: regenerate or explicitly update
`CONFIG_LWIP_SNTP_MAX_SERVERS=3` and
`CONFIG_LWIP_DHCP_GET_NTP_SRV=y` in disposable build configurations when moving
from the earlier native image. Also regenerate the logger configuration to select
`CONFIG_ESP32S3_UNIVERSAL_MAC_ADDRESSES_TWO=y`; the target checker rejects FOUR.
Do not modify owner NVS or physical flash for a
build configuration update.

Source and host checks do not establish router option-42 behavior on a specific
network, NTP accuracy/authenticity, physical radio coexistence, phone permission
or location behavior, SD timing or power-cut durability. The later CoreS3
native run observed successful network UTC and a short USB file readback;
router/local/public transitions and location provisioning remain unqualified.
Finalized owner archives and old schemas were
not rewritten.

## Local app signing

The app signing key lives only on the owner machine, in `secrets/` at the
repository root. It is never a GitHub secret and CI never sees it. `secrets/`
is ignored by git, and `pixi run lint` runs `tools/check_secrets.py`, which
fails when any path under `secrets/` is tracked, in any letter case, when a
tracked file holds a PEM or PGP private key header, or when a tracked file has
a binary key store name such as `.p12`, `.pfx` or `.jks`. `pixi run
secrets-check` runs the same check alone.

Firmware does not check app signatures yet. Turning that on is #40, and it
waits on an owner-approved bootloader reflash. Until then a signed image boots
exactly like an unsigned one, so signing only proves the release came from the
owner key.

ESP32-S3 Secure Boot v2 verifies RSA-3072 signatures only, so that is the one
scheme the task accepts.

### One-time owner setup

Run these from the repository root.

1. Create the directory, readable only by you.

   ```sh
   mkdir -m 700 secrets
   ```

2. Generate the key with the pinned espsecure. It writes the file readable by
   everyone, so restrict it right away.

   ```sh
   pixi run espsecure generate-signing-key --version 2 --scheme rsa3072 secrets/app_signing_key.pem
   chmod 600 secrets/app_signing_key.pem
   ```

3. Write `secrets/.env`. The key path is relative to `secrets/`.

   ```sh
   printf 'OS_APP_SIGNING_KEY=app_signing_key.pem\nOS_APP_SIGNING_SCHEME=rsa3072\n' > secrets/.env
   chmod 600 secrets/.env
   ```

   | Variable | Meaning |
   | --- | --- |
   | `OS_APP_SIGNING_KEY` | Required. File name of the PEM private key inside `secrets/` |
   | `OS_APP_SIGNING_SCHEME` | Optional. `rsa3072`, the default and the only accepted value |

   The file takes `NAME=VALUE` lines, an optional `export` prefix, `#`
   comments and optional quotes. Nothing else is read from it.

4. Back up `secrets/app_signing_key.pem` offline, for example on two encrypted
   USB drives kept in different places. Once #40 enables signed app checks, a
   device only takes an OTA image signed by this key. Losing the key then means
   every deployed device needs a USB reflash to take an update. Never copy the
   key into the repository, a cloud drive or a chat.

### Sign a release

Build the board images as usual, then sign the app `.bin` files into a
directory outside `secrets/`.

```sh
pixi run release-sign --out exports/signed \
  firmware/esp-idf-waveshare-sim7670g/build/logger/opensensor_waveshare.bin
```

For each input it runs `espsecure sign-data --version 2` to
`<out>/<name>.signed.bin`, then `espsecure verify-signature --version 2` on the
result. It then writes `<out>/SHA256SUMS` in `sha256sum` format. It refuses to
run when

- `secrets/.env` is missing or does not set `OS_APP_SIGNING_KEY`
- git tracks any path under `secrets/`
- `secrets/`, `secrets/.env` or the key is open to group or other
- the key is outside `secrets/`, password protected or not an RSA-3072 key
- the output directory or an input is inside `secrets/`
- an output would replace one of the inputs, or is already a symlink
- the key or `secrets/.env` cannot be read, or `.env` is not UTF-8

It never prints the key, the `.env` values or any file contents. It names the
key only as `secrets/<name>`. `release-sign-test` and `secrets-check-test`
use throwaway keys in a temp dir and never touch `secrets/`.

## Cutting a release

`.github/workflows/release.yml` runs when a `v*` tag is pushed. It builds the
CoreS3 and Waveshare logger images with the same pinned SDK and build tasks as
CI, with no cache. It then creates a **draft** release that holds the unsigned
app images, one SPDX SBOM per image from `esp-idf-sbom`, `SHA256SUMS` and a
build provenance attestation for every file in `SHA256SUMS`. Assets are named
`opensensor_cores3-bringup-<tag>.bin` and `opensensor_waveshare-logger-<tag>.bin`. CI never
holds the signing key and never publishes. A manual run from the Actions tab
on any branch is a dry run. It builds and uploads the same files as the
`release-files` artifact and creates no release. A re-run for a tag that
already has a release, draft or published, fails before it attests or uploads
anything. `release-workflow-test` in the host gate pins these rules.

The release holds app images only, with no bootloader, partition table or
`flash_args`. It serves an OTA update or an app only flash of a board that
already runs this firmware. It cannot first flash a blank board. For that, use
the build and flash workflow of the
[CoreS3](../../firmware/esp-idf-cores3/README.md#current-build-and-flash-workflow)
or the
[Waveshare V2](../../firmware/esp-idf-waveshare-sim7670g/README.md#current-build-and-flash-workflow)
and the checks in the root `AGENTS.md` before any physical flash write.

Run these from the repository root, with `v1.2.0` standing for the new tag.

1. Check that CI passed on the `main` commit. The command prints `success`
   only when every job, `CI result` included, passed. Then tag the commit and
   push the tag.

   ```sh
   gh run list --workflow ci.yml --commit <main-commit> --event push \
     --json conclusion --jq '.[0].conclusion'
   git tag -a v1.2.0 -m v1.2.0 <main-commit>
   git push origin v1.2.0
   ```

2. Wait for the Release workflow to finish and the draft to appear. The run
   can take a few seconds to show up after the push, so the loop looks for it
   every 10 seconds and gives up after 30 tries, about 5 minutes, also when
   `gh` keeps failing. It does not sleep after the last try.
   `--exit-status` makes the watch fail when the run fails. The run is also on
   the repository Actions page under Release.

   ```sh
   run_id=""
   for try in $(seq 30); do
     run_id="$(gh run list --workflow release.yml --branch v1.2.0 --event push \
       --limit 1 --json databaseId --jq '.[0].databaseId // empty')" || run_id=""
     [ -n "$run_id" ] && break
     if [ "$try" -lt 30 ]; then sleep 10; fi
   done
   if [ -n "$run_id" ]; then
     gh run watch "$run_id" --exit-status
   else
     echo "no Release run for v1.2.0 after 5 minutes" >&2
     false
   fi
   ```

3. Download the draft and check it against `SHA256SUMS`. Then check the
   attestation of every image and SBOM, tied to `release.yml` so an
   attestation from any other workflow in the repository does not pass.

   ```sh
   gh release download v1.2.0 --dir exports/release-v1.2.0/unsigned
   (cd exports/release-v1.2.0/unsigned && shasum -a 256 -c SHA256SUMS)
   for f in exports/release-v1.2.0/unsigned/*.bin exports/release-v1.2.0/unsigned/*.spdx.json; do
     gh attestation verify "$f" --repo walkthru-earth/opensensor-space-firmware \
       --signer-workflow walkthru-earth/opensensor-space-firmware/.github/workflows/release.yml
   done
   ```

4. Sign the app images with the local key, see [Local app signing](#local-app-signing).

   ```sh
   pixi run release-sign --out exports/release-v1.2.0/signed exports/release-v1.2.0/unsigned/*.bin
   ```

5. Join both checksum lists into one `SHA256SUMS`, then upload it with the
   signed images. It replaces the draft `SHA256SUMS`.

   ```sh
   cat exports/release-v1.2.0/unsigned/SHA256SUMS exports/release-v1.2.0/signed/SHA256SUMS \
     | LC_ALL=C sort -k 2 > exports/release-v1.2.0/SHA256SUMS
   gh release upload v1.2.0 --clobber \
     exports/release-v1.2.0/signed/*.signed.bin exports/release-v1.2.0/SHA256SUMS
   ```

6. Review the draft on GitHub, then publish it.

   ```sh
   gh release edit v1.2.0 --draft=false
   ```

The attestation covers the unsigned images and SBOMs as CI built them. The
signed images are those bytes plus the owner signature block, and the owner
key is their proof of origin. Until #40 the firmware does not check that
signature.

## Owner maintenance

Physical serial `parquet erase-archive CONFIRM` (CoreS3 v6.10 / Waveshare v2.4)
removes only `/sd/output` and historical `/sd/parquet`, including partials and
quarantine. It closes archive/partial handles, discards the RAM batch, pauses
acquisition, rejects further archive requests and remains paused until reboot,
even after a deletion error. It never formats the card or deletes unrelated
card content. Use only for an explicitly requested destructive reset. The
`PARQUET ERASE_ARCHIVE` response reports success and deletion counts; a host
fixture does not prove physical FAT durability.
