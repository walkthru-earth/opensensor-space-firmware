# M5Stack CoreS3 board references

The current [CoreS3 application](../../../firmware/esp-idf-cores3/README.md)
uses native **ESP-IDF 6.1** with M5Unified **0.2.29** and M5GFX **0.2.36**
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

## Stack and heap budgets

Issue #37 sizes task stacks from two numbers. The static worst case comes from
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
   stack left (cores3-board has 16,384 B) is a finding to raise on issue #37.

| Date | Image hash | Exercised | cores3-board | parquet-sd | os-lan | os_ble_error | nimble_host | tcpip | esp_timer | sys_evt | internal_min | internal_largest | dma_largest | psram_largest |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
