# 4-Source SRT Routing Foundation (Phase 2a) — Design

**Status:** approved (brainstorm) → spec
**Date:** 2026-06-15
**Branch:** `feat/srt-4cam`

## 1. Motivation

Phase 1 (#31) made the test build able to ingest one real `srt://` stream and
proved it with a content check. Phase 2 validates the actual multi-camera
functionality against real SRT. This is **Phase 2a — the foundation**: prove that
**4 real SRT streams connect, record into a 4-view MKV, and route correctly** —
view *i* carries camera *i*'s content, not blue-fill silence.

Routing correctness is the load-bearing property: the engine maps source *i* →
view *i*, and an operator's whole mental model depends on "view 2 is camera 2."
Phase 1 also taught the key lesson — `record_harness`/`sync_harness` emit
blue-fill video + **silence** for any source that fails to connect, so structural
checks (track count) pass even when nothing was ingested. We must verify **content
per view**.

**Phase 2b** (inter-camera sync, per-source trim, connection-status) and **2c**
(disconnect/reconnect, packet loss) follow as their own specs, built on this.

## 2. Decisions (from brainstorm)

- **2a foundation only** (routing + all-connected), validated by content per view.
- **Per-camera identity = a distinct audio tone** — camera *i* emits a sine at
  `(i+1)×1000 Hz` (1k/2k/3k/4k). *Rejected:* per-camera video markers
  (flash-count/luma — harder to detect than audio-band RMS) and burned-in text +
  OCR (fragile). Audio-band RMS proves routing **and** real content in one check.
- **Local-only** (the `srt` ctest label, already CI-excluded). It is a **real
  gate** (exits non-zero on wrong routing / a missing source), unlike the
  report-only UDP smokes.

## 3. Components

No C++ changes — `sync_harness` already records N `--url` sources → N views (1:1
source→view), transport-agnostic, and built with `-DOLR_FFMPEG_SRT_PREFIX` it
ingests `srt://`. The analysis is shell-side.

### 3.1 `tests/e2e/run_srt_4cam.sh`

`Usage: run_srt_4cam.sh <sync_harness_exe> [base_srt_port]` (default base 23510).

1. **Generate 4 distinct-tone SRT cameras.** For `i` in 0..3, on port-pair
   `(SRT=base+2i, UDP=base+2i+1)`: an ffmpeg producer of `testsrc2` video + a
   **sine at `(i+1)*1000` Hz** audio → UDP MPEG-TS; an `srt-live-transmit`
   `udp→srt://...mode=listener` bridge. (Same pattern as `run_srt_smoke.sh`, ×4.)
   All started together; a `sleep` before recording.
2. **Record all 4** with `sync_harness --url srt0 --url srt1 --url srt2 --url srt3
   --outdir … --seconds 8 --fps 30` → a 4-view MKV (source *i* → view *i*).
3. **Assert routing (the proof).** The MKV must have 4 video tracks. For each view
   *i* (0..3), detect the dominant audio tone and assert it equals
   `(i+1)*1000 Hz`:
   - For each candidate band `f ∈ {1000,2000,3000,4000}`, measure the recorded
     view's audio energy in that band:
     `ffmpeg -i MKV -map 0:a:i -af "bandpass=f=f:width_type=h:w=200,astats=metadata=1:measure_overall=RMS_level" -f null -`
     → parse the Overall RMS dB.
   - The band with the **maximum** RMS is the detected camera. Assert
     `detected_freq == (i+1)*1000` AND that the winning band's RMS is above the
     silence floor (`> -60 dB`) — a blue-fill/silence view (failed connect) has no
     dominant tone and FAILS.
4. **Hermetic + SKIP/FAIL semantics** (mirrors `run_srt_smoke.sh`): `mktemp -d`,
   `trap` kills all 8 producer/bridge PIDs + removes the temp dir; `SKIP` (exit 0)
   if ffmpeg/ffprobe/srt-live-transmit missing; **FAIL** (exit 1) on no MKV, wrong
   track count, or any view whose dominant tone ≠ its camera.
5. **Report** a per-view line, e.g.
   `[srt-4cam] view0=1000Hz view1=2000Hz view2=3000Hz view3=4000Hz → routing OK`,
   then `PASS: 4-source SRT routing — each view carries its own camera`.

### 3.2 `tests/e2e/CMakeLists.txt`

```cmake
add_test(NAME e2e_srt_4cam
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_4cam.sh" "$<TARGET_FILE:sync_harness>" 23510)
set_tests_properties(e2e_srt_4cam PROPERTIES LABELS "srt" TIMEOUT 180 RUN_SERIAL TRUE)
```
The `srt` label is already excluded from both CI ctest runs (`-LE 'sync-report|srt'`,
from Phase 1), so this stays local-only. `sync_harness` is built by the SRT build
the same way `record_harness` is.

### 3.3 `tests/e2e/SRT_README.md`

Add a short note: `e2e_srt_4cam` proves 4 real SRT streams route correctly to their
views (per-camera tone), and how to run it (same SRT build; `ctest -L srt`).

## 4. Frequency-detection detail

The producers use ffmpeg `sine` (near-pure tones, negligible harmonics), and the
tones are 1 kHz apart, so a 200 Hz-wide `bandpass` per candidate cleanly isolates
each fundamental. For a correctly-routed view *i*, the band at `(i+1)*1000` has
high RMS (the tone passes) and the other 3 bands are far lower (tone filtered
out) — a clear max. The winning-band RMS must also clear the `-60 dB` floor so a
silent (failed-connect) view cannot accidentally "win" a band by noise.

## 5. Error handling

- Missing ffmpeg/ffprobe/srt-live-transmit → `SKIP` (exit 0).
- Harness built WITHOUT `-DOLR_FFMPEG_SRT_PREFIX` → it can't open `srt://` → all
  views blue-fill/silence → every view's dominant-tone check fails → the test
  FAILS (correct; the README documents the SRT-build requirement).
- A single source failing to connect → that view silent → its check fails →
  overall FAIL, naming the offending view.
- Ports: 8 ports from `base` (SRT=`base+2i`, UDP=`base+2i+1` per camera). `base=23510`
  → ports **23510-23517**, clear of `e2e_srt_smoke`'s 23501/23502 and all other
  registered test ports (highest prior is 23491).

## 6. Testing

- The **`e2e_srt_4cam`** scenario IS the verification: 4 streams → 4-view record →
  per-view tone routing assertion.
- **Teeth-check (required, like Phase 1):** the script supports an env override
  `OLR_SRT4_EXPECT_SHIFT=1` that rotates the expected mapping (view *i* expects
  camera *i+1* mod 4); with it set, a correctly-routed recording MUST **FAIL** the
  assertion. This proves the routing check has teeth (it isn't just asserting
  "some tone exists"). Run manually during implementation; report PASS (normal) +
  FAIL (shifted).
- Default brew build unaffected (no C++ change; `srt`-labelled, CI-excluded).

## 7. Out of scope (2b / 2c)

- Inter-camera **sync** precision (frame-phase offset between cameras) — 2b.
- Per-source **trim** effect over SRT, **audio-latency**, **connection-status**
  signal assertions — 2b.
- **Disconnect/reconnect** + **packet-loss** robustness — 2c.
- Any engine C++ change — none here; this is a test-only foundation.

## 8. Risks

- **4 concurrent ffmpeg + 4 srt-live-transmit + the engine** is heavier than the
  1-stream smoke; on a loaded machine the SRT connects may need a longer startup
  `sleep` (the engine reconnects with backoff, mitigating). The 180 s timeout has
  headroom.
- **Tone bleed / detection robustness:** mitigated by 1 kHz spacing + 200 Hz
  bands + the `-60 dB` floor; the teeth-check (shifted mapping must fail) is the
  guard that the discriminator is real.
- **Port contention** if a prior run's sockets linger: `RUN_SERIAL` + a unique
  `base` port range avoid overlap; cleanup kills all PIDs on exit.
