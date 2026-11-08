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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
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
