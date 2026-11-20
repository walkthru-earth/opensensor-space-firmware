# CrowPanel 4.2" e-paper native ESP-IDF / SEN63C logger

The ELECROW CrowPanel ESP32-S3 4.2" E-paper HMI v1.2 logs a Sensirion SEN63C
to Parquet on the microSD card every 10 seconds and shows the readings on its
400 x 300 black and white panel. The source is pure **ESP-IDF 6.1**, firmware
identity **`idf-crowpanel-parquet-v0.1`**, with the **39-column** schema and
dictionary `crowpanel-epaper-telemetry-v1`. There are no Arduino APIs or
Arduino component. BLE and LAN sync, the storage worker, the clock and
location rules are the shared ones the other boards use.

**No hardware run is recorded yet.** Host builds and contract tests pass. The
[bench record](../../docs/boards/elecrow-crowpanel-epaper-42/bench-verified.md)
gets a dated entry for each physical check. Wiring and first flash steps are in
the [board guide](../../docs/boards/elecrow-crowpanel-epaper-42/README.md).

## Columns

The SEN63C columns are `sen6x_status` (0 absent, 1 warming, 2 stale, 3 sensor
error, 4 valid), `sen6x_device_status` (the raw device status register),
`pm1_mass_ug_m3`, `pm25_mass_ug_m3`, `pm4_mass_ug_m3`, `pm10_mass_ug_m3`,
`ambient_temperature_c`, `relative_humidity_percent` and `co2_ppm`. PM and CO2
are null unless the status is valid. Temperature and humidity are null when the
sensor is absent, stale or reports an RHT error. The rest are the shared
system, clock and location columns.

LIVE keys are `seq`, `mono`, `utc`, `sen`, `m1`, `m25`, `m4`, `m10`, `t`, `rh`
and `co2`, see the [protocol](../../docs/shared/ble-sync-protocol.md).

## Screen

Nine pages. Page 0 is the dashboard, a large PM2.5 value with its air quality
band and tiles for CO2, temperature, humidity, PM1, PM4 and PM10, each with a
2 hour sparkline. Pages 1 to 7 chart one metric each over 24 hours. Page 8 is
the status page with the clock, uptime, sample count, card, Bluetooth,
Wi-Fi, Thing id, firmware and sensor state.

| Key | Action |
| --- | --- |
| Rotary down or up | Next or previous page, wraps |
| Rotary press | Full refresh of the current page, clears ghosting |
| Menu | Opens the settings popup over any page |
| Exit | Back to the dashboard |

While the popup is open the knob picks a row and a press changes it. On
Altitude and Temp offset a press starts an edit, turn to adjust, press to
keep, Exit to cancel. Menu or Exit closes the popup. The rows and what they
do are in the
[board guide](../../docs/boards/elecrow-crowpanel-epaper-42/README.md#keys-and-the-settings-popup).

Updates use partial refresh, with a full refresh on every page change and on
every 30th update. While a phone pairs, the screen shows the six digit BLE
passkey and the device name. The passkey never reaches a log. Chart history
lives in RAM and starts empty after each reset.

## Build and flash

```sh
pixi run idf-setup
pixi run crowpanel-build
pixi run crowpanel-contract-test --sanitize
pixi run epaper-ui-test
pixi run epaper-ui-preview   # PNG pages in artifacts/epaper-ui-preview/400x300/
```

The pages come from the shared `firmware/common/epaper_ui` component. This
board adds its UC8276C driver and `display/display_profile.h`.

The image is 1,744,112 bytes in a 3.75 MiB (0x3C0000) OTA slot, 44.4% of the
slot, under the 50% CI budget (2026-11-20).
Flashing follows the root checklist. The first flash of a factory board needs
the full erase in the [board guide](../../docs/boards/elecrow-crowpanel-epaper-42/README.md#first-flash).
Later flashes use the backup-gated helper.

```sh
pixi run backup --board crowpanel --port <checked port>
pixi run crowpanel-flash <checked port> backup/crowpanel-epaper-42-flash-<stamp>.bin
```

## Open decisions

- `SensorPolicy` is unset, so the board refuses sensor identity provisioning
  until the owner picks a policy.
- `self_test_field` is `pm25_mass_ug_m3`. The boot self test (#40) marks the
  running app valid only after that column holds a valid value within 180 s,
  so a board without a working SEN63C never passes it.
- No STAC File Item sidecar yet. The app leaves `Config::stac_columns` unset,
  because `stac-item-test` proves every board that writes the sidecar against a
  stac-opensensor example and that repo has no CrowPanel example. Adding one is
  a contract change in stac-opensensor first, then this board opts in.
