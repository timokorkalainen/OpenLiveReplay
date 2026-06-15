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

## Next (Phase 2)

A 4-source SRT framework (per-camera identity; inter-camera sync, per-source
trim, audio latency, connection-status, disconnect/loss injection) builds on this
infra — see `docs/superpowers/specs/`.
