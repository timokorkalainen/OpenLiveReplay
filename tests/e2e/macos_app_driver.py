#!/usr/bin/env python3
import argparse
import base64
import hashlib
import json
import os
import select
import shutil
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

_APP_PID = None
_WINDOW_INFO_CACHE = {}


class WsClient:
    def __init__(self, host, port, timeout=10.0):
        self.host = host
        self.port = port
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setblocking(False)
        self.buffer = bytearray()
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        request = (
            f"GET /api/ws HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(request.encode("ascii"))
        response = self._read_http_response(timeout)
        if b" 101 " not in response.split(b"\r\n", 1)[0]:
            raise RuntimeError(f"websocket handshake failed: {response[:200]!r}")
        self.state = {}
        self.timecode = {}
        self.events = []

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _read_http_response(self, timeout):
        deadline = time.monotonic() + timeout
        data = bytearray()
        while b"\r\n\r\n" not in data:
            if time.monotonic() > deadline:
                raise TimeoutError("timed out waiting for websocket handshake")
            readable, _, _ = select.select([self.sock], [], [], 0.1)
            if not readable:
                continue
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("socket closed during websocket handshake")
            data.extend(chunk)
        header, _, rest = bytes(data).partition(b"\r\n\r\n")
        self.buffer.extend(rest)
        return header + b"\r\n\r\n"

    def _read_exact(self, n, timeout):
        deadline = time.monotonic() + timeout
        data = bytearray()
        if self.buffer:
            take = min(n, len(self.buffer))
            data.extend(self.buffer[:take])
            del self.buffer[:take]
        while len(data) < n:
            if time.monotonic() > deadline:
                raise TimeoutError("timed out waiting for websocket frame")
            readable, _, _ = select.select([self.sock], [], [], 0.1)
            if not readable:
                continue
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise RuntimeError("websocket closed")
            data.extend(chunk)
        return bytes(data)

    def recv_json(self, timeout=10.0):
        header = self._read_exact(2, timeout)
        opcode = header[0] & 0x0F
        length = header[1] & 0x7F
        masked = bool(header[1] & 0x80)
        if length == 126:
            length = struct.unpack("!H", self._read_exact(2, timeout))[0]
        elif length == 127:
            length = struct.unpack("!Q", self._read_exact(8, timeout))[0]
        mask = self._read_exact(4, timeout) if masked else b""
        payload = bytearray(self._read_exact(length, timeout))
        if masked:
            for i in range(length):
                payload[i] ^= mask[i % 4]
        if opcode == 8:
            raise RuntimeError("websocket closed by server")
        if opcode != 1:
            return self.recv_json(timeout)
        msg = json.loads(payload.decode("utf-8"))
        self._apply_message(msg)
        return msg

    def send_json(self, obj):
        payload = json.dumps(obj, separators=(",", ":")).encode("utf-8")
        header = bytearray([0x81])
        if len(payload) < 126:
            header.append(0x80 | len(payload))
        elif len(payload) < 65536:
            header.append(0x80 | 126)
            header.extend(struct.pack("!H", len(payload)))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack("!Q", len(payload)))
        mask = os.urandom(4)
        masked = bytearray(payload)
        for i in range(len(masked)):
            masked[i] ^= mask[i % 4]
        self.sock.sendall(bytes(header) + mask + bytes(masked))

    def command(self, name, args=None, timeout=10.0):
        cmd_id = f"{name}-{int(time.time() * 1000)}-{os.getpid()}"
        self.send_json({"type": "command", "id": cmd_id, "name": name, "args": args or {}})
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            msg = self.recv_json(max(0.1, deadline - time.monotonic()))
            if msg.get("type") == "ack" and msg.get("id") == cmd_id:
                if not msg.get("ok"):
                    raise RuntimeError(f"{name} failed: {msg}")
                return msg
        raise TimeoutError(f"timed out waiting for ack to {name}")

    def wait_for(self, predicate, timeout, label):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            try:
                self.recv_json(max(0.1, min(1.0, deadline - time.monotonic())))
            except TimeoutError:
                pass
        raise TimeoutError(f"timed out waiting for {label}")

    def _apply_message(self, msg):
        typ = msg.get("type")
        if typ == "state.snapshot":
            self.state = msg.get("state", {})
        elif typ == "state.patch":
            path = msg.get("path")
            value = msg.get("value")
            if path == "snapshot":
                self.state = value or {}
            elif path:
                self.state[path] = value
        elif typ == "timecode":
            self.timecode = msg
        elif typ == "event":
            self.events.append(msg)


def wait_for_port(port, timeout):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.1)
    raise TimeoutError(f"control port {port} did not open: {last_error}")


def newest_jpeg(videos_dir, previous, token=None):
    files = sorted(Path(videos_dir).glob("*.jpg"), key=lambda p: p.stat().st_mtime_ns)
    for path in reversed(files):
        if token and f"_{token}_" not in path.name:
            continue
        previous_mtime = previous.get(str(path), -1)
        if path.stat().st_mtime_ns > previous_mtime:
            return path
    return None


def parse_probe_marker(output):
    fields = {}
    for part in output.strip().split():
        if "=" in part:
            k, v = part.split("=", 1)
            fields[k] = v
    if "firstIndex" not in fields:
        raise RuntimeError(f"marker probe output missing firstIndex: {output}")
    return int(fields["firstIndex"]), output.strip()


def decode_marker(jpeg, marker_probe):
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError("ffmpeg not found")
    ff = subprocess.Popen(
        [ffmpeg, "-hide_banner", "-loglevel", "error", "-i", str(jpeg),
         "-vf", "scale=256:144", "-f", "rawvideo", "-pix_fmt", "gray", "-"],
        stdout=subprocess.PIPE,
    )
    probe = subprocess.run(
        [marker_probe, "256", "144"],
        stdin=ff.stdout,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if ff.stdout:
        ff.stdout.close()
    ff_rc = ff.wait()
    if ff_rc != 0:
        raise RuntimeError(f"ffmpeg failed decoding {jpeg}: {ff_rc}")
    if probe.returncode != 0:
        raise RuntimeError(f"marker probe failed for {jpeg}: {probe.stderr.strip()}")
    return parse_probe_marker(probe.stdout)


def decode_marker_index_from_luma(frame):
    if len(frame) < 256 * 144:
        return -1
    index = 0
    for bit in range(24):
        x = bit * 8 + 4
        y = 4
        index = (index << 1) | (1 if frame[y * 256 + x] > 128 else 0)
    return index


def newest_recording(recordings_dir):
    files = sorted(Path(recordings_dir).glob("*.mkv"), key=lambda p: p.stat().st_mtime_ns)
    if not files:
        raise FileNotFoundError(f"no MKV recordings in {recordings_dir}")
    return files[-1]


def marker_timeline_from_recording(recording):
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError("ffmpeg not found")
    frame_bytes = 256 * 144
    ff = subprocess.Popen(
        [ffmpeg, "-hide_banner", "-loglevel", "error",
         "-i", str(recording), "-map", "0:v:0", "-vf", "scale=256:144",
         "-f", "rawvideo", "-pix_fmt", "gray", "-"],
        stdout=subprocess.PIPE,
    )
    timeline = []
    try:
        while True:
            frame = ff.stdout.read(frame_bytes)
            if not frame:
                break
            if len(frame) != frame_bytes:
                raise RuntimeError(f"short raw frame from {recording}: {len(frame)} bytes")
            timeline.append(decode_marker_index_from_luma(frame))
    finally:
        if ff.stdout:
            ff.stdout.close()
    ff_rc = ff.wait()
    if ff_rc != 0:
        raise RuntimeError(f"ffmpeg failed decoding marker timeline for {recording}: {ff_rc}")
    if not timeline or any(marker < 0 for marker in timeline):
        raise RuntimeError(f"marker timeline decode failed for {recording}: frames={len(timeline)}")
    print(f"APP_RECORDING_TIMELINE path={recording} frames={len(timeline)} first={timeline[0]} last={timeline[-1]}")
    return timeline


def marker_from_recording(recording, marker_probe, position_ms):
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError("ffmpeg not found")
    ff = subprocess.Popen(
        [ffmpeg, "-hide_banner", "-loglevel", "error",
         "-ss", f"{position_ms / 1000.0:.3f}", "-i", str(recording),
         "-frames:v", "1", "-vf", "scale=256:144",
         "-f", "rawvideo", "-pix_fmt", "gray", "-"],
        stdout=subprocess.PIPE,
    )
    probe = subprocess.run(
        [marker_probe, "256", "144"],
        stdin=ff.stdout,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if ff.stdout:
        ff.stdout.close()
    ff_rc = ff.wait()
    if ff_rc != 0:
        raise RuntimeError(f"ffmpeg failed decoding {recording} at {position_ms}ms: {ff_rc}")
    if probe.returncode != 0:
        raise RuntimeError(
            f"marker probe failed for {recording} at {position_ms}ms: {probe.stderr.strip()}")
    marker, line = parse_probe_marker(probe.stdout)
    print(f"APP_RECORDING_MARKER path={recording} positionMs={position_ms} marker={marker} {line}")
    return marker


def preview_target(state):
    for row in state.get("output", {}).get("previewTargets", []):
        if row.get("id") in ("feed0-qt-preview", "qt-preview-feed-0"):
            return row
    return {}


def compact_debug_state(state):
    return {
        "recording": state.get("recording", {}),
        "transport": state.get("transport", {}),
        "view": state.get("view", {}),
        "sources": state.get("sources", []),
        "output": state.get("output", {}),
        "previewTarget": preview_target(state),
    }


def dump_debug_state(ws, args, label):
    workdir = Path(args.workdir)
    state = ws.state if ws else {}
    refreshed = {}
    try:
        probe = WsClient("127.0.0.1", args.port, timeout=2.0)
        probe.wait_for(lambda: bool(probe.state), 3.0, "debug snapshot")
        refreshed = probe.state
        probe.close()
    except Exception as exc:
        print(f"APP_DEBUG refresh_failed={exc!r}")

    debug = {
        "label": label,
        "state": compact_debug_state(state),
        "refreshedState": compact_debug_state(refreshed),
        "eventsTail": (ws.events[-10:] if ws else []),
    }
    path = workdir / f"debug-{label}.json"
    path.write_text(json.dumps(debug, indent=2, sort_keys=True), encoding="utf-8")
    print(f"APP_DEBUG_STATE path={path}")
    print("APP_DEBUG_SUMMARY " + json.dumps(compact_debug_state(refreshed or state),
                                           sort_keys=True, separators=(",", ":")))
    screenshot(workdir / f"debug-{label}.png")


def capture_and_decode(ws, docs_root, marker_probe, label, token="PGM"):
    videos = Path(docs_root) / "videos"
    videos.mkdir(parents=True, exist_ok=True)
    previous = {str(p): p.stat().st_mtime_ns for p in videos.glob("*.jpg")}
    ws.command("capture.current", timeout=10.0)
    deadline = time.monotonic() + 5.0
    captured = None
    while time.monotonic() < deadline:
        captured = newest_jpeg(videos, previous, token)
        if captured:
            break
        time.sleep(0.05)
    if not captured:
        raise TimeoutError(f"capture.current did not create a {token} JPEG for {label}")
    marker, probe_line = decode_marker(captured, marker_probe)
    print(f"APP_CAPTURE {label} path={captured} marker={marker} {probe_line}")
    return marker, captured


def wait_for_marker(ws, docs_root, marker_probe, label, predicate, timeout=8.0):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            marker, path = capture_and_decode(ws, docs_root, marker_probe, label)
            if predicate(marker):
                return marker, path
            last_error = AssertionError(f"marker predicate rejected {marker}")
        except Exception as exc:
            last_error = exc
        time.sleep(0.15)
    raise TimeoutError(f"timed out waiting for marker {label}: {last_error}")


def wait_for_stable_marker(ws, docs_root, marker_probe, label, predicate, timeout=8.0):
    deadline = time.monotonic() + timeout
    last_marker = None
    last_path = None
    last_error = None
    while time.monotonic() < deadline:
        try:
            marker, path = capture_and_decode(ws, docs_root, marker_probe, label)
            if marker == last_marker and predicate(marker):
                return marker, path
            last_marker = marker
            last_path = path
            last_error = AssertionError(f"marker {marker} not stable or rejected")
        except Exception as exc:
            last_error = exc
        time.sleep(0.15)
    raise TimeoutError(f"timed out waiting for stable marker {label}: {last_error} last={last_marker} path={last_path}")


def transport_position(ws):
    return int(ws.state.get("transport", {}).get("positionMs", 0))


def wait_for_transport_position(ws, position_ms, label, timeout=3.0):
    ws.wait_for(
        lambda: transport_position(ws) == position_ms,
        timeout,
        f"{label} transport position {position_ms} ms",
    )


def wait_for_transport_change(ws, previous_ms, label, timeout=3.0):
    ws.wait_for(
        lambda: transport_position(ws) != previous_ms,
        timeout,
        f"{label} transport position changed from {previous_ms} ms",
    )
    return transport_position(ws)


def frame_index_to_ms(frame_index, fps=30):
    return 0 if frame_index <= 0 else int(frame_index * 1000 / fps)


def ms_to_frame_index(ms, fps=30):
    return 0 if ms <= 0 else int(ms * fps / 1000)


def first_frame_index_at_or_after_ms(ms, fps=30):
    if ms <= 0:
        return 0
    index = ms_to_frame_index(ms, fps)
    while frame_index_to_ms(index, fps) < ms:
        index += 1
    while index > 0 and frame_index_to_ms(index - 1, fps) >= ms:
        index -= 1
    return index


def first_frame_index_after_ms(ms, fps=30):
    index = first_frame_index_at_or_after_ms(ms, fps)
    while frame_index_to_ms(index, fps) <= ms:
        index += 1
    return index


def stepped_position_ms(position_ms, frames, fps=30):
    if frames == 0:
        return position_ms
    if frames > 0:
        target_frame = first_frame_index_after_ms(position_ms, fps) + frames - 1
    else:
        target_frame = max(0, first_frame_index_at_or_after_ms(position_ms, fps) + frames)
    return frame_index_to_ms(target_frame, fps)


def expected_marker_for_frame(timeline, frame_index):
    frame_index = int(frame_index)
    if frame_index < 0 or frame_index >= len(timeline):
        raise IndexError(f"frame index {frame_index} outside marker timeline length {len(timeline)}")
    return timeline[frame_index]


def frame_index_for_position(position_ms, fps=30):
    return ms_to_frame_index(max(0, int(position_ms)), fps)


def assert_preview_at_frame(ws, docs_root, marker_probe, timeline, frame_index, label,
                            workdir=None, timeout=4.0, screen_first=False,
                            max_screen_elapsed_ms=None):
    expected = expected_marker_for_frame(timeline, frame_index)
    if workdir and screen_first:
        assert_screen_preview_at_frame(workdir, marker_probe, expected, label, timeout=timeout,
                                       max_elapsed_ms=max_screen_elapsed_ms)
    marker, _ = wait_for_stable_marker(
        ws,
        docs_root,
        marker_probe,
        label,
        lambda observed: observed == expected,
        timeout=timeout,
    )
    if workdir and not screen_first:
        assert_screen_preview_at_frame(workdir, marker_probe, expected, label, timeout=timeout,
                                       max_elapsed_ms=max_screen_elapsed_ms)
    print(f"APP_ASSERT_FRAME {label} frame={frame_index} marker={marker}")
    return marker


def assert_live_preview_visible(ws, docs_root, marker_probe, label, timeout=8.0):
    marker, _ = wait_for_marker(
        ws,
        docs_root,
        marker_probe,
        label,
        lambda observed: observed not in (0, 16777215),
        timeout=timeout,
    )
    print(f"APP_ASSERT_VISIBLE {label} marker={marker}")
    return marker


def screenshot(path):
    tool = shutil.which("screencapture")
    if not tool:
        print("APP_SCREENSHOT skipped=screencapture-not-found")
        return False
    window_info = app_window_info(_APP_PID)
    if not window_info:
        print("APP_SCREENSHOT skipped=openlivereplay-window-not-found")
        return False
    result = subprocess.run([tool, "-x", "-l", str(window_info["id"]), str(path)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            check=False)
    if result.returncode == 0 and Path(path).exists():
        print(
            "APP_SCREENSHOT "
            f"path={path} pid={window_info['pid']} windowId={window_info['id']} "
            f"bounds={window_info['x']},{window_info['y']},"
            f"{window_info['width']},{window_info['height']}"
        )
        return True
    else:
        print(f"APP_SCREENSHOT skipped=failed stderr={result.stderr.strip()!r}")
        return False


def image_dimensions(path):
    ffprobe = shutil.which("ffprobe")
    if not ffprobe:
        raise RuntimeError("ffprobe not found")
    result = subprocess.run(
        [ffprobe, "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height", "-of", "csv=p=0:s=x", str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"ffprobe failed for {path}: {result.stderr.strip()}")
    width_text, height_text = result.stdout.strip().split("x", 1)
    return int(width_text), int(height_text)


def app_window_info(pid):
    global _WINDOW_INFO_CACHE
    if pid is None:
        print("APP_WINDOW_INFO skipped=app-pid-not-set")
        return None
    if pid in _WINDOW_INFO_CACHE:
        return _WINDOW_INFO_CACHE[pid]
    swift = shutil.which("swift")
    if not swift:
        print("APP_WINDOW_INFO skipped=swift-not-found")
        return None
    script = r'''
import Foundation
import CoreGraphics

guard let targetPidText = ProcessInfo.processInfo.environment["OLR_APP_E2E_PID"],
      let targetPid = Int(targetPidText) else {
    print("missing target pid")
    exit(2)
}

let windows = CGWindowListCopyWindowInfo(
    [.optionOnScreenOnly, .excludeDesktopElements],
    kCGNullWindowID
) as? [[String: Any]] ?? []

var layerZeroRank = 0
var matches: [String] = []
for window in windows {
    let owner = window[kCGWindowOwnerName as String] as? String ?? ""
    let pid = window[kCGWindowOwnerPID as String] as? Int ?? -1
    let layer = window[kCGWindowLayer as String] as? Int ?? -1
    if layer != 0 {
        continue
    }
    let isOnscreen = (window[kCGWindowIsOnscreen as String] as? Bool) ?? false
    defer { layerZeroRank += 1 }
    if owner != "OpenLiveReplay" || pid != targetPid || !isOnscreen {
        continue
    }
    guard let number = window[kCGWindowNumber as String] as? Int,
          let bounds = window[kCGWindowBounds as String] as? [String: Any],
          let x = bounds["X"] as? Double,
          let y = bounds["Y"] as? Double,
          let width = bounds["Width"] as? Double,
          let height = bounds["Height"] as? Double else {
        continue
    }
    matches.append("\(number) \(Int(x)) \(Int(y)) \(Int(width)) \(Int(height)) \(pid) \(layerZeroRank)")
}

if matches.count == 1 {
    print(matches[0])
    exit(0)
}
print("matches=\(matches.count)")
exit(matches.isEmpty ? 1 : 2)
'''
    env = os.environ.copy()
    env["OLR_APP_E2E_PID"] = str(pid)
    result = subprocess.run(
        [swift, "-e", script],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
        check=False,
    )
    if result.returncode != 0:
        print(
            f"APP_WINDOW_INFO skipped pid={pid} "
            f"stdout={result.stdout.strip()!r} stderr={result.stderr.strip()!r}"
        )
        return None
    parts = [int(part) for part in result.stdout.strip().split()]
    if len(parts) != 7:
        print(f"APP_WINDOW_INFO skipped output={result.stdout.strip()!r}")
        return None
    if parts[6] != 0:
        print(f"APP_WINDOW_INFO skipped pid={pid} frontRank={parts[6]} not-frontmost")
        return None
    _WINDOW_INFO_CACHE[pid] = {
        "id": parts[0],
        "x": parts[1],
        "y": parts[2],
        "width": parts[3],
        "height": parts[4],
        "pid": parts[5],
        "frontRank": parts[6],
    }
    print(
        "APP_WINDOW_INFO "
        f"pid={parts[5]} id={parts[0]} frontRank={parts[6]} "
        f"bounds={parts[1]},{parts[2]},{parts[3]},{parts[4]}"
    )
    return _WINDOW_INFO_CACHE[pid]


def clamp_crop(crop, image_width, image_height):
    name, x, y, width, height = crop
    x = max(0, min(int(x), image_width - 2))
    y = max(0, min(int(y), image_height - 2))
    width = max(1, min(int(width), image_width - x))
    height = max(1, min(int(height), image_height - y))
    if width < 96 or height < 54:
        return None
    return name, x, y, width, height


def screen_marker_crop_candidates(bounds, image_width, image_height):
    if bounds:
        win_x, win_y, win_w, win_h = bounds
    else:
        win_x, win_y = 0, 0
        win_w, win_h = image_width, image_height

    feed_left = win_x + int(win_w * 0.024)
    feed_top = win_y + int(win_h * 0.134)
    feed_width = int(win_w * 0.335)
    feed_height = int(feed_width * 9 / 16)
    feed_offsets = [
        ("feed0-video", 0.0, 0.0, 1.0),
        ("feed0-video-left", -0.01, 0.0, 1.0),
        ("feed0-video-up", 0.0, -0.012, 1.0),
        ("feed0-video-wide", -0.01, -0.012, 1.06),
        ("feed0-video-tight", 0.01, 0.012, 0.94),
    ]

    rail_free_w = int(win_w * 0.74)
    toolbar_offsets = [
        40,
        44,
        48,
        84,
        88,
        92,
        max(36, int(win_h * 0.05)),
        max(40, int(win_h * 0.08)),
    ]
    widths = [
        rail_free_w,
        int(rail_free_w * 0.90),
        int(rail_free_w * 0.78),
        int(win_w * 0.50),
        int(win_w * 0.37),
    ]

    raw = []
    for name, x_scale, y_scale, size_scale in feed_offsets:
        width = int(feed_width * size_scale)
        height = int(width * 9 / 16)
        raw.append((name,
                    feed_left + int(feed_width * x_scale),
                    feed_top + int(feed_height * y_scale),
                    width,
                    height))

    for y_offset in toolbar_offsets:
        for width in widths:
            height = int(width * 9 / 16)
            max_stage_h = int(win_h * 0.68)
            if height > max_stage_h:
                height = max_stage_h
            raw.append((f"stage-{width}x{height}+0+{y_offset}",
                        win_x + 2, win_y + y_offset, width, height))

    # Fallback to the old visibility crop in case accessibility cannot report a
    # sane app window. It is less precise for marker decoding but useful evidence
    # in failure logs.
    raw.append(("legacy-visible",
                max(0, win_x), max(0, win_y + int(win_h * 0.08)),
                max(32, int(win_w * 0.35)), max(32, int(win_h * 0.30))))

    candidates = []
    seen = set()
    for crop in raw:
        clamped = clamp_crop(crop, image_width, image_height)
        if not clamped:
            continue
        key = clamped[1:]
        if key in seen:
            continue
        seen.add(key)
        candidates.append(clamped)
    return candidates


def decode_marker_from_crop(image, marker_probe, crop):
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError("ffmpeg not found")
    name, x, y, width, height = crop
    vf = f"crop={width}:{height}:{x}:{y},scale=256:144"
    ff = subprocess.Popen(
        [ffmpeg, "-hide_banner", "-loglevel", "error", "-i", str(image),
         "-vf", vf, "-f", "rawvideo", "-pix_fmt", "gray", "-"],
        stdout=subprocess.PIPE,
    )
    probe = subprocess.run(
        [marker_probe, "256", "144"],
        stdin=ff.stdout,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if ff.stdout:
        ff.stdout.close()
    ff_rc = ff.wait()
    if ff_rc != 0:
        raise RuntimeError(f"ffmpeg failed decoding screen crop {name}: {ff_rc}")
    if probe.returncode != 0:
        raise RuntimeError(f"marker probe failed for screen crop {name}: {probe.stderr.strip()}")
    marker, line = parse_probe_marker(probe.stdout)
    return marker, line


def decode_marker_from_screen(image, marker_probe, expected_marker):
    image_width, image_height = image_dimensions(image)
    bounds = (0, 0, image_width, image_height)
    attempts = []
    for crop in screen_marker_crop_candidates(bounds, image_width, image_height):
        name, x, y, width, height = crop
        try:
            marker, line = decode_marker_from_crop(image, marker_probe, crop)
            attempts.append(f"{name}@{width}x{height}+{x}+{y}:{marker}")
            if marker == expected_marker:
                return marker, crop, line, attempts
        except Exception as exc:
            attempts.append(f"{name}@{width}x{height}+{x}+{y}:ERR:{exc}")
    return None, None, "", attempts


def assert_screen_preview_at_frame(workdir, marker_probe, expected_marker, label, timeout=4.0,
                                   max_elapsed_ms=None):
    started = time.perf_counter()
    deadline = time.monotonic() + timeout
    attempt = 0
    last_attempts = []
    last_shot = None
    while time.monotonic() < deadline:
        attempt += 1
        shot = Path(workdir) / f"screen-marker-{label}-{attempt}.png"
        last_shot = shot
        if not screenshot(shot):
            raise RuntimeError(f"OS screenshot failed for screen marker {label}")
        marker, crop, probe_line, attempts = decode_marker_from_screen(
            shot, marker_probe, expected_marker)
        last_attempts = attempts
        if crop:
            name, x, y, width, height = crop
            elapsed_ms = (time.perf_counter() - started) * 1000.0
            if max_elapsed_ms is not None and elapsed_ms > max_elapsed_ms:
                raise AssertionError(
                    "visible app screenshot reached expected marker too slowly "
                    f"for {label}: expected={expected_marker} elapsedMs={elapsed_ms:.2f} "
                    f"maxElapsedMs={max_elapsed_ms:.2f} shot={shot}"
                )
            print(
                "APP_SCREEN_MARKER "
                f"{label} path={shot} marker={marker} expected={expected_marker} "
                f"elapsedMs={elapsed_ms:.2f} "
                f"crop={name}@{width}x{height}+{x}+{y} {probe_line}"
            )
            return marker
        time.sleep(0.10)
    raise AssertionError(
        "visible app screenshot did not show expected marker "
        f"for {label}: expected={expected_marker} shot={last_shot} attempts={last_attempts}"
    )


def assert_screen_video_visible(workdir, label):
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError("ffmpeg not found")
    shot = Path(workdir) / f"screen-{label}.png"
    if not screenshot(shot):
        raise RuntimeError(f"OS screenshot failed for {label}")
    image_width, image_height = image_dimensions(shot)
    crop_x = 0
    crop_y = int(image_height * 0.08)
    crop_w = max(32, int(image_width * 0.35))
    crop_h = max(32, int(image_height * 0.30))
    vf = f"crop={crop_w}:{crop_h}:{crop_x}:{crop_y},scale=256:144"
    raw = subprocess.check_output(
        [ffmpeg, "-hide_banner", "-loglevel", "error", "-i", str(shot),
         "-vf", vf, "-f", "rawvideo", "-pix_fmt", "gray", "-"]
    )
    if len(raw) != 256 * 144:
        raise RuntimeError(f"screen crop decode failed for {label}: {len(raw)} bytes")
    values = list(raw)
    mean = sum(values) / len(values)
    variance = sum((value - mean) * (value - mean) for value in values) / len(values)
    stddev = variance ** 0.5
    p90 = sorted(values)[int(len(values) * 0.90)]
    print(
        "APP_SCREEN_ASSERT "
        f"{label} path={shot} crop={crop_w}x{crop_h}+{crop_x}+{crop_y} "
        f"mean={mean:.2f} stddev={stddev:.2f} p90={p90}"
    )
    if p90 < 40 or stddev < 18:
        raise AssertionError(
            f"visible app video pane looks blank for {label}: p90={p90} stddev={stddev:.2f}"
        )


def configure_source(ws, srt_url, save_location):
    sources = ws.state.get("sources", [])
    if not sources:
        ws.command("sources.add")
        ws.wait_for(lambda: len(ws.state.get("sources", [])) >= 1, 5.0, "source add")
    sources = ws.state.get("sources", [])
    for index in range(len(sources) - 1, 0, -1):
        ws.command("sources.remove", {"index": index})
    ws.command("sources.updateUrl", {"index": 0, "url": srt_url})
    ws.command("sources.updateName", {"index": 0, "name": "marker"})
    ws.command("sources.updateId", {"index": 0, "id": "marker-0"})
    ws.command("settings.setProject", {"fileName": "app_oracle", "saveLocation": save_location})
    ws.command("settings.setRecordingFormat", {"width": 256, "height": 144, "fps": 30})
    ws.command("settings.save")


def main():
    global _APP_PID
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--documents-root", required=True)
    ap.add_argument("--srt-url", required=True)
    ap.add_argument("--marker-probe", required=True)
    ap.add_argument("--workdir", required=True)
    args = ap.parse_args()

    docs_root = Path(args.documents_root)
    docs_root.mkdir(parents=True, exist_ok=True)
    app_log = Path(args.workdir) / "OpenLiveReplay-app.log"
    env = os.environ.copy()
    env["OLR_CONTROL_PORT"] = str(args.port)
    env["OLR_DOCUMENTS_ROOT"] = str(docs_root)
    env["OLR_APP_E2E_FORCE_ACTIVATE"] = "1"
    env["OLR_GPU_PIPELINE"] = "1"
    env.setdefault("QT_LOGGING_RULES", "qt.multimedia.ffmpeg=false")

    with app_log.open("w") as log:
        proc = subprocess.Popen([args.app], stdout=log, stderr=subprocess.STDOUT, env=env)
    _APP_PID = proc.pid
    ws = None
    try:
        wait_for_port(args.port, 20.0)
        ws = WsClient("127.0.0.1", args.port)
        ws.wait_for(lambda: bool(ws.state), 10.0, "initial snapshot")

        configure_source(ws, args.srt_url, str(Path(args.workdir) / "recordings"))

        try:
            ws.command("recording.start", timeout=10.0)
            ws.wait_for(lambda: ws.state.get("recording", {}).get("active") is True,
                        10.0, "recording active")
            ws.wait_for(lambda: ws.state.get("sources", [{}])[0].get("connected") is True,
                        10.0, "source connected")
            ws.command("view.setPlaybackViewState", {"singleView": True, "selectedIndex": 0})
            ws.wait_for(lambda: ws.state.get("recording", {}).get("durationMs", 0) >= 5000,
                        12.0, "recorded duration >= 5000 ms")
            assert_live_preview_visible(ws, docs_root, args.marker_probe, "startup_visible",
                                        timeout=8.0)
            assert_screen_video_visible(args.workdir, "startup_visible")
            ws.wait_for(lambda: ws.state.get("recording", {}).get("durationMs", 0) >= 32000,
                        45.0, "recorded duration >= 32000 ms")
            recording_file = newest_recording(Path(args.workdir) / "recordings")
            timeline = marker_timeline_from_recording(recording_file)

            ws.command("transport.pause")

            current_frame = 900
            ws.command("transport.seek", {"positionMs": frame_index_to_ms(current_frame)})
            assert_preview_at_frame(ws, docs_root, args.marker_probe, timeline, current_frame,
                                    "prime_30s", workdir=args.workdir)

            backward_frames = []
            forward_frames = []
            second_backward_frames = []

            def drive_steps(label, count, delta, delay_s, bucket):
                nonlocal current_frame
                for index in range(count):
                    current_frame += delta
                    ws.command("transport.stepFrame", {"frames": delta})
                    bucket.append(current_frame)
                    assert_preview_at_frame(
                        ws,
                        docs_root,
                        args.marker_probe,
                        timeline,
                        current_frame,
                        f"{label}_{index + 1}",
                        workdir=args.workdir,
                        timeout=3.0,
                        screen_first=True,
                        max_screen_elapsed_ms=750.0,
                    )
                    time.sleep(delay_s)

            drive_steps("backward60_500ms", 60, -1, 0.5, backward_frames)
            drive_steps("forward15_500ms", 15, 1, 0.5, forward_frames)
            drive_steps("backward30_500ms", 30, -1, 0.5, second_backward_frames)

            cold_seek_frames = [120, 840, 60]
            for index, frame in enumerate(cold_seek_frames):
                if abs(frame_index_to_ms(frame) - frame_index_to_ms(current_frame)) < 5000:
                    raise AssertionError(f"cold seek target {frame} is not >5s away from {current_frame}")
                started = time.perf_counter()
                ws.command("transport.seek", {"positionMs": frame_index_to_ms(frame)})
                current_frame = frame
                assert_preview_at_frame(ws, docs_root, args.marker_probe, timeline, current_frame,
                                        f"cold_seek_{index + 1}", workdir=args.workdir,
                                        timeout=3.0, screen_first=True,
                                        max_screen_elapsed_ms=750.0)
                elapsed_ms = (time.perf_counter() - started) * 1000.0
                print(f"APP_COLD_SEEK_OBSERVED index={index + 1} frame={frame} clientElapsedMs={elapsed_ms:.2f}")

            assert_screen_video_visible(args.workdir, "final_visible")
            ws.command("recording.stop", timeout=10.0)
            print(
                "APP_E2E_PASS "
                f"primeFrame=900 backward60={len(backward_frames)} "
                f"forward15={len(forward_frames)} backward30={len(second_backward_frames)} "
                f"coldSeek={len(cold_seek_frames)} "
                f"log={app_log}"
            )
            return 0
        except Exception:
            dump_debug_state(ws, args, "failure")
            raise
    finally:
        if ws:
            ws.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"APP_E2E_FAIL {exc}", file=sys.stderr)
        raise
