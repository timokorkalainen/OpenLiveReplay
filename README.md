# OpenLiveReplay

OpenLiveReplay is a performant, cross-platform multi-track live video recorder and replay system focused on low-latency capture, synchronized multi-stream recording, and lightweight replay for field use.

**Objective:**
Provide a reliable, low-latency platform for recording and replaying multiple camera feeds simultaneously, optimized for event safety, sports judging, and small-scale live productions.

**Target audience:**
- Event safety teams and sports judges who need quick, accurate replay and review
- Small production crews and live-event operators exploring compact multi-track workflows

**Project status:**
- Prototype stage. Core functionality implemented.
- MacOS and iOS builds are functional via automated CMake based pipeline.
- SRT-based recording has been found reliable in testing; project is approaching MVP readiness.

**Key features:**
- Multi-track synchronized recording (video only)
- Low-latency ingest and replay pipeline
- Support for SRT and RMPT streams, local recording and screenshotting
- MIDI controller support, tested with Behringer X-Touch One

Getting started
-------------

1. Clone the repository
2. Open the project in Qt Creator (open the project root or `CMakeLists.txt`), configure for your target (macOS or iOS), then build.

For **Windows** (Qt MinGW kit), the dependencies (FFmpeg + SRT) are built from
source by a one-command script — see [`docs/windows-build.md`](docs/windows-build.md):

```bash
./build-scripts/build_windows_app.sh    # Windows (Git Bash)
./build-scripts/build_macos_app.sh      # macOS (Homebrew ffmpeg + srt)
```

A manually-triggered GitHub Actions workflow
([`.github/workflows/build.yml`](.github/workflows/build.yml)) runs these on
clean runners and uploads packaged Windows/macOS bundles as artifacts.

Notes on dependencies
---------------------
- The build config downloads or configures native dependencies during CMake configure step, including FFmpeg and RtMidi for macOS and iOS targets.
- Ensure you have a compatible Qt toolchain installed (Qt 6.x recommended) and developer toolchains for the target platform.

Project layout (high level)
---------------------------
- `main.cpp`, `Main.qml` — app entry and UI
- `recorder_engine/` — muxer, replay manager, recording clock, stream worker
- `playback/` — playback worker, transport and frame provider
- `midi/` — MIDI integration and manager
- `settingsmanager.*`, `uimanager.*` — app settings and UI glue

Development (testing, linting, CI)
----------------------------------
Tests are opt-in and headless. Configure with `-DOLR_BUILD_TESTS=ON`, then:

```bash
cmake -S . -B build -G Ninja -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=~/Qt/6.10.1/macos
cmake --build build
ctest --test-dir build --output-on-failure   # unit + smoke + e2e
```

- **Unit** tests (Qt Test) cover the recording clock, settings persistence,
  playback transport, and muxer.
- A **smoke** test runs `qmllint` over the QML.
- **E2E** tests drive the real recording engine against a synthetic FFmpeg
  stream, including a regression for the mono-audio recording crash.
- **Sanitizers**: add `-DOLR_SANITIZER="address;undefined"` (or `thread`).
- **Formatting/linting**: `.clang-format`, `.editorconfig`, `.clang-tidy`.
- **CI**: `.github/workflows/ci.yml` (build+test, lint, sanitizers). iOS and the
  full E2E matrix are *recommended* to build/run locally before pushing (not CI,
  not forced); an optional pre-push hook surfaces the reminder — enable with
  `git config core.hooksPath .githooks`. See [`tests/README.md`](tests/README.md).

See [`tests/README.md`](tests/README.md) for full details.

Contributing
------------
Contributions, issues and feature requests are welcome. Please open issues describing the problem or enhancement and follow the repository's contribution guidelines when submitting pull requests. Before opening a PR, run the test suite and format any changed C++ with `xcrun clang-format -i`.

License
-------
See `LICENSE.md` for license terms.

Contact
-------
For questions or collaboration inquiries, open an issue or contact the maintainers via the repository's issue tracker.
