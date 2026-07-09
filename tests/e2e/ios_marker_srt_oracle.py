#!/usr/bin/env python3
"""Drive an iOS device against a local numbered-frame SRT marker stream.

This is a manual/device E2E helper, not a CI test. It configures a connected
iOS/iPadOS app via its app data container, drives the app over the WebSocket
control API, captures app preview JPEGs, pulls those JPEGs back with devicectl,
and verifies the marker number in each captured frame.
"""

import argparse
import json
import os
import re
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from macos_app_driver import WsClient


DEFAULT_BUNDLE = "com.timokorkalainen.OpenLiveReplay"


def run(cmd, *, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True):
    proc = subprocess.run(cmd, stdout=stdout, stderr=stderr, text=text, check=False)
    if check and proc.returncode != 0:
        raise RuntimeError(
            f"command failed ({proc.returncode}): {' '.join(map(str, cmd))}\n"
            f"stdout={proc.stdout}\nstderr={proc.stderr}"
        )
    return proc


def require_tool(name):
    path = shutil.which(name)
    if not path:
        raise RuntimeError(f"{name} not found in PATH")
    return path


def choose_h264_args():
    encoders = run(["ffmpeg", "-hide_banner", "-encoders"], check=True).stdout
    if re.search(r"(^|\s)libx264\s", encoders):
        return [
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            "-tune",
            "zerolatency",
            "-pix_fmt",
            "yuv420p",
            "-g",
            "1",
            "-keyint_min",
            "1",
            "-sc_threshold",
            "0",
            "-b:v",
            "4M",
        ]
    if re.search(r"(^|\s)h264_videotoolbox\s", encoders):
        return [
            "-c:v",
            "h264_videotoolbox",
            "-allow_sw",
            "1",
            "-realtime",
            "1",
            "-pix_fmt",
            "yuv420p",
            "-g",
            "1",
            "-b:v",
            "4M",
        ]
    raise RuntimeError("ffmpeg has no usable H.264 encoder")


def generate_marker_fixture(args, workdir):
    prefix = workdir / "marker"
    run([args.marker_source, str(prefix), str(args.marker_seconds)], check=True)
    video_args = choose_h264_args()
    marker_mkv = workdir / "marker.mkv"
    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "yuv420p",
        "-s",
        "256x144",
        "-r",
        str(args.fps),
        "-i",
        str(prefix.with_suffix(".yuv")),
        "-f",
        "s16le",
        "-ar",
        "48000",
        "-ac",
        "2",
        "-i",
        str(prefix.with_suffix(".pcm")),
        "-filter_complex",
        f"[0:v]settb=1/1000,setpts=trunc(N*1000/{args.fps})[v]",
        "-map",
        "[v]",
        "-map",
        "1:a",
        *video_args,
        "-enc_time_base",
        "1:1000",
        "-c:a",
        "pcm_s16le",
        str(marker_mkv),
    ]
    run(cmd, check=True)
    return marker_mkv


def start_srt_marker_producer(args, marker_mkv, workdir):
    srt_log = (workdir / "srt-live-transmit.log").open("w", encoding="utf-8")
    ffmpeg_log = (workdir / "ffmpeg-stream.log").open("w", encoding="utf-8")
    srt_proc = subprocess.Popen(
        [
            "srt-live-transmit",
            f"udp://127.0.0.1:{args.udp_port}?mode=listener",
            f"srt://0.0.0.0:{args.srt_port}?mode=listener&transtype=live&latency=120",
        ],
        stdout=srt_log,
        stderr=subprocess.STDOUT,
        text=True,
    )
    time.sleep(0.5)
    ffmpeg_proc = subprocess.Popen(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "warning",
            "-re",
            "-stream_loop",
            "-1",
            "-i",
            str(marker_mkv),
            "-map",
            "0:v:0",
            "-map",
            "0:a:0",
            "-c:v",
            "copy",
            "-c:a",
            "aac",
            "-b:a",
            "96k",
            "-f",
            "mpegts",
            f"udp://127.0.0.1:{args.udp_port}?pkt_size=1316",
        ],
        stdout=ffmpeg_log,
        stderr=subprocess.STDOUT,
        text=True,
    )
    time.sleep(0.8)
    if srt_proc.poll() is not None:
        raise RuntimeError(f"srt-live-transmit exited early, see {srt_log.name}")
    if ffmpeg_proc.poll() is not None:
        raise RuntimeError(f"ffmpeg marker stream exited early, see {ffmpeg_log.name}")
    return [srt_proc, ffmpeg_proc], [srt_log, ffmpeg_log]


def stop_processes(processes):
    for proc in processes:
        if proc.poll() is None:
            proc.terminate()
    deadline = time.monotonic() + 3
    for proc in processes:
        while proc.poll() is None and time.monotonic() < deadline:
            time.sleep(0.05)
        if proc.poll() is None:
            proc.kill()
        proc.wait(timeout=2)


def devicectl_base(args):
    return [
        "xcrun",
        "devicectl",
        "device",
        "copy",
    ]


def device_copy_from(args, source, destination, *, check=True):
    return run(
        [
            *devicectl_base(args),
            "from",
            "--device",
            args.device,
            "--domain-type",
            "appDataContainer",
            "--domain-identifier",
            args.bundle,
            "--source",
            source,
            "--destination",
            str(destination),
        ],
        check=check,
    )


def device_copy_to(args, source, destination):
    run(
        [
            *devicectl_base(args),
            "to",
            "--device",
            args.device,
            "--domain-type",
            "appDataContainer",
            "--domain-identifier",
            args.bundle,
            "--source",
            str(source),
            "--destination",
            destination,
        ],
        check=True,
    )


def device_files(args, subdir, name_contains, workdir):
    out = workdir / f"files-{re.sub(r'[^A-Za-z0-9_.-]+', '_', name_contains)}.json"
    run(
        [
            "xcrun",
            "devicectl",
            "device",
            "info",
            "files",
            "--device",
            args.device,
            "--domain-type",
            "appDataContainer",
            "--domain-identifier",
            args.bundle,
            "--subdirectory",
            subdir,
            "--filter",
            f"Name CONTAINS '{name_contains}'",
            "--columns",
            "*",
            "-r",
            "--json-output",
            str(out),
        ],
        check=True,
    )
    return json.loads(out.read_text(encoding="utf-8"))["result"]["files"]


def write_marker_config(args, workdir, srt_url):
    config = {
        "audioOutputLatencyMs": 0,
        "broadcastOutputs": [],
        "fileName": args.project,
        "fps": args.fps,
        "fpsDen": 1,
        "fpsNum": args.fps,
        "importSettingsUrl": "",
        "metadataFields": [],
        "midiBindings": [],
        "midiPortName": "",
        "multiviewCount": 1,
        "saveLocation": "",
        "showTimeOfDay": False,
        "sources": [
            {
                "id": "marker-1",
                "name": "LOCAL MARKER SRT",
                "url": srt_url,
                "metadata": [],
                "trimOffsetMs": 0,
                "telemetryDelayMs": 0,
            }
        ],
        "streamDeckDialPressMaps": {},
        "streamDeckDialRotateMaps": {},
        "streamDeckKeyMaps": {},
        "telemetrySseUrl": "",
        "videoCodec": "mpeg2",
        "videoHeight": 144,
        "videoWidth": 256,
    }
    path = workdir / "config.local-marker.json"
    path.write_text(json.dumps(config, indent=2, sort_keys=True), encoding="utf-8")
    return path


def push_marker_config(args, workdir, srt_url):
    backup = workdir / "config.original.json"
    result = device_copy_from(
        args, "Documents/settings/config.json", backup, check=False
    )
    if result.returncode != 0:
        backup = None
    config = write_marker_config(args, workdir, srt_url)
    device_copy_to(args, config, "Documents/settings/config.json")
    return backup


def restore_config(args, backup):
    if backup and backup.exists():
        device_copy_to(args, backup, "Documents/settings/config.json")


def launch_app(args):
    env = {
        "OLR_CONTROL_BIND": "any",
        "OLR_E2E_LATENCY_TRACE": "1",
        "OLR_PB_TELEMETRY": "1",
    }
    if args.launch_gpu_pipeline:
        env["OLR_GPU_PIPELINE"] = "1"
    cmd = [
        "xcrun",
        "devicectl",
        "device",
        "process",
        "launch",
        "--device",
        args.device,
        "--terminate-existing",
        "--environment-variables",
        json.dumps(env, separators=(",", ":")),
    ]
    if args.launch_console_log:
        cmd.append("--console")
    cmd.append(args.bundle)

    console_proc = None
    if args.launch_console_log:
        args.launch_console_log.parent.mkdir(parents=True, exist_ok=True)
        log = args.launch_console_log.open("w", encoding="utf-8")
        console_proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, text=True)
    else:
        run(cmd, check=True)
    time.sleep(args.launch_wait_seconds)
    if console_proc and console_proc.poll() is not None:
        raise RuntimeError(
            f"console launch exited early with {console_proc.returncode}; "
            f"see {args.launch_console_log}"
        )
    return console_proc


def stop_console_launch(proc):
    if not proc or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=2)


def drain(ws, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            ws.recv_json(timeout=min(0.05, max(0.01, deadline - time.monotonic())))
        except Exception:
            pass


def command(ws, name, args=None, timeout=10.0):
    ack = ws.command(name, args or {}, timeout=timeout)
    drain(ws, 0.1)
    return ack


def wait_for(ws, predicate, timeout, label):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        drain(ws, 0.1)
        if predicate():
            return
        time.sleep(0.1)
    raise TimeoutError(f"timed out waiting for {label}")


class NdiMarkerWatcher:
    def __init__(self, probe, sender_name, workdir, *, run_timeout_ms, find_timeout_ms):
        self.probe = probe
        self.sender_name = sender_name
        self.run_timeout_ms = run_timeout_ms
        self.find_timeout_ms = find_timeout_ms
        self.proc = None
        self.log = (workdir / "ndi-recv-probe.log").open("w", encoding="utf-8")
        self.ready = False
        self.last_markers = []
        self.started_at = None
        self.last_drained_marker = None
        self.last_drained_frames_decoded = None
        self._lines = queue.SimpleQueue()
        self._reader_done = threading.Event()
        self._reader_thread = None

    def start(self):
        self.started_at = time.monotonic()
        env = os.environ.copy()
        env["OLR_NDI_FIND_TIMEOUT_MS"] = str(self.find_timeout_ms)
        self.proc = subprocess.Popen(
            [
                self.probe,
                "--stream-markers",
                self.sender_name,
                str(self.run_timeout_ms),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            env=env,
        )
        self._reader_thread = threading.Thread(
            target=self._reader_loop,
            name="olr-ios-ndi-probe-reader",
            daemon=True,
        )
        self._reader_thread.start()

    def _reader_loop(self):
        try:
            if not self.proc or not self.proc.stdout:
                return
            for raw in self.proc.stdout:
                arrived_at = time.perf_counter()
                line = raw.rstrip()
                self.log.write(line + "\n")
                self.log.flush()
                self._lines.put((arrived_at, line))
        finally:
            self._reader_done.set()

    def _read_line_samples(self, timeout):
        if not self.proc or not self.proc.stdout:
            raise RuntimeError("NDI marker watcher was not started")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            wait = max(0.0, min(0.1, deadline - time.monotonic()))
            try:
                yield self._lines.get(timeout=wait)
            except queue.Empty:
                if self.proc.poll() is not None:
                    break
                continue

    def _read_lines(self, timeout):
        for _, line in self._read_line_samples(timeout):
            yield line

    def wait_ready(self, timeout):
        if self.ready:
            return
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for line in self._read_lines(max(0.01, deadline - time.monotonic())):
                if line.startswith("NDIWAIT "):
                    self.ready = True
                    print(f"NDI_READY sender={self.sender_name}")
                    return
            if self.proc and self.proc.poll() is not None:
                raise RuntimeError(f"ndi_recv_probe exited before source was ready; see {self.log.name}")
        raise TimeoutError(f"timed out waiting for NDI source '{self.sender_name}'")

    def _marker_fields(self, line):
        if not line.startswith("NDIMARKER "):
            return None
        fields = {}
        for part in line.split()[1:]:
            if "=" not in part:
                continue
            key, value = part.split("=", 1)
            fields[key] = value
        if "marker" not in fields:
            return None
        return fields

    def drain(self):
        drained = 0
        last_marker = None
        last_frames_decoded = None
        while True:
            samples = []
            try:
                while True:
                    samples.append(self._lines.get_nowait())
            except queue.Empty:
                pass
            if not samples:
                break
            for _, line in samples:
                fields = self._marker_fields(line)
                if fields:
                    last_marker = int(fields["marker"])
                    if "framesDecoded" in fields:
                        last_frames_decoded = int(fields["framesDecoded"])
                drained += 1
        self.last_drained_marker = last_marker
        self.last_drained_frames_decoded = last_frames_decoded
        return last_frames_decoded

    def elapsed_ms(self):
        if self.started_at is None:
            return 0
        return int((time.monotonic() - self.started_at) * 1000)

    def _wait_for_marker_sample(self, expected, label, timeout, *, min_frames_decoded=None,
                                latency_started_at=None):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for arrived_at, line in self._read_line_samples(max(0.01, deadline - time.monotonic())):
                fields = self._marker_fields(line)
                if not fields:
                    continue
                marker = int(fields["marker"])
                frames_decoded = int(fields.get("framesDecoded", "-1"))
                elapsed_ms = int(fields.get("elapsedMs", "-1"))
                self.last_markers.append((marker, elapsed_ms, frames_decoded))
                self.last_markers = self.last_markers[-12:]
                if min_frames_decoded is not None and frames_decoded <= min_frames_decoded:
                    continue
                if marker == expected:
                    latency_ms = (
                        (arrived_at - latency_started_at) * 1000.0
                        if latency_started_at is not None else None
                    )
                    print(
                        f"NDI_MATCH {label} marker={marker} elapsedMs={elapsed_ms} "
                        f"framesDecoded={frames_decoded}"
                    )
                    return {
                        "marker": marker,
                        "probeElapsedMs": elapsed_ms,
                        "framesDecoded": frames_decoded,
                        "latencyMs": latency_ms,
                        "line": line,
                    }
            if self.proc and self.proc.poll() is not None:
                raise RuntimeError(f"ndi_recv_probe exited while waiting for {label}; see {self.log.name}")
        raise AssertionError(
            f"NDI PGM did not show marker {expected} for {label}; "
            f"recent={self.last_markers} log={self.log.name}"
        )

    def wait_for_marker(self, expected, label, timeout, *, min_frames_decoded=None):
        return self._wait_for_marker_sample(
            expected,
            label,
            timeout,
            min_frames_decoded=min_frames_decoded,
        )["marker"]

    def wait_for_marker_with_latency(self, expected, label, timeout, latency_started_at,
                                     threshold_ms, *, min_frames_decoded=None):
        sample = self._wait_for_marker_sample(
            expected,
            label,
            timeout,
            min_frames_decoded=min_frames_decoded,
            latency_started_at=latency_started_at,
        )
        latency_ms = sample["latencyMs"]
        if latency_ms is None:
            raise AssertionError(f"{label}: NDI latency sample missing")
        sample["thresholdMs"] = threshold_ms
        return sample

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2)
        if self._reader_thread:
            self._reader_thread.join(timeout=2)
        self.log.close()


def require_pgm_transaction(completion, label):
    transaction = completion.get("pgmTransaction") if isinstance(completion, dict) else None
    if not isinstance(transaction, dict):
        raise AssertionError(f"{label}: command.completed missing pgmTransaction: {completion}")
    if not transaction.get("completed") or not transaction.get("submittedPgm"):
        raise AssertionError(f"{label}: command.completed did not complete PGM submit: {completion}")
    if transaction.get("timedOut"):
        raise AssertionError(f"{label}: PGM transaction timed out: {completion}")
    identity = transaction.get("identity", {})
    print(
        "PGM_COMPLETED "
        f"{label} elapsedNs={transaction.get('elapsedNs')} "
        f"targetMs={transaction.get('targetMs')} "
        f"sampledMs={identity.get('sampledPlayheadMs')} "
        f"sourcePtsMs={identity.get('sourcePtsMs')} "
        f"placeholder={identity.get('videoPlaceholder')}"
    )
    return transaction


def send_command_for_latency(ws, name, args=None):
    cmd_id = f"{name}-{int(time.time() * 1000)}-{os.getpid()}-latency"
    command_args = dict(args or {})
    if name in ("transport.seek", "transport.stepFrame", "action.jog"):
        command_args.setdefault("waitForPgm", True)
    ws.send_json({"type": "command", "id": cmd_id, "name": name, "args": command_args})
    return cmd_id


def wait_for_command_ack(ws, cmd_id, name, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        msg = ws.recv_json(max(0.1, deadline - time.monotonic()))
        if msg.get("type") == "ack" and msg.get("id") == cmd_id:
            if not msg.get("ok"):
                raise RuntimeError(f"{name} failed: {msg}")
            return msg
    raise TimeoutError(f"timed out waiting for ack to {name}")


def wait_for_command_completed(ws, cmd_id, name, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        msg = ws.recv_json(max(0.1, deadline - time.monotonic()))
        if (msg.get("type") == "event" and msg.get("name") == "command.completed"
                and msg.get("data", {}).get("id") == cmd_id):
            return msg["data"]
    raise TimeoutError(f"timed out waiting for command.completed to {name}")


def run_command_with_ndi_latency(args, ws, ndi, label, expected_marker, name,
                                 command_args=None, *, timeout=10.0):
    if not ndi:
        return command(ws, name, command_args or {}, timeout=timeout), None

    min_frames_decoded = ndi.drain()
    started = time.perf_counter()
    cmd_id = send_command_for_latency(ws, name, command_args or {})
    # Mirror the two-phase contract used elsewhere in this oracle (see
    # WsClient.send_command_and_wait_completed): the ack is read - and
    # ack_elapsed_ms is timestamped - as soon as it actually arrives, strictly
    # before we wait on anything else. That keeps the round-trip measurement
    # isolated from the NDI marker and command.completed waits below, both of
    # which are only expected to land once the PGM outcome is ready; reading
    # completion only after the ack is in hand also rules out the "completion
    # before its ack" ordering bug the two-phase helper guards against.
    wait_for_command_ack(ws, cmd_id, name, timeout)
    ack_elapsed_ms = (time.perf_counter() - started) * 1000.0
    sample = ndi.wait_for_marker_with_latency(
        expected_marker,
        label,
        args.ndi_marker_timeout,
        started,
        args.latency_threshold_ms,
        min_frames_decoded=min_frames_decoded,
    )
    command_completion = wait_for_command_completed(ws, cmd_id, name, timeout)
    command_elapsed_ms = (time.perf_counter() - started) * 1000.0
    if ack_elapsed_ms >= 100.0:
        raise AssertionError(
            "transactional command ack round-trip exceeded 100 ms "
            f"label={label} name={name} ackElapsedMs={ack_elapsed_ms:.2f}"
        )
    transaction = (
        command_completion.get("pgmTransaction")
        if isinstance(command_completion, dict) else None
    )
    transaction_elapsed_ns = transaction.get("elapsedNs") if isinstance(transaction, dict) else "?"
    completion_latency_ms = (
        command_completion.get("latencyMs") if isinstance(command_completion, dict) else "?"
    )
    latency_ms = sample["latencyMs"]
    if latency_ms > args.latency_threshold_ms:
        raise AssertionError(
            "PGM NDI marker latency exceeded threshold "
            f"label={label} expected={expected_marker} elapsedMs={latency_ms:.2f} "
            f"max_ndi_latency_ms={args.latency_threshold_ms:.2f} "
            f"commandElapsedMs={command_elapsed_ms:.2f} "
            f"ackElapsedMs={ack_elapsed_ms:.2f} "
            f"completionLatencyMs={completion_latency_ms} "
            f"pgmTransactionElapsedNs={transaction_elapsed_ns} "
            f"lastDrainedMarker={ndi.last_drained_marker} "
            f"lastDrainedFramesDecoded={ndi.last_drained_frames_decoded} "
            f"line={sample['line']}"
        )
    print(
        "NDI_LATENCY "
        f"label={label} marker={expected_marker} elapsedMs={latency_ms:.2f} "
        f"max_ndi_latency_ms={args.latency_threshold_ms:.2f} "
        f"commandElapsedMs={command_elapsed_ms:.2f} "
        f"ackElapsedMs={ack_elapsed_ms:.2f} "
        f"completionLatencyMs={completion_latency_ms} "
        f"pgmTransactionElapsedNs={transaction_elapsed_ns} "
        f"framesDecoded={sample['framesDecoded']} "
        f"lastDrainedMarker={ndi.last_drained_marker} "
        f"lastDrainedFramesDecoded={ndi.last_drained_frames_decoded}"
    )
    sample["commandElapsedMs"] = command_elapsed_ms
    return command_completion, sample


def decode_marker(args, jpeg):
    ff = subprocess.Popen(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            str(jpeg),
            "-vf",
            "scale=256:144",
            "-f",
            "rawvideo",
            "-pix_fmt",
            "gray",
            "-",
        ],
        stdout=subprocess.PIPE,
    )
    probe = subprocess.run(
        [args.marker_probe, "256", "144"],
        stdin=ff.stdout,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if ff.stdout:
        ff.stdout.close()
    ff_rc = ff.wait()
    if ff_rc != 0 or probe.returncode != 0:
        raise RuntimeError(
            f"marker decode failed for {jpeg}: ffmpeg={ff_rc} probe={probe.returncode} "
            f"stdout={probe.stdout} stderr={probe.stderr}"
        )
    match = re.search(r"firstIndex=(\d+)", probe.stdout)
    if not match:
        raise RuntimeError(f"marker probe did not report firstIndex: {probe.stdout}")
    return int(match.group(1)), probe.stdout.strip()


def capture_marker(args, ws, workdir, label):
    safe_label = re.sub(r"[^A-Za-z0-9_.-]+", "_", label)
    prefix = f"ipad_marker_oracle_{safe_label}"
    command(ws, "settings.setProject", {"fileName": prefix}, timeout=5)
    before = {
        f["name"]
        for f in device_files(args, "Documents/videos", prefix, workdir)
        if f["name"].endswith(".jpg")
    }
    command(ws, "capture.current", timeout=10)
    deadline = time.monotonic() + args.capture_timeout
    while time.monotonic() < deadline:
        files = device_files(args, "Documents/videos", prefix, workdir)
        candidates = [
            f
            for f in files
            if "_PGM_" in f["name"] and f["name"].endswith(".jpg") and f["name"] not in before
        ]
        if candidates:
            candidates.sort(key=lambda f: (f["metadata"].get("lastModDate", ""), f["name"]))
            selected = candidates[-1]
            capture_dir = workdir / "captures"
            capture_dir.mkdir(exist_ok=True)
            dest = capture_dir / selected["name"]
            device_copy_from(args, "Documents/videos/" + selected["relativePath"], dest)
            marker, probe_line = decode_marker(args, dest)
            return marker, dest, probe_line
        time.sleep(0.08)
    raise TimeoutError(f"capture.current did not produce a PGM JPEG for {label}")


def parse_steps(text):
    steps = []
    for chunk in text.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        match = re.fullmatch(r"([+-]?\d+)x(\d+)", chunk)
        if not match:
            raise ValueError(f"invalid step segment '{chunk}', expected -1x60,+1x15")
        delta = int(match.group(1))
        count = int(match.group(2))
        if delta == 0 or count <= 0:
            raise ValueError(f"invalid step segment '{chunk}'")
        steps.extend([delta] * count)
    return steps


def parse_int_list(text):
    values = []
    for chunk in text.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        values.append(int(chunk))
    return values


def frame_index_to_ms(frame_index, fps):
    return 0 if frame_index <= 0 else int(frame_index * 1000 / fps)


def ms_to_frame_index(ms, fps):
    return 0 if ms <= 0 else int(ms * fps / 1000)


def require_cold_seek_distance(current_frame, target_frame, fps, min_distance_ms):
    current_ms = frame_index_to_ms(current_frame, fps)
    target_ms = frame_index_to_ms(target_frame, fps)
    distance_ms = abs(target_ms - current_ms)
    if distance_ms <= min_distance_ms:
        raise AssertionError(
            "cold seek target must be more than the configured distance from current playhead "
            f"currentFrame={current_frame} targetFrame={target_frame} "
            f"distanceMs={distance_ms} minDistanceMs={min_distance_ms}"
        )
    return distance_ms


def run_oracle(args, workdir, srt_url):
    ws = WsClient(args.app_host, args.control_port, timeout=5)
    ndi = None
    results = []
    ndi_latency_samples = []
    failures = []
    try:
        ws.wait_for(lambda: bool(ws.state), 5, "initial websocket state")
        drain(ws, 0.5)

        if args.stop_active_recording and ws.state.get("recording", {}).get("active"):
            command(ws, "recording.stop", timeout=10)
            wait_for(ws, lambda: not ws.state.get("recording", {}).get("active"), 10,
                     "recording stopped")

        command(ws, "settings.setProject", {"fileName": args.project}, timeout=5)
        command(
            ws,
            "settings.setRecordingFormat",
            {"width": 256, "height": 144, "fps": args.fps},
            timeout=5,
        )
        command(ws, "view.setPlaybackViewState", {"singleView": True, "selectedIndex": 0})
        if args.ndi_recv_probe:
            sender = args.ndi_sender_name or f"{args.project} PGM NDI"
            command(
                ws,
                "outputs.ndi.setSenderName",
                {"busKind": "pgm", "feedIndex": -1, "senderName": sender},
                timeout=5,
            )
            command(
                ws,
                "outputs.ndi.setEnabled",
                {"busKind": "pgm", "feedIndex": -1, "enabled": True},
                timeout=5,
            )
            ndi = NdiMarkerWatcher(
                args.ndi_recv_probe,
                sender,
                workdir,
                run_timeout_ms=args.ndi_probe_timeout_ms,
                find_timeout_ms=args.ndi_find_timeout_ms,
            )
            ndi.start()
        command(ws, "recording.start", timeout=10)
        wait_for(ws, lambda: ws.state.get("recording", {}).get("active"), 10,
                 "recording active")
        wait_for(
            ws,
            lambda: bool(ws.state.get("sources", [{}])[0].get("connected")),
            args.source_timeout,
            f"source connected to {srt_url}",
        )
        wait_for(
            ws,
            lambda: int(ws.state.get("recording", {}).get("durationMs", 0))
            >= args.warmup_seconds * 1000,
            args.warmup_seconds + 20,
            "warmup duration",
        )
        if ndi:
            ndi.wait_ready(args.ndi_ready_timeout)

        command(ws, "transport.pause", timeout=5)
        seek_min_frames = ndi.drain() if ndi else None
        _seek_ack, seek_completion, seek_ack_elapsed_ms = ws.send_command_and_wait_completed(
            "transport.seek",
            {"positionMs": args.target_ms, "waitForPgm": True},
            timeout=10,
        )
        if seek_ack_elapsed_ms >= 100.0:
            raise AssertionError(
                "transactional command ack round-trip exceeded 100 ms "
                f"label=seek_target ackElapsedMs={seek_ack_elapsed_ms:.2f}"
            )
        require_pgm_transaction(seek_completion, "seek_target")
        time.sleep(args.step_delay_seconds)

        base_marker, path, probe = capture_marker(args, ws, workdir, "seek_target")
        ndi_marker = ndi.wait_for_marker(
            base_marker,
            "seek_target",
            args.ndi_marker_timeout,
            min_frames_decoded=seek_min_frames,
        ) if ndi else None
        results.append(
            {
                "label": "seek_target",
                "positionMs": args.target_ms,
                "marker": base_marker,
                "ndiMarker": ndi_marker,
                "expectedMarker": base_marker,
                "ok": True,
                "pgmTransaction": seek_completion.get("pgmTransaction"),
                "path": str(path),
                "probe": probe,
            }
        )
        print(f"CAPTURE seek_target marker={base_marker} path={path}")

        base_frame = ms_to_frame_index(args.target_ms, args.fps)
        cumulative = 0
        steps = parse_steps(args.steps)
        for index, delta in enumerate(steps, start=1):
            before = int(ws.state.get("transport", {}).get("positionMs", -1))
            label = f"step_{index:03d}_{'fwd' if delta > 0 else 'back'}"
            expected = base_marker + cumulative + delta
            step_completion, ndi_sample = run_command_with_ndi_latency(
                args,
                ws,
                ndi,
                label,
                expected,
                "transport.stepFrame",
                {"frames": delta},
                timeout=10,
            )
            require_pgm_transaction(step_completion, label)
            if ndi_sample:
                ndi_latency_samples.append(ndi_sample["latencyMs"])
            time.sleep(args.step_delay_seconds)
            drain(ws, 0.2)
            position = int(ws.state.get("transport", {}).get("positionMs", -1))
            cumulative += delta
            marker, path, probe = capture_marker(args, ws, workdir, label)
            ndi_marker = ndi_sample["marker"] if ndi_sample else None
            ok = marker == expected and (ndi_marker is None or ndi_marker == expected)
            row = {
                "label": label,
                "delta": delta,
                "beforeMs": before,
                "positionMs": position,
                "marker": marker,
                "ndiMarker": ndi_marker,
                "expectedMarker": expected,
                "ok": ok,
                "pgmTransaction": step_completion.get("pgmTransaction"),
                "ndiLatencyMs": ndi_sample["latencyMs"] if ndi_sample else None,
                "path": str(path),
                "probe": probe,
            }
            results.append(row)
            if not ok:
                failures.append(row)
            print(
                f"STEP {label} delta={delta} beforeMs={before} positionMs={position} "
                f"marker={marker} ndiMarker={ndi_marker} expected={expected} ok={ok}"
            )

        current_frame = base_frame + cumulative
        cold_seek_count = 0
        for index, target_frame in enumerate(parse_int_list(args.cold_seek_frames), start=1):
            distance_ms = require_cold_seek_distance(
                current_frame, target_frame, args.fps, args.cold_seek_min_distance_ms
            )
            target_ms = frame_index_to_ms(target_frame, args.fps)
            expected = base_marker + (target_frame - base_frame)
            label = f"cold_seek_{index:02d}"
            cold_completion, ndi_sample = run_command_with_ndi_latency(
                args,
                ws,
                ndi,
                label,
                expected,
                "transport.seek",
                {"positionMs": target_ms},
                timeout=10,
            )
            require_pgm_transaction(cold_completion, label)
            if ndi_sample:
                ndi_latency_samples.append(ndi_sample["latencyMs"])
            time.sleep(args.step_delay_seconds)
            drain(ws, 0.2)
            marker, path, probe = capture_marker(args, ws, workdir, label)
            ndi_marker = ndi_sample["marker"] if ndi_sample else None
            ok = marker == expected and (ndi_marker is None or ndi_marker == expected)
            row = {
                "label": label,
                "targetFrame": target_frame,
                "targetMs": target_ms,
                "distanceMs": distance_ms,
                "marker": marker,
                "ndiMarker": ndi_marker,
                "expectedMarker": expected,
                "ok": ok,
                "pgmTransaction": cold_completion.get("pgmTransaction"),
                "ndiLatencyMs": ndi_sample["latencyMs"] if ndi_sample else None,
                "path": str(path),
                "probe": probe,
            }
            results.append(row)
            if not ok:
                failures.append(row)
            cold_seek_count += 1
            current_frame = target_frame
            print(
                f"COLD_SEEK {label} targetFrame={target_frame} targetMs={target_ms} "
                f"distanceMs={distance_ms} marker={marker} ndiMarker={ndi_marker} "
                f"expected={expected} ok={ok}"
            )

        summary = {
            "failCount": len(failures),
            "stepCount": len(steps),
            "coldSeekCount": cold_seek_count,
            "firstMarker": base_marker,
            "lastMarker": results[-1]["marker"],
            "ndiLatencySamples": len(ndi_latency_samples),
            "expectedNdiLatencySamples": len(steps) + cold_seek_count if ndi else 0,
            "maxNdiLatencyMs": max(ndi_latency_samples) if ndi_latency_samples else None,
            "latencyThresholdMs": args.latency_threshold_ms if ndi else None,
            "srtUrl": srt_url,
        }
        (workdir / "results.json").write_text(
            json.dumps({"summary": summary, "results": results}, indent=2), encoding="utf-8"
        )
        print("SUMMARY " + json.dumps(summary, sort_keys=True))
        return 1 if failures else 0
    finally:
        try:
            if ws.state.get("recording", {}).get("active") and not args.keep_recording:
                command(ws, "recording.stop", timeout=10)
        finally:
            if ndi:
                ndi.stop()
            ws.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="CoreDevice identifier from devicectl")
    parser.add_argument("--app-host", required=True, help="iPad Wi-Fi IP/host for WebSocket")
    parser.add_argument("--mac-host", required=True, help="Mac LAN IP reachable from iPad SRT")
    parser.add_argument("--marker-source", required=True, help="ndi_marker_mkv_source executable")
    parser.add_argument("--marker-probe", required=True, help="marker_yuv_probe executable")
    parser.add_argument("--ndi-recv-probe", help="ndi_recv_probe executable for PGM NDI marker validation")
    parser.add_argument("--ndi-sender-name", default="")
    parser.add_argument("--ndi-ready-timeout", type=float, default=30.0)
    parser.add_argument("--ndi-marker-timeout", type=float, default=2.5)
    parser.add_argument("--ndi-probe-timeout-ms", type=int, default=900000)
    parser.add_argument("--ndi-find-timeout-ms", type=int, default=30000)
    parser.add_argument("--latency-threshold-ms", type=float, default=25.0)
    parser.add_argument("--bundle", default=DEFAULT_BUNDLE)
    parser.add_argument("--control-port", type=int, default=8115)
    parser.add_argument("--srt-port", type=int, default=32900)
    parser.add_argument("--udp-port", type=int, default=32901)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--marker-seconds", type=int, default=180)
    parser.add_argument("--warmup-seconds", type=int, default=30)
    parser.add_argument("--target-ms", type=int, default=30000)
    parser.add_argument("--step-delay-seconds", type=float, default=0.5)
    parser.add_argument("--capture-timeout", type=float, default=8.0)
    parser.add_argument("--source-timeout", type=float, default=25.0)
    parser.add_argument("--steps", default="-1x60,+1x15,-1x30")
    parser.add_argument("--cold-seek-frames", default="")
    parser.add_argument("--cold-seek-min-distance-ms", type=int, default=5000)
    parser.add_argument("--project", default="ipad_marker_oracle")
    parser.add_argument("--workdir", type=Path)
    parser.add_argument("--keep-workdir", action="store_true")
    parser.add_argument("--skip-config-push", action="store_true")
    parser.add_argument("--no-restore-config", action="store_true")
    parser.add_argument("--keep-recording", action="store_true")
    parser.add_argument("--launch-app", action="store_true",
                        help="relaunch the iOS app with GPU and WebSocket test env after config push")
    parser.add_argument("--launch-wait-seconds", type=float, default=4.0)
    parser.add_argument("--launch-console-log", type=Path,
                        help="capture CoreDevice app console output while the oracle runs")
    parser.add_argument("--launch-without-gpu", dest="launch_gpu_pipeline", action="store_false")
    parser.add_argument("--no-stop-active-recording", dest="stop_active_recording",
                        action="store_false")
    parser.set_defaults(stop_active_recording=True, launch_gpu_pipeline=True)
    args = parser.parse_args()

    for tool in ("ffmpeg", "srt-live-transmit", "xcrun"):
        require_tool(tool)
    if not Path(args.marker_source).exists():
        raise RuntimeError(f"marker source not found: {args.marker_source}")
    if not Path(args.marker_probe).exists():
        raise RuntimeError(f"marker probe not found: {args.marker_probe}")
    if args.ndi_recv_probe and not Path(args.ndi_recv_probe).exists():
        raise RuntimeError(f"NDI receiver probe not found: {args.ndi_recv_probe}")

    if args.workdir:
        workdir = args.workdir
        workdir.mkdir(parents=True, exist_ok=True)
        cleanup_workdir = False
    else:
        workdir = Path(tempfile.mkdtemp(prefix="olr-ios-marker-oracle-"))
        cleanup_workdir = not args.keep_workdir

    producer_processes = []
    producer_logs = []
    console_process = None
    backup = None
    srt_url = f"srt://{args.mac_host}:{args.srt_port}?transtype=live"
    print(f"IOS_MARKER_ORACLE workdir={workdir} srtUrl={srt_url}")
    try:
        marker_mkv = generate_marker_fixture(args, workdir)
        producer_processes, producer_logs = start_srt_marker_producer(args, marker_mkv, workdir)
        if not args.skip_config_push:
            backup = push_marker_config(args, workdir, srt_url)
            print("IOS_MARKER_ORACLE pushed marker config to iOS app container")
        if args.launch_app:
            print("IOS_MARKER_ORACLE relaunching app with OLR_CONTROL_BIND=any")
            console_process = launch_app(args)
        elif not args.skip_config_push:
            print("IOS_MARKER_ORACLE relaunch the app after config push if it was already running")
        rc = run_oracle(args, workdir, srt_url)
        return rc
    finally:
        stop_console_launch(console_process)
        if not args.no_restore_config:
            try:
                restore_config(args, backup)
            except Exception as exc:
                print(f"WARN: failed to restore app config: {exc}", file=sys.stderr)
        stop_processes(producer_processes)
        for handle in producer_logs:
            handle.close()
        if cleanup_workdir:
            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"IOS_MARKER_ORACLE kept workdir={workdir}")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
