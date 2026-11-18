# Optional accessory: M134 air-quality module

Read only when this accessory is selected; CoreS3 does not contain these sensors.
Source-checked 2026-09-06 and bench-updated 2026-09-08. Wiring remains a schematic comparison; the PMS UART path is now verified on Board 1.

## Identify the hardware first

- The supplied [shop URL](https://shop.m5stack.com/products/pm2-5-air-quality-kit) now names **M134 / PMSA003 module**, excluding the controller. The historical kit bundled a classic ESP32 Core and SHT20; its firmware is not a CoreS3 board definition.
- Current [M134 documentation](https://docs.m5stack.com/en/products/sku/M134) lists PMSA003; its linked [schematic](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1030/Module-Air-Quality.pdf) also contains **U2 SHT20**. Check actual PCB revision/population; absence from the product specification table does not establish absence of SHT20.
- PMSA003 is the UART particulate sensor; temperature/RH comes from the separate SHT20.

## CoreS3 connection and collision audit

Physical **M5-Bus position** is the translation key; old schematic `GPIOxx` labels name the classic Core. Map positions through the [CoreS3 pinout](https://docs.m5stack.com/en/core/CoreS3#pinmap).

| Accessory signal | Physical bus position | CoreS3 connection |
| --- | --- | --- |
| PMS TX → host RX | 15 | GPIO18; also Port C RX |
| PMS RX ← host TX | 16 | GPIO17; also Port C TX |
| SHT20 SDA / SCL | 17 / 18 | GPIO12 / GPIO11; **internal** I²C |
| PMS SET | 22 | GPIO7; reserve if fitted |
| PMS RESET | 6 | RST/EN, shared host reset |
| Supply | 28 / 12 / 1,3,5 | 5 V / 3.3 V / GND |

The [schematic](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1030/Module-Air-Quality.pdf) routes SET/RESET where the web PinMap says NC. Verify actual revision continuity; reset is shared with the host.

**SHT20 and CoreS3 ES7210 both use address `0x40` on bus positions 17/18.** This collision follows from the schematic, [SHT20 datasheet §5.3](https://sensirion.com/resource/datasheet/sht20), and [CoreS3 codec pinout](https://docs.m5stack.com/en/core/CoreS3#pinmap).
Use a routing adapter that disconnects the accessory SDA/SCL from the internal bus and connects them to external Port A GPIO2/GPIO1, or electrically isolate that accessory branch behind a mux. Adding wires while retaining the original connections joins the buses; software address changes or disabling microphone sampling do not isolate two fixed-address devices. For PM-only operation, isolate the accessory I²C branch before normal codec initialization too.

Reserve GPIO17/18 against other Port C/UART users. The native adapter configures its dedicated UART driver for 9600 8N1 with host RX18/TX17; two sensor TX outputs cannot share RX.
The schematic's J3 USB-C connects VBUS directly to bus 5 V and carries no USB data; check the host power path before feeding it. [Schematic](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1030/Module-Air-Quality.pdf)

## PMSA003: wire contract

[Plantower datasheet V2.5](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/base/PMSA003_cn.pdf): electrical/interface pp.2–5; installation p.11; protocol appendices pp.12–14.

| Item | Contract |
| --- | --- |
| Power / logic | Sensor 4.5–5.5 V, ≤100 mA working / ≤200 µA standby; UART/control 3.3 V |
| Sensor connector | 1/2 VCC; 3/4 GND; 5 RESET; 7 RX; 9 TX; 10 SET; 6/8 unused |
| SET / RESET | SET low sleeps; high/floating runs. RESET low resets; internal pull-ups |
| Wake | Allow ≥30 s fan stabilization after sleep; budget the same acquisition gate at startup |
| Serial | 9600 8N1; 32-byte measurement frame; big-endian words |
| Frame | `42 4D`, length `001C`; checksum `sum(bytes[0..29]) == BE16(bytes[30..31])` |
| PM fields | Byte offsets 4/6/8: CF=1 PM1/2.5/10; 10/12/14: atmospheric PM1/2.5/10; µg/m³ |
| Counts / status | Offsets 16..26: counts per 0.1 L above 0.3/0.5/1/2.5/5/10 µm; 28 version, 29 error |
| Commands | `42 4D CMD 00 DATA checksum16`; E1: passive=0/active=1; E2: passive read; E4: sleep=0/wake=1 |

Default active output has variable cadence and repeated frames; passive mode changes reporting, not fan power. Keep inlet/outlet clear and prevent recirculation; enclosure instructions are on p.11.

Bench result, **2026-09-08**: Board 1 received checksum-valid active PMSA003 frames on GPIO18/GPIO17. The initial 24-second capture produced two 10-second application reports with fresh-frame ages below one second and zero parser checksum or length failures across 23 frames. The later automatic 60-row Parquet batch retained all twelve PM/count channels and status fields; stored checksum/length-error, row-drop and missed-deadline counters were zero. Real SD readback matched in PyArrow and DuckDB. This verifies transport, parsing and bounded acquisition/storage on the attached unit, not long-duration reliability, physical warm-up sufficiency or calibration. [Exact run](bench-verified.md#board-1-on-device-parquet-and-hive-partitions)

### Current logger validity and display lessons

The application continuously drains active UART frames and snapshots the latest accepted frame once per ten-second row. These are snapshots, not ten-second averages. Its status precedence is `0=missing`, `1=warming` (device uptime below 30 seconds), `2=stale` (accepted frame age above 5,000 ms), `3=sensor-error`, `4=valid`. All twelve PM/count values are null unless status is 4; frame age, firmware/error and parser counters preserve diagnostic context when available. The uptime gate is not a tracked sensor wake timestamp; revisit it before introducing sleep/wake or independent sensor resets. [Implementation](../../../firmware/esp-idf-cores3/bringup/telemetry_logger.cpp)

The LCD/`MEAS` report refreshes independently of the Parquet row and can observe a different, newer sensor frame. Do not compare adjacent display lines as if they were the exact stored values. Both use the row's status rule, `pms_status_of` in [telemetry_contract.h](../../../firmware/esp-idf-cores3/bringup/telemetry_contract.h) (#165). The PM pages show measurements only while the status is valid, else the status. Frame age and the error byte appear only once an accepted frame exists. Before the first frame the pages show a waiting message and the parser counts. `MEAS` prints `values_valid=true` only when the frame it evaluates classifies as status 4 under the row's rule, and it names the status, missing before the first frame. Frame ages saturate at 2^32 - 1 ms, so a silent sensor never reads fresh again. The device page shows `Logging on` or `Logging off` with the reason (not started, erased, reboot after a UART archive erase, no card, card full, card retry, writer stopped) and the rows dropped since boot, live from the logger. The capture-to-file check matched explicit row sequence/monotonic timestamps; the two host readers compared the same stored values/nulls. A valid UART frame and a software warm-up gate do not validate environmental accuracy.

The later 90-row uncompressed file and three paired 60-row LZ4 comparisons also preserved PMS values/status through real SD readback, with zero recorded parser/drop/deadline errors in the checked rows. Compression reduced file bytes without changing sensor values; it does not improve sensor accuracy or resolve SHT20 isolation. [Dated codec evidence](bench-verified.md#board-1-lz4-compression)

Parser keywords: bounded streaming, resynchronization, length/checksum/status validation, partial/concatenated-frame tests, stale/error counters, explicit byte decoding. Keep consumption independent of uploads/display. Timeouts mean invalid samples, not zero pollution. Use timestamp-based windows; variable/repeated frames distort frame-count averages. Preserve calibration-field identity; atmospheric fields suit ambient monitoring.

Driver lookup: ESP-IDF `driver/uart.h`, `uart_param_config`, `uart_set_pin`, `uart_driver_install` and bounded ring-buffer reads. [M5 classic PM25 example](https://github.com/m5stack/M5Stack/tree/master/examples/KIT/PM25): protocol cross-check only. A separate PMS library is optional.

## SHT20, only when populated and electrically isolated

The current firmware does **not** acquire SHT20. Electrical isolation/population has not been verified on the attached assembly, so `ambient_temperature_c` and `relative_humidity_percent` remain null. Do not substitute BMI270 die temperature or treat an ACK at the shared `0x40` address as proof that SHT20 is independently readable.

- Address `0x40`; sensor supply 2.1–3.6 V. M134 schematic supplies 3.3 V and adds 4.7 kΩ pull-ups; account for parallel pull-ups when rerouting.
- Historical Arduino register/timing reference, not a native dependency: [`Sensirion/arduino-sht` v1.2.6](https://github.com/Sensirion/arduino-sht/releases/tag/v1.2.6), released 2024-11-08; recheck [latest release](https://github.com/Sensirion/arduino-sht/releases/latest) when selecting dependencies. Its explicit `SHTSensor::SHT2X` selection illustrates model-specific probing; a native implementation must borrow board I²C callbacks and preserve isolation. [Tagged source](https://github.com/Sensirion/arduino-sht/blob/v1.2.6/SHTSensor.cpp)
- That driver waits 85 ms for each of T and RH: run acquisition outside latency-sensitive callbacks and compute dew point/VPD from the cached pair. It checks CRC-8 (`0x31`, initial `0`) and status bits before conversion. Keep derived values floating-point. [Tagged implementation](https://github.com/Sensirion/arduino-sht/blob/v1.2.6/SHTSensor.cpp)
- C driver keywords: no-hold `F3/F5`, busy NACK, conversion deadline, CRC before clearing low two status bits; hold mode stretches SCL. **No-hold erratum:** SCL rising edges 15–18.5 µs after command ACK can cause persistent offsets; implement a documented workaround and inspect controller timing. A delay after `endTransmission()` alone does not prove compliant STOP timing. [Datasheet §§5.4–5.8](https://sensirion.com/resource/datasheet/sht20)
- Keep measurement duty below 10%; sample conservatively, e.g. one T/RH pair per 5 s. Thermal separation from the host matters: high T biases RH low; characterize the assembled enclosure before choosing a correction. [Datasheet §2.3](https://sensirion.com/resource/datasheet/sht20)

## Historical repository: narrowly reusable evidence

Explored [`koenvervloesem/M5Stack-Air-Quality-ESPHome` at `6944e355734da4dd42ca969f2c05621a9343eaaa`](https://github.com/koenvervloesem/M5Stack-Air-Quality-ESPHome/tree/6944e355734da4dd42ca969f2c05621a9343eaaa), commit dated 2023-03-16.

- `requirements.txt`: ESPHome **2022.11.0**, Pillow 9.3.0; YAML targets **m5stack-core-esp32**, PMS RX16, I²C21/22 and classic display/button pins. Reuse sensor intent/UI ideas, not dependencies, GPIOs or board initialization.
- `include/sht20.h` polls every **5 s** although the README says one second; additional VPD/dew-point calls remeasure temperature and `int` locals discard fractional humidity/derived values. Cache one pair and calculate locally.
- Its reported hot temperature/low RH is a useful enclosure-test hypothesis, not proof of every revision's behavior. Its disabled BLE provisioning comment records an old firmware memory failure, not a CoreS3 Wi-Fi/BLE limitation.
- If revisiting ESPHome, look up current `pmsx003`, SHT2x support and external-component migration; this file's C/C++ guidance does not depend on ESPHome.
