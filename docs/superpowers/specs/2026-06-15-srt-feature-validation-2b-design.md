# SRT Feature Validation (Phase 2b) — Design

**Status:** approved
**Date:** 2026-06-15
**Depends on:** Phase 1 SRT test infra (#31), Phase 2a 4-source SRT routing (#32),
the sync measurement harness (#26), per-camera trim (#28), per-source
connection-status (#24).

## Goal

Prove — over **real `srt://` transport**, not synthetic localhost UDP — that three
already-shipped features actually work end-to-end:

1. **Inter-camera sync** lands within a bounded window for coincident input.
2. **Per-source trim** (#28) shifts a source's content by the set amount.
3. **Connection-status** (#24) reports all sources connected, and discriminates a
   source that fails to connect.

Phase 2a proved *routing* (source *i* → view *i*) over 4 SRT streams using distinct
continuous tones. Phase 2b proves *timing/behavioral* features using time-structured
flash+beep markers, real SRT ingest, and **pass/fail gates** (2a/2b are diagnostics +
gates; the existing `run_sync_e2e.sh` remains report-only).

## Why this is low-risk

The synthetic `tests/e2e/run_sync_e2e.sh` already contains proven building blocks we
reuse verbatim or near-verbatim:

- A flash+beep marker producer (`geq` full-frame luma flash for the first ~60 ms of
  every source-second + a gated 1 kHz beep).
- `flash_pts_series <mkv> <video-track>` — per-track flash-onset PTS via
  `signalstats` YAVG rising edge (threshold 180, above h264 cold-start gray ~128).
- `beep_pts_series <mkv> <audio-track>` — per-track beep onset via `silencedetect`.
- `intercam_matched` — one producer tee'd to N ports (byte-identical, simultaneous);
  measure inter-view flash offset. This is the inter-camera-sync method.
- `intercam_trim` — tee'd source, `--trim` on one view, measure the offset shift
  (≈ −TRIM in the delay direction). This is the trim method.

Phase 2b swaps the transport (localhost UDP → real SRT via `srt-live-transmit`
bridges, exactly as #31/#32 do), scales to 4 cameras, and converts the measurements
into gates plus a connection count.

## Honest ceiling (carried from the framesync audit)

The engine anchors each source to its **first-packet arrival time** (`RecordingClock`,
free-running `QElapsedTimer`); there is no shared reference clock / genlock (audit
finding REF-2, unfixed). So even *coincident* SRT input is only
**frame-phase-locked-within-measured-bounds**, never zero-skew. The inter-camera-sync
gate is therefore a **generous bound** (catches gross breakage) plus a strict
per-view *content* gate (every view must carry the live marker). Trim and
connection-status are clean, discriminating gates.

## Architecture

### Shared plumbing — `tests/e2e/srt_lib.sh` (new, sourced)

A small sourced library so the bridge / marker / extraction logic isn't copied a
third time. Contents:

- **SKIP guards:** if `ffmpeg`, `ffprobe`, or `srt-live-transmit` is missing, the
  sourcing script should `echo "SKIP: ..."` and `exit 0` (helper `srt_require_tools`).
- **`srt_bridge <udp_port> <srt_port>`** — spawn one `srt-live-transmit` UDP→SRT
  *listener* (`srt://127.0.0.1:<srt_port>?mode=listener&transtype=live&latency=200`),
  append its PID to `PIDS`.
- **`flash_marker_to_udps [--beep] <udp_port>...`** — spawn **one** ffmpeg flash
  (optionally flash+beep) producer, tee'd to all given UDP ports (byte-identical,
  coincident content), append PID to `PIDS`. Reuses the proven `geq`/`volume` lavfi.
- **`flash_pts_series <mkv> <video-track>`** and **`beep_pts_series <mkv> <audio-track>`**
  — copied from `run_sync_e2e.sh` (proven awk extractors).
- **`srt_caller_url <srt_port>`** — `srt://127.0.0.1:<srt_port>?transtype=live`.
- Caller is responsible for declaring `PIDS=()` + a cleanup `trap` (matching the
  existing scripts) and for `sleep`-ing ~1.5 s after spawning so producers + listeners
  come up before the engine connects.

The merged `run_sync_e2e.sh`, `run_srt_4cam.sh`, `run_srt_smoke.sh` are **not**
modified (no refactor of working code).

### Scenario 1 — `tests/e2e/run_srt_sync.sh` → CTest `e2e_srt_sync`

Inter-camera sync over real SRT. **Generous gate.**

- One flash producer tee'd to **4** UDP ports → 4 `srt_bridge` listeners → 4 SRT
  caller URLs (coincident, identical content).
- Record a 4-view MKV via `sync_harness` (`--url`×4, `--seconds 8 --fps 30`).
- Assert 4 video tracks. For each view run `flash_pts_series`; index-pair flashes
  across the 4 views; for each flash compute the max−min PTS spread across views;
  report `mean` and `max` spread in ms.
- **Gate A (content / teeth):** every view produced ≥ `MIN_FLASHES` (e.g. 4) flashes.
  A view from a source that failed to connect is blue-fill (no flash) → 0 → FAIL.
  This is the real discriminator (a dead view cannot fabricate a flash).
- **Gate B (bound):** `max` spread ≤ `MAX_SPREAD_MS` (default 250). Deliberately
  generous per the honest ceiling; fails only on gross misalignment.
- Always prints the scoreboard line (`[srt-sync] views=4 flashes=... spread_ms: mean=... max=...`).

### Scenario 2 — `tests/e2e/run_srt_trim.sh` → CTest `e2e_srt_trim`

Per-source trim (#28) over real SRT. **Clean gate.** A direct port of the proven
synthetic `intercam_trim` (one tee'd source → **2** coincident views; trim the second)
to real SRT — 2 views is the lowest-noise faithful proof of the #28 mechanism (the
4-stream requirement is carried by Scenarios 1 and 3).

- One flash producer tee'd to **2** SRT views (coincident, identical content).
- `measure_offset <trim_ms>`: spawn a fresh producer + 2 bridges, record 2-view with
  `sync_harness --trim <trim_ms>` (applies to the **last** source, view 1), extract
  `flash_pts_series` for both views; index-pair and report mean (view0 − view1) ms.
  Kill the producer between sub-runs.
- Run `measure_offset 0` (baseline) and `measure_offset T` (T default 300).
- **Gate:** `(trimmed_offset − untrimmed_offset)` ≈ **−T** within `TOL_MS` (default
  120). The trim *delays* view 1, so its flash PTS *increases* → the signed
  (view0−view1) offset *decreases* by ≈ T (matching the proven `intercam_trim`:
  "delay => trimmed ≈ untrimmed − TRIM"). T=0 yields ≈0 shift, so the gate
  self-discriminates. Measuring the run-to-run *difference* cancels any systematic
  per-view connect-order bias, leaving only arrival jitter (which `TOL_MS` covers).
- Prints `[srt-trim] untrimmed_ms=... trimmed_ms=... applied=T expect_shift=-T`.

### Scenario 3 — `tests/e2e/run_srt_connect.sh` → CTest `e2e_srt_connect`

Connection-status (#24) over real SRT. **Clean gate.** Requires the harness change
below.

- Spawn 4 flash producers + bridges (distinct tone not needed; any live content).
- Record with `sync_harness --report-connections`; parse the printed
  `connected=<N>` line. **Gate:** N == 4.
- **Teeth (same script, second run):** spawn only 3 bridges; the 4th URL points at a
  **dead** SRT port (no listener). Record again; **Gate:** N == 3. Proves the count
  reflects real connection state, not a constant.
- Prints `[srt-connect] live_run=4 dead_run=3`.

### Harness change — `tests/e2e/sync_harness.cpp`

Additive, opt-in flag `--report-connections` (default off → existing scripts
unaffected):

- Parse the flag (same `argValue`/presence pattern as `--trim`).
- Before `startRecording()`, `QObject::connect(&rm,
  &ReplayManager::sourceConnectionChanged, &app, [&](int idx, bool connected){ if
  (connected) connectedSet.insert(idx); });` (a `QSet<int>` captured by reference).
- In the stop callback (after the 700 ms flush, before/around printing the path),
  if the flag is set, `fprintf(stderr, "connected=%d\n", connectedSet.size());`
  Print to **stderr** so it never pollutes the stdout MKV-path contract the scripts
  rely on (`tail -n1`).
- The harness already runs a `QCoreApplication` event loop, so the queued signal
  delivers normally.

### CTest registration — `tests/CMakeLists.txt`

Register `e2e_srt_sync`, `e2e_srt_trim`, `e2e_srt_connect` mirroring `e2e_srt_4cam`:
each gated behind the same `OLR_FFMPEG_SRT_PREFIX`-built `sync_harness`, labelled
`srt` (local-only). CI already excludes them via `-LE 'sync-report|srt'`. Base ports:
sync **23520**, trim **23530**, connect **23540** (no collision with smoke 23501 /
4cam 23510). Each test passes the harness path + base port as args, matching the 2a
signature.

### Docs — `tests/e2e/SRT_README.md`

Add a Phase 2b section listing the three scenarios, how to run them
(`ctest -L srt`), what each gate asserts, and the honest-ceiling caveat (the sync gate
proves *bounded + all-views-live*, not zero-skew; trim/connect are clean gates).

## Gate thresholds

`MIN_FLASHES` (≥4), `MAX_SPREAD_MS` (250), trim `T` (300) / `TOL_MS` (120) are
starting defaults chosen with margin against the honest-ceiling jitter. They are
**validated and tuned against real local SRT runs during implementation** (the
implementer runs each scenario, confirms the gate passes on a correct build with
comfortable margin, and confirms the inherent teeth-check fails). If a gate proves
flaky, prefer widening the bound / increasing `T` over removing the gate — the
content gates (per-view flash count, connection count) carry the discrimination.

## Testing strategy

These scripts **are** the tests (e2e gates). Per-scenario the gate + an inherent
teeth-check (dead view → 0 flashes; T=0 → ~0 shift; dead port → connected=3) prove the
assertion discriminates. Manual local verification: build `sync_harness` with
`-DOLR_FFMPEG_SRT_PREFIX="$PWD/macos_build/ffmpeg-srt"`, then `ctest -L srt
--output-on-failure`. CI is unaffected (label excluded).

## Out of scope (future Phase 2c)

Disconnect/reconnect mid-recording, packet loss / jitter injection, reconnect
re-anchoring behavior, long-run drift over SRT.
