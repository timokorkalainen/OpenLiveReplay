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

Contributing
------------
Contributions, issues and feature requests are welcome. Please open issues describing the problem or enhancement and follow the repository's contribution guidelines when submitting pull requests.

License
-------
See `LICENSE.md` for license terms.

Contact
-------
For questions or collaboration inquiries, open an issue or contact the maintainers via the repository's issue tracker.
