# Native-only ingest: remove the ffmpeg path + Windows MF AAC — Design (PR B)

**Status:** approved (brainstorm 2026-06-17). Decisions: cross-platform AAC unit tests + Windows compile + user smoke-test for live decode; drop udp://+file:// with clean rejection and no decode fallback; **rename `AudioToolboxAacDecoder` → `NativeAacDecoder`** with platform TUs (mirrors `NativeVideoDecoder`).
**Base branch:** `feat/ffmpeg-ingest-removal`, **stacked on `feat/rtmp-parity`** (PR A). Local-only — **no push** until the user re-grants permission.
**Stacked context:** This is **PR B** of two. PR A (RTMP parity) is complete on `feat/rtmp-parity`. PR B targets `feat/rtmp-parity` until PR A merges, then rebases onto `main`.

## Goal

OpenLiveReplay ingests **only** native SRT and native RTMP (H.264/H.265 video + AAC audio). Remove `FfmpegIngestSession` and the native→ffmpeg fallback machinery, make native SRT the default (no env gate), and add a Windows Media Foundation AAC decoder so Windows keeps native audio once the ffmpeg fallback is gone. **FFmpeg remains a dependency** — `muxer.cpp` (libavformat output) and the MPEG-2 encoder in `streamworker.cpp`/`replaymanager.cpp` (libavcodec) are unchanged; only the *ingest* use of ffmpeg (demux + decode of an incoming stream) is removed.

## Background

From the ingest architecture audit (2026-06-17):
- Routing today: `rtmp/rtmps` → native by default; `srt` → ffmpeg unless `OLR_NATIVE_SRT` is set; `udp`/`file`/other → always ffmpeg (the `IngestBackendKind::Ffmpeg` default).
- `FfmpegIngestSession` is the only backend serving `udp://` (MPEG-TS) and `file://`, and the fallback target when a native backend rejects a URL or hits a decode-capability error.
- Native Windows AAC decode is a **stub** (`audiotoolboxaacdecoder_stub.cpp`) — Windows native ingest relies on the ffmpeg fallback for audio today.
- The udp:// e2e tests (`e2e_record_*`, `e2e_play_*` record phase, `sync_*`, `e2e_av_lipsync`) feed the engine via ffmpeg ingest. The `e2e_srt_*` (`srt` label) gates exist solely to exercise the *ffmpeg* SRT path; their `e2e_native_srt_*` twins run the same scripts natively.
- CI installs `srt` (`brew install ninja ffmpeg srt ccache`) and its primary gate runs unit-only (`-L ci -LE 'sync-report|srt|native-apple-ingest|native-rtmp'`); the udp:// e2e run via the local pre-push hook, where `srt-live-transmit` is already required by the SRT ladder. So the SRT-bridge migration adds **no new CI dependency** and loses no CI coverage. The Windows CI job builds the app (compile-check) but runs no tests.

## Components

### Component 1 — Windows Media Foundation AAC decoder + `NativeAacDecoder` rename (prerequisite; land first)

The AAC decoder both native sessions use has three TUs today under the historical name `AudioToolboxAacDecoder`: `audiotoolboxaacdecoder.h` (decl), `audiotoolboxaacdecoder.mm` (Apple), `audiotoolboxaacdecoder_stub.cpp` (everything else, incl. Windows = no decode). It is now genuinely multi-platform, so rename it to `NativeAacDecoder` to match the `NativeVideoDecoder` precedent (`nativevideodecoder.h` + `_videotoolbox.mm`/`_mediafoundation.cpp`/`_stub.cpp`), and add the Windows TU:

- **Rename** (mechanical, behavior-preserving):
  - `audiotoolboxaacdecoder.h` → **`nativeaacdecoder.h`**, class `AudioToolboxAacDecoder` → `NativeAacDecoder` (keep `AacAdtsFrameInfo` + the static `parseAdtsFrame(...)` API).
  - `audiotoolboxaacdecoder.mm` → **`nativeaacdecoder_audiotoolbox.mm`** (`#ifdef __APPLE__`, AudioToolbox impl, logic unchanged).
  - `audiotoolboxaacdecoder_stub.cpp` → **`nativeaacdecoder_stub.cpp`**, guard narrowed from `#ifndef __APPLE__` to `#if !defined(__APPLE__) && !defined(_WIN32)`.
  - Update the includes + type names in `nativesrtingestsession.{h,cpp}` and `nativertmpingestsession.{h,cpp}` (the two callers).
  - Rename the test `tests/unit/tst_audiotoolboxaacdecoder.cpp` → **`tst_nativeaacdecoder.cpp`** (class `TestNativeAacDecoder`, target `tst_nativeaacdecoder`), update `tests/unit/CMakeLists.txt`, and update the **`ci.yml` sanitizer matrix** entry `tst_audiotoolboxaacdecoder` → `tst_nativeaacdecoder` (the one place CI names it).
  - Update the `recorder_engine` + `tests/CMakeLists.txt` source lists to the new filenames, selecting TUs by platform exactly as the video decoder does (`if(APPLE)`/`elseif(WIN32)`/`else()`).
- **New `recorder_engine/ingest/nativeaacdecoder_mediafoundation.cpp`** (`#ifdef _WIN32`): implements the `NativeAacDecoder` API via the Media Foundation AAC decoder MFT (`MFTEnumEx` for `MFAudioFormat_AAC` / `CLSID_CMSAACDecMFT`): set the input type from the AudioSpecificConfig (sample rate / channels / object type), output `MFAudioFormat_PCM` 16-bit, then resample/remix to the engine's canonical **48 kHz S16 stereo** (matching the `.mm`'s 48k/2ch/S16 contract). AAC-LC only (reject other object types, same as Apple). Initialize/teardown COM + MF per the conventions already in `nativevideodecoder_mediafoundation.cpp`. The static `parseAdtsFrame(...)` (ADTS header parse) is platform-agnostic — keep it available on every platform (in `nativeaacdecoder_mediafoundation.cpp` for Windows, same as the stub/.mm provide it on their platforms) so the parsing unit tests compile and run everywhere.

**Verification:** the ADTS/ASC parsing + the 48k/S16/stereo output contract are exercised by cross-platform unit tests in `tst_nativeaacdecoder` (the parse path is compiled on every platform). The MF *decode* itself is compile-checked by the Windows CI build and live-smoke-tested by the user on a Windows machine. (No Windows test runner is added — out of scope per the brainstorm.)

### Component 2 — Native-only ingest excision (cohesive)

This must change together to keep the build consistent.

- **Delete** `recorder_engine/ingest/ffmpegingestsession.{h,cpp}` and `recorder_engine/ingest/nativefallbackpolicy.{h,cpp}`.
- **`ingestsession.h`/`.cpp`:**
  - `enum class IngestBackendKind { NativeSrt, NativeRtmp, Unsupported };` (drop `Ffmpeg`).
  - `selectIngestBackend(url, options)`: `srt` → `NativeSrt`; `rtmp`/`rtmps` → `NativeRtmp`; anything else → `Unsupported`. (The native-availability gating stays in StreamWorker via the `*_AVAILABLE` macros; on a build without a native backend compiled, the scheme still maps to its native kind but StreamWorker reports it unsupported — see below.)
  - `ingestBackendOptionsFromEnvironment(...)`: SRT is native by default — `preferNativeSrt = nativeSrtAvailable && scheme=="srt"` (remove the `qEnvironmentVariableIsSet("OLR_NATIVE_SRT")` gate). `preferNativeRtmp` unchanged.
  - Remove `augmentSrtUrl()` (ffmpeg-SRT only; no native caller — verify the grep before deleting) and the `IngestSession::nativeFallbackReason()` virtual. Keep `IngestFailureKind`, `shouldStopNativeRtmpAfterFailure`, `jitterWindowMs`, and the SRT latency/jitter constants (still used by the native SRT path + StreamWorker).
- **`nativesrtingestsession.{h,cpp}`:** remove the `nativeDecodeErrorRequestsFallback`/`nativeFallbackReason` usage. On a decode-capability error there is no fallback: log a clear message and let the source fail/stop (the existing reconnect/backoff loop will retry the connection, but an undecodable stream stays unhealthy — surfaced via connection status). Drop the `nativeFallbackReason()` override.
- **`streamworker.cpp`:** remove `#include "ingest/ffmpegingestsession.h"` and the `FfmpegIngestSession` construction branch; remove the `suppressNativeForCurrentUrl` → ffmpeg-retry path and the `nativeFallbackReason()` handling. Add an `Unsupported` branch: log `"Source N: unsupported ingest scheme '<scheme>' — OpenLiveReplay ingests only srt://, rtmp://, rtmps://"`, `setConnected(false)`, and fail the source cleanly (do not spin a tight reconnect loop on an permanently-unsupported URL — break out / mark the source idle). Remove the now-dead int-`targetFps`→ingest plumbing (the native ctors take no fps; the `FrameRate` encoder path is unchanged).
- **Unit tests:**
  - Rewrite `tests/unit/tst_ingestbackendselector.cpp`: drop `defaultRoutesEverythingToFfmpeg`, `canConstructFfmpegSession`, the `FfmpegIngestSession` include, and the `nativeDecodeErrorRequestsFallback` cases. Add/keep: `srt → NativeSrt`, `rtmp/rtmps → NativeRtmp`, `udp/file/other → Unsupported`; the env-default tests (SRT native without any env var); `shouldStopNativeRtmpAfterFailure`; and the RTMP anchor + shared-anchor tests (from PR A) unchanged.
  - Remove `tests/unit/tst_srt_options.cpp` (tests the deleted `augmentSrtUrl`) and its registration.
  - **CMake:** drop `ffmpegingestsession.cpp` + `nativefallbackpolicy.cpp` from the `olr_test_*` / app source lists; drop the `tst_srt_options` registration.

### Component 3 — Test migration + delete the duplicate gates

- **Migrate the udp:// drivers to the native SRT bridge** (reuse `tests/e2e/srt_lib.sh`'s `srt_bridge`, which runs `srt-live-transmit "udp://…listener" "srt://…listener"`):
  - `run_record_e2e.sh`: keep the ffmpeg `testsrc2`+sine producer emitting UDP MPEG-TS, add an `srt_bridge` UDP→SRT, and point `record_harness --url` at the `srt://` caller URL. Native SRT is now the default, so no env var. The `fps2997` scenario (PR P1) keeps its 29.97 producer through the bridge.
  - `run_sync_e2e.sh`: same bridge for every scenario (`intercam_matched/skew/drift_2997/lipsync/intercam_trim`); reuse the flash/beep producers + ffprobe asserts.
  - `run_playback_e2e.sh`: its record phase feeds udp:// today — migrate that phase to the SRT bridge (playback itself is decode-only and unchanged).
  - These require `srt-live-transmit` (already a local + CI dependency via `brew install srt`); SKIP cleanly (exit 0) if the tool is missing, matching `srt_lib.sh`'s `srt_require_tools` convention.
- **Delete the ffmpeg-SRT duplicate gates** (`srt` label): `e2e_srt_smoke/4cam/sync/trim/connect` and their `set_tests_properties`. Their `e2e_native_srt_*` twins (same scripts, native backend) remain the coverage.
- **Build-config cleanup:** remove the `OLR_FFMPEG_SRT_PREFIX` CMake cache option (`tests/CMakeLists.txt`) and its references in `run_srt_*.sh` comments + `SRT_README.md`; remove the now-defunct `ENVIRONMENT "OLR_NATIVE_SRT=1"` from the `e2e_native_srt_*` registrations (the env var no longer exists). Keep all `native-rtmp` and `native-apple-ingest` gates unchanged.

### Component 4 — Docs

Update `tests/e2e/SRT_README.md` (and any ingest doc): native-only ingest; SRT native-default; the removed ffmpeg path / `OLR_NATIVE_SRT` / `OLR_FFMPEG_SRT_PREFIX`; the new Windows MF AAC decoder; the udp→SRT-bridge test migration; the dropped udp/file support + clean rejection + no fallback.

## Error handling / edge cases
- **Unsupported scheme** (`udp://`, `file://`, anything not srt/rtmp/rtmps): the selector returns `Unsupported`; StreamWorker logs a clear one-line reason, marks the source disconnected, and does not busy-retry. The app keeps running; the source dot stays red/disconnected.
- **Undecodable native stream** (e.g. Windows HEVC without the OS HEVC extension, non-AAC-LC audio, an unsupported H.264 profile): no ffmpeg fallback — the native session logs the capability error; video/audio for that source is absent and the source is unhealthy. This is the intended "native-or-nothing" behavior.
- **Native backend not compiled** (a build without `OLR_NATIVE_SRT_AVAILABLE`/`OLR_NATIVE_RTMP_AVAILABLE`): StreamWorker has no session to construct → treat as Unsupported with a clear log (rather than the old silent ffmpeg fallthrough).
- **Windows AAC MFT absent/older OS:** the MF decoder reports an init error; audio for that source is unavailable (logged) — analogous to the Windows HEVC-extension gap on the video side.

## Testing
1. **Unit — selector** (`tst_ingestbackendselector`): srt→NativeSrt, rtmp/rtmps→NativeRtmp, udp/file/other→Unsupported; SRT native by default (no env); `shouldStopNativeRtmpAfterFailure`; RTMP anchor/shared-anchor cases unchanged. No `Ffmpeg`/fallback references remain.
2. **Unit — AAC parsing** (`tst_nativeaacdecoder`, renamed from `tst_audiotoolboxaacdecoder`): the ADTS/ASC parsing + the decode-output contract run cross-platform (the parse logic compiled on every platform). Apple decode path unchanged by the rename.
3. **E2e — migrated** (`e2e_record_stereo/mono/2997`, `e2e_play_*`, `sync_*`, `e2e_av_lipsync`): pass over the native SRT bridge (producers/markers/asserts reused). `drift_2997` keeps measuring 29.97 through the bridge.
4. **E2e — unchanged:** `native-rtmp` (8) + `native-apple-ingest` (12) gates stay green; the deleted `e2e_srt_*` gates are gone.
5. **Build:** macOS app + tests build without `ffmpegingestsession`/`nativefallbackpolicy`; FFmpeg still links (muxer/encoder). Windows CI build compiles the MF AAC decoder. The full local CTest gate (pre-push) passes on native sources.
6. **Manual (user):** live AAC decode smoke on a Windows machine.

## Files touched
- **New:** `recorder_engine/ingest/nativeaacdecoder_mediafoundation.cpp` (Windows MF AAC).
- **Renamed (AAC decoder → `NativeAacDecoder`):** `audiotoolboxaacdecoder.h`→`nativeaacdecoder.h`; `audiotoolboxaacdecoder.mm`→`nativeaacdecoder_audiotoolbox.mm`; `audiotoolboxaacdecoder_stub.cpp`→`nativeaacdecoder_stub.cpp` (guard narrowed); `tests/unit/tst_audiotoolboxaacdecoder.cpp`→`tests/unit/tst_nativeaacdecoder.cpp`. Class `AudioToolboxAacDecoder`→`NativeAacDecoder` updated at its callers (`nativesrtingestsession.{h,cpp}`, `nativertmpingestsession.{h,cpp}`).
- **Deleted:** `recorder_engine/ingest/ffmpegingestsession.{h,cpp}`, `recorder_engine/ingest/nativefallbackpolicy.{h,cpp}`, `tests/unit/tst_srt_options.cpp`.
- **Modified:** `recorder_engine/ingest/ingestsession.{h,cpp}` (backend kinds, selector, env, remove augmentSrtUrl/fallback), `recorder_engine/ingest/nativesrtingestsession.{h,cpp}` (drop fallback + AAC rename), `recorder_engine/streamworker.{h,cpp}` (drop ffmpeg branch + Unsupported handling), `recorder_engine/ingest/nativertmpingestsession.{h,cpp}` (AAC rename; drop the `nativeFallbackReason` override if present), `CMakeLists.txt` + `tests/CMakeLists.txt` (source lists incl. the renamed AAC TUs + the new MF TU, drop deleted sources + `OLR_FFMPEG_SRT_PREFIX`), `tests/unit/tst_ingestbackendselector.cpp` + `tests/unit/CMakeLists.txt` (selector rewrite, AAC test rename, drop `tst_srt_options`), `.github/workflows/ci.yml` (sanitizer matrix `tst_audiotoolboxaacdecoder`→`tst_nativeaacdecoder`), `tests/e2e/run_record_e2e.sh` / `run_sync_e2e.sh` / `run_playback_e2e.sh` (SRT bridge), `tests/e2e/CMakeLists.txt` (delete `srt`-label gates, drop `OLR_NATIVE_SRT=1` env), `tests/e2e/SRT_README.md`.

## Success criteria
- The app constructs only `NativeSrtIngestSession` / `NativeRtmpIngestSession`; `FfmpegIngestSession` and `nativefallbackpolicy` are gone; an unsupported scheme is cleanly rejected with a logged reason and a disconnected source.
- SRT ingests natively with no `OLR_NATIVE_SRT`; `tst_ingestbackendselector` proves the native-only routing; no `Ffmpeg`/`augmentSrtUrl`/fallback symbols remain in code.
- Windows builds a Media Foundation AAC decoder (compile-checked in CI; live decode smoke-tested by the user); macOS AAC path unchanged.
- The migrated record/sync/playback e2e pass over native SRT; native-rtmp + native-apple-ingest stay green; the ffmpeg-SRT duplicate gates and `OLR_FFMPEG_SRT_PREFIX` are removed.
- FFmpeg remains linked for the muxer/encoder; only ingest no longer uses it.

## Out of scope / deferred
- A Windows test runner in CI (Windows decode verified manually).
- UI-level URL validation (engine-level rejection only).
- A native `file://` reader (dropped with udp://; tests use the SRT bridge).
- Linux native ingest (still stub; was never ffmpeg-ingest-free anyway).
