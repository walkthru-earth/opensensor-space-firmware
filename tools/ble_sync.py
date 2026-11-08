"""Host client for the device sync service (docs/shared/ble-sync-protocol.md, v2).

Bench tool: exercises the same contract the phone app implements, from a
laptop, so the firmware can be verified without a handset. Read-only towards
the device except for the explicitly named write operations (`time`, `flush`,
`set`, `reboot`, `ota`, `ota-cancel`).

  pixi run ble-sync scan [--seconds 8]
  pixi run ble-sync info  [--name OS-6b40]
  pixi run ble-sync status
  pixi run ble-sync live  [--seconds 35]
  pixi run ble-sync list
  pixi run ble-sync fetch <relative-name> --out <dir>
  pixi run ble-sync sync  --out <dir>          # every listed file, verified
  pixi run ble-sync time                       # send host UTC (explicit)
  pixi run ble-sync flush                      # finalize RAM rows (explicit)
  pixi run ble-sync config                     # GET_CONFIG as JSON
  pixi run ble-sync set wifi.ssid=home wifi.psk=... wifi.on=1   # SET_CONFIG
  pixi run ble-sync wifi-scan                  # WIFI_AP list
  pixi run ble-sync token --save artifacts/lan-token.txt        # BLE only
  pixi run ble-sync log [--bytes 8192]         # LOG_TAIL
  pixi run ble-sync diag                       # GET_DIAG disconnect counters and events
  pixi run ble-sync descriptor --out artifacts/template.json   # GET_DESCRIPTOR, hash checked
  pixi run ble-sync abort                      # ABORT whatever the device is running
  pixi run ble-sync reboot                     # explicit
  pixi run ble-sync --lan HOST --token-file F ota build/app.bin [--reboot]   # LAN OTA, explicit
  pixi run ble-sync --lan HOST --token-file F ota-cancel        # drop a staged or partial image

Every command also runs over the LAN transport once a token is held:

  pixi run ble-sync --lan os-6b40.local --token-file artifacts/lan-token.txt list

macOS pairs through a system passkey dialog on first encrypted access; type the
six digits shown on the CoreS3 screen (or the fixed PIN on a screenless
board). Bleak cannot drive pairing itself. `dns-sd -B _ossync._tcp` browses
the LAN for devices.
"""
import argparse
import asyncio
import binascii
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

import parquet_device as device

BASE = "255f595e-{:04x}-47ed-8e0d-59994199b680"
SERVICE = BASE.format(1)
INFO, STATUS, LIVE, CONTROL, RESPONSE = (BASE.format(i) for i in range(2, 7))

OP_LIST, OP_OPEN, OP_READ, OP_CLOSE, OP_SET_TIME, OP_FLUSH, OP_STATUS = range(1, 8)
OP_GET_CONFIG, OP_SET_CONFIG, OP_WIFI_SCAN, OP_REBOOT, OP_LOG_TAIL, OP_GET_TOKEN = range(8, 14)
OP_GET_DIAG, OP_ABORT = 0x0E, 0x0F
# Opcodes and frame types are separate number spaces, so OP_LIST_SINCE and
# F_FILE share the value 0x10.
OP_LIST_SINCE = 0x10
OP_GET_DESCRIPTOR = 0x11
# LAN OTA (#40). LAN only, BLE answers ERROR 13.
OP_OTA_BEGIN, OP_OTA_WRITE, OP_OTA_END, OP_OTA_CANCEL = 0x12, 0x13, 0x14, 0x15
F_FILE, F_LIST_END = 0x10, 0x11
F_OPENED, F_CHUNK, F_READ_END, F_CLOSED = 0x20, 0x21, 0x22, 0x23
F_TIME_SET, F_FLUSHED, F_ERROR = 0x30, 0x31, 0x7F
F_CONFIG, F_WIFI_AP, F_WIFI_SCAN_END, F_REBOOTING = 0x40, 0x41, 0x42, 0x43
F_LOG, F_LOG_END, F_TOKEN, F_HELLO, F_STATUS, F_LIVE = 0x44, 0x45, 0x46, 0x47, 0x48, 0x49
F_BYE, F_DIAG, F_ABORTED, F_LIST_SINCE_END = 0x4B, 0x4C, 0x4D, 0x4E
F_DESCRIPTOR_CHUNK = 0x4F
F_OTA_STATE = 0x50
OTA_PHASES = {0: "idle", 1: "receiving", 2: "ready"}
# esp_image_header_t, segment header and esp_app_desc_t offsets, as in
# firmware/common/src/ota_transfer.h.
OTA_IMAGE_MAGIC, OTA_DESC_MAGIC = 0xE9, 0xABCD5432
OTA_HEADER_BYTES = 112
ERRORS = {1: "malformed", 2: "invalid-name", 3: "not-finalized", 4: "open-failed",
          5: "bad-handle", 6: "range", 7: "busy", 8: "storage-unavailable",
          9: "invalid-epoch", 10: "nothing-to-flush", 11: "unknown-op",
          12: "invalid-config", 13: "not-on-this-link", 14: "auth-failed", 15: "wifi-unavailable",
          16: "aborted", 17: "no-descriptor", 18: "image-rejected"}
DESCRIPTOR_HEADER = 9  # type, total u32, offset u32
DESCRIPTOR_TRIES = 3   # attempts per offset before giving up
# What a person should do about an ERROR. Code 14 is never a busy device, so
# the client never retries it. Over LAN it means the token was rejected. Over
# BLE an unpaired link normally gets the ATT refusal below instead, and 14 is
# only the firmware's fallback if its check and NimBLE's ever disagree.
ERROR_HINTS = {14: "the link is not authenticated. Pair the device again, or for LAN fetch a new token over BLE"}
# Backend wording for an ATT Insufficient Authentication (0x05), Encryption
# Key Size (0x0C) or Encryption (0x0F) write error. BlueZ, CoreBluetooth and
# WinRT each phrase it their way, and BlueZ may only say "Not paired".
# Insufficient Authorization (0x08) is a different refusal.
ATT_AUTH_REFUSAL = re.compile(
    r"insufficient (authentication|encryption)|(authentication|encryption) is insufficient"
    r"|not paired|protocol error 0x0?[5cf]\b",
    re.IGNORECASE)
# Protocol 3 typed disconnect reasons (BYE, DIAG events, STATUS `dr`).
DISCONNECT_REASONS = ["unknown", "idle-timeout", "handshake-timeout", "auth-failed",
                      "sessions-full", "slow-peer", "tx-overflow", "malformed", "wifi-lost",
                      "lan-disabled", "token-rotated", "reboot", "peer-closed", "recv-error",
                      "send-error", "server-stop", "keepalive-failed"]
WIFI_AUTH = {0: "open", 1: "WEP", 2: "WPA", 3: "WPA2", 4: "WPA/WPA2", 5: "WPA2-Enterprise",
             6: "WPA3", 7: "WPA2/WPA3", 255: "other"}
LAN_PORT = 47390
LAN_PAYLOAD_MAX = 1024


class LanLink:
    """TCP transport with the BleakClient surface Session uses (docs: LAN transport).

    Length-prefixed frames; STATUS/LIVE pushes are demultiplexed by type byte and
    delivered to the callbacks registered for the matching characteristic UUID,
    so every command works unchanged over either link.
    """

    def __init__(self, host: str, token: bytes, port: int = LAN_PORT, timeout: float = 10.0):
        if len(token) != 32:
            raise ProtocolError("LAN token must be 32 bytes")
        self.host, self.port, self.token, self.timeout = host, port, token, timeout
        self.callbacks: dict[str, object] = {}
        self.latest = {STATUS: b"{}", LIVE: b"{}"}
        self.snapshot_ready = {STATUS: asyncio.Event(), LIVE: asyncio.Event()}
        self.info_json = b"{}"
        self.payload_max = LAN_PAYLOAD_MAX
        self.reader = None
        self.writer = None
        self.task = None
        self.is_connected = False
        self.mtu_size = LAN_PAYLOAD_MAX + 3
        # Set when the device ends the session. Session registers on_close so a
        # pending or later frame() raises at once instead of waiting a deadline.
        self.close_error: ProtocolError | None = None
        self.on_close = None

    async def connect(self):
        self.reader, self.writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port), self.timeout)
        self.writer.write(b"OSS1" + self.token)
        await self.writer.drain()
        hello = await self._read_frame()
        if hello[0] == F_BYE:
            raise LinkClosedByDevice(decode_bye(hello))
        if hello[0] == F_ERROR:
            raise ProtocolError(f"LAN handshake refused code={hello[2]} ({ERRORS.get(hello[2], '?')})")
        if hello[0] != F_HELLO:
            raise ProtocolError(f"expected HELLO, got 0x{hello[0]:02x}")
        proto, self.payload_max = struct.unpack_from("<BH", hello, 1)
        self.info_json = bytes(hello[4:])
        self.is_connected = True
        self.task = asyncio.create_task(self._pump())
        print(f"LAN CONNECTED host={self.host} port={self.port} proto={proto} payload_max={self.payload_max}", flush=True)

    async def _read_frame(self) -> bytes:
        header = await asyncio.wait_for(self.reader.readexactly(2), self.timeout)
        (length,) = struct.unpack("<H", header)
        if length == 0 or length > LAN_PAYLOAD_MAX:
            raise ProtocolError(f"LAN frame length {length} out of range")
        return await asyncio.wait_for(self.reader.readexactly(length), self.timeout)

    async def _pump(self):
        try:
            while True:
                header = await self.reader.readexactly(2)
                (length,) = struct.unpack("<H", header)
                frame = await self.reader.readexactly(length)
                if frame and frame[0] == F_STATUS:
                    self._snapshot(STATUS, frame[1:])
                    self._deliver(STATUS, frame[1:])
                elif frame and frame[0] == F_LIVE:
                    self._snapshot(LIVE, frame[1:])
                    self._deliver(LIVE, frame[1:])
                elif frame and frame[0] == F_BYE:
                    # The device closes right after BYE, so stop reading here.
                    try:
                        self._closed(LinkClosedByDevice(decode_bye(frame)))
                    except ProtocolError as error:
                        self._closed(error)
                    return
                else:
                    self._deliver(RESPONSE, frame)
        except (asyncio.IncompleteReadError, ConnectionError):
            # Protocol 3 sends BYE before every device close. EOF without it is
            # reason unknown (the BYE was lost or the peer is older).
            self._closed(ProtocolError("LAN link closed by the device without BYE (reason unknown)"))
        except asyncio.CancelledError:
            self.is_connected = False

    def _closed(self, error: ProtocolError):
        self.is_connected = False
        if self.close_error is None:
            self.close_error = error
        if self.on_close:
            self.on_close(self.close_error)

    def _snapshot(self, uuid: str, data: bytes):
        self.latest[uuid] = bytes(data)
        if data and data != b"{}":
            self.snapshot_ready[uuid].set()
        else:
            self.snapshot_ready[uuid].clear()

    def _deliver(self, uuid: str, data: bytes):
        callback = self.callbacks.get(uuid)
        if callback:
            callback(None, bytearray(data))

    async def start_notify(self, uuid: str, callback):
        self.callbacks[uuid] = callback

    async def read_gatt_char(self, uuid: str):
        if uuid == INFO:
            return bytearray(self.info_json)
        # OP_STATUS produces a STATUS push, not a RESPONSE frame. Request one
        # even when cached, and never report the initial empty placeholder.
        # Session supplies the overall deadline; this also bounds direct calls.
        async with asyncio.timeout(self.timeout):
            if uuid == STATUS:
                self._snapshot(STATUS, b"{}")
                await self.write_gatt_char(CONTROL, bytes([OP_STATUS]))
            while self.latest[uuid] in (b"", b"{}"):
                await self.snapshot_ready[uuid].wait()
        return bytearray(self.latest[uuid])

    async def write_gatt_char(self, _uuid: str, data: bytes, response=True):
        if not self.is_connected:
            raise self.close_error or ProtocolError("LAN link closed")
        self.writer.write(struct.pack("<H", len(data)) + bytes(data))
        await self.writer.drain()

    async def disconnect(self):
        self.is_connected = False
        if self.task:
            self.task.cancel()
        if self.writer:
            self.writer.close()


class ProtocolError(RuntimeError):
    pass


class ListSinceUnsupported(ProtocolError):
    """The device answered LIST_SINCE with ERROR 11, so the client lists with LIST."""


class LinkClosedByDevice(ProtocolError):
    """The device sent BYE (0x4B) and closed the LAN session.

    Carries the decoded BYE fields. `reason` is the DisconnectReason name."""

    def __init__(self, bye: dict):
        self.bye = dict(bye)
        self.reason = bye["reason"]
        self.reason_code = bye["reason_code"]
        self.generation = bye["generation"]
        self.err = bye["err"]
        self.queued_frames = bye["queued_frames"]
        self.uptime_s = bye["uptime_s"]
        super().__init__(f"device closed the link: BYE reason={self.reason} generation={self.generation} "
                         f"err={self.err} queued_frames={self.queued_frames} uptime_s={self.uptime_s}")


class Session:
    """One connection; serializes control requests and collects response frames."""

    def __init__(self, client: BleakClient, verbose=False, *, read_timeout=20.0):
        if not math.isfinite(read_timeout) or read_timeout <= 0:
            raise ValueError("read timeout must be finite and positive")
        self.client = client
        # Response frames, or the error that ended the link (kept at the head
        # so every later frame() raises it without waiting).
        self.frames: asyncio.Queue[bytes | ProtocolError] = asyncio.Queue()
        self.verbose = verbose
        self.bytes_received = 0
        self.read_timeout = read_timeout

    async def start(self):
        await self.client.start_notify(RESPONSE, self._on_response)
        if hasattr(self.client, "on_close"):
            self.client.on_close = self._on_close
            if self.client.close_error is not None:
                self._on_close(self.client.close_error)

    def _on_response(self, _characteristic, data: bytearray):
        self.bytes_received += len(data)
        self.frames.put_nowait(bytes(data))

    def _on_close(self, error: ProtocolError):
        self.frames.put_nowait(error)

    async def request(self, payload: bytes):
        try:
            await self.client.write_gatt_char(CONTROL, payload, response=True)
        except ProtocolError:
            raise
        except Exception as error:
            # The normal unpaired BLE case. NimBLE refuses the Control write
            # at the ATT layer before the firmware sees it, so it gets the
            # pairing hint and no retry, the same as ERROR 14.
            if not ATT_AUTH_REFUSAL.search(str(error)):
                raise
            raise ProtocolError(f"Control write refused by ATT ({error}). {ERROR_HINTS[14]}") from error

    # Per frame deadline. On LAN a reply can wait up to the device's 15 s
    # slow-peer bound behind another phone, so callers there may raise it.
    frame_timeout = 15.0

    async def frame(self, timeout=None, *, raise_error=True) -> bytes:
        if timeout is None:
            timeout = self.frame_timeout
        try:
            data = await asyncio.wait_for(self.frames.get(), timeout)
        except asyncio.TimeoutError:
            raise ProtocolError("no response frame within deadline") from None
        if isinstance(data, ProtocolError):
            self.frames.put_nowait(data)  # the link stays closed for later reads
            raise data
        if not data:
            raise ProtocolError("empty frame")
        if data[0] == F_ERROR and raise_error:
            raise self._device_error(data)
        return data

    @staticmethod
    def _device_error(data: bytes) -> ProtocolError:
        op, code = data[1], data[2]
        hint = f". {ERROR_HINTS[code]}" if code in ERROR_HINTS else ""
        return ProtocolError(f"device ERROR op=0x{op:02x} code={code} ({ERRORS.get(code, '?')}) "
                             f"detail={data[3:].decode('utf-8', 'replace')!r}{hint}")

    async def read_char(self, uuid: str) -> bytearray:
        # Bleak 3.0.2's CoreBluetooth backend ignores read timeout kwargs and
        # its delegate expires after 20 s, including during first pairing.
        # Reads are idempotent: retry backend timeouts within one fixed deadline.
        # The outer deadline also bounds backends that never finish a read.
        try:
            async with asyncio.timeout(self.read_timeout):
                while True:
                    try:
                        return await self.client.read_gatt_char(uuid)
                    except TimeoutError:
                        await asyncio.sleep(0.1)
        except TimeoutError:
            raise ProtocolError(f"characteristic read {uuid} did not complete within "
                                f"{self.read_timeout:g}s") from None

    async def info(self) -> dict:
        return json.loads(bytes(await self.read_char(INFO)))

    async def status(self) -> dict:
        return json.loads(bytes(await self.read_char(STATUS)))

    async def list_files(self) -> tuple[dict[str, int], dict]:
        await self.request(bytes([OP_LIST]))
        listed: dict[str, int] = {}
        while True:
            data = await self.frame()
            if data[0] == F_FILE:
                (size,) = struct.unpack_from("<I", data, 1)
                name = device.safe_name(data[5:].decode("utf-8"))
                listed[name] = size
            elif data[0] == F_LIST_END:
                end = decode_list_end(data)
                if end["count"] != len(listed):
                    raise ProtocolError(f"LIST_END count {end['count']} != {len(listed)} received entries")
                return listed, end
            else:
                raise ProtocolError(f"unexpected frame 0x{data[0]:02x} during LIST")

    async def list_since(self, cursor: dict) -> tuple[dict[str, int], dict]:
        """LIST_SINCE (0x10) from `cursor`. Raises ListSinceUnsupported on ERROR 11."""
        await self.request(bytes([OP_LIST_SINCE]) + encode_cursor(cursor))
        listed: dict[str, int] = {}
        while True:
            data = await self.frame(raise_error=False)
            if data[0] == F_ERROR:
                if len(data) >= 3 and data[1] == OP_LIST_SINCE and data[2] == 11:
                    raise ListSinceUnsupported("device has no LIST_SINCE (ERROR 11)")
                raise self._device_error(data)
            if data[0] == F_FILE:
                (size,) = struct.unpack_from("<I", data, 1)
                name = device.safe_name(data[5:].decode("utf-8"))
                listed[name] = size
            elif data[0] == F_LIST_SINCE_END:
                end = decode_list_since_end(data)
                if end["count"] != len(listed):
                    raise ProtocolError(f"LIST_SINCE_END count {end['count']} != {len(listed)} received entries")
                return listed, end
            else:
                raise ProtocolError(f"unexpected frame 0x{data[0]:02x} during LIST_SINCE")

    async def open(self, name: str) -> tuple[int, int, int]:
        await self.request(bytes([OP_OPEN]) + name.encode("ascii"))
        data = await self.frame()
        if data[0] != F_OPENED:
            raise ProtocolError(f"expected OPENED, got 0x{data[0]:02x}")
        handle, size, crc = struct.unpack_from("<HII", data, 1)
        echoed = data[11:].decode("utf-8")
        if echoed != name:
            raise ProtocolError(f"OPENED echoed {echoed!r}, requested {name!r}")
        return handle, size, crc

    async def read_window(self, handle: int, offset: int, length: int) -> bytes:
        await self.request(struct.pack("<BHII", OP_READ, handle, offset, length))
        out = bytearray()
        while True:
            data = await self.frame()
            if data[0] == F_CHUNK:
                got_handle, got_offset = struct.unpack_from("<HI", data, 1)
                if got_handle != handle:
                    raise ProtocolError("CHUNK for a different handle")
                if got_offset != offset + len(out):
                    raise ProtocolError(f"CHUNK offset {got_offset}, expected {offset + len(out)}")
                out.extend(data[7:])
            elif data[0] == F_READ_END:
                got_handle, next_offset, status = struct.unpack_from("<HIB", data, 1)
                if status:
                    raise ProtocolError(f"READ_END status={status} ({ERRORS.get(status, '?')})")
                if next_offset != offset + len(out):
                    raise ProtocolError(f"READ_END next_offset {next_offset} != {offset + len(out)}")
                return bytes(out)
            else:
                raise ProtocolError(f"unexpected frame 0x{data[0]:02x} during READ")

    async def close(self, handle: int):
        await self.request(struct.pack("<BH", OP_CLOSE, handle))
        data = await self.frame()
        if data[0] != F_CLOSED:
            raise ProtocolError(f"expected CLOSED, got 0x{data[0]:02x}")

    async def fetch(self, name: str, max_read: int) -> tuple[bytes, dict]:
        started = time.monotonic()
        handle, size, expected_crc = await self.open(name)
        payload = bytearray()
        windows = 0
        while len(payload) < size:
            window = await self.read_window(handle, len(payload), min(max_read, size - len(payload)))
            if not window:
                raise ProtocolError("empty READ window before end of file")
            payload.extend(window)
            windows += 1
        await self.close(handle)
        elapsed = time.monotonic() - started
        crc = binascii.crc32(payload) & 0xFFFFFFFF
        if crc != expected_crc:
            raise ProtocolError(f"CRC32 mismatch computed {crc:08x} device {expected_crc:08x}")
        if payload[:4] != b"PAR1" or payload[-4:] != b"PAR1":
            raise ProtocolError("missing PAR1 magic")
        return bytes(payload), {"bytes": size, "windows": windows, "seconds": round(elapsed, 3),
                                "bytes_per_s": round(size / elapsed) if elapsed else None,
                                "crc32": f"{crc:08x}"}

    async def set_time(self, epoch_s: int) -> dict:
        await self.request(struct.pack("<Bq", OP_SET_TIME, epoch_s))
        data = await self.frame()
        if data[0] != F_TIME_SET:
            raise ProtocolError(f"expected TIME_SET, got 0x{data[0]:02x}")
        epoch, mono = struct.unpack_from("<qq", data, 1)
        return {"epoch_s": epoch, "monotonic_us": mono}

    async def flush(self) -> dict:
        await self.request(bytes([OP_FLUSH]))
        data = await self.frame(timeout=60)
        if data[0] != F_FLUSHED:
            raise ProtocolError(f"expected FLUSHED, got 0x{data[0]:02x}")
        rows, finalized = struct.unpack_from("<HI", data, 1)
        return {"rows": rows, "files_finalized": finalized}

    async def get_config(self, page: int = 0) -> dict:
        if not 0 <= page <= 4:
            raise ValueError("Configuration page must be 0..4")
        await self.request(bytes([OP_GET_CONFIG]) + (bytes([page]) if page else b""))
        return self._config_frame(await self.frame())

    async def set_config(self, pairs: list[str]) -> dict:
        for pair in pairs:
            if "=" not in pair:
                raise ProtocolError(f"expected key=value, got {pair!r}")
        await self.request(bytes([OP_SET_CONFIG]) + "\n".join(pairs).encode("utf-8"))
        return self._config_frame(await self.frame())

    @staticmethod
    def _config_frame(data: bytes) -> dict:
        if data[0] != F_CONFIG:
            raise ProtocolError(f"expected CONFIG, got 0x{data[0]:02x}")
        config = json.loads(bytes(data[2:]))
        config["reboot_required"] = bool(data[1] & 1)
        return config

    async def wifi_scan(self) -> list[dict]:
        await self.request(bytes([OP_WIFI_SCAN]))
        networks = []
        while True:
            data = await self.frame(timeout=30)
            if data[0] == F_WIFI_AP:
                rssi, auth, channel = struct.unpack_from("<bBB", data, 1)
                networks.append({"ssid": data[4:].decode("utf-8", "replace"), "rssi": rssi,
                                 "auth": WIFI_AUTH.get(auth, str(auth)), "channel": channel})
            elif data[0] == F_WIFI_SCAN_END:
                count, status = struct.unpack_from("<HB", data, 1)
                if status:
                    raise ProtocolError(f"WIFI_SCAN_END status={status} ({ERRORS.get(status, '?')})")
                if count != len(networks):
                    raise ProtocolError(f"WIFI_SCAN_END count {count} != {len(networks)} received")
                return networks
            else:
                raise ProtocolError(f"unexpected frame 0x{data[0]:02x} during WIFI_SCAN")

    async def get_token(self) -> tuple[int, bytes]:
        await self.request(bytes([OP_GET_TOKEN]))
        data = await self.frame()
        if data[0] != F_TOKEN:
            raise ProtocolError(f"expected TOKEN, got 0x{data[0]:02x}")
        (port,) = struct.unpack_from("<H", data, 1)
        token = bytes(data[3:])
        if len(token) != 32:
            raise ProtocolError(f"TOKEN carries {len(token)} bytes, expected 32")
        return port, token

    async def log_tail(self, max_bytes: int) -> tuple[str, dict]:
        await self.request(struct.pack("<BH", OP_LOG_TAIL, max_bytes))
        text = bytearray()
        while True:
            data = await self.frame()
            if data[0] == F_LOG:
                text.extend(data[1:])
            elif data[0] == F_LOG_END:
                total, returned = struct.unpack_from("<IH", data, 1)
                if returned != len(text):
                    raise ProtocolError(f"LOG_END returned {returned} != {len(text)} received")
                return text.decode("utf-8", "replace"), {"total_bytes_since_boot": total, "returned": returned}
            else:
                raise ProtocolError(f"unexpected frame 0x{data[0]:02x} during LOG_TAIL")

    async def get_diag(self) -> dict:
        await self.request(bytes([OP_GET_DIAG]))
        data = await self.frame()
        if data[0] != F_DIAG:
            raise ProtocolError(f"expected DIAG, got 0x{data[0]:02x}")
        return decode_diag(data)

    async def descriptor(self, length: int = 0xFFFF) -> bytes:
        """GET_DESCRIPTOR: the board template, one DESCRIPTOR_CHUNK per request.

        Requests the next missing offset until `total` bytes arrived. A lost
        or late reply only costs a re-request of the same offset, and a stale
        chunk for another offset is dropped. A changed `total` means another
        template, so the bytes are dropped and the download restarts from
        offset 0 once. A second change raises.
        """
        if not 1 <= length <= 0xFFFF:
            raise ValueError("length must be 1..65535")
        data = bytearray()
        total = None
        restarted = False
        while total is None or len(data) < total:
            offset = len(data)
            for attempt in range(DESCRIPTOR_TRIES):
                await self.request(struct.pack("<BIH", OP_GET_DESCRIPTOR, offset, length))
                try:
                    chunk = await self._descriptor_chunk(offset)
                except ProtocolError as error:
                    if "deadline" not in str(error) or attempt + 1 == DESCRIPTOR_TRIES:
                        raise
                    continue
                break
            chunk_total, payload = chunk
            if total is None:
                total = chunk_total
            elif chunk_total != total:
                if restarted:
                    raise ProtocolError(f"DESCRIPTOR_CHUNK total changed {total} -> {chunk_total} "
                                        "again after a restart")
                restarted = True
                data.clear()
                total = None
                continue
            if not payload or offset + len(payload) > total:
                raise ProtocolError(f"DESCRIPTOR_CHUNK of {len(payload)} bytes at {offset} "
                                    f"does not fit total {total}")
            data.extend(payload)
        return bytes(data)

    async def _descriptor_chunk(self, offset: int) -> tuple[int, bytes]:
        while True:
            data = await self.frame()
            if data[0] != F_DESCRIPTOR_CHUNK or len(data) < DESCRIPTOR_HEADER:
                raise ProtocolError(f"expected DESCRIPTOR_CHUNK, got 0x{data[0]:02x}")
            total, got = struct.unpack_from("<II", data, 1)
            if got == offset:
                return total, bytes(data[DESCRIPTOR_HEADER:])

    async def abort(self) -> dict:
        """ABORT. A running op first ends with ERROR code 16, then ABORTED names it."""
        await self.request(bytes([OP_ABORT]))
        interrupted = None
        while True:
            data = await self.frame(raise_error=False)
            if data[0] == F_ERROR and len(data) >= 3 and data[2] == 16:
                interrupted = data[1]
            elif data[0] == F_ERROR:
                raise self._device_error(data)
            elif data[0] == F_ABORTED:
                result = decode_aborted(data)
                result["error_aborted_op"] = interrupted
                return result
            elif data[0] in (F_FILE, F_CHUNK, F_LOG, F_WIFI_AP) and interrupted is None:
                continue  # in flight from the op being stopped
            else:
                raise ProtocolError(f"unexpected frame 0x{data[0]:02x} during ABORT")

    async def _ota_state(self, timeout=None) -> dict:
        data = await self.frame(timeout)
        if data[0] != F_OTA_STATE:
            raise ProtocolError(f"expected OTA_STATE, got 0x{data[0]:02x}")
        return decode_ota_state(data)

    async def ota_push(self, image: bytes, progress=None) -> dict:
        """OTA_BEGIN, OTA_WRITE in order, OTA_END. The device never reboots here.

        Every request waits for its STATE frame, so at most one chunk is in
        flight. A device ERROR has already aborted the upload on the device.
        """
        project = check_ota_image(image)
        digest = hashlib.sha256(image).digest()
        await self.request(bytes([OP_OTA_BEGIN]) + struct.pack("<I", len(image)) + digest)
        state = await self._ota_state()
        if state["phase"] != "receiving" or state["next"] != 0 or state["size"] != len(image):
            raise ProtocolError(f"unexpected OTA_BEGIN state {state}")
        chunk = state["max_chunk"]
        if chunk <= 0:
            raise ProtocolError("device offered no chunk size")
        offset = 0
        try:
            while offset < len(image):
                part = image[offset:offset + chunk]
                await self.request(bytes([OP_OTA_WRITE]) + struct.pack("<I", offset) + part)
                state = await self._ota_state()
                if state["next"] != offset + len(part):
                    raise ProtocolError(f"device at {state['next']}, expected {offset + len(part)}")
                offset = state["next"]
                if progress:
                    progress(offset, len(image))
            await self.request(bytes([OP_OTA_END]))
            # The device reads the whole image back for the SHA-256 and lets
            # ESP-IDF verify it, several seconds for a 1.6 MB image.
            state = await self._ota_state(timeout=120)
        except ProtocolError as error:
            if not str(error).startswith("device ERROR") and not isinstance(error, LinkClosedByDevice):
                try:
                    await self.ota_cancel()
                except ProtocolError:
                    pass
            raise
        if state["phase"] != "ready":
            raise ProtocolError(f"unexpected OTA_END state {state}")
        state["project"] = project
        state["sha256"] = digest.hex()
        return state

    async def ota_cancel(self) -> dict:
        await self.request(bytes([OP_OTA_CANCEL]))
        return await self._ota_state()

    async def reboot(self) -> dict:
        await self.request(bytes([OP_REBOOT]))
        data = await self.frame(timeout=60)
        if data[0] != F_REBOOTING:
            raise ProtocolError(f"expected REBOOTING, got 0x{data[0]:02x}")
        (delay_ms,) = struct.unpack_from("<H", data, 1)
        result = {"delay_ms": delay_ms}
        if isinstance(self.client, LanLink):
            # Hanging up first would record peer-closed instead of reboot.
            try:
                await self.frame(timeout=delay_ms / 1000 + 5)
            except LinkClosedByDevice as bye:
                result["bye"] = bye.reason
            except ProtocolError:
                pass
        return result


def advertises_sync_service(ad):
    # Protocol 2.1 puts service data in ADV and the UUID list in SCAN_RSP.
    # A scan callback may contain ADV alone; discovery must recognize either.
    return any(uuid.lower() == SERVICE for uuid in ad.service_uuids) or any(
        uuid.lower() == SERVICE for uuid in ad.service_data)


async def find(args):
    if getattr(args, "address", None):
        return args.address
    name = getattr(args, "name", None)
    found = await BleakScanner.find_device_by_filter(
        lambda d, ad: advertises_sync_service(ad)
        and (name is None or d.name == name or ad.local_name == name),
        timeout=args.timeout)
    if not found:
        raise SystemExit("no device advertising the OpenSensor Sync service; is it powered and unpaired-or-bonded to this host?")
    print(f"BLE FOUND name={found.name} address={found.address}", flush=True)
    return found


def load_token(args) -> bytes:
    if getattr(args, "token", None):
        return bytes.fromhex(args.token)
    if getattr(args, "token_file", None):
        return bytes.fromhex(Path(args.token_file).read_text().strip())
    raise SystemExit("--lan needs --token <hex> or --token-file <path> (fetch it with the BLE `token` command)")


async def connect(args):
    if getattr(args, "lan", None):
        host, _, port = args.lan.partition(":")
        client = LanLink(host, load_token(args), int(port) if port else LAN_PORT, timeout=args.timeout)
        await client.connect()
    else:
        target = await find(args)
        client = BleakClient(target, timeout=args.timeout)
        await client.connect()
        print(f"BLE CONNECTED mtu={client.mtu_size}", flush=True)
    session = Session(client, read_timeout=args.timeout)
    await session.start()
    return client, session


ADVERT_FLAGS = ["no_utc", "new_files", "sd", "lan", "fail", "clk_restored", "setup"]


def decode_advert(payload):
    """Advertising service data, protocol v2.1 (docs/shared/ble-sync-protocol.md, 'Advertising payload').

    Ten little-endian bytes under the service UUID: version, flags, fin u32, boot16 u16, two spare.
    Returns a dict, or None for an unknown version / short payload (a pre-v2.1 device has none)."""
    if payload is None or len(payload) < 8 or payload[0] != 1:
        return None
    flags = payload[1]
    return {
        "ver": payload[0],
        "flags": [name for bit, name in enumerate(ADVERT_FLAGS) if flags & (1 << bit)],
        "fin": int.from_bytes(payload[2:6], "little"),
        "boot16": f"{int.from_bytes(payload[6:8], 'little'):04x}",
    }


def reason_name(code: int) -> str:
    # A newer device may add codes. Show them as unknown, never reject them.
    return DISCONNECT_REASONS[code] if code < len(DISCONNECT_REASONS) else "unknown"


def link_name(link: int) -> str:
    return "ble" if link == 0 else "refused" if link == 0xFF else f"lan{link}"


def decode_list_end(frame: bytes) -> dict:
    """LIST_END (0x11), protocol 3: count u32, partials u16, sd_kib u32, sd_used_kib u32, 15 bytes.

    The 13-byte form with a u16 count is rejected, never read as a short count."""
    if len(frame) != 15 or frame[0] != F_LIST_END:
        raise ProtocolError(f"LIST_END must be 15 bytes, got {len(frame)}")
    count, partials, sd_kib, used_kib = struct.unpack_from("<IHII", frame, 1)
    return {"count": count, "partials": partials, "sd_kib": sd_kib, "sd_used_kib": used_kib}


ZERO_CURSOR = {"mount_epoch": 0, "day": 0, "boot": "00" * 16, "seq": 0}
LIST_SINCE_FULL, LIST_SINCE_UNSYNCED_FULL = 0x01, 0x02


def encode_cursor(cursor: dict) -> bytes:
    """The 32-byte LIST_SINCE cursor: mount_epoch u32, day u32, boot[16], seq u64."""
    boot = bytes.fromhex(cursor["boot"])
    if len(boot) != 16:
        raise ValueError("cursor boot must be 32 hex digits")
    return struct.pack("<II", cursor["mount_epoch"], cursor["day"]) + boot + struct.pack("<Q", cursor["seq"])


def decode_cursor(raw: bytes) -> dict:
    if len(raw) != 32:
        raise ProtocolError(f"cursor must be 32 bytes, got {len(raw)}")
    mount_epoch, day = struct.unpack_from("<II", raw)
    (seq,) = struct.unpack_from("<Q", raw, 24)
    return {"mount_epoch": mount_epoch, "day": day, "boot": raw[8:24].hex(), "seq": seq}


def decode_list_since_end(frame: bytes) -> dict:
    """LIST_SINCE_END (0x4E): the 15 LIST_END bytes, flags u8, next cursor, 48 bytes."""
    if len(frame) != 48 or frame[0] != F_LIST_SINCE_END:
        raise ProtocolError(f"LIST_SINCE_END must be 48 bytes, got {len(frame)}")
    count, partials, sd_kib, used_kib = struct.unpack_from("<IHII", frame, 1)
    flags = frame[15]
    return {"count": count, "partials": partials, "sd_kib": sd_kib, "sd_used_kib": used_kib,
            "full": bool(flags & LIST_SINCE_FULL), "unsynced_full": bool(flags & LIST_SINCE_UNSYNCED_FULL),
            "cursor": decode_cursor(frame[16:48])}


def read_cursors(path: Path) -> dict:
    """Every saved cursor by thing. A missing file is empty. A corrupt one
    warns and is treated as empty, so the next listing is a full one."""
    try:
        saved = json.loads(path.read_text())
    except FileNotFoundError:
        return {}
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        print(f"warning: ignoring unreadable cursor file {path} ({error}), listing everything",
              file=sys.stderr)
        return {}
    if not isinstance(saved, dict):
        print(f"warning: ignoring cursor file {path} that is not a JSON object, listing everything",
              file=sys.stderr)
        return {}
    return saved


def load_cursor(path: Path, thing: str) -> dict:
    """The saved cursor for `thing`, or the zero cursor, which lists everything."""
    cursor = read_cursors(path).get(thing)
    if cursor is None:
        return dict(ZERO_CURSOR)
    try:
        encode_cursor(cursor)
    except (TypeError, KeyError, ValueError, struct.error) as error:
        print(f"warning: ignoring bad cursor for thing {thing} in {path} ({error!r}), listing everything",
              file=sys.stderr)
        return dict(ZERO_CURSOR)
    return dict(cursor)


def save_cursor(path: Path, thing: str, cursor: dict) -> None:
    """Keeps one cursor per thing. The cursor carries its mount epoch."""
    saved = read_cursors(path)
    saved[thing] = cursor
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(saved, indent=1, sort_keys=True) + "\n")
    temporary.replace(path)


def decode_bye(frame: bytes) -> dict:
    """BYE (0x4B), protocol 3: reason u8, generation u32, err i16, queued_frames u16, uptime_s u32."""
    if len(frame) != 14 or frame[0] != F_BYE:
        raise ProtocolError(f"BYE must be 14 bytes, got {len(frame)}")
    reason, generation, err, queued, uptime = struct.unpack_from("<BIhHI", frame, 1)
    return {"reason": reason_name(reason), "reason_code": reason, "generation": generation,
            "err": err, "queued_frames": queued, "uptime_s": uptime}


def decode_event(raw: bytes) -> dict:
    """One 16-byte DisconnectEvent. uptime_s u32, generation u32, reason u8, link u8, err i16,
    queued_frames u16, boot_seq u16."""
    if len(raw) != 16:
        raise ProtocolError(f"DisconnectEvent must be 16 bytes, got {len(raw)}")
    uptime, generation, reason, link, err, queued, boot_seq = struct.unpack("<IIBBhHH", raw)
    return {"uptime_s": uptime, "generation": generation, "reason": reason_name(reason),
            "reason_code": reason, "link": link, "link_name": link_name(link), "err": err,
            "queued_frames": queued, "boot_seq": boot_seq}


def decode_diag(frame: bytes) -> dict:
    """DIAG (0x4C), protocol 3: boot_seq u16, counts[17] u16, n u8, then n 16-byte events oldest first."""
    if len(frame) < 38 or frame[0] != F_DIAG or len(frame) != 38 + 16 * frame[37]:
        raise ProtocolError(f"DIAG length {len(frame)} does not match its event count")
    (boot_seq,) = struct.unpack_from("<H", frame, 1)
    counts = list(struct.unpack_from("<17H", frame, 3))
    return {"boot_seq": boot_seq, "counts": counts,
            "events": [decode_event(frame[38 + 16 * i:54 + 16 * i]) for i in range(frame[37])]}


def decode_ota_state(frame: bytes) -> dict:
    if len(frame) != 16 or frame[0] != F_OTA_STATE:
        raise ProtocolError(f"bad OTA_STATE frame of {len(frame)} bytes")
    phase, nxt, size, slot, chunk = struct.unpack_from("<BIIIH", frame, 1)
    return {"phase": OTA_PHASES.get(phase, f"unknown-{phase}"), "next": nxt, "size": size,
            "slot_bytes": slot, "max_chunk": chunk}


def check_ota_image(image: bytes) -> str:
    """Local sanity check before any byte is sent. The device checks again."""
    if len(image) < OTA_HEADER_BYTES or image[0] != OTA_IMAGE_MAGIC:
        raise ProtocolError("not an ESP-IDF app image (.bin from the build directory)")
    if struct.unpack_from("<I", image, 32)[0] != OTA_DESC_MAGIC:
        raise ProtocolError("app description missing, use the app .bin, not the merged flash image")
    return image[80:112].split(b"\0", 1)[0].decode("ascii", "replace")


def decode_aborted(frame: bytes) -> dict:
    """ABORTED (0x4D): op u8, the opcode that was stopped, 0 when nothing was running."""
    if len(frame) != 2 or frame[0] != F_ABORTED:
        raise ProtocolError(f"ABORTED must be 2 bytes, got {len(frame)}")
    return {"op": frame[1]}


async def cmd_scan(args):
    seen = {}

    def on_adv(d, ad):
        advert = decode_advert(ad.service_data.get(SERVICE))
        if advertises_sync_service(ad):
            seen[d.address] = (d.name, ad.rssi, advert)

    async with BleakScanner(on_adv):
        await asyncio.sleep(args.seconds)
    for address, (name, rssi, advert) in seen.items():
        extra = ""
        if advert is not None:
            extra = f" adv_ver={advert['ver']} flags={','.join(advert['flags']) or '-'} fin={advert['fin']} boot16={advert['boot16']}"
        print(f"BLE DEVICE name={name} address={address} rssi={rssi}{extra}")
    print(f"BLE SCAN END devices={len(seen)}")


async def cmd_info(args):
    client, session = await connect(args)
    try:
        print(json.dumps({"info": await session.info(), "status": await session.status()}, indent=2))
    finally:
        await client.disconnect()


async def cmd_live(args):
    client, session = await connect(args)
    deadline = time.monotonic() + args.seconds

    def show(kind):
        def handler(_c, data):
            print(f"{kind} {bytes(data).decode('utf-8', 'replace')}", flush=True)
        return handler

    try:
        print(f"LIVE {bytes(await session.read_char(LIVE)).decode('utf-8', 'replace')}")
        await client.start_notify(LIVE, show("LIVE"))
        await client.start_notify(STATUS, show("STATUS"))
        while time.monotonic() < deadline and client.is_connected:
            await asyncio.sleep(0.5)
        if getattr(client, "close_error", None) is not None:
            print(f"CLOSED {client.close_error}", flush=True)
    finally:
        await client.disconnect()


async def cmd_list(args):
    client, session = await connect(args)
    try:
        if args.since:
            thing = (await session.info()).get("thing", "")
            cursor = load_cursor(args.since, thing)
            try:
                listed, end = await session.list_since(cursor)
            except ListSinceUnsupported:
                print("BLE LIST_SINCE unsupported, falling back to LIST", flush=True)
                listed, end = await session.list_files()
            else:
                save_cursor(args.since, thing, end["cursor"])
        else:
            listed, end = await session.list_files()
        for name, size in sorted(listed.items()):
            print(f"BLE FILE name={name} bytes={size}")
        print("BLE LIST END " + " ".join(f"{k}={v}" for k, v in end.items()))
    finally:
        await client.disconnect()


def publish(payload: bytes, name: str, out: Path, transfer: dict, source_info: dict) -> dict:
    relative = name if name.startswith("legacy-parquet/") else "output/" + name
    summary = device.save_verified(payload, relative, out)
    return {"path": relative, "source": source_info.get("_transport", "ble") + "-readback", "bytes": len(payload), "rows": summary["rows"],
            "crc32": summary["crc32"], "sha256": hashlib.sha256(payload).hexdigest(),
            "readers_match": summary["readers_match"], "transfer": transfer,
            "thing": source_info.get("thing"), "device": source_info.get("dev")}


async def cmd_fetch(args):
    client, session = await connect(args)
    try:
        info = await session.info()
        info["_transport"] = "lan" if getattr(args, "lan", None) else "ble"
        payload, transfer = await session.fetch(device.safe_name(args.name), info.get("max_read", 4096))
        record = publish(payload, args.name, args.out, transfer, info)
        print(json.dumps(record), flush=True)
    finally:
        await client.disconnect()


async def cmd_sync(args):
    if (args.out / "manifest.json").exists():
        raise SystemExit("choose a new output directory; manifest already exists")
    client, session = await connect(args)
    records = {}
    started = time.monotonic()
    try:
        info = await session.info()
        info["_transport"] = "lan" if getattr(args, "lan", None) else "ble"
        listed, end = await session.list_files()
        print(f"LIST END count={end['count']} partials={end['partials']}", flush=True)
        names = sorted(listed)
        if args.limit:
            names = names[: args.limit]
        for name in names:
            payload, transfer = await session.fetch(name, info.get("max_read", 4096))
            if len(payload) != listed[name]:
                raise ProtocolError(f"listed {listed[name]} bytes, transferred {len(payload)}: {name}")
            records[name] = publish(payload, name, args.out, transfer, info)
            print(json.dumps(records[name]), flush=True)
    finally:
        await client.disconnect()
    total_bytes = sum(r["bytes"] for r in records.values())
    elapsed = time.monotonic() - started
    report = {"completed_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
              "transport": "lan" if getattr(args, "lan", None) else "ble",
              "protocol": info.get("proto"), "device": {k: v for k, v in info.items() if not k.startswith("_")}, "listing": end, "files": list(records.values()),
              "total_files": len(records), "total_bytes": total_bytes,
              "total_rows": sum(r["rows"] for r in records.values()),
              "wall_seconds": round(elapsed, 1), "bytes_per_s_incl_overhead": round(total_bytes / elapsed) if elapsed else None,
              "device_deletions": False, "flush_requested": False}
    args.out.mkdir(parents=True, exist_ok=True)
    with (args.out / "manifest.json").open("x") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(f"PASS {len(records)} files / {total_bytes} bytes in {elapsed:.1f}s -> {args.out}", flush=True)


async def cmd_time(args):
    client, session = await connect(args)
    try:
        epoch = int(time.time())
        print(json.dumps({"sent_epoch_s": epoch, "result": await session.set_time(epoch)}))
        await asyncio.sleep(1)
        print(json.dumps({"status": await session.status()}))
    finally:
        await client.disconnect()


async def cmd_flush(args):
    client, session = await connect(args)
    try:
        print(json.dumps({"flushed": await session.flush()}))
    finally:
        await client.disconnect()


async def cmd_config(args):
    client, session = await connect(args)
    try:
        print(json.dumps(await session.get_config(args.page), indent=2))
    finally:
        await client.disconnect()


async def cmd_set(args):
    client, session = await connect(args)
    try:
        print(json.dumps(await session.set_config(args.pairs), indent=2))
    finally:
        await client.disconnect()


async def cmd_wifi_scan(args):
    client, session = await connect(args)
    try:
        networks = await session.wifi_scan()
        for network in sorted(networks, key=lambda n: -n["rssi"]):
            print(f"WIFI AP ssid={network['ssid']!r} rssi={network['rssi']} auth={network['auth']} channel={network['channel']}")
        print(f"WIFI SCAN END count={len(networks)}")
    finally:
        await client.disconnect()


async def cmd_token(args):
    client, session = await connect(args)
    try:
        port, token = await session.get_token()
        if args.save:
            args.save.parent.mkdir(parents=True, exist_ok=True)
            args.save.write_text(token.hex() + "\n")
            print(f"LAN TOKEN port={port} saved={args.save} (bearer secret: keep it out of docs and logs)")
        else:
            print(f"LAN TOKEN port={port} token={token.hex()}")
    finally:
        await client.disconnect()


async def cmd_log(args):
    client, session = await connect(args)
    try:
        text, meta = await session.log_tail(args.bytes)
        sys.stdout.write(text)
        if not text.endswith("\n"):
            sys.stdout.write("\n")
        print("LOG END " + " ".join(f"{k}={v}" for k, v in meta.items()))
    finally:
        await client.disconnect()


async def cmd_diag(args):
    client, session = await connect(args)
    try:
        diag = await session.get_diag()
        counts = " ".join(f"{reason_name(code)}={n}" for code, n in enumerate(diag["counts"]) if n)
        print(f"DIAG boot_seq={diag['boot_seq']} counts={counts or '-'}")
        for event in diag["events"]:
            print(f"DIAG EVENT boot_seq={event['boot_seq']} uptime_s={event['uptime_s']} "
                  f"link={event['link_name']} generation={event['generation']} reason={event['reason']} "
                  f"err={event['err']} queued_frames={event['queued_frames']}")
        print(f"DIAG END events={len(diag['events'])}")
    finally:
        await client.disconnect()


def check_descriptor(data: bytes, info: dict, expect: str | None = None) -> str:
    """desc is the SHA-256 of these exact bytes. info carries its first 16 hex."""
    digest = hashlib.sha256(data).hexdigest()
    prefix = info.get("desc")
    if not isinstance(prefix, str) or len(prefix) != 16:
        raise ProtocolError(f"info has no 16 hex desc ({prefix!r})")
    if not digest.startswith(prefix):
        raise ProtocolError(f"descriptor sha256 {digest} does not start with info desc {prefix}")
    if expect is not None and digest != expect.lower():
        raise ProtocolError(f"descriptor sha256 {digest} != expected {expect}")
    return digest


async def cmd_descriptor(args):
    client, session = await connect(args)
    try:
        info = await session.info()
        data = await session.descriptor()
    finally:
        await client.disconnect()
    digest = check_descriptor(data, info, args.expect)
    # Parse only to prove it is JSON. The file keeps the exact device bytes.
    json.loads(data)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(data)
    print(f"DESCRIPTOR bytes={len(data)} sha256={digest} desc={info['desc']} out={args.out}")


async def cmd_abort(args):
    client, session = await connect(args)
    try:
        print(json.dumps({"aborted": await session.abort()}))
    finally:
        await client.disconnect()


def lan_only(args, command: str) -> None:
    if not getattr(args, "lan", None):
        raise SystemExit(f"{command} runs over LAN only, pass --lan HOST and a token")


async def cmd_ota(args):
    lan_only(args, "ota")
    image = args.image.read_bytes()
    check_ota_image(image)
    client, session = await connect(args)
    try:
        started = time.monotonic()
        last = [0.0]

        def progress(done, total):
            now = time.monotonic()
            if now - last[0] >= 5 or done == total:
                last[0] = now
                print(f"OTA PROGRESS bytes={done}/{total}", flush=True)

        result = await session.ota_push(image, progress)
        result["seconds"] = round(time.monotonic() - started, 1)
        print(json.dumps({"ota": result}), flush=True)
        if args.reboot:
            print(json.dumps({"rebooting": await session.reboot()}))
        else:
            print("OTA staged. Run `reboot` to boot it, or `ota-cancel` to keep the running app.")
    finally:
        await client.disconnect()


async def cmd_ota_cancel(args):
    lan_only(args, "ota-cancel")
    client, session = await connect(args)
    try:
        print(json.dumps({"ota": await session.ota_cancel()}))
    finally:
        await client.disconnect()


async def cmd_reboot(args):
    client, session = await connect(args)
    try:
        print(json.dumps({"rebooting": await session.reboot()}))
    finally:
        await client.disconnect()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--name", help="advertised local name, e.g. OS-6b40")
    parser.add_argument("--address", help="skip scanning and connect to this address/UUID")
    parser.add_argument("--timeout", type=device.positive_seconds, default=20.0,
                        help="seconds per discovery, connection and characteristic read; "
                             "allow time for the system pairing dialog on first access")
    parser.add_argument("--lan", metavar="HOST[:PORT]", help="use the LAN transport (needs a token) instead of BLE")
    parser.add_argument("--token", help="LAN token as 64 hex characters")
    parser.add_argument("--token-file", help="file holding the LAN token hex (see `token --save`)")
    sub = parser.add_subparsers(dest="command", required=True)
    scan = sub.add_parser("scan")
    scan.add_argument("--seconds", type=float, default=8.0)
    scan.set_defaults(run=cmd_scan)
    sub.add_parser("info").set_defaults(run=cmd_info)
    sub.add_parser("status").set_defaults(run=cmd_info)
    live = sub.add_parser("live")
    live.add_argument("--seconds", type=float, default=35.0)
    live.set_defaults(run=cmd_live)
    listing = sub.add_parser("list")
    listing.add_argument("--since", type=Path, metavar="CURSOR_FILE",
                         help="list with LIST_SINCE from the thing's cursor in this JSON file, then save "
                              "the new one. A missing file or thing lists everything")
    listing.set_defaults(run=cmd_list)
    fetch = sub.add_parser("fetch")
    fetch.add_argument("name")
    fetch.add_argument("--out", type=Path, required=True)
    fetch.set_defaults(run=cmd_fetch)
    sync = sub.add_parser("sync")
    sync.add_argument("--out", type=Path, required=True)
    sync.add_argument("--limit", type=int, default=0, help="only the first N listed files (bench)")
    sync.set_defaults(run=cmd_sync)
    sub.add_parser("time", help="send host UTC to the device (explicit write)").set_defaults(run=cmd_time)
    sub.add_parser("flush", help="finalize buffered rows on the device (explicit write)").set_defaults(run=cmd_flush)
    config_parser = sub.add_parser("config", help="GET_CONFIG")
    config_parser.add_argument("--page", type=int, choices=range(5), default=0,
                               help="0 legacy, 1 location, 2 NTP policy, 3/4 NTP server")
    config_parser.set_defaults(run=cmd_config)
    setter = sub.add_parser("set", help="SET_CONFIG key=value ... (explicit write)")
    setter.add_argument("pairs", nargs="+", metavar="key=value")
    setter.set_defaults(run=cmd_set)
    sub.add_parser("wifi-scan", help="list Wi-Fi networks the device can see").set_defaults(run=cmd_wifi_scan)
    token = sub.add_parser("token", help="fetch the LAN token (BLE only)")
    token.add_argument("--save", type=Path, help="write the hex token to this file instead of printing it")
    token.set_defaults(run=cmd_token)
    log = sub.add_parser("log", help="LOG_TAIL: the device's recent serial log")
    log.add_argument("--bytes", type=int, default=8192)
    log.set_defaults(run=cmd_log)
    sub.add_parser("diag", help="GET_DIAG: persistent disconnect counters and recent events").set_defaults(run=cmd_diag)
    descriptor = sub.add_parser("descriptor", help="GET_DESCRIPTOR: download the board template, "
                                                   "check its sha256 against info desc")
    descriptor.add_argument("--out", type=Path, required=True, help="file for the exact template bytes")
    descriptor.add_argument("--expect", metavar="SHA256", help="full 64 hex desc to require")
    descriptor.set_defaults(run=cmd_descriptor)
    sub.add_parser("abort", help="ABORT the op the device is running, if any").set_defaults(run=cmd_abort)
    sub.add_parser("reboot", help="reboot the device after it finalizes its RAM batch (explicit)").set_defaults(run=cmd_reboot)
    ota = sub.add_parser("ota", help="LAN OTA: stage an app .bin in the inactive slot (explicit write)")
    ota.add_argument("image", type=Path, help="the app .bin of a matching board build")
    ota.add_argument("--reboot", action="store_true", help="send REBOOT once the image is staged")
    ota.set_defaults(run=cmd_ota)
    sub.add_parser("ota-cancel", help="LAN OTA: drop a partial or staged image").set_defaults(run=cmd_ota_cancel)
    args = parser.parse_args()
    if getattr(args, "out", None) is not None and "build" in args.out.parts:
        parser.error("use artifacts/, not a build directory that firmware tools may clean")
    try:
        asyncio.run(args.run(args))
    except ProtocolError as error:
        print(f"FAIL {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
