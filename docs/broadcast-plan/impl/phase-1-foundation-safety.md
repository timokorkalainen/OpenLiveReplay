# Phase 1 — Implementation plan

> Full task stacks for the active phase. Each initiative is test-first (RED → GREEN → refactor), one logical commit per slice. Files lists were verified at plan time and are re-verified at slice start.

### `arch-dependency-layer-lint` — CI include-graph / layering dependency lint
**Size:** M  ·  **Dependencies:** `arch-ffmpeg-firewall`, `arch-engine-library`

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: a libav include reachable from any UI/websocket/control translation unit fails the lint.
2. GREEN — A CI lint enforcing module layering / the FFmpeg-header firewall via the include graph (the ci-gate fail-open is owned solely by qe-ci-gate-failclosed).
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

### `arch-engine-library` — Extract a single canonical olr_engine static library linked by app + all tests
**Size:** L  ·  **Dependencies:** none

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: A grep proves no engine .cpp is listed in more than one CMake target_sources block..
2. GREEN — Compile every engine/UI-logic source exactly once into an olr_engine (and olr_engine_ui) static library that OpenLiveReplay and all test targets link, eliminating the triple-listed, independently-flagged source sets that let 'tests pass' diverge from ...
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- Add olr_engine STATIC in CMakeLists.txt aggregating recorder_engine/**, playback/** (non-GPU + GPU behind OLR_GPU_PIPELINE_BUILD), websocket/**, and the FFmpeg IMPORTED targets; keep main.cpp + QML in qt_add_executable(OpenLiveReplay) linking olr_engine.
- Split UI-facing QObjects (uimanager.*, control adapters) into olr_engine_ui if they must see Qt Gui; keep pure engine in olr_engine (Qt::Core only).
- Rewrite tests/CMakeLists.txt olr_test_core/olr_test_engine/olr_test_playback (lines 76/146/222) to link olr_engine instead of re-listing .cpp via target_sources (replaymanager.cpp:149, playbackworker.cpp:230, ...).
- Centralize compile defines (OLR_UNIT_TEST, OLR_GPU_PIPELINE_BUILD, sanitizer flags via olr_warnings/olr_sanitize) so app and test builds are flag-identical.

#### PR slices: `arch-engine-library`
| Slice | Scope | Proof | Rollback / evidence | Merge prerequisite |
|-------|-------|-------|---------------------|--------------------|
| 1 | Add olr_engine STATIC in CMakeLists.txt aggregating recorder_engine/**, playback/** (non-GPU + GPU behind OLR_GPU_PIPELINE_BUILD), websocket/**, and the FFmpeg IMPORTED targets; keep main.cpp + QML in qt_add_executable(OpenLiveReplay) linking olr_engine. | A grep proves no engine .cpp is listed in more than one CMake target_sources block. | revert slice; CI gate green | none |
| 2 | Split UI-facing QObjects (uimanager.*, control adapters) into olr_engine_ui if they must see Qt Gui; keep pure engine in olr_engine (Qt::Core only). | ctest -L unit passes linking olr_engine; adding a new engine .cpp requires editing exactly one CMakeLists location. | revert slice; CI gate green | slice 1 |

### `arch-ffmpeg-firewall` — Firewall libav headers out of UI, websocket AND the full DAG (record path included)
**Size:** M  ·  **Dependencies:** `arch-engine-library`

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: Preprocessing uimanager.cpp and every websocket/*.cpp yields zero libavformat/libavcodec/libavutil includes (CI check on -H / compile_commands include graph)..
2. GREEN — Make libav headers physically unreachable from uimanager.h, uimanagercontroladapter.h, and websocket/*, so the UI/control layers compile against a Qt-only engine facade — the precondition for a clean engine/UI split and for auditing the ...
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- Introduce a PIMPL/opaque boundary for PlaybackWorker: move the extern "C" libav includes (playbackworker.h:35-40), DecoderTrack/AudioDecoderTrack structs, colorMetadataForAvFrame, and all AVFrame*/AVCodecContext* members into a new playbackworker_internal.h / PlaybackWorkerImpl compiled only inside olr_engine.
- Reduce playbackworker.h to a Qt-only public surface (openFile/seekTo/armNextCut/counters/signals) consumed by uimanager.h:23 and uimanagercontroladapter.h.
- Move colorMetadataForAvFrame and any AVFrame-typed helpers behind the internal header; expose the ColorMetadata result (already a plain struct) publicly.
- Add an include-graph guard (below) asserting no UI/websocket TU pulls libav*.

### `ing-audio-degrade` — Graceful audio-unavailable, keep-video degrade
**Size:** S  ·  **Dependencies:** none

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: an unsupported-audio source keeps video with an amber audio badge (e2e).
2. GREEN — On a missing/failed audio decoder, drop audio and continue video with an amber audio-health state instead of failing the source.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

### `ing-windows-live-validation` — Prove and CI-gate the Windows live ingest path
**Size:** M  ·  **Dependencies:** none

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: Windows CI runs a native-ingest e2e green per-PR.
2. GREEN — Turn the compile-only Windows Media Foundation AAC/RTMP path into a real, automated Windows test lane.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- Run the docs/windows-aac-rtmp-smoke-runbook.md live smoke on real hardware (44.1/48 kHz, mono->stereo, MFT stream-change)
- Harden nativeaacdecoder_mediafoundation.cpp MF_E_TRANSFORM_STREAM_CHANGE + AudioSpecificConfig USER_DATA paths under sustained load
- Add a windows-latest ctest job to CI running tst_nativeaacdecoder + a native-ingest e2e
- Confirm caps.hevc=false graceful path when HEVC Video Extensions absent (nativevideodecoder_mediafoundation.cpp)

### `perf-latency-budget-harness` — End-to-end latency budget: instrumentation, headless harness, CI gate (incl. interactive scrub/jog metric)
**Size:** L  ·  **Dependencies:** none

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: e2e_latency_budget prints a per-stage p50/p99 table and exits non-zero if glass-to-glass p99 > 2 frames or control-to-air p99 > 2 frames on the reference fixture/hardware profile..
2. GREEN — Turn the existing OLR_LATENCY trace into a measured, budgeted, CI-gated glass-to-glass and control-to-air latency contract with per-stage p50/p99 histograms.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- Add a lightweight lock-free span recorder (new playback/telemetry/latencyspans.{h,cpp}) that OLR_LATENCY sites feed instead of only qInfo(); accumulate per-stage HdrHistogram-style buckets (ingest->decode, decode->composite, composite->readback, readback->sink-submit, submit->on-air).
- Stamp a monotonic capture-time and command-time onto frames/commands: extend OutputBusFrame identity (playback/output/outputframecache.* / mediaframe.h) with a captureMonotonicNs and a commandSeq so control-to-air can be attributed to the originating take/recall/cut in playbackworker.cpp.
- New headless harness tests/e2e/run_latency_budget_e2e.sh + a driver that plays a fixture, drives takes/cuts via the transport, scrapes the span recorder, and asserts p50/p99 against budgets.
- Register e2e_latency_budget in CTest (opt-in label 'perf'); wire into .github/workflows/ci.yml as a gated job.

#### PR slices: `perf-latency-budget-harness`
| Slice | Scope | Proof | Rollback / evidence | Merge prerequisite |
|-------|-------|-------|---------------------|--------------------|
| 1 | Add a lightweight lock-free span recorder (new playback/telemetry/latencyspans.{h,cpp}) that OLR_LATENCY sites feed instead of only qInfo(); accumulate per-stage HdrHistogram-style buckets (ingest->decode, decode->composite, composite->readback, readback->sink-submit, submit->on-air). | e2e_latency_budget prints a per-stage p50/p99 table and exits non-zero if glass-to-glass p99 > 2 frames or control-to-air p99 > 2 frames on the reference fixture/hardware profile. | revert slice; CI gate green | none |
| 2 | Stamp a monotonic capture-time and command-time onto frames/commands: extend OutputBusFrame identity (playback/output/outputframecache.* / mediaframe.h) with a captureMonotonicNs and a commandSeq so control-to-air can be attributed to the originating take/recall/cut in playbackworker.cpp. | The span recorder adds < 50 ns per span in the on-air path (measured by an A/B run with spans disabled) so instrumentation does not perturb the budget it measures. | revert slice; CI gate green | slice 1 |

### `perf-trace-tooling` — Structured Perfetto/Chrome trace + flamegraph tooling
**Size:** M  ·  **Dependencies:** `perf-latency-budget-harness`

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: Running the play harness with OLR_TRACE set produces a JSON that loads in ui.perfetto.dev showing per-frame spans on named thread tracks..
2. GREEN — Make every latency regression diagnosable from a single trace: emit Perfetto/Chrome-trace-JSON spans and a CPU flamegraph per harness run.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- Extend the span recorder from perf-latency-budget-harness to serialize Chrome-trace JSON (thread tracks for decode/composite/readback/dispatch/sink) behind OLR_TRACE=path.
- Add a scripts/perf/collect_flamegraph.sh wrapper (dtrace/Instruments on macOS, ETW/WPR on Windows, perf on Linux) around the play_harness.
- Emit the trace as a CI artifact from the perf job; document the workflow in docs/perf-tracing.md.

### `pi-control-api-hardening` — Authenticate (bearer token + Origin allowlist), encrypt (TLS1.3/wss), rate/size/connection-limit, capability-tier and path-sandbox the control API, with a privileged-command audit log
**Size:** M  ·  **Dependencies:** none

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: New tst_controlauth unit test: unauthenticated command rejected with not_authenticated; valid token accepted; Origin mismatch rejected.
2. GREEN — Turn the unauthenticated 0.0.0.0 WebSocket control API into a professionally deployable, authenticated, audited surface — the security baseline every broadcast/NMOS/automation integration depends on.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- main.cpp:64-70 — default bind to QHostAddress::LocalHost; require OLR_CONTROL_BIND=any to expose on the LAN; load TLS cert/key for wss
- websocket/controlwebsocketserver.{h,cpp} — bearer-token handshake, Origin allowlist, QSslServer/wss, per-client RBAC role
- new websocket/controlauth.{h,cpp} — token store, constant-time compare, structured audit-log sink
- websocket/controlprotocol.cpp — auth-required gate + new not_authenticated error code

### `qe-ci-gate-failclosed` — Make the CI gate provably fail-closed (negative self-test), preserving the docs-only skip path
**Size:** S  ·  **Dependencies:** none

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: a deliberately-broken change-classifier turns the required 'CI gate' RED (negative self-test).
2. GREEN — Make the required 'CI gate' provably fail-closed: add the change-classifier to its needs, treat an errored/skipped classifier as failure, and prove it with a negative self-test; this is the sole owner of the fail-open fix and also covers the build/sanitizer/fuzz-smoke security gates.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- .github/workflows/ci.yml: add 'changes' to ci-gate 'needs' and assert needs.changes.result=='success'
- ci-gate: when needs.changes.outputs.app=='true', require build-test-macos AND build-test-linux AND build-test-windows results to be 'success' (reject 'skipped')
- Add a negative self-test job (or a tests/ci/ script) that runs the gate logic against a synthetic all-skipped matrix and asserts it returns non-zero
- docs/build-and-run.md / tests/README.md: document 'CI gate' as the single stable required check and the fail-closed contract

### `qe-flake-observability` — Flake detection, JUnit ingestion, quarantine
**Size:** S  ·  **Dependencies:** `qe-ci-gate-failclosed`

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: JUnit results uploaded for every ctest leg.
2. GREEN — Stop until-pass retries from hiding chronic flakes: measure per-test stability and quarantine offenders instead of silently retrying.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- .github/workflows/ci.yml: emit CTest JUnit XML (--output-junit) from each ctest leg and upload as artifacts
- New build-scripts/flake_report.sh aggregating pass/fail-on-retry across a rolling window
- Convert chronic offenders to an explicit tests/quarantine.txt (run, reported, non-gating) rather than masked by --repeat until-pass
- Per-test timing budget report to catch slow-creep

### `qe-qml-lint-format-full` — Lint and format all 32/33 QML files; enforce qmlformat
**Size:** S  ·  **Dependencies:** none

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: All 32 product QML pass 'qmllint --unqualified error --Quick.layout-positioning error'.
2. GREEN — Close the QML coverage hole so no un-linted style override or theme file can ship a runtime error, and make qmlformat enforced.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- .github/workflows/ci.yml lint job: extend the qmllint file list (ci.yml:267-280) to all 18 ui/style/*.qml + ui/theme/Theme.qml
- One-shot 'qmlformat -i' across the QML tree, then flip the qmlformat step from continue-on-error (ci.yml:285-292) to a gate
- Register the existing tests/qmlstyle/tst_*.qml Quick Tests into a ctest label run on the desktop legs

### `qe-windows-ctest-lane` — Promote Windows from build-only to a real unit + native-ingest e2e test lane; wire iOS into CI
**Size:** M  ·  **Dependencies:** `qe-ci-gate-failclosed`

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: windows-latest runs the full deterministic unit label green (not 4 tests).
2. GREEN — Run the deterministic unit label plus a native-ingest e2e on Windows so the Media Foundation decode/encode and native RTMP/SRT paths are automatically validated, not manually smoke-tested.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- .github/workflows/ci.yml build-test-windows: run 'ctest -L unit' excluding only the known-hanging MF video probe by name, instead of the 4 hard-coded targets (ci.yml:547-548)
- Add a Windows native SRT loopback record e2e (reuse tests/e2e/record_harness + srt bridge) as a ctest, offscreen QPA
- Scope-enable -Werror on Windows with '-Wno-error=null-dereference' (isolate the Qt qhash.h generated-loader false positive noted at ci.yml:487-489) so first-party warnings gate
- Investigate running tst_nativevideodecoder via WARP/software MFT instead of skipping it (ci.yml:521-527)

### `rel-output-autorecover` — Output device-loss auto-recovery state machine (closes outputdispatcher.cpp:251)
**Size:** M  ·  **Dependencies:** `rel-telemetry-bus`

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: tst_outputdispatcher: a fake sink that flips inactive then active is auto-reopened without external calls; downtime and reopen count are counted; last-good frame is held throughout.
2. GREEN — Make every output endpoint self-heal: on device loss or repeated submit failure, re-run the open/start handshake on backoff and re-arm automatically, holding last-good video during the gap.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- Extend IOutputSink (playback/output/outputsink.h) with reopen() and a structured failure reason on OutputSinkStatus
- Add a per-endpoint recovery FSM (Healthy/Degraded/Recovering/Down) in OutputDispatcher/OutputRuntime driving off !isActive() or a submit-failure streak at the current skip site (outputdispatcher.cpp:249-266); exponential backoff with jitter
- Implement reopen() in NdiOutputSink (ndisink.cpp) and the iotargets stubs (decklinksink/ajasink/omtsink) so the handshake path is uniform
- Keep m_holdLastFrame (outputdispatcher.cpp:274-291) painting last-good video while Recovering; emit outputDeviceLost/outputDeviceRegained telemetry with downtime duration

### `rel-telemetry-bus` — Structured ops telemetry bus + loopback-only /healthz + /metrics + rotating log
**Size:** M  ·  **Dependencies:** none

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: tst_opstelemetry: counters/gauges/events publish and read back thread-safely under TSan.
2. GREEN — Stand up the process-wide operational observability substrate every other reliability feature reports through, and expose it in formats a broadcast NOC can consume.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- New ops/opstelemetry.{h,cpp}: thread-safe counters/gauges/severity-tagged events (distinct from the replay-data SSE client in telemetry/telemetryevent.h)
- Publishers added to Muxer, StreamWorker, ReplayManager, OutputDispatcher/OutputRuntime, GpuDeviceLossMonitor for write bitrate, source health/ppm, queue depths, dropped/held frames, device-loss events
- Read-only HTTP endpoints: GET /healthz (liveness/readiness JSON) and GET /metrics (Prometheus text) served from a small ops HTTP listener bound to loopback by default
- Extend the WebSocket control state (controlstate.cpp:97-104) with an ops-health object

### `sec-amf0-extract-fuzz` — Extract the AMF0 scanner to a pure module and fuzz it; add AAC/ADTS and control-JSON fuzz targets
**Size:** M  ·  **Dependencies:** none

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: fuzz_amf0scanner builds under -fsanitize=fuzzer,address,undefined and survives an extended run with 0 crashes/leaks/UB..
2. GREEN — Bring the recursive attacker-facing AMF0 command/metadata walker under isolated unit + fuzz coverage, and prove its depth/bounds guards.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- New recorder_engine/ingest/amf0scanner.{h,cpp}: move amf0ValueContainsString/scanAmf0ObjectEntries/amf0ScanObjectForStringKey/amf0DataMessageTimecode/commandPayloadContainsReconnectRequest (nativertmpingestsession.cpp:56-288) out of the anonymous namespace into a testable, Qt-light unit.
- nativertmpingestsession.cpp: include and call the extracted module (behavior-preserving).
- New tests/fuzz/fuzz_amf0scanner.cpp + tests/fuzz/CMakeLists.txt target; committed seeds (onStatus/ReconnectRequest, onMetaData/@setDataFrame with tc, deeply nested, truncated).
- New tests/unit/tst_amf0scanner.cpp: depth-cap, truncation, cyclic-ish nesting, huge count field.

### `sec-control-loopback-default` — Fail-closed default: control API binds loopback unless auth+TLS configured
**Size:** S  ·  **Dependencies:** `sec-threat-model`

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: New unit test (tst_appenv / tst_controlbind): default policy resolves to loopback; 'all' without token+TLS returns a refuse-to-bind result..
2. GREEN — Invert the dangerous default so an out-of-the-box install is not remotely drivable;
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- main.cpp:64-77: default controlBindAddress to QHostAddress::LocalHost; require OLR_CONTROL_BIND=all/public (or an explicit interface) to bind non-loopback, and log a prominent warning when a routable bind is chosen.
- Refuse to start a non-loopback listener unless a token (sec-control-auth-token) AND TLS (sec-control-tls-wss) are configured — hard fail with a clear diagnostic rather than silently exposing an open API.
- appenv.h/appenv.cpp: add controlBindPolicy() parsing/validation with unit coverage.

### `sec-fuzz-ci-continuous` — Continuous, coverage-tracked fuzzing gated on parser changes
**Size:** M  ·  **Dependencies:** `sec-amf0-extract-fuzz`, `sec-h26x-au-cap`

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: A PR that modifies a parser triggers the fuzz smoke and fails on any crash reproducer (crash artifact uploaded)..
2. GREEN — Turn fuzzing from a 90s manual afterthought into a measured, gating, continuously-running assurance for every untrusted parser.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- ci.yml fuzz job: add a scheduled (nightly) long-run that persists/reuses corpus (cache), and a PR-triggered short smoke that RUNS (and gates) whenever changes.native touches recorder_engine/ingest/** or tests/fuzz/**.
- Add -fsanitize-coverage tracking + a coverage summary artifact per harness; add all five harnesses (incl. sec-amf0scanner).
- Grow committed seeds; document a corpus-minimization step.
- Optionally prepare an OSS-Fuzz project manifest for external continuous fuzzing.

### `sec-h26x-au-cap` — Bound the pending access-unit buffer AND attacker-controlled dimension/count ceilings
**Size:** S  ·  **Dependencies:** none

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: Unit test: feeding K MB of VCL slices with no new-picture trigger keeps m_pendingAnnexB <= cap and drops+resyncs..
2. GREEN — Eliminate the last unbounded ingest accumulator: cap m_pendingAnnexB so a malformed/hostile elementary stream cannot exhaust memory.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- h26xaccessunit.cpp:263-268: add kMaxPendingAccessUnitBytes; on overflow, drop the in-progress AU and resync at the next picture-start/AUD (mirroring the mpegtsparser PES-cap recovery at mpegtsparser.cpp:245-249) rather than emit garbage.
- h26xaccessunit.h: document the invariant.
- Extend fuzz_h26xaccessunit seeds with a never-flushing slice storm; add a unit test asserting the buffer never exceeds the cap.

### `sec-secrets` — Secret hygiene: redaction, keychain storage, scoped snapshot exposure
**Size:** M  ·  **Dependencies:** none

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: Unit test: a URL with passphrase logs as masked; snapshot to a Monitor client omits the passphrase; to a Configure client includes it only over wss..
2. GREEN — Ensure SRT passphrases, control tokens, and TLS keys are never logged, never sent in cleartext to under-privileged clients, and stored via OS secret stores.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- Add a redaction helper and apply to all URL logging (nativesrtingestsession.cpp:689-698 log(), any URL qDebug in ingest/uimanager) so passphrase/streamid query params are masked.
- controlstate.cpp / uimanagercontroladapter.cpp: strip or mask the passphrase from source URLs in the control snapshot for Monitor-tier clients; only Configure-tier over wss may see/set it.
- Store control token + SRT passphrase via keychain/DPAPI (macOS Keychain, Windows Credential Manager/DPAPI, libsecret) with an env fallback; keep settings file free of cleartext secrets.
- Best-effort memory zeroization for secret buffers; tests asserting redaction and scoped snapshot filtering.

### `sec-supply-chain` — Verified, parity dependency builds + SBOM + CVE scanning
**Size:** L  ·  **Dependencies:** `sec-threat-model`

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: A tampered dependency (wrong sha256) fails the build on every platform..
2. GREEN — Move from git-SHA pinning to verified provenance across all platforms, so the broadcast binary cannot inherit a tampered dependency.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- build-scripts/build_ffmpeg_*.sh (macos/macos_app/ios/windows) + SRT/OpenSSL/NDI fetch: add SHA-256 (and GPG signature where upstream publishes) verification of every fetched tarball/checkout, failing the build on mismatch; factor the pinned {ref, sha256} into a single shared manifest consumed by all scripts to guarantee macOS/iOS/Windows/Linux parity.
- Generate a CycloneDX SBOM per release listing every third-party component + version + hash.
- Document the update procedure (bump ref -> record new sha256 -> regenerate SBOM) in docs/security/supply-chain.md.
- CI: verify all four platform build scripts reference the same manifest (parity lint).

#### PR slices: `sec-supply-chain`
| Slice | Scope | Proof | Rollback / evidence | Merge prerequisite |
|-------|-------|-------|---------------------|--------------------|
| 1 | build-scripts/build_ffmpeg_*.sh (macos/macos_app/ios/windows) + SRT/OpenSSL/NDI fetch: add SHA-256 (and GPG signature where upstream publishes) verification of every fetched tarball/checkout, failing the build on mismatch; factor the pinned {ref, sha256} into a single shared manifest consumed by all scripts to guarantee macOS/iOS/Windows/Linux parity. | A tampered dependency (wrong sha256) fails the build on every platform. | revert slice; CI gate green | none |
| 2 | Generate a CycloneDX SBOM per release listing every third-party component + version + hash. | Parity lint fails if any build script pins a version/hash diverging from the shared manifest. | revert slice; CI gate green | slice 1 |

### `sec-threat-model` — Written STRIDE threat model incl. SSRF classification of import/SSE URLs and runtime-NDI load path
**Size:** M  ·  **Dependencies:** none

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: threat-model.md enumerates every ControlProtocol command (controlprotocol.cpp) with its required capability tier and every ingest parser with its bounding strategy..
2. GREEN — Establish the auditable security baseline the rest of the track enforces: enumerate assets, trust boundaries, attacker capabilities, and per-surface mitigations, so hardening is designed against a model, not improvised.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

**Files (verified at plan time)**
- Add docs/security/threat-model.md: STRIDE table + a data-flow diagram (ingest sources -> parsers -> engine -> control API -> operators/integrations), trust-boundary map (loopback vs LAN vs source network), and the abuse cases for each control command and each parser.
- Add SECURITY.md at repo root: supported versions, private disclosure channel, CVSS triage SLA.
- Add docs/security/control-api-security.md documenting the authn/authz/TLS model the sec-control-* initiatives implement.
- Cite AMWA NMOS IS-10/BCP-003-01/-02, EBU R143, OWASP ASVS mapping.

### `tc-ptp-hardening` — Harden the opt-in PTP runtime (torn reads + message matching)
**Size:** S  ·  **Dependencies:** none

**Test-first steps**
1. RED — add the failing test named by the acceptance criteria: a torn-read / monotonicity test passes across repeated lock<->loss cycles.
2. GREEN — Close the review-confirmed torn-read and unmatched Follow_Up/Delay_Resp defects in the opt-in software-PTP path.
3. REFACTOR + commit (one logical commit per slice; keep the diff inside the declared Files list).

---

[Phase 1 initiatives](../phase-1-foundation-safety.md) · [Index](../README.md)
