#!/usr/bin/env python3
"""Tiny RTMP play server for OpenLiveReplay E2E tests.

It serves the media tags from a generated FLV file to RTMP clients. The goal is
a deterministic test fixture, not a general-purpose RTMP server.
"""

from __future__ import annotations

import argparse
import os
import re
import socket
import ssl
import struct
import sys
import time


RTMP_VERSION = 3
DEFAULT_CHUNK_SIZE = 128
OUT_CHUNK_SIZE = 65536
STREAM_ID = 1


def redact_query_values(value: str) -> str:
    return re.sub(r"([?&][^=\s'\"]+=)[^&\s'\"]+", r"\1<redacted>", value)


def u24(value: int) -> bytes:
    return bytes(((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF))


def read_exact(conn: socket.socket, size: int) -> bytes:
    chunks = []
    remaining = size
    while remaining > 0:
        data = conn.recv(remaining)
        if not data:
            raise EOFError("client disconnected")
        chunks.append(data)
        remaining -= len(data)
    return b"".join(chunks)


def amf0_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return b"\x02" + struct.pack(">H", len(encoded)) + encoded


def amf0_number(value: float) -> bytes:
    return b"\x00" + struct.pack(">d", value)


def amf0_bool(value: bool) -> bytes:
    return b"\x01" + (b"\x01" if value else b"\x00")


def amf0_null() -> bytes:
    return b"\x05"


def amf0_object(values: dict[str, object]) -> bytes:
    out = bytearray(b"\x03")
    for key, value in values.items():
        key_bytes = key.encode("utf-8")
        out.extend(struct.pack(">H", len(key_bytes)))
        out.extend(key_bytes)
        out.extend(amf0_value(value))
    out.extend(b"\x00\x00\x09")
    return bytes(out)


def amf0_value(value: object) -> bytes:
    if isinstance(value, bool):
        return amf0_bool(value)
    if isinstance(value, (int, float)):
        return amf0_number(float(value))
    if isinstance(value, str):
        return amf0_string(value)
    if value is None:
        return amf0_null()
    if isinstance(value, dict):
        return amf0_object(value)
    raise TypeError(f"unsupported AMF0 value: {value!r}")


def read_amf0_string(data: bytes, offset: int) -> tuple[str, int]:
    if offset >= len(data) or data[offset] != 0x02:
        raise ValueError("expected AMF0 string")
    size = struct.unpack(">H", data[offset + 1 : offset + 3])[0]
    start = offset + 3
    end = start + size
    return data[start:end].decode("utf-8", errors="replace"), end


def read_amf0_number(data: bytes, offset: int) -> tuple[float, int]:
    if offset >= len(data) or data[offset] != 0x00:
        raise ValueError("expected AMF0 number")
    return struct.unpack(">d", data[offset + 1 : offset + 9])[0], offset + 9


def parse_command(data: bytes) -> tuple[str, float]:
    name, offset = read_amf0_string(data, 0)
    transaction_id, _ = read_amf0_number(data, offset)
    return name, transaction_id


class RtmpReader:
    def __init__(self, conn: socket.socket) -> None:
        self.conn = conn
        self.chunk_size = DEFAULT_CHUNK_SIZE
        self.headers: dict[int, dict[str, int]] = {}
        self.inflight: dict[int, bytearray] = {}

    def read_message(self) -> tuple[int, int, int, bytes]:
        while True:
            first = read_exact(self.conn, 1)[0]
            fmt = first >> 6
            csid = first & 0x3F
            if csid == 0:
                csid = 64 + read_exact(self.conn, 1)[0]
            elif csid == 1:
                ext = read_exact(self.conn, 2)
                csid = 64 + ext[0] + 256 * ext[1]

            previous = self.headers.get(csid, {})
            header = dict(previous)
            extended_timestamp = False
            if fmt == 0:
                raw = read_exact(self.conn, 11)
                timestamp = int.from_bytes(raw[0:3], "big")
                header = {
                    "timestamp": timestamp,
                    "length": int.from_bytes(raw[3:6], "big"),
                    "type": raw[6],
                    "stream": int.from_bytes(raw[7:11], "little"),
                }
                extended_timestamp = timestamp == 0xFFFFFF
            elif fmt == 1:
                raw = read_exact(self.conn, 7)
                delta = int.from_bytes(raw[0:3], "big")
                timestamp = int(previous.get("timestamp", 0)) + delta
                header.update(
                    {
                        "timestamp": timestamp,
                        "length": int.from_bytes(raw[3:6], "big"),
                        "type": raw[6],
                    }
                )
                extended_timestamp = delta == 0xFFFFFF
            elif fmt == 2:
                raw = read_exact(self.conn, 3)
                delta = int.from_bytes(raw, "big")
                header["timestamp"] = int(previous.get("timestamp", 0)) + delta
                extended_timestamp = delta == 0xFFFFFF
            elif not header:
                raise ValueError("fmt=3 without previous header")

            if extended_timestamp:
                header["timestamp"] = int.from_bytes(read_exact(self.conn, 4), "big")

            self.headers[csid] = header
            expected = int(header["length"])
            payload = self.inflight.setdefault(csid, bytearray())
            to_read = min(self.chunk_size, expected - len(payload))
            payload.extend(read_exact(self.conn, to_read))
            if len(payload) < expected:
                continue

            data = bytes(payload)
            del self.inflight[csid]
            message_type = int(header["type"])
            if message_type == 1 and len(data) >= 4:
                self.chunk_size = int.from_bytes(data[:4], "big")
            return message_type, int(header["stream"]), int(header["timestamp"]), data


class RtmpWriter:
    def __init__(
        self,
        conn: socket.socket,
        chunk_size: int = OUT_CHUNK_SIZE,
        write_fragment: int = 0,
        write_fragment_delay: float = 0.0,
    ) -> None:
        self.conn = conn
        self.chunk_size = chunk_size
        self.write_fragment = write_fragment
        self.write_fragment_delay = write_fragment_delay

    def send_bytes(self, data: bytes) -> None:
        if self.write_fragment <= 0:
            self.conn.sendall(data)
            return
        for offset in range(0, len(data), self.write_fragment):
            self.conn.sendall(data[offset : offset + self.write_fragment])
            if self.write_fragment_delay > 0:
                time.sleep(self.write_fragment_delay)

    def send_message(self, csid: int, message_type: int, stream_id: int, timestamp: int, payload: bytes) -> None:
        header_ts = min(timestamp, 0xFFFFFF)
        header = bytearray()
        header.append(csid & 0x3F)
        header.extend(u24(header_ts))
        header.extend(u24(len(payload)))
        header.append(message_type)
        header.extend(struct.pack("<I", stream_id))
        if timestamp >= 0xFFFFFF:
            header.extend(struct.pack(">I", timestamp))

        offset = 0
        first = True
        while offset < len(payload) or (first and not payload):
            chunk = payload[offset : offset + self.chunk_size]
            if first:
                self.send_bytes(bytes(header) + chunk)
                first = False
            else:
                continuation = bytes(((3 << 6) | (csid & 0x3F),))
                if timestamp >= 0xFFFFFF:
                    continuation += struct.pack(">I", timestamp)
                self.send_bytes(continuation + chunk)
            offset += len(chunk)

    def set_chunk_size(self) -> None:
        self.send_message(2, 1, 0, 0, struct.pack(">I", self.chunk_size))

    def window_ack(self) -> None:
        self.send_message(2, 5, 0, 0, struct.pack(">I", 5_000_000))

    def peer_bandwidth(self) -> None:
        self.send_message(2, 6, 0, 0, struct.pack(">IB", 5_000_000, 2))

    def user_control(self, event_type: int, event_data: int) -> None:
        self.send_message(2, 4, 0, 0, struct.pack(">HI", event_type, event_data))

    def command(self, timestamp: int, *values: object) -> None:
        payload = b"".join(amf0_value(value) for value in values)
        self.send_message(3, 20, STREAM_ID, timestamp, payload)


def flv_tags(path: str) -> list[tuple[int, int, bytes]]:
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 13 or data[:3] != b"FLV":
        raise ValueError(f"{path} is not an FLV file")

    offset = 9 + 4
    tags: list[tuple[int, int, bytes]] = []
    while offset + 15 <= len(data):
        tag_type = data[offset]
        size = int.from_bytes(data[offset + 1 : offset + 4], "big")
        timestamp = (
            int.from_bytes(data[offset + 4 : offset + 7], "big")
            | (data[offset + 7] << 24)
        )
        payload_start = offset + 11
        payload_end = payload_start + size
        if payload_end + 4 > len(data):
            break
        if tag_type in (8, 9, 18):
            tags.append((tag_type, timestamp, data[payload_start:payload_end]))
        offset = payload_end + 4
    return tags


def split_annexb(data: bytes) -> list[bytes]:
    starts: list[tuple[int, int]] = []
    i = 0
    while i + 3 <= len(data):
        if data[i : i + 3] == b"\x00\x00\x01":
            starts.append((i, 3))
            i += 3
        elif i + 4 <= len(data) and data[i : i + 4] == b"\x00\x00\x00\x01":
            starts.append((i, 4))
            i += 4
        else:
            i += 1

    nals: list[bytes] = []
    for index, (start, prefix_size) in enumerate(starts):
        nal_start = start + prefix_size
        nal_end = starts[index + 1][0] if index + 1 < len(starts) else len(data)
        while nal_end > nal_start and data[nal_end - 1] == 0:
            nal_end -= 1
        if nal_end > nal_start:
            nals.append(data[nal_start:nal_end])
    return nals


def hevc_nal_type(nal: bytes) -> int:
    if len(nal) < 2:
        raise ValueError("HEVC NAL unit is too short")
    return (nal[0] >> 1) & 0x3F


def hevc_is_vcl(nal_type: int) -> bool:
    return 0 <= nal_type <= 31


def rbsp_from_payload(payload: bytes) -> bytes:
    out = bytearray()
    zeroes = 0
    for byte in payload:
        if zeroes >= 2 and byte == 0x03:
            zeroes = 0
            continue
        out.append(byte)
        if byte == 0:
            zeroes += 1
        else:
            zeroes = 0
    return bytes(out)


def hevc_starts_new_picture(nal: bytes) -> bool:
    if len(nal) < 3:
        return False
    rbsp = rbsp_from_payload(nal[2:])
    return bool(rbsp) and (rbsp[0] & 0x80) != 0


def build_hvcc(vps: list[bytes], sps: list[bytes], pps: list[bytes]) -> bytes:
    if not vps or not sps or not pps:
        raise ValueError("HEVC Annex B source lacks VPS/SPS/PPS")

    out = bytearray(23)
    out[0] = 1
    out[21] = 0xFC | 3  # four-byte NAL lengths
    out[22] = 3
    for nal_type, nals in ((32, vps), (33, sps), (34, pps)):
        out.append(0x80 | nal_type)
        out.extend(struct.pack(">H", len(nals)))
        for nal in nals:
            out.extend(struct.pack(">H", len(nal)))
            out.extend(nal)
    return bytes(out)


def length_prefixed(nals: list[bytes]) -> bytes:
    out = bytearray()
    for nal in nals:
        out.extend(struct.pack(">I", len(nal)))
        out.extend(nal)
    return bytes(out)


def enhanced_hevc_tags(path: str) -> list[tuple[int, int, bytes]]:
    with open(path, "rb") as f:
        nals = split_annexb(f.read())
    if not nals:
        raise ValueError("HEVC Annex B source contains no NAL units")

    vps: list[bytes] = []
    sps: list[bytes] = []
    pps: list[bytes] = []
    frames: list[list[bytes]] = []
    current: list[bytes] = []
    prefix: list[bytes] = []

    for nal in nals:
        nal_type = hevc_nal_type(nal)
        if nal_type == 32:
            vps.append(nal)
            continue
        if nal_type == 33:
            sps.append(nal)
            continue
        if nal_type == 34:
            pps.append(nal)
            continue

        if hevc_is_vcl(nal_type):
            if current and hevc_starts_new_picture(nal):
                frames.append(current)
                current = []
            if prefix:
                current.extend(prefix)
                prefix = []
            current.append(nal)
        else:
            prefix.append(nal)

    if current:
        frames.append(current)
    if not frames:
        raise ValueError("HEVC Annex B source contains no coded access units")

    tags: list[tuple[int, int, bytes]] = []
    tags.append((9, 0, bytes((0x80 | 0,)) + b"hvc1" + build_hvcc(vps, sps, pps)))
    for index, frame in enumerate(frames):
        payload = bytes((0x80 | 1,)) + b"hvc1" + u24(0) + length_prefixed(frame)
        tags.append((9, index * 33, payload))
    return tags


def loop_tags(tags: list[tuple[int, int, bytes]], duration_ms: int) -> list[tuple[int, int, bytes]]:
    if duration_ms <= 0 or not tags:
        return tags
    source_duration_ms = max(timestamp for _tag_type, timestamp, _payload in tags) + 33
    if source_duration_ms <= 0 or source_duration_ms >= duration_ms:
        return tags

    looped: list[tuple[int, int, bytes]] = []
    offset_ms = 0
    while offset_ms < duration_ms:
        for tag_type, timestamp, payload in tags:
            shifted = offset_ms + timestamp
            if shifted > duration_ms:
                break
            looped.append((tag_type, shifted, payload))
        offset_ms += source_duration_ms
    return looped


def run_server(args: argparse.Namespace) -> None:
    tags: list[tuple[int, int, bytes]] = []
    if args.enhanced_hevc:
        if not args.hevc_annexb_source:
            raise ValueError("--enhanced-hevc requires --hevc-annexb-source")
        tags = enhanced_hevc_tags(args.hevc_annexb_source)
    elif args.flv:
        tags = flv_tags(args.flv)
    if args.loop_duration > 0:
        tags = loop_tags(tags, int(args.loop_duration * 1000))
    if not args.idle_after_play and not tags:
        raise ValueError("FLV fixture contains no RTMP media tags")

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", args.port))
    listener.listen(max(1, args.max_clients))
    listener.settimeout(args.accept_timeout)
    print(f"READY port={args.port}", flush=True)

    tls_context: ssl.SSLContext | None = None
    if args.tls_cert or args.tls_key:
        if not args.tls_cert or not args.tls_key:
            raise ValueError("--tls-cert and --tls-key must be supplied together")
        tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls_context.load_cert_chain(args.tls_cert, args.tls_key)

    def send_reconnect_request(writer: RtmpWriter, timestamp: int) -> None:
        writer.command(
            timestamp,
            "onStatus",
            0,
            None,
            {
                "level": "status",
                "code": "NetConnection.Connect.ReconnectRequest",
                "description": "Reconnect requested.",
            },
        )

    def serve_client(conn: socket.socket, client_index: int) -> None:
        if tls_context is not None:
            conn = tls_context.wrap_socket(conn, server_side=True)
        with conn:
            conn.settimeout(args.io_timeout)
            c0c1 = read_exact(conn, 1537)
            if c0c1[0] != RTMP_VERSION:
                raise ValueError("unsupported RTMP version")
            s1 = struct.pack(">II", int(time.time()), 0) + os.urandom(1528)
            conn.sendall(bytes((RTMP_VERSION,)) + s1 + c0c1[1:])
            read_exact(conn, 1536)

            reader = RtmpReader(conn)
            writer = RtmpWriter(
                conn,
                args.out_chunk_size,
                args.write_fragment,
                args.write_fragment_delay,
            )
            writer.set_chunk_size()
            writer.window_ack()
            writer.peer_bandwidth()

            saw_play = False
            playpath = ""
            while not saw_play:
                message_type, _stream, _timestamp, payload = reader.read_message()
                if message_type != 20:
                    continue
                name, transaction_id = parse_command(payload)
                if name == "connect":
                    writer.command(
                        0,
                        "_result",
                        transaction_id,
                        {"fmsVer": "FMS/3,5,7,7009", "capabilities": 31},
                        {
                            "level": "status",
                            "code": "NetConnection.Connect.Success",
                            "description": "Connection succeeded.",
                            "objectEncoding": 0,
                        },
                    )
                elif name == "createStream":
                    writer.command(0, "_result", transaction_id, None, STREAM_ID)
                elif name == "play":
                    try:
                        playpath, _ = read_amf0_string(
                            payload, len(amf0_string("play")) + 9 + 1
                        )
                    except Exception as exc:
                        if args.expect_play_path:
                            raise ValueError("play command omitted expected play path") from exc
                        playpath = "stream"
                    if args.expect_play_path and playpath != args.expect_play_path:
                        raise ValueError(
                            "play path mismatch: expected "
                            f"{redact_query_values(args.expect_play_path)!r}, got "
                            f"{redact_query_values(playpath)!r}"
                        )
                    if args.require_play_query and "?" not in playpath:
                        raise ValueError("play path omitted required query string")
                    writer.user_control(0, STREAM_ID)
                    writer.command(
                        0,
                        "onStatus",
                        0,
                        None,
                        {
                            "level": "status",
                            "code": "NetStream.Play.Start",
                            "description": "Start live",
                            "details": playpath,
                        },
                    )
                    saw_play = True

            print(f"CLIENT index={client_index} play={redact_query_values(playpath)}", flush=True)
            if args.idle_after_play:
                time.sleep(args.hold_open)
                return

            disconnect_after_tags = args.disconnect_after_tags
            if args.max_clients > 1 and client_index == args.max_clients:
                disconnect_after_tags = 0

            started = time.monotonic()
            first_ts = tags[0][1]
            media_tags_sent = 0
            for tag_type, timestamp, payload in tags:
                target = (timestamp - first_ts) / 1000.0
                now = time.monotonic() - started
                if target > now:
                    time.sleep(target - now)
                csid = 4 if tag_type == 8 else 6 if tag_type == 9 else 5
                writer.send_message(csid, tag_type, STREAM_ID, timestamp, payload)
                if tag_type in (8, 9):
                    media_tags_sent += 1
                    if disconnect_after_tags > 0 and media_tags_sent >= disconnect_after_tags:
                        if args.send_reconnect_request:
                            send_reconnect_request(writer, timestamp)
                        print(
                            f"DISCONNECT index={client_index} tags={media_tags_sent}",
                            flush=True,
                        )
                        return

            if args.send_reconnect_request and disconnect_after_tags <= 0 and args.max_clients == 1:
                send_reconnect_request(writer, tags[-1][1])
            time.sleep(args.hold_open)

    with listener:
        for client_index in range(1, args.max_clients + 1):
            conn, _ = listener.accept()
            if client_index == args.max_clients:
                listener.close()
            serve_client(conn, client_index)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--flv")
    parser.add_argument("--source", dest="flv")
    parser.add_argument("--enhanced-hevc", action="store_true")
    parser.add_argument("--hevc-annexb-source")
    parser.add_argument("--disconnect-after-tags", type=int, default=0)
    parser.add_argument("--send-reconnect-request", action="store_true")
    parser.add_argument("--max-clients", type=int, default=1)
    parser.add_argument("--accept-timeout", type=float, default=10.0)
    parser.add_argument("--io-timeout", type=float, default=20.0)
    parser.add_argument("--hold-open", type=float, default=2.0)
    parser.add_argument("--idle-after-play", action="store_true")
    parser.add_argument("--require-play-query", action="store_true")
    parser.add_argument("--expect-play-path")
    parser.add_argument("--tls-cert")
    parser.add_argument("--tls-key")
    parser.add_argument("--out-chunk-size", type=int, default=OUT_CHUNK_SIZE)
    parser.add_argument("--write-fragment", type=int, default=0)
    parser.add_argument("--write-fragment-delay", type=float, default=0.0)
    parser.add_argument("--loop-duration", type=float, default=0.0)
    args = parser.parse_args()
    if args.out_chunk_size < 4:
        parser.error("--out-chunk-size must be at least 4")
    if args.write_fragment < 0:
        parser.error("--write-fragment must be non-negative")
    if args.write_fragment_delay < 0:
        parser.error("--write-fragment-delay must be non-negative")
    if args.loop_duration < 0:
        parser.error("--loop-duration must be non-negative")

    try:
        run_server(args)
        return 0
    except Exception as exc:
        print(f"RTMP_FIXTURE_ERROR: {exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
