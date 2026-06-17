# Frame-Sync Phase 0 — Measurement & Acceptance Rig — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Build the instrument that *defines* "frame-perfect" — synchronized flash+beep+SMPTE-timecode markers fed through N encoders, an injectable clock-skew relay, and a measurement driver that records via the real engine and reports/gates lip-sync, inter-camera phase, long-run drift, and timecode accuracy.

**Architecture:** Pure test-tooling (bash + Python + the existing `sync_harness`), reusing `tests/e2e/srt_lib.sh` producers/extractors, `lossy_udp_relay.py`, and the `run_soak_matrix.sh` cell-runner pattern. No engine code changes — this measures the engine as-is, establishing a baseline the later phases improve. Bands start **report-only** and become **gated** as Phase 1 lands.

**Tech Stack:** bash, Python 3, ffmpeg/ffprobe, srt-live-transmit, CTest.

## Global Constraints
- Spec: `docs/superpowers/specs/2026-06-17-broadcast-framesync-program-design.md` (Phase 0).
- Worktree `/tmp/olr-bcast`. Local-only. Shell/Python are not clang-formatted.
- New ctest port band: **≥ 24000** (disjoint from record 23456-59, playback 23464-78, sync-report 23480-89, av-sync 23492, native 23550-23800, soak 23900).
- SKIP convention: `exit 77` + `SKIP_RETURN_CODE 77` when a tool/encoder is missing (mirrors `srt_require_tools` / `srt_hevc_vcodec_args`).
- Gate convention: a scenario is **report-only** by default and a **gate** when its `OLR_FRAMESYNC_GATE=1` env is set (mirrors `OLR_AV_SYNC_GATE` in `run_sync_e2e.sh`).
- Reuse, don't reinvent: `flash_marker_to_udps` / `flash_beep_marker_to_udp` / `flash_pts_series` / `beep_pts_series` / `srt_bridge` / `srt_caller_url` / `srt_require_tools` / `srt_hevc_vcodec_args` (`tests/e2e/srt_lib.sh`); `sync_harness` (N `--url`, 1:1 views, `--report-stats`); `run_cell` (`tests/e2e/run_soak_matrix.sh`); `lossy_udp_relay.py`.

---

### Task 1: `skew_injector` — a ppm clock-skew UDP relay

Extend `lossy_udp_relay.py` with a deterministic rate-skew mode so cross-device crystal drift can be simulated on one machine (the one thing the existing single-machine rig cannot do — source and recorder share a wall clock, so drift ≈ 0 by construction).

**Files:** Modify `tests/e2e/lossy_udp_relay.py`.

- [ ] **Step 1: Read** `tests/e2e/lossy_udp_relay.py` — note the `heapq` downstream release queue keyed on `release_at`, the `select` timeout computed from the next-due packet, the `reorder_ms` path, and `write_stats_and_exit`.

- [ ] **Step 2: Add a `skew_ppm` CLI arg + a deterministic accumulating-offset release.** Add `skew_ppm` as an optional 7th positional arg (after `reorder_ms`), default `0`. When `skew_ppm != 0`, downstream **DATA** packets (SRT control still passes immediately) are released on a stretched/compressed schedule: the k-th forwarded DATA packet is released at `base + k * mean_interval * (1 + skew_ppm/1e6)`, where `mean_interval` is an EWMA of observed inter-DATA-arrival gaps. Concretely, track `data_index`, `first_data_mono`, and `ewma_interval`; compute `release_at = first_data_mono + data_index * ewma_interval * (1 + skew_ppm/1e6)` and push onto the existing heap (reuse `release_due`). This forwards the stream at a slightly different rate → the engine, anchoring on arrival, sees a drifting source. Record `skew_ppm` + `data_forwarded` in the stats file.

```python
# near the arg parsing (after reorder_ms):
skew_ppm = float(sys.argv[7]) if len(sys.argv) > 7 else 0.0
# state:
data_index = 0
first_data_mono = None
ewma_interval = None  # seconds
last_data_mono = None
ALPHA = 0.05
# in the downstream DATA branch, instead of (or in addition to) reorder:
now = time.monotonic()
if skew_ppm != 0.0:
    if last_data_mono is not None:
        gap = now - last_data_mono
        ewma_interval = gap if ewma_interval is None else (1-ALPHA)*ewma_interval + ALPHA*gap
    last_data_mono = now
    if first_data_mono is None:
        first_data_mono = now
        release_at = now
    else:
        interval = ewma_interval if ewma_interval else (1.0/1000.0)
        release_at = first_data_mono + data_index * interval * (1.0 + skew_ppm/1e6)
    data_index += 1
    heapq.heappush(release_queue, (release_at, arrival_seq, data))
    arrival_seq += 1
else:
    # existing immediate / reorder path unchanged
```
(Adapt to the exact variable names in the file; keep the existing loss + reorder paths working when `skew_ppm == 0`.)

- [ ] **Step 3: Self-test the skew math** — add a `--selftest` short-circuit at the top of `main` (or a tiny separate assertion) that, given `skew_ppm` and a synthetic 1 kHz packet train, asserts the last release time ≈ `count * interval * (1 + skew_ppm/1e6)` within 1%. Run: `python3 tests/e2e/lossy_udp_relay.py --selftest` → prints `SELFTEST OK`.

- [ ] **Step 4: Commit** — `git add tests/e2e/lossy_udp_relay.py && git commit -m "test(framesync): add ppm clock-skew mode to lossy_udp_relay (skew_injector)"`.

---

### Task 2: SMPTE timecode marker producer + a `tmcd`/TC reader

The current markers (flash + beep) prove lip-sync and *relative* phase but carry **no absolute timecode** — which is the enabler for frame-accurate inter-camera alignment and the TC-accuracy metric. Add a TC-burning producer and a TC reader.

**Files:** Modify `tests/e2e/srt_lib.sh`.

- [ ] **Step 1: Add `flash_beep_tc_marker_to_udp`** to `srt_lib.sh` — a single producer with flash + beep (the existing 60 ms window) **plus** a burnt-in SMPTE timecode locked to the same source clock, MPEG-TS to one UDP port. Codec honors `OLR_FLASH_CODEC` (reuse `srt_hevc_vcodec_args`). The TC starts at a fixed `OLR_MARKER_TC` (default `10:00:00:00`) at 30 fps via ffmpeg's `drawtext`+`timecode`:
```bash
# Usage: flash_beep_tc_marker_to_udp <udp_port>
flash_beep_tc_marker_to_udp() {
    local port="$1"
    local tc="${OLR_MARKER_TC:-10\\:00\\:00\\:00}"
    local vflt="geq=lum='if(lt(mod(T,1),0.06),235,16)':cb=128:cr=128,drawtext=fontfile=:text='':timecode='${tc}':r=30:fontsize=20:fontcolor=white:x=10:y=10"
    local vargs
    if [ "${OLR_FLASH_CODEC:-avc}" = "hevc" ]; then
        srt_hevc_vcodec_args || { echo "SKIP: no HEVC encoder for TC marker"; exit 77; }
        vargs=("${SRT_HEVC_VCODEC_ARGS[@]}")
    else
        vargs=(-c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M)
    fi
    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "color=c=black:s=320x240:r=30" \
        -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
        -filter_complex "[0:v]${vflt}[v];[1:a]volume=volume='if(lt(mod(t,1),0.06),1,0)':eval=frame[a]" \
        -map "[v]" -map "[a]" "${vargs[@]}" \
        -c:a aac -b:a 128k -timecode "${OLR_MARKER_TC:-10:00:00:00}" \
        -f mpegts "udp://127.0.0.1:${port}?pkt_size=1316" &
    SRT_LAST_PID=$!
    PIDS+=("$SRT_LAST_PID")
}
```
(The burnt-in `drawtext timecode` is the *visible* TC the rig can OCR-free verify by counting frames; the container `-timecode` sets the stream start TC. The engine's TC extraction is Phase 3 — for the rig, the *injected* start TC + 30 fps cadence is the ground truth to compare the recorded `tmcd` against.)

- [ ] **Step 2: Add `mkv_start_timecode`** to `srt_lib.sh` — read the recorded MKV's timecode via ffprobe (`tmcd` track or `format_tags=timecode`):
```bash
# Echoes the MKV's start timecode (HH:MM:SS:FF) or empty.
mkv_start_timecode() {
    ffprobe -v error -show_entries format_tags=timecode -of default=nk=1:nw=1 "$1" 2>/dev/null | head -n1
}
```
(If the engine doesn't yet write `tmcd` — true until Phase 3 — this returns empty; the TC metric is then reported as `n/a (engine writes no tmcd yet)`, not a FAIL. The rig is forward-ready.)

- [ ] **Step 3: Consolidate the duplicated extractors.** `run_sync_e2e.sh` has private copies of `flash_pts_series`/`beep_pts_series`. Leave them (out of scope), but ensure the new framesync driver sources `srt_lib.sh` and uses *its* copies. Smoke: `bash -n tests/e2e/srt_lib.sh` → OK.

- [ ] **Step 4: Commit** — `git add tests/e2e/srt_lib.sh && git commit -m "test(framesync): SMPTE-TC marker producer + tmcd reader"`.

---

### Task 3: The framesync measurement driver

A driver that, for a given transport + codec + N sources, produces synchronized markers, bridges them to the engine, records via `sync_harness`, and emits the four metrics (gated when asked).

**Files:** Create `tests/e2e/run_framesync_e2e.sh`.

- [ ] **Step 1: READ** `tests/e2e/run_sync_e2e.sh` (the `lipsync` + `drift_2997` + `intercam_matched` scenarios: nearest-beep pairing for A/V, least-squares slope for drift, cross-view flash spread for inter-cam) and `tests/e2e/run_soak_matrix.sh` (the `run_cell` PASS/SKIP/FAIL pattern). Reuse their measurement awk.

- [ ] **Step 2: Create `run_framesync_e2e.sh`.** Args: `<sync_harness> <scenario> <base_port>`. Scenarios: `lipsync` (1 source, A/V offset), `intercam` (N=2 tee'd sources, flash-spread), `drift` (1 source through the `skew_injector` at `OLR_FRAMESYNC_SKEW_PPM` ppm, slope/ppm/slip over `OLR_FRAMESYNC_SECS`), `timecode` (1 source, recorded `tmcd` vs injected). Transport via `OLR_FRAMESYNC_TRANSPORT=srt|rtmp|ndi` (start with `srt`; rtmp/ndi reuse the rtmp_lib/NDI fixtures — leave rtmp/ndi as `SKIP 77` stubs wired in Task 4 of later phases). Structure:
  - `srt_require_tools`; `mktemp -d`; `PIDS=()` + cleanup trap.
  - Produce markers (`flash_beep_tc_marker_to_udp` for lipsync/drift/timecode; `flash_marker_to_udps` tee'd to 2 ports for intercam) → for `drift`, route through `python3 lossy_udp_relay.py <relay_port> 127.0.0.1:<srt_listener> 0 <stats> 1234 0 ${SKEW_PPM}` then `srt_bridge` the relay's input; else `srt_bridge` directly → `sync_harness --url $(srt_caller_url …)` (one or two `--url`).
  - Measure from the MKV(s) using `flash_pts_series`/`beep_pts_series`/`mkv_start_timecode`:
    - **lipsync:** nearest-beep-within-200ms mean `(audio-video)` ms; report; gate band **−40..+60 ms** (EBU R37; target tighten to ±20 later) when `OLR_FRAMESYNC_GATE=1`.
    - **intercam:** flash-onset PTS spread (max|Δ|) across the 2 views; report; gate band **≤ 1 frame (33 ms @30)** when gated **and** common-TC present (else report bound, never FAIL).
    - **drift:** least-squares slope of (flash_index → flash_pts); `ppm=(slope-1)*1e6`; `slip_frames=(slope-1)*fps*secs`; report; gate band **|A/V-offset drift| < 1 frame over the run** (and an informational `|ppm|` bound) when gated. With `SKEW_PPM=0` the slope must be ~1.0 (sanity); with `SKEW_PPM≠0` it documents the engine's current (arrival-anchored) drift — pre-Phase-1 this will show the injected ppm (a teeth-check that Phase 1's servo later flattens).
    - **timecode:** `mkv_start_timecode` vs `OLR_MARKER_TC`; report exact-match; `n/a` (not FAIL) until the engine writes `tmcd` (Phase 3).
  - Print one `PASS:`/`FAIL:`/`SKIP:`/`REPORT:` line + the metric; `exit 0/1/77`.

- [ ] **Step 3: chmod + syntax** — `chmod +x tests/e2e/run_framesync_e2e.sh && bash -n tests/e2e/run_framesync_e2e.sh` → OK.

- [ ] **Step 4: Smoke-run (SRT, report-only)** — build the harness if needed (`cmake --build build/bcast --target sync_harness`), then `OLR_FRAMESYNC_TRANSPORT=srt OLR_FRAMESYNC_SECS=20 tests/e2e/run_framesync_e2e.sh <sync_harness> lipsync 24000` and `... drift 24010` (with `OLR_FRAMESYNC_SKEW_PPM=200`). Confirm it records and prints sane metrics (lipsync within EBU; drift slope ≈ 1.0002 at 200 ppm — proving the skew_injector works). If `srt-live-transmit` is absent it SKIPs.

- [ ] **Step 5: Commit** — `git add tests/e2e/run_framesync_e2e.sh && git commit -m "test(framesync): measurement driver (lipsync/intercam/drift/timecode)"`.

---

### Task 4: CTest registration + the `framesync` matrix runner

**Files:** Modify `tests/e2e/CMakeLists.txt`; create `tests/e2e/run_framesync_matrix.sh`.

- [ ] **Step 1: Register the framesync gates** in `tests/e2e/CMakeLists.txt` (port band ≥ 24000), report-only by default:
```cmake
# Frame-sync acceptance rig (opt-in; report-only until Phase 1 tightens the bands).
foreach(_fs lipsync intercam drift timecode)
    add_test(NAME e2e_framesync_${_fs}
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_framesync_e2e.sh"
                "$<TARGET_FILE:sync_harness>" ${_fs} 24000)
endforeach()
set_tests_properties(e2e_framesync_lipsync e2e_framesync_intercam e2e_framesync_drift e2e_framesync_timecode
    PROPERTIES LABELS "framesync" TIMEOUT 180 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
# The lipsync + drift(skew=0) cells double as gates once Phase 1 lands:
set_tests_properties(e2e_framesync_lipsync PROPERTIES ENVIRONMENT "OLR_FRAMESYNC_GATE=1")
```
(Give each a distinct base port: 24000/24010/24020/24030 — adjust the foreach to pass per-scenario ports if they overlap; each scenario uses ~3 ports.)

- [ ] **Step 2: Create `run_framesync_matrix.sh`** modeled on `run_soak_matrix.sh`'s `run_cell` — run the four scenarios × transports `{srt, rtmp, ndi}` (rtmp/ndi SKIP until their fixtures land), each report-only, with a final PASS/SKIP/FAIL summary table. Duration via `OLR_FRAMESYNC_SECS`. `chmod +x`.

- [ ] **Step 3: Reconfigure + list** — `cmake -S . -B build/bcast -DOLR_BUILD_TESTS=ON >/dev/null && ( cd build/bcast && ctest -N -L framesync )` → lists the 4 gates. Run `( cd build/bcast && ctest -L framesync --output-on-failure )` (SKIPs cleanly without srt-live-transmit; otherwise reports baseline metrics).

- [ ] **Step 4: Commit** — `git add tests/e2e/CMakeLists.txt tests/e2e/run_framesync_matrix.sh && git commit -m "test(framesync): framesync ctest label + matrix runner"`.

---

### Task 5: Docs — the acceptance bands as the operational definition of "frame-perfect"

**Files:** Create `tests/e2e/FRAMESYNC.md`.

- [ ] **Step 1:** Document the rig: the four metrics + bands (lip-sync EBU R37 −40..+60 ms / target ±20; inter-cam ≤1 frame with common TC else bounded-report; drift |A/V offset drift| <1 frame over the run + ppm; TC exact-match when the engine writes `tmcd`), how to run (`ctest -L framesync`, the matrix runner, the env knobs `OLR_FRAMESYNC_*`/`OLR_MARKER_TC`), the `skew_injector` (the only way to test true drift on one machine), and the honest caveat (single-machine drift ≈ 0 without the injector; cross-source phase is bounded without common TC). State explicitly: **this rig is the acceptance instrument every later phase is gated by.** Commit.

---

## After all tasks
- `( cd build/bcast && ctest -L framesync )` reports baseline metrics for the current (arrival-anchored) engine; `skew_injector` self-test passes; the `drift` cell at 200 ppm shows ~200 ppm slope (proving measurement + injection work).
- Hand-off: Phase 1 flips the drift/lipsync cells from report-only to **gated** (tightened bands) as the servo lands.

## Self-review
- **Spec coverage:** Phase-0 `marker_gen` → Task 2; `skew_injector` → Task 1; measurement harness + four bands → Task 3; `framesync` ctest + matrix → Task 4; "rig = definition of 100%" → Task 5. Covered.
- **No engine changes** (Phase 0 measures as-is) — intentional; the bands are report-only until Phase 1.
- **Types/names consistent:** `OLR_FRAMESYNC_*`, `flash_beep_tc_marker_to_udp`, `mkv_start_timecode`, `skew_ppm`, the `framesync` label used identically across tasks.
