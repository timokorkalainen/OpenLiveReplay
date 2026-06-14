# Sync-Measurement Scoreboard — Design

**Status:** approved (brainstorm) → spec
**Date:** 2026-06-15
**Branch:** `feat/sync-measurement-harness`

## 1. Motivation

A 12-dimension audit of OpenLiveReplay's timing chain (see
[broadcast frame-sync memory] / workflow `wf_b8c7fd02-125`) concluded that the app
cannot today guarantee broadcast-grade frame-sync, and — more importantly — that
**there is no automated way to measure how far off it is.** The recording e2e
(`run_record_e2e.sh`) only checks intra-source A/V end-alignment of a *single*
source; nothing measures inter-camera offset, clock drift, or lip-sync magnitude.

You cannot improve what you cannot measure. Before implementing any of the
roadmap's sync fixes (per-camera trim, rational frame rate, source clock
recovery, …), we build a **report-only diagnostic** that quantifies the relevant
behaviors. Every future fix then has a baseline number to move, and a regression
scoreboard to defend.

This harness is **diagnostic only**: it prints measured numbers and always exits
0. It does **not** gate CI. (A later PR may flip individual measurements into
pass/fail gates once the corresponding fix lands and the number is known-good.)

## 2. Scope

In scope (v1):

- A new multi-source recording harness (`sync_harness`) that records N sources
  into an N-view MKV using the real engine.
- A driver script (`run_sync_e2e.sh`) that generates synthetic synchronized
  sources, runs the harness, analyzes the output with ffmpeg/ffprobe, and prints
  a scoreboard.
- Four measurement scenarios: `intercam_matched`, `intercam_skew`, `drift_2997`,
  `lipsync`.
- CTest registration under a **separate, non-gating label** (`sync-report`).
- A committed baseline snapshot (`tests/e2e/SYNC_BASELINE.md`) capturing the
  numbers this harness produces on the current `main`, so the starting point is
  reviewable in git.

Explicitly out of scope (YAGNI for v1):

- Any pass/fail thresholds or CI gating.
- Playback-side measurement (we measure on the recorded MKV; the playback path is
  a later extension).
- Real SRT/RTMP latency injection (we model skew with a producer start-stagger).
- More than two simultaneous sources.
- Channel-count / >2ch audio measurement (separate roadmap item).

## 3. Architecture

Three pieces, mirroring the existing `record_harness` + `run_record_e2e.sh`
pattern. Measurement logic stays **shell-side** (ffmpeg/ffprobe/awk), exactly as
`run_record_e2e.sh` does today; the C++ harness only drives the engine.

### 3.1 `tests/e2e/sync_harness.cpp`

`record_harness` generalized from one source to **N sources → N views (1:1)**.

CLI:

```
sync_harness --url <U1> [--url <U2> ...] --outdir <dir> --seconds <S>
             [--name <base>] [--width <W>] [--height <H>] [--fps <F>]
```

Behavior:

- Configure `ReplayManager` with the N source URLs and N views, mapping view *i*
  ← source *i* (1:1), reusing the same engine entry points `record_harness` uses.
- `startRecording()`, run the Qt event loop for `--seconds`, `stopRecording()`.
- Print the absolute path of the produced `.mkv` as the **last stdout line**
  (the driver reads `tail -n 1`), matching `record_harness`'s contract.
- Exit non-zero on engine init failure (no output file), like `record_harness`.

No measurement, no assertions in C++. Links `olr_test_engine` (the same lib
`record_harness` uses).

### 3.2 `tests/e2e/run_sync_e2e.sh <sync_harness> <scenario> <base_port> [--write-baseline]`

The optional trailing `--write-baseline` flag (4th arg) appends the scoreboard
line to `SYNC_BASELINE.md`; CTest invocations omit it, so CI never writes the
baseline.

Responsibilities, per scenario:

1. **Produce** the synthetic source(s) with FFmpeg lavfi (markers described in
   §4). Real-time paced (`-re`), MPEG-TS over UDP, h264+aac — same transport as
   `run_record_e2e.sh`.
2. **Record** by invoking `sync_harness` with the relevant `--url`(s).
3. **Analyze** the output MKV per §4 (per-view `signalstats` YAVG flash detection,
   `silencedetect`/`astats` beep onset), compute the scenario's metric with awk.
4. **Report** a one-line scoreboard block to stdout (§5) and append it to
   `SYNC_BASELINE.md` when `--write-baseline` is passed (manual, not in CI).
5. `exit 0` always (unless ffmpeg/ffprobe is missing → `SKIP`, exit 0; or the
   harness produced no file → print `ERROR` and exit 0, since this is report-only
   — a broken run is visible in the scoreboard, not a CI failure).

Hermetic: per-run `mktemp -d`, all producers killed and temp removed on `trap
EXIT`, unique base port per scenario (caller-supplied), `RUN_SERIAL`.

### 3.3 CTest registration (`tests/e2e/CMakeLists.txt`)

```
qt_add_executable(sync_harness sync_harness.cpp)
target_link_libraries(sync_harness PRIVATE Qt6::Core olr_test_engine olr_warnings olr_sanitize)

add_test(NAME sync_intercam_matched COMMAND bash run_sync_e2e.sh $<TARGET_FILE:sync_harness> intercam_matched 23480)
add_test(NAME sync_intercam_skew    COMMAND bash run_sync_e2e.sh $<TARGET_FILE:sync_harness> intercam_skew    23482)
add_test(NAME sync_drift_2997       COMMAND bash run_sync_e2e.sh $<TARGET_FILE:sync_harness> drift_2997       23484)
add_test(NAME sync_lipsync          COMMAND bash run_sync_e2e.sh $<TARGET_FILE:sync_harness> lipsync          23486)

set_tests_properties(sync_intercam_matched sync_intercam_skew sync_drift_2997 sync_lipsync
    PROPERTIES LABELS "sync-report" TIMEOUT 180 RUN_SERIAL TRUE)
```

The `sync-report` label is **distinct** from `e2e`, so existing `ctest`/CI
invocations that select or exclude `e2e` are unaffected, and these can be run on
demand with `ctest -L sync-report`.

**As built, CI handling is:** both `ctest` invocations in
`.github/workflows/ci.yml` (the Build+Test job and the sanitizer job) add
`-LE sync-report`, so the four diagnostic tests never run in CI and can never
gate it — but `sync_harness` is still compiled by the Build+Test job's
`cmake --build`, so it stays regression-proof. `sync_harness` is intentionally
**omitted** from the sanitizer job's explicit `--target` list (it is a
diagnostic harness, not a sanitized gate); the `-LE sync-report` exclusion is
what keeps that omission from producing missing-binary test runs there.

## 4. Measurement methodology

All markers use the project's already-proven detection technique (memory:
"signalstats YAVG vs silencedetect"; `run_record_e2e.sh` lineage): a full-frame
luma **flash** (via lavfi `geq`/overlay) for video timing, and a gated **sine
beep** for audio timing. Flash/beep onset PTS is extracted with `ffmpeg
-vf signalstats -show_entries frame=pkt_pts_time:frame_tags=lavfi.signalstats.YAVG`
(peak-pick) and `silencedetect`/`astats` respectively.

### 4.1 `intercam_matched` — align-path soundness

- **Producer:** ONE lavfi source, full-frame flash every 1 s, fed through the
  FFmpeg `tee` muxer to **two** UDP ports (`base`, `base+1`). Both outputs are
  byte-identical with identical source PTS, emitted simultaneously.
- **Record:** `sync_harness --url udp://…:base --url udp://…:base+1` → 2-view MKV.
- **Analyze:** for each view track (`-map 0:v:0`, `0:v:1`), extract flash PTS
  series; pair flashes by index; metric = mean and max `|PTS_view0(k) − PTS_view1(k)|`.
- **Interpretation:** both views originate from the same tee, so the *only*
  source of offset is the engine's per-source anchoring + mux. Expectation ≈ 0;
  any non-trivial offset is an alignment/mux defect. This is the control.

### 4.2 `intercam_skew` — baked-in latency offset (REF-2 / PHASE-1)

- **Producer:** TWO independent `-re` producers to ports `base`, `base+1`. Source
  B is started a **known delay D** after source A (default `D=250 ms`, shell
  `sleep`). Each emits a 1 s flash cadence.
- **Record:** 2-view MKV as above.
- **Analyze:** **as built**, the two per-track flash series are paired by index
  (flash #k on view 0 with flash #k on view 1); the planned 3-flash "clap" was
  simplified to index-pairing, which is equivalent for these controlled synthetic
  sources (`FLASH_THRESH=180` excludes the h264 cold-start gray, so each series
  starts at the first true flash). Metric = signed mean `PTS_view0(k) −
  PTS_view1(k)` (ms) and its stdev across the run.
- **Interpretation:** the engine anchors each source to its own first-packet
  arrival with no shared reference, so it should bake in ≈ D. The number
  quantifies how much (and whether it stays constant). `D_injected` is printed
  alongside for comparison.

### 4.3 `drift_2997` — fractional-rate timeline drift (FRAC-1)

- **Producer:** ONE source paced at **30000/1001** (29.97), flash every
  source-second, run ~60 s. Harness session runs at **integer `--fps 30`** (the
  only option the engine supports today).
- **Analyze:** flash #k should land at file-PTS ≈ k s on a 1.000 slope. Fit slope
  of (filePTS vs k) by least squares; metric = `drift_ppm = (slope−1)·1e6` and
  total **frames-of-slip** = `(slope−1)·fps·duration`.
- **Interpretation:** an integer-fps timeline against a 29.97 source cannot stay
  phase-locked; the measured ppm/slip quantifies FRAC-1 empirically (rather than
  predicting it). When rational-rate support lands, this number should collapse
  toward 0.

### 4.4 `lipsync` — A/V offset magnitude (AUD-4 / LIPSYNC)

- **Producer:** ONE source, full-frame flash **and** a sine beep fired
  simultaneously every 1 s.
- **Analyze:** per second, `audio_onset_PTS − video_flash_PTS`; metric = mean and
  max signed offset (ms). (EBU R37 reference band ≈ +40/−60 ms, printed for
  context only — not asserted.)
- **Interpretation:** quantifies the A/V offset the *recording* path produces and
  whether it is constant. Playback-side lip-sync is a later extension.

## 5. Scoreboard output

Each scenario prints a single grep-able line plus a human header. Example:

```
=== sync scoreboard: intercam_skew ===
[sync] scenario=intercam_skew  flashes_paired=23  intercam_offset_ms: mean=251.2 max=255.0 stdev=1.1  (D_injected=250)
PASS: report emitted (diagnostic, non-gating)
```

`SYNC_BASELINE.md` accumulates these `[sync] …` lines (under a dated heading)
when the driver is run locally with `--write-baseline`, giving a committed,
reviewable snapshot of where `main` stands. CI runs do **not** write the
baseline.

Metric keys (stable, for later trending):

- `intercam_offset_ms` — mean/max/stdev (matched + skew)
- `drift_ppm`, `drift_frames_slip` (drift)
- `av_offset_ms` — mean/max (lipsync)

## 6. Components & boundaries

| Unit | Purpose | Depends on | Independently testable |
|---|---|---|---|
| `sync_harness.cpp` | record N URLs → N-view MKV | `olr_test_engine` (real engine) | yes — run it with 2 URLs, check MKV has 2 video tracks |
| `run_sync_e2e.sh` | produce + record + analyze + report | `sync_harness`, ffmpeg/ffprobe | yes — each scenario runs standalone |
| analysis awk blocks | marker-PTS → metric | ffprobe output only | yes — feed canned signalstats output |

The harness knows nothing about measurement; the script knows nothing about
engine internals. The seam is the MKV file + the printed path.

## 7. Error handling

- ffmpeg/ffprobe absent → `SKIP` (exit 0), matching `run_record_e2e.sh`.
- Harness exits non-zero / no MKV → print `ERROR: no output` and the captured
  harness stderr, exit 0 (report-only: surfaced in the scoreboard, not a CI
  failure).
- Too few flashes detected to compute a metric → print `ERROR:
  insufficient markers (got N)`, exit 0.
- All producers `kill`ed and `mktemp` dir removed on `trap EXIT`.

## 8. Testing the harness itself

- The `intercam_matched` control IS the harness's own sanity check: a ≈0 offset
  confirms the record+analyze pipeline measures correctly end-to-end.
- The driver verifies the multi-view recording path inline before analysis: it
  `ffprobe`s the output MKV and prints `ERROR: expected N video tracks, got M`
  (exit 0, report-only) if the engine did not produce one video track per source.
  This guards the `sync_harness` N-source→N-view contract without a separate test.
- Manual local run of all four scenarios populates `SYNC_BASELINE.md`; that file
  is committed so reviewers see the current numbers.

## 9. Risks / open notes

- **Synthetic-localhost limitation:** producer and consumer share one machine
  clock, so true crystal drift between *physical* devices is not reproducible.
  `drift_2997` deliberately induces drift via a *fractional source rate*, which is
  the FRAC-1 mechanism we care about and is fully controllable. Cross-device PPM
  drift remains a field-measurement concern, documented, not synthesized here.
- **Flash-detection robustness:** depends on a high-contrast full-frame flash and
  peak-picking YAVG; the threshold/peak logic is the fiddly part and gets its own
  unit-style validation against canned signalstats output (§6).
- **Skew model fidelity:** a producer start-stagger models "camera B's timeline is
  shifted by D," a faithful proxy for an arrival-latency difference for the
  purpose of measuring whether the engine corrects it (it does not). Real SRT
  latency-window behavior is out of scope for v1.
