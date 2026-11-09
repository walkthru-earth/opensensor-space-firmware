# Third-party notices

This repository and the firmware images built from it contain work by
[Walkthru.Earth](https://walkthru.earth) and work by third parties.

The [CC BY 4.0 license](LICENSE) covers only original Walkthru.Earth work. It
does not relicense any third-party component listed here. Each component keeps
its own license, copyright headers and NOTICE files, and those terms apply when
you copy the source or ship a binary that contains it.

If you distribute a firmware image (`.bin`, `.elf` or a merged flash image) built
from this repository, ship this file together with the license texts referenced
below, and keep the visible Walkthru.Earth credit described in the
[README](README.md#license-and-attribution).

The firmware inventory below was taken from the ESP32-S3 linker maps of the
`esp-idf-cores3` and `esp-idf-waveshare-sim7670g` builds against the pinned
ESP-IDF in [`tools/idf-dependencies.lock`](tools/idf-dependencies.lock)
(ESP-IDF v6.1, commit `fff9895c82d744c7237be8847347bdd1b07c6643`), and each
license was checked against the license files, SPDX headers and `sbom.yml`
manifests in that source tree. Only ESP32-S3 is hardware verified.

## 1. Third-party code vendored in this repository

| Component | Path | Version | License | Copyright |
| :--- | :--- | :--- | :--- | :--- |
| LZ4 library | [`firmware/common/vendor/lz4`](firmware/common/vendor/lz4) | 1.11.0 | BSD-2-Clause, see [`LICENSE`](firmware/common/vendor/lz4/LICENSE) | Copyright (c) Yann Collet (source headers), Copyright (c) 2011-2020, Yann Collet (`LICENSE`) |
| Uber H3 | [`firmware/common/location/vendor/h3`](firmware/common/location/vendor/h3) | 4.5.0 | Apache-2.0, see [`LICENSE`](firmware/common/location/vendor/h3/LICENSE) and [`NOTICE`](firmware/common/location/vendor/h3/NOTICE) | Copyright 2017-2021 Uber Technologies, Inc. |
| esp-nimble host patch | [`firmware/common/vendor/esp-nimble`](firmware/common/vendor/esp-nimble) | backport of esp-nimble `d7061976e9` and `79316193f9` onto `139cada` | Apache-2.0, see [`LICENSE`](firmware/common/vendor/esp-nimble/LICENSE) and [`NOTICE`](firmware/common/vendor/esp-nimble/NOTICE) | Copyright 2015-2022 The Apache Software Foundation, modifications Copyright 2017-2022 Espressif Systems (Shanghai) CO., LTD. |

Notes

- Only the LZ4 `lib/` sources (`lz4.c`, `lz4.h`) are vendored, and those are
  BSD-2-Clause. Other parts of the upstream LZ4 repository, such as the
  command-line programs, use different licenses and are not included here. See
  [`firmware/common/vendor/lz4/README.md`](firmware/common/vendor/lz4/README.md)
  for the pinned source and hashes.
- The H3 sources are unmodified upstream files verified against
  [`manifest.json`](firmware/common/location/vendor/h3/manifest.json). The
  Apache-2.0 `NOTICE` file must accompany any binary that contains H3 code.
- The esp-nimble patch changes one NimBLE host file, `ble_hs_hci.c`, at
  build time. The build compiles a patched copy in place of the SDK file, and
  the patched code stays Apache-2.0. See
  [`firmware/common/vendor/esp-nimble/README.md`](firmware/common/vendor/esp-nimble/README.md)
  for the pinned hashes and the upstream commits.

## 2. Algorithms reused without copied code

| Item | Where | Terms |
| :--- | :--- | :--- |
| Howard Hinnant `days_from_civil` date algorithm | [`firmware/common/src/utc_clock.h`](firmware/common/src/utc_clock.h) | Public domain, per the author ("Consider these donated to the public domain", [chrono-Compatible Low-Level Date Algorithms](https://howardhinnant.github.io/date_algorithms.html)) |

## 3. ESP-IDF v6.1 framework linked into the firmware

ESP-IDF is Copyright Espressif Systems (Shanghai) CO LTD and is licensed under
Apache-2.0 unless a component states otherwise. Espressif components used by the
firmware (drivers, HAL, `esp_system`, `esp_wifi`, `esp_netif`, `nvs_flash`,
`spi_flash`, `vfs`, `esp_pm`, `esp_psram`, `sdmmc`, `bt`, the bootloader and
others) are Apache-2.0. The upstream projects bundled inside ESP-IDF that end up
in the linked image are listed below.

| Component | ESP-IDF path | Version | License | Copyright |
| :--- | :--- | :--- | :--- | :--- |
| ESP-IDF | `/` | 6.1 | Apache-2.0 | Copyright Espressif Systems (Shanghai) CO LTD |
| Apache Mynewt NimBLE (BLE host) | `components/bt/host/nimble/nimble` | 1.6.0 | Apache-2.0, with `NOTICE` | Copyright 2015-2022 The Apache Software Foundation, modifications Copyright 2017-2022 Espressif Systems (Shanghai) CO., LTD. |
| TinyCrypt (used by NimBLE security manager) | `components/bt/common/tinycrypt` | Espressif fork | BSD-3-Clause, ECC files BSD-2-Clause | Copyright (c) 2017 Intel Corporation, ECC code Copyright (c) 2014 Kenneth MacKay, Espressif changes Apache-2.0 |
| lwIP TCP/IP stack | `components/lwip/lwip` | 2.2.0 | BSD-3-Clause | Copyright (c) 2001, 2002 Swedish Institute of Computer Science and lwIP contributors |
| Mbed TLS and TF-PSA-Crypto | `components/mbedtls/mbedtls` | 4.1.0 | Apache-2.0 OR GPL-2.0-or-later, used here under Apache-2.0 | Copyright The Mbed TLS Contributors |
| FreeRTOS kernel | `components/freertos/FreeRTOS-Kernel` | 10.5.1 | MIT, Espressif SMP additions Apache-2.0 | Copyright (C) 2021 Amazon.com, Inc. or its affiliates |
| FatFs | `components/fatfs/src` | R0.16 | FatFs license (1-clause BSD style) | Copyright (C) 2025 ChaN |
| wpa_supplicant | `components/wpa_supplicant` | 2.10 | BSD-3-Clause | Copyright (c) 2002-2022 Jouni Malinen and contributors |
| TLSF allocator | `components/heap/tlsf` | Espressif fork | BSD-3-Clause | Copyright (c) 2006-2016 Matthew Conte |
| Wi-Fi, PHY, coexistence and BLE controller libraries (binary) | `components/esp_wifi/lib`, `components/esp_phy/lib`, `components/esp_coex/lib`, `components/bt/controller/lib_esp32c3_family` | ESP-IDF 6.1 | Apache-2.0 | Copyright Espressif Systems (Shanghai) CO LTD |

## 4. Toolchain runtime libraries linked into the firmware

ESP-IDF v6.1 builds this firmware with picolibc (`CONFIG_LIBC_PICOLIBC=y`), not
newlib. The libraries below come from the Espressif `xtensa-esp-elf` GCC 15.2.0
toolchain. Their license texts ship with the toolchain under
`xtensa-esp-elf/share/licenses/`.

| Component | Version | License | Copyright |
| :--- | :--- | :--- | :--- |
| picolibc C library | 1.8.10 | BSD-3-Clause and other permissive licenses inherited from newlib, see `share/licenses/picolibc/COPYING.picolibc` | Copyright 2019 Keith Packard, The Newlib Project and others |
| GCC runtime (`libgcc`) and C++ library (`libstdc++`) | GCC 15.2.0 | GPL-3.0-or-later WITH GCC-exception-3.1 | Copyright Free Software Foundation, Inc. |

The GCC Runtime Library Exception allows these libraries to be linked into
firmware under any license.

## 5. Managed components from the ESP Component Registry

Versions are pinned in each board's `dependencies.lock` and component
`idf_component.yml` and track the latest stable releases. They describe builds
from current source. Older images may contain earlier versions of the same
components, which carry the same licenses and copyright notices.

| Component | Version | Used by | License | Copyright |
| :--- | :--- | :--- | :--- | :--- |
| `espressif/mdns` | 1.15.1 | CoreS3, Waveshare SIM7670G | Apache-2.0 | Copyright Espressif Systems (Shanghai) CO LTD |
| `espressif/led_strip` | 3.1.1 | Waveshare SIM7670G | Apache-2.0 | Copyright Espressif Systems (Shanghai) CO LTD |
| `m5stack/m5unified` | 0.3.0 | CoreS3 | MIT | Copyright (c) 2021 M5Stack |
| `m5stack/m5gfx` | 0.3.0 | CoreS3 | MIT, with bundled third-party parts listed below | Copyright (c) 2021 M5Stack |

M5GFX bundles code and fonts under their own licenses, as listed in its README.
LovyanGFX (BSD-2-Clause FreeBSD, lovyan03), TJpgDec (ChaN), Pngle (MIT,
kikuchan), QRCode (MIT, Richard Moore and Nayuki), result (MIT, Matthew
Rodusek), Adafruit GFX and GLCD fonts (BSD-2-Clause, Adafruit Industries),
TFT_eSPI fonts 2, 4, 6, 7 and 8 (BSD-2-Clause FreeBSD, Bodmer), converted IPA
fonts (IPA Font License), efont (BSD-3-Clause, The Electronic Font Open
Laboratory) and the TomThumb font (BSD-3-Clause). Only the parts the CoreS3
firmware references are linked, but the M5GFX license files should travel with
any CoreS3 image.

## 6. Host tools, not shipped in firmware

The Python and C++ host tools under [`tools/`](tools) run on a developer machine
and are not part of any firmware image. Their dependencies, such as Python,
pyarrow, DuckDB, bleak, pyserial, esptool, CMake, Ninja, clang-tools and
cppcheck, are installed by [Pixi](pixi.toml) from conda-forge and PyPI under
their own licenses and are not redistributed by this repository.
`pixi.toml` holds the allowed version ranges and `pixi.lock` holds the exact
resolved versions. As of 2026-11-09 the workspace requires Pixi 0.84.1 and the
lock resolves Python 3.15.2, pyarrow 26.0.1, DuckDB 2.0.0, bleak 3.1.0,
pyserial 3.5, esptool 5.6.0, CMake 4.4.5, Ninja 1.13.2, ccache 4.14.3,
clang-tools, clangxx and compiler-rt 23.1.4, cppcheck 2.22.0, jsonschema
4.26.0, rfc3986-validator 0.1.1, PyYAML 6.0.3, pre-commit 4.7.0, ripgrep
15.2.0, GNU coreutils 9.12, Git 2.56.0, Rust 1.99.1 and pip 26.2.2. esptool
5.6.0 no longer pulls in bitstring, and pyarrow 26 now pulls in NumPy.

The JSON Schemas in [`tools/stac_opensensor/v0.1.0`](tools/stac_opensensor/v0.1.0)
(`template.json`, `profile.json`, `schema.json`, `footer.json`), the profile
text [`tools/stac_opensensor/PROFILE.md`](tools/stac_opensensor/PROFILE.md)
and the example [`tools/stac_opensensor/footers/m5stack-cores3.json`](tools/stac_opensensor/footers/m5stack-cores3.json)
are copies of
[stac-opensensor](https://github.com/walkthru-earth/stac-opensensor) at commit
`83787768cc8451109d48a03d4ef5493ffcd403dd`. The schemas and the example are
unmodified. In `PROFILE.md` only the relative links that do not resolve in
this tree are rewritten as absolute links at that commit. Copyright (c) 2025-2026 Walkthru.Earth, licensed
[CC-BY-4.0](https://creativecommons.org/licenses/by/4.0/). The host contract
tests use them to validate the board templates and the `opensensor` Parquet
footer each board writes. They are not in any firmware image.

The GeoParquet metadata schema
[`tools/geoparquet/v2.0.0/schema.json`](tools/geoparquet/v2.0.0/schema.json)
is an unmodified copy of `format-specs/schema.json` from
[opengeospatial/geoparquet](https://github.com/opengeospatial/geoparquet) at
commit `1edd56fb020a188fcb35fd8babe89c41e241288e` (git blob `99b8312`),
Copyright the GeoParquet contributors, licensed
[Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0). The host tests use
it to validate the `geo` Parquet footer each board writes. It is not in any
firmware image.

The four SigV4 test vectors in [`tools/test_upload.cpp`](tools/test_upload.cpp)
(their canonical requests, strings to sign and signatures) are test data from
the AWS signing test suite `tests/aws-signing-test-suite/v4` in
[awslabs/aws-c-auth](https://github.com/awslabs/aws-c-auth), licensed
[Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0). Its NOTICE reads
"AWS C Auth", "Copyright Amazon.com, Inc. or its affiliates. All Rights
Reserved." and "SPDX-License-Identifier: Apache-2.0.". They use the public
`AKIDEXAMPLE` example credentials, which sign nothing real. The host upload
test uses them to check the signer. They are not in any firmware image.
