# CoreS3 hardware: pins, ownership, power

Current firmware uses native ESP-IDF 6.1 with M5Unified 0.3.0/M5GFX 0.3.0.
Pins, power and controller ownership remain board-specific. Native builds pass;
the [native bench record](bench-verified.md#board-1-first-native-esp-idf-flash-and-readback)
adds a short SD/USB/sensor/network-time run. Earlier dependency/source snapshots
and measured results below remain dated historical evidence, not current SDK
recommendations. [Current application](../../../firmware/esp-idf-cores3/README.md).

Read when wiring, allocating peripherals, debugging boot, or changing power/sleep. Retrieval snapshot: **2026-09-06**; schematic v1.0, M5Unified 0.2.21, M5GFX 0.2.28. Source inspection unless stated otherwise. Facts confirmed on a real board, including chip revision, flash size and flash mode, live in [bench-verified](bench-verified.md), which overrides this file where they disagree.

## Identify the board first

- **CoreS3 K128:** ESP32-S3, 16 MB flash, **8 MB Quad PSRAM**. Do not select Octal PSRAM because another S3 board uses it. CoreS3 kit includes DinBase/500 mAh battery; the controller itself has Port A, with B/C provided by its base. [Board][board]
- **CoreS3 SE:** same main compute/display/audio/RTC/PMIC family; lacks camera, proximity, BMI270/BMM150 and supplied battery/base. Use its own board detection; missing sensors are expected. CoreS3 Lite has the full sensor set but a different cover/200 mAh battery. [Variants][variants]
- Optional stack modules are separate devices; resolve their physical bus contacts against the table below. Classic Core/Core2 GPIO labels do not transfer to CoreS3.

## GPIO ownership

All numbers below are **ESP32-S3 GPIO numbers**, never expander numbers. Native BSP definitions cross-check the onboard peripherals. [BSP header][bsp]

| Circuit | GPIO assignment | Non-obvious constraint |
| --- | --- | --- |
| Internal I²C | SDA **12**, SCL **11** | Also M-Bus 17/18; distinct from Port A. |
| LCD, 320×240 | MOSI **37**, SCK **36**, CS **3**, D/C **35** | Reset through AW9523 P1_1; backlight through AXP2101, no PWM GPIO. |
| microSD, SPI | MOSI **37**, MISO **35**, SCK **36**, CS **4** | Shared LCD bus; card detect AW9523 P0_4. See [SD usage](cores3-storage.md). |
| Speaker AW88298 | BCLK **34**, LRCLK **33**, data-out **13** | Shares clocks with microphone codec. |
| Microphones ES7210 | BCLK **34**, LRCLK **33**, data-in **14**, MCLK **0** | GPIO0 is also boot strap/M-Bus24. |
| GC0308 camera | D0…D7 = **39,40,41,42,15,16,48,47**; PCLK **45**, VSYNC **46**, HREF **38** | Camera-driver bit names differ from schematic `CAM_D2…D9` net names. |
| Camera control | SCCB SDA **12**, SCL **11**; XCLK/reset/PWDN GPIO = **-1** | Onboard 20 MHz oscillator; actual reset is AW9523 P1_0. |
| USB-C data | D− **19**, D+ **20** | Native USB, not a separate USB-UART bridge. |
| Expander IRQ | **21** | AW9523 `INTN`; service peripheral status over I²C. |
| UART0 | TX **43**, RX **44** | Exposed on M-Bus; boot/log output can reach an attached device. |
| Memory/reserved | **26–32** flash/PSRAM; **22–25** do not exist | GPIO33–37 are usable by the onboard circuits because this board uses Quad memory. |

### External connectors and M-Bus

Port A: **SDA2/SCL1** (yellow/white); B: **IN8/OUT9**; C: **RX18/TX17**, relative to CoreS3. All carry GND and 5 V supply; signals remain **3.3 V logic**. B/C software pin-role ordering and the product page's cable-color ordering differ: wire by signal/connector markings, not assumed color. [Pin tables][pins]

Use the official CoreS3 diagram's numbering/orientation. A mating base schematic may reverse odd/even numbering; match nets/physical contacts before copying a contact number. [Board diagram][board]

| Left contact → net | Right contact → net |
| --- | --- |
| 1 → GND | 2 → GPIO10, ADC1 |
| 3 → GND | 4 → GPIO8, Port B IN |
| 5 → GND | 6 → RST/EN |
| 7 → GPIO37, SPI MOSI | 8 → GPIO5 |
| 9 → GPIO35, SPI MISO/LCD D/C | 10 → GPIO9, Port B OUT |
| 11 → GPIO36, SPI SCK | 12 → 3V3 |
| 13 → GPIO44, UART0 RX | 14 → GPIO43, UART0 TX |
| 15 → GPIO18, Port C RX | 16 → GPIO17, Port C TX |
| 17 → GPIO12, internal SDA | 18 → GPIO11, internal SCL |
| 19 → GPIO2, Port A SDA | 20 → GPIO1, Port A SCL |
| 21 → GPIO6 | 22 → GPIO7 |
| 23 → GPIO13, speaker data | 24 → GPIO0, **onboard MCLK/BOOT** |
| 25 → HVIN via DinBase | 26 → GPIO14, microphone data |
| 27 → HVIN via DinBase | 28 → 5V/BUS_OUT |
| 29 → HVIN via DinBase | 30 → BAT |

GPIO0's generic M-Bus `I2S_LRCK` label does **not** describe the onboard audio clock: LRCLK is GPIO33. Treat HVIN/BAT as power nets, never spare GPIOs. [Schematic][sch]

## Internal I²C and expander

7-bit addresses; use the existing internal-bus owner rather than creating another controller on 11/12. [Board address table][board]

| Address | Device | Address | Device |
| --- | --- | --- | --- |
| 0x21 | GC0308 | 0x23 | LTR-553 |
| 0x34 | AXP2101 | 0x36 | AW88298 |
| 0x38 | FT6336U touch | 0x40 | ES7210 |
| 0x51 | BM8563 RTC | 0x58 | AW9523B |
| 0x69 | BMI270 | 0x10 | BMM150 **behind BMI270 auxiliary I²C** |

BMM150 is not an ordinary directly attached 0x10 device; a simple system-bus scan does not establish its absence. Keep its sensor-hub setup intact. Calibrate with the assembled enclosure; magnets/speaker/stack hardware affect compass results.

Scalar telemetry additions, source-checked **2026-09-08**:

- LTR-553ALS-WA uses address `0x23`, part/manufacturer IDs `0x92`/`0x05`. The existing M5 internal I2C owner can configure it without another controller. Preserve CH1-then-CH0 read ordering, ALS new-data/invalid flags and proximity saturation. Raw light/proximity counts are not lux or distance. [CoreS3-specific datasheet](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/core/K128%20CoreS3/LTR-553ALS-WA.PDF), [M5 example](https://docs.m5stack.com/en/arduino/m5cores3/ltr553).
- M5Unified 0.2.21 `Power_Class::getBatteryCurrent()` returns a constant zero for the CoreS3/AXP2101 path; it is not a measured current. Encode unavailable current as null. [Pinned implementation](https://github.com/m5stack/M5Unified/blob/0.2.21/src/utility/Power_Class.cpp).
- Use the per-sensor mask from `M5.Imu.update()` to distinguish fresh accelerometer, gyro and auxiliary magnetic readings. Do not infer freshness of every sensor from a generic successful getter. Keep magnetic raw counts separate from calibrated field units, and label IMU temperature as die temperature. [Pinned IMU implementation](https://github.com/m5stack/M5Unified/blob/0.2.21/src/utility/IMU_Class.cpp).

Bench follow-up, **2026-09-08**: Parquet rows on Board 1 contained fresh accelerometer/gyro/auxiliary magnetometer data (`imu_fresh_mask=7`) and valid LTR raw light/proximity readings. This establishes acquisition, not lux/distance conversion, compass calibration or physical accuracy. RTC date/time reads still failed and remained null (see the RTC note below: the chip had never been written). Near-zero reported battery voltage does not establish battery presence; the earlier reported `0 mA` was an unsupported API result, not a current measurement. [Measured record](bench-verified.md#board-1-on-device-parquet-and-hive-partitions)

**BM8563 RTC**, source-checked **2026-09-18** against the pinned M5Unified 0.2.21 sources in the toolchain cache (`src/utility/rtc/PCF8563_Class.cpp`, `src/utility/RTC_Class.cpp`):

- The BM8563 is a PCF8563-compatible clock/calendar at `0x51` on the internal bus, one-second resolution, no sub-second register. Register `0x02` bit 7 is the **VL (voltage-low)** flag: set by the chip after a supply drop or first power-up, cleared by the next write of the seconds register. `M5.Rtc.getVoltLow()` reads it.
- `M5.Rtc.getDateTime()` validates the BCD fields and **returns `false` for an unwritten chip** instead of returning a bogus date. A CoreS3 fresh from the box therefore reports "RTC read failed" until firmware writes the calendar once; it is not a bus or hardware fault. This is what Board 1 showed before firmware v6.2; the first v6.2 boot confirmed it (`reason=voltage-low`), and the first host sync cleared it ([measured](bench-verified.md#board-1-rtc-hand-off-host-anchor--bm8563--next-boot-firmware-v62)).
- `M5.begin()` with `internal_rtc = true` calls `setSystemTimeFromRtc()`, which converts the calendar with `TZ=GMT0`, i.e. M5Unified itself treats the chip as **UTC**. This project keeps that convention: the RTC is written in UTC from the host anchor and never holds local time.
- Backup power: the CoreS3 datasheet lists the RTC but the M5Stack pages fetched so far do not state whether the BM8563 runs from the main battery through the AXP2101 or from a separate cell. Retention across a full power-off is therefore **unverified**; the firmware relies on the VL flag rather than on an assumption, and the measurement belongs in `bench-verified.md`.

The later LZ4 comparison used the same board, bus ownership and pins; it did not require a second MCU/framework or explicitly pinned cores. Sensor snapshots survived compression/readback unchanged, with post-write acquisition continuity checked. This is workload evidence, not calibration, battery endurance or radio-concurrency qualification. [Compression bench](bench-verified.md#board-1-lz4-compression)

On ESP32-S3, do not probe reserved 7-bit addresses `0x00` through `0x07`. M5Unified 0.2.21 deliberately starts its full-bus scan at `0x08` because probing the low range can stop the controller. The first diagnostic reproduced that hang on this board; scan only `0x08` through `0x77`. [M5Unified implementation](https://github.com/m5stack/M5Unified/blob/0.2.21/src/utility/I2C_Class.cpp#L105-L112), [measured](bench-verified.md)

AW9523B map; `P0_n/P1_n` are expander bits, not ESP GPIOs. Registers: input **0x00/01**, output **0x02/03**, direction **0x04/05** (`1=input`). [Schematic p5][sch], [Initialization][gfx]; remaining register semantics: [AW9523 datasheet][aw].

| Bank | Bit 0 | Bit 1 | Bit 2 | Bit 3 | Bit 4 | Bit 5 | Bit 6 | Bit 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| P0 | TOUCH_RST | BUS_OUT_EN | AW_RST | ES_INT | TF_SW | USB_OTG_EN | NC | NC |
| P1 | CAM_RST | LCD_RST | TOUCH_INT | AW_INT | NC | NC | NC | BOOST_EN |

Serialize complete read-modify-write operations: an unrelated whole-port write can reset the screen or reverse the supply path. Preserve direction/interrupt configuration. Do not bit-bang camera clocks or run high-rate I/O through this expander.

## Development traps worth remembering

- **LCD + SD:** GPIO35 must be D/C output during LCD selection and high-impedance MISO input otherwise. M5GFX's `Panel_M5StackCoreS3::cs_control()` implements this; locking SPI alone cannot repair a generic driver's wrong pin direction. CoreS3 uses `SPI2_HOST` in M5GFX 0.2.28. That release also detects ILI9342C/E panel variants. [Display implementation][gfx]
- **Audio:** both M5Unified onboard audio configurations select **I2S1**. Assign one clock/driver owner; coordinate playback/recording transitions, or use a deliberately configured full-duplex stack. Changing either stream's sample rate independently can break the shared clocks. `internal_mic=false` does not electrically remove ES7210/0x40. [Pin/initialization source][pins], [Power source][power]
- **Camera:** GC0308 has no native JPEG output; choose RGB565/YUV/grayscale, begin with one small framebuffer, return every `camera_fb_t`. VGA RGB565 alone is 614,400 bytes; PSRAM bandwidth competes with Wi-Fi. Reuse the internal SCCB bus and perform PMIC/expander initialization first. [Camera driver constraints][camera]
- **Candidate expansion pins:** 5/6/7/10 first; 1/2,8/9,17/18 only when their ports are unused. GPIO1–10 are ADC1; 11–20 ADC2. Recheck physical stack occupancy before assigning LEDC/RMT/UART/SPI by GPIO matrix. `gpio_dump_io_configuration` exposes accidental ownership changes. [GPIO reference][gpio]
- **Boot/debug:** straps **0,3,45,46** already have board functions; do not add pulls/drivers that change reset levels. Camera occupies external JTAG pins39–42; USB Serial/JTAG uses19/20. Native USB-OTG and Serial/JTAG share the internal PHY: inspect the USB-mode configuration before expecting both simultaneously. [SoC datasheet][soc]
- **eFuse says nothing about PSRAM here:** `PSRAM_CAP`, `PSRAM_VENDOR` and the derived `PSRAM_CAPACITY` all read zero on a measured CoreS3, because those fuses describe in-package PSRAM and this board carries its 8 MB Quad PSRAM as a separate part. A zero reading is not an absent-PSRAM finding. Confirm size at runtime instead, and treat a runtime zero as a Quad-versus-Octal build error. [Measured](bench-verified.md)
- **Recovery:** hold RST ~3 s until green LED, then release for download mode. Never wait indefinitely for a USB reader in unattended native firmware. [Board recovery][board]
- **Reading live data need not reset the board.** On the measured macOS native-USB connection, the first Parquet helper's DTR/RTS settings caused resets on open. Its corrected settings preserved the boot and RAM batch across separate reads; use that bounded helper rather than assuming every serial client is non-resetting. Esptool's read-only chip query also resets the application on exit in the current workflow. [Host lesson](bench-verified.md#measured-facts-that-changed-how-we-work)
- **A board in ROM download mode prints nothing.** Measured, a CoreS3 held in download mode enumerates as USB Serial/JTAG and answers `esptool` normally, yet emits not one byte on the CDC port and ignores a REPL interrupt, because no application is running. Silence there is the expected state, not a dead board. `esptool chip-id` succeeding while the port stays quiet is the quickest way to tell the two apart. [Measured](bench-verified.md)

## Power and sleep

- Decide supply direction **before** `M5.begin()`: official example sets `cfg.output_power=false` when receiving power through Grove/DC. When supplying an add-on, use the board power API; verify actual rail/current behavior. DinBase accepts 9–24 V at its intended DC input; do not feed that into 5V/3V3. [Power example][power-example], [DinBase schematic][din]
- AXP2101 rails: **ALDO1 1.8 V amplifier**, **ALDO2 3.3 V codec**, **ALDO3 3.3 V camera**, **ALDO4 3.3 V SD**; LCD backlight **DLDO1**. Board initialization enables rails even if an application never reads the corresponding peripheral. [Power implementation][power], [Display implementation][gfx]
- `BUS_OUT_EN`/`USB_OTG_EN` select power direction; `BOOST_EN` supplies the boost path. Port/BUS supply control is shared, not independent per connector. Use `M5.Power` methods; raw expander writes can invalidate that state. [Power implementation][power]
- Deep sleep does not automatically turn off peripheral rails. Quiesce storage/audio/camera/radio, select retained rails, then measure current. RTC alarm→AXP wake is distinct from ESP GPIO wake. Touch wake follows FT6336→AW9523 P1_2→INTN→GPIO21; clear touch data **and** read expander input state when rearming. [Wake implementation][pins]

## Open only for the relevant subsystem

[Board][board]: schematic, all peripheral datasheets, mechanical files, magnet warning; [ESP32-S3][soc]: electrical limits, straps, GPIO matrix; [M-Bus footprint](https://github.com/m5stack/M5_Hardware/blob/master/Common/Module_Type_A/Footprints/Module_Type_A_CoreS3_M5_Bus.PcbDoc): custom stack PCB. For clocking/DMA/cache tuning, search the selected IDF release for `LCD_CAM`, `I2S full duplex`, `PSRAM DMA`, `USB PHY`, `ADC calibration`.

[board]: https://docs.m5stack.com/en/core/CoreS3
[variants]: https://docs.m5stack.com/en/core/M5CoreS3%20SE
[sch]: https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/490/Sch_M5_CoreS3_v1.0.pdf
[din]: https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/559/SCH_DinBase_V1.1.pdf
[aw]: https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/products/core/CoreS3/AW9523B-EN.pdf
[pins]: https://github.com/m5stack/M5Unified/blob/0.2.21/src/M5Unified.cpp
[power]: https://github.com/m5stack/M5Unified/blob/0.2.21/src/utility/Power_Class.cpp
[gfx]: https://github.com/m5stack/M5GFX/blob/0.2.28/src/M5GFX.cpp
[power-example]: https://docs.m5stack.com/en/arduino/m5cores3/power
[bsp]: https://github.com/espressif/esp-bsp/blob/master/bsp/m5stack_core_s3/include/bsp/m5stack_core_s3.h
[camera]: https://github.com/espressif/esp32-camera/blob/master/README.md
[gpio]: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/gpio.html
[soc]: https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf
