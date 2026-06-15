# Native SRT VideoToolbox Ingest Design

## Goal

Add an iOS ingest path for SRT feeds that does not use FFmpeg for network receive, MPEG-TS demuxing, H.264 parsing, HEVC parsing, or H.264/HEVC decoding. The native path will receive SRT directly through libsrt, demux MPEG-TS in app code, feed compressed H.264/H.265 access units into Apple VideoToolbox, and hand decoded frames to the existing recorder pipeline.

The current FFmpeg ingest path remains for RTMP/RTMPS H.264 and as a non-iOS fallback during the transition. FFmpeg HEVC support will be removed from the iOS build.

## Non-Goals

- No native RTMP/RTMPS implementation in this phase.
- No FFmpeg HEVC decoder, parser, hwaccel, or encoder in the iOS build.
- No software HEVC decoder.
- No attempt to support arbitrary MPEG-TS variants. The first target is live SRT MPEG-TS from cameras or encoders with one H.264 or H.265 video stream and optional AAC audio.
- No claim that Apple-only HEVC decode eliminates every possible patent-license question. The design avoids shipping an HEVC decoder implementation, but legal risk must still be evaluated separately.

## Architecture

`StreamWorker` should stop owning all ingest details directly. It should depend on an ingest abstraction that emits decoded video frames and decoded audio samples on the app recording timeline.

Initial backends:

- `FfmpegIngestSession`: wraps the current FFmpeg path. It remains responsible for RTMP/RTMPS H.264 and non-iOS fallback behavior.
- `NativeSrtIngestSession`: iOS-only SRT path. It uses libsrt for receive, a narrow MPEG-TS demuxer for stream extraction, a codec packetizer for H.264/H.265 access units, and VideoToolbox for decode.

The shared `StreamWorker` behavior should stay common:

- source switching and stop/restart control;
- connection state signals;
- stall detection;
- jitter queue;
- view-track assignment;
- metadata writing;
- existing MPEG-2 encode and mux output.

## Data Flow

Native SRT path:

1. Parse the source URL and SRT options.
2. Open a libsrt socket directly.
3. Read 188-byte MPEG-TS packets from SRT.
4. Parse PAT and PMT to identify video and audio PIDs.
5. Reassemble PES packets and extract PTS/DTS.
6. For video:
   - detect H.264 or H.265 from PMT stream type;
   - split access units;
   - collect SPS/PPS for H.264 and VPS/SPS/PPS for H.265;
   - create or recreate a VideoToolbox decompression session when parameter sets change;
   - submit compressed access units as `CMSampleBuffer`s;
   - receive `CVPixelBufferRef` frames.
7. Convert decoded frames into the format expected by the existing recorder path. The first implementation can copy into `AV_PIX_FMT_YUV420P` frames so the current MPEG-2 encoder path keeps working.
8. For audio:
   - phase 1 may ship native SRT as video-only if that is the fastest safe milestone.
   - production parity should decode AAC with AudioToolbox and emit the same 48 kHz stereo S16 samples used by `StreamWorker` today.
   - native SRT should not call FFmpeg for audio before decoded video frames reach the recorder path.

## Build Configuration

The iOS FFmpeg build should keep:

- SRT/RTMP/RTMPS protocol support for the FFmpeg fallback path;
- H.264 decoder/parser/hwaccel for RTMP H.264 and fallback behavior;
- MPEG-2 encoder for recording;
- AAC audio decode if the FFmpeg path remains responsible for RTMP audio.

The iOS FFmpeg build should remove:

- `--enable-decoder=hevc`
- `--enable-parser=hevc`
- `--enable-hwaccel=hevc_videotoolbox`
- `--enable-encoder=hevc_videotoolbox`

The smoke test should assert that HEVC is absent from the FFmpeg iOS build script while H.264 and MPEG-2 remain present.

## Backend Selection

On iOS:

- `srt://` uses `NativeSrtIngestSession` by default.
- `rtmp://` and `rtmps://` use `FfmpegIngestSession`.
- A developer escape hatch can force FFmpeg SRT during early rollout, but the release default should be native SRT.

On macOS and test builds:

- The existing FFmpeg path can remain the default until the native SRT backend is portable or explicitly tested on macOS.

## Error Handling

Native SRT should fail fast and clearly when a stream is outside the supported profile:

- no PAT/PMT within the probe window;
- unsupported PMT stream type;
- multiple video streams;
- missing PTS/DTS;
- missing H.264 SPS/PPS or HEVC VPS/SPS/PPS;
- VideoToolbox session creation failure;
- VideoToolbox decode failure;
- SRT timeout or disconnect.

Recoverable events should trigger reconnect or session recreation:

- SRT read timeout;
- MPEG-TS continuity discontinuity;
- PTS/DTS jump or wrap;
- H.264/H.265 parameter-set change;
- VideoToolbox decoder reset.

All failures should include source index, URL scheme, backend name, and concise reason in logs.

## Testing

Unit tests:

- MPEG-TS PAT/PMT parser with H.264, HEVC, AAC, unknown stream types, and malformed packets.
- PES reassembly with split packets, continuity counter gaps, PTS/DTS extraction, and discontinuity indicators.
- H.264 access-unit splitter and SPS/PPS extraction.
- HEVC access-unit splitter and VPS/SPS/PPS extraction.
- Backend selector: SRT maps to native on iOS; RTMP/RTMPS maps to FFmpeg.
- iOS FFmpeg build-config smoke test rejects HEVC.

Integration tests:

- Synthetic SRT MPEG-TS H.264 feed decodes through VideoToolbox on iOS.
- Synthetic SRT MPEG-TS HEVC feed decodes through VideoToolbox on iOS.
- RTMP H.264 continues through FFmpeg.
- Source change, reconnect, and empty-source blue-paint behavior remain intact.

Manual/device verification:

- Real iPhone/iPad device test with H.264 SRT.
- Real iPhone/iPad device test with HEVC SRT.
- Long-running soak with packet loss/reconnect.
- Confirm the built iOS binary does not contain FFmpeg HEVC decoder/parser symbols.

## Rollout

Phase 1: refactor `StreamWorker` behind an ingest-session interface while preserving existing behavior.

Phase 2: add native SRT transport and MPEG-TS demux unit-tested in isolation.

Phase 3: add VideoToolbox H.264 decode for native SRT and compare output timing against the FFmpeg path.

Phase 4: add VideoToolbox HEVC decode and remove FFmpeg HEVC from iOS build.

Phase 5: enable native SRT by default on iOS, keep FFmpeg fallback behind a developer option during initial stabilization.

Phase 6: evaluate native RTMP/RTMPS as a separate project after native SRT is stable.

## Risks

This is a significant media-pipeline change. The SRT MPEG-TS scope is bounded, but MPEG-TS timestamp behavior, malformed feeds, access-unit splitting, and decoder-session resets are easy places to introduce subtle recording glitches.

The largest risk is trying to match FFmpeg's tolerance for arbitrary live streams. The design avoids that by supporting a narrow, documented SRT MPEG-TS profile first and keeping FFmpeg for RTMP/H.264.

Audio is the second largest scope risk. A no-audio native SRT milestone can prove the video path quickly, but production parity needs AudioToolbox AAC decode so native SRT keeps the no-FFmpeg-before-decoded-frames boundary.
