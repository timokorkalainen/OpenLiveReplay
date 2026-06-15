# SRT Test Infrastructure (Phase 1) — Design

**Status:** approved (brainstorm) → spec
**Date:** 2026-06-15
**Branch:** `feat/srt-test-infra`

## 1. Motivation

Our e2e tests feed the engine over **UDP MPEG-TS**, but the app's primary
broadcast transport is **SRT**, and nothing in CI or the dev loop exercises the
`scheme == "srt"` path in `StreamWorker::setupDecoder` (the SRT latency /
`transtype` / reconnect config at streamworker.cpp:794-805). Worse, the
homebrew ffmpeg 8.1.1 on this machine — which the test engine **and** the desktop
app link — is built **without libsrt** (`srt` absent from `-protocols`;
`libavformat.dylib` does not link `libsrt`), so the engine currently *cannot
ingest `srt://` at all* locally.

The project already builds **ffmpeg 8.0 + libsrt 1.5.5 + openssl** from source for
iOS (`build-scripts/build_ffmpeg_ios_srt.sh`), and `libsrt 1.5.4` + `openssl@3`
are brew-installed. So the fix is tractable: a **native macOS** ffmpeg+SRT build
the test engine can link, plus a one-stream proof that the engine records real
SRT end to end.

This is **Phase 1 (infra)** of a two-phase effort. **Phase 2** — a 4-source SRT
e2e framework (per-camera identity; inter-camera sync, per-source trim, audio
latency, connection-status, disconnect/loss validation) — gets its own spec,
built on this. Phase 1 de-risks Phase 2 by proving the transport works.

## 2. Decisions (from brainstorm)

- **SRT infra first**, framework second (this spec is the infra).
- **Local-run only.** The SRT e2e is a developer/validation tool, not a CI gate:
  CI keeps the fast UDP smoke gates (no CI ffmpeg-SRT build). Real SRT timing is
  inherently local; a from-source ffmpeg build per CI run is not worth it now.
- **Native build against the brew libsrt/openssl** (lightest of three options).
  *Rejected:* the `homebrew-ffmpeg` tap (would mutate the global ffmpeg the UDP
  tests rely on) and building srt/openssl from source too (redundant — brew has
  them).

## 3. Components

### 3.1 `build-scripts/build_ffmpeg_macos_srt.sh`

A **native** (no cross-compile) ffmpeg+SRT build, modeled on the iOS script but
much smaller (it links the already-built brew `libsrt`/`openssl@3` instead of
building them).

- **Source:** extract `ios_build/src/ffmpeg-8.0.tar.bz2` **fresh** into a separate
  native build tree (`macos_build/src/ffmpeg-8.0`), so the iOS cross-compiled
  object files are never reused and the native configure starts clean. (Re-extract
  only if the native source dir is absent.)
- **Configure** (native arm64, drops the iOS cross-compile/videotoolbox flags):
  ```
  PKG_CONFIG_PATH=/opt/homebrew/opt/srt/lib/pkgconfig:/opt/homebrew/opt/openssl@3/lib/pkgconfig \
  ./configure --prefix="$DIST" \
    --enable-gpl --enable-version3 --enable-nonfree \
    --disable-static --enable-shared --disable-doc --disable-programs \
    --disable-avdevice --disable-indevs --disable-outdevs \
    --enable-libsrt --enable-openssl --enable-protocol=libsrt
  ```
- **Output:** `macos_build/ffmpeg-srt/` (gitignored) with `include/` + `lib/`
  (`libavformat/avcodec/avutil/swscale/swresample/avfilter` dylibs, SRT-enabled).
- **Install names:** set each dylib `-id` and inter-lib deps to `@rpath/...`
  (reuse the iOS script's `install_name_tool` fix-up), so a consumer with the
  right rpath loads them without absolute build paths.
- **Idempotent:** skip the ~10-min rebuild if `macos_build/ffmpeg-srt/lib/libavformat.dylib`
  already exists (matches the iOS script's only-when-missing guard).
- **Verifies itself:** at the end, assert the built libs expose SRT — e.g.
  `nm -gU lib/libavformat*.dylib | grep -qi srt` or a tiny check that
  `libavformat` links `libsrt` (`otool -L`), failing loudly if libsrt didn't link.

`macos_build/` is added to `.gitignore` (alongside the existing `ios_build/`).

### 3.2 CMake opt-in linkage (`tests/CMakeLists.txt`)

Add an option that overrides the ffmpeg include/lib dirs for the test engine +
harnesses:

```cmake
# Opt-in: link the test engine against a libsrt-enabled ffmpeg (built by
# build-scripts/build_ffmpeg_macos_srt.sh) so the SRT e2e can ingest srt://.
# Default unset -> brew ffmpeg, so CI and the UDP gates are unaffected.
set(OLR_FFMPEG_SRT_PREFIX "" CACHE PATH "Path to a libsrt-enabled ffmpeg install")
if(OLR_FFMPEG_SRT_PREFIX)
    set(OLR_FFMPEG_INCLUDE "${OLR_FFMPEG_SRT_PREFIX}/include")
    set(OLR_FFMPEG_LIBDIR  "${OLR_FFMPEG_SRT_PREFIX}/lib")
endif()
```

The engine/harness targets that link ffmpeg get the SRT prefix's `lib` added to
their **BUILD_RPATH** (when the prefix is set) so the harness binaries resolve the
SRT dylibs at runtime without `DYLD_LIBRARY_PATH`.

Default (`OLR_FFMPEG_SRT_PREFIX` unset): identical to today — brew ffmpeg, no
change to CI or the UDP tests.

### 3.3 `tests/e2e/run_srt_smoke.sh` — one-stream SRT proof

Mirrors `run_record_e2e.sh`, but the source is **SRT**:

1. Start one **SRT listener** carrying the flash/beep MPEG-TS. The simplest
   generator that works regardless of the built CLI: the **already-installed
   `srt-live-transmit`** bridging a UDP MPEG-TS producer to an SRT listener —
   `ffmpeg … -f mpegts udp://127.0.0.1:$UDP &` then
   `srt-live-transmit "udp://127.0.0.1:$UDP" "srt://127.0.0.1:$SRT?mode=listener&transtype=live&latency=200" &`.
   (ffmpeg here is the stock brew CLI — it only emits UDP; `srt-live-transmit`
   does the SRT.)
2. Run `record_harness --url "srt://127.0.0.1:$SRT?transtype=live"` (caller mode,
   the engine's default) for a few seconds.
3. Assert the produced MKV is valid: video + stereo audio streams, frame count in
   the right ballpark — the same `ffprobe` assertions `run_record_e2e.sh` uses.
4. Hermetic: `mktemp -d`, `trap` kills the producer + `srt-live-transmit` and
   removes the temp dir.
5. `SKIP` (exit 0) if `srt-live-transmit` or `ffmpeg`/`ffprobe` is missing; **FAIL
   loudly** if the harness produces no/empty MKV (that is the SRT-ingest
   regression we are guarding).

### 3.4 CTest registration (`tests/e2e/CMakeLists.txt`)

```cmake
add_test(NAME e2e_srt_smoke
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_smoke.sh" "$<TARGET_FILE:record_harness>" 23501)
set_tests_properties(e2e_srt_smoke PROPERTIES LABELS "srt" TIMEOUT 120 RUN_SERIAL TRUE)
```

The **`srt` label is distinct** from `e2e`, so CI (which selects/excludes `e2e`)
never runs it. Run locally with `ctest -L srt`. It is only meaningful when the
build was configured with `OLR_FFMPEG_SRT_PREFIX` (otherwise the harness's
avformat lacks SRT and the test fails — that is correct: it documents the
requirement).

### 3.5 `tests/e2e/SRT_README.md`

Short doc: the one-time SRT-ffmpeg build (`build-scripts/build_ffmpeg_macos_srt.sh`),
how to configure + run the SRT e2e:
```
bash build-scripts/build_ffmpeg_macos_srt.sh                       # ~10 min, one time
cmake -S . -B build/srt -G Ninja -DOLR_BUILD_TESTS=ON \
   -DOLR_FFMPEG_SRT_PREFIX="$PWD/macos_build/ffmpeg-srt" -DCMAKE_PREFIX_PATH=...
ninja -C build/srt record_harness
( cd build/srt && ctest -L srt --output-on-failure )
```
plus the `--enable-nonfree` caveat: this ffmpeg is **for local testing only**, not
redistributable.

## 4. Error handling / edge cases

- `OLR_FFMPEG_SRT_PREFIX` unset → brew ffmpeg, SRT test still registers but will
  fail if run (expected; the README explains it needs the SRT build). CI never
  runs `-L srt`, so this is invisible there.
- Missing `srt-live-transmit`/ffmpeg → smoke `SKIP`s (exit 0).
- The build script fails loudly if libsrt did not link into the produced
  `libavformat` (the self-check), so a silently-SRT-less build can't pass as good.
- `srt-live-transmit` bridging adds a small startup delay; the smoke `sleep`s
  before launching the harness (as `run_record_e2e.sh` already does).

## 5. Testing

- The **`e2e_srt_smoke`** test IS the verification: `srt://` in → valid MKV out
  proves the engine's SRT ingest path works against real SRT.
  - **As built (hardening beyond §3.3's structural checks):** because
    `record_harness` writes blue-fill video + **silence** for the whole duration
    even when no source connects, the structural checks (frame count, channel
    count) would *falsely pass* without SRT. The smoke therefore adds a **positive
    audio-content proof** — the recorded audio must carry the producer's 1 kHz tone
    (overall RMS `> -60 dB`), not silence (`-inf`). A teeth-check confirms the
    discriminator: SRT build → RMS ≈ −25 dB → PASS; SRT-less brew build → RMS
    `-inf` → FAIL. That content proof is what actually verifies SRT *content* was
    ingested.
- Manual sanity: run it locally after the build; confirm PASS. Confirm the
  default (no SRT prefix) build + CI are unchanged (`ctest -L e2e` still 10/10).

## 6. Out of scope (Phase 2 / later)

- The **4-source SRT e2e framework** (per-camera identity, inter-camera sync,
  per-source trim, audio-latency, connection-status, disconnect/loss injection) —
  Phase 2, its own spec, built on this infra.
- Running SRT e2e in **CI** (local-run only per the brainstorm).
- Relinking the **desktop GUI app** against the SRT ffmpeg (this Phase wires the
  test build only; whether the shipped desktop app needs an SRT ffmpeg is a
  separate product question worth raising, not solved here).
- Building the ffmpeg **CLI/programs** with SRT (Phase 1 uses `srt-live-transmit`
  for generation; `--disable-programs` keeps the build lean — Phase 2 may enable
  programs if it wants direct SRT-ffmpeg generation).

## 7. Risks

- **Build fragility:** a from-source ffmpeg configure can fail on toolchain
  specifics. Mitigation: reuse the proven iOS configure flags (minus cross-compile)
  and the existing ffmpeg-8.0 source; the self-check catches a libsrt-less result.
- **rpath/dylib resolution:** the harness must find the SRT dylibs at runtime.
  Mitigation: BUILD_RPATH to the SRT lib dir + `@rpath` install names (reusing the
  iOS fix-up); the smoke test exercises this on first run.
- **`srt-live-transmit` timing:** the UDP→SRT bridge + caller connect has startup
  latency; if the harness connects before the listener is up it retries (the
  engine already reconnects with backoff). The smoke's pre-`sleep` covers normal
  startup.
- **nonfree:** `--enable-openssl` forces `--enable-nonfree`; the artifact is
  local-test-only. Documented; it lives under the gitignored `macos_build/`.
