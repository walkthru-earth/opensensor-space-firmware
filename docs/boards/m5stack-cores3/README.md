# M5Stack CoreS3 board references

The current [CoreS3 application](../../../firmware/esp-idf-cores3/README.md)
uses native **ESP-IDF 6.1.1** with M5Unified **0.3.0** and M5GFX **0.3.0**
components. A [short native run](bench-verified.md#board-1-first-native-esp-idf-flash-and-readback)
verified 20 MHz SD logging/readback, sensors, RTC/network time and retained
settings/bonds. The newest dated record is the
[2026-10-26 reflash of main 689bb61](bench-verified.md#main-689bb61-usb-reflash-thing-id-and-first-geo-footer-2026-10-26),
with the Thing id, a null `geometry` column and the `geo` footer key on the
first file. Phone transfers and durability remain unqualified. Earlier
results belong to their identified historical Arduino images. The optional M134 contains
PMSA003; its wiring is distinct from Waveshare/PMS5003T.

Use the [native build/flash workflow](../../../firmware/esp-idf-cores3/README.md#current-build-and-flash-workflow)
and [shared development guide](../../shared/development.md) for current source.
The development guide describes the native workflow; dated Arduino API/build commands and resource figures remain historical.

| Topic | File |
| --- | --- |
| Wiring, power, onboard peripherals, boot | [Hardware](cores3-hardware.md) |
| Current native build and component pins | [Application workflow](../../../firmware/esp-idf-cores3/README.md), [shared development](../../shared/development.md) |
| Native drivers/runtime and historical toolchain notes | [Development](cores3-development.md) |
| Wi-Fi/BLE/ESP-NOW | [Wireless](cores3-wireless.md) |
| microSD, SPI and recovery | [Storage](cores3-storage.md) |
| Optional add-ons | [Add-ons](addons.md), [M134/PMSA003](addon-air-quality.md) |
| Dated native/historical image measurements and limits | [Bench record](bench-verified.md), [compression experiment](compression-benchmark.md) |

The [shared contracts](../../README.md#shared-contracts) apply to both board targets. Results in the CoreS3 bench record do not establish Waveshare behavior.

## Internal flash archive

The `spiffs` data partition at 0xc20000 (0x3e0000 bytes, 3.875 MiB) is a FAT
volume mounted at `/flash`. With no usable card the logger writes Parquet there
and copies each file to the card once one is Ready, see [decision record 0004](../../shared/decisions/0004-flash-fallback-archive.md).
`firmware/esp-idf-cores3/sdkconfig.defaults` enables it. It holds roughly 19
hours of the CoreS3 schema. Nothing is proven on the bench yet.

## Settings storage fault

NVS holds the Thing id, the pairing PIN, Wi-Fi and the board settings. When it
does not start, the CoreS3 records no data and starts no radio. The screen
shows "Settings storage error" in red with the code, and the board does nothing
else. The serial console prints this line at boot and again every 30 seconds.

```text
DIAG ERROR operation=nvs-init error=<code> erased=false
```

To recover, follow these steps in order.

1. Read the serial line with `pixi run monitor <checked port> 115200` and note
   the code.
2. Never erase the flash or the NVS partition. That destroys the Thing id and
   the owner's settings.
3. Run the [identification and backup sequence](../../../AGENTS.md#before-any-physical-flash-write),
   then flash the current release with `pixi run cores3-flash <checked port> <backup>`.
   The helper writes the bootloader, partition table, OTA data and app, never NVS.
4. If the line still appears after the reflash, or the helper refuses the
   backup, stop and contact the owner with the serial capture and the backup.

| Code | ESP-IDF name | Meaning |
| --- | --- | --- |
| 4365 (0x110d) | `ESP_ERR_NVS_NO_FREE_PAGES` | NVS has no free page. Contact the owner. |
| 4368 (0x1110) | `ESP_ERR_NVS_NEW_VERSION_FOUND` | A newer firmware wrote NVS. Flash the current release. |
| 261 (0x105) | `ESP_ERR_NOT_FOUND` | The partition table has no NVS. The helper refuses such a backup. Contact the owner. |

## Thing id fault

The Thing id lives in NVS `parquet/thing`. When NVS starts but the Thing id
cannot load, the CoreS3 records no data and starts no radio for that boot.
The screen shows "Settings storage error" in red with `thing-id stage=<stage>` and `error=<code>` below it.
The serial console prints this line at boot and again every 30 seconds.

```text
PARQUET ERROR operation=thing-identity stage=<stage> error=<code> logging=false
```

| Stage | Meaning |
| --- | --- |
| `open` | The `parquet` NVS namespace did not open. |
| `read` | The stored value did not read. 4355 (0x1103) `ESP_ERR_NVS_TYPE_MISMATCH` is a key of another type, 4364 (0x110c) `ESP_ERR_NVS_INVALID_LENGTH` a value longer than a Thing id. |
| `write` | A first boot could not store its new Thing id. |
| `format` | The stored value is not a 36 character lowercase UUID. The code is 0. |

The firmware writes the Thing id only when the key does not exist. It never
rewrites or repairs a stored value that failed to load, that value is the
owner's Thing id. Note the line, never erase the flash or the NVS partition,
and contact the owner with the serial capture.

## Stack and heap budgets

Task stacks are sized from two numbers, the method of issue #37. The static worst case comes from
the build, and the measured high water mark comes from the board.

- **Static.** `pixi run stack-budget --image cores3/bringup --path` prints each
  tracked task's deepest call path from the GCC call graphs of the last build,
  plus the linker section sizes. CI fails the board leg when a task or section
  grows past [tools/stack_budget.json](../../../tools/stack_budget.json).
  The figure is a static estimate, not a bound. Pointer calls follow reviewed
  rules and an uncovered one counts as zero, ROM code counts as zero, and
  recursion is cut and flagged, see `pixi run stack-budget --help`. The check
  also fails when a source file the image links calls `xTaskCreate` for a task
  it does not track, with a stack size that differs from the budget file, or
  in a form it cannot parse (a lambda entry or a task name in a variable).
- **Measured.** The logger prints two lines to serial and the LOG ring 60 s
  after boot, then hourly. `parquet mem` on the USB console prints them at
  once from the board task, so it answers even while the storage worker is busy.
  `DIAG tasks` gives the bytes never used on each task stack (`cores3-board`,
  `parquet-sd`, `os-lan`, `os_ble_error`, `nimble_host`, `tcpip`, `esp_timer`
  and `sys_evt`, with `-1` for a task that is not running). `DIAG heap` gives
  free, minimum free and largest free block for internal, DMA capable and PSRAM
  heap. Nothing new crosses BLE or LAN except these lines in `LOG_TAIL`.

Bench recipe, about 75 min.

1. Flash a checked image and note its `os-build-sha256.json` hash. A high water
   mark only grows until the next reset, so do not reset the board until step 4.
2. Wait for the first `DIAG tasks` line (60 s after boot) as the idle baseline.
3. Exercise the deep paths. Sync a full day over BLE, then over LAN, run
   `parquet flush`, and let one rotation window close a file (default 900 s, at most 1 h).
4. Read the marks with
   `pixi run parquet-device command --port <checked-port> 'parquet mem'`,
   which returns after the `DIAG heap` line. Opening the CoreS3 USB console
   does not reset the app.
5. Record the smallest value seen per task and heap below, with the date,
   image hash and what was exercised. A task with less than a quarter of its
   stack left (cores3-board has 16,384 B) is a finding to raise on issue #61.

| Date | Image hash | Exercised | cores3-board | parquet-sd | os-lan | os_ble_error | nimble_host | tcpip | esp_timer | sys_evt | internal_min | internal_largest | dma_largest | psram_largest |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
