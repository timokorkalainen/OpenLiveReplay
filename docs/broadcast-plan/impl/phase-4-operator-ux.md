# Phase 4 — Implementation plan

<!-- draft -->

> **Draft by construction.** This plan is re-audited, re-sliced and re-estimated against the live codebase at the Phase 3 exit re-plan (see the [operating loop](../operating-loop.md)). Until then it is a stub: one heading per initiative so coverage holds, with slices deferred.

### `pi-audio-mixer-monitor` — Multi-channel audio mixer + monitoring (gain/mute/solo, N->M, 5.1/16-ch, pitch-preserving varispeed)
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `pi-clip-export-engine` — Frame-accurate clip export to MP4/MOV/MXF OP1a + ProRes/DNxHR (growing-file safe)
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `pi-graphics-overlay` — GPU overlay / branding / score-bug / TC burn-in compositing
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `pi-highlight-package` — Highlight-package builder (stitch + brand + R128-normalize)
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `pi-loudness-r128` — EBU R128 / BS.1770-4 loudness metering + delivery normalization
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `pi-mam-delivery` — MAM / playout handoff + editorial interchange (EDL/FCPXML/AAF) + CEA-608/708 captions, with receipts
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `pi-recall-playlist-ui` — Control-adapter playlist commands + own the recall list's glanceable presentation (residual of shipped T2.2)
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `qe-ebu-r128-avsync-gate` — EBU R128 loudness + A/V-sync conformance gate
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `qe-ux-interaction-tests` — Headless operator-flow interaction + cross-platform visual snapshots + accessibility checks
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `rop-angle-bank-gang` — Synchronized angle bar, gang recall, switch-during-playout, at-a-glance sync badges
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `rop-cockpit-layout` — Dedicated replay-operator cockpit + operator profiles + on-air full-screen mode
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `rop-controller-feedback-parity` — Universal controller feedback (MCU LEDs, jog ring, unified push)
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `rop-crossclip-recall` — Cross-recording & per-angle recall
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `rop-cue-metadata` — Rich rundown cue metadata (name / color / keyword / rating)
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `rop-hid-jogshuttle-backend` — USB-HID jog / shuttle / T-bar hardware backend (ShuttlePro + descriptor-driven generic)
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `rop-keyboard-hotkeys` — First-class keyboard operator model + discoverable on-screen keymap (also uiux-keyboard-command-layer)
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `rop-preview-program-bus` — PVW/PGM two-bus operator model with TAKE + transitions
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `rop-recall-latency-sla` — Instant-recall latency telemetry + e2e SLA gate
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `rop-supermotion-interp` — Motion-compensated super slow-motion + field-rate, with blend fallback
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `uiux-accessibility-wcag` — WCAG 2.2 AA: roles/names, keyboard-operable surfaces, reduced-motion, contrast gate, CVD-safe redundant-encoded tally, touch/iOS 44pt + gesture transport
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `uiux-alarm-health-center` — Persistent alarm & health center with severity, acknowledge, bounded history
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `uiux-design-system-tokens` — Complete the design-token foundation (motion, alarm, gallery type, brightness, embedded tabular TC font)
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `uiux-localization-i18n` — Full localization: qsTr everywhere, RTL mirroring, locale-aware formatting, pseudo-loc gate
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `uiux-multiviewer-umd-hud` — Broadcast multiviewer: per-tile UMD/tally/meters/confidence/no-signal + layout presets + compositor-overlay layout contract
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `uiux-operator-error-prevention` — Operator-error prevention: armed-state HUD, confirms, undo, safe guards
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `uiux-qml-test-lint-hardening` — Raise the UI verification bar: full qmllint + contrast/keyboard/pseudo-loc CI gates
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

### `uiux-sync-confidence-surface` — Always-on sync & timing confidence HUD (PTP tier, inter-camera ±ms, ppm, buffer depth)
_Draft — task stack and PR slices to be authored at the Phase 3 exit re-plan._

---

[Phase 4 initiatives](../phase-4-operator-ux.md) · [Index](../README.md)
