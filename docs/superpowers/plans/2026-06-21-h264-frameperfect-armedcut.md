# Frame-Perfect Armed Cut on H.264 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Hardware-decode H.264 in the armed-cut pre-roll bank so Recall is frame-perfect on H.264 (today it degrades to a plain seek). Design: `docs/superpowers/specs/2026-06-21-h264-frameperfect-armedcut-design.md`.

**Architecture:** Port the primary decode bank's synchronous `NativeVideoDecoder` pattern into `openPrerollContext`/`fillStaging` (the decoder's `decode()` blocks until the inline callback fires, so the bounded synchronous staging fill works unchanged). Then flip the `e2e_play_armedcut_h264` gate from validating degradation to validating a frame-perfect cut.

**Tech Stack:** C++17, Qt 6, FFmpeg, VideoToolbox/MediaFoundation NativeVideoDecoder, CTest, bash.

## Global Constraints

- WORKTREE ONLY: `/Users/timo.korkalainen/Development/timo/OpenLiveReplay/.claude/worktrees/h264-armedcut`. Verify `git rev-parse --show-toplevel` ends with `h264-armedcut` before any commit. NEVER touch the main checkout or other worktrees.
- PRODUCTION code in a delicate path. Format ONLY changed lines: `git clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format origin/main` after staging. Match the file's hand style. `git add` only the task's files.
- Build: `cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON`; build `~/Qt/Tools/Ninja/ninja -C build/claude-debug <target>`. H.264 HW IS available here (gates run, not skip).
- PRESERVE the armed-cut hardening (#94 re-anchor/divergence, #98 re-arm race, #104 decoder-follow/post-seek flush, #107 seek-vs-cut): the native pre-roll decode runs entirely on the worker thread inside `fillStaging`, writes staging only via `insertVideoFrame`, and `m_stagingCovers` stays the sole scheduling gate. Do not alter `maybeFireScheduledCut`/the swap.
- No header / `DecoderTrack` change — the fields (`nativeDecoder`, `h264ParamSets`, `codecWidth/Height`) already exist.

---

### Task 1: Wire NativeVideoDecoder into the pre-roll bank + flip the H.264 armed-cut gate

This is ONE cohesive change: the production wiring makes `armNextCut` succeed on H.264 (which would break the *current* degradation gate), and the gate flip validates the new frame-perfect behavior. They land together.

**Files:**
- Modify: `playback/playbackworker.cpp` (`openPrerollContext`, `fillStaging`, pre-roll teardown)
- Modify: `tests/e2e/play_harness.cpp` (`armedcut-h264` scenario)
- Modify: `tests/e2e/run_playback_e2e.sh` (`armedcut-h264` assertion case)

**Interfaces (read these in the current code before editing):**
- Primary bank H.264 wiring to MIRROR: `playbackworker.cpp:1463-1522` (avcC→SPS/PPS parse + `NativeVideoDecoder` construction + `feedIndex/providerIndex++`).
- Primary native decode+commit to MIRROR: `playbackworker.cpp:557-636` (`decodePacketIntoBank` native branch: avcC→Annex-B, `CompressedAccessUnit`, `decode(unit, handleFrame)`, inline `convertToMediaVideoFrame` + insert).
- `NativeVideoDecoder` API: `recorder_engine/ingest/nativevideodecoder.h:21-37` (`decode(unit, FrameCallback, err)` synchronous; `reset()`). `CompressedAccessUnit`/`H26xParameterSets`: `recorder_engine/ingest/h26xaccessunit.h:16-22`.
- Pre-roll: `openPrerollContext` (~`:979-1083`, H.264 skip at ~`:1022`), `fillStaging` (~`:1144-1297`, FFmpeg decode at ~`:1187-1212`, backward seek at ~`:1156`, coverage `m_stagingNewestRefPtsMs >= coverTo`), teardown (~`:2034-2037`).
- `DecoderTrack`: `playbackworker.h:34-49`. (Line numbers are guides; locate by code.)

- [ ] **Step 1: openPrerollContext — build a native decoder for H.264 instead of skipping**

In `openPrerollContext`'s video-stream loop, replace the unconditional `if (codecParams->codec_id == AV_CODEC_ID_H264) continue;` with a mirror of the primary bank (`:1463-1522`): when `codecParams->codec_id == AV_CODEC_ID_H264`:
  - if `queryNativeVideoDecodeCapabilities().h264 && codecParams->extradata_size >= 8`: parse avcC → `H26xParameterSets` exactly as the primary bank does; create a `DecoderTrack` with `track->nativeDecoder = std::make_unique<NativeVideoDecoder>(codecParams->width, codecParams->height)`, `track->h264ParamSets = <parsed>`, `track->codecWidth = codecParams->width`, `track->codecHeight = codecParams->height`, `track->codecCtx = nullptr`, `track->provider = nullptr`, `track->streamIndex = i`, `track->feedIndex = feedIndex`; append to `m_prerollBank`; `feedIndex++`.
  - else (no HW or bad extradata): `continue;` (skip — graceful fallback, feature off for that file).
Keep the non-H.264 (FFmpeg) branch exactly as-is. Reuse the primary bank's avcC-parse code verbatim (do not re-derive it).

- [ ] **Step 2: fillStaging — decode native H.264 tracks into the staging cache**

In `fillStaging`'s per-track loop, branch: `if (track->nativeDecoder) { <native> } else { <existing FFmpeg send/receive, unchanged> }`. The native branch mirrors `decodePacketIntoBank`'s native path (`:557-634`) but writes to staging:
  - convert the avcC packet to Annex-B (copy the primary's conversion); build `CompressedAccessUnit{codec=H264, parameterSets=track->h264ParamSets, pts90k/dts90k = av_rescale_q(pkt->pts/dts, stream tb, {1,90000})}`;
  - `auto stage = [&](AVFrame* nvf) { MediaVideoFrame mf = convertToMediaVideoFrame(nvf, track->feedIndex); mf.ptsMs = av_rescale_q(pkt->pts, stream tb, {1,1000}); if (mf.isValid()) { m_prerollStagingCache->insertVideoFrame(mf); if (track->streamIndex == <ref/primary stream>) m_stagingNewestRefPtsMs = qMax(m_stagingNewestRefPtsMs, mf.ptsMs); } };`
  - `track->nativeDecoder->decode(unit, stage, nullptr);` (synchronous — `stage` runs inline before it returns).
  - Do NOT use `m_bufferMutex`/`track->buffer`/`FrameIndex` (those are primary-bank live-path only). Match how the existing FFmpeg pre-roll branch identifies the ref/coverage stream and updates `m_stagingNewestRefPtsMs`.

- [ ] **Step 3: fillStaging — reset the native session after the backward seek**

Right after the backward `av_seek_frame` + `avformat_flush` (the first-call seek block, ~`:1156`), for each native pre-roll track call `track->nativeDecoder->reset();` so the VT/MF session starts clean post-seek (PTS fidelity for the divergence gate). (All-intra makes this safe + cheap.)

- [ ] **Step 4: Teardown — release the native decoder**

In the pre-roll bank teardown (~`:2034-2037`, where `track->codecCtx` is freed), add `track->nativeDecoder.reset();` before `delete track` (mirror the primary `clearDecoders` lambda at `:1412`). Prevents a VT/MF session leak/UAF.

- [ ] **Step 5: Flip the `armedcut-h264` play_harness scenario to a real armed cut**

In `tests/e2e/play_harness.cpp`, rework the `armedcut-h264` scenario to MIRROR the MPEG-2 `armedcut` scenario (read it in the same file): arm a cut to mid-clip, play through it, and arm a SECOND (queued re-arm) so `cutsFired==2` — i.e. make it behave like `armedcut` but on the H.264 fixture. Keep capturing `armNextCutArmed = worker.armNextCut(target) ? 1 : 0` (it now becomes 1). Remove the old "simulate fallback seek" (it's no longer the path). Keep the COUNTERS line emitting `armNextCutArmed` + `decodedVideoFrames` (already present).

- [ ] **Step 6: Flip the `armedcut-h264` assertions to frame-perfect**

In `tests/e2e/run_playback_e2e.sh`, replace the `armedcut-h264)` case-block assertions (currently degradation: armNextCutArmed==0, cutsFired==0, reposition>=1) with the frame-perfect set mirroring the MPEG-2 `armedcut)` block:
  - `placeholderFramesDelta == 0`, `reposition == 0`, `cutsFired == 2`, `cutFollowReposition == 0`, `maxClockDivergenceMs <= 1500`,
  - `armNextCutArmed == 1` (armed succeeded on H.264),
  - `decodedVideoFrames >= 30` (NON-VACUITY: real HW frames decoded into staging — keep this, it is load-bearing).
Keep `REC_CODEC_EXTRA="--codec h264"` for `armedcut-h264` (the fixture stays H.264).

- [ ] **Step 7: Build + verify (the gate now proves frame-perfect H.264 armed cut)**

```bash
cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
~/Qt/Tools/Ninja/ninja -C build/claude-debug play_harness record_harness
bash -n tests/e2e/run_playback_e2e.sh && echo SH_OK
# THE feature gate — must now PASS frame-perfect (cutsFired==2, placeholderFramesDelta==0, divergence<=1500, decodedVideoFrames>=...):
ctest --test-dir build/claude-debug -R 'e2e_play_armedcut_h264' --output-on-failure
# Regressions — MPEG-2 armed cut, backward cut, and H.264 plain playback must be UNAFFECTED:
ctest --test-dir build/claude-debug -R 'e2e_play_armedcut$|e2e_play_armedcut_back|e2e_play_h264' --output-on-failure
```
Expected: `e2e_play_armedcut_h264` PASSES with `cutsFired=2 placeholderFramesDelta=0 reposition=0 maxClockDivergenceMs<=1500 armNextCutArmed=1 decodedVideoFrames>=~300`. The mpeg2/back/h264_play gates unchanged. Run `e2e_play_armedcut_h264` **3×** to confirm it is not flaky (the divergence/coverage timing on the native path). Capture the COUNTERS line. If divergence exceeds 1500 or it's flaky → the native PTS path needs the post-seek reset (Step 3) / PTS-rescale audit; debug, do NOT weaken the gate.

- [ ] **Step 8: Format changed lines + commit**

```bash
git add playback/playbackworker.cpp tests/e2e/play_harness.cpp tests/e2e/run_playback_e2e.sh
git clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format origin/main
git add playback/playbackworker.cpp tests/e2e/play_harness.cpp tests/e2e/run_playback_e2e.sh
git commit -m "feat(playback): frame-perfect armed cut on H.264 (HW-decode the pre-roll bank) + flip the e2e gate"
```

---

### Task 2: Full verification

**Files:** none.

- [ ] **Step 1: Build everything + run the playback acceptance matrix + H.264 gates**

```bash
~/Qt/Tools/Ninja/ninja -C build/claude-debug
ctest --test-dir build/claude-debug -R 'e2e_play_' --output-on-failure
ctest --test-dir build/claude-debug -L unit --output-on-failure
```
Expected: all `e2e_play_*` pass (incl. armedcut, armedcut-back, armedcut-h264, h264, playlist, storm, seekflash, …); units green. Capture the `e2e_play_armedcut_h264` + `e2e_play_armedcut` COUNTERS lines side by side (H.264 should now match MPEG-2's frame-perfect profile).

- [ ] **Step 2: Clean tree + main untouched**

```bash
git diff --check
git status --short
git -C /Users/timo.korkalainen/Development/timo/OpenLiveReplay status --porcelain | grep -E "playback/|tests/" || echo "main: no tracked changes"
```

---

## Self-Review Checklist

- **Design coverage:** native pre-roll decode (Steps 1-2), post-seek reset (3), teardown (4), gate flip (5-6), verification (7, Task 2). Matches the spec.
- **No async bridge:** `decode()` is synchronous; the native branch sits where the FFmpeg send/receive sat; coverage watermark unchanged.
- **Hardening preserved:** change gated behind `track->nativeDecoder` (H.264 only); MPEG-2 takes the unchanged FFmpeg branch; `maybeFireScheduledCut`/swap untouched; staging written only on the worker via `insertVideoFrame`.
- **Non-vacuity:** the flipped gate keeps `decodedVideoFrames>=30` so a cut firing off an empty staging cache fails.
- **No header change:** `DecoderTrack` fields already exist.
