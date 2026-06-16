# Local SRT end-to-end test

The standard e2e tests feed the engine over UDP MPEG-TS. This SRT test exercises
the app's primary transport (`srt://`) against the **real** recording engine.

It is **local-only** (not in CI): the homebrew ffmpeg CI uses has no libsrt, so
the SRT test needs a one-time libsrt-enabled ffmpeg build.

## One-time setup (~10 min)

```bash
brew install srt openssl@3            # if not already present
bash build-scripts/build_ffmpeg_macos_srt.sh
```
This builds a native `macos_build/ffmpeg-srt/` (gitignored). It is
`--enable-nonfree` (openssl), so it is **for local testing only — not
redistributable**.

## Build + run the SRT e2e

```bash
cmake -S . -B build/srt -G Ninja -DOLR_BUILD_TESTS=ON \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.1/macos" \
  -DOLR_FFMPEG_SRT_PREFIX="$PWD/macos_build/ffmpeg-srt"
ninja -C build/srt record_harness
( cd build/srt && ctest -L srt --output-on-failure )
```

`e2e_srt_smoke` stands up one `srt://` flash/beep stream (via `srt-live-transmit`)
and asserts the engine records a valid MKV **with real audio content** (the 1 kHz
tone, not the blue-fill/silence the engine emits when no source connects). Without
`-DOLR_FFMPEG_SRT_PREFIX` the engine's avformat lacks SRT and the test fails by
design.

`e2e_srt_4cam` goes further: it stands up **4** SRT cameras, each emitting a
distinct audio tone (1/2/3/4 kHz), records them into a 4-view MKV, and asserts
each recorded view carries its own camera's tone — proving 4 real SRT streams
connect and **route correctly** (view *i* = camera *i*), not blue-fill silence.
Run it with the same SRT build via `ctest -L srt`.

## Phase 2b: feature validation over real SRT

Three gates validate shipped features over real `srt://` ingest (same SRT build,
`ctest -L srt`). They build on `srt_lib.sh` (bridge + tee'd full-frame-flash
producer + the `flash_pts_series` extractor reused from `run_sync_e2e.sh`):

- `e2e_srt_sync` — one flash source tee'd to **4** coincident SRT views; asserts
  every view carries the live flash (≥4 flashes — a disconnected view is blue-fill
  with 0 and FAILS) and the per-flash inter-view spread stays within a **generous**
  bound (250 ms). The bound is generous by design: the engine anchors each source
  to first-packet **arrival** (no genlock, audit REF-2), so coincident SRT is
  phase-locked-within-bounds, not zero-skew (typical measured spread ≈ 30 ms).
  Teeth: `OLR_SRT_SYNC_DROP_VIEW=<i>`.
- `e2e_srt_trim` — flash source tee'd to **2** coincident SRT views; trims view1 by
  `T` (default 300 ms) and asserts the measured (view0−view1) flash offset shifts by
  ≈ **−T** (the trim delays view1). Proves per-source trim (#28) works over SRT.
- `e2e_srt_connect` — records 4 SRT URLs with `sync_harness --report-connections`
  and asserts `connected=4`; a second run with the 4th URL pointed at a dead port
  asserts `connected=3`. Proves connection-status (#24) detects and discriminates.
  This gate also guards a teardown-robustness fix: a closing SRT socket used to
  stall `stopRecording()` for minutes (SRT's 180 s default linger × the global
  `srt_close()` lock), so the dead-source run hung ~7 min; the engine now sets
  `linger=0` in the SRT URL query (it is receive-only) and the run completes in
  ~10 s. See the `fix(srt): set linger=0` commit.

The gate thresholds are validated against real local runs; if one proves flaky,
widen the bound (never delete the gate) — the content gates (per-view flash count,
connection count) carry the discrimination.

## Native SRT path

The native ingest path can be tested without an SRT-enabled FFmpeg build. It
uses the same SRT producers, but runs the engine with `OLR_NATIVE_SRT=1`, so
`srt://` input goes through:

```text
libsrt -> MPEG-TS parser -> H.264/H.265 access-unit splitter -> platform native decoder
```

Platform native decoders:

- Apple: `VideoToolbox`
- Windows: `Media Foundation/D3D11` (planned/when implemented)

The native SRT e2e scripts are shared across platforms. CTest registers the
same `e2e_native_srt_*` test names for each native platform and changes only the
label:

Shared native test names:

- `e2e_native_srt_smoke`
- `e2e_native_srt_4cam`
- `e2e_native_srt_sync`
- `e2e_native_srt_trim`
- `e2e_native_srt_connect`

Shared scripts:

- `run_srt_smoke.sh`
- `run_srt_4cam.sh`
- `run_srt_sync.sh`
- `run_srt_trim.sh`
- `run_srt_connect.sh`

Labels:

- macOS/iOS host builds: `native-apple-ingest`
- Windows host builds: `native-windows-ingest`

Both labels run with `OLR_NATIVE_SRT=1`. The producer side still uses local
`ffmpeg` and `srt-live-transmit`; the engine side must not require FFmpeg SRT
support for native ingest.

Configure against the normal Homebrew FFmpeg (no `OLR_FFMPEG_SRT_PREFIX`) and run
the `native-apple-ingest` label:

```bash
cmake -S . -B build/native-srt -G Ninja -DOLR_BUILD_TESTS=ON \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.1/macos"
ninja -C build/native-srt record_harness sync_harness
( cd build/native-srt && ctest -L native-apple-ingest --output-on-failure )
```

Windows uses the same shared scripts and `e2e_native_srt_*` test names under the
`native-windows-ingest` label. It requires local producer tools (`ffmpeg`,
`srt-live-transmit`, and a Bash-compatible shell) and runs:

```powershell
ctest --test-dir build/windows-native -C Debug -L native-windows-ingest --output-on-failure
```

This intentionally proves the native SRT path is carrying the stream: the default
Homebrew FFmpeg used by the harness does not provide `srt://`, so a fallback to
FFmpeg records blue-fill/silence and fails the content checks.

## Next (Phase 2c)

Disconnect/reconnect mid-recording, packet-loss / jitter injection, reconnect
re-anchoring, and long-run drift over SRT — each its own spec under
`docs/superpowers/specs/`.
