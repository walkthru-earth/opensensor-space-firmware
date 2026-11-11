# Waveshare ESP32-S3-SIM7670G-4G V2

The current [V2.0 application](../../../firmware/esp-idf-waveshare-sim7670g/README.md)
is native **ESP-IDF 6.1**, with logger and UART/read-only TF diagnostic variants.
The logger retains UART1 PMS5003T, one-bit SDMMC, OPI PSRAM, UART0 console,
MAX17048 and the 62-column v6 schema with dictionary v8, including battery
health state, gauge diagnostics and the location columns.
The newest dated native readback is the
[2026-10-26 reflash of main 689bb61](bench-verified.md#main-689bb61-usb-reflash-thing-id-and-first-geo-footer-2026-10-26),
with the Thing id, a null `geometry` column and the `geo` footer key on the
first file. **Phone sync, GNSS fix, endurance and power-cut durability remain
unqualified.** Use the [native build/flash workflow](../../../firmware/esp-idf-waveshare-sim7670g/README.md#current-build-and-flash-workflow)
and [shared development guide](../../shared/development.md).

The following hardware observations describe historical Arduino images.
On 2026-09-28, identification, full backup, diagnostic/full logger boot, TF mount/capacity, PMS5003T reception, OPI PSRAM and bounded 49-field Parquet serial readbacks with unsynced/host-UTC behavior were verified. BLE advertising/discovery worked; the first macOS encrypted-pairing attempt timed out, while a later Android foreground bond repair succeeded. The owner has since installed an 18650 and verified battery-only boot. A USB-attached 2026-09-30 readback verified MAX17048 voltage/SOC rows; the connected Android app then displayed 4,277 mV on USB and 4,067 mV after board USB was unplugged, both with capped 100%. BLE file transfer and Wi-Fi/LAN remain unverified. Modem, GNSS, camera and charging status remain unimplemented/unverified. RGB visual behavior was not checked. Source checks, host tests and physical measurements are recorded separately.

- [Hardware and pin ownership](hardware.md)
- [PMS5003T wiring and UART contract](pms5003t.md)
- [Cairo PM2.5 publication and field-quality contract](cairo-pm25-publication.md)
- [Logger storage, measurement contract and local sync](storage.md)
- [Hardware evidence and limitations](bench-verified.md)
- [Official software resources and V2 adoption limits](software-resources.md)
- [Native ESP-IDF application and historical image notes](../../../firmware/esp-idf-waveshare-sim7670g/README.md)

The board's product name includes a modem, but local sampling and SD copies must continue to work without a SIM or internet. A later cellular upload path cannot replace the card or local archive.

## Stack and heap budgets

Task stacks are sized from two numbers, the method of issue #37. The static worst case comes from
the build, and the measured high water mark comes from the board.

- **Static.** `pixi run stack-budget --image waveshare/logger --path` prints each
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
  `DIAG tasks` gives the bytes never used on each task stack (`os-board`,
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
4. Opening the CH343 console resets this board and clears the marks, so read
   them without a new serial session. Wait for the hourly report and pull it
   with `pixi run ble-sync log`, or keep one capture running from before step 1
   with its stdout redirected to a file. Send `parquet mem` only from a console
   session that was already open.
5. Record the smallest value seen per task and heap below, with the date,
   image hash and what was exercised. A task with less than a quarter of its
   stack left (os-board has 12,288 B) is a finding to raise on issue #61.

| Date | Image hash | Exercised | os-board | parquet-sd | os-lan | os_ble_error | nimble_host | tcpip | esp_timer | sys_evt | internal_min | internal_largest | dma_largest | psram_largest |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
