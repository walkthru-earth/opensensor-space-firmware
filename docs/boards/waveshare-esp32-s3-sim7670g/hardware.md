# Waveshare ESP32-S3-SIM7670G-4G V2 hardware

The current board adapter uses native ESP-IDF 6.1 UART, I²C and SDMMC drivers,
with led_strip 3.1.1 for GPIO38. Native builds pass. A short native USB and SD readback of v2.15 on 2026-10-26 is in the
[bench record](bench-verified.md#main-689bb61-usb-reflash-thing-id-and-first-geo-footer-2026-10-26).
Phone sync, GNSS fix, endurance and power-cut durability remain unqualified.
The source checks and measured runs below retain their dated
historical Arduino-image scope. [Current firmware](../../../firmware/esp-idf-waveshare-sim7670g/README.md).

Source-checked 2026-09-28 for PCB revision **V2.0**, as confirmed by the owner. Identification, flash capacity/type, PSRAM initialization/readback, eFuse status, PMS UART reception and bounded logger TF write/serial readback were measured on the unit. Remaining pin assignments and optional peripheral function are source-checked; RGB visual operation, encrypted BLE transfer and Wi-Fi/LAN remain unverified. See the [bench record](bench-verified.md). [Waveshare product documentation](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G), [Arduino guide with V1/V2 tables](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G/Arduino), [FAQ and recovery](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G/FAQ).

| Function | V2 pins / source status | Ownership rule |
| --- | --- | --- |
| PMS5003T UART | Host RX GPIO1, TX GPIO2, per owner's four-wire connection | Dedicated native `UART_NUM_1`, 9600 8N1; sensor TX crosses to host RX |
| TF slot | SDMMC CLK GPIO5, CMD GPIO4, DATA0 GPIO6. The vendor example calls GPIO46 card detect, but the schematic routes it to camera U13 pin 8 (unverified on the board) | One storage worker; use SDMMC one-bit mode, not CoreS3 SPI pins |
| Battery gauge MAX17048 | SDA GPIO15, SCL GPIO16 on V2 | Logger adapter owns this I²C bus with explicit installed-battery configuration; V1 GPIO3/GPIO2 mapping is wrong here. A bounded USB-attached readback succeeded on 2026-09-30. |
| Camera | V2 mapping includes XCLK GPIO39 and PCLK GPIO46 | Camera and TF card detect need a conflict check before use together |
| RGB LED | GPIO38 | Logger implements startup/card/sensor state; visual operation unverified; no file-copy or durability claim |
| SIM7670G GNSS AT port | ESP TX GPIO18 to modem RXD, ESP RX GPIO17 from modem TXD, through the TXB0104 level shifter. Modem supply enable GPIO21, active high | Logger v2.17 owns `UART_NUM_2` at 115200 8N1 for GNSS AT queries only and latches GPIO21 high. It never drives GPIO21 low, because the pin ORs with the 4G DIP switch. GPIO45 (DTR, a strapping pin) and GPIO40 stay unconfigured. The diagnostic does not touch the modem |
| SIM7670G data, camera | Onboard | Not initialized by the diagnostic or current logger adapter. No network, SIM, band, NVM, CFUN or reset command is sent |

Waveshare identifies the module as ESP32-S3R8 with **16 MB flash and 8 MB PSRAM**. Its FAQ gives conflicting PSRAM-mode advice. The first diagnostic leaves PSRAM disabled. A separate 2026-09-28 OPI build booted on this unit, reported initialized physical/heap capacity of **8,388,608 bytes**, and passed four patterns over a 512 KiB allocation. The logger consequently selects **Quad/QIO flash with OPI PSRAM** (native `CONFIG_SPIRAM_MODE_OCT`; historical Arduino `qio_opi` SDK), checks initialized physical capacity before starting, and puts its bounded writer workspace in PSRAM. The full logger subsequently booted and wrote/read Parquet files; this establishes bounded operation, not all-address or endurance coverage. Evidence and image identities belong in [bench-verified.md](bench-verified.md). Do not reuse CoreS3's Quad-PSRAM setting.

Before any first flash write on another unit, run the root `ports`, `chip`, `flash-id`, `efuse` and full verified-backup sequence. Firmware build wrappers verify the exact locked target and selected SDK; board identity and flash size still require physical readback.

The board's UART download and BOOT+RESET recovery are described in the [Waveshare FAQ](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G/FAQ). Verify the actual port and recovery behavior on this V2 unit before treating CoreS3 serial-control observations as portable. Strapping pin GPIO46 is already used for card detect in the vendor example; do not add a pull or driver to it.

The pictured PMS power connection uses the exposed 5 V and GND. Confirm the selected 5 V rail is present under the board's chosen USB/battery supply mode before attributing absent UART frames to the sensor. No board power measurement has been made here.

### GNSS (logger v2.17, owner approved 2026-10-27)

Pins come from the [V2.0 schematic](https://files.waveshare.com/wiki/ESP32-S3-A-SIM7670X-4G-HAT/ESP32-S3-A-SIM7670X-4G-V2.pdf) (SHA-256 `91877a6627b21acb8e20c08a7b3667ad4879a06d9883a878be1abfced57671ca`). The TXB0104 shifter is powered from the modem VDD_EXT rail, so the UART is silent until the modem is powered. UART0 is the console and UART1 the PMS5003T, so GNSS takes UART2.

The board code only fills `opensensor::gnss::ReaderConfig` with these pins and a GPIO21 power callback. The reader task, the SIMCom dialect and the row rule live in `firmware/common/gnss` and `firmware/common/gnss_reader`. The task polls `AT+CGNSSPWR?` until the engine reports on, sends `AT+CGNSSPWR=1` when it is off, and then sends `AT+CGNSSINFO` about every 10 s, per the [SIM767XX AT manual](https://files.waveshare.com/wiki/SIM7670G-LTE-Cat-1-GNSS-HAT/SIM767XX_Series_AT_Command_Manual_V1.01.pdf). It does not wait for the `+CGNSSPWR: READY!` URC.

Bench status. On 2026-10-27 module firmware V1.9.05 answered `+CGNSSPWR: 1` and the empty no-fix reply `+CGNSSINFO: ,,,,,,,,` indoors. No fixed line has been read from this unit yet, so the fixed field layout is taken from the AT manual and stays unverified on this firmware until a dated bench record in [bench-verified.md](bench-verified.md) shows one. The parser finds the latitude, N or S, longitude, E or W run wherever it sits, so a different number of leading fields still parses.

The current logger has no external RTC adapter. UTC stays null after boot until an explicit host anchor or successful native Wi-Fi SNTP response arrives; GNSS/modem time is not substituted. The owner subsequently installed an 18650 and verified battery-only boot. The logger explicitly configures it as installed and samples MAX17048 cell voltage and estimated SOC; an I²C acknowledgement alone still would not establish battery presence. The V2 schematic routes ETA6098 `STAT` and solar-charger `CHRG` to indicator circuits without a verified ESP32 input, so the logger does not infer charging from voltage or USB attachment.

### Battery health inputs (schematic reading, unverified on the board)

A 2026-10-16 schematic review, not yet confirmed with a meter, found the following.

- ETA6098 `STAT` ends in an unconnected stub. It does not drive an LED or an ESP32 pin.
- CN3791 solar charger `CHRG` drives LED2 only.
- MAX17048 `ALRT` is tied to ground through R45, so gauge alerts cannot reach the ESP32. The logger polls STATUS instead.
- GPIO46 reaches camera connector U13 pin 8. It is not a confirmed TF card detect line.
- On USB, the SY8105 path holds VBAT near 4.267 V, so VCELL reads the rail, not the resting cell.

The logger therefore classifies battery state from VCELL alone. `battery_health_state` 1 (external power likely) means VCELL is at or above 4240 mV, a heuristic, not a measured USB or charger signal. Low enters at 3500 mV and critical at 3350 mV with 75 mV hysteresis and three-sample debounce. These thresholds are uncalibrated, with no dated discharge run yet.

VCELL and SOC are read and range checked on their own. When VCELL is valid but SOC fails to read or is above 120% raw, the row still writes `battery_mv` and the classifier still uses it, so Low and Critical still trigger. `battery_percent`, `battery_soc_raw_percent` and `battery_minutes_to_empty` stay null and `gauge_status` is 1. When SOC is valid but VCELL fails, `battery_percent` and `battery_soc_raw_percent` are written and `battery_mv` stays null. `gauge_status` 2 needs both words. CRATE, STATUS and MODE columns depend only on their own register. With a battery configured, `battery_health_state` is always written. It keeps the last state through up to two invalid VCELL samples and reads 0 unknown from the third.

## Power slider and DIP switches

The [product hardware description](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G#hardware-description) identifies the single ON/OFF slider beside the battery holder as the **18650 battery power switch**. Use ON for battery-powered operation. USB already powered the diagnostic during this board's tests; those tests do not establish battery operation or the sensor's 5 V supply under either slider position. The slider is separate from BOOT/RESET.

The [FAQ](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G/FAQ#hardware-functions) lists the separate DIP switches: **CAM** enables the camera, **HUB** powers the USB hub, **4G** powers the cellular module, and **USB** selects its USB path. None is documented as a PMS5003T enable. Keep the working USB/UART configuration for host diagnostics; switching HUB off may remove host USB access. Identify the switch by its position/label before applying these instructions.
