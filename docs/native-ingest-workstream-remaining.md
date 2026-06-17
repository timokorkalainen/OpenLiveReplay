# Native Ingest & Frame-Sync Workstream — Remaining Work

_Status as of 2026-06-17. This is the running inventory of everything still open in the
native-ingest / broadcast-frame-sync workstream after the merges below._

---

## 0. Snapshot — what just shipped (all merged to `main` 2026-06-17)

| PR | What it landed |
|----|----------------|
| #49 | **P0** — decouple the recording heartbeat from fps (fixed 8 ms scheduler + a pure, unit-tested `heartbeatFrameSpan` seam). |
| #50 | **P1** — rational recording frame rate (`int fps` → `FrameRate {num,den}` end-to-end; 29.97/59.94 record correctly; settings/UI/harness fractional-rate aware). |
| #51 | **Native RTMP parity** — backend-tagged `IngestStats`/`SourceHealth` (`srtHealth` + new `rtmpHealth`); RTMP emits stats; single shared A/V anchor (audio follows video). |
| #52 | **Native-only ingest** — removed `FfmpegIngestSession` + the native→ffmpeg fallback; selector is native-only (`srt`→NativeSrt, `rtmp/rtmps`→NativeRtmp, else→Unsupported); SRT native by default; **Windows Media Foundation AAC decoder** + **native RTMP enabled on Windows**; udp:// e2e migrated to the native SRT bridge. |
| #53 | **Soak matrix** — `run_soak_matrix.sh` ({SRT,RTMP}×{H264,H265}) + an `OLR_SRT_SOAK_CODEC` switch. Full 4×30-min run passed (no crash/hang/stall). |
| #54 | **Broadcast output bus foundation** — preview/PGM/multiview output buses with Qt preview and NDI output dispatch hooks. |

**Net state of the engine now:** ingest is **native-only** — native SRT and native RTMP, decoding **H.264 + H.265 + AAC-LC**, at **rational frame rates**, with a backend-tagged health dot. FFmpeg remains linked **only** for the Matroska muxer + the MPEG-2 record encoder.

**Parallel / adjacent:** #54 is the first move on the **output** side (see §7, P5). It is not part of the ingest stack, but it is part of the same broadcast arc.

---

## 1. Immediate — verification before relying on the merge

1. **Live Windows AAC decode smoke-test (only truly unverified item).** The Media Foundation AAC decoder (`nativeaacdecoder_mediafoundation.cpp`) and native RTMP on Windows are `#ifdef _WIN32` — they were **compile-checked locally only via review**, never run. Needed on a real Windows box:
   - The MS AAC Decoder MFT accepts the `MF_MT_USER_DATA`/AudioSpecificConfig blob (no `MF_E_INVALIDMEDIATYPE`).
   - Audio plays at correct pitch/speed for 44.1 kHz and 48 kHz sources (validates the resampler + negotiated-rate handling).
   - A mono AAC source → both stereo channels present.
   - On an older Windows SKU without the AAC MFT, the "AAC decoder unavailable" path degrades gracefully (audio dropped, video continues).
2. **Watch the Windows CI build on `main`** (`build.yml`, `windows-latest`, MinGW) — it now compiles the MF AAC decoder + native RTMP session. Confirm green; a missing include/link in `wmcodecdsp.h`/`mfuuid` would surface here.
3. **Confirm `main` CI green post-merge** (lint, Build+Test macOS, sanitizers).

---

## 2. Windows hardening follow-ups (from the MF AAC code review)

- **Per-frame MFT teardown under load.** The RTMP path calls `decoder.reset()` per audio frame; the MF decoder now does a **flush-only** reset (keeps the MFT, rebuilds only on a config change), but **watch CPU under sustained RTMP load** on Windows — if high, profile the MFT reuse path.
- **`MF_E_TRANSFORM_STREAM_CHANGE` path** is hardened (re-asserts the explicit PCM type, bounded — no infinite loop, re-keys the resampler from the negotiated output type) but has **never executed on a real device**. Exercise it with a source that renegotiates mid-stream.
- **No Windows test runner in CI.** Windows is build-only today. Consider a `windows-latest` ctest job running at least `tst_nativeaacdecoder` + a Windows native-ingest e2e, so the MF paths get automated coverage rather than manual smoke-tests.

---

## 3. Capability regressions from removing the ffmpeg ingest path

Removing `FfmpegIngestSession` (the universal catch-all) intentionally narrowed what the app can ingest. Each of these **used to work via ffmpeg** and now hits the "unsupported" path. They are tracked here so the loss is deliberate, not forgotten — restore natively if/when a real source needs them.

| Dropped capability | Why it fails now | Restore path |
|--------------------|------------------|--------------|
| **Encrypted SRT** (`passphrase`/`pbkeylen`) | `NativeSrtIngestSession::supportsUrl` rejects encrypted URLs | add `SRTO_PASSPHRASE`/`SRTO_PBKEYLEN` to the native SRT socket setup |
| **Hostname / DNS SRT** (`srt://host.example:port`) | native SRT requires a **numeric IPv4** caller host | add DNS resolution (`getaddrinfo`) + IPv6 in `nativeSrtMakeIpv4Sockaddr` |
| **Listener / rendezvous SRT** | native SRT is **caller-only** | add listener/rendezvous modes |
| **`udp://` MPEG-TS ingest** | no native UDP backend | a native UDP+MPEG-TS reader (the `MpegTsParser` already exists; only the UDP socket is missing) — or keep as test-only via the SRT bridge |
| **`file://` ingest** | no native file reader | a native file/demux reader (low priority; tests use the SRT bridge) |

The unsupported-scheme diagnostic was made **scheme-aware** in #52 (it distinguishes "unknown scheme" from "native backend can't take this srt:// URL — hostname/encryption/mode"), so operators get an accurate message; this section is about actually re-adding the capabilities.

---

## 4. Codec / profile envelope (native-only constraints)

The native decoders define a hard envelope — and there is **no ffmpeg fallback** anymore, so anything outside it makes that source fail (logged, source unhealthy) rather than silently degrading.

- **Video:** H.264 + HEVC only. No AV1/VP9 (enhanced-RTMP `av01`/`vp09` → Unsupported), no exotic H.264 profiles VideoToolbox/MF can't open.
- **Audio:** **AAC-LC only.** No HE-AAC/AAC-LD, no non-AAC RTMP audio formats, no MPEG-TS AAC-LATM decode on the audio side (parser recognizes it; decoder is AAC-LC).
- **Windows HEVC** needs the optional OS "HEVC Video Extensions"; absent it, `caps.hevc=false` and HEVC sources fail on Windows.
- **Linux / other platforms:** native video + AAC decode are **stubs** — i.e. no decode at all. Linux is "planned for a later PR"; today a Linux build ingests nothing. Track if Linux becomes a target.

Decide per real-world source feeds whether any of these need broadening (most likely candidates: HE-AAC, and a graceful "audio-unavailable but keep video" UX when the decoder is missing).

---

## 5. Deferred from P1 (rational frame rate)

- **Drop-frame timecode (TC-2).** Rational rates record correctly, but drop-frame TC display/side-data is not implemented.
- **Rational playback stepping.** `PlaybackTransport` still uses the **rounded integer** rate, so step-by-frame on a 29.97/59.94 recording is ~0.1% off over long durations. Recording is exact; playback stepping is the gap.
- **Arbitrary (non-preset) rates in the UI.** The engine accepts any `{num,den}`, but the UI offers only the standard preset list (23.976/24/25/29.97/30/50/59.94/60).

---

## 6. Test & tooling gaps

- **Wire the soak matrix into CTest (opt-in).** `run_soak_matrix.sh` is a standalone script today. An opt-in `soak`-label ctest registration (guarded like `e2e_native_rtmp_soak`) would make it discoverable; keep it excluded from the default gate (it's long).
- **Cross-device drift harness.** Every current soak/drift number has slope ≈ 1.0 **by construction** — on one machine the source and recorder share the same wall clock. The framesync rig now has a ppm skew injector for one-machine stress, but true clock-drift / phase-lock measurement still needs **two machines** for final validation.
- **Update `drift_2997`.** It still records a 29.97 source at integer 30 (report-only, measuring the mismatch P1 fixes). With rational fps, add a **gated** variant that records at 29.97 and asserts ≈zero drift — a real teeth check for P1, complementing `e2e_record_2997`.
- **macOS-CI vs local gate.** The full e2e/native suites run only in the local pre-push gate (CI runs the short `ci` label). Fine as designed, but means transport regressions are caught at push, not on PRs.

---

## 7. Frame-sync roadmap — the remaining arc (P2 → P5)

This is the larger goal: from "arrival-approximate" toward "measured phase-lock" and true broadcast output. (Source: the leverage-ordered framesync audit.)

Concrete implementation plans now live alongside the Superpowers planning docs:
Phase 0 measurement rig (`docs/superpowers/plans/2026-06-17-framesync-phase0-measurement-rig.md`),
Phase 1 timing core (`docs/superpowers/plans/2026-06-17-framesync-phase1-timing-core.md`), and
Phase 2 NDI ingest (`docs/superpowers/plans/2026-06-17-framesync-phase2-ndi-ingest.md`), backed by
`docs/superpowers/specs/2026-06-17-broadcast-framesync-program-design.md`.

- **P2 — Source clock recovery + drift servo.** Partially live: native SRT/RTMP now route timestamps through `SourceClock`, run a per-source `DriftEstimator`, expose `clockppm`/`clockq` telemetry, and rate-correct the audio FIFO cursor with fractional accumulation. Remaining: persist the recovered clock across destroyed/recreated ingest sessions so reconnects re-lock to the prior recovered clock instead of fresh arrival time, and add an A/V-offset-drift metric to the framesync skew cell.
- **P3 — Timecode.** Extract SMPTE 12M side data (TC-1), align sources by timecode (TC-4), write `tmcd`/tags into the MKV (TC-5).
- **P4 — Interlace / fields.** Deinterlace policy + field-rate slow-mo (only if interlaced sport is in scope; today fields are silently frame-blended).
- **P5 — Architectural / true broadcast output.** A PTP (ST 2059) house-clock client (REF-1); genlocked SDI / ST 2110 output via DeckLink/Rivermax (GENLOCK-1, today output is software `QVideoSink` only); interim: PTS-stamp the `QVideoFrame` + vsync-pace presentation (GENLOCK-2/3/5). **#54 (NDI output bus) is the first concrete step here** — a broadcast output path with preview.
- **Cross-device drift** (two machines) and **encrypted SRT** were also flagged "beyond Phase 2."

**Honest ceiling (unchanged):** non-genlocked SRT/RTMP/UDP ingest can never be true-genlock 100%; the achievable target is *frame-phase-locked-within-measured-bounds* via source-clock recovery (P2).

---

## 8. Known caveats / risk register

- **Windows audio is unproven on a real device** until the §1 smoke-test runs. Highest-risk open item.
- **No decode fallback:** an undecodable stream now fails visibly (source stays connected but unhealthy; decode-error log is throttled to once/5 s). This is by design but is a behavior change for any source outside §4's envelope.
- **Playback step on fractional rates** is ~0.1% off (§5).
- **One-machine soak proves stability, not drift** (§6) — don't read the slope≈1.0 as a phase-lock guarantee.
- **Local backup tags** `bk-A`/`bk-B`/`bk-soak` (the pre-squash commits) exist in the local clone only; safe to delete once the merges are confirmed good.

---

## 9. Suggested next sequence

1. **Verify Windows** (§1) — smoke-test live AAC/RTMP on a Windows machine; confirm the Windows CI build is green on `main`. _Unblocks confidence in #52._
2. **Pick up the capability regressions that real feeds hit** (§3) — most likely **hostname/DNS SRT** and **encrypted SRT** first (common in production SRT).
3. **Rational playback stepping** (§5) — small, closes the last P1 gap.
4. **Stand up the Phase 0 measurement rig** (§6/§7) — including the skew injector, so P2 can be measured on one machine before any two-machine drift work.
5. **Finish P2 reconnect persistence + skew A/V metric** (§7) — the timing core and audio servo are live; reconnect re-lock and the stronger injected-skew gate remain.
6. Output side (**P5 / #54**) proceeds in parallel on its own track.

---

_Maintainer note: keep this file in sync as items close. Memory anchor: `broadcast-framesync-roadmap`._
