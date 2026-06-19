#!/usr/bin/env bash
# Tier (b): play a marker MKV through the real PlaybackWorker with NDI output enabled and
# verify the captured NDI output is continuous, A-V synced, and steady. Opt-in (label
# "ndi-output"); SKIP (77) when ffmpeg or the NDI runtime is unavailable.
#
# Usage: run_ndi_playback_e2e.sh <ndi_marker_mkv_source> <play_harness> <ndi_recv_probe>
set -uo pipefail
SKIP=77

SRC="${1:?ndi_marker_mkv_source required}"
PLAY="${2:?play_harness required}"
PROBE="${3:?ndi_recv_probe required}"
SECONDS_RUN="${OLR_NDI_PLAYBACK_SECONDS:-6}"
SENDER="OLR NDI Playback Probe $$"

command -v ffmpeg >/dev/null || { echo "SKIP: ffmpeg not found"; exit "$SKIP"; }

WORK="$(mktemp -d)"
PLAY_PID=""
cleanup() { [ -n "$PLAY_PID" ] && kill "$PLAY_PID" 2>/dev/null; wait "$PLAY_PID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

# 1. Generate the marker as raw planes (a couple seconds longer than the capture window).
"$SRC" "$WORK/m" "$((SECONDS_RUN + 4))" || { echo "FAIL: marker source"; exit 1; }

# 2. Mux to a worker-decodable MKV. ffv1 is lossless so the counter cells survive bit-exact.
#    The setpts+enc_time_base flags force PTS = floor(N*1000/30) so the output clock (which
#    also uses floor truncation) samples a unique source frame each tick with no dupe/drop.
if ! ffmpeg -loglevel error -y \
        -f rawvideo -pix_fmt yuv420p -s 256x144 -r 30 -i "$WORK/m.yuv" \
        -f s16le -ar 48000 -ac 2 -i "$WORK/m.pcm" \
        -vf "settb=1/1000,setpts=trunc(N*1000/30)" \
        -c:v ffv1 -enc_time_base 1:1000 -c:a pcm_s16le "$WORK/marker.mkv"; then
    echo "FAIL: ffmpeg mux"; exit 1
fi

# 3. Play it with NDI output enabled (background); give the source time to register.
OLR_NDI_OUTPUT_SENDER="$SENDER" "$PLAY" "$WORK/marker.mkv" play1x 1 > "$WORK/play.log" 2>&1 &
PLAY_PID=$!
sleep 2
if ! kill -0 "$PLAY_PID" 2>/dev/null; then
    wait "$PLAY_PID"; rc=$?
    if [ "$rc" = "$SKIP" ]; then echo "SKIP: player exited 77 (no NDI runtime)"; exit "$SKIP"; fi
    echo "FAIL: player exited early ($rc)"; cat "$WORK/play.log"; exit 1
fi

# 4. Capture + measure.
OUT="$("$PROBE" "$SENDER" "$SECONDS_RUN")"; rc=$?
echo "$OUT"
if [ "$rc" = "$SKIP" ]; then echo "SKIP: NDI runtime/source not available (probe)"; exit "$SKIP"; fi
if [ "$rc" != "0" ]; then echo "FAIL: probe error ($rc)"; cat "$WORK/play.log"; exit 1; fi

# 5. Wait for the play_harness to finish so its COUNTERS line is in the log.
#    The play1x scenario runs for 12 s after a 1.5 s warmup (total ~13.5 s from launch);
#    the probe window ends at ~8 s from launch, so the player is still running here.
wait "$PLAY_PID" 2>/dev/null; PLAY_PID=""

line="$(grep '^NDIRECV ' <<<"$OUT" || true)"
[ -n "$line" ] || { echo "FAIL: no NDIRECV report"; cat "$WORK/play.log"; exit 1; }
field() { sed -n "s/.*$1=\\([0-9.-]*\\).*/\\1/p" <<<"$line"; }
frames=$(field framesReceived); drops=$(field drops); dupes=$(field dupes)
reorders=$(field reorders); avsync=$(field avSyncMaxFrames); maxgap=$(field maxGapFrames)

# Worker playback health from the COUNTERS line.
counters="$(grep '^COUNTERS ' "$WORK/play.log" || true)"
cfield() { sed -n "s/.*$1=\\([0-9-]*\\).*/\\1/p" <<<"$counters"; }
reposition=$(cfield reposition); audioPushes=$(cfield audioPushes)

fail=0
[ "${frames:-0}" -ge "$((SECONDS_RUN * 15))" ] || { echo "FAIL: too few frames ($frames)"; fail=1; }
[ "${drops:-1}" = "0" ]    || { echo "FAIL: drops=$drops"; fail=1; }
[ "${dupes:-1}" = "0" ]    || { echo "FAIL: dupes=$dupes"; fail=1; }
[ "${reorders:-1}" = "0" ] || { echo "FAIL: reorders=$reorders"; fail=1; }
[ "${avsync:-9}" -ge 0 ] && [ "${avsync:-9}" -le 1 ] || { echo "FAIL: avSyncMaxFrames=$avsync"; fail=1; }
[ "${maxgap:-9}" -le 2 ]   || { echo "FAIL: maxGapFrames=$maxgap"; fail=1; }
[ "${reposition:-1}" = "0" ] || { echo "FAIL: worker reposition=$reposition"; fail=1; }
[ "${audioPushes:-0}" -gt 0 ] || { echo "WARN: audioPushes=$audioPushes (audio path idle)"; }

if [ "$fail" = "0" ]; then echo "PASS: NDI playback continuity/sync/cadence OK"; exit 0; fi
echo "NDI PLAYBACK VALIDATION FAILED"; cat "$WORK/play.log"; exit 1
