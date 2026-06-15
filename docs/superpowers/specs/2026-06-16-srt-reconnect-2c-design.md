# SRT Disconnect/Reconnect Survival (Phase 2c-a) — Design

**Status:** implemented — but **superseded on one point** by what the build revealed.
This doc designs the gate against the **ffmpeg** ingest. During implementation the
gate uncovered a real cross-source coupling on that path (a dead source's avformat
reconnect churn monopolizes libsrt's single global receive thread and starves the
other sources). The **native Apple SRT ingest** (`OLR_NATIVE_SRT=1`, landed on `main`
separately) does not have it, so the shipped gate is **`e2e_native_srt_reconnect`**
in the **`native-apple-ingest`** label (not `e2e_srt_reconnect` in `srt`), with the
control-isolation assertion made **strict** (no mid-record disconnect, no content
gap). See `tests/e2e/SRT_README.md` (Phase 2c-a) for the final rationale.
**Date:** 2026-06-16
**Depends on:** Phase 2b SRT feature-validation gates (`srt_lib.sh`, `sync_harness`,
`--report-connections`, the `linger=0` teardown fix) — all on `main`.

## Goal

Prove — over real `srt://` transport — that the recording engine **survives a
mid-recording source drop**: it *observes* the disconnect, *reconnects* when the
source returns, and *resumes recording real frames* (not a frozen frame or
permanent blue-fill). A camera/source cable wiggle must not kill the recording.

This is the first of three Phase 2c sub-projects (the others — packet-loss/jitter
injection, long-run drift — are out of scope here).

## Why the obvious version is wrong (review findings)

An adversarial design review (six code-grounded critics) found the naive
"kill@5s / restart@9s, record 20s" plan would **silently false-pass**:

1. **Disconnect is detected indirectly.** The engine does not see the kill at
   wall-clock time. It detects a dead source via the **stall timeout**
   (`m_stallTimeoutMs = 8000` ms with no frame enqueued, `streamworker.cpp`) or the
   **5 s socket `rw_timeout`** on a hung `av_read_frame`. Meanwhile the **15 MB SRT
   receive buffer** (`recv_buffer_size`) keeps draining already-buffered packets
   after the bridge dies. So a 4 s outage can be **fully masked** — the engine never
   enters the disconnect path, and the test proves nothing.
   **→ The outage MUST exceed the stall window (> 8 s).**
2. **Bare up/down counts can't distinguish a real reconnect from a flaky initial
   connect** (`up=2` via fail-then-succeed during warm-up). **→ Use a per-source
   *timestamped* transition log** and assert the ordered sequence.
3. **A single tee'd producer couples the sources.** **→ Use a separate producer per
   source** so the victim is fully isolated.

Confirmed sound by the review (no change needed): the per-connection anchor /
`firstPacketDts` reset is correct (resets on each `setupDecoder` success); and the
flash-PTS-window content check is robust against stale queued frames — a pre-kill
flash keeps its **pre-kill PTS**, so it cannot masquerade as post-reconnect content,
and frozen video produces **no new flashes**.

## Architecture

Reuses `tests/e2e/srt_lib.sh` (bridge, flash marker, `flash_pts_series`,
`srt_caller_url`, `srt_require_tools`, `SRT_LAST_PID`) and `sync_harness`. The shell
script orchestrates the kill/restart timing; the harness just records and reports
connection transitions — it stays unaware of the outage.

### Component 1 — `sync_harness.cpp`: `--report-connection-events` (additive)

`StreamWorker::setConnected` emits `connectionChanged(idx, bool)` on **every** real
transition (it `exchange`s `m_connected` and emits only on change), relayed to
`ReplayManager::sourceConnectionChanged` via a queued connection → delivered on the
app/main thread. The harness:

- Parses a new presence flag `--report-connection-events` (default off; independent
  of `--report-connections`).
- Maintains `QHash<int, QVector<QPair<qint64,bool>>>` — per source, a chronological
  list of `(elapsedMs, connected)` transitions, appended in the existing main-thread
  lambda (no extra locking needed — same single-thread invariant as the current
  `connected=N` code). `elapsedMs` comes from a `QElapsedTimer` started just before
  `startRecording()`.
- On stop, prints **one line per source in ascending index order** to **stderr**
  (keeping stdout the MKV-path-only contract):
  `conn_events src=<i> <t0>:<up|down> <t1>:<up|down> ...`
  e.g. `conn_events src=1 412:up 18730:down 21940:up`.

Off-by-default and stderr-only ⇒ no impact on existing scenarios.

### Component 2 — `tests/e2e/run_srt_reconnect.sh` → CTest `e2e_srt_reconnect`

Two **independent** sources (separate producers + bridges): **src0 = control**
(never touched), **src1 = victim**. Tunable constants (env-overridable):
`KILL_TIME=10`, `RESTART_TIME=20`, `DURATION=38`, `LATE_MIN=$((RESTART_TIME + 6))`
(= 26). Base port **23550**.

Flow:
1. `srt_require_tools`; mktemp workdir; `PIDS=()` + cleanup trap.
2. Start a **separate** flash producer + bridge for src0 (port 23550/23551) and src1
   (23552/23553). Capture `src1_bridge_pid` explicitly right after spawning src1's
   bridge (`$SRT_LAST_PID`). `sleep 1.5` warm-up.
3. Launch `sync_harness --url <src0> --url <src1> --report-connection-events
   --seconds $DURATION ...` in the **background** with explicit redirects
   (`>"$out" 2>"$err" &`); capture `$harness_pid` (bash-3.2-safe).
4. `sleep $KILL_TIME` → `kill "$src1_bridge_pid"` (the bridge only; src1's producer
   keeps running, modelling a network drop, not a source removal).
5. Unless `OLR_SRT_RECONN_NO_RESTART=1`: `sleep $((RESTART_TIME - KILL_TIME))` →
   restart src1's bridge on the **same** SRT port, with a short **retry loop**
   (e.g. up to 5×, 300 ms apart) in case the OS port is briefly held. Capture the new
   `src1_bridge_pid` for cleanup.
6. `wait "$harness_pid"`; read `$out` (MKV path) and `$err` (`conn_events`).

Assertions (all must hold):
- **MKV valid:** non-empty path, 2 video tracks.
- **Transition sequence (src1):** parse src1's event line. Require, in order,
  (a) an initial `up` with `t < KILL_TIME*1000`, (b) a `down` with
  `t > KILL_TIME*1000`, (c) a later `up` whose `t` is **greater than the `down`'s
  `t`** (the reconnect). Checking the reconnect `up` follows the `down` (not just
  `t > KILL_TIME`) disambiguates a real reconnect from a flaky warm-up double-connect.
- **Control intact (src0):** `down` count = 0, and `flash_pts_series` for view0 has
  flashes that **span the outage** (at least one flash with
  `KILL_TIME < pts < RESTART_TIME`, plus flashes before and after) — proving the
  outage was isolated to src1.
- **Content resumes (src1) — the real teeth:** `flash_pts_series` for view1 has
  flashes with `pts < KILL_TIME` (pre-kill) **and** flashes with `pts > LATE_MIN`
  (post-reconnect). A frozen/blue-fill source yields no late flashes.

Built-in teeth (`OLR_SRT_RECONN_NO_RESTART=1`, **shell-side** — no engine change):
the restart in step 5 is skipped → src1 never reconnects → its event line has **no
second `up`** and **no late flashes** → the gate FAILS, proving it discriminates.

### Non-gating diagnostic — re-anchor offset

After a reconnect, the engine re-anchors src1 to fresh arrival time (audit REF-4/5),
so src1's post-reconnect content is offset from src0's. Report (never gate):
pair each src0 late-window flash (`pts > LATE_MIN`) with the nearest src1 late-window
flash, take the mean signed PTS delta, print
`reanchor_offset_ms=<mean> (diagnostic; re-anchored to fresh arrival, expected nonzero)`.

### Component 3 — CMake + docs

- Register `e2e_srt_reconnect` in `tests/e2e/CMakeLists.txt` after `e2e_srt_connect`,
  label `srt`, **`TIMEOUT 240`** (longer record + possible backoff), `RUN_SERIAL`,
  base port 23550 passed as an arg.
- Extend `tests/e2e/SRT_README.md` with the Phase 2c-a scenario + the honest note
  that disconnect detection is stall/`rw_timeout`-driven (hence the >8 s outage).

## Timing rationale

| Event | t (s) | Why |
|---|---|---|
| warm-up + both connect | 0–1.5 | initial `up` for both before the kill |
| kill src1 bridge | 10 | mid-record, after steady state |
| engine detects drop | ~15–18 | stall (8 s) / `rw_timeout` (5 s) + buffer drain |
| restart src1 bridge | 20 | 10 s outage > 8 s stall ⇒ detection guaranteed; port free of TIME_WAIT |
| engine reconnects | ~21–24 | next backoff retry after the bridge is back (backoff resets to 1 s on success) |
| late content window | >26 | ≥ 2 s of post-reconnect footage ⇒ multiple flashes |
| record ends | 38 | ≥ 10 s of post-reconnect footage for robust detection |

All values are env-tunable and validated against real local runs during
implementation; if flaky, **widen** windows (never delete the gate).

## Testing strategy

This script *is* the test. Its teeth (`OLR_SRT_RECONN_NO_RESTART=1` → FAIL) prove the
gate discriminates. Local run: build `sync_harness` with `-DOLR_FFMPEG_SRT_PREFIX`,
then `ctest -R e2e_srt_reconnect --output-on-failure`. CI is unaffected (the `srt`
label is excluded).

## Out of scope (later Phase 2c sub-projects)

- Packet-loss / jitter injection (needs privileged macOS network emulation).
- Long-run drift over SRT.
- Permanent-loss handling as a distinct gate, multi-source simultaneous reconnect,
  and gating the re-anchor offset (today a diagnostic).
