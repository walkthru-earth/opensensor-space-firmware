# CoreS3 wireless: constraints and tuning

The current application uses native ESP-IDF 6.1 NimBLE, Wi-Fi events/netif/SNTP
and managed mDNS 1.15.1. See [native source and qualification](../../../firmware/esp-idf-cores3/README.md#native-board-ownership-and-qualification)
and the [shared wire contract](../../shared/ble-sync-protocol.md). Native radio
checks restored the original Bluetooth identity and two bonds, and observed
network UTC; bonded reconnect and transfers remain pending. Arduino APIs, performance and phone behavior
below describe historical images and are not native bench evidence.

Load for Wi-Fi/BLE/ESP-NOW design or debugging. Source-checked 2026-09-06; implementation scope updated 2026-09-17; target **ESP32-S3**. Only short BLE sync sessions and one Wi-Fi scan have been bench-verified in this project; no Wi-Fi association or coexistence load has.
SDK/library pins: [development](cores3-development.md). Linked `stable` guides currently describe IDF 6.1; select the installed SDK version before copying APIs/Kconfig.

The historical Arduino trial logged Parquet to SD without ESP-NOW, NTP or object-storage upload; since 2026-09-16 it runs a BLE GATT sync service ([contract](../../shared/ble-sync-protocol.md), [measurements](bench-verified.md#board-1-bluetooth-le-sync)) and since protocol v2 an optional Wi-Fi station with a LAN sync server ([below](#wi-fi-station-and-lan-sync-in-the-arduino-trial)). Its UTC comes from an explicit USB host or phone command, not a network clock. Separate acquisition/storage tasks and available internal heap do not establish headroom under TLS/radio traffic or a dual-core speedup. When upload is added, preserve immutable file identities, read SD in bounded chunks, release the display/SD mutex before network waits, and remeasure jitter, drops and memory under reconnect/coexistence load. The acknowledgement/retention design is still planned in the [telemetry pipeline](../../shared/telemetry-pipeline.md#upload-and-cloud-layout).

## Radio and channel model

The LZ4 SD benchmark establishes local feasibility and smaller payloads, not network throughput or successful synchronization. Offline capacity is a storage projection; continuous power, clock handling and acknowledged reconnect/upload still need their own design/tests. See [offline capacity and reconnection](../../shared/telemetry-pipeline.md#offline-capacity-and-reconnection). Plain Parquet object upload can precede Iceberg.

| Requirement | CoreS3 behavior / decision |
|---|---|
| Wi-Fi | 2.4 GHz 802.11 b/g/n (Wi-Fi 4), HT20/HT40; no integrated 5/6 GHz or Wi-Fi 6. [SoC][soc] |
| Bluetooth | Bluetooth 5 LE; no Classic BR/EDR, SPP `BluetoothSerial`, A2DP or HFP. A BLE GATT UART service is possible. [Chip capability][bt-architecture] |
| STA + SoftAP | One shared home channel; upstream STA channel wins and SoftAP migrates. Multiple AP clients are connections, not independent channels. [Home channel][wifi-overview] |
| BLE + Wi-Fi | One shared RF resource, time divided by coexistence; two CPU cores do not create two radios. [Coexistence][coex] |
| HT40 / BLE channels | HT40 bonds spectrum for one Wi-Fi link; BLE hopping/multiple links do not add independent Wi-Fi interfaces. [SoC][soc], [BLE stack][bt] |
| Independent channels/bands | Evaluate a second MCU/radio via UART/SPI or supported USB/network coprocessor; keywords: ESP-Hosted, ESP-AT, USB host class driver, VBUS budget, antenna isolation. |

## ESP-NOW with infrastructure Wi-Fi

- Start Wi-Fi before ESP-NOW. Normally every communicating peer must share the active channel; peer `channel=0` uses the current channel, **not** channel discovery. Select the enabled STA/AP interface correctly. [ESP-NOW][now]
- Architecture choice: managed fixed AP channel, explicit peer rediscovery after STA roaming/AP channel changes, or a second radio. Test channel changes after association, not only boot-time communication.
- Off-channel tools: `esp_now_switch_channel_tx()` and `esp_now_remain_on_channel()` exist in both [IDF 5.5.5][now55] and [6.1][now61]. They schedule temporary channel visits, not simultaneous reception. Check bundled headers, completion events and return codes; measure disruption under active Wi-Fi/BLE traffic.
- Match callback typedefs to installed headers: these snapshots use `const esp_now_send_info_t *` for send information; old MAC-pointer callback examples need migration. Copy received bytes/metadata before callback return. [Headers][now55]
- Send success acknowledges the MAC layer only: application sequence IDs, acknowledgement, retry/backoff and deduplication handle loss. Queue callback work; pace sends using completion rather than flooding the driver. [ESP-NOW][now]
- Mixed legacy nodes: keep payloads ≤250 bytes unless every receiver supports v2's 1470-byte payload. Explicit PMK/LMK and encrypted unicast protect peers; multicast encryption is unsupported. Read actual target limits/Kconfig instead of assuming a universal encrypted-peer count. [ESP-NOW][now]

## Wi-Fi station and LAN sync in the Arduino trial

Source-checked 2026-09-17 against the historical board-local Wi-Fi implementation (current native replacement: [shared wifi_link.cpp](../../../firmware/common/connectivity/src/wifi_link.cpp)) (protocol v2, [contract](../../shared/ble-sync-protocol.md#lan-transport-v3)). Measured scope (2026-09-17): scan, STA join and rejoin, mDNS, TCP sync from a Mac and an Android phone, session takeover, BLE+Wi-Fi coexistence cost; see [bench-verified](bench-verified.md#board-1-protocol-v2-configuration-wi-fi-lan-sync-phone).

- **Off by default.** The STA radio comes up at runtime only when `wifi.on=1` and an SSID are stored in NVS (`oscfg`, via `SET_CONFIG`); otherwise it stays `WIFI_OFF` and `GET_CONFIG` reports an all-zero MAC. Start sequence: `WiFi.persistent(false)` (NVS holds the credentials, not the driver), `WiFi.mode(WIFI_STA)`, `WiFi.setSleep(true)` (modem sleep, deliberately kept **on** for BLE coexistence), `WiFi.setAutoReconnect(false)`, then `WiFi.begin(ssid, psk)`. [Arduino WiFi API][arduino-wifi], [WiFi library 3.3.11][arduino-wifi-src]
- **Own reconnect state machine** on the `os-lan` task (100 ms tick), not the driver's auto-reconnect: 30 s connect timeout → `failed`, 30 s retry while `wifi.on`; a lost link stops the server and reconnects immediately. Serial/`LOG_TAIL` lines: `WIFI CONNECT ssid=`, `WIFI CONNECTED ssid= ip= rssi=`, `WIFI LOST`, `WIFI FAILED ssid= status= retry_s=`, `WIFI OFF reason=`. `wifi.state` in `CONFIG` mirrors these.
- **mDNS** via ESPmDNS once connected and `lan.on`: `MDNS.begin(os-xxxx)`, `addService("ossync","tcp",47390)`, TXT `proto`, `station`, `dev`, `fw`; torn down with `MDNS.end()` when the link drops. `LAN LISTEN port= host=os-xxxx.local mdns= service=_ossync._tcp` confirms it. [ESPmDNS 3.3.11][espmdns], [ESP-IDF mDNS component][idf-mdns]
- **Scans** (`WIFI_SCAN`) run on the LAN task, never the storage worker; when the radio is off it is brought up as `WIFI_STA` for the scan and returned to `WIFI_OFF` afterwards. `WiFi.scanNetworks()` is blocking (2–4 s per the contract; the bench run found 7 APs but did not time it), then `scanDelete()`; hidden SSIDs skipped, duplicates collapsed to strongest RSSI, ≤ 48 entries.
- **BLE and Wi-Fi run together.** NimBLE keeps advertising/serving while the STA is up. Coexistence is **source-expected only**: the pinned Arduino core's prebuilt config has `CONFIG_ESP_COEX_SW_COEXIST_ENABLE 1` (`esp32s3-libs/3.3.11/qio_qspi/include/sdkconfig.h`, [prebuilt libs][arduino-libs]) and pins the BT controller, NimBLE host **and** the Wi-Fi task all to core 0 (`CONFIG_BT_CTRL_PINNED_TO_CORE 0`, `CONFIG_BT_NIMBLE_PINNED_TO_CORE 0`, `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0 1`), so the core split suggested below is not what the packaged core ships. Its effect on sampling jitter, notification drops and internal heap with STA + mDNS + TCP active is **not measured**; the v4 jitter/drop numbers were taken with Wi-Fi off. Record the first coexistence run in [bench-verified](bench-verified.md#still-unverified-on-hardware) before relying on it.

## BLE coexistence and scheduling

- Start with ESP-IDF NimBLE for BLE-only memory efficiency; Arduino C++ wrapper/version lives in [development](cores3-development.md). Choose one host stack and inspect required features before enabling it. [Stacks][bt]
- Enable/check `CONFIG_ESP_COEX_SW_COEXIST_ENABLE`. Espressif's matrix marks STA+BLE supported; SoftAP clients+BLE and sniffer+BLE have unstable performance; ESP-NOW RX+BLE is supported in STA mode. Verify your exact combination. [Matrix][coex]
- When profiling supports it: keep BT controller and host on one core, Wi-Fi task on the other. Keywords: `CONFIG_BT_CTRL_PINNED_TO_CORE_CHOICE`, `CONFIG_BT_NIMBLE_PINNED_TO_CORE_CHOICE`, `CONFIG_ESP_WIFI_TASK_CORE_ID`. This changes CPU contention, not RF airtime. Avoid blindly pinning application tasks onto those cores. [Coexistence options][coex]
- Tune BLE scan window/interval, active/passive scan, connection interval, PHY, MTU and data length together; test discovery latency and packet loss during Wi-Fi reconnects. Continuous scanning and maximum throughput compete for airtime. [Coexistence][coex], [NimBLE options][nimble-config]
- **Keep the BLE address public and stable.** The phone's Companion Device Manager presence watcher and per-device scan filters match on the advertised address; NimBLE's default (the factory MAC, also the source of `device_id`) is exactly right. Do not enable NimBLE privacy / RPA (`CONFIG_BT_NIMBLE_…PRIVACY`, `setOwnAddrType(BLE_OWN_ADDR_RANDOM…)`) on this device. Since firmware v6.3 the ADV PDU is full (flags + 10-byte service data) and the name/UUID list live in the scan response — see the [advertising payload](../../shared/ble-sync-protocol.md#advertising-payload-v21) and [background-sync-triggers](../../shared/background-sync-triggers.md).
- Multiple BLE connections require host **and** controller capacity: `CONFIG_BT_NIMBLE_MAX_CONNECTIONS`, S3 `CONFIG_BT_CTRL_BLE_MAX_ACT`; budget advertising/scanning activities, buffers and connection events too. [NimBLE Kconfig][nimble-config]

## Connection lifecycle and power

- Give one application task ownership of network state. Event handlers/ESP-NOW callbacks enqueue bounded work; filesystem writes, sensor waits, JSON and TLS belong in workers. Keep queue overflow counters. [Station events][station], [ESP-NOW][now]
- Gate IPv4 sockets/MQTT/HTTPS on `IP_EVENT_STA_GOT_IP`, not merely association. On disconnect/IP change, invalidate/recreate affected sockets. Reconnect using bounded backoff with jitter; distinguish intentional disconnect from failed authentication. [Lifecycle][station]
- Serialize scans with connection attempts. Reconnect loops can starve scans; limit retry bursts. Retrieve/free scan records once, and shorten/channel-limit scans only after measuring missed APs. [Scan lifecycle][station]
- `WIFI_PS_NONE` reduces receive latency and increases power; it does **not** disable coexistence time slicing. Compare against `WIFI_PS_MIN_MODEM` with your AP's DTIM, BLE load and battery budget. SoftAP does not buffer multicast for sleeping clients. [Power save][power]
- Set country/channel policy through supported Wi-Fi APIs for deployment; channel availability derives from that policy and AP information. Do not treat a scan's numeric channel range as universal permission. [Country configuration][wifi-overview]

## On-demand investigation

| Trigger | Keywords / evidence to gather |
|---|---|
| Throughput or stalls | [`wifi/iperf`, Wi-Fi buffer sizing, AMPDU, lwIP windows][power]; compare radio-only baseline with display/camera/SD workloads; RSSI, channel, latency percentiles, loss, internal heap minimum, queue drops. |
| Field reliability | Test AP reboot/channel move, wrong credentials, DHCP/DNS loss, TLS reconnect, simultaneous BLE scans and Wi-Fi upload, queue saturation, sleep/wake and long soak. Log disconnect reason and SDK/config identity. |
| WPA/TLS | [WPA3-SAE, PMF, WPA2-Enterprise][security]; [ESP-TLS certificate bundle, hostname verification, time validity][tls]. Keep credentials out of source/logs. |
| OTA | [HTTPS OTA, partial download/resumption, signed images][ota]; `ota_0/ota_1`, rollback, post-boot health confirmation; power-cut and invalid-certificate/image tests. |
| Provisioning / mesh / sensing | SDK keywords only: `wifi_provisioning`, `protocomm`, `wifi/roaming`, `esp_netif`, MQTT outbox, BLE Mesh, ESP-WIFI-MESH, CSI. Confirm S3 support and dependency versions before adopting. |

[arduino-wifi]: https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html
[arduino-wifi-src]: https://github.com/espressif/arduino-esp32/tree/3.3.11/libraries/WiFi/src
[espmdns]: https://github.com/espressif/arduino-esp32/tree/3.3.11/libraries/ESPmDNS
[idf-mdns]: https://docs.espressif.com/projects/esp-protocols/mdns/docs/latest/en/index.html
[arduino-libs]: https://github.com/espressif/esp32-arduino-libs
[soc]: https://www.espressif.com/en/products/socs/esp32-s3
[bt-architecture]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/bt-architecture/overview.html
[bt]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/bluetooth/index.html
[wifi-overview]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/overview.html
[coex]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/coexist.html
[now]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/network/esp_now.html
[now55]: https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_wifi/include/esp_now.h
[now61]: https://github.com/espressif/esp-idf/blob/v6.1/components/esp_wifi/include/esp_now.h
[nimble-config]: https://github.com/espressif/esp-idf/blob/v6.1/components/bt/host/nimble/Kconfig.in
[station]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/station-scenarios.html
[power]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/wifi-performance-and-power-save.html
[security]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-security.html
[tls]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/protocols/esp_tls.html
[ota]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/esp_https_ota.html
