# Codec Selection + Device Benchmark — Design

**Date:** 2026-06-18
**Status:** Design — pending review

## Summary

Add a user-selectable recording video codec — **H.264 (hardware)** or **MPEG-2 (software)** — and a
**device benchmark** that measures how many concurrent feeds each codec can sustain in real time, then
recommends a codec and a safe feed count. The goal is to move the heavy encode (and playback decode)
leg off the CPU on capable devices while keeping the existing MPEG-2 software path as a universal
fallback.

The recorder is a live-TV-style replay system, so **frame-accurate seeking is non-negotiable**: both
codecs are recorded **intra-only** (every frame a keyframe), exactly as the current MPEG-2 path already
does (`gop_size = 1`).

## Goals

- Let the user choose the recording codec; persist the choice.
- Offer H.264 **only when the OS provides hardware encode/decode**; otherwise it is unavailable.
- Benchmark **both** codecs on the actual device and recommend codec + safe feed count.
- Keep the existing recording/playback/muxer pipeline, clock, jitter buffer, and metadata/telemetry
  tracks unchanged.

## Non-Goals

- **MXF export is out of scope** (dropped). H.264 is plain 4:2:0 8-bit in the existing Matroska
  container, purely for lower CPU + better compression. No interchange/conformance constraints.
- No zero-copy GPU transcode in v1 (the existing decoder already copies decoded frames to CPU
  `AVFrame`; encoders consume CPU frames). Possible future optimization.
- No change to ingest decode (already hardware via `NativeVideoDecoder`).

## Hard Constraints

- **H.264 = hardware only, encode *and* decode, no software path ever.** Rationale: licensing —
  the OS frameworks (VideoToolbox / MediaFoundation) hold the codec license. We do not link or run a
  software H.264 codec. The FFmpeg build is configured `--disable-encoder=libx264
  --disable-decoder=h264` so no software H.264 path can be taken even accidentally.
- **MPEG-2 = software only** (FFmpeg `mpeg2video`), exactly as today.
- **All-intra** for both codecs (PTS == DTS; no B-frame reordering).
- Target platforms: **macOS, iOS, Windows** in v1.

## Architecture

### Approach: fully native H.264, reuse the existing decoder

H.264 encode and decode go entirely through OS-native frameworks; FFmpeg is used only for MPEG-2 and
for container mux/demux.

- **Encode (new):** a `NativeVideoEncoder` abstraction with VideoToolbox (mac + iOS) and
  MediaFoundation (Windows) backends, mirroring the existing `NativeVideoDecoder`.
- **Decode (reuse):** the existing `NativeVideoDecoder` already does hardware H.264/HEVC on all three
  platforms and outputs CPU `AVFrame` (YUV420P). Playback feeds it the file's H.264 packets +
  parameter sets.
- **MPEG-2:** untouched FFmpeg software encode/decode.

Why native rather than FFmpeg's HW wrappers (`h264_videotoolbox` / `h264_mf`): airtight licensing
(no FFmpeg H.264 code in the path), no dependency on FFmpeg build flags including those encoders
(notably the iOS build), and codebase symmetry with the already-native decoder.

### New module layout

```
recorder_engine/codec/
  nativevideoencoder.h                    # abstract interface (below)
  nativevideoencoder_videotoolbox.mm      # VTCompressionSession (macOS + iOS)
  nativevideoencoder_mediafoundation.cpp  # H.264 encoder MFT (Windows)
  nativevideoencoder_stub.cpp             # unsupported -> create() returns nullptr
  videoencodecaps.{h,cpp}                 # queryNativeVideoEncodeCapabilities()

recorder_engine/benchmark/
  codecbenchmark.{h,cpp}                  # ramp + sustain measurement, worker-threaded
```

### Encoder interface

```cpp
enum class VideoCodecChoice { Mpeg2Software, H264Hardware };

class NativeVideoEncoder {
public:
    struct Config { int width, height, fpsNum, fpsDen, bitrate; };
    // Returns nullptr if hardware H.264 encode cannot be opened.
    static std::unique_ptr<NativeVideoEncoder> create(const Config&, QString* error);
    // All-intra, synchronous drain: encode one CPU YUV420P frame, emit AVPacket(s).
    bool encode(const AVFrame* frame, int64_t ptsMs,
                const std::function<void(AVPacket*)>& onPacket, QString* error);
    bool flush(const std::function<void(AVPacket*)>& onPacket, QString* error);
    QByteArray avccExtradata() const;  // SPS/PPS as avcC; valid after the priming encode
};
```

Configuration: VideoToolbox `kVTCompressionPropertyKey_MaxKeyFrameInterval = 1` +
`AllowFrameReordering = false` + real-time + bitrate; MediaFoundation equivalent. VideoToolbox emits
length-prefixed (AVCC) NALs (what MKV wants); MediaFoundation Annex B is converted to length-prefixed.

Bitrate: the H.264 `Config.bitrate` default mirrors the current MPEG-2 setting (30 Mbit/s, per
`streamworker.cpp:515` / `muxer.cpp:49`) so quality is at least on par; tunable later. All-intra H.264
at the same bitrate is visually superior to MPEG-2 and produces smaller files.

### Threading the codec choice end to end

- `AppSettings::videoCodec` (persisted `"mpeg2"` / `"h264"`), pushed
  `UIManager -> ReplayManager::setVideoCodec()` alongside the existing width/height/fps setters
  (`uimanager.cpp:2093-2094`).
- `ReplayManager::startRecording()` (`replaymanager.cpp:169`): for H.264, perform **one priming
  encode** (the blue frame is already allocated) to obtain `avcC` before the muxer header is written.
  All video streams share w/h/fps/profile, so SPS/PPS are identical and the single `avcC` applies to
  every video stream.
- `Muxer::init()` gains `VideoCodecChoice codec` + `QByteArray extradata`. The hardcoded
  `AV_CODEC_ID_MPEG2VIDEO` (`muxer.cpp:44`) becomes conditional: `AV_CODEC_ID_H264` with
  `codecpar->extradata` set **before** `avformat_write_header` (`muxer.cpp:139`).
- `StreamWorker::setupEncoder()` (`streamworker.cpp:467`) branches: MPEG-2 -> existing FFmpeg
  `AVCodecContext`; H.264 -> its own `NativeVideoEncoder`. The tick at `streamworker.cpp:211`
  (`avcodec_send_frame`) becomes `encoder->encode(frame, ..., onPacket = muxer->writePacket)`.
- The blue-frame encoder in `ReplayManager` (`replaymanager.cpp:89`) uses the same branch.
- **Playback:** `PlaybackWorker` keeps FFmpeg demux; the decode call sites
  (`playbackworker.cpp:682`, `:719`) branch on the file stream's `codec_id`: `AV_CODEC_ID_H264` ->
  existing `NativeVideoDecoder` (payload + SPS/PPS from `codecpar->extradata`); `AV_CODEC_ID_MPEG2VIDEO`
  -> existing FFmpeg software decode.

### Risk areas (designed for explicitly)

1. **Parameter-set -> `avcC` extradata** is the riskiest bridge (a wrong byte = unplayable file).
   Handled by the priming-encode-before-header sequence and covered by a round-trip test.
2. **Bitstream packaging** — AVCC vs Annex B per backend; wrap bytes + pts/dts + keyframe flag into
   `AVPacket`.
3. **Async -> tick model** — VTCompressionSession is callback-async; wrapped synchronously
   (encode -> CompleteFrames -> collect) so the StreamWorker tick is a clean swap. All-intra
   (PTS == DTS) removes reordering/DTS complexity.

## Benchmark Engine

### Purpose

Probe HW availability, then measure the largest concurrent feed count each codec sustains in real
time, and recommend codec + safe feed count.

### Unit of work

Ingest decode is already hardware and constant, so it is excluded. The codec choice controls two legs:
encode (recording) and decode (multiview/playback). Worst case = recording all N feeds while a
multiview decodes all N. One **pipeline unit = encode one frame + decode one frame** per frame
interval.

### Method (per codec)

1. **Synthetic source:** procedurally and **deterministically** generate a handful of YUV420P frames
   with real spatial detail + per-frame motion (not a flat frame, which would encode unrealistically
   fast). Cycled as input.
2. **Probe:** try to `create()` encoder + decoder at the target resolution. If H.264 cannot open ->
   `h264Available = false`, skip only its ramp. MPEG-2 is always measured.
3. **Concurrency ramp:** N walks `1, 2, 4, 8, 12, 16, 20, 24, 28, 32`. For each N, spin up N threads
   (matching the per-source worker model), each looping encode -> feed packet -> decode at the target
   fps cadence for a fixed window (~3-5 s). Record achieved vs required throughput and per-frame
   latency.
4. **Stop on first failure:** the moment a step misses the sustain criterion, halt the ramp for that
   codec — higher N would also fail. The last passing step is the measured ceiling.
5. **Sustain criterion:** N passes if processed frame-pairs >= `N * fps * duration * 0.95` and no
   frame blew its interval budget.
6. **Safe count:** derived from the ceiling with headroom (largest passing N that held with >= 1.2x
   margin).
7. Run for H.264 (if available) and MPEG-2, at the resolution/fps from `AppSettings`.

### Threading

`CodecBenchmark` owns its threads: a controller thread orchestrates the ramp; N pipeline threads do
all codec work, allocation, and timing. The GUI thread only fires the async start, receives
**coalesced** progress (one queued update per ramp step, never per frame), and receives the final
result. No codec work, allocation, or measurement runs on the GUI thread.

### Result (cached, device-keyed)

```cpp
struct CodecBenchmarkResult {
    bool h264Available;
    int  h264SafeFeeds, mpeg2SafeFeeds;          // -1 if unavailable
    double h264EncodeMs, h264DecodeMs,
           mpeg2EncodeMs, mpeg2DecodeMs;          // per-frame at the safe count
    VideoCodecChoice recommended;
    QString deviceLabel, resolution, timestamp;   // deviceLabel/res/fps key the cache
};
```

Recommendation: H.264 whenever available and `h264SafeFeeds >= mpeg2SafeFeeds` (it also frees the
CPU/thermal budget, so it wins on ties); otherwise MPEG-2.

### Safety

Whole run time-boxed (target < 60 s; early-stop keeps it well inside). N capped at 32 with an explicit
`log()` if the ceiling is reached (no silent truncation). Result cached to a small JSON beside
settings, re-runnable on demand, invalidated on device/resolution/fps change.

## Settings / UI

- **Codec selector** in app settings, persisted to `AppSettings::videoCodec`:
  - **MPEG-2 (software)** — always enabled.
  - **H.264 (hardware)** — enabled iff `queryNativeVideoEncodeCapabilities()` reports it; otherwise
    disabled with a reason ("No hardware H.264 encoder on this device"). Probe runs once at startup,
    off the GUI thread, cached.
  - Selection is independent of the recommendation; the user may pick either available codec.
- **Benchmark panel:** "Run benchmark" -> async `CodecBenchmark`, progress bar from coalesced ramp
  updates, then a results table (per codec: safe feeds + avg encode/decode ms) and an advisory line
  ("Recommended: H.264 — 12 feeds on this device"). Last cached result shown when present.
- **Gating:**
  1. **Hard block (availability):** at `startRecording()`, re-probe. If H.264 is selected but HW
     won't open, refuse to start with an actionable message offering MPEG-2 — never a silent fallback,
     never a silent broken start. (Surfaces via the existing failure path at `uimanager.cpp:1617`.)
  2. **Soft warning (capacity):** if the configured source/view count exceeds the benchmarked safe
     feed count for the chosen codec, show a non-blocking warning before recording. Guidance, not a
     block.
- **Persistence:** `videoCodec` in the settings JSON via `SettingsManager`; benchmark result cached in
  a separate small JSON keyed by device + resolution + fps.

## Testing Strategy

Test-first, layered from the riskiest unit outward, reusing the existing headless E2E recording
harness.

1. **Native encoder unit tests (per platform, HW-gated):** open at 1080p, encode the deterministic
   synthetic frames, assert valid `AVPacket`s, **every packet a keyframe** (all-intra), well-formed
   `avcC`. mac runner (VideoToolbox) + Windows runner (MediaFoundation). Where no HW exists, assert
   `create()` returns `nullptr` gracefully — never a software fallback.
2. **avcC bridging round-trip:** prime -> assert `avcC` parses (SPS/PPS structure, profile/level) ->
   encode N frames -> mux to MKV -> demux + decode via both `NativeVideoDecoder` and `ffprobe`;
   confirm frame count, dimensions, all-intra, PSNR within tolerance vs source.
3. **Codec round-trip E2E:** extend the harness to record a short session per codec; verify output
   MKV `codec_id`, intra-only, **track layout intact** (video + per-view audio + metadata + telemetry
   text tracks), playable, frame count.
4. **Playback frame-accuracy:** record H.264, drive `PlaybackWorker`, seek to frame K, assert the
   decoded frame is K's content.
5. **Benchmark engine (deterministic, no real HW):** inject a fake codec with programmable per-frame
   cost; assert the ramp visits `1,2,4,8,12,16,20,24,28,32` **and stops at the first failing step**,
   the 32 cap is logged, the safe-feed math matches expected costs, cancellation works mid-ramp, and
   no codec work runs on the GUI thread (assert worker-thread affinity).
6. **Gating + persistence:** H.264-selected-but-unavailable -> `startRecording` refuses with the
   actionable message; feeds > safe -> soft warning; MPEG-2 always available; `videoCodec` round-trips
   through `SettingsManager`; benchmark cache saves/loads and invalidates on device/resolution change.

**CI notes:** native-encoder tests are platform-gated (skip-with-assert where HW absent). iOS shares
the VideoToolbox backend exercised on macOS, so its native path is covered by code-sharing; on-device
iOS validation stays manual (consistent with the existing `SKIP_IOS_BUILD` arrangement). Synthetic
frames are generated deterministically so every test and benchmark run is reproducible.

## Open Questions

None blocking. Build-system wiring for the new `recorder_engine/codec/` and `recorder_engine/benchmark/`
sources (CMake, per-platform source selection like the existing native decoder backends) is mechanical
and handled in the implementation plan.
