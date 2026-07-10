# Phase 1 — Foundation & Safety

> Part of the [Broadcast-Perfection Plan](./README.md). The index holds the goal, architecture guardrails, non-goals, the quality gates referenced below, the governance rules and the audit lane. Read it first.

**Duration estimate.** ~2-3 months

## Why this phase, in this order

Nothing above is credible until the gate that enforces it is fail-closed and the control plane that drives recording is not LAN-open..

## Initiatives

### `arch-dependency-layer-lint` — CI include-graph / layering dependency lint
- **Size:** M
- **Dependencies:** `arch-ffmpeg-firewall`, `arch-engine-library`
- **Definition:** A CI lint enforcing module layering / the FFmpeg-header firewall via the include graph (the ci-gate fail-open is owned solely by qe-ci-gate-failclosed).
- **Acceptance criteria:**
  - [ ] a libav include reachable from any UI/websocket/control translation unit fails the lint
  - [ ] the layering lint runs in CI

### `arch-engine-library` — Extract a single canonical olr_engine static library linked by app + all tests
- **Size:** L
- **Dependencies:** none
- **Definition:** Compile every engine/UI-logic source exactly once into an olr_engine (and olr_engine_ui) static library that OpenLiveReplay and all test targets link, eliminating the triple-listed, independently-flagged source sets that let 'tests pass' diverge from ...
- **Acceptance criteria:**
  - [ ] A grep proves no engine .cpp is listed in more than one CMake target_sources block.
  - [ ] ctest -L unit passes linking olr_engine; adding a new engine .cpp requires editing exactly one CMakeLists location.

### `arch-ffmpeg-firewall` — Firewall libav headers out of UI, websocket AND the full DAG (record path included)
- **Size:** M
- **Dependencies:** `arch-engine-library`
- **Definition:** Make libav headers physically unreachable from uimanager.h, uimanagercontroladapter.h, and websocket/*, so the UI/control layers compile against a Qt-only engine facade — the precondition for a clean engine/UI split and for auditing the ...
- **Acceptance criteria:**
  - [ ] Preprocessing uimanager.cpp and every websocket/*.cpp yields zero libavformat/libavcodec/libavutil includes (CI check on -H / compile_commands include graph).
  - [ ] Full build + unit + e2e suite unchanged; playbackworker public API callers compile without libav on the include path.

### `ing-audio-degrade` — Graceful audio-unavailable, keep-video degrade
- **Size:** S
- **Dependencies:** none
- **Definition:** On a missing/failed audio decoder, drop audio and continue video with an amber audio-health state instead of failing the source.
- **Acceptance criteria:**
  - [ ] an unsupported-audio source keeps video with an amber audio badge (e2e)
  - [ ] no video interruption when the audio decoder errors

### `ing-windows-live-validation` — Prove and CI-gate the Windows live ingest path
- **Size:** M
- **Dependencies:** none
- **Definition:** Turn the compile-only Windows Media Foundation AAC/RTMP path into a real, automated Windows test lane.
- **Acceptance criteria:**
  - [ ] Windows CI runs a native-ingest e2e green per-PR
  - [ ] the AAC AudioSpecificConfig / 44.1k+48k pitch / mono->stereo assertions are automated

### `perf-latency-budget-harness` — End-to-end latency budget: instrumentation, headless harness, CI gate (incl. interactive scrub/jog metric)
- **Size:** L
- **Dependencies:** none
- **Definition:** Turn the existing OLR_LATENCY trace into a measured, budgeted, CI-gated glass-to-glass and control-to-air latency contract with per-stage p50/p99 histograms.
- **Acceptance criteria:**
  - [ ] e2e_latency_budget prints a per-stage p50/p99 table and exits non-zero if glass-to-glass p99 > 2 frames or control-to-air p99 > 2 frames on the reference fixture/hardware profile.
  - [ ] The span recorder adds < 50 ns per span in the on-air path (measured by an A/B run with spans disabled) so instrumentation does not perturb the budget it measures.
  - [ ] meets QG-LAT-GG

### `perf-trace-tooling` — Structured Perfetto/Chrome trace + flamegraph tooling
- **Size:** M
- **Dependencies:** `perf-latency-budget-harness`
- **Definition:** Make every latency regression diagnosable from a single trace: emit Perfetto/Chrome-trace-JSON spans and a CPU flamegraph per harness run.
- **Acceptance criteria:**
  - [ ] Running the play harness with OLR_TRACE set produces a JSON that loads in ui.perfetto.dev showing per-frame spans on named thread tracks.
  - [ ] The perf CI job attaches both the trace JSON and a flamegraph SVG as artifacts.

### `pi-control-api-hardening` — Authenticate (bearer token + Origin allowlist), encrypt (TLS1.3/wss), rate/size/connection-limit, capability-tier and path-sandbox the control API, with a privileged-command audit log
- **Size:** M
- **Dependencies:** none
- **Definition:** Turn the unauthenticated 0.0.0.0 WebSocket control API into a professionally deployable, authenticated, audited surface — the security baseline every broadcast/NMOS/automation integration depends on.
- **Acceptance criteria:**
  - [ ] New tst_controlauth unit test: unauthenticated command rejected with not_authenticated; valid token accepted; Origin mismatch rejected
  - [ ] E2E: non-loopback connect without token is refused AND written to the audit log; wss handshake with a self-signed cert succeeds
  - [ ] meets QG-FAC-NMOS

### `qe-ci-gate-failclosed` — Make the CI gate provably fail-closed (negative self-test), preserving the docs-only skip path
- **Size:** S
- **Dependencies:** none
- **Definition:** Make the required 'CI gate' provably fail-closed: add the change-classifier to its needs, treat an errored/skipped classifier as failure, and prove it with a negative self-test; this is the sole owner of the fail-open fix and also covers the build/sanitizer/fuzz-smoke security gates.
- **Acceptance criteria:**
  - [ ] a deliberately-broken change-classifier turns the required 'CI gate' RED (negative self-test)
  - [ ] the docs-only skip path still works
  - [ ] meets QG-CI-CLOSED

### `qe-flake-observability` — Flake detection, JUnit ingestion, quarantine
- **Size:** S
- **Dependencies:** `qe-ci-gate-failclosed`
- **Definition:** Stop until-pass retries from hiding chronic flakes: measure per-test stability and quarantine offenders instead of silently retrying.
- **Acceptance criteria:**
  - [ ] JUnit results uploaded for every ctest leg
  - [ ] A test that fails-then-passes on retry is counted and surfaced (not invisibly absorbed)

### `qe-qml-lint-format-full` — Lint and format all 32/33 QML files; enforce qmlformat
- **Size:** S
- **Dependencies:** none
- **Definition:** Close the QML coverage hole so no un-linted style override or theme file can ship a runtime error, and make qmlformat enforced.
- **Acceptance criteria:**
  - [ ] All 32 product QML pass 'qmllint --unqualified error --Quick.layout-positioning error'
  - [ ] qmlformat diff is enforced empty on changed QML

### `qe-windows-ctest-lane` — Promote Windows from build-only to a real unit + native-ingest e2e test lane; wire iOS into CI
- **Size:** M
- **Dependencies:** `qe-ci-gate-failclosed`
- **Definition:** Run the deterministic unit label plus a native-ingest e2e on Windows so the Media Foundation decode/encode and native RTMP/SRT paths are automatically validated, not manually smoke-tested.
- **Acceptance criteria:**
  - [ ] windows-latest runs the full deterministic unit label green (not 4 tests)
  - [ ] A Windows native-ingest record e2e produces a demuxable MKV in CI

### `rel-output-autorecover` — Output device-loss auto-recovery state machine (closes outputdispatcher.cpp:251)
- **Size:** M
- **Dependencies:** `rel-telemetry-bus`
- **Definition:** Make every output endpoint self-heal: on device loss or repeated submit failure, re-run the open/start handshake on backoff and re-arm automatically, holding last-good video during the gap.
- **Acceptance criteria:**
  - [ ] tst_outputdispatcher: a fake sink that flips inactive then active is auto-reopened without external calls; downtime and reopen count are counted; last-good frame is held throughout
  - [ ] Backoff is bounded and never busy-loops (verified by tick accounting)
  - [ ] meets QG-REL-RECOVER

### `rel-telemetry-bus` — Structured ops telemetry bus + loopback-only /healthz + /metrics + rotating log
- **Size:** M
- **Dependencies:** none
- **Definition:** Stand up the process-wide operational observability substrate every other reliability feature reports through, and expose it in formats a broadcast NOC can consume.
- **Acceptance criteria:**
  - [ ] tst_opstelemetry: counters/gauges/events publish and read back thread-safely under TSan
  - [ ] curl /metrics returns valid Prometheus text with the documented metric set; /healthz reflects record/output liveness

### `sec-amf0-extract-fuzz` — Extract the AMF0 scanner to a pure module and fuzz it; add AAC/ADTS and control-JSON fuzz targets
- **Size:** M
- **Dependencies:** none
- **Definition:** Bring the recursive attacker-facing AMF0 command/metadata walker under isolated unit + fuzz coverage, and prove its depth/bounds guards.
- **Acceptance criteria:**
  - [ ] fuzz_amf0scanner builds under -fsanitize=fuzzer,address,undefined and survives an extended run with 0 crashes/leaks/UB.
  - [ ] Unit tests assert the 64-depth cap and that every malformed input returns Malformed without OOB read.

### `sec-control-loopback-default` — Fail-closed default: control API binds loopback unless auth+TLS configured
- **Size:** S
- **Dependencies:** `sec-threat-model`
- **Definition:** Invert the dangerous default so an out-of-the-box install is not remotely drivable;
- **Acceptance criteria:**
  - [ ] New unit test (tst_appenv / tst_controlbind): default policy resolves to loopback; 'all' without token+TLS returns a refuse-to-bind result.
  - [ ] Manual/e2e: launching with defaults, a connection from a non-loopback address is impossible (server not listening there).
  - [ ] meets QG-SEC-CTRL

### `sec-fuzz-ci-continuous` — Continuous, coverage-tracked fuzzing gated on parser changes
- **Size:** M
- **Dependencies:** `sec-amf0-extract-fuzz`, `sec-h26x-au-cap`
- **Definition:** Turn fuzzing from a 90s manual afterthought into a measured, gating, continuously-running assurance for every untrusted parser.
- **Acceptance criteria:**
  - [ ] A PR that modifies a parser triggers the fuzz smoke and fails on any crash reproducer (crash artifact uploaded).
  - [ ] Nightly run publishes per-harness edge-coverage; coverage does not regress across releases.

### `sec-h26x-au-cap` — Bound the pending access-unit buffer AND attacker-controlled dimension/count ceilings
- **Size:** S
- **Dependencies:** none
- **Definition:** Eliminate the last unbounded ingest accumulator: cap m_pendingAnnexB so a malformed/hostile elementary stream cannot exhaust memory.
- **Acceptance criteria:**
  - [ ] Unit test: feeding K MB of VCL slices with no new-picture trigger keeps m_pendingAnnexB <= cap and drops+resyncs.
  - [ ] fuzz_h26xaccessunit runs the slice-storm seed with bounded RSS (ASan no OOM) over an extended run.

### `sec-secrets` — Secret hygiene: redaction, keychain storage, scoped snapshot exposure
- **Size:** M
- **Dependencies:** none
- **Definition:** Ensure SRT passphrases, control tokens, and TLS keys are never logged, never sent in cleartext to under-privileged clients, and stored via OS secret stores.
- **Acceptance criteria:**
  - [ ] Unit test: a URL with passphrase logs as masked; snapshot to a Monitor client omits the passphrase; to a Configure client includes it only over wss.
  - [ ] Grep-based CI check: no code path logs a raw ingest URL.

### `sec-supply-chain` — Verified, parity dependency builds + SBOM + CVE scanning
- **Size:** L
- **Dependencies:** `sec-threat-model`
- **Definition:** Move from git-SHA pinning to verified provenance across all platforms, so the broadcast binary cannot inherit a tampered dependency.
- **Acceptance criteria:**
  - [ ] A tampered dependency (wrong sha256) fails the build on every platform.
  - [ ] Parity lint fails if any build script pins a version/hash diverging from the shared manifest.

### `sec-threat-model` — Written STRIDE threat model incl. SSRF classification of import/SSE URLs and runtime-NDI load path
- **Size:** M
- **Dependencies:** none
- **Definition:** Establish the auditable security baseline the rest of the track enforces: enumerate assets, trust boundaries, attacker capabilities, and per-surface mitigations, so hardening is designed against a model, not improvised.
- **Acceptance criteria:**
  - [ ] threat-model.md enumerates every ControlProtocol command (controlprotocol.cpp) with its required capability tier and every ingest parser with its bounding strategy.
  - [ ] SECURITY.md present and linked from README.

### `tc-ptp-hardening` — Harden the opt-in PTP runtime (torn reads + message matching)
- **Size:** S
- **Dependencies:** none
- **Definition:** Close the review-confirmed torn-read and unmatched Follow_Up/Delay_Resp defects in the opt-in software-PTP path.
- **Acceptance criteria:**
  - [ ] a torn-read / monotonicity test passes across repeated lock<->loss cycles
  - [ ] a spoofed-port or mismatched-sequence Follow_Up is rejected

---

[Index](./README.md) · [Implementation plan](./impl/phase-1-foundation-safety.md) · [Phase 2 »](./phase-2-extensible-core.md)
