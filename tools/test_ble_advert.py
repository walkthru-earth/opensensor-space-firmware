"""Decode native firmware advertising bytes through the shipped BLE client.

Host interoperability checks; no hardware/radio behavior is established.
Run: pixi run python tools/test_ble_advert.py --sanitize
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile
from types import SimpleNamespace
from uuid import UUID

import ble_sync as client
from host_cxx import CXX_STD


def ad_fields(packet: bytes) -> dict[int, bytes]:
    """Independent Bluetooth AD parser: length includes type, excludes itself."""
    fields = {}
    position = 0
    while position < len(packet):
        length = packet[position]
        assert length and position + length < len(packet)
        kind = packet[position + 1]
        assert kind not in fields
        fields[kind] = packet[position + 2:position + length + 1]
        position += length + 1
    assert position == len(packet)
    return fields


# info shares one 400 byte buffer between the BLE info characteristic and the
# LAN HELLO frame (ble::kMaxInfo, NUL included). Keep this much room for later
# keys or longer firmware tags.
INFO_CAP = 400
INFO_MARGIN = 48
BOARDS = {
    "cores3": "firmware/esp-idf-cores3/bringup",
    "waveshare": "firmware/esp-idf-waveshare-sim7670g/logger",
}


def board_constant(root: Path, board: str, header: str, name: str) -> str:
    text = (root / BOARDS[board] / header).read_text()
    found = re.findall(rf'\b{name} =\s*"([^"]+)"', text)
    assert len(found) == 1, (board, name)
    return found[0]


def check_info(root: Path, executable: Path) -> list[str]:
    """Longest realistic info on both boards stays under the shared cap."""
    report = []
    for board in BOARDS:
        schema = board_constant(root, board, "telemetry_contract.h", "kSchemaName")
        firmware = board_constant(root, board, "telemetry_contract.h", "kFirmware")
        desc = board_constant(root, board, "telemetry_descriptor_digest.h",
                              "kDescriptorSha256")
        assert re.fullmatch(r"[0-9a-f]{64}", desc)
        text = subprocess.check_output([str(executable), schema, firmware, desc],
                                       text=True).rstrip("\n")
        info = json.loads(text)
        assert info["desc"] == desc[:16] and info["fw"] == firmware
        assert info["schema"] == schema and info["proto"] == 3
        assert list(info)[:6] == ["proto", "fw", "schema", "cols", "dict", "desc"]
        size = len(text.encode())
        # The NUL rides in the same buffer, so size + 1 must fit the cap.
        assert size + 1 + INFO_MARGIN <= INFO_CAP, (board, size)
        report.append(f"{board} {size} B")
    return report


def wire_uuid(value: bytes) -> str:
    assert len(value) == 16
    return str(UUID(bytes=bytes(reversed(value))))


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    return text[start:text.index("\n}\n", start)]


def check_setup_wiring(root: Path) -> None:
    """The logger sets bit 6 from config::setup_needed() and republishes the
    advert right after every handled SET_CONFIG (#147). The bit encoding and
    the derived state have their own tests, this ties them together."""
    logger = (root / "firmware/common/logger/src/os_logger.cpp").read_text()
    advert = function_body(logger, "void publish_advert() {")
    assert re.search(
        r"if \(::config::setup_needed\(::config::location_settings\(\)\)\)\s*"
        r"advert\.flags \|= ble::kAdvSetup;", advert), \
        "publish_advert sets kAdvSetup from setup_needed()"
    assert advert.index("kAdvSetup") < advert.index("ble::publish_advert(advert)"), \
        "the setup bit is set before the advert is published"
    control = function_body(logger, "void handle_control_request(")
    assert re.search(
        r"if \(ossync::handle_common_control\(request, control_replies\)\) \{"
        r"[^{}]*?if \(request\.bytes\[0\] == ble::kOpSetConfig\)\s*publish_advert\(\);",
        control), "a BLE or LAN SET_CONFIG republishes the advert at once"
    worker = function_body(logger, "void storage_worker(void *) {")
    serial = worker[worker.index("apply_config_hex(command.text + 19)"):]
    assert re.match(
        r"apply_config_hex\(command\.text \+ 19\);\s*if \(result\.ok\) \{\s*publish_advert\(\);",
        serial), "a serial config-hex republishes the advert at once"
    links = function_body(logger, "bool start_links(bool display_detected) {")
    assert re.search(
        r"ossync::begin\([^;]*\.ble_started;[^{}]*?if \(started\)\s*publish_advert\(\);"
        r"\s*return started;", links), \
        "start_links publishes the flag baseline right after BLE starts"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    check_setup_wiring(root)
    with tempfile.TemporaryDirectory(prefix="aq-ble-advert-") as directory:
        executable = Path(directory) / "advert"
        command = [
            "clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
            "-I", str(root / "firmware/common/runtime/src"),
            "-I", str(root / "firmware/common/connectivity/src"),
            str(root / "firmware/common/connectivity/tests/test_ble_advert.cpp"),
            "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        info_sizes = check_info(root, executable)
        packets = [bytes.fromhex(line) for line in subprocess.check_output(
            [str(executable)], text=True).splitlines()]
    assert len(packets) == 17
    expected = [client.SERVICE, client.INFO, client.STATUS, client.LIVE,
                client.CONTROL, client.RESPONSE]
    assert [wire_uuid(value) for value in packets[:6]] == expected
    for index, packet in enumerate(packets[6:15]):
        assert len(packet) == 31
        fields = ad_fields(packet)
        assert set(fields) == {0x01, 0x21}
        assert fields[0x01] == b"\x06"
        service = wire_uuid(fields[0x21][:16])
        assert service == client.SERVICE
        payload = fields[0x21][16:]
        assert len(payload) == 10 and payload[-2:] == b"\0\0"
        decoded = client.decode_advert(payload)
        assert decoded is not None
        if index < 7:
            assert decoded == {"ver": 1, "flags": [client.ADVERT_FLAGS[index]],
                               "fin": 0x89abcdef, "boot16": "7654"}
        else:
            assert decoded == {"ver": 1, "flags": [],
                               "fin": 0 if index == 7 else 0xffffffff,
                               "boot16": "ffff"}
        # A platform receiving ADV without SCAN_RSP can still discover the board.
        assert client.advertises_sync_service(SimpleNamespace(
            service_uuids=[], service_data={service: payload}))
    for packet, name in zip(packets[15:], ["OS-6b40", "12345678901"]):
        assert len(packet) <= 31
        fields = ad_fields(packet)
        assert set(fields) == {0x09, 0x07}
        assert fields[0x09].decode() == name
        assert wire_uuid(fields[0x07]) == client.SERVICE
        assert client.advertises_sync_service(SimpleNamespace(
            service_uuids=[wire_uuid(fields[0x07])], service_data={}))
    print("Native BLE UUID/ADV/SCAN_RSP interoperates with the BLE client; bounds passed")
    print(f"info with desc under {INFO_CAP} B with {INFO_MARGIN} B margin: "
          + ", ".join(info_sizes))


if __name__ == "__main__":
    main()
