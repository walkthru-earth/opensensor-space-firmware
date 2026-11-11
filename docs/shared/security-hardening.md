# Firmware security and deployment hardening

This boundary applies to both native ESP-IDF 6.1 logger adapters. The retained
Waveshare diagnostic starts no BLE/LAN/file-sync service. Native source/build
and host tests establish the controls below; physical native pairing, flash/NVS
retention, radio and storage behavior still require qualification. Historical
Arduino bench results remain tied to their recorded board/image identities.

## Implemented

- BLE uses LE Secure Connections and bonding. Display-equipped CoreS3 devices show a fresh random passkey per pairing attempt. Display-less devices use a random per-device six-digit fixed PIN stored in NVS; operators can replace it with `ble.pin`, and legacy installations that still contain the old universal `123456` value rotate it at boot.
- BLE configuration does not return the fixed PIN or Wi-Fi PSK. LAN bearer tokens are 32 random bytes, returned only over an authenticated BLE link, can be rotated, and are never included in CONFIG JSON or normal logs.
- Wi-Fi is off until enabled/provisioned; LAN runs only while connected and `lan.on` is enabled. Driver credentials use RAM storage and `oscfg` alone persists them. Constant-time token comparison and a bounded three-client pool preserve independent sessions; rejected/full-pool challengers do not evict authenticated peers.
- Finalized files are structurally checked and immutable. Interrupted `.partial` files are moved under `/output/quarantine/` at startup or right after a same-boot failure, never silently repaired, published, or deleted. Bounded status counters expose total quarantine files and bytes across boots without exposing filenames.

## Development versus production

The current image is a development/field-trial firmware, not a hardened production image.

- LAN sync is authenticated but plaintext TCP. A bearer token does not provide confidentiality, server identity, forward secrecy, or protection against a LAN sniffer. Use it only on a trusted WPA2/WPA3 network, disable `lan.on` when unnecessary, rotate the token after suspected exposure, and use BLE for provisioning. TLS is not claimed: certificate/trust provisioning and ESP32-S3 memory, latency, and coexistence effects must be designed and measured first.
- Wi-Fi PSKs, BLE PINs, and LAN tokens are stored in NVS. This board currently has no verified flash encryption, so physical flash access can expose them. Native diagnostics log pairing mode only; random digits are displayed through the pairing UI and the fixed PIN is available only through the explicit physical owner command. Production deployments must restrict physical/serial access.
- `ble.pair=none` is encrypted Just Works without authenticated MITM protection and is for controlled development benches only.
- Six-digit BLE passkeys have limited entropy. Per-device generation removes a universal credential but does not make an unattended, physically exposed device resistant to determined nearby guessing or physical extraction.

## Serial console

The physical serial console needs no credential. Anyone who can reach the
board port can run the documented owner commands in
[OSLogger](../../firmware/common/logger/README.md) and the
[UART owner provisioning](ble-sync-protocol.md#physical-uart-owner-provisioning)
section.

- `parquet wifi-profile` exports the Wi-Fi SSID and password.
- `parquet owner-pin` reads out the fixed owner PIN.
- `parquet config-hex` changes configuration through the SET_CONFIG handler.
- `parquet time` sets the clock.
- `parquet list` and `parquet get` copy finalized files.
- `parquet erase-archive CONFIRM` erases the archive.

Treat physical access to the port as owner access.

## No factory reset

No factory reset exists. A device that is resold or handed on keeps all of
the following until someone removes each one.

- The Wi-Fi password, the LAN token and the BLE PIN in the `oscfg` `cfg` record
- The BLE bonds
- The location cell and country
- The owner text (`owner.title`, `owner.description`, `owner.provider`,
  `owner.url`) and `thing.deployment_id`
- The Thing id in NVS `parquet/thing`
- Any core dump in the `coredump` partition
- The SD archive

Partial tools exist. `ble.clear_bonds` removes the bonds, `lan.rotate_token`
replaces the LAN token, and SET_CONFIG can replace the PIN and clear or replace
the Wi-Fi, location and owner values one key at a time. `parquet erase-archive CONFIRM`
removes the archive and `pixi run coredump-read <board> <port> --erase` clears
the core dump. None of them resets the Thing id, and none of them is one
step. Clearing or replacing a config value does not remove the old bytes from
flash either. The firmware opens NVS in the standard read and write mode, so
NVS only marks the old entry as erased and the bytes stay on the chip until
NVS erases that page to reclaim space, see [#170](https://github.com/walkthru-earth/opensensor-space-firmware/issues/170).

## Not implemented or verified

This repository does **not** claim secure boot, flash encryption, signed OTA, anti-rollback, protected key storage, remote attestation, sensor calibration, SD-card encryption, or power-loss endurance. The logger has an authenticated LAN OTA (#40), but images are not signed and rollback is off, see [OTA](#ota). Do not burn eFuses as part of normal development; irreversible provisioning needs a separate production design, recovery plan, and hardware verification.

The owner can already sign app images with a local key that never leaves the owner machine, see [local app signing](development.md#local-app-signing). Devices do not check those signatures until #40 enables it with an owner-approved bootloader reflash.

## OTA

The logger takes a new app image over an authenticated LAN session (#40). The
wire rules are in [LAN OTA](ble-sync-protocol.md#lan-ota). This section covers
what that does and does not protect.

- The LAN bearer token is the only authentication. Anyone with the token can
  stage an image. The device checks the chip, the project name, the ESP-IDF
  image check and a SHA-256 that the sender declares. None of these prove who
  built the image. Treat the token like a flashing cable.
- Images are not signed, and the device does not check signatures. Signed apps
  (`CONFIG_SECURE_SIGNED_APPS_*`) or Secure Boot change the bootloader, so they
  wait on an owner approved bootloader reflash.
- Rollback is off. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` changes the
  bootloader too, so it waits on the same reflash. Without it the bootloader
  boots a staged image even when it crashes, and a crash loop needs USB to
  recover. In ESP-IDF 6.1 the rollback transitions exist only under that option
  (`bootloader_utility.c` lines 393 to 449).
- The boot self test still marks a healthy app valid. This is safe with
  rollback off. In ESP-IDF 6.1 `esp_ota_set_boot_partition` writes the new
  otadata entry as `ESP_OTA_IMG_UNDEFINED` (`esp_ota_ops.c` lines 127 to 135),
  and `esp_ota_mark_app_valid_cancel_rollback` only rewrites the running
  slot's state to `ESP_OTA_IMG_VALID` (lines 1179 to 1222). After a USB flash
  otadata is blank, `esp_ota_get_state_partition` returns `ESP_ERR_NOT_FOUND`
  (lines 1282 to 1312), and the self test skips the mark. Once rollback is on
  the same mark keeps a healthy image.
- `esp_ota_begin` refuses the running slot (lines 155 to 230), so an upload
  never overwrites the app that is running. NVS, otadata outside the boot
  pointer and the SD archive are never written.
- The Waveshare logger and diagnostic images share the project name
  `opensensor_waveshare`. A diagnostic image pushed to a logger is accepted.
  The diagnostic has no LAN, so that board needs USB to get back.
- There is no LAN or BLE core dump endpoint. A core dump holds task stacks,
  which can hold the Wi-Fi PSK, the LAN token or the device PIN. On a board
  running the GNSS reader it can also hold the raw receiver reply line with
  coordinates, because the reader keeps that line on its task stack (#172).
  `pixi run coredump-read` reads it over USB serial only and prints the backtraces
  without memory. esp_coredump writes the flash read to a temporary file and
  leaves it behind when decoding fails, so the tool points TMPDIR at a private
  0700 folder and removes it on every path. The partition stays on the board
  unless the owner passes `--erase`, and the erase runs only after a good
  read.

Before production, define a manufacturing/provisioning ceremony, unique device ownership transfer and reset behavior, debug-port policy, TLS trust lifecycle, update signing/rollback, secret rotation/revocation, and destructive power-cut/full-card/removal tests on the exact board and card SKUs.
