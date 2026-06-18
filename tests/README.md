# OpenLiveReplay test suite

Automated testing, linting, formatting, and CI for OpenLiveReplay. Everything
here is **opt-in** at configure time (`-DOLR_BUILD_TESTS=ON`) and runs headless,
so it never affects a normal app build.

## Quick start

```bash
# Configure with tests enabled (adjust the Qt prefix to your install)
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=~/Qt/6.10.1/macos \
  -DOLR_BUILD_TESTS=ON

cmake --build build                 # build app + tests
ctest --test-dir build -L ci --output-on-failure
```

Run a subset by label: `ctest --test-dir build -L unit` (or `smoke`, `e2e`).
The `ci` label is the short PR gate and intentionally excludes the expensive
record/playback E2E matrix. Run the full local gate with:

```bash
ctest --test-dir build --output-on-failure -LE 'sync-report|srt|native-apple-ingest'
```

Local transport bring-up gates use distinct labels such as `srt`,
`native-apple-ingest`, `native-ndi`, and `native-rtmp`; they are excluded from
CI until their matching native ingest path is ready.

The `ndi-runtime` label is a real NDI sender/receiver smoke test. It loads the
NDI runtime dynamically, routes cache-backed app output frames through
`OutputDispatcher` into the app's `NdiOutputSink`, discovers the local sender,
and verifies video cadence plus non-silent audio. It skips only when the NDI
runtime is not installed or discoverable; set
`OLR_NDI_RUNTIME_LIBRARY=/path/to/libndi.dylib` when the runtime is outside the
platform default or known NDI Tools locations. Set
`OLR_NDI_RUNTIME_SOAK_SECONDS=300` to keep the sender and receiver running for a
five-minute soak.

```bash
OLR_NDI_RUNTIME_SOAK_SECONDS=300 \
ctest --test-dir build -L ndi-runtime --output-on-failure
```

The `native-ndi` label records a real MKV through the native NDI ingest path.
By default it starts `ndi_runtime_sender`, a local sender that uses the app's own
runtime-loaded `NdiOutputSink`; set `OLR_NDI_TEST_SOURCE='Studio (CAM1)'` only
when you intentionally want to test an external source instead.

`native-rtmp` mirrors the applicable SRT transport gates: one-source RTMP/RTMPS
smoke, 4-source routing, 4-source sync, per-source trim, and live/dead
connection count.

Native RTMP/RTMPS is the default ingest path. The legacy FFmpeg RTMP path is no
longer selected for RTMP/RTMPS when the native backend is available.

The optional real-server interop gate is also under `native-rtmp`. It skips
unless `OLR_RTMP_INTEROP_PLAY_URL` is set; set `OLR_RTMP_INTEROP_PUBLISH_URL`
when the publish URL differs, and optionally `OLR_RTMP_INTEROP_SERVER_CMD` to
start a local server for the duration of the test.

Real-server native RTMP interop:

```bash
OLR_RTMP_INTEROP_PLAY_URL=rtmp://127.0.0.1/live/stream \
OLR_RTMP_INTEROP_PUBLISH_URL=rtmp://127.0.0.1/live/stream \
ctest --test-dir build/native-rtmp -R e2e_native_rtmp_interop --output-on-failure
```

For HEVC/E-RTMP:

```bash
OLR_RTMP_INTEROP_CODEC=hevc \
OLR_RTMP_INTEROP_PLAY_URL=rtmp://127.0.0.1/live/hevc \
OLR_RTMP_INTEROP_PUBLISH_URL=rtmp://127.0.0.1/live/hevc \
ctest --test-dir build/native-rtmp -R e2e_native_rtmp_interop --output-on-failure
```

The long-run soak gate is separate and opt-in:

```bash
OLR_RTMP_RUN_SOAK=1 OLR_RTMP_SOAK_SECONDS=1800 OLR_RTMP_SOAK_CODEC=avc \
ctest --test-dir build/native-rtmp -R e2e_native_rtmp_soak --output-on-failure
```

## What's covered

| Layer | Where | What it checks |
|-------|-------|----------------|
| **Unit** (`-L unit`) | `tests/unit/` | `RecordingClock` (monotonic timeline), `SettingsManager` (JSON save/load round-trip + failure modes), `PlaybackTransport` (seek/step/speed/fps math + signals), `Muxer` (track layout, stream bounds, stereo/mono channel layout, file output). Qt Test + CTest. |
| **Smoke** (`-L smoke`) | `tests/smoke/` | `qmllint` over `Main.qml` / `MultiviewWindow.qml` — fails on QML syntax/type errors, tolerates the project's existing style warnings. Also checks the iOS FFmpeg build config stays static, GPL/nonfree-free, SecureTransport-based, and RTMP/RTMPS-capable. |
| **E2E** (`-L e2e`) | `tests/e2e/` | A headless `record_harness` drives the real `ReplayManager` against a synthetic FFmpeg stream, then `ffprobe` asserts the output: stream layout, ~correct frame count, and **stereo** audio. Includes the **mono-audio regression** (a mono source must record without the SIGBUS crash from commit `3c7d9b4` and be rematrixed up to stereo). |

The E2E tests need `ffmpeg`/`ffprobe` on `PATH` (they `SKIP` cleanly if absent)
and bind UDP loopback ports (they run serially). They are full local/pre-push
coverage, not PR CI coverage.

## Sanitizers

```bash
cmake -S . -B build-asan -G Ninja -DOLR_BUILD_TESTS=ON \
  -DCMAKE_PREFIX_PATH=~/Qt/6.10.1/macos \
  -DOLR_SANITIZER="address;undefined"     # or "thread"
cmake --build build-asan
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan --output-on-failure
```

`OLR_SANITIZER` instruments the app and tests. CI keeps this to fast unit spot
checks instead of rerunning the full suite in every sanitizer leg:
ASan+UBSan covers mux/parser/audio primitives, while TSan covers playback buffer
primitives. The record/playback E2E drivers stay in the local pre-push gate.

## Linting & formatting

- **Format:** `.clang-format` (C++) and `.editorconfig`. Format changed files with
  `xcrun clang-format -i <files>`. The tree was *not* mass-reformatted; CI checks
  formatting on changed files only.
- **C++ static analysis:** `.clang-tidy` (bug-focused, advisory). Run with
  `clang-tidy -p build --extra-arg=-isysroot --extra-arg=$(xcrun --show-sdk-path) <file>`
  (the `-isysroot` arg is required for Homebrew LLVM to find the macOS SDK headers).
- **QML:** `qmllint -I "$QT/qml" Main.qml MultiviewWindow.qml`.

## CI

`.github/workflows/ci.yml` runs on PRs and pushes to `main`:

- **changes** — classifies the diff first. Docs/workflow-only PRs do not run
  app builds, tests, or sanitizers.
- **workflow-lint** — `actionlint` for workflow changes.
- **build-test-macos** — app/test changes only; builds app + tests and runs the
  short `ci` CTest label (primary PR gate).
- **lint** — source/QML changes only; runs changed-line `clang-format` and/or
  `qmllint` only when those file types changed.
- **sanitizers** — native-code/CMake changes only; focused ASan+UBSan spot
  checks (gating) and a focused ThreadSanitizer spot check (advisory).

macOS jobs cache Qt via `install-qt-action`, Homebrew downloads via
`actions/cache`, and C/C++ compiler outputs via `ccache`.

## Full local gate + iOS build (recommended before pushing, not CI)

iOS is **not** built in GitHub CI: the FFmpeg+SRT from-source build
(~20 min) OOM-kills hosted runners. The expensive E2E matrix is also kept out
of PR CI. Both are **recommended** to run locally before pushing risky changes —
they are not forced. Run the full non-local-only CTest matrix with:

```bash
cmake -S . -B build/prepush-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=~/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
cmake --build build/prepush-tests
ctest --test-dir build/prepush-tests --output-on-failure \
  --repeat until-pass:2 -LE 'sync-report|srt|native-apple-ingest'
```

then cross-build the iOS target (the FFmpeg xcframeworks are cached in
`ios_build/xcframeworks`, so only the first build is slow; override the Qt
location with `QT_IOS_PREFIX` / `QT_HOST_PREFIX`):

```bash
~/Qt/6.10.1/ios/bin/qt-cmake -S . -B build/ios-prepush -G Xcode \
  -DQT_HOST_PATH=~/Qt/6.10.1/macos -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build/ios-prepush --config Debug -- \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO
```

The optional [`.githooks/pre-push`](../.githooks/pre-push) hook surfaces this
reminder at push time without blocking — it just prints the recommendation and
lets the push through. Enable it once per clone:

```bash
git config core.hooksPath .githooks
```

If you want the hook to actually run the full gate + iOS build as a one-off
**blocking** check, opt in with `OLR_PREPUSH_FULL=1 git push` (within that, skip
a part with `SKIP_FULL_TESTS=1` or `SKIP_IOS_BUILD=1`; if no Qt iOS kit is found
the iOS part skips and the push proceeds after the CTest gate passes).

The iOS FFmpeg slice uses SecureTransport for TLS/RTMPS and builds libsrt
without OpenSSL/mbedTLS encryption support. Encrypted native SRT is intentionally
out of scope for this slice. FFmpeg also intentionally excludes HEVC; SRT HEVC
ingest uses the native VideoToolbox path instead.

## Known follow-ups

- A runtime offscreen QML load test (beyond `qmllint`) needs the app factored
  into a library + thin `main`; today the smoke test is static-only.
- `clang-tidy` is advisory; flip it to gating once the warning backlog is
  triaged.
- Introducing `-Werror` for the app (currently first-party warnings are
  surfaced but non-fatal) after the existing warning backlog is cleared.
