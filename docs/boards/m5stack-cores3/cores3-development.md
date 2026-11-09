# CoreS3 C/C++ development

The current application is pure **ESP-IDF 6.1**, with native M5Unified
**0.2.29** and M5GFX **0.2.36** components. Native board and fixture builds plus
host gates pass. A [short native SD/USB/network-time run](bench-verified.md#board-1-first-native-esp-idf-flash-and-readback)
also passed; phone transfers and endurance remain pending. Use the
[current workflow](../../../firmware/esp-idf-cores3/README.md#current-build-and-flash-workflow)
and [shared development guide](../../shared/development.md).

## Current exact pins and documentation

| Layer | Pin observed 2026-11-09 | Integration |
| --- | --- | --- |
| ESP-IDF | [6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/index.html) | Native ESP32-S3 C/C++, Kconfig, FreeRTOS, drivers, NimBLE, Wi-Fi/netif/SNTP |
| M5Unified | [0.2.29](https://github.com/m5stack/M5Unified/releases/tag/0.2.29) | Native component owns board services and power; [manifest](../../../firmware/esp-idf-cores3/components/os_cores3/idf_component.yml) |
| M5GFX | [0.2.36](https://github.com/m5stack/M5GFX/releases/tag/0.2.36) | Native display and SPI2 bus owner; SD borrows that controller |
| mDNS | [1.15.1](https://components.espressif.com/components/espressif/mdns/versions/1.15.1) | Managed component used by shared LAN transport |
| LZ4 | [1.11.0](https://github.com/lz4/lz4/releases/tag/v1.11.0) | Vendored bytes and license remain hash-checked |

Before adding/changing dependencies, check the latest stable upstream release
and current official documentation; pin exact source/version identities and
record compatibility exceptions with evidence. Do not add Arduino APIs or
components. Common driver, writer, logger and radio components remain reusable
across boards; pins, controller initialization and actual hardware capability
belong to each board adapter.

## Native build and operational notes

- Host tools come from pinned Pixi. `pixi run idf-setup` installs the exact SDK
  source and its SDK-selected Python/compiler tools. `pixi run cores3-build`
  compiles and verifies dictionary/vendor hashes, custom partitions, 16 MB QIO
  flash, Quad PSRAM and USB Serial/JTAG console.
- SDK source defaults to `$OS_TOOLCHAIN_ROOT`, otherwise
  `${XDG_CACHE_HOME:-~/.cache}/opensensor-space-firmware/toolchains`.
  Compiler tools default to official shared `~/.espressif`, and
  `$OS_IDF_TOOLS_PATH` overrides that directory. The ESP-IDF Python venv
  lives in the checkout under `.pixi/idf-python-env`. The wrapper keeps the SDK
  Python/compiler environment separate from Pixi host validators.
- C++ `app_main` has C linkage. Use native FreeRTOS tasks, `esp_timer`, UART,
  NVS and VFS APIs. Every repo component builds as C++23, see the
  [C++ dialect pin](../../shared/development.md#c-dialect). Exceptions and RTTI
  remain off.
  [C++ constraints](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/cplusplus.html).
- Initialize M5 board services once. Disable unused services through `M5.config()`
  before `M5.begin(cfg)`. M5Unified owns internal I²C; never create another
  driver on the same controller. Shared register drivers borrow board callbacks.
  [M5 source](https://github.com/m5stack/M5Unified/blob/0.2.29/src/M5Unified.hpp).
- M5GFX owns SPI2. Native SDSPI attaches the card without independently
  initializing/freeing that controller. Finish display DMA/transactions and
  guard complete display/card operations with the application mutex.
  [Current SD ownership](cores3-storage.md).
- OSLogger owns the 10-second monotonic deadline, bounded queues and single
  archive worker. Board acquisition and RTC callbacks run on the main task;
  radio callbacks copy requests and never access sensors, display or SD.
- Native NimBLE GAP/GATT notifications, advertising updates and bond clearing
  run on the host task. Keep public addressing, secure characteristic flags,
  UUIDs, long reads, MTU bounds and ordered response semantics. Successful SNTP
  callbacks alone publish network time. Existing Arduino-era bonds need native
  reconnect/re-pairing qualification without erasing Thing/config NVS.
- Native DebugLog uses `LogOutput` and a function/context sink; radio hot-path
  `record_only()` writes stay in the ring without console IO. Pairing logs show
  mode only. Physical owner/provisioning replies use the console directly.
- Retain the exact BIN/ELF and hashes under ignored `artifacts/` before a
  physical run. `build/bringup/` is rebuildable. Decode native crash addresses
  using the pinned SDK's `xtensa-esp32s3-elf-addr2line -pfiaC -e <retained-native.elf>
  <addresses>`, or its IDF monitor with the matching build. Run them from the
  repo root, since ELF source paths are relative to it. Never decode a
  historical image with a newly compiled ELF.

## Historical resource and debugging evidence

Arduino-ESP32 3.3.11 with M5Unified 0.2.21/M5GFX 0.2.28 and NimBLE-Arduino
2.5.1 supplied the earlier bench images. The September bootstrap measured
7.5 GB for the completed Arduino SDK cache plus 34 MB for Arduino CLI; this is
not a native SDK size or setup-time estimate. The old commands/API shapes and
image-specific heap/jitter observations below are retained for interpreting
historical evidence, not the current build or radio implementation.

## Historical NimBLE-Arduino 2.5.1 integration notes (observed 2026-09-16)

What the [BLE sync service](../../shared/ble-sync-protocol.md) needed from the library, recorded so the next radio feature does not rediscover it.

- **2.x API shape.** Server callbacks take `NimBLEConnInfo&` (`onConnect(NimBLEServer*, NimBLEConnInfo&)`, `onDisconnect(…, int reason)`, `onMTUChange(uint16_t, NimBLEConnInfo&)`, `onAuthenticationComplete(NimBLEConnInfo&)`); `onPassKeyDisplay()` **returns** the passkey. Characteristic writes arrive in `onWrite(NimBLECharacteristic*, NimBLEConnInfo&)`; read the bytes with `getValue()` → `NimBLEAttValue` (`data()`, `length()`). `notify(const uint8_t*, size_t, connHandle)` returns `bool` and can refuse when host buffers are full — retry with a short `vTaskDelay`, never drop the frame. Properties are `NIMBLE_PROPERTY::READ_ENC | READ_AUTHEN | WRITE_ENC | WRITE_AUTHEN | NOTIFY`; security via `NimBLEDevice::setSecurityAuth(bond, mitm, sc)` + `setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY)`.
- **`nimconfig.h` is not the knob on Arduino-ESP32.** Under `ESP_PLATFORM` the library takes `CONFIG_BT_NIMBLE_*` from the core's prebuilt `sdkconfig.h`; the user overrides at the top of `nimconfig.h` only apply to non-ESP targets. So max connections / bonds come from the pinned core, and "one connection at a time" is enforced by application logic (advertising stops on connect, `advertiseOnDisconnect(true)` restarts it) rather than by a config value.
- **Attribute values are capped at 512 bytes** (`BLE_ATT_ATTR_MAX_LEN`); pass `max_len` to `createCharacteristic` for anything larger than the 20-byte default. Set `NimBLEDevice::setMTU(517)` so a phone can negotiate a 514-byte payload; the measured negotiation went 23 → 517.
- **Cost on CoreS3:** +252,856 program bytes and +12,376 static RAM bytes over the same firmware without BLE (589,887 / 26,996 → 842,743 / 39,372). Free internal heap while connected and transferring: ≈198 KB. Well inside the 6 MB OTA slot; note `arduino-cli` prints a 3 MB "maximum" from the default board definition, not from our partition table.
- **Cost of protocol v2 (Wi-Fi STA + lwIP + ESPmDNS + TCP server + log ring), measured 2026-09-16:** program **1,407,435** bytes (v4 842,743 → +≈565 KB), static internal RAM **78,564** bytes (v4 39,372; includes the 8 KiB `oslog` ring, an 8 KiB `LOG_TAIL` staging buffer and the LAN rx/push buffers). The storage worker stack went 16 → 24 KiB after its high-water mark read 1,592 bytes free during a light `GET_CONFIG`. Still ample room in the 6 MB OTA slots. Free internal heap with BLE + Wi-Fi + mDNS + one TCP session active: **82,072** bytes (v4, BLE only: ≈198 KB), measured 2026-09-17. [Bench record](bench-verified.md#board-1-protocol-v2-configuration-wi-fi-lan-sync-phone)
- **Never touch SD or the display from a NimBLE callback.** They run on the host task. The service turns writes into queue entries for the storage worker, which owns the filesystem and emits notifications itself; `live`/`status` are published from the sampling loop. This kept jitter ≤ 7.5 ms and drops at zero during transfers.
- **Bench from a Terminal window on macOS.** CoreBluetooth aborts a client whose responsible process lacks a Bluetooth usage entitlement (`Abort trap: 6`, no output). Launching the same `pixi run ble-sync` through Terminal.app gets the normal permission prompt.
- **All serial output goes through `oslog`.** `debug_log.h` defines a log writer with a ring (8 KiB in the Arduino era, now a persistent PSRAM ring sized by `CONFIG_OS_LOG_PERSIST_RING_BYTES` (64 KiB default) with a 4 KiB internal fallback) that tees to the console, so `LOG_TAIL` can serve the same lines to a phone. Every `Serial.print*` in the trial was replaced by `oslog`; new code must print through `oslog`, never `Serial`, or the line is invisible over BLE/LAN. Serial **input** is unchanged (`Serial.available()` / `Serial.read()` in the command parser).

## Debugging a wedged task (observed 2026-09-16)

- The storage worker stamps a heartbeat; when it stops moving for 5 s the main loop prints `PARQUET ERROR operation=worker-stall seconds=N state=… stack_free=… watchdog=…`. After `CONFIG_OS_LOGGER_WORKER_WDT_TIMEOUT_S` (60 s) the task watchdog resets the chip with a core dump, see `firmware/common/logger/README.md` (Worker watchdog). That line is the symptom to look for when a worker-side command goes unanswered while `PARQUET ROW … dropped=` climbs. `eTaskGetState` called from the other core reports a spinning task as `ready`, not `running`, so do not read `state=ready` as "idle".
- The task watchdog names the culprit per CPU (`Tasks currently running: CPU 0: parquet-sd`) and prints a `Backtrace:` line. Decode it with `xtensa-esp-elf-addr2line -pfiaC -e firmware/esp-idf-cores3/build/bringup.ino.elf <addresses>`; the tool lives under `~/.cache/m5stack-aq-parquet/toolchains/arduino/data/packages/esp32/tools/esp-x32/*/bin/` (not on `PATH`, not in Pixi). The ELF must be the one that was flashed: Arduino rebuilds wipe `build/`, so keep the ELF with the retained binary in `artifacts/` when a session is worth debugging later. [Fatal errors / backtraces](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-guides/fatal-errors.html), [task watchdog](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/system/wdts.html)
