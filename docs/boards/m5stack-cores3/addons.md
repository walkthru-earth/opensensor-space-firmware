# Optional add-on integration

[Router](../../README.md) · Read only when attaching a Unit (cable), Module (stack), Base, or third-party peripheral. CoreS3 remains the host; an accessory is never assumed to be built in. Source snapshot **2026-09-06**, integration lessons updated **2026-09-08**.

## Resolve the actual connection

1. Record accessory **SKU + PCB revision + fitted chips + switch/jumper positions**. Product titles and old examples can describe different revisions; use schematic nets and chip datasheets.
2. Map **physical connector position → CoreS3 GPIO → signal direction from the host**. Legacy Core/Core2 GPIO labels do not carry across M-Bus. Compare the entire assembled stack using [M5 Stack Compatibility](https://docs.m5stack.com/en/compatible_stack?host=K128), then verify against [CoreS3 hardware](cores3-hardware.md) and each schematic.
3. Reserve power rail, GPIOs, peripheral/controller, I²C address or SPI CS, interrupt/reset/enable, and driver owner. Check shared onboard loads too; changing a software pin cannot reroute a soldered stack net.
4. Check supply separately from logic level, peak/inrush current, rail gating, pull-ups and combined bus capacitance. Grove/stack form factor alone does not establish electrical compatibility or support insertion while powered. [M5-Bus interface](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/static/pdf/static/en/start/interface/mbus.pdf).

## Integration choices

| Interface | Decision / trap |
| --- | --- |
| I²C | Scan only the intended powered bus; ACK is not chip identification. Address collisions require an address strap, isolated branch/multiplexer, or physical rerouting. A second software instance on the same wires does not isolate devices. [M5 address table](https://docs.m5stack.com/en/product_i2c_addr), [IDF bus constraints](https://docs.espressif.com/projects/esp-idf/en/v6.1.1/esp32s3/api-reference/peripherals/i2c.html). |
| SPI | Dedicated CS per device; verify inactive MISO is high impedance, per-device mode/clock and common arbitration. CoreS3 GPIO35 has a display-specific direction change; see [SD/SPI](cores3-storage.md). |
| UART | Cross TX→RX; reserve a hardware UART and explicit pins; keep console output out of device protocols. Multiple talkers cannot share ordinary UART RX without switching/arbitration. |
| CAN/RS485/RS232 | Peripheral capability is not a line transceiver. Lookup: TWAI transceiver + termination; RS485 DE/RE half-duplex; RS232 level converter. |
| USB or another radio | Check host/device role, VBUS budget, PHY use and driver support. Independently active RF channels require independent radio hardware; see [wireless](cores3-wireless.md). |

Driver discovery, on demand: [M5UnitUnified](https://github.com/m5stack/M5UnitUnified), [M5Stack libraries](https://github.com/orgs/m5stack/repositories), [ESP Component Registry](https://components.espressif.com/). Verify supported chip and bus-injection API before pinning a version; a driver must accept the application's existing bus owner.

## Individual reference format

Create `docs/boards/m5stack-cores3/addon-<name>.md` only for an accessory actually being investigated. Keep: identity/variants; supply + logic + current; host pin map/address; protocol/timing/units; CoreS3 conflicts and workable wiring; driver/version evidence; failure/recovery notes; schematic/datasheet/example links; verification date and unresolved revision differences. Generic setup APIs need only a keyword/link.

Available: [M134 air-quality module / PMSA003, SHT20 when fitted](addon-air-quality.md). This is an optional accessory reference, not the definition of the project hardware.

## Lessons from the first attached sensor

Board 1's PMS UART path was verified through a real ten-minute Parquet batch; that does not identify every fitted chip or qualify the whole accessory. The possible SHT20/ES7210 `0x40` collision remains unresolved, and no SHT20 values are acquired. Record physical revision/population and isolation before enabling another driver; disabling microphone sampling is not electrical isolation. [Accessory evidence](addon-air-quality.md), [bench scope](bench-verified.md)

Missing, warming, stale or erroring sensors must produce explicit status/nulls rather than plausible zeros. Keep sensor acquisition separate from storage and display refresh; an add-on with a slower conversion period needs its own cached value and freshness rule. Adding a field requires updating the schema, independent-reader fixtures and bench evidence without claiming that earlier files tested the new field.
