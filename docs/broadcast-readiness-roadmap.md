# Broadcast Readiness & Playback-Perfectness Roadmap

**Last verified:** 2026-06-21 against `main` @ `7c71367` (Merge #122 ci/sanitize-worker-and-ingest).

This is a prioritized, evidence-grounded backlog of what remains to make OpenLiveReplay's
playback frame-perfect and broadcast-grade. Every item below was checked against the actual code
(file:line); items already shipped are recorded as **Resolved** / **Partial** so nobody re-does them.

## How this was produced (and how to refresh it)

1. A parallel survey across 7 dimensions (frame-sync, seek/scheduler, A-V sync, output standards,
   codec coverage, 24/7 resilience, validation/CI), each agent reading real code/docs/tests.
2. An **adversarial verification** pass — every claimed gap re-checked against the code; several
   survey claims were refuted (a non-existent skip-forward bug, a miscomputed fractional-fps error,
   two "false-PASS" A-V claims that are really false-FAIL nuisances).
3. A **re-verification** against the latest `main`, to mark each item open / partial / resolved
   with current evidence.

To refresh: re-run the same survey → verify → re-verify flow and update the statuses + the
"Last verified" line above.

### Update log

- **2026-06-19** — initial roadmap, verified against `main` @ `01eaa88`.
- **2026-06-20** — re-evaluated against `main` @ `f3a70e6` (#102, #103). PTP / TimingReference seam
  resolved (#103).
- **2026-06-21** — re-verified against `main` @ `7c71367`. **The entire Tier-1 sprint landed**
  (T1.2/T1.3/T1.4/T1.5/T1.6, and T1.1 closed as a corrected mis-diagnosis), plus **T2.4, T2.5,
  T2.6, T3.2**. All moved to "Recently landed" with current evidence. Only **T2.2, T2.1, T3.3,
  T3.4** remain open.

## Status legend

- 🔴 **Open** — not started.
- 🟡 **Partial** — substantial pieces landed; concrete residual remains (listed).
- 🟢 **Resolved** — fully addressed on current `main` (kept for the record).
- **Leverage** = how much closer to broadcast-ready it gets us. **Effort** = rough implementation size.

---

## Recently landed (do NOT re-do)

Verified present on current `main`. Kept for the record so nobody re-opens them.

### Framesync / timing core

- 🟢 **PTP / TimingReference seam** (PR #103). Pure-virtual `TimingReference`
  (`timingreference.h:14-22`) with `LocalMonotonicReference` (default, byte-identical) and a full
  `PtpReference` (IEEE 1588 slave `udpptpclient.cpp`, windowed servo `ptpservo.*`, loss-degrade,
  single env swap `OLR_TIMING_PTP=1`, tier surfaced as `sessionReferenceTier`, ~36 unit cases).
- 🟢 **Inter-camera phase servo + confidence tier** (PR #100). `SourceOffsetEstimator` with a
  `ConfidenceTier`, an *active* capped+ramped phase servo (`replaymanager.cpp`, `±80ms`, `4ms/pulse`),
  confidence in the UI, and a live `e2e_framesync_intercam` gate.
- 🟢 **SMPTE 12M timecode ingest → MKV** (PR #90). SEI TC extraction (`h26xseitimecode.cpp`),
  RTMP AMF fallback, the MKV `tmcd`/timecode tag (`muxer.cpp`), `TimecodeAligner` wiring.
  *(Output-side TC passthrough is still open — see T2.1.)*

### Tier 1 sprint (all landed 2026-06-20 → -21)

- 🟢 **T1.1 — Audio drift servo: closed as a corrected mis-diagnosis.** The roadmap originally
  prescribed scaling `srcAdvance` by `(1 + ppm/1e6)` in `writeAudioForTick`. Investigation showed the
  recording audio FIFO is **already** slope-corrected through `toSessionMs`, so applying ppm on top
  *double-corrects* (measured +24 ms → −75 ms). `srcAdvance = n` (`streamworker.cpp:769`) is therefore
  intentionally left as-is — no servo is needed. The `run_drift_avsync` scenario remains unregistered
  by design (too flaky at 2000 ppm / 60 s to give a stable signal). The SRT reconnect re-lock guard
  that would have protected such a servo already landed (`nativesrtingestsession.cpp`, `if (!m_externalClock)`).
- 🟢 **T1.2 — Output graph uses the rational frame rate** (commit `661fd78`). `PlaybackWorker` builds
  the output `FrameRate` from `m_transport->frameRate()` (both sites); the transport is fed the true
  rational from the UI (`uimanager.cpp:376/1509/2425`); `NdiOutputSink` emits `frame_rate_N/D` from it
  (`ndisink.cpp:84-85`). `fps()` stays integer for scheduling. Tested (`tst_ndisink` 30000/1001,
  `tst_playbacktransport`, `tst_outputdispatcher`). 29.97/59.94 no longer collapse to 30/60.
- 🟢 **T1.3 — Pre-roll bank never software-decodes H.264** (PR #115). `openPrerollContext`
  (`playbackworker.cpp:1032`) now **hardware-decodes** the H.264 pre-roll via `NativeVideoDecoder`
  (feedIndex 1:1), going beyond the original residual (which proposed merely skipping H.264). Kills the
  armed-cut flash on H.264 recordings.
- 🟢 **T1.4 — Fatal muxer write errors surfaced** (`muxer.{h,cpp}`, `replaymanager.{h,cpp}`,
  `uimanager.cpp`). A sustained-write-failure flag → `recordingError(QString)` → operator alert
  (recording is not auto-stopped; the operator decides).
- 🟢 **T1.5 — Output bus under the CI sanitizers** (`ci.yml`). `tst_outputdispatcher`,
  `tst_outputbusengine`, `tst_outputruntime` (and more) now run in the ASan/UBSan + TSan matrix.
- 🟢 **T1.6 — Unimplemented output sinks warn** (`playbackworker.cpp:394`). DeckLink/ST2110/AJA/OMT
  now `qWarning` instead of silently producing zero output.

### Tier 2 / 3 items since shipped

- 🟢 **T2.4 — Scrub-cancels-armed-cut race.** A manual `seekTo` now supersedes pending armed-cut
  work: it clears the queued re-arm and bumps the seek generation so `maybeFireScheduledCut` aborts a
  stale/in-flight cut (`playbackworker.cpp` seekTo + `m_armSeekGen`).
- 🟢 **T2.5 — NDI ingest resilience** (PRs #123/#125). Stall timer + reconnect (mirroring SRT/RTMP),
  the stop-time receiver UAF fixed (flag-before-close on the capture thread), plus reconnect/soak gates.
- 🟢 **T2.6 — Playback/lipsync acceptance matrix in CI.** CI now runs the high-signal `e2e_play_*`
  cases (`ci.yml`: storm/seekplay/armedcut/playlist) alongside the record + sanitizer matrix, so the
  playback hardening can no longer regress unseen.
- 🟢 **T3.2 — Rational fps through encode/mux + drop-frame TC** (PRs #128/#130/#131/#132). Settings
  carry `fpsNum/fpsDen` through ReplayManager/StreamWorker → encoder `framerate` + muxer
  `avg/r_frame_rate` (integer `time_base` kept to avoid an A/V-drift leak); `olr::Timecode` drives the
  drop-frame TC display; a rate-agnostic PTS-timing e2e lock gates every record path.
- 🟢 **Codec benchmark engine + capability gating** (PRs #96, #101). Off-GUI-thread capability probe,
  H.264 greyed-out when unavailable, hard-block + soft capacity warning at record start.
  *(Objective PSNR/SSIM gate still open — see T3.4.)*
- 🟢 **Operator replay mark-in/out/recall — core** (PRs #97/#99). `markIn/markOut/recallEntry`
  Q_INVOKABLEs + buttons exist. *(Full recallable playlist UI still open — see T2.2.)*
- 🟢 **Click-free audio at the armed cut** (PR #97). Active-view audio is staged for the cut window
  and the transition fades out/in.

---

## Open items

### T2.1 🟡 SMPTE timecode — NDI output-side passthrough — **leverage: medium, effort: medium**
Ingest→MKV timecode landed (#90). What remains is carrying timecode (and a real transport timestamp)
onto the NDI *output* — today both are hardcoded.
- **Evidence (@ 7c71367):** `playback/output/ndisink.cpp:89,167` (`timecode = kTimecodeSynthesize` for
  both video and audio); `OutputBusFrame` carries no timecode field.
- **Residual:** add a timecode field (e.g. `int64_t sourceTimecode100ns`) to `OutputBusFrame`, thread it
  from the playback cache, and assign real `timecode`/`timestamp` in `NdiOutputSink::sendFrame`. Composes
  with the now-landed rational + drop-frame TC work (T3.2). The transport timestamp also improves
  receiver jitter buffering, independent of programme TC.

### T2.2 🟡 Operator replay UI — full recallable playlist — **leverage: medium, effort: medium**
Mark-in/out/recall and the firing/playout engine exist, but the operator *recall UI* is still a single
hardcoded "Recall 0" button — an operator can't see or recall the accumulated entries.
- **Evidence (@ 7c71367):** `uimanager.h:301-302` (`recallEntry(int)`, `playlistCount()` are
  Q_INVOKABLE, not Q_PROPERTY); `Main.qml:1029` (`Button { text: "Recall 0"; … recallEntry(0) }`, no
  Repeater/ListView bound to the playlist); no `playlistCountChanged` signal and no per-entry labels.
- **Residual:** expose playlist count as a Q_PROPERTY+signal; replace the hardcoded button with a
  Repeater/ListView; expose entry labels (timecode/clip) for display.

### T3.3 🟡 10-bit / 4:2:2 / HDR + colorimetry, multiview scaling, real SDI/ST2110 — **leverage: low-med, effort: large (small for multiview)**
Output is locked to 8-bit YUV420P; multiview uses nearest-neighbour scaling; SDI/ST2110 sinks are stubs.
*(Qt preview colorspace tagging was fixed in `qtpreviewsink.cpp`.)*
- **Evidence (@ 7c71367):** `outputtypes.h` (only `Yuv420p`), `mediaframe.h` (no colorimetry field),
  `yuv420pcompositor.cpp:8,44-57` (`scalePlaneNearest`), `playbackworker.cpp` SDI/ST2110 arms warn-and-skip (T1.6).
- **Residual:** (1) add `MediaPixelFormat` variants (Yuv422p/P010) + a colorimetry field threaded through
  decode→sinks; (2) bilinear (or swscale) multiview scaling — small, self-contained; (3) real DeckLink/ST2110
  sinks (needs the DeckLink SDK) — large.

### T3.4 🟡 Objective quality gate + two-machine drift soak — **leverage: low, effort: small (PSNR) / medium (soak)**
The codec benchmark/probe/UI all landed (#96/#101). What's missing is objective video-quality measurement and
true cross-device drift testing.
- **Evidence (@ 7c71367):** `tst_h264_roundtrip.cpp` checks frame count/all-intra/dimensions but no PSNR/SSIM
  (`grep psnr|ssim|vmaf` across `tests/` = none). `run_srt_soak.sh` marks two-machine drift out of scope
  (single wall clock).
- **Residual:** add a PSNR (≥~40 dB) assertion to the H.264 round-trip (test-only); build a two-machine soak
  harness (recorder + source on different wall clocks) for real drift measurement.

---

## Summary table

| ID | Item | Tier | Status | Leverage | Effort |
|----|------|------|--------|----------|--------|
| T1.1 | Audio drift servo (ppm) | 1 | 🟢 Resolved (corrected: no servo needed) | — | — |
| T1.2 | Output clock collapses fractional fps | 1 | 🟢 Resolved | High | Small |
| T1.3 | Pre-roll software-decodes H.264 (seek flash) | 1 | 🟢 Resolved | High | Small |
| T1.4 | Muxer write errors silently swallowed | 1 | 🟢 Resolved | High | Small-Med |
| T1.5 | Output bus has no sanitizer coverage | 1 | 🟢 Resolved | High | Small |
| T1.6 | Unimplemented sinks fail silently (warning) | 1 | 🟢 Resolved | Low* | Small |
| T2.1 | SMPTE timecode: NDI output-side passthrough | 2 | 🟡 Partial | Medium | Medium |
| T2.2 | Operator replay UI: full recallable playlist | 2 | 🟡 Partial | Medium | Medium |
| T2.4 | Scrub-cancels-armed-cut race | 2 | 🟢 Resolved | Medium | Small-Med |
| T2.5 | NDI ingest resilience (stall/reconnect/race/gates) | 2 | 🟢 Resolved | Medium | Medium |
| T2.6 | Playback/lipsync acceptance matrix into CI | 2 | 🟢 Resolved | Medium | Medium |
| T3.2 | Rational fps through encode/mux + DF TC display | 3 | 🟢 Resolved | Medium | Medium |
| T3.3 | 10-bit/4:2:2/HDR, multiview scaling, SDI/ST2110 | 3 | 🟡 Partial | Low-Med | Large |
| T3.4 | PSNR/SSIM gate + two-machine drift soak | 3 | 🟡 Partial | Low | Small-Med |

\* T1.6 leverage is low for the one-line warning; the real hardware sinks (T3.3) are high-leverage/large.

## Notes & caveats

- **Tier 1 is fully cleared.** The remaining work is the four 🟡 items: NDI output-side timecode (T2.1),
  the recallable playlist UI (T2.2), 10-bit/HDR/SDI + multiview scaling (T3.3), and the objective
  quality gate + two-machine soak (T3.4). T2.1 is the natural next step — it finishes the SMPTE-12M
  story (ingest→MKV landed in #90) and composes with the rational + drop-frame TC work (T3.2).
- **Not genlock-grade → largely closed.** The inter-camera servo (#100), SMPTE timecode ingest (#90),
  and the PTP/TimingReference external-reference seam (#103) landed the hardest framesync parts. The PTP
  path is opt-in (`OLR_TIMING_PTP=1`) with no two-machine validation rig yet (T3.4).
- **Validation reality.** The NDI/SRT/RTMP gates are self-hosted/opt-in (need a runtime). True
  frame-accurate cross-device lock and real drift still need a two-machine rig (T3.4).
- This doc is a point-in-time snapshot — re-verify before acting on a 🟡 item, as `main` moves quickly.
