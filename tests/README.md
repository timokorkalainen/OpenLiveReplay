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

### Pre-push gate (fast pre-flight, always-on)

`git push` runs a blocking pre-push gate (`.githooks/pre-push`) — a fast (~5 min)
local pre-flight so a push rarely fails CI on trivia. On every push it builds the
current-platform host with `-Werror`, then runs — in order — the `delivery-gate`
matrix (below), clang-tidy on changed first-party C++, and the fast unit label
(`-L ci`). Any failure blocks the push. The slower playback e2e + clang
ASan/UBSan/TSan passes run in GitHub CI now; reproduce them locally with
`OLR_PREPUSH_FULL=1`. The `delivery-gate` matrix is the four transport×codec
delivery smokes with 5-second clips:

| | h264 | h265 |
| --- | --- | --- |
| **RTMP** | `e2e_native_rtmp_smoke` | `e2e_native_rtmp_hevc_smoke` |
| **SRT**  | `e2e_native_srt_smoke`  | `e2e_native_srt_hevc_smoke` |

Run the matrix on demand with:

```bash
OLR_E2E_CLIP_SECONDS=5 ctest --test-dir build -L delivery-gate --output-on-failure
```

The gate **hard-fails the push** (it does not silently skip) if any required
tooling is missing: `ffmpeg`, `ffprobe`, `srt-live-transmit`, a Python 3, an
H.264 encoder (`libx264`/`h264_mf`) and an HEVC encoder (`libx265`/`hevc_*`) in
ffmpeg, and the Qt host kit at `$QT_HOST_PREFIX`. On macOS: `brew install ffmpeg srt`.

Tunables / bypass (never use `git push --no-verify` — see CLAUDE.md):
- `OLR_E2E_CLIP_SECONDS` — clip length for the e2e/delivery matrices (default `5`).
- `OLR_PREPUSH_LIGHT=1 git push` — run the delivery matrix only (skip clang-tidy + unit).
- `SKIP_UNIT=1` / `SKIP_TIDY=1` — skip one default pre-flight gate.
- `OLR_PREPUSH_FULL=1 git push` — also run the playback e2e + ASan/UBSan + TSan +
  full local CTest matrix + iOS build (skip parts: `SKIP_E2E=1` / `SKIP_ASAN=1` /
  `SKIP_TSAN=1` / `SKIP_FULL_TESTS=1` / `SKIP_IOS_BUILD=1`).
- `OLR_PREPUSH_SKIP=1 git push` — emergency full bypass only.

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

`OLR_SANITIZER` instruments the app and tests. GitHub CI runs the full sanitizer
matrix: clang **ASan+UBSan** (unit + playback e2e) and **TSan** (threading cases)
on macOS — the only place the VideoToolbox/AudioToolbox native-codec tests are
sanitized, since they build as stubs on Linux — plus the GCC ASan+UBSan+**LSan**
pass over the unit suite on Linux (LeakSanitizer is unsupported on macOS), which
doubles as the second-compiler `-Werror` gate. Reproduce the clang passes locally
with `OLR_PREPUSH_FULL=1`.

## Linting & formatting

- **Format:** `.clang-format` (C++) and `.editorconfig`. Format changed files with
  `xcrun clang-format -i <files>`. The tree was *not* mass-reformatted; CI checks
  formatting on changed files only.
- **C++ static analysis:** `.clang-tidy` (bug-focused). The high-signal checks
  gate changed first-party files in the pre-push hook and pre-commit; run by hand
  with `clang-tidy -p build --extra-arg=-isysroot --extra-arg=$(xcrun --show-sdk-path) <file>`
  (the `-isysroot` arg is required for Homebrew LLVM to find the macOS SDK headers).
- **QML:** `qmllint -I "$QT/qml" Main.qml MultiviewWindow.qml`.

## CI

`.github/workflows/ci.yml` is the comprehensive gate. GHA is free for this
public repo, so it is bounded by WALL-CLOCK (~6 min), not minutes: jobs fan out
in parallel across macOS, Linux and Windows, all cached. It runs on PRs and
pushes to `main`:

- **changes** — classifies the diff first. Docs-only PRs do not run app builds
  or tests.
- **workflow-lint** — `actionlint` for workflow changes.
- **lint** — source/QML changes only; changed-line `clang-format` and `qmllint`.
- **build-test-macos** — Apple-clang `-Werror` build + the fast unit label +
  clang-tidy on changed files. (The playback e2e runs once, under ASan, in the
  `sanitizers` leg — not duplicated here, to stay within the wall-clock budget.)
- **build-test-windows** — MinGW build of the app + the deterministic AAC Media
  Foundation unit test (FFmpeg/SRT from pinned source, **cached** at
  `windows_build/dist`). The MF video-decode capability probe is compiled but not
  run — it hangs on the headless hosted runner.
- **build-test-linux** — GCC `-Werror` + the unit suite under ASan+UBSan+**LSan**
  (the only leg with LeakSanitizer + a second compiler).
- **sanitizers** — clang ASan+UBSan (unit + e2e) and TSan, on macOS.
- **ci-gate** — the one required status check; passes when the gating jobs above
  succeed or are skipped (e.g. a docs-only PR).
- **coverage** / **mutation-testing** / **fuzz** — manual (`workflow_dispatch`) only.

CI caches Qt (`install-qt-action`), Homebrew/apt downloads, C/C++ outputs
(`ccache`), and the from-source Windows FFmpeg/SRT deps.

## Full local CTest matrix + iOS build (opt-in)

iOS is **not** built in GitHub CI: the FFmpeg+SRT from-source build
(~20 min) OOM-kills hosted runners. The pre-push gate above already runs the
unit/e2e/sanitizer gates by default; `OLR_PREPUSH_FULL=1 git push` *additionally*
runs the full non-local-only CTest matrix and the iOS build. To run that matrix
by hand:

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

Enable the [`.githooks/pre-push`](../.githooks/pre-push) hook once per clone:

```bash
git config core.hooksPath .githooks
```

The full CTest matrix + iOS build are opt-in on top of the default gate:
`OLR_PREPUSH_FULL=1 git push` (within that, skip a part with `SKIP_FULL_TESTS=1`
or `SKIP_IOS_BUILD=1`; if no Qt iOS kit is found the iOS part skips and the push
proceeds after the rest of the gate passes).

The iOS FFmpeg slice uses SecureTransport for TLS/RTMPS and builds libsrt
without OpenSSL/mbedTLS encryption support. Encrypted native SRT is intentionally
out of scope for this slice. FFmpeg also intentionally excludes HEVC; SRT HEVC
ingest uses the native VideoToolbox path instead.

## Known follow-ups

- A runtime offscreen QML load test (beyond `qmllint`) needs the app factored
  into a library + thin `main`; today the smoke test is static-only.
