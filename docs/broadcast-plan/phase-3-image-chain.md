# Phase 3 — Pristine Image & Slow-Motion Chain

> Part of the [Broadcast-Perfection Plan](./README.md). The index holds the goal, architecture guardrails, non-goals, the quality gates referenced below, the governance rules and the audit lane. Read it first.

**Duration estimate.** ~4-5 months

## Why this phase, in this order

The image chain is hard-locked to 8-bit 4:2:0 SDR end-to-end — a hard blocker for any technical-excellence claim and the prerequisite pixel-format/colorimetry pipeline for real SDI/ST 2110 in Phase 5..

## Initiatives

### `bvp-color-quality-gate` — Objective color/quality gate + SMPTE bars test source
- **Size:** M
- **Dependencies:** `bvp-colorimetry-convert`, `bvp-hq-scaling`
- **Definition:** Prove and protect color and scaling correctness with objective metrics and a standard test source, closing T3.4 for this dimension.
- **Acceptance criteria:**
  - [ ] CI fails if multiview delta-E2000 > 1.0 vs reference, Lanczos PSNR ordering regresses, or bars round-trip drifts
  - [ ] A documented dashboard reports PSNR/SSIM/delta-E per build
  - [ ] meets QG-IMG-VMAF

### `bvp-color-scopes` — Operator color scopes: waveform, vectorscope, RGB parade, histogram
- **Size:** M
- **Dependencies:** none
- **Definition:** Operator waveform / vectorscope / RGB-parade / histogram scopes over the composited output.
- **Acceptance criteria:**
  - [ ] waveform, vectorscope, RGB parade and histogram render live from the composited frame

### `bvp-colorimetry-convert` — Per-source colorimetry conversion & HDR<->SDR mapping
- **Size:** L
- **Dependencies:** `bvp-gpu-highbit-composite`
- **Definition:** Convert each source into a single working/output colorimetry so mixed-colorimetry multiview is correct and HDR/SDR can be mixed to a chosen programme space.
- **Acceptance criteria:**
  - [ ] A multiview mixing a 601 and a 709 test source composites each tile with delta-E2000 <= 1.0 vs a per-source reference conversion
  - [ ] An HDR (PQ) tile down-converted to a Rec.709 programme matches a BT.2408 reference within delta-E2000 <= 3.0

### `bvp-decode-highbit` — 10-bit hardware decode (VideoToolbox / MediaFoundation P010)
- **Size:** L
- **Dependencies:** `bvp-highbit-frame-model`
- **Definition:** Decode 10-bit HEVC Main10 sources natively to a 10-bit surface instead of truncating to 8-bit NV12.
- **Acceptance criteria:**
  - [ ] An e2e that ingests a 10-bit HEVC fixture and asserts the decoded FrameHandle reports bitDepth==10 and non-truncated luma histogram (values beyond 8-bit quantization present)
  - [ ] 8-bit sources still decode to the existing 8-bit format (regression gate)

### `bvp-gpu-highbit-composite` — 10/16-bit GPU compositor & readback with correct, transfer-aware YCbCr<->RGB (fixes Bt2020->709 collapse) and chroma siting
- **Size:** XL
- **Dependencies:** `bvp-highbit-frame-model`, `bvp-hdr-wcg-metadata`, `bvp-decode-highbit`
- **Definition:** Composite and read back in 10/16-bit precision and do YCbCr<->RGB in a colorimetrically correct, transfer-aware way.
- **Acceptance criteria:**
  - [ ] tst_gpucompositor gains a Rec.2020 case proving the 2020 matrix (not 709) is used (delta-E vs reference < 1)
  - [ ] A 10-bit checker composites and reads back with < 1 LSB error at 10-bit
  - [ ] meets QG-IMG-DE

### `bvp-hdr-wcg-metadata` — PQ/HLG transfer, P3/2020 primaries & mastering-display (ST 2086/MaxCLL) metadata model
- **Size:** M
- **Dependencies:** none
- **Definition:** Represent PQ/HLG transfer, P3/2020 primaries, and HDR volume metadata so HDR/WCG can be signalled and rendered correctly.
- **Acceptance criteria:**
  - [ ] tst_colormetadatapolicy asserts AV transfer 16->PQ, 18->HLG, primaries 12->P3
  - [ ] A new tst_hdrsei parses a synthetic ST 2086 + MaxCLL SEI blob to the expected luminance/primaries values

### `bvp-highbit-encode-mux` — 10-bit HEVC Main10/P010 record encode + write colorimetry/HDR (Matroska Colour, ST 2086) to the MKV master
- **Size:** L
- **Dependencies:** none
- **Definition:** 10-bit HEVC Main10/P010 record encode that writes colorimetry/HDR metadata into the MKV master.
- **Acceptance criteria:**
  - [ ] 10/12-bit source records without truncation and the MKV carries correct Colour + ST 2086 metadata

### `bvp-highbit-frame-model` — High-bit-depth / 4:2:2 / 4:4:4 frame model foundation (8-bit path byte-identical)
- **Size:** L
- **Dependencies:** none
- **Definition:** Extend the core pixel/plane abstractions so a frame can carry 10/12/16-bit samples and 4:2:0/4:2:2/4:4:4 chroma without truncation, keeping the 8-bit path byte-identical.
- **Acceptance criteria:**
  - [ ] New unit tests prove planeShape/stride/bytes correct for P010, 422p10, 444p10 at odd dimensions
  - [ ] An 8-bit Yuv420p frame through the extended path is byte-identical to today (golden test)
  - [ ] meets QG-IMG-TRUNC

### `bvp-hq-scaling` — High-quality polyphase (Lanczos-3) linear-light scaling on CPU-reference and GPU paths
- **Size:** M
- **Dependencies:** `bvp-highbit-frame-model`, `bvp-gpu-highbit-composite`
- **Definition:** Eliminate nearest-neighbour scaling and add a Lanczos-3 option, done in linear light, on both CPU-reference and GPU paths.
- **Acceptance criteria:**
  - [ ] tst_yuv420pcompositor / tst_gpucompositor assert Lanczos output PSNR > bilinear > nearest against a high-res ground truth on a downscale (measurable ordering)
  - [ ] Zebra/frequency-sweep test shows reduced aliasing energy vs nearest (FFT metric)

### `bvp-interlace-fields` — Interlaced field handling & selectable deinterlace policy
- **Size:** L
- **Dependencies:** `bvp-highbit-frame-model`
- **Definition:** Handle interlaced sources field-accurately with a selectable deinterlace policy instead of silent frame-blending.
- **Acceptance criteria:**
  - [ ] tst_deinterlace: a synthetic 1080i field pattern reconstructs full vertical resolution (weave on static, bob on motion) with measurable resolution/aliasing metrics
  - [ ] Field-rate slow-mo emits 2x the fields as distinct frames

### `bvp-legalizer` — Broadcast-legal levels/gamut legalizer + live illegal-level/gamut metering
- **Size:** M
- **Dependencies:** `bvp-colorimetry-convert`
- **Definition:** Guarantee EBU R103-compliant output levels and gamut and surface live illegal-level/illegal-gamut metering to the operator.
- **Acceptance criteria:**
  - [ ] tst_legalizer: a super-white/RGB-illegal test frame is brought within EBU R103 limits (0 illegal pixels after hard mode)
  - [ ] The meter reports the correct illegal-pixel percentage on a known bars+illegal fixture

### `bvp-ndi-highbit` — NDI 10-bit / 4:2:2 output with colorimetry metadata
- **Size:** M
- **Dependencies:** `bvp-highbit-frame-model`, `bvp-decode-highbit`
- **Definition:** Send higher-fidelity NDI (10-bit / 4:2:2 where the SDK supports it) and carry colorimetry in NDI frame metadata.
- **Acceptance criteria:**
  - [ ] tst_ndisink asserts the correct FourCC and stride for a 10-bit/4:2:2 frame and correct colorimetry XML
  - [ ] 8-bit path remains byte-identical (regression)

### `qe-output-quality-oracle` — SSIM + VMAF gate on codec round-trip and the composited NDI output path
- **Size:** L
- **Dependencies:** none
- **Definition:** Extend objective quality from PSNR-only to SSIM/VMAF, and cover the composited OUTPUT (multiview scaler + color conversion + NDI), not just the codec.
- **Acceptance criteria:**
  - [ ] Round-trip SSIM >=0.98 and VMAF >=95 gated (in addition to PSNR)
  - [ ] Output-path oracle asserts VMAF >=98 / SSIM >=0.99 vs golden reference

### `tc-dropframe-source` — Drop-frame source-timecode preservation
- **Size:** S
- **Dependencies:** `tc-full-sei`
- **Definition:** Carry the drop-frame flag from source TC through alignment and the muxer tmcd tag (recovered as non-drop today).
- **Acceptance criteria:**
  - [ ] a drop-frame source TC round-trips as drop-frame in the recorded MKV tmcd

### `tc-full-sei` — Full pic_timing / registered-ATC SEI timecode parsing
- **Size:** M
- **Dependencies:** none
- **Definition:** Parse the full H.264/HEVC pic_timing clock_timestamp / registered-ATC SEI instead of the raw packed word.
- **Acceptance criteria:**
  - [ ] a corpus of real pic_timing / ATC SEIs decodes to the correct TC
  - [ ] TC is exact at 23.976 / 25 / 50 / 59.94

### `tc-output-passthrough` — Output-side timecode and timestamp passthrough (finish T2.1)
- **Size:** M
- **Dependencies:** none
- **Definition:** Assign real programme timecode and transport timestamp on the output instead of the hardcoded synthesize value.
- **Acceptance criteria:**
  - [ ] an NDI receiver reads back the injected programme TC frame-exact
  - [ ] output TC equals session TC across a recording (e2e)

### `tc-rational-recorder` — Rational recorder frame rate
- **Size:** M
- **Dependencies:** none
- **Definition:** Record on num/den through ReplayManager/StreamWorker instead of the rounded integer compatibility fps.
- **Acceptance criteria:**
  - [ ] a 29.97 source recorded at 29.97 asserts near-zero drift (was report-only)

---

[« Phase 2](./phase-2-extensible-core.md) · [Index](./README.md) · [Implementation plan](./impl/phase-3-image-chain.md) · [Phase 4 »](./phase-4-operator-ux.md)
