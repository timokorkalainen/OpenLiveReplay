# Phase 4 — LSM-Class Operator Experience & Control-Room UX

> Part of the [Broadcast-Perfection Plan](./README.md). The index holds the goal, architecture guardrails, non-goals, the quality gates referenced below, the governance rules and the audit lane. Read it first.

**Duration estimate.** ~6-8 months

## Why this phase, in this order

This is the usability-award engine, and it depends on the ControlSurfaceHub (Phase 2), the recallable-playlist model, and the colorimetry/HQ-scaling multiview (Phase 3)..

## Initiatives

### `pi-audio-mixer-monitor` — Multi-channel audio mixer + monitoring (gain/mute/solo, N->M, 5.1/16-ch, pitch-preserving varispeed)
- **Size:** L
- **Dependencies:** `pi-loudness-r128`
- **Definition:** Replace single-active-view stereo passthrough with a real mixer/monitor stage: per-source gain, mute/solo, N-in->M-out routing, and >2-channel (5.1 / up to 16-ch) mapping.
- **Acceptance criteria:**
  - [ ] tst_audiomixer: routing-matrix output sample-exact; solo/mute correctness; 5.1 passthrough to a 6-channel NDI frame
  - [ ] Unity-gain single-view path is bit-identical to the current stereo output (no regression)

### `pi-clip-export-engine` — Frame-accurate clip export to MP4/MOV/MXF OP1a + ProRes/DNxHR (growing-file safe)
- **Size:** XL
- **Dependencies:** `pi-recall-playlist-ui`
- **Definition:** Export any marked in/out replay range (and multi-entry selections) from the growing recording to MP4/MOV/MXF OP1a and ProRes/DNxHR mezzanine — the single largest media-delivery gap (today only a PNG still exists).
- **Acceptance criteria:**
  - [ ] E2E tests/e2e/run_clip_export_e2e.sh: export a known in/out from a fixture; ffprobe confirms exact frame count and first/last PTS map to the requested TC (+/-0 frames); copied-region PSNR >= 45 dB; MXF OP1a validates via mediainfo/bmx
  - [ ] Exporting a closed range WHILE recording never corrupts the growing MKV (concurrent reader/writer soak)

### `pi-graphics-overlay` — GPU overlay / branding / score-bug / TC burn-in compositing
- **Size:** L
- **Dependencies:** none
- **Definition:** Add a GPU overlay layer on the program/output buses — clock/TC burn-in, station bug, lower-thirds, and a data-driven score bug — so replays go to air branded.
- **Acceptance criteria:**
  - [ ] E2E: enable TC burn-in + bug overlay, capture output, assert overlay pixels present at expected coords and program pixels elsewhere unchanged
  - [ ] A/B identity: overlay OFF is byte-identical to today's output

### `pi-highlight-package` — Highlight-package builder (stitch + brand + R128-normalize)
- **Size:** L
- **Dependencies:** `pi-clip-export-engine`, `pi-graphics-overlay`, `pi-loudness-r128`
- **Definition:** Stitch multiple replay entries into one rendered, branded, R128-normalized package for immediate distribution — the sports-highlights workflow in one action.
- **Acceptance criteria:**
  - [ ] E2E: build a 3-clip package with a dissolve + bug; output is one file with total duration = sum of ranges (+/-0 frames per boundary), audio -23 LUFS +/-0.5, overlay present
  - [ ] Deterministic byte-stable output for identical inputs

### `pi-loudness-r128` — EBU R128 / BS.1770-4 loudness metering + delivery normalization
- **Size:** L
- **Dependencies:** none
- **Definition:** Measure loudness live and normalize on delivery so every exported/streamed clip meets the -23 LUFS / -1 dBTP broadcast contract.
- **Acceptance criteria:**
  - [ ] tst_loudnessmeter against EBU Tech 3341 compliance signals, each within stated tolerance (e.g. -23.0 LUFS +/-0.1 on the reference set)
  - [ ] Export-normalization test drives an off-target clip to -23 +/-0.5 LUFS with TP <= -1 dBTP
  - [ ] meets QG-AUD-R128

### `pi-mam-delivery` — MAM / playout handoff + editorial interchange (EDL/FCPXML/AAF) + CEA-608/708 captions, with receipts
- **Size:** L
- **Dependencies:** `pi-clip-export-engine`, `pi-control-api-hardening`
- **Definition:** Push exported clips plus standards metadata sidecars to watch-folders / FTP / S3 / Aspera with receipts, retry, and atomic visibility.
- **Acceptance criteria:**
  - [ ] E2E: export -> deliver to a local watch-folder + mock S3; assert the file appears atomically with its sidecar; an injected transient failure is retried and eventually delivered; a receipt is recorded
  - [ ] Delivery queue resumes after an app restart

### `pi-recall-playlist-ui` — Control-adapter playlist commands + own the recall list's glanceable presentation (residual of shipped T2.2)
- **Size:** M
- **Dependencies:** `pi-control-api-hardening`
- **Definition:** Give the operator a live, recallable, reorderable rundown of accumulated replay entries instead of the single hardcoded 'Recall 0' button.
- **Acceptance criteria:**
  - [ ] QML test tst_playlist_ui + control e2e: mark 3 entries -> list shows 3 with correct TC labels; recall(2) seeks to entry 2 in-point; reorder + retime persist and JSON round-trips
  - [ ] Model never desyncs from ReplayPlaylist over a 1000 mark/recall/reorder fuzz sequence

### `qe-ebu-r128-avsync-gate` — EBU R128 loudness + A/V-sync conformance gate
- **Size:** M
- **Dependencies:** none
- **Definition:** Add objective audio-conformance evidence: integrated loudness, true-peak, and measured audio-to-video offset against the broadcast tolerances.
- **Acceptance criteria:**
  - [ ] Recorded + output audio measured for R128 integrated loudness / true-peak and gated
  - [ ] A/V sync measured and gated within +/-0.5 frame

### `qe-ux-interaction-tests` — Headless operator-flow interaction + cross-platform visual snapshots + accessibility checks
- **Size:** L
- **Dependencies:** `qe-qml-lint-format-full`
- **Definition:** Regression-test the usability surface: drive the real operator flows headlessly on all three desktop platforms and diff visual snapshots beyond macOS.
- **Acceptance criteria:**
  - [ ] Operator-flow interaction suite runs headless and green on all 3 desktop CI legs
  - [ ] A broken mark-in/recall/arm-cut binding fails CI

### `rop-angle-bank-gang` — Synchronized angle bar, gang recall, switch-during-playout, at-a-glance sync badges
- **Size:** M
- **Dependencies:** `rop-crossclip-recall`
- **Definition:** Turn multi-angle from a feed-select into a broadcast angle bank: hotkeyed on-screen angle bar, gang recall across all angles, angle switching mid-recalled-clip, and at-a-glance sync badges.
- **Acceptance criteria:**
  - [ ] Angle switch during recalled playout lands PGM on the same session frame index across angles (e2e frame-identity across two synchronized fixture angles)
  - [ ] Gang recall produces entries with a common in/out for every enabled angle (unit test)

### `rop-cockpit-layout` — Dedicated replay-operator cockpit + operator profiles + on-air full-screen mode
- **Size:** M
- **Dependencies:** `rop-preview-program-bus`, `rop-angle-bank-gang`
- **Definition:** Ship a tuned single-screen cockpit (angle bar + PVW/PGM + rundown + transport + T-bar readout) and per-operator profiles (layout + all bindings) with a full-screen on-air mode.
- **Acceptance criteria:**
  - [ ] Cockpit lays out without overlap at target operator resolutions (qmlstyle layout test)
  - [ ] Switching operator profile restores layout + all bindings (round-trip test)

### `rop-controller-feedback-parity` — Universal controller feedback (MCU LEDs, jog ring, unified push)
- **Size:** M
- **Dependencies:** `rop-control-surface-hub`
- **Definition:** Close the muscle-memory loop on any panel: generic MIDI/MCU transport-lamp + jog-LED-ring feedback and a single feedback path so every connected surface reflects TC/speed/rec/armed state.
- **Acceptance criteria:**
  - [ ] A generic MCU controller lights play/rec/armed lamps correctly (unit test over a faked MIDI-out sink capturing the emitted bytes)
  - [ ] Feedback push stays <=10 Hz and always lands the final state after pause/step (test)

### `rop-crossclip-recall` — Cross-recording & per-angle recall
- **Size:** L
- **Dependencies:** none
- **Definition:** Let a rundown cue reference any recording and the angle it was marked on, and recall it frame-accurately even when it is not the open clip.
- **Acceptance criteria:**
  - [ ] Recalling a cue on a different recording lands the correct in-point with no flash (e2e assertion on PGM frame identity)
  - [ ] JSON round-trips the new fields byte-exact (unit test)

### `rop-cue-metadata` — Rich rundown cue metadata (name / color / keyword / rating)
- **Size:** M
- **Dependencies:** none
- **Definition:** Let operators label, color, and flag cues for instant visual recall under time pressure, elevating the shipped rundown to broadcast usability.
- **Acceptance criteria:**
  - [ ] Metadata round-trips through JSON byte-exact (unit test)
  - [ ] RundownRail shows/edits name+color and filters by tag (qmlstyle test)

### `rop-hid-jogshuttle-backend` — USB-HID jog / shuttle / T-bar hardware backend (ShuttlePro + descriptor-driven generic)
- **Size:** L
- **Dependencies:** `rop-control-surface-hub`
- **Definition:** Add a generic USB-HID control backend (Contour ShuttlePro v2/Xpress + a descriptor-driven generic profile) with weighted jog -> frame step, spring-return shuttle -> shuttle ladder, and an absolute T-bar axis -> variable ...
- **Acceptance criteria:**
  - [ ] ShuttlePro jog steps exactly 1 frame per detent; shuttle ring maps to the ladder; T-bar sweeps 0->1x slow-mo (manual device smoke documented, like the Windows AAC smoke)
  - [ ] Hot-unplug/replug reconnects with no crash (unit test over a faked HID event source through the hub)

### `rop-keyboard-hotkeys` — First-class keyboard operator model + discoverable on-screen keymap (also uiux-keyboard-command-layer)
- **Size:** M
- **Dependencies:** `rop-control-surface-hub`
- **Definition:** Make the keyboard a learnable, persisted control surface with the standard replay muscle-memory set and a discoverable overlay.
- **Acceptance criteria:**
  - [ ] A qmlstyle test drives J-K-L / I / O / number-angle / R and asserts the corresponding hub actions fire
  - [ ] Rebinding a key persists across restart (round-trip test)

### `rop-preview-program-bus` — PVW/PGM two-bus operator model with TAKE + transitions
- **Size:** XL
- **Dependencies:** `rop-crossclip-recall`
- **Definition:** Introduce a preview bus so the operator cues/trims off-air and TAKEs to program with a selectable transition, matching production-switcher convention.
- **Acceptance criteria:**
  - [ ] Preview cue does not alter PGM until take() (e2e: PGM unchanged while preview armed)
  - [ ] TAKE lands on program deterministically within 1 frame; dissolve produces the requested frame count (e2e)

### `rop-recall-latency-sla` — Instant-recall latency telemetry + e2e SLA gate
- **Size:** M
- **Dependencies:** `rop-crossclip-recall`
- **Definition:** Measure and gate the operator-critical recall-arm-to-PGM and take-to-air latencies so 'instant recall' is defensible and regression-proof.
- **Acceptance criteria:**
  - [ ] e2e reports p50 <= 1 frame and p99 <= 2 frames recall-arm-to-PGM and fails if exceeded
  - [ ] Take-to-air latency measured and gated (with rop-preview-program-bus)
  - [ ] meets QG-SEEK-ACC

### `rop-supermotion-interp` — Motion-compensated super slow-motion + field-rate, with blend fallback
- **Size:** XL
- **Dependencies:** `rop-control-surface-hub`
- **Definition:** Replace frame-repeat slow-mo with optical-flow-interpolated intermediate frames for smooth 2x-8x slow, plus field-based slow-mo for interlaced sources.
- **Acceptance criteria:**
  - [ ] 8x slow sustains output cadence with 0 dropped output frames on the target GPU (e2e counter check)
  - [ ] Interpolated output measurably smoother than frame-repeat (objective metric vs baseline)

### `uiux-accessibility-wcag` — WCAG 2.2 AA: roles/names, keyboard-operable surfaces, reduced-motion, contrast gate, CVD-safe redundant-encoded tally, touch/iOS 44pt + gesture transport
- **Size:** M
- **Dependencies:** `uiux-design-system-tokens`
- **Definition:** Make the entire UI screen-reader- and keyboard-accessible and provably AA-contrast compliant.
- **Acceptance criteria:**
  - [ ] CI gate: the contrast test passes for all text/graphic token pairs and fails on a regression.
  - [ ] A Tab-order test reaches every interactive control; a documented NVDA/VoiceOver manual pass exists.
  - [ ] meets QG-A11Y

### `uiux-alarm-health-center` — Persistent alarm & health center with severity, acknowledge, bounded history
- **Size:** M
- **Dependencies:** `uiux-design-system-tokens`
- **Definition:** Give the operator a control-room-grade fault surface that never lets a critical event pass unnoticed.
- **Acceptance criteria:**
  - [ ] A test drives a simulated source loss + muxer write error and asserts an alarm row appears, blinks at the standardized cadence, and clears only on acknowledge.
  - [ ] History is a bounded ring buffer (no unbounded growth) verified over a long-run test.

### `uiux-design-system-tokens` — Complete the design-token foundation (motion, alarm, gallery type, brightness, embedded tabular TC font)
- **Size:** M
- **Dependencies:** none
- **Definition:** Turn Theme.qml from a color/spacing set into a full control-room design system that every later initiative builds on, and eliminate token leaks.
- **Acceptance criteria:**
  - [ ] A new tests/qmlstyle no-raw-literal test asserts zero raw hex and zero raw font.family across all QML_FILES (fails on regression).
  - [ ] Theme exposes blinkHz, brightness multiplier, and reducedMotion; changing brightness repaints within one frame.

### `uiux-localization-i18n` — Full localization: qsTr everywhere, RTL mirroring, locale-aware formatting, pseudo-loc gate
- **Size:** M
- **Dependencies:** `uiux-design-system-tokens`, `uiux-multiviewer-umd-hud`, `uiux-alarm-health-center`
- **Definition:** Make the UI 100% translatable and internationally deployable.
- **Acceptance criteria:**
  - [ ] A CI pseudo-localization run boots the app oracle with expanded strings and asserts zero clipping/overflow.
  - [ ] A lint (lupdate report) shows 0 untranslated user-facing literals remaining.

### `uiux-multiviewer-umd-hud` — Broadcast multiviewer: per-tile UMD/tally/meters/confidence/no-signal + layout presets + compositor-overlay layout contract
- **Size:** L
- **Dependencies:** `uiux-design-system-tokens`
- **Definition:** Replace the debug-grid multiviewer with a broadcast-grade multiviewer that reads at gallery distance.
- **Acceptance criteria:**
  - [ ] tests/qmlstyle/tst_pgmstage_mapping.qml extended to assert tally/health/confidence/timecode bindings per tile.
  - [ ] The app oracle reports zero QML errors rendering a 16-tile multiview with full HUD.

### `uiux-operator-error-prevention` — Operator-error prevention: armed-state HUD, confirms, undo, safe guards
- **Size:** S
- **Dependencies:** `uiux-alarm-health-center`
- **Definition:** Apply ISO 9241 error-tolerance so no destructive action is a single unguarded click.
- **Acceptance criteria:**
  - [ ] A test asserts destructive actions require one confirm or offer undo, and that the armed state is always visible when armed.
  - [ ] Undo restores the prior mark-in/out state.

### `uiux-qml-test-lint-hardening` — Raise the UI verification bar: full qmllint + contrast/keyboard/pseudo-loc CI gates
- **Size:** M
- **Dependencies:** `uiux-design-system-tokens`, `uiux-multiviewer-umd-hud`, `uiux-sync-confidence-surface`, `uiux-alarm-health-center`, `uiux-accessibility-wcag`, `uiux-localization-i18n`
- **Definition:** Lock the whole dimension behind automated gates so award-level quality cannot regress.
- **Acceptance criteria:**
  - [ ] CI runs all four new gates green; qmllint covers 32/32 QML files.
  - [ ] A deliberate raw-hex or missing-accessible-name regression fails CI.

### `uiux-sync-confidence-surface` — Always-on sync & timing confidence HUD (PTP tier, inter-camera ±ms, ppm, buffer depth)
- **Size:** M
- **Dependencies:** `uiux-design-system-tokens`
- **Definition:** Surface the shipped frame-sync telemetry (PTP tier, inter-camera phase, confidence tiers, buffer depth) as a persistent, glanceable panel instead of hover text.
- **Acceptance criteria:**
  - [ ] A test asserts the reference pill reflects referenceTierChanged and a confidence chip reflects a simulated tier transition with no hover.
  - [ ] No new polling timers introduced - the surface is event-driven off existing sourceStats/referenceTier signals.
  - [ ] meets QG-SYNC-TIER

---

[« Phase 3](./phase-3-image-chain.md) · [Index](./README.md) · [Implementation plan](./impl/phase-4-operator-ux.md) · [Phase 5 »](./phase-5-facility-interop.md)
