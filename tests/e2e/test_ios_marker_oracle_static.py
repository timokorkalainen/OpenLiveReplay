#!/usr/bin/env python3
import sys
from pathlib import Path


def require(text: str, needle: str, message: str) -> None:
    if needle not in text:
        raise AssertionError(message)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_ios_marker_oracle_static.py <ios_marker_srt_oracle.py>")

    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    require(source, "--ndi-recv-probe", "iOS oracle must expose a PGM NDI receiver probe option")
    require(source, "--ndi-sender-name", "iOS oracle must allow a stable PGM NDI sender name")
    require(source, "outputs.ndi.setSenderName", "iOS oracle must configure PGM NDI sender name")
    require(source, "outputs.ndi.setEnabled", "iOS oracle must enable PGM NDI output")
    require(source, "NDIMARKER", "iOS oracle must consume marker lines from ndi_recv_probe")
    require(source, "wait_for_marker", "iOS oracle must wait for expected PGM NDI markers")
    require(source, "pgmTransaction", "iOS oracle must verify websocket PGM transaction ACKs")
    require(source, "waitForPgm", "iOS oracle must explicitly request PGM transaction ACKs")
    require(source, "framesDecoded", "iOS oracle must drain stale NDI receiver frames")
    require(source, "--latency-threshold-ms", "iOS oracle must expose a command-to-PGM latency threshold")
    require(source, "run_command_with_ndi_latency", "iOS oracle must measure command-to-PGM NDI latency")
    require(source, "send_command_for_latency", "iOS oracle latency timing must send the WebSocket command synchronously")
    require(source, "NDI_LATENCY", "iOS oracle must log per-command NDI latency samples")
    require(source, "OLR_E2E_LATENCY_TRACE", "iOS oracle must launch the app with latency tracing")
    require(source, "--launch-console-log", "iOS oracle must be able to capture device console latency logs")
    require(source, "threading.Thread", "iOS oracle must continuously drain ndi_recv_probe stdout")
    require(source, "_reader_loop", "iOS oracle must keep the NDI probe pipe drained during warmup/capture")
    require(source, "arrived_at", "iOS oracle latency must use the reader arrival timestamp")
    if "(time.perf_counter() - latency_started_at)" in source:
        raise AssertionError(
            "iOS oracle latency timing must use the reader arrival timestamp, "
            "not delayed main-thread scheduling"
        )
    send_start = source.find("def send_command_for_latency")
    send_end = source.find("def wait_for_command_ack", send_start)
    if send_start < 0 or send_end < 0:
        raise AssertionError("iOS oracle must expose send_command_for_latency before ACK wait")
    if "threading.Thread" in source[send_start:send_end]:
        raise AssertionError("iOS oracle command timing must not add a send-side Python thread")
    require(source, "--cold-seek-frames", "iOS oracle must include cold seek coverage")
    require(source, "coldSeekCount", "iOS oracle summary must report cold seek coverage")
    wait_index = source.find("run_command_with_ndi_latency(")
    capture_index = source.find("capture_marker(args, ws, workdir, label)")
    if wait_index < 0 or capture_index < 0 or wait_index > capture_index:
        raise AssertionError("iOS oracle must wait for expected PGM NDI before slower preview capture")
    print("PASS: iOS marker oracle exposes PGM NDI marker validation")


if __name__ == "__main__":
    main()
