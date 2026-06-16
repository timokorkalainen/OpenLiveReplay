# Native SRT Windows Hardware Decode Design

## Goal

Add native Windows hardware decoding for SRT MPEG-TS H.264 and H.265 ingest, matching the Apple VideoToolbox path's role in the current pipeline. The Windows path should receive SRT through libsrt, reuse the existing MPEG-TS parser and H.26x access-unit splitter, decode video with Windows native media APIs, and emit CPU `AVFrame` objects compatible with the recorder path.

The first production target is Windows 10/11 desktop PCs with Media Foundation available. FFmpeg remains the fallback for unsupported URLs, unsupported stream profiles, missing codecs, failed hardware setup, or developer opt-out.

## Research Summary

The recommended implementation is Media Foundation decoder MFTs with a D3D11/DXGI device manager.

Microsoft documents the H.264 video decoder as a Media Foundation Transform supporting Baseline, Main, and High profiles up to level 5.1. It accepts Annex B H.264 bitstreams and can output common YUV formats such as `NV12`, `I420`, and `YV12`.

Microsoft documents the H.265/HEVC video decoder as a Media Foundation Transform supporting Annex B HEVC, Main and Main10 profiles, and `NV12`/`P010` output. HEVC availability must be treated as a runtime capability because some Windows installs require optional media features or the Microsoft HEVC extension.

Media Foundation's D3D11 decode path is wired through `IMFDXGIDeviceManager`, passed to transforms with `MFT_MESSAGE_SET_D3D_MANAGER`. This is the closest Windows equivalent to the current Apple VideoToolbox design: vendor-neutral, OS-integrated, and compatible with Intel, NVIDIA, and AMD drivers.

Primary references:

- Microsoft H.264 decoder: https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-decoder
- Microsoft H.265/HEVC decoder: https://learn.microsoft.com/en-us/windows/win32/medfound/h-265---hevc-video-decoder
- Media Foundation D3D11 decode support: https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation
- Media Foundation transform enumeration: https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mftenumex
- Media Foundation MFT flags: https://learn.microsoft.com/en-us/windows/win32/api/mfapi/ne-mfapi-_mft_enum_flag

## Non-Goals

- No direct D3D11/D3D12 bitstream decoder implementation in this phase.
- No vendor-specific decoder SDKs in this phase, including NVDEC, Intel VPL, or AMD AMF.
- No replacement of the recorder's CPU `AVFrame` contract yet.
- No GPU-zero-copy recorder path in this phase.
- No promise that every Windows PC can decode HEVC natively. The app must probe and fall back when HEVC is absent or unusable.
- No native RTMP/RTMPS work.

## Architecture

The current class named `VideoToolboxDecoder` should become a platform-neutral native video decoder boundary.

Proposed files:

- `recorder_engine/ingest/nativevideodecoder.h`
- `recorder_engine/ingest/nativevideodecoder_videotoolbox.mm`
- `recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp`
- `recorder_engine/ingest/nativevideodecoder_stub.cpp`

The public decoder API should stay close to the existing shape:

- construct with output width and height;
- accept a `CompressedAccessUnit`;
- invoke a callback with decoded `AVFrame*`;
- expose `reset()`;
- expose a capability probe for H.264 and HEVC.

`NativeSrtIngestSession` should depend on `NativeVideoDecoder`, not an Apple-named class. Its video data flow remains unchanged:

1. Receive bytes from libsrt.
2. Reassemble aligned 188-byte MPEG-TS packets.
3. Parse PAT/PMT and PES.
4. Split H.264/H.265 Annex B access units.
5. Decode with the platform-native decoder.
6. Emit `DecodedVideoFrame` with source timeline PTS.

## Windows Decoder Design

`NativeVideoDecoder` on Windows should use Media Foundation directly through `IMFTransform`, not the Source Reader. The app already has demuxed compressed access units, so the decoder should feed samples with `ProcessInput` and drain frames with `ProcessOutput`.

Initialization:

- call `MFStartup` through a small process-wide reference guard;
- create a D3D11 device with video support;
- create an `IMFDXGIDeviceManager`;
- enumerate or instantiate the Microsoft H.264 or HEVC decoder MFT;
- send `MFT_MESSAGE_SET_D3D_MANAGER` with the DXGI device manager;
- configure input media type as H.264 or HEVC Annex B;
- configure output type, preferring `NV12` for hardware decode.

Decode:

- wrap each `CompressedAccessUnit::annexB` in an `IMFSample` and `IMFMediaBuffer`;
- set sample time and duration when available from 90 kHz PTS/DTS;
- call `ProcessInput`;
- drain `ProcessOutput` until `MF_E_TRANSFORM_NEED_MORE_INPUT`;
- copy decoded `NV12` or `I420` output into an `AV_PIX_FMT_YUV420P` `AVFrame`;
- set `frame->pts` to the source access-unit PTS in 90 kHz units, matching the current VideoToolbox behavior.

Session reset:

- recreate the MFT when codec changes;
- recreate the MFT when parameter sets change;
- flush and recreate on stream discontinuity or unrecoverable transform error.

## Capability Probing

Add a native capability helper so backend selection can be explicit and testable.

Proposed API:

```cpp
struct NativeVideoDecodeCapabilities {
    bool h264 = false;
    bool hevc = false;
    bool d3d11 = false;
    QString detail;
};

NativeVideoDecodeCapabilities queryNativeVideoDecodeCapabilities();
```

On Windows, the probe should:

- initialize Media Foundation;
- attempt to create a D3D11 device and DXGI device manager;
- check H.264 decoder availability;
- check HEVC decoder availability;
- report whether D3D11 setup succeeded.

The probe should not require an SRT stream. It should be usable by tests, diagnostics, and future UI status.

## Native SRT Portability

`NativeSrtIngestSession` currently includes POSIX networking headers for IPv4 parsing. Windows should keep libsrt as the transport, but isolate OS socket details.

Add a small helper for numeric IPv4 conversion:

- Apple/Linux implementation uses `inet_pton` from POSIX headers.
- Windows implementation includes Winsock headers and links `ws2_32`.

Avoid spreading `#ifdef _WIN32` through `NativeSrtIngestSession`. Keep platform shims narrow and testable.

## Backend Selection

Keep rollout conservative:

- `OLR_NATIVE_SRT=1` remains required for native SRT on desktop.
- `srt://` is eligible for native SRT only when `NativeSrtIngestSession::supportsUrl(url)` passes and the platform native decoder reports support for at least H.264.
- default behavior without `OLR_NATIVE_SRT=1` remains unchanged.

Because the actual stream codec is only known after MPEG-TS PAT/PMT parsing, fallback needs two levels:

- before opening, `StreamWorker` chooses native SRT only for eligible URLs and platforms;
- during native ingest, unsupported codec, missing HEVC support, Media Foundation setup failure, or unrecoverable decode error marks the native attempt as failed with a reason.

After a native attempt fails for one of those decode-capability reasons, the next retry for that source should use `FfmpegIngestSession` until the source URL changes or capture restarts. This prevents an HEVC stream on a machine without HEVC support from looping forever through the native path.

## Build Configuration

On Windows:

- compile `nativevideodecoder_mediafoundation.cpp`;
- compile `nativesrtingestsession.cpp`;
- define `OLR_NATIVE_SRT_AVAILABLE=1` when libsrt and Media Foundation linkage are present;
- link Media Foundation, D3D11/DXGI, and Winsock libraries:
  - `mfplat`
  - `mf`
  - `mfuuid`
  - `d3d11`
  - `dxgi`
  - `ole32`
  - `ws2_32`
  - `srt` when available.

CMake should not assume Homebrew-style FFmpeg paths on Windows. Windows dependency discovery should be explicit through cache variables or package-manager integration already used by the developer environment.

## Error Handling

Native Windows decode should log concise, actionable reasons:

- Media Foundation startup failed;
- D3D11 device creation failed;
- H.264 decoder unavailable;
- HEVC decoder unavailable;
- HEVC decoder present but media type setup failed;
- transform needs a format change;
- output copy failed;
- decoder returned an unrecoverable HRESULT.

HEVC errors should mention that Windows HEVC availability depends on system media components or extensions. Logs should avoid implying the app itself bundles an HEVC decoder.

Recoverable events:

- stream parameter-set change;
- DTS/PTS discontinuity;
- transform stream-change notification;
- SRT stall or reconnect.

Unrecoverable decoder errors should reset the native decoder. If the next reconnect still fails, the session should fall back to FFmpeg where the backend selector allows it.

## Testing

Unit tests:

- native backend selector remains opt-in for SRT;
- `NativeVideoDecoder` stub reports no support on unsupported platforms;
- Windows capability probe returns a structured result without requiring an SRT stream;
- parameter-set change resets the native decoder session;
- NV12-to-YUV420P copy handles even frame dimensions and stride padding.

Integration tests:

- native Windows SRT H.264 smoke, reusing `run_srt_smoke.sh`;
- native Windows SRT H.264 4-camera routing, reusing `run_srt_4cam.sh`;
- native Windows SRT H.264 inter-camera sync, reusing `run_srt_sync.sh`;
- native Windows SRT H.264 per-source trim, reusing `run_srt_trim.sh`;
- native Windows SRT H.264 connection-status/dead-port behavior, reusing `run_srt_connect.sh`;
- native Windows HEVC smoke when HEVC capability is present;
- HEVC absent test should skip or assert graceful fallback, not fail the suite.

The Windows native SRT label should match the Apple native SRT parity set where
the existing scripts apply:

```text
e2e_native_windows_srt_smoke
e2e_native_windows_srt_4cam
e2e_native_windows_srt_sync
e2e_native_windows_srt_trim
e2e_native_windows_srt_connect
```

These tests should run with `OLR_NATIVE_SRT=1` and should not require an
SRT-enabled FFmpeg build for ingest. FFmpeg and `srt-live-transmit` are still
allowed as local test producers. As with the Apple native label, fallback to
FFmpeg ingest should fail the content checks when the build is configured
without FFmpeg SRT support.

HEVC gets separate coverage because the current SRT e2e scripts generate H.264
with `libx264`. Add an HEVC producer variant only after the H.264 parity label is
green. The HEVC test should be capability-gated so machines without the Windows
HEVC decoder report a skip or a successful fallback assertion instead of a hard
failure.

Manual verification:

- Windows 11 Intel integrated GPU;
- Windows 11 NVIDIA GPU;
- Windows 10 or 11 AMD GPU if available;
- HEVC installed and HEVC absent cases;
- long-running SRT reconnect/stall soak.

Existing macOS native SRT tests should keep passing after the decoder rename.

## Rollout

Phase 1: Rename the native decoder boundary from `VideoToolboxDecoder` to `NativeVideoDecoder` while preserving Apple behavior.

Phase 2: Isolate native SRT socket portability and make `NativeSrtIngestSession` build on Windows.

Phase 3: Add Windows Media Foundation capability probe and stub tests.

Phase 4: Add H.264 decode with Media Foundation and D3D11, copying output to `AV_PIX_FMT_YUV420P`.

Phase 5: Add HEVC decode with runtime capability handling and clear fallback behavior.

Phase 6: Add Windows native SRT e2e tests and documentation.

Phase 7: Evaluate GPU-zero-copy recording or vendor-specific accelerated paths only if measured decode-copy overhead becomes a real bottleneck.

## Risks

HEVC availability varies across Windows installations. The design treats this as a capability probe and fallback concern rather than a hard requirement.

Media Foundation MFT behavior can vary by driver and Windows build. The first implementation should keep the output contract simple and copy to CPU frames, even though that leaves some performance on the table.

Direct MFT decode has more setup ceremony than VideoToolbox. Keeping the implementation behind a small `NativeVideoDecoder` boundary limits the complexity seen by `NativeSrtIngestSession`.

The current test CMake is macOS-centric. Windows test/build support may need dependency-discovery work before full e2e validation is pleasant.
