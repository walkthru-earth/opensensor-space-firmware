"""Frame-level tests for tools/ble_sync.py against docs/shared/ble-sync-protocol.md v3.

Run: pixi run python tools/test_ble_sync.py
A fake client answers control writes with scripted response frames; nothing
touches a radio. This proves the host parser's reading of the contract, not
the firmware.
"""
import asyncio
import contextlib
import hashlib
import io
import json
import binascii
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import AsyncMock, patch

import ble_sync as ble

NAME = "thing=abc/unsynced/boot=def/data_unsynced_def_0-1-0.parquet"
FILE = b"PAR1" + bytes(range(256)) * 3 + struct.pack("<I", 40) + b"PAR1"
CONFIG = (b'{"ble":{"pair":"random","pin_set":1,"pin_default":0,"bonds":1,"display":1},'
          b'"wifi":{"on":0,"ssid":"","psk_set":0,"state":"off","ip":"","rssi":0,"mac":"aa"},'
          b'"lan":{"on":1,"port":47390,"host":"os-6b40","clients":0}}')
TOKEN = bytes(range(32))
LOG = b"PARQUET BEGIN x\n" + b"BLE CONNECT peer=y\n" * 20
INFO = b'{"proto":3,"max_read":16384,"thing":"abc","dev":"0123456789ab"}'
STATUS = b'{"part":3,"qf":7,"qb":123456,"open":0,"open_rg":0}'
# Protocol 3 goldens, byte for byte from docs/shared/ble-sync-protocol.md.
GOLDEN_LIST_END = bytes.fromhex("117011010001000066ed0000040000")
GOLDEN_LIST_END_V2 = bytes.fromhex("11" + "7011" + "0100" + "0066ed00" + "00040000")  # 13-byte u16 count
GOLDEN_EVENT = bytes.fromhex("78000000070000000302ffff00000200")
GOLDEN_DIAG = bytes.fromhex("4c0200" + "0000" + "0500" + "0000" + "0100" + "0000" * 13 + "01") + GOLDEN_EVENT
GOLDEN_BYE = bytes.fromhex("4b0104030201f5ff0300100e0000")
# GET_DESCRIPTOR offset 512 length 1024, and the header of the 0x4f chunk
# that answers it on a 1300 byte template, byte for byte from the protocol doc.
GOLDEN_GET_DESCRIPTOR = bytes.fromhex("11" "00020000" "0004")
GOLDEN_DESCRIPTOR_CHUNK_HEADER = bytes.fromhex("4f" "14050000" "00020000")
DESCRIPTOR = b'{"type":"Feature","id":"template","x":"' + b"a" * 1259 + b'"}'
GOLDEN_ABORTED_IDLE = bytes.fromhex("4d00")
GOLDEN_ABORTED_READ = bytes.fromhex("4d03")
# LIST_SINCE request and LIST_SINCE_END, byte for byte from the protocol doc.
GOLDEN_LIST_SINCE = bytes.fromhex("10d4c3b2a1db25350100112233445566778899aabbccddeeff0700000000000000")
GOLDEN_LIST_SINCE_END = bytes.fromhex("4e020000000000e80300000a00000002d4c3b2a1dc253501"
                                      "00112233445566778899aabbccddeeff2a00000000000000")


# GET_CONFIG page 6, the owner description, with two byte UTF-8 characters.
PAGE6 = ('{"owner":{"description":"Air near the Nile, '
         '\u0627\u0644\u0642\u0627\u0647\u0631\u0629"}}').encode()


class FakeClient:
    """Scripted device: LIST of two files, one openable file served in chunks."""

    def __init__(self, chunk=100, drop_frame=False, list_end=None, auth_refused=False, att_error=None,
                 list_since=True):
        self.chunk = chunk
        # False answers LIST_SINCE with ERROR 11, as firmware without it does.
        self.list_since = list_since
        self.since_requests = []
        # Raise this from every Control write, as a backend does when NimBLE
        # rejects an unpaired write at the ATT layer. The normal BLE case.
        self.att_error = att_error
        # Answer every Control write with ERROR 14. Over LAN a bad token, over
        # BLE only the firmware fallback if its check and ATT ever diverge.
        self.auth_refused = auth_refused
        self.writes = 0
        self.list_end = list_end
        self.reading = False
        self.callback = None
        self.handle = 0
        self.drop_frame = drop_frame
        self.mtu_size = 517
        # GET_DESCRIPTOR: served bytes (None answers ERROR 17), the frame
        # budget, offsets whose first reply is lost (or every reply when
        # descriptor_drop_always), and a stale chunk sent ahead of the reply.
        self.descriptor = DESCRIPTOR
        self.descriptor_budget = 512
        self.descriptor_drop = set()
        self.descriptor_drop_always = False
        # Templates swapped in, one per request at a nonzero offset, as a
        # reflash between chunks would.
        self.descriptor_next = []
        self.descriptor_stale = False
        self.descriptor_requests = []
        # CONFIG replies as BLE CONFIG_CHUNK frames of this slice size, as the
        # firmware sends them when the JSON does not fit the ATT payload.
        # config_drop names the chunk index never sent, config_skew changes the
        # total from that chunk index on.
        self.config_chunk = None
        self.config_drop = None
        self.config_skew = None

    def emit_config(self, flags: int, text: bytes):
        if self.config_chunk is None:
            self.emit(bytes([ble.F_CONFIG, flags]) + text)
            return
        for index, offset in enumerate(range(0, len(text), self.config_chunk)):
            if index == self.config_drop:
                continue
            total = len(text) + (1 if self.config_skew is not None and index >= self.config_skew else 0)
            self.emit(bytes([ble.F_CONFIG_CHUNK, flags]) + struct.pack("<HH", total, offset)
                      + text[offset:offset + self.config_chunk])

    async def start_notify(self, _uuid, callback):
        self.callback = callback

    def emit(self, frame: bytes):
        self.callback(None, bytearray(frame))

    async def read_gatt_char(self, uuid):
        if uuid == ble.INFO:
            return bytearray(INFO)
        return bytearray(STATUS if uuid == ble.STATUS else b'{"buf":1}')

    async def write_gatt_char(self, _uuid, data, response=True):
        op = data[0]
        self.writes += 1
        if self.att_error is not None:
            raise self.att_error
        if self.auth_refused:
            self.emit(bytes([ble.F_ERROR, op, 14]) + b"auth")
            return
        if op == ble.OP_LIST_SINCE:
            self.since_requests.append(bytes(data))
            if not self.list_since:
                self.emit(bytes([ble.F_ERROR, op, 11]))
                return
            self.emit(bytes([ble.F_FILE]) + struct.pack("<I", len(FILE)) + NAME.encode())
            self.emit(bytes([ble.F_FILE]) + struct.pack("<I", 7) + b"legacy-parquet/x.parquet")
            self.emit(GOLDEN_LIST_SINCE_END)
        elif op == ble.OP_LIST:
            self.emit(bytes([ble.F_FILE]) + struct.pack("<I", len(FILE)) + NAME.encode())
            self.emit(bytes([ble.F_FILE]) + struct.pack("<I", 7) + b"legacy-parquet/x.parquet")
            self.emit(self.list_end or bytes([ble.F_LIST_END]) + struct.pack("<IHII", 2, 0, 1000, 10))
        elif op == ble.OP_OPEN:
            name = data[1:].decode()
            if name != NAME:
                self.emit(bytes([ble.F_ERROR, op, 3]) + b"not finalized")
                return
            self.handle += 1
            crc = binascii.crc32(FILE) & 0xFFFFFFFF
            self.emit(bytes([ble.F_OPENED]) + struct.pack("<HII", self.handle, len(FILE), crc) + name.encode())
        elif op == ble.OP_READ:
            handle, offset, length = struct.unpack_from("<HII", data, 1)
            end = min(len(FILE), offset + length)
            first = True
            for position in range(offset, end, self.chunk):
                if self.drop_frame and first:
                    first = False
                    continue  # simulate a lost notification: parser must reject
                self.emit(bytes([ble.F_CHUNK]) + struct.pack("<HI", handle, position) + FILE[position:min(end, position + self.chunk)])
            self.emit(bytes([ble.F_READ_END]) + struct.pack("<HIB", handle, end, 0))
        elif op == ble.OP_CLOSE:
            (handle,) = struct.unpack_from("<H", data, 1)
            self.emit(bytes([ble.F_CLOSED]) + struct.pack("<H", handle))
        elif op == ble.OP_SET_TIME:
            (epoch,) = struct.unpack_from("<q", data, 1)
            self.emit(bytes([ble.F_TIME_SET]) + struct.pack("<qq", epoch, 123456))
        elif op == ble.OP_FLUSH:
            self.emit(bytes([ble.F_ERROR, op, 10]) + b"empty")
        elif op == ble.OP_STATUS:
            self.emit(bytes([ble.F_STATUS]) + STATUS)
        elif op == ble.OP_GET_CONFIG:
            assert len(data) in (1, 2)
            page = data[1] if len(data) == 2 else 0
            if page == 1:
                self.emit_config(0, b'{"location":{"cell":"","resolution":5,"cell_resolution":null,"lat":null,"lon":null,"country":""}}')
            elif page == 5:
                self.emit_config(0, b'{"owner":{"title":"","provider":"","url":""},"thing":{"deployment_id":"cairo-07"}}')
            elif page == 6:
                self.emit_config(0, PAGE6)
            else:
                self.emit_config(0, CONFIG)
        elif op == ble.OP_SET_CONFIG:
            lines = data[1:].decode().split("\n")
            if any(line.startswith("ble.pin=") and len(line) != len("ble.pin=") + 6 for line in lines):
                self.emit(bytes([ble.F_ERROR, op, 12]) + b"ble.pin")
                return
            self.emit_config(1, CONFIG.replace(b'"random"', b'"fixed"'))
        elif op == ble.OP_WIFI_SCAN:
            self.emit(bytes([ble.F_WIFI_AP]) + struct.pack("<bBB", -58, 3, 6) + b"home")
            self.emit(bytes([ble.F_WIFI_AP]) + struct.pack("<bBB", -80, 0, 11) + b"cafe")
            self.emit(bytes([ble.F_WIFI_SCAN_END]) + struct.pack("<HB", 2, 0))
        elif op == ble.OP_GET_TOKEN:
            self.emit(bytes([ble.F_TOKEN]) + struct.pack("<H", 47390) + TOKEN)
        elif op == ble.OP_LOG_TAIL:
            (max_bytes,) = struct.unpack_from("<H", data, 1)
            text = LOG[-max_bytes:]
            for position in range(0, len(text), 50):
                self.emit(bytes([ble.F_LOG]) + text[position:position + 50])
            self.emit(bytes([ble.F_LOG_END]) + struct.pack("<IH", len(LOG), len(text)))
        elif op == ble.OP_REBOOT:
            self.emit(bytes([ble.F_REBOOTING]) + struct.pack("<H", 500))
        elif op == ble.OP_GET_DIAG:
            assert len(data) == 1, data
            self.emit(GOLDEN_DIAG)
        elif op == ble.OP_GET_DESCRIPTOR:
            # Mirrors firmware encode_descriptor_chunk, error for error.
            self.descriptor_requests.append(bytes(data))
            if self.descriptor_next and len(data) == 7 and struct.unpack_from("<I", data, 1)[0]:
                self.descriptor = self.descriptor_next.pop(0)
            if len(data) != 7:
                self.emit(bytes([ble.F_ERROR, op, 1]) + b"descriptor")
                return
            if self.descriptor is None:
                self.emit(bytes([ble.F_ERROR, op, 17]))
                return
            offset, length = struct.unpack_from("<IH", data, 1)
            total = len(self.descriptor)
            if length == 0 or offset >= total:
                self.emit(bytes([ble.F_ERROR, op, 1 if length == 0 else 6]))
                return
            if offset in self.descriptor_drop:
                if not self.descriptor_drop_always:
                    self.descriptor_drop.discard(offset)
                return
            if self.descriptor_stale and offset:
                self.emit(bytes([ble.F_DESCRIPTOR_CHUNK]) + struct.pack("<II", total, 0) + self.descriptor[:4])
            count = min(length, total - offset, self.descriptor_budget - ble.DESCRIPTOR_HEADER)
            self.emit(bytes([ble.F_DESCRIPTOR_CHUNK]) + struct.pack("<II", total, offset)
                      + self.descriptor[offset:offset + count])
        elif op == ble.OP_ABORT:
            assert len(data) == 1, data
            if self.reading:  # a stopped READ ends with ERROR 16, then ABORTED names it
                self.emit(bytes([ble.F_CHUNK]) + struct.pack("<HI", 1, 0) + b"PAR1")
                self.emit(bytes([ble.F_ERROR, ble.OP_READ, 16]) + b"aborted")
                self.emit(GOLDEN_ABORTED_READ)
            else:
                self.emit(GOLDEN_ABORTED_IDLE)


async def run_list_since():
    """LIST_SINCE request, LIST_SINCE_END, cursor file and the ERROR 11 fallback."""
    cursor = {"mount_epoch": 0xA1B2C3D4, "day": 20260315, "boot": "00112233445566778899aabbccddeeff", "seq": 7}
    assert bytes([ble.OP_LIST_SINCE]) + ble.encode_cursor(cursor) == GOLDEN_LIST_SINCE
    end = ble.decode_list_since_end(GOLDEN_LIST_SINCE_END)
    assert end == {"count": 2, "partials": 0, "sd_kib": 1000, "sd_used_kib": 10, "full": False,
                   "unsynced_full": True,
                   "cursor": {"mount_epoch": 0xA1B2C3D4, "day": 20260316,
                              "boot": "00112233445566778899aabbccddeeff", "seq": 42}}, end
    # LIST_END is its first 15 bytes.
    assert ble.decode_list_end(bytes([ble.F_LIST_END]) + GOLDEN_LIST_SINCE_END[1:15])["count"] == 2
    for frame in (GOLDEN_LIST_SINCE_END[:47], GOLDEN_LIST_SINCE_END + b"\0"):
        expect_protocol_error(lambda frame=frame: ble.decode_list_since_end(frame), "must be 48 bytes")
    client = FakeClient()
    session = ble.Session(client)
    await session.start()
    listed, end = await session.list_since(cursor)
    assert client.since_requests == [GOLDEN_LIST_SINCE]
    assert listed == {NAME: len(FILE), "legacy-parquet/x.parquet": 7} and end["cursor"]["seq"] == 42
    old = ble.Session(FakeClient(list_since=False))
    await old.start()
    try:
        await old.list_since(ble.ZERO_CURSOR)
    except ble.ListSinceUnsupported:
        pass
    else:
        raise AssertionError("ERROR 11 must raise ListSinceUnsupported")
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "cursors.json"
        assert ble.load_cursor(path, "abc") == ble.ZERO_CURSOR
        ble.save_cursor(path, "abc", end["cursor"])
        ble.save_cursor(path, "other", cursor)
        assert ble.load_cursor(path, "abc") == end["cursor"]
        assert ble.load_cursor(path, "other") == cursor
        assert ble.load_cursor(path, "new") == ble.ZERO_CURSOR
        # A corrupt file or entry warns and falls back to a full listing.
        for text in ("{not json", "[1, 2]", '{"abc": {"boot": "zz"}}', '{"abc": 7}', "\udcff"):
            path.write_text(text, errors="surrogateescape")
            warnings = io.StringIO()
            with contextlib.redirect_stderr(warnings):
                assert ble.load_cursor(path, "abc") == ble.ZERO_CURSOR, text
            assert "warning: ignoring" in warnings.getvalue(), text
        # Saving over a corrupt file replaces it with a valid one.
        path.write_text("{not json")
        with contextlib.redirect_stderr(io.StringIO()):
            ble.save_cursor(path, "abc", end["cursor"])
        assert ble.load_cursor(path, "abc") == end["cursor"]
    print("PASS ble_sync LIST_SINCE: request and end goldens, cursor per thing, corrupt cursor fallback, ERROR 11 fallback signal")


async def run():
    await run_list_since()
    await run_discovery()
    await run_read_deadlines()
    client = FakeClient()
    session = ble.Session(client)
    await session.start()
    status = await session.status()
    assert status["qf"] == 7 and status["qb"] == 123456 and status["part"] == 3, status
    listed, end = await session.list_files()
    assert listed == {NAME: len(FILE), "legacy-parquet/x.parquet": 7}, listed
    assert end["count"] == 2 and end["sd_kib"] == 1000
    payload, transfer = await session.fetch(NAME, 250)
    assert payload == FILE
    assert transfer["windows"] == -(-len(FILE) // 250), transfer
    result = await session.set_time(1_800_000_000)
    assert result == {"epoch_s": 1_800_000_000, "monotonic_us": 123456}
    try:
        await session.flush()
    except ble.ProtocolError as error:
        assert "code=10" in str(error) and "nothing-to-flush" in str(error), error
    else:
        raise AssertionError("ERROR frame must raise")
    try:
        await session.open("thing=abc/other.parquet")
    except ble.ProtocolError as error:
        assert "code=3" in str(error)
    else:
        raise AssertionError("OPEN of unknown file must raise")
    # A lost CHUNK must be detected by the absolute offset check, never patched over.
    lossy = ble.Session(FakeClient(drop_frame=True))
    await lossy.start()
    try:
        await lossy.fetch(NAME, 250)
    except ble.ProtocolError as error:
        assert "CHUNK offset" in str(error), error
    else:
        raise AssertionError("missing chunk must fail")
    # v2: configuration, scan, token, log, reboot.
    config = await session.get_config()
    assert config["ble"]["pair"] == "random" and config["reboot_required"] is False, config
    location = await session.get_config(1)
    assert location["location"]["cell"] == "" and location["location"]["lat"] is None
    owner = await session.get_config(5)
    assert owner["thing"]["deployment_id"] == "cairo-07", owner
    assert ble.LAST_CONFIG_PAGE == 6
    description = await session.get_config(6)
    assert description == json.loads(PAGE6) | {"reboot_required": False}, description
    writes = client.writes
    for page in (7, -1):
        try:
            await session.get_config(page)
        except ValueError:
            pass
        else:
            raise AssertionError(f"configuration page {page} must fail before transmission")
    assert client.writes == writes, "an invalid page is never sent"
    changed = await session.set_config(["ble.pair=fixed", "ble.pin=654321"])
    assert changed["ble"]["pair"] == "fixed" and changed["reboot_required"] is True, changed
    try:
        await session.set_config(["ble.pin=12"])
    except ble.ProtocolError as error:
        assert "code=12" in str(error) and "ble.pin" in str(error), error
    else:
        raise AssertionError("invalid config must raise")
    networks = await session.wifi_scan()
    assert [n["ssid"] for n in networks] == ["home", "cafe"] and networks[0]["auth"] == "WPA2", networks
    port, token = await session.get_token()
    assert port == 47390 and token == TOKEN
    text, meta = await session.log_tail(100)
    assert text.encode() == LOG[-100:] and meta["total_bytes_since_boot"] == len(LOG), meta
    assert await session.reboot() == {"delay_ms": 500}
    print("PASS ble_sync frame codec: list, fetch, time, error, lost-chunk detection, config, scan, token, log, reboot")
    await run_auth_refused()
    await run_protocol3()
    await run_lan()
    await run_lan_snapshot_deadlines()
    await run_lan_bye()
    await run_descriptor()
    await run_config_chunks()
    await run_ota()


async def run_config_chunks():
    """BLE CONFIG_CHUNK replies (#151): assembled before decoding, a UTF-8
    character split across chunks, and a gap, a changed total or a missing
    last chunk fail instead of returning partial JSON."""
    slice_bytes = 7
    split = [offset for offset in range(slice_bytes, len(PAGE6), slice_bytes)
             if PAGE6[offset] & 0xC0 == 0x80]
    assert split, "a chunk boundary falls inside a UTF-8 character"
    client = FakeClient()
    client.config_chunk = slice_bytes
    session = ble.Session(client)
    await session.start()
    assert await session.get_config(6) == json.loads(PAGE6) | {"reboot_required": False}
    legacy = await session.get_config()
    assert legacy["ble"]["pair"] == "random" and legacy["reboot_required"] is False, legacy
    changed = await session.set_config(["ble.pair=fixed", "ble.pin=654321"])
    assert changed["ble"]["pair"] == "fixed" and changed["reboot_required"] is True, changed

    async def refused(**fake):
        client = FakeClient()
        client.config_chunk = slice_bytes
        for key, value in fake.items():
            setattr(client, key, value)
        session = ble.Session(client)
        session.frame_timeout = 0.05
        await session.start()
        try:
            await session.get_config(6)
        except ble.ProtocolError as error:
            return str(error)
        raise AssertionError(f"{fake} must not decode")

    assert "CONFIG_CHUNK offset 14, expected 7" in await refused(config_drop=1)
    last = -(-len(PAGE6) // slice_bytes) - 1
    assert "no response frame within deadline" in await refused(config_drop=last)
    assert "flags or total changed" in await refused(config_skew=2)

    # The CLI accepts page 6, sends it and prints the assembled JSON.
    seen = []

    async def record(args):
        seen.append(args.page)

    for argv in (["ble_sync.py", "config", "--page", "6"], ["ble_sync.py", "config"]):
        with patch.object(ble, "cmd_config", record), patch.object(sys, "argv", argv):
            await asyncio.to_thread(ble.main)
    assert seen == [6, 0], seen
    with patch.object(sys, "argv", ["ble_sync.py", "config", "--page", "7"]), \
            contextlib.redirect_stderr(io.StringIO()):
        try:
            await asyncio.to_thread(ble.main)
        except SystemExit as error:
            assert error.code == 2, error
        else:
            raise AssertionError("--page 7 must be refused")
    client = FakeClient()
    client.config_chunk = slice_bytes
    client.disconnect = AsyncMock()
    session = ble.Session(client)
    await session.start()
    with patch.object(ble, "connect", AsyncMock(return_value=(client, session))), \
            contextlib.redirect_stdout(io.StringIO()) as out:
        await ble.cmd_config(SimpleNamespace(page=6))
    assert json.loads(out.getvalue()) == json.loads(PAGE6) | {"reboot_required": False}, out.getvalue()
    assert client.disconnect.await_count == 1
    print("PASS ble_sync CONFIG_CHUNK: page 6 and legacy replies assembled, UTF-8 split across chunks, "
          "gap, changed total and missing last chunk refused, CLI --page 6 accepted and 7 refused")


def descriptor_offsets(client):
    return [struct.unpack_from("<I", request, 1)[0] for request in client.descriptor_requests]


async def run_descriptor():
    """GET_DESCRIPTOR chunking, re-request, stale drop, hash check and ERROR 17."""
    assert len(DESCRIPTOR) == 1300
    assert struct.pack("<BIH", ble.OP_GET_DESCRIPTOR, 512, 1024) == GOLDEN_GET_DESCRIPTOR
    assert struct.pack("<BII", ble.F_DESCRIPTOR_CHUNK, 1300, 512) == GOLDEN_DESCRIPTOR_CHUNK_HEADER
    digest = hashlib.sha256(DESCRIPTOR).hexdigest()
    info = {"desc": digest[:16]}
    for budget in (512, 1024):
        client = FakeClient()
        client.descriptor_budget = budget
        session = ble.Session(client)
        await session.start()
        data = await session.descriptor()
        assert data == DESCRIPTOR, (budget, len(data))
        step = budget - ble.DESCRIPTOR_HEADER
        assert descriptor_offsets(client) == list(range(0, len(DESCRIPTOR), step)), descriptor_offsets(client)
        assert ble.check_descriptor(data, info, digest.upper()) == digest

    # A lost reply costs one re-request of the same offset, a stale chunk is dropped.
    client = FakeClient()
    client.descriptor_budget = 1024
    client.descriptor_drop = {600}
    client.descriptor_stale = True
    session = ble.Session(client)
    session.frame_timeout = 0.05
    await session.start()
    assert await session.descriptor(length=600) == DESCRIPTOR
    assert descriptor_offsets(client) == [0, 600, 600, 1200], descriptor_offsets(client)

    # A changed total drops the bytes and restarts from offset 0 once.
    other = DESCRIPTOR[:-2] + b'bb"}'
    client = FakeClient()
    client.descriptor_next = [other]
    session = ble.Session(client)
    await session.start()
    assert await session.descriptor() == other
    assert descriptor_offsets(client) == [0, 503, 0, 503, 1006], descriptor_offsets(client)
    # A second change after the restart fails the download.
    client = FakeClient()
    client.descriptor_next = [other, DESCRIPTOR]
    session = ble.Session(client)
    await session.start()
    try:
        await session.descriptor()
    except ble.ProtocolError as error:
        assert "again after a restart" in str(error), error
    else:
        raise AssertionError("a second total change must raise")
    assert descriptor_offsets(client) == [0, 503, 0, 503], descriptor_offsets(client)

    # A reply lost every time gives up after DESCRIPTOR_TRIES requests.
    client = FakeClient()
    client.descriptor_drop = {0}
    client.descriptor_drop_always = True
    session = ble.Session(client)
    session.frame_timeout = 0.05
    await session.start()
    try:
        await session.descriptor()
    except ble.ProtocolError as error:
        assert "deadline" in str(error), error
    else:
        raise AssertionError("a reply lost every time must raise")
    assert descriptor_offsets(client) == [0] * ble.DESCRIPTOR_TRIES

    # An unconfigured board answers ERROR 17, never a retry.
    client = FakeClient()
    client.descriptor = None
    session = ble.Session(client)
    await session.start()
    try:
        await session.descriptor()
    except ble.ProtocolError as error:
        assert "no-descriptor" in str(error), error
    else:
        raise AssertionError("ERROR 17 must raise")
    assert len(client.descriptor_requests) == 1

    # The command writes the exact device bytes after the info desc check.
    client = FakeClient()
    client.disconnect = AsyncMock()
    session = ble.Session(client)
    await session.start()
    session.info = AsyncMock(return_value=info)
    with tempfile.TemporaryDirectory() as tmp, \
            patch.object(ble, "connect", AsyncMock(return_value=(client, session))), \
            contextlib.redirect_stdout(io.StringIO()) as out:
        target = Path(tmp) / "nested" / "template.json"
        await ble.cmd_descriptor(SimpleNamespace(out=target, expect=digest))
        assert target.read_bytes() == DESCRIPTOR
    assert f"sha256={digest}" in out.getvalue() and client.disconnect.await_count == 1, out.getvalue()

    expect_protocol_error(lambda: ble.check_descriptor(DESCRIPTOR + b" ", info), "does not start with")
    expect_protocol_error(lambda: ble.check_descriptor(DESCRIPTOR, {}), "no 16 hex desc")
    expect_protocol_error(lambda: ble.check_descriptor(DESCRIPTOR, info, "0" * 64), "expected")
    print("PASS ble_sync GET_DESCRIPTOR: golden request and header, 512 and 1024 budgets, "
          "re-request after a lost reply, stale chunk dropped, one restart on a changed total, retry limit, "
          "ERROR 17, hash check")


async def run_auth_refused():
    """An unpaired refusal fails at once with a pairing hint and is never retried.

    Over BLE the real case is the ATT error checked second. ERROR 14 is the
    LAN answer and the BLE firmware fallback."""
    client = FakeClient(auth_refused=True)
    session = ble.Session(client)
    await session.start()
    started = asyncio.get_running_loop().time()
    try:
        await session.list_files()
    except ble.ProtocolError as error:
        text = str(error)
        assert "op=0x01 code=14 (auth-failed)" in text and "detail='auth'" in text, text
        assert "Pair the device again" in text and "busy" not in text, text
    else:
        raise AssertionError("ERROR 14 must raise")
    assert client.writes == 1, client.writes
    assert asyncio.get_running_loop().time() - started < 0.5
    print("PASS ble_sync auth refusal: ERROR 14 raises at once with a pairing hint, one write, no busy retry")
    # NimBLE rejects an unencrypted Control write at the ATT layer, so the
    # backend raises before any ERROR frame. Same hint, still one write.
    for wording in ("Insufficient Authentication", "Authentication is insufficient.",
                    "Protocol Error 0x05: Insufficient Authentication",
                    "Protocol Error 0x0C: Insufficient Encryption Key Size",
                    "Protocol Error 0x0F: Insufficient Encryption",
                    "Insufficient encryption", "Not paired"):
        client = FakeClient(att_error=RuntimeError(wording))
        session = ble.Session(client)
        await session.start()
        try:
            await session.list_files()
        except ble.ProtocolError as error:
            assert "refused by ATT" in str(error) and "Pair the device again" in str(error), error
        else:
            raise AssertionError("ATT auth refusal must raise")
        assert client.writes == 1, client.writes
    # Any other backend failure passes through unchanged.
    for other in (OSError("adapter gone"),
                  RuntimeError("Protocol Error 0x08: Insufficient Authorization"),
                  RuntimeError("[org.bluez.Error.NotAuthorized] Not authorized")):
        client = FakeClient(att_error=other)
        session = ble.Session(client)
        await session.start()
        try:
            await session.list_files()
        except ble.ProtocolError:
            raise AssertionError(f"{other!r} must not be mapped")
        except (OSError, RuntimeError):
            pass
    print("PASS ble_sync ATT auth refusal: mapped to the pairing hint, other write errors untouched")


def expect_protocol_error(call, needle):
    try:
        call()
    except ble.ProtocolError as error:
        assert needle in str(error), error
    else:
        raise AssertionError(f"expected ProtocolError containing {needle!r}")


async def run_protocol3():
    """LIST_END width, DIAG, ABORTED and BYE goldens from the protocol 3 contract."""
    assert len(GOLDEN_LIST_END) == 15 and len(GOLDEN_LIST_END_V2) == 13
    assert ble.decode_list_end(GOLDEN_LIST_END) == {"count": 70000, "partials": 1,
                                                    "sd_kib": 15558144, "sd_used_kib": 1024}
    for frame in (GOLDEN_LIST_END_V2, GOLDEN_LIST_END[:14], GOLDEN_LIST_END + b"\0"):
        expect_protocol_error(lambda frame=frame: ble.decode_list_end(frame), "LIST_END must be 15 bytes")
    # A proto 2 device's 13-byte LIST_END fails the listing, never commits it.
    old = ble.Session(FakeClient(list_end=bytes([ble.F_LIST_END]) + struct.pack("<HHII", 2, 0, 1000, 10)))
    await old.start()
    try:
        await old.list_files()
    except ble.ProtocolError as error:
        assert "LIST_END must be 15 bytes, got 13" in str(error), error
    else:
        raise AssertionError("13-byte LIST_END must raise")

    event = ble.decode_event(GOLDEN_EVENT)
    assert event == {"uptime_s": 120, "generation": 7, "reason": "auth-failed", "reason_code": 3,
                     "link": 2, "link_name": "lan2", "err": -1, "queued_frames": 0, "boot_seq": 2}, event
    assert len(GOLDEN_DIAG) == 54
    diag = ble.decode_diag(GOLDEN_DIAG)
    assert diag["boot_seq"] == 2 and len(diag["counts"]) == 17, diag
    assert diag["counts"][1] == 5 and diag["counts"][3] == 1 and sum(diag["counts"]) == 6, diag
    assert diag["events"] == [event], diag
    expect_protocol_error(lambda: ble.decode_diag(GOLDEN_DIAG[:-1]), "DIAG length")
    expect_protocol_error(lambda: ble.decode_diag(GOLDEN_DIAG[:37]), "DIAG length")

    assert ble.decode_aborted(GOLDEN_ABORTED_IDLE) == {"op": 0}
    assert ble.decode_aborted(GOLDEN_ABORTED_READ) == {"op": ble.OP_READ}
    expect_protocol_error(lambda: ble.decode_aborted(b"\x4d"), "ABORTED must be 2 bytes")

    bye = ble.decode_bye(GOLDEN_BYE)
    assert bye == {"reason": "idle-timeout", "reason_code": 1, "generation": 0x01020304,
                   "err": -11, "queued_frames": 3, "uptime_s": 3600}, bye
    expect_protocol_error(lambda: ble.decode_bye(GOLDEN_BYE[:13]), "BYE must be 14 bytes")
    # Codes from a newer device are shown as unknown, never rejected.
    assert ble.decode_bye(bytes([ble.F_BYE, 200]) + GOLDEN_BYE[2:])["reason"] == "unknown"
    assert ble.DISCONNECT_REASONS[16] == "keepalive-failed" and len(ble.DISCONNECT_REASONS) == 17

    client = FakeClient()
    session = ble.Session(client)
    await session.start()
    assert (await session.get_diag())["events"][0]["reason"] == "auth-failed"
    assert await session.abort() == {"op": 0, "error_aborted_op": None}
    client.reading = True
    assert await session.abort() == {"op": ble.OP_READ, "error_aborted_op": ble.OP_READ}
    print("PASS ble_sync protocol 3: 15-byte LIST_END (13 rejected), DIAG, ABORTED, BYE goldens, GET_DIAG and ABORT")


async def run_discovery():
    """Connect discovery accepts ADV service data without the SCAN_RSP UUIDs."""
    candidate = SimpleNamespace(name=None, address="test-device")
    unrelated = SimpleNamespace(service_uuids=[], service_data={"other": b"x"},
                                local_name="OS-6b40")
    adv_only = SimpleNamespace(service_uuids=[],
                               service_data={ble.SERVICE.upper(): b"\x01\x04"},
                               local_name="OS-6b40")
    legacy = SimpleNamespace(service_uuids=[ble.SERVICE.upper()],
                             service_data={}, local_name=None)
    assert not ble.advertises_sync_service(unrelated)
    assert ble.advertises_sync_service(adv_only)
    assert ble.advertises_sync_service(legacy)

    class Scanner:
        @staticmethod
        async def find_device_by_filter(predicate, timeout):
            assert timeout == 5
            assert not predicate(candidate, unrelated)
            assert not predicate(SimpleNamespace(name="OS-other"),
                                 SimpleNamespace(service_uuids=[ble.SERVICE],
                                                 service_data={}, local_name="OS-other"))
            assert predicate(SimpleNamespace(name="OS-6b40"), legacy)
            return candidate if predicate(candidate, adv_only) else None

    original = ble.BleakScanner
    ble.BleakScanner = Scanner
    try:
        assert await ble.find(SimpleNamespace(name="OS-6b40", timeout=5)) is candidate
        assert await ble.find(SimpleNamespace(address="known-device")) == "known-device"
    finally:
        ble.BleakScanner = original
    print("PASS ble_sync discovery: ADV-only service data, scan-response UUIDs and name matching")


async def run_read_deadlines():
    """The pairing read can outlast one backend timeout, but never its deadline."""
    client = FakeClient()
    client.connect = AsyncMock()
    args = SimpleNamespace(address="known-device", timeout=90)
    with patch.object(ble, "BleakClient", return_value=client) as constructor:
        connected, session = await ble.connect(args)
    constructor.assert_called_once_with("known-device", timeout=90)
    assert connected is client and session.read_timeout == 90
    assert (await session.info())["proto"] == 3
    assert (await session.status())["qf"] == 7

    # Simulate the CoreBluetooth delegate's timeout while pairing, followed by
    # success once the user has answered. No backend-specific kwargs are needed.
    read = AsyncMock(side_effect=[TimeoutError(), bytearray(INFO)])
    session = ble.Session(SimpleNamespace(read_gatt_char=read), read_timeout=0.4)
    assert (await session.info())["proto"] == 3
    assert read.await_count == 2
    assert all(call.args == (ble.INFO,) and not call.kwargs for call in read.await_args_list)

    # INFO, STATUS and the initial LIVE read all cancel a stalled backend at
    # the session deadline, rather than waiting for a backend's own timeout.
    for uuid in (ble.INFO, ble.STATUS, ble.LIVE):
        cancelled = asyncio.Event()

        async def stalled(_uuid):
            try:
                await asyncio.Future()
            finally:
                cancelled.set()

        session = ble.Session(SimpleNamespace(read_gatt_char=stalled), read_timeout=0.02)
        started = asyncio.get_running_loop().time()
        try:
            if uuid == ble.INFO:
                await session.info()
            elif uuid == ble.STATUS:
                await session.status()
            else:
                await session.read_char(uuid)
        except ble.ProtocolError as error:
            assert uuid in str(error) and "0.02s" in str(error), error
        else:
            raise AssertionError("stalled characteristic read must fail")
        assert cancelled.is_set()
        assert asyncio.get_running_loop().time() - started < 0.5

    # Repeated backend timeouts cannot reset the outer deadline or spin freely.
    read = AsyncMock(side_effect=TimeoutError)
    session = ble.Session(SimpleNamespace(read_gatt_char=read), read_timeout=0.15)
    started = asyncio.get_running_loop().time()
    try:
        await session.info()
    except ble.ProtocolError:
        pass
    else:
        raise AssertionError("backend timeouts must exhaust the fixed deadline")
    assert 1 <= read.await_count <= 2
    assert asyncio.get_running_loop().time() - started < 0.5

    # ATT/connection failures and explicit caller cancellation must not retry.
    for failure in (RuntimeError("ATT read refused"), asyncio.CancelledError()):
        read = AsyncMock(side_effect=failure)
        session = ble.Session(SimpleNamespace(read_gatt_char=read), read_timeout=0.4)
        try:
            await session.status()
        except type(failure) as error:
            assert error is failure
        else:
            raise AssertionError("non-timeout failure must propagate")
        assert read.await_count == 1

    # The LIVE command must use Session too, not bypass the configured deadline.
    client.is_connected = True
    client.disconnect = AsyncMock()
    live_session = SimpleNamespace(read_char=AsyncMock(return_value=bytearray(b'{"buf":1}')))
    with patch.object(ble, "connect", AsyncMock(return_value=(client, live_session))):
        await ble.cmd_live(SimpleNamespace(seconds=0))
    live_session.read_char.assert_awaited_once_with(ble.LIVE)
    client.disconnect.assert_awaited_once()
    print("PASS ble_sync read deadlines: configured pairing retries, INFO/STATUS/LIVE bounds, cancellation and errors")


async def run_lan():
    """A scripted TCP device: handshake, HELLO, framed requests, pushes between CHUNKs."""
    fake = FakeClient(chunk=200)
    pushed = []

    async def serve(reader, writer):
        head = await reader.readexactly(36)
        if head != b"OSS1" + TOKEN:
            writer.write(struct.pack("<H", 3) + bytes([ble.F_ERROR, 0, 14]))
            await writer.drain()
            writer.close()
            return
        hello = bytes([ble.F_HELLO, 3]) + struct.pack("<H", 1024) + INFO
        writer.write(struct.pack("<H", len(hello)) + hello)
        emitted = []
        fake.callback = lambda _c, data: emitted.append(bytes(data))
        while True:
            try:
                (length,) = struct.unpack("<H", await reader.readexactly(2))
                request = await reader.readexactly(length)
            except asyncio.IncompleteReadError:
                return
            emitted.clear()
            await fake.write_gatt_char(None, request)
            for index, frame in enumerate(emitted):
                writer.write(struct.pack("<H", len(frame)) + frame)
                if index == 0 and request[0] == ble.OP_READ:
                    live = bytes([ble.F_LIVE]) + b'{"seq":1}'
                    writer.write(struct.pack("<H", len(live)) + live)  # push mid-transfer
            await writer.drain()

    server = await asyncio.start_server(serve, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    link = ble.LanLink("127.0.0.1", TOKEN, port)
    await link.connect()
    assert link.payload_max == 1024 and link.mtu_size - 3 == 1024
    await link.start_notify(ble.LIVE, lambda _c, data: pushed.append(bytes(data)))
    session = ble.Session(link)
    await session.start()
    assert (await session.info())["proto"] == 3
    assert (await session.status())["qf"] == 7
    listed, _ = await session.list_files()
    payload, transfer = await session.fetch(NAME, 1000)
    assert payload == FILE and transfer["windows"] == 1, transfer
    assert pushed == [b'{"seq":1}'], pushed
    assert await session.read_char(ble.LIVE) == bytearray(b'{"seq":1}')
    assert (await session.get_diag())["counts"][1] == 5
    assert await session.abort() == {"op": 0, "error_aborted_op": None}
    await link.disconnect()
    bad = ble.LanLink("127.0.0.1", bytes(32), port)
    try:
        await bad.connect()
    except ble.ProtocolError as error:
        assert "code=14" in str(error), error
    else:
        raise AssertionError("wrong token must be refused")
    server.close()
    await server.wait_closed()
    print("PASS ble_sync LAN link: handshake, HELLO, framed fetch with LIVE push interleaved, auth refusal")


async def run_lan_snapshot_deadlines():
    """A fresh authenticated socket may receive its first STATUS after 2 s."""
    requests = []

    async def serve(reader, writer):
        async def emit(frame):
            writer.write(struct.pack("<H", len(frame)) + frame)
            await writer.drain()

        try:
            assert await reader.readexactly(36) == b"OSS1" + TOKEN
            await emit(bytes([ble.F_HELLO, 3]) + struct.pack("<H", 1024) + INFO)
            while True:
                (length,) = struct.unpack("<H", await reader.readexactly(2))
                request = await reader.readexactly(length)
                assert request == bytes([ble.OP_STATUS]), request
                requests.append(request)
                if len(requests) == 1:
                    await emit(bytes([ble.F_STATUS]) + b"{}")
                    await asyncio.sleep(2.15)
                    await emit(bytes([ble.F_STATUS]) + STATUS)
                elif len(requests) == 2:
                    await emit(bytes([ble.F_STATUS]) + STATUS.replace(b'"qf":7', b'"qf":8'))
                # Later requests deliberately receive no STATUS or LIVE.
        except asyncio.IncompleteReadError:
            pass
        finally:
            writer.close()
            await writer.wait_closed()

    server = await asyncio.start_server(serve, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    link = ble.LanLink("127.0.0.1", TOKEN, port, timeout=3)
    try:
        await link.connect()
        session = ble.Session(link, read_timeout=3)
        await session.start()
        started = asyncio.get_running_loop().time()
        assert (await session.info())["proto"] == 3
        assert asyncio.get_running_loop().time() - started < 0.5
        started = asyncio.get_running_loop().time()
        assert (await session.status())["qf"] == 7
        assert asyncio.get_running_loop().time() - started >= 2
        assert (await session.status())["qf"] == 8  # fresh, not the cached 7
        assert len(requests) == 2

        session.read_timeout = 0.02
        for uuid in (ble.STATUS, ble.LIVE):
            try:
                await session.read_char(uuid)
            except ble.ProtocolError as error:
                assert uuid in str(error) and "0.02s" in str(error), error
            else:
                raise AssertionError("missing LAN snapshot must fail at the session deadline")
        # Direct LanLink callers are also bounded by the link timeout.
        link.timeout = 0.02
        try:
            await link.read_gatt_char(ble.LIVE)
        except TimeoutError:
            pass
        else:
            raise AssertionError("direct LAN read must not wait indefinitely")
    finally:
        await link.disconnect()
        server.close()
        await server.wait_closed()
    print("PASS ble_sync LAN snapshots: fresh OP_STATUS, >2 s response, empty push rejection and deadline cancellation")


async def run_lan_bye():
    """BYE on the LAN read path raises the typed close, and later reads fail at once."""
    async def serve(reader, writer):
        try:
            assert await reader.readexactly(36) == b"OSS1" + TOKEN
            hello = bytes([ble.F_HELLO, 3]) + struct.pack("<H", 1024) + INFO
            writer.write(struct.pack("<H", len(hello)) + hello)
            await writer.drain()
            (length,) = struct.unpack("<H", await reader.readexactly(2))
            assert await reader.readexactly(length) == bytes([ble.OP_GET_DIAG])
            wire = struct.pack("<H", len(GOLDEN_BYE)) + GOLDEN_BYE
            assert wire[:2] == bytes.fromhex("0e00") and len(wire) == 16
            writer.write(wire)
            await writer.drain()
            await asyncio.sleep(0.05)
        finally:
            writer.close()
            await writer.wait_closed()

    server = await asyncio.start_server(serve, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    link = ble.LanLink("127.0.0.1", TOKEN, port, timeout=3)
    try:
        await link.connect()
        session = ble.Session(link)
        await session.start()
        try:
            await session.get_diag()
        except ble.LinkClosedByDevice as error:
            assert error.reason == "idle-timeout" and error.generation == 0x01020304, error
            assert error.err == -11 and error.queued_frames == 3 and error.uptime_s == 3600, error
            assert isinstance(error, ble.ProtocolError) and "reason=idle-timeout" in str(error)
            first = error
        else:
            raise AssertionError("BYE must raise LinkClosedByDevice")
        assert not link.is_connected
        started = asyncio.get_running_loop().time()
        for later in (session.frame(timeout=5), session.list_files()):
            try:
                await later
            except ble.LinkClosedByDevice as error:
                assert error is first
            else:
                raise AssertionError("reads after BYE must raise the same close")
        assert asyncio.get_running_loop().time() - started < 0.5
    finally:
        await link.disconnect()
        server.close()
        await server.wait_closed()

    # REBOOT over LAN waits for BYE reboot instead of hanging up first.
    bye_received = asyncio.Event()

    async def rebooting(reader, writer):
        try:
            await reader.readexactly(36)
            hello = bytes([ble.F_HELLO, 2]) + struct.pack("<H", 1024) + INFO
            writer.write(struct.pack("<H", len(hello)) + hello)
            await writer.drain()
            (length,) = struct.unpack("<H", await reader.readexactly(2))
            assert await reader.readexactly(length) == bytes([ble.OP_REBOOT])
            for frame in (bytes([ble.F_REBOOTING]) + struct.pack("<H", 500),
                          bytes([ble.F_BYE, ble.DISCONNECT_REASONS.index("reboot")]) + GOLDEN_BYE[2:]):
                await asyncio.sleep(0.05)
                writer.write(struct.pack("<H", len(frame)) + frame)
                await writer.drain()
            # The client is still connected when BYE is written.
            if not reader.at_eof():
                bye_received.set()
        finally:
            writer.close()
            await writer.wait_closed()

    server = await asyncio.start_server(rebooting, "127.0.0.1", 0)
    link = ble.LanLink("127.0.0.1", TOKEN, server.sockets[0].getsockname()[1], timeout=3)
    try:
        await link.connect()
        session = ble.Session(link)
        await session.start()
        assert await session.reboot() == {"delay_ms": 500, "bye": "reboot"}
        assert bye_received.is_set()
    finally:
        await link.disconnect()
        server.close()
        await server.wait_closed()

    # EOF without BYE is reason unknown and also fails at once.
    async def drop(reader, writer):
        await reader.readexactly(36)
        hello = bytes([ble.F_HELLO, 3]) + struct.pack("<H", 1024) + INFO
        writer.write(struct.pack("<H", len(hello)) + hello)
        await writer.drain()
        writer.close()

    server = await asyncio.start_server(drop, "127.0.0.1", 0)
    link = ble.LanLink("127.0.0.1", TOKEN, server.sockets[0].getsockname()[1], timeout=3)
    try:
        await link.connect()
        session = ble.Session(link)
        await session.start()
        started = asyncio.get_running_loop().time()
        try:
            await session.frame(timeout=5)
        except ble.LinkClosedByDevice:
            raise AssertionError("EOF without BYE is not a typed close") from None
        except ble.ProtocolError as error:
            assert "without BYE" in str(error) and "unknown" in str(error), error
        else:
            raise AssertionError("EOF must raise")
        assert asyncio.get_running_loop().time() - started < 0.5
    finally:
        await link.disconnect()
        server.close()
        await server.wait_closed()
    print("PASS ble_sync LAN BYE: typed close with reason, no hang after BYE, reboot waits for BYE, EOF without BYE is unknown")


# OTA_STATE golden from docs/shared/ble-sync-protocol.md, section LAN OTA.
GOLDEN_OTA_STATE = bytes.fromhex("50" "01" "04030201" "0d0c0b0a" "00006000" "fb01")


def ota_image(size=1300, project=b"opensensor_cores3"):
    image = bytearray(size)
    image[0] = 0xE9
    struct.pack_into("<I", image, 32, 0xABCD5432)
    image[80:80 + len(project)] = project
    for i in range(112, size):
        image[i] = i % 251
    return bytes(image)


class OtaFake:
    """Scripted OTA device. Keeps next and size like opensensor::ota::Transfer."""

    def __init__(self, max_chunk=507, reject_at=None, skew=False, end_error=None):
        self.callback = None
        self.max_chunk, self.reject_at, self.skew, self.end_error = max_chunk, reject_at, skew, end_error
        self.phase, self.next, self.size, self.digest = 0, 0, 0, b""
        self.received = bytearray()
        self.requests = []

    async def start_notify(self, _uuid, callback):
        self.callback = callback

    def state(self, nxt=None):
        return struct.pack("<BBIIIH", ble.F_OTA_STATE, self.phase,
                           self.next if nxt is None else nxt, self.size, 0x600000, self.max_chunk)

    def error(self, op, code, detail):
        self.phase, self.next, self.size = 0, 0, 0
        return bytes([ble.F_ERROR, op, code]) + detail

    async def write_gatt_char(self, _uuid, data, response=True):
        data = bytes(data)
        op = data[0]
        self.requests.append(op)
        if op == ble.OP_OTA_BEGIN:
            (self.size,) = struct.unpack_from("<I", data, 1)
            self.digest, self.phase, self.next = data[5:37], 1, 0
            self.received.clear()
            reply = self.state()
        elif op == ble.OP_OTA_WRITE:
            (offset,) = struct.unpack_from("<I", data, 1)
            chunk = data[5:]
            assert len(data) <= 512 and len(chunk) <= self.max_chunk
            if offset != self.next:
                reply = self.error(op, 6, b"offset")
            elif self.reject_at is not None and offset >= self.reject_at:
                reply = self.error(op, 18, b"project")
            else:
                self.received += chunk
                self.next += len(chunk)
                reply = self.state(self.next + 1 if self.skew else None)
        elif op == ble.OP_OTA_END:
            if self.end_error:
                reply = self.error(op, 18, self.end_error)
            elif self.next == self.size and hashlib.sha256(self.received).digest() == self.digest:
                self.phase = 2
                reply = self.state()
            else:
                reply = self.error(op, 18, b"sha256")
        elif op == ble.OP_OTA_CANCEL:
            self.phase, self.next, self.size = 0, 0, 0
            reply = self.state()
        else:
            raise AssertionError(f"unexpected op 0x{op:02x}")
        self.callback(None, bytearray(reply))


async def run_ota():
    assert ble.decode_ota_state(GOLDEN_OTA_STATE) == {
        "phase": "receiving", "next": 0x01020304, "size": 0x0A0B0C0D,
        "slot_bytes": 0x600000, "max_chunk": 507}
    expect_protocol_error(lambda: ble.decode_ota_state(GOLDEN_OTA_STATE[:-1]), "bad OTA_STATE")
    assert ble.ERRORS[18] == "image-rejected"
    image = ota_image()
    assert ble.check_ota_image(image) == "opensensor_cores3"
    expect_protocol_error(lambda: ble.check_ota_image(image[:111]), "not an ESP-IDF app image")
    expect_protocol_error(lambda: ble.check_ota_image(b"\x00" + image[1:]), "not an ESP-IDF app image")
    broken = bytearray(image)
    broken[32] ^= 1
    expect_protocol_error(lambda: ble.check_ota_image(bytes(broken)), "app description missing")

    # Happy path. BEGIN carries size and SHA-256, chunks are contiguous and
    # at most max_chunk, END is the last request and nothing reboots.
    fake = OtaFake(max_chunk=300)
    session = ble.Session(fake)
    await session.start()
    seen = []
    result = await session.ota_push(image, lambda done, total: seen.append(done))
    assert result["phase"] == "ready" and result["size"] == len(image)
    assert result["sha256"] == hashlib.sha256(image).hexdigest()
    assert bytes(fake.received) == image
    assert fake.requests == [ble.OP_OTA_BEGIN] + [ble.OP_OTA_WRITE] * 5 + [ble.OP_OTA_END]
    assert seen == [300, 600, 900, 1200, 1300]
    assert ble.OP_REBOOT not in fake.requests

    # A device ERROR has already aborted the upload, so no CANCEL follows.
    fake = OtaFake(reject_at=0)
    session = ble.Session(fake)
    await session.start()
    try:
        await session.ota_push(image)
    except ble.ProtocolError as error:
        assert "code=18 (image-rejected)" in str(error), error
    else:
        raise AssertionError("rejected image must fail")
    assert fake.requests == [ble.OP_OTA_BEGIN, ble.OP_OTA_WRITE]

    # A device that reports the wrong offset is cancelled, never trusted.
    fake = OtaFake(skew=True)
    session = ble.Session(fake)
    await session.start()
    try:
        await session.ota_push(image)
    except ble.ProtocolError as error:
        assert "expected 507" in str(error), error
    else:
        raise AssertionError("skewed state must fail")
    assert fake.requests[-1] == ble.OP_OTA_CANCEL and fake.phase == 0

    fake = OtaFake(end_error=b"digest")
    session = ble.Session(fake)
    await session.start()
    try:
        await session.ota_push(image)
    except ble.ProtocolError as error:
        assert "detail='digest'" in str(error), error
    else:
        raise AssertionError("digest mismatch must fail")
    assert fake.requests[-1] == ble.OP_OTA_END

    fake = OtaFake()
    session = ble.Session(fake)
    await session.start()
    assert (await session.ota_cancel())["phase"] == "idle"
    try:
        ble.lan_only(SimpleNamespace(lan=None), "ota")
    except SystemExit as error:
        assert "LAN only" in str(error)
    else:
        raise AssertionError("ota must refuse BLE")
    print("PASS ble_sync OTA: STATE golden, local image checks, contiguous chunks, no auto reboot, "
          "device errors end the push, skew is cancelled")


if __name__ == "__main__":
    asyncio.run(run())
