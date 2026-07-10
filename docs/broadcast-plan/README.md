# Broadcast-Perfection Plan — canonical index

The machine-checkable roadmap and execution model for taking OpenLiveReplay to
award-winning professional-broadcast level. Agents work this plan non-stop; the
human steers by exception through [`directives.md`](./directives.md). The
[operating loop](./operating-loop.md) is the runbook; this index is the "what".

Everything here is enforced by a fast structural audit
([`tools/roadmap/audit.py`](../../tools/roadmap/audit.py)) wired into the
pre-commit hook, the pre-push docs gate, and a required CI job — so a malformed
roadmap cannot merge. Run it any time: `python tools/roadmap/audit.py`.

## Goal

Make OpenLiveReplay a certifiable, standards-conformant professional replay
engine a production gallery trusts as a clean source: instant, frame-accurate
multi-angle recall on a tactile LSM-class surface; a 10-bit / 4:2:2,
colorimetrically-correct, HDR-capable, super-slow-motion image chain;
genlocked / PTP-disciplined PGM/PVW/multiview over SDI, ST 2110 and NDI; a native
AMWA NMOS node; record-while-replay with N+1 redundancy — every headline claim
instrumented, tiered honestly, and proven under a 24/7 fault-injection soak.
Primary users: replay operators and technical directors on event-safety, sports
and small-to-mid live productions. It wins **both** a technical-excellence award
(measured, standards-verified engineering) and a usability award (muscle-memory
operation where a mis-click never dumps a dirty frame to air).

## Architecture guardrails

- **Preserve the shipped core.** Qt6 / C++17 / QML; the native-only ingest
  selector; the frame-sync program (Phases 0–5, delivered); the GPU
  compositor/readback pipeline; and FFmpeg confined to the record/mux layer -- a
  firewall this plan *completes* (headers still leak into UI/websocket today; see
  `arch-ffmpeg-firewall`) and never widens.
- **Extensibility before surfaces.** New sinks, decoders, control surfaces and
  hardware I/O enter through registries/SPIs, not through edits to the
  PlaybackWorker / UIManager god-objects or `#ifdef` ladders.
- **Honest tiering, never overclaiming.** Every source advertises
  FrameAccurate / Bounded ±ms / Approximate and a Local/PTP reference tier.
  Software PTP is ~tens of µs, **not** genlock-grade; genlock claims are gated on
  real hardware in Phase 6 and stated numerically.
- **Every claim is a measured, gated number.** No feature is done until it holds
  its quality gate under a soak and a clean ASan/UBSan/TSan pass.
- **Bound everything on untrusted or backpressured paths** — caps, drop/coalesce
  policy, and a visible counter at every seam.

## Non-goals (explicit)

- Not a non-linear editor, grading suite, or graphics/CG system beyond overlay
  compositing and clip export.
- Not shipping the vendor hardware itself: DeckLink/AJA cards, Rivermax NICs and
  NIC-PHC timestamping are integration targets behind the existing seams, not
  deliverables of this repo, and their genlock-grade timing is validated on a
  hardware lab bench, not in free CI.
- Not a cloud SaaS; no phone-home telemetry. The control API stays a
  trusted-network appliance surface, hardened but not internet-exposed.
- Not Linux desktop/UI parity in the near term — Linux is a decode/ingest and CI
  target, not a first-class operator platform yet.

## Quality gates

Stable IDs with concrete numbers. Initiatives reference these in their acceptance
criteria; the audit checks every reference resolves.

### QG-LAT-GG — Glass-to-glass latency
Target: arrival->PGM pixels p99 <= 2 frames at 1080p60, published per transport.

### QG-LAT-CTA — Control-to-air latency
Target: take/recall/cut -> on-air pixels p99 <= 2 frames.

### QG-SEEK-ACC — Seek & recall accuracy
Target: 0-frame landing error; every recall returns the identical frame across all camera timelines.

### QG-SYNC-TIER — Inter-camera sync tiering
Target: each source surfaces FrameAccurate (+/-0) or a numeric Bounded +/-ms tier; never an unquantified claim.

### QG-CAD-JIT — Program cadence jitter
Target: <= 0.2 ms RMS and <= 1 ms peak-to-peak over 1 h, GUI-load-independent.

### QG-IMG-DE — Colorimetry accuracy
Target: deltaE2000 <= 1.0 GPU-vs-CPU-reference across all matrix/primaries/transfer combos.

### QG-IMG-TRUNC — Bit-depth integrity
Target: 0 truncation on 10/12-bit sources through decode, composite, encode and mux; 8-bit path byte-identical.

### QG-IMG-VMAF — Objective quality gate
Target: SSIM/VMAF gate is deterministic and catches a > 1 VMAF-point or injected matrix-swap regression 100% of the time.

### QG-AUD-R128 — Loudness compliance
Target: programme within EBU R128 -23 LUFS +/-0.5 LU, true peak <= -1 dBTP; A/V sync within +/-0.5 frame.

### QG-REL-SOAK — 24/7 endurance
Target: multi-day fault-injection soak: 0 dropped/duplicated output frames, 0 content loss under any single fault, MTBF measured in weeks.

### QG-REL-RECOVER — Fault recovery
Target: output device-loss re-arms <= 2 s with 0 operator action; source failover gap <= 1 frame with common TC.

### QG-SEC-CTRL — Control-plane security
Target: default install exposes 0 remotely-reachable control endpoints; 100% of state-changing commands are rejected unauthenticated off loopback.

### QG-CI-CLOSED — Fail-closed gate
Target: a deliberately-broken change-classifier turns the required 'CI gate' RED; 0 vacuously-green merges are possible.

### QG-A11Y — Accessibility
Target: WCAG 2.2 AA: text >= 4.5:1 contrast, all interactive elements keyboard-operable, tally redundantly encoded (not color-only).

### QG-FAC-NMOS — Facility interop
Target: AMWA NMOS IS-04/05/07 testing tool 100% of applicable tests green; ST 2110-21 within the narrow (type N) profile.

## Initiative registry

The single source of truth for what exists. Every row has a matching definition
in a phase file (the audit enforces the bijection). Statuses live in
[`program-state.json`](./program-state.json), not here.

| ID | Phase | Size | Title |
|----|-------|------|-------|
| `arch-dependency-layer-lint` | 1 | M | CI include-graph / layering dependency lint |
| `arch-engine-library` | 1 | L | Extract a single canonical olr_engine static library linked by app + all tests |
| `arch-ffmpeg-firewall` | 1 | M | Firewall libav headers out of UI, websocket AND the full DAG (record path included) |
| `ing-audio-degrade` | 1 | S | Graceful audio-unavailable, keep-video degrade |
| `ing-windows-live-validation` | 1 | M | Prove and CI-gate the Windows live ingest path |
| `perf-latency-budget-harness` | 1 | L | End-to-end latency budget: instrumentation, headless harness, CI gate (incl. interactive scrub/jog metric) |
| `perf-trace-tooling` | 1 | M | Structured Perfetto/Chrome trace + flamegraph tooling |
| `pi-control-api-hardening` | 1 | M | Authenticate (bearer token + Origin allowlist), encrypt (TLS1.3/wss), rate/size/connection-limit, capability-tier and path-sandbox the control API, with a privileged-command audit log |
| `qe-ci-gate-failclosed` | 1 | S | Make the CI gate provably fail-closed (negative self-test), preserving the docs-only skip path |
| `qe-flake-observability` | 1 | S | Flake detection, JUnit ingestion, quarantine |
| `qe-qml-lint-format-full` | 1 | S | Lint and format all 32/33 QML files; enforce qmlformat |
| `qe-windows-ctest-lane` | 1 | M | Promote Windows from build-only to a real unit + native-ingest e2e test lane; wire iOS into CI |
| `rel-output-autorecover` | 1 | M | Output device-loss auto-recovery state machine (closes outputdispatcher.cpp:251) |
| `rel-telemetry-bus` | 1 | M | Structured ops telemetry bus + loopback-only /healthz + /metrics + rotating log |
| `sec-amf0-extract-fuzz` | 1 | M | Extract the AMF0 scanner to a pure module and fuzz it; add AAC/ADTS and control-JSON fuzz targets |
| `sec-control-loopback-default` | 1 | S | Fail-closed default: control API binds loopback unless auth+TLS configured |
| `sec-fuzz-ci-continuous` | 1 | M | Continuous, coverage-tracked fuzzing gated on parser changes |
| `sec-h26x-au-cap` | 1 | S | Bound the pending access-unit buffer AND attacker-controlled dimension/count ceilings |
| `sec-secrets` | 1 | M | Secret hygiene: redaction, keychain storage, scoped snapshot exposure |
| `sec-supply-chain` | 1 | L | Verified, parity dependency builds + SBOM + CVE scanning |
| `sec-threat-model` | 1 | M | Written STRIDE threat model incl. SSRF classification of import/SSE URLs and runtime-NDI load path |
| `tc-ptp-hardening` | 1 | S | Harden the opt-in PTP runtime (torn reads + message matching) |
| `arch-decoder-backend-spi` | 2 | L | Codec-keyed decoder/encoder factory registry (video & audio) |
| `arch-decompose-playbackworker` | 2 | XL | Decompose PlaybackWorker into ResidencyScheduler / ArmedCutEngine / GPU-lifecycle / cache-publish classes, atomics moved verbatim |
| `arch-decompose-uimanager` | 2 | XL | Split UIManager into headless-testable domain controllers |
| `arch-dedup-decoderbank-avcc` | 2 | M | Deduplicate decoder-bank open, avcC usage, sink status bookkeeping |
| `arch-output-sink-abi-and-registry` | 2 | L | Registry-based output-sink SPI + version OutputBusFrame with colorimetry + SMPTE-12M timecode fields |
| `perf-bounded-backpressure-audit` | 2 | M | Bounded backpressure + drop-policy + counter at every ingest->record->playback->output seam, incl. encode-falling-behind ladder |
| `perf-hotpath-scans-and-fences` | 2 | M | Make audioSpanOrSilence & telemetry stateAt O(log n); pool GPU fences |
| `perf-output-cadence-decouple` | 2 | L | Sleep-until-due + TimingReference/PTP phase-lock on the OutputRuntime loop (replace the 1ms msleep poll) |
| `perf-regression-harness-ci` | 2 | M | Per-PR fail-closed perf-regression gate with trend + trace artifacts on a pinned runner; multi-source scaling target |
| `perf-rt-scheduling` | 2 | L | Real-time scheduling class, priority elevation, CPU affinity for the on-air chain (degrade cleanly where denied) |
| `perf-zero-copy-interop` | 2 | XL | Zero-copy GPU-to-sink interop for GPU-capable sinks; one shared readback for CPU sinks |
| `rel-crash-safe-mkv` | 2 | L | Crash-recoverable recording: journal + startup orphan scan + auto-remux repair; dual-target A/B disk write; crash-loop safe-boot |
| `rel-degradation-ladder` | 2 | S | Explicit, surfaced, monotonic-down degradation ladders per subsystem |
| `rel-session-restore` | 2 | M | Durable operator session/state persistence with crash restore-and-resume |
| `rel-storage-guardian` | 2 | M | Storage supervisor: preflight, tiered thresholds, reserved-space safety file, retention reclaim |
| `rel-worker-watchdog` | 2 | M | Liveness watchdog & self-heal supervisor for all critical threads |
| `rop-control-surface-hub` | 2 | M | Unified ControlSurfaceHub + IControlSurface seam (also arch-control-surface-spi) |
| `tc-reference-reselection` | 2 | M | Reference-source re-selection robustness |
| `bvp-color-quality-gate` | 3 | M | Objective color/quality gate + SMPTE bars test source |
| `bvp-color-scopes` | 3 | M | Operator color scopes: waveform, vectorscope, RGB parade, histogram |
| `bvp-colorimetry-convert` | 3 | L | Per-source colorimetry conversion & HDR<->SDR mapping |
| `bvp-decode-highbit` | 3 | L | 10-bit hardware decode (VideoToolbox / MediaFoundation P010) |
| `bvp-gpu-highbit-composite` | 3 | XL | 10/16-bit GPU compositor & readback with correct, transfer-aware YCbCr<->RGB (fixes Bt2020->709 collapse) and chroma siting |
| `bvp-hdr-wcg-metadata` | 3 | M | PQ/HLG transfer, P3/2020 primaries & mastering-display (ST 2086/MaxCLL) metadata model |
| `bvp-highbit-encode-mux` | 3 | L | 10-bit HEVC Main10/P010 record encode + write colorimetry/HDR (Matroska Colour, ST 2086) to the MKV master |
| `bvp-highbit-frame-model` | 3 | L | High-bit-depth / 4:2:2 / 4:4:4 frame model foundation (8-bit path byte-identical) |
| `bvp-hq-scaling` | 3 | M | High-quality polyphase (Lanczos-3) linear-light scaling on CPU-reference and GPU paths |
| `bvp-interlace-fields` | 3 | L | Interlaced field handling & selectable deinterlace policy |
| `bvp-legalizer` | 3 | M | Broadcast-legal levels/gamut legalizer + live illegal-level/gamut metering |
| `bvp-ndi-highbit` | 3 | M | NDI 10-bit / 4:2:2 output with colorimetry metadata |
| `qe-output-quality-oracle` | 3 | L | SSIM + VMAF gate on codec round-trip and the composited NDI output path |
| `tc-dropframe-source` | 3 | S | Drop-frame source-timecode preservation |
| `tc-full-sei` | 3 | M | Full pic_timing / registered-ATC SEI timecode parsing |
| `tc-output-passthrough` | 3 | M | Output-side timecode and timestamp passthrough (finish T2.1) |
| `tc-rational-recorder` | 3 | M | Rational recorder frame rate |
| `pi-audio-mixer-monitor` | 4 | L | Multi-channel audio mixer + monitoring (gain/mute/solo, N->M, 5.1/16-ch, pitch-preserving varispeed) |
| `pi-clip-export-engine` | 4 | XL | Frame-accurate clip export to MP4/MOV/MXF OP1a + ProRes/DNxHR (growing-file safe) |
| `pi-graphics-overlay` | 4 | L | GPU overlay / branding / score-bug / TC burn-in compositing |
| `pi-highlight-package` | 4 | L | Highlight-package builder (stitch + brand + R128-normalize) |
| `pi-loudness-r128` | 4 | L | EBU R128 / BS.1770-4 loudness metering + delivery normalization |
| `pi-mam-delivery` | 4 | L | MAM / playout handoff + editorial interchange (EDL/FCPXML/AAF) + CEA-608/708 captions, with receipts |
| `pi-recall-playlist-ui` | 4 | M | Control-adapter playlist commands + own the recall list's glanceable presentation (residual of shipped T2.2) |
| `qe-ebu-r128-avsync-gate` | 4 | M | EBU R128 loudness + A/V-sync conformance gate |
| `qe-ux-interaction-tests` | 4 | L | Headless operator-flow interaction + cross-platform visual snapshots + accessibility checks |
| `rop-angle-bank-gang` | 4 | M | Synchronized angle bar, gang recall, switch-during-playout, at-a-glance sync badges |
| `rop-cockpit-layout` | 4 | M | Dedicated replay-operator cockpit + operator profiles + on-air full-screen mode |
| `rop-controller-feedback-parity` | 4 | M | Universal controller feedback (MCU LEDs, jog ring, unified push) |
| `rop-crossclip-recall` | 4 | L | Cross-recording & per-angle recall |
| `rop-cue-metadata` | 4 | M | Rich rundown cue metadata (name / color / keyword / rating) |
| `rop-hid-jogshuttle-backend` | 4 | L | USB-HID jog / shuttle / T-bar hardware backend (ShuttlePro + descriptor-driven generic) |
| `rop-keyboard-hotkeys` | 4 | M | First-class keyboard operator model + discoverable on-screen keymap (also uiux-keyboard-command-layer) |
| `rop-preview-program-bus` | 4 | XL | PVW/PGM two-bus operator model with TAKE + transitions |
| `rop-recall-latency-sla` | 4 | M | Instant-recall latency telemetry + e2e SLA gate |
| `rop-supermotion-interp` | 4 | XL | Motion-compensated super slow-motion + field-rate, with blend fallback |
| `uiux-accessibility-wcag` | 4 | M | WCAG 2.2 AA: roles/names, keyboard-operable surfaces, reduced-motion, contrast gate, CVD-safe redundant-encoded tally, touch/iOS 44pt + gesture transport |
| `uiux-alarm-health-center` | 4 | M | Persistent alarm & health center with severity, acknowledge, bounded history |
| `uiux-design-system-tokens` | 4 | M | Complete the design-token foundation (motion, alarm, gallery type, brightness, embedded tabular TC font) |
| `uiux-localization-i18n` | 4 | M | Full localization: qsTr everywhere, RTL mirroring, locale-aware formatting, pseudo-loc gate |
| `uiux-multiviewer-umd-hud` | 4 | L | Broadcast multiviewer: per-tile UMD/tally/meters/confidence/no-signal + layout presets + compositor-overlay layout contract |
| `uiux-operator-error-prevention` | 4 | S | Operator-error prevention: armed-state HUD, confirms, undo, safe guards |
| `uiux-qml-test-lint-hardening` | 4 | M | Raise the UI verification bar: full qmllint + contrast/keyboard/pseudo-loc CI gates |
| `uiux-sync-confidence-surface` | 4 | M | Always-on sync & timing confidence HUD (PTP tier, inter-camera ±ms, ppm, buffer depth) |
| `bvp-st2110-real` | 5 | XL | Real ST 2110-20 (RFC 4175 10-bit 4:2:2) + ANC + VPID + ST 2110-21 pacing |
| `hwio-capture-half` | 5 | XL | SDI / ST 2110 ingest (the capture half) |
| `ing-envelope-broaden` | 5 | M | HE-AAC / AAC-LD / LATM decode |
| `ing-linux-native-decode` | 5 | L | Native H.264/HEVC + AAC decode on Linux |
| `ing-udp-file-restore` | 5 | M | Restore udp:// (multicast MPEG-TS) and file:// ingest |
| `pi-automation-protocols` | 5 | L | Playout/newsroom automation adapters (VDCP + MOS) |
| `pi-decklink-sdi-backend` | 5 | L | Real DeckLink SDI/HDMI output backend (embedded audio + RP188 VANC TC) |
| `pi-nmos-control` | 5 | XL | AMWA NMOS node — IS-04 registration/discovery + IS-05 connection management (+ HTTP + DNS-SD stack) |
| `pi-remi-cloud` | 5 | XL | REMI / remote & cloud production hooks (SRT/RIST program egress, remote control bridge, headless render) |
| `pi-st2110-egress` | 5 | XL | ST 2110-10/-20/-30/-40 network egress with PTP pacing, SDP and SMPTE 2022-7 redundancy |
| `pi-tally-gpi-is07` | 5 | M | Tally / GPI / vision-mixer integration (with IS-07) |
| `qe-nmos-conformance` | 5 | L | AMWA NMOS IS-04/IS-05 conformance in CI |
| `rel-alerting` | 5 | M | Rule-driven alerting & escalation (SNMP / webhook / dry-contact / on-air banner) |
| `rel-hot-standby-recorder` | 5 | XL | Hot-standby / mirrored-recorder redundancy with primary election + planned-maintenance graceful drain |
| `rel-nmos-observability` | 5 | L | NMOS IS-07 event/alarm emission tied to the ops telemetry bus |
| `rel-redundant-source-failover` | 5 | XL | Dual/redundant source feeds with hitless (ST 2022-7-class) failover preserving PTS/timeline continuity |
| `tc-ltc-vitc-anc` | 5 | M | LTC / VITC / ANC (RP 188) timecode in and out |
| `arch-genlock-reference-backend` | 6 | XL | Genlock/NIC-PHC/hardware-capture TimingReference backend swap (behind the shipped seam) |
| `ns6-evidence-harness` | 6 | L | Award-grade measurement, certification & evidence harness (NS-6) + dual-jury submission package |
| `qe-soak-chaos-ci` | 6 | L | Wire the 24/7 soak + fault-injection into scheduled CI with MKV-conformance validation |
| `qe-st2110-ptp-conformance` | 6 | XL | ST 2110-21 + ST 2059 PTP timing conformance harness |
| `qe-two-clock-drift-rig` | 6 | L | Two-clock drift & genlock soak rig |
| `rel-24x7-soak` | 6 | L | 24/7 endurance + fault-injection validation harness with recovery SLAs |

## Phases

Phases are planning/reporting groupings; the **dependency graph** gates when an
initiative may start (all its dependencies shipped), not phase boundaries. One
"advance slot" from the next phase is allowed. Each phase ships at least one
user-visible improvement.

| # | Phase | Initiatives | Plan |
|---|-------|:-----------:|------|
| 1 | [Foundation & Safety](./phase-1-foundation-safety.md) | 22 | [impl](./impl/phase-1-foundation-safety.md) |
| 2 | [Extensible Core & Real-Time Backbone](./phase-2-extensible-core.md) | 18 | [impl](./impl/phase-2-extensible-core.md) |
| 3 | [Pristine Image & Slow-Motion Chain](./phase-3-image-chain.md) | 17 | [impl](./impl/phase-3-image-chain.md) |
| 4 | [LSM-Class Operator Experience & Control-Room UX](./phase-4-operator-ux.md) | 27 | [impl](./impl/phase-4-operator-ux.md) |
| 5 | [Facility Interop & Hardware I/O](./phase-5-facility-interop.md) | 17 | [impl](./impl/phase-5-facility-interop.md) |
| 6 | [Genlock, Certification & Award Polish](./phase-6-genlock-certification.md) | 6 | [impl](./impl/phase-6-genlock-certification.md) |

Implementation plans live under [`impl/`](./impl/); the active phase's plan is
full (task stacks, RED→GREEN, commit boundaries, PR-slice tables for XL/XXL
initiatives), later phases are drafts by construction.

## Governance

- **Execution-gating.** An initiative is ready when all its `Dependencies` are
  `shipped` (see `--frontier`). Phase order is guidance; the graph is the gate.
- **WIP cap = 3**, with **disjoint write scopes** (agents compare Files lists
  before claiming work); at most **one advance slot** pulled from the next phase.
- **Continuous value.** Every phase must ship a user-visible improvement.
- **Re-planning gates.** At each phase exit, the next phase's file is re-audited
  against the live codebase, re-sliced and re-estimated. Later phase files and
  impl plans are drafts by construction.
- **Evidence ladder for human-bound validation:** directional (gates iteration) →
  expert (gates phase exits) → full (gates the final release). Such work is an
  **external evidence gate** with a named owner, never an agent checkbox.
- **Never without a directive:** production deploys/migrations, spending,
  publishing to external services, deleting evidence, or overriding a failed
  safety/security gate. See the [operating loop](./operating-loop.md).

## Human owners (assumption — steer via directives)

Solo-maintainer repo. **Timo Korkalainen** steers by exception through
[`directives.md`](./directives.md) and owns the external evidence gates:
LSM-operator usability studies, real-hardware / interop-lab validation
(DeckLink/ST 2110/genlock), production deploys, and any spending. These
assumptions are seeded as the first directive; correct them there and the loop
adapts. Silence means continue.

## The audit lane

`tools/roadmap/audit.py` (< 1 s, pure stdlib) enforces, each with a test in
`test_audit.py` proving it detects its defect class:

1. **unique-ids** — initiative IDs unique across phase files.
2. **deps-present** — exactly one `Dependencies:` field per initiative.
3. **deps-resolvable** — every dependency names a real initiative.
4. **deps-acyclic** — no dependency cycles.
5. **deps-not-later-phase** — a dependency never points to a later phase.
6. **registry-agreement** — index registry ↔ phase-file definitions agree (id, phase, size).
7. **impl-coverage** — every initiative has an implementation-task heading under `impl/`.
8. **slice-tables** — every XL/XXL initiative has a `PR slices` table (waived only for draft plans).
9. **qg-refs-resolve** — every referenced quality-gate ID is defined here.
10. **links-resolve** — relative links and in-plan anchors resolve.
11. **word-budget** — index ≤ 4000 words, each phase file ≤ 3500 words.
12. **state-consistency** — `program-state.json` keys == registry; valid statuses; `implementing`
    carries a `pr` (draft PR), and `measuring`/`shipped` carry a `pr` **and** an `evidence` link.
13. **deps-completion-order** — no initiative is `implementing`/`measuring`/`shipped` while any
    dependency is not yet `shipped` (you cannot complete work ahead of its dependencies).
14. **size-valid** / **acceptance-present** / **qg-has-number** — schema hygiene.

`--frontier` prints the ready set (all dependencies shipped) as JSON after a clean
audit; `--emit-state` regenerates `program-state.json` from the docs **preserving**
existing status/pr/evidence (it never wipes shipped progress) — never hand-edit the
seed. State transitions go through
[`tools/roadmap/state.py`](../../tools/roadmap/state.py)
(`set <id> --status … --pr … --evidence …`), which rolls back any transition that
would fail the audit. To run the loop, invoke the
[`execute-roadmap`](../../.claude/skills/execute-roadmap/SKILL.md) skill — the
executable, resumable form of the [operating loop](./operating-loop.md).

## Maintenance checklist

- Adding an initiative: add a registry row **and** a phase-file definition (id,
  size, one `Dependencies:` field, definition, acceptance criteria); add an
  `impl/` task heading; run `python tools/roadmap/audit.py --emit-state` (it
  preserves existing progress and adds the new entry as `discovery`); then
  `python tools/roadmap/audit.py`.
- Shipping an initiative: set its `program-state.json` entry to `shipped` with the
  merged `pr` and an `evidence` link; the audit rejects `shipped` without both.
- At a phase exit: re-audit and re-slice the next phase file; flip its impl plan
  from draft to full for the phase becoming active.
- Never hand-edit `program-state.json` statuses to skip the evidence rule; never
  bypass the audit with `--no-verify`.

## Repository facts (verified 2026-07-10)

- Required merge check: the single **"CI gate"** context
  ([`.github/workflows/ci.yml`](../../.github/workflows/ci.yml)) — which includes
  the always-run `docs-audit`; `enforce_admins` on (no admin bypass),
  `allow_force_pushes` off, no required human review, repo auto-merge disabled → the
  loop self-merges through [`tools/roadmap/merge_guard.sh`](../../tools/roadmap/merge_guard.sh)
  (verifies CI-gate-green + an `independent-reviewed` label, then `gh pr merge --merge`).
- Local gates: [`.githooks/pre-commit`](../../.githooks/pre-commit)
  (clang-format + qmllint + diff clang-tidy) and
  [`.githooks/pre-push`](../../.githooks/pre-push) (build + delivery matrix + unit
  + clang-tidy; docs-only fast path; `OLR_PREPUSH_FULL=1` for e2e/sanitizers/iOS).
  Enable once: `git config core.hooksPath .githooks`.
- Docs convention: flat `docs/*.md` roadmaps + `docs/superpowers/{plans,specs}/`.
  Branches `agent/*`, `codex/*`, `fix/*`, `design/*` off `origin/main`; worktrees
  under `.claude/worktrees/`.
- Agent tool permissions for the loop: [`.claude/settings.json`](../../.claude/settings.json).

---

- [Operating loop (runbook)](./operating-loop.md)
- [Directives (steering channel)](./directives.md)
- [Program state](./program-state.json)
- Phases: [P1](./phase-1-foundation-safety.md) · [P2](./phase-2-extensible-core.md) · [P3](./phase-3-image-chain.md) · [P4](./phase-4-operator-ux.md) · [P5](./phase-5-facility-interop.md) · [P6](./phase-6-genlock-certification.md)

_Memory anchor: `broadcast-perfection-plan`. Keep this index and the phase files
in sync via the audit; the audit is the contract._
