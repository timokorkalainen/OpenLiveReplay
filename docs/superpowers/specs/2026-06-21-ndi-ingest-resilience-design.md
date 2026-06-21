# NDI Ingest Resilience (T2.5) — Design

**Date:** 2026-06-21
**Status:** Approved (full scope) — grounded against `main` @ `a01da9f`.

## Summary

Harden the native NDI ingest path (`recorder_engine/ingest/nativendiingestsession.cpp`) to the same
24/7-resilience bar as the SRT and RTMP ingest sessions. Three parts:

1. **Fix the shutdown use-after-free / data race** on the NDI receiver handle (the genuine defect).
2. **Add a session-internal stall timer** for symmetry with SRT/RTMP and clearer failure classification.
3. **Add `e2e_native_ndi_reconnect` + `e2e_native_ndi_soak` gates** mirroring the SRT gold-standards.

## Grounding: what the code actually does today (and a roadmap correction)

The broadcast-readiness roadmap framed T2.5 as "an NDI source freeze hangs silently." That premise is
**partly wrong** and the design is scoped accordingly:

- **A frozen NDI source does NOT hang silently.** `StreamWorker` already runs an 8 s watchdog
  (`m_stallTimeoutMs = 8000`, `streamworker.h:174`; `streamworker.cpp:137-142`) that sets
  `m_restartCapture = 1` when connected-but-no-frames. The NDI `run()` loop polls `shouldStop()` every
  iteration (`nativendiingestsession.cpp:425,479-487`), the per-capture timeout is bounded
  (`kCaptureTimeoutMs = 100`, `:20`), and `shouldStop()` honors `m_restartCapture`
  (`streamworker.cpp:361-364`). So a `Capture::None` freeze is already broken after ~8 s and reconnects
  via `captureLoop`'s existing exponential backoff (`streamworker.cpp:484-534`).
- **The real defect is the shutdown UAF.** `requestStop()` (`nativendiingestsession.cpp:472-477`) runs on
  the worker/UI thread (via `StreamWorker::stop`, `streamworker.cpp:71-79`) and calls
  `m_backend->closeReceiver()` → `NDIlib_recv_destroy(m_receiver)` (`:194-199`) **while the capture
  thread may be inside** `capture()` → `NDIlib_recv_capture_v3(m_receiver)` (`:201-211`) or
  `freeVideo/freeAudio` (`:238-250`). `m_receiver` is a plain pointer with no mutex/atomic, read/null-checked
  at `:202,:239,:246` and nulled at `:198` — destroying it under an in-flight capture is a use-after-free
  plus a data race, hit on every stop and every worker-watchdog-triggered reconnect.
- **NDI lacks a session-internal stall timer** that SRT/RTMP have (SRT: `m_lastPacketAtMs` + per-loop
  break at `nativesrtingestsession.cpp:285-290`; RTMP: age check in `readMessage`,
  `nativertmpingestsession.cpp:640-646`). NDI's `run()` sleeps 1 ms forever on `Capture::None`
  (`:463-464`), relying entirely on the worker-level watchdog.

### Lifecycle (why the UAF fix is safe)

The session and its owned backend are created, used, **and destroyed entirely on the capture thread**:
`captureLoop` owns the `std::unique_ptr<IngestSession> session` (`streamworker.cpp:323-545`); after
`session->run()` returns it nulls `m_activeSession` under `m_sessionMutex` and, on exit, destroys the
session on the capture thread (so `~NativeNdiIngestSession` runs there too). `StreamWorker::stop()` sets
`m_captureRunning=false` + `requestStop()` then the owner **joins the capture thread**
(`streamworker.cpp:100`). The only cross-thread touch of the session is `requestStop()`. NDI's `capture()`
self-unblocks within `kCaptureTimeoutMs=100`, so — unlike SRT's blocking recv that needs `srt_close` to
unblock — `requestStop()` does **not** need to close the receiver to make `run()` return.

## Design

### Part 1 — Fix the shutdown UAF (make `requestStop()` flag-only)

Mirror the RTMP model: the stopping thread only flips a flag; the capture thread tears down its own
resources.

- **`requestStop()`** (`nativendiingestsession.cpp:472-477`): set `m_stopRequested = true` **only**.
  Remove the `m_backend->closeReceiver()` call. The capture thread sees `shouldStop()` within ≤100 ms and
  exits `run()`.
- **`run()`** (`:421-470`): after the loop exits, call `m_backend->closeReceiver()` **on the capture
  thread** (before/after the existing `setConnected(false)`), so the receiver is destroyed by the same
  thread that called `capture()`.
- **`~NativeNdiIngestSession()`** (`:351-357`): still call `requestStop()` (now flag-only); also call
  `m_backend->closeReceiver()` as an idempotent backstop for the path where `run()` never ran (e.g. open
  succeeded but the loop was never entered). `closeReceiver()` is already idempotent (`:194-199`,
  null-check + null-after-destroy). The destructor runs on the capture thread after `run()` has returned,
  so this never races `capture()`.

Net: `closeReceiver()`/`NDIlib_recv_destroy` is only ever called on the capture thread, never concurrently
with `capture()`/`free*()`. No mutex on the hot capture path; stop latency is bounded by the existing
100 ms capture timeout (and `stop()` already joins the capture thread, so it already waits for `run()`).

### Part 2 — Session-internal stall timer (symmetry + classification)

Mirror SRT (`nativesrtingestsession.cpp:285-290`), framed honestly as **symmetry + faster/clearer failure
classification**, not as the sole defense against an unbounded hang (the worker watchdog already covers the
`Capture::None` freeze; a hypothetical `capture()` that blocks past its 100 ms timeout is an SDK pathology
neither an elapsed-poll nor the worker watchdog can break, and is out of scope).

- Add a file-local `constexpr int kStallTimeoutMs = 8000;` (matching the existing per-file idiom in
  `nativesrtingestsession.cpp:25` / `nativertmpingestsession.cpp:20`; a one-line note that the three+ copies
  could converge later is in scope as a comment, consolidation is not).
- Track last-frame time using the existing `m_monotonic` (`nativendiingestsession.h:65`, already
  `start()`ed in the ctor). Add `int64_t m_lastFrameAtMs = -1;`. Initialize it in `open()` to
  `m_monotonic.elapsed()`; refresh it on every `Capture::Video` and `Capture::Audio`.
- In the `Capture::None` branch (`:463-464`), before the `msleep(1)`, break when
  `m_lastFrameAtMs >= 0 && m_monotonic.elapsed() - m_lastFrameAtMs > kStallTimeoutMs`: set
  `m_lastFailureKind = IngestFailureKind::TransientNetwork` (matches RTMP) and log
  `"Native NDI stalled. Restarting..."` via `m_callbacks.logInfo` (SRT's `log()` shape). `run()` returns →
  `captureLoop` reconnects. The clock-preservation guard (`if (!m_externalClock) m_clock->reset();`,
  `:403-405`) is already correct for NDI (external clock owned by `StreamWorker`) and must not regress.

### Part 3 — `e2e_native_ndi_reconnect` + `e2e_native_ndi_soak` gates

Mirror the SRT gold-standards (`run_srt_reconnect.sh`, `run_srt_soak.sh`) on the NDI runtime, using the
existing killable local source (`ndi_runtime_sender`, started + PID-captured + SKIP-77-on-runtime-absent in
`run_ndi_smoke.sh:44-61`) and the transport-agnostic `conn_events` telemetry
(`sync_harness --report-connection-events`, `sync_harness.cpp:104-165`, fed by `setConnected`).

- **`run_ndi_reconnect.sh` → `e2e_native_ndi_reconnect`:** two NDI senders (control `src0` + victim
  `src1`); record both in the background via `sync_harness`; kill `src1` mid-record; restart it on the same
  `--name` after an outage **> the 8 s stall window** (so the outage is not masked); assert
  (A) `src1` `conn_events` show up→down→up, (B) `src0` stays isolated (no mid-record down), (C) `src1` has
  pre-kill AND post-reconnect content. Content/resumption is asserted via ffprobe video-packet count in
  pre-kill vs post-reconnect time windows (`ndi_runtime_sender` has no per-second flash marker, unlike the
  SRT flash; packet-count windows mirror `run_ndi_smoke.sh:82-91`). **Teeth:** `OLR_NDI_RECONN_NO_RESTART=1`
  skips the restart → no second `up` / no post content → gate FAILS (proves it discriminates).
- **`run_ndi_soak.sh` → `e2e_native_ndi_soak`:** record one NDI source for `OLR_NDI_SOAK_SECS` (default 30 s);
  FAIL on no MKV (crash/hang), slow warmup, sustained-content shortfall (packet count), or a >1.5 s gap
  (mid-run stall). Slope/A-V are diagnostic-only (loopback shares a wall clock), mirroring `run_srt_soak.sh`.
- **CMake:** register both mirroring `e2e_native_ndi_smoke` (`tests/e2e/CMakeLists.txt:610-617`) and
  `e2e_native_srt_reconnect`/`_soak` (`:513-538`): label `native-ndi`, `RUN_SERIAL TRUE`,
  `SKIP_RETURN_CODE 77`, cross-platform (not `APPLE`/`WIN32`-gated). The reconnect gate uses `sync_harness`
  (for `--report-connection-events`) + two `ndi_runtime_sender`s; the soak uses `record_harness` or
  `sync_harness` with one sender.

### Part 4 — Unit regression (TDD)

Use the existing injectable seam `INdiReceiverBackend` + `FakeNdiReceiverBackend`
(`tests/unit/tst_ndiingest.cpp:11-44`) to lock in the production behavior without the NDI runtime:

- **Stall break:** a fake backend that returns `Capture::None` forever; drive `run()` with a fake clock so
  `m_monotonic` elapses past `kStallTimeoutMs`; assert `run()` returns and `lastFailureKind() ==
  TransientNetwork`. (Time is controlled by making the fake backend's `capture()` advance the test's notion
  of elapsed; if `m_monotonic` cannot be injected, gate the assertion on real elapsed with a reduced
  test-only timeout, or assert the break-condition logic in isolation — see plan.)
- **Flag-only stop / no close-on-stop-thread:** assert `requestStop()` does **not** call
  `closeReceiver()` (the fake records `closeReceiver` call count + the calling thread), and that
  `closeReceiver()` is invoked exactly once, from the thread that ran `run()`, after `run()` returns.
- **Idempotent close:** two `closeReceiver()` calls (run-teardown + destructor) destroy the receiver once.

## Hard constraints / non-goals

- **No new behavior on the hot capture path** beyond the stall-elapsed check (no per-frame mutex).
- **Do not regress the clock-preservation guard** (`if (!m_externalClock) m_clock->reset();`) — NDI uses the
  `StreamWorker`-owned `AnchoredSourceClock`, so same-URL reconnects keep recovered drift.
- **No consolidation** of the duplicated `kStallTimeoutMs` constants (out of scope; one-line comment only).
- **No new ingest signal** (`setReconnecting`/`ndiHealth`); reconnect is modeled as `setConnected(false)` +
  re-open, exactly as SRT/RTMP. (An `ndiHealth` grader is a possible follow-up, not T2.5.)
- The e2e gates SKIP-77 without the NDI runtime; CI lacks it, so they validate locally (same status as the
  existing `e2e_native_ndi_smoke` and the NDI output-validation lane).

## Files

- Modify: `recorder_engine/ingest/nativendiingestsession.{h,cpp}` (requestStop flag-only; run() teardown +
  stall timer; `m_lastFrameAtMs`; `kStallTimeoutMs`).
- Modify: `tests/unit/tst_ndiingest.cpp` (stall-break + flag-only-stop + idempotent-close regressions);
  `tests/unit/CMakeLists.txt` only if a new target is needed (it is not — extend the existing test).
- Create: `tests/e2e/run_ndi_reconnect.sh`, `tests/e2e/run_ndi_soak.sh`.
- Modify: `tests/e2e/CMakeLists.txt` (register `e2e_native_ndi_reconnect`, `e2e_native_ndi_soak`).
- Possibly modify: `tests/e2e/ndi_runtime_sender.cpp` only if the reconnect gate needs a second instance
  knob (it accepts `--name`/`--seconds` already; a second instance is just a second spawn).

## Verification

- Unit: `ctest -L unit` (full suite — a worker/ingest change can ripple).
- Sanitizers: the NDI ingest unit test should run under ASan/UBSan and TSan (the UAF/race fix is exactly
  what TSan guards); add `tst_ndiingest` to the sanitizer matrix if not already present.
- e2e: `e2e_native_ndi_reconnect` + `e2e_native_ndi_soak` locally (with the NDI runtime); SKIP-77 elsewhere.
- Independent concurrency review before merge (per CLAUDE.md, threading changes need it).
