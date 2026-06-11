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
ctest --test-dir build --output-on-failure
```

Run a subset by label: `ctest --test-dir build -L unit` (or `smoke`, `e2e`).

## What's covered

| Layer | Where | What it checks |
|-------|-------|----------------|
| **Unit** (`-L unit`) | `tests/unit/` | `RecordingClock` (monotonic timeline), `SettingsManager` (JSON save/load round-trip + failure modes), `PlaybackTransport` (seek/step/speed/fps math + signals), `Muxer` (track layout, stream bounds, stereo/mono channel layout, file output). Qt Test + CTest. |
| **Smoke** (`-L smoke`) | `tests/smoke/` | `qmllint` over `Main.qml` / `MultiviewWindow.qml` — fails on QML syntax/type errors, tolerates the project's existing style warnings. |
| **E2E** (`-L e2e`) | `tests/e2e/` | A headless `record_harness` drives the real `ReplayManager` against a synthetic FFmpeg stream, then `ffprobe` asserts the output: stream layout, ~correct frame count, and **stereo** audio. Includes the **mono-audio regression** (a mono source must record without the SIGBUS crash from commit `3c7d9b4` and be rematrixed up to stereo). |

The E2E tests need `ffmpeg`/`ffprobe` on `PATH` (they `SKIP` cleanly if absent)
and bind UDP loopback ports 23456/23457 (they run serially).

## Sanitizers

```bash
cmake -S . -B build-asan -G Ninja -DOLR_BUILD_TESTS=ON \
  -DCMAKE_PREFIX_PATH=~/Qt/6.10.1/macos \
  -DOLR_SANITIZER="address;undefined"     # or "thread"
cmake --build build-asan
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan --output-on-failure
```

`OLR_SANITIZER` instruments the app and tests. Running the E2E suite under
ASan+UBSan exercises the threaded capture→resample→mux pipeline — the exact
path that produced the mono-audio crash.

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

- **build-test-macos** — build app + tests, run CTest (primary gate).
- **lint** — `clang-format` (changed lines) + `qmllint`.
- **sanitizers** — ASan+UBSan (gating) and ThreadSanitizer (advisory).

## iOS build (local pre-push hook, not CI)

iOS is **not** built in GitHub CI: the FFmpeg+SRT+OpenSSL from-source build
(~20 min) OOM-kills hosted runners. It is validated locally by
[`.githooks/pre-push`](../.githooks/pre-push), which cross-builds the iOS target
on `git push`. Enable it once per clone:

```bash
git config core.hooksPath .githooks
```

The FFmpeg xcframeworks are cached in `ios_build/xcframeworks`, so only the
first push is slow. Override the Qt location with `QT_IOS_PREFIX` /
`QT_HOST_PREFIX`. Skip a single push with `SKIP_IOS_BUILD=1 git push` (or
`git push --no-verify`); if no Qt iOS kit is found the hook skips and allows the
push.

## Known follow-ups

- A runtime offscreen QML load test (beyond `qmllint`) needs the app factored
  into a library + thin `main`; today the smoke test is static-only.
- `clang-tidy` is advisory; flip it to gating once the warning backlog is
  triaged.
- Introducing `-Werror` for the app (currently first-party warnings are
  surfaced but non-fatal) after the existing warning backlog is cleared.
