# Phase 3 — Implementation plan

<!-- draft -->

> **Draft by construction.** This plan is re-audited, re-sliced and re-estimated against the live codebase at the Phase 2 exit re-plan (see the [operating loop](../operating-loop.md)). Until then it is a stub: one heading per initiative so coverage holds, with slices deferred.

### `bvp-color-quality-gate` — Objective color/quality gate + SMPTE bars test source
_Draft — task stack and PR slices to be authored at the Phase 2 exit re-plan._

### `bvp-color-scopes` — Operator color scopes: waveform, vectorscope, RGB parade, histogram
_Draft — task stack and PR slices to be authored at the Phase 2 exit re-plan._

### `bvp-colorimetry-convert` — Per-source colorimetry conversion & HDR<->SDR mapping
_Draft — task stack and PR slices to be authored at the Phase 2 exit re-plan._

### `bvp-decode-highbit` — 10-bit hardware decode (VideoToolbox / MediaFoundation P010)
_Draft — task stack and PR slices to be authored at the Phase 2 exit re-plan._

### `bvp-gpu-highbit-composite` — 10/16-bit GPU compositor & readback with correct, transfer-aware YCbCr<->RGB (fixes Bt2020->709 collapse) and chroma siting
_Draft — task stack and PR slices to be authored at the Phase 2 exit re-plan._

### `bvp-hdr-wcg-metadata` — PQ/HLG transfer, P3/2020 primaries & mastering-display (ST 2086/MaxCLL) metadata model
_Draft — task stack and PR slices to be authored at the Phase 2 exit re-plan._

### `bvp-highbit-encode-mux` — 10-bit HEVC Main10/P010 record encode + write colorimetry/HDR (Matroska Colour, ST 2086) to the MKV master
_Draft — task stack and PR slices to be authored at the Phase 2 exit re-plan._

### `bvp-highbit-frame-model` — High-bit-depth / 4:2:2 / 4:4:4 frame model foundation (8-bit path byte-identical)
_Draft — task stack and PR slices to be authored at the Phase 2 exit re-plan._

### `bvp-hq-scaling` — High-quality polyphase (Lanczos-3) linear-light scaling on CPU-reference and GPU paths
_Draft — task stack and PR slices to be authored at the Phase 2 exit re-plan._

### `bvp-interlace-fields` — Interlaced field handling & selectable deinterlace policy
_Draft — task stack and PR slices to be authored at the Phase 2 exit re-plan._

### `bvp-legalizer` — Broadcast-legal levels/gamut legalizer + live illegal-level/gamut metering
_Draft — task stack and PR slices to be authored at the Phase 2 exit re-plan._

### `bvp-ndi-highbit` — NDI 10-bit / 4:2:2 output with colorimetry metadata
_Draft — task stack and PR slices to be authored at the Phase 2 exit re-plan._

### `qe-output-quality-oracle` — SSIM + VMAF gate on codec round-trip and the composited NDI output path
_Draft — task stack and PR slices to be authored at the Phase 2 exit re-plan._

### `tc-dropframe-source` — Drop-frame source-timecode preservation
_Draft — task stack and PR slices to be authored at the Phase 2 exit re-plan._

### `tc-full-sei` — Full pic_timing / registered-ATC SEI timecode parsing
_Draft — task stack and PR slices to be authored at the Phase 2 exit re-plan._

### `tc-output-passthrough` — Output-side timecode and timestamp passthrough (finish T2.1)
_Draft — task stack and PR slices to be authored at the Phase 2 exit re-plan._

### `tc-rational-recorder` — Rational recorder frame rate
_Draft — task stack and PR slices to be authored at the Phase 2 exit re-plan._

---

[Phase 3 initiatives](../phase-3-image-chain.md) · [Index](../README.md)
