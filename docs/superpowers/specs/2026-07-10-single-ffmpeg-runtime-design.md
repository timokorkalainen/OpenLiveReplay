# Single FFmpeg Runtime Design

**Status:** Approved, revised to use stock Qt without rebuilding Qt components

## Context

OpenLiveReplay owns a controlled FFmpeg 8 build for recording, playback,
demuxing, seeking, audio and fallback codecs, SRT integration, and frame
conversion. Stock Qt Multimedia also ships an FFmpeg media-player backend. If
that plugin is deployed or statically imported, the process or package can
contain a second, independently versioned FFmpeg.

OpenLiveReplay does not use Qt's media player, recorder, decoder, camera, or
capture-session APIs. It uses only:

- `QMediaDevices` and `QAudioSink` for raw PCM monitoring;
- `QVideoFrame`, `QVideoSink`, and QML `VideoOutput` for frames produced
  by OpenLiveReplay's own pipeline.

Those core audio and video-sink surfaces do not require Qt's FFmpeg media
backend.

## Decision

Use stock Qt binaries on every supported platform. Do not rebuild Qt or Qt
Multimedia.

Qt's FFmpeg media backend is excluded at deployment or static-plugin selection.
OpenLiveReplay's controlled FFmpeg 8 is the only FFmpeg allowed in every
package and running process.

The guarantee is enforced at four boundaries:

1. source policy prevents introducing Qt APIs that require a media backend;
2. runtime policy selects a native Qt backend where one exists;
3. packaging excludes the Qt FFmpeg plugin and its private FFmpeg runtime;
4. binary, link-map, and loaded-module audits reject a second FFmpeg.

## Goals

- Ship exactly one FFmpeg implementation on Windows, macOS, Linux, and iOS.
- Keep the stock aqt/Qt installation immutable and unmodified.
- Avoid all Qt source builds and custom Qt binary overlays.
- Keep OpenLiveReplay's pinned FFmpeg 8 and SRT builds.
- Preserve raw audio monitoring and application-fed video previews.
- Fail closed when Qt's FFmpeg plugin or another FFmpeg ABI enters a package.
- Keep local, CI, and release runtime behavior aligned.

## Non-Goals

- Rebuilding Qt or Qt Multimedia.
- Making Qt's FFmpeg plugin use OpenLiveReplay's FFmpeg 8.
- Adding `QMediaPlayer`, `QMediaRecorder`, `QAudioDecoder`, cameras, or
  capture sessions.
- Replacing OpenLiveReplay's own FFmpeg pipeline.
- Relying on a system FFmpeg for release artifacts.

## Platform Policy

### Windows

- Use the stock Qt Multimedia library.
- Set `QT_MEDIA_BACKEND=windows` before constructing `QGuiApplication`.
- Deploy the Windows native media plugin when required by Qt.
- Exclude `ffmpegmediaplugin.dll` and the FFmpeg DLL family deployed solely
  for that plugin.
- Package OpenLiveReplay's FFmpeg 8 DLLs and `libsrt.dll`.

The existing Windows probe demonstrated that `QAudioSink` starts, previews
remain operational, and only ABI-62 OpenLiveReplay FFmpeg modules load when the
Windows backend is selected.

### macOS

- Use the stock Qt Multimedia framework.
- Set `QT_MEDIA_BACKEND=darwin` before constructing `QGuiApplication`.
- Retain the Darwin/AVFoundation backend.
- Exclude the Qt FFmpeg media plugin and its private FFmpeg dylibs from the app
  bundle.
- Bundle OpenLiveReplay's controlled FFmpeg 8 and SRT dylibs with
  origin-relative install names.

### Linux

- Use the stock Qt Multimedia library.
- Deploy no Qt media-player backend.
- Use Qt's integrated PipeWire or PulseAudio support for `QAudioSink`.
- Continue using application-fed `QVideoSink` / `VideoOutput`.
- Build and package OpenLiveReplay's pinned FFmpeg 8 and SRT instead of distro
  FFmpeg.

The packaged runtime uses `qt.conf` and package-local plugin paths so the
stock Qt installation's FFmpeg plugin cannot be discovered. Tests launch from
that isolated runtime.

### iOS

- Use the stock Qt Multimedia static libraries.
- Set `QT_MEDIA_BACKEND=darwin`.
- Use `qt_import_plugins` to exclude the Qt FFmpeg media plugin and include
  the Darwin media plugin.
- Do not call `qt_add_ios_ffmpeg_libraries`.
- Keep OpenLiveReplay's existing FFmpeg 8 and SRT XCFrameworks.
- Inspect the final Xcode link map to prove that no Qt FFmpeg plugin or Qt
  FFmpeg archive objects were linked.

If the installed Qt version cannot exclude the static FFmpeg plugin without
linking Qt's FFmpeg archives, the iOS build fails closed. It does not silently
introduce a second FFmpeg or switch to an unverified ABI.

## Runtime Backend Policy

`appenv::configureQtMediaBackend()` runs before `QGuiApplication` construction:

- Windows overwrites inherited selection with `windows`;
- macOS and iOS overwrite inherited selection with `darwin`;
- Linux clears inherited selection and relies on the isolated package
  containing no media-player plugin.

Package tests verify that core audio and video-sink APIs operate without Qt's
FFmpeg backend. Unsupported inherited `QT_MEDIA_BACKEND` values cannot
override application policy.

## Plugin Isolation

Desktop packages contain a package-local `qt.conf`. Qt plugin and QML lookup
is restricted to package directories. A release package must not search a
developer's global Qt installation.

Deployment uses stock Qt tools, followed by deterministic filtering:

- identify the Qt FFmpeg media plugin by plugin filename and metadata;
- resolve that plugin's complete transitive FFmpeg dependency graph;
- remove only the plugin and FFmpeg libraries owned solely by it;
- preserve Qt platform, audio, rendering, image, and native media plugins;
- run a recursive audit after filtering.

Filtering is package-local. It never modifies the aqt/Qt installation.

## Application FFmpeg Policy

OpenLiveReplay's controlled FFmpeg remains:

- FFmpeg 8 shared DLLs on Windows;
- FFmpeg 8 shared dylibs on macOS;
- pinned FFmpeg 8 shared objects on Linux;
- FFmpeg 8 static XCFrameworks on iOS.

The Linux release path gains a pinned source builder equivalent to the existing
Windows, macOS, and iOS builders. System FFmpeg remains allowed only for
explicit non-release developer configurations.

## API Boundary

Allowed production APIs:

- `QMediaDevices`, `QAudioDevice`, `QAudioSink`, and raw-audio types;
- `QVideoFrame`, `QVideoSink`, and application-fed QML `VideoOutput`.

Prohibited production APIs:

- `QMediaPlayer` / `MediaPlayer`;
- `QMediaRecorder` / `MediaRecorder`;
- `QAudioDecoder`;
- camera and capture-session APIs requiring a media backend.

A source-level smoke test enforces the boundary.

## Package Audit

The package audit recursively inspects binaries, frameworks, plugins, QML
modules, symlinks, and nested bundles.

Desktop acceptance requires:

- no Qt FFmpeg media plugin;
- no FFmpeg ABI other than OpenLiveReplay's expected FFmpeg 8 ABI;
- every FFmpeg library resolves inside the package;
- every FFmpeg library's provenance is the controlled dependency prefix;
- no system or global Qt plugin path is usable;
- no unresolved SRT or FFmpeg dependency.

iOS acceptance requires:

- no Qt FFmpeg plugin target in the static plugin import source;
- no Qt-provided FFmpeg archive/object origin in the Xcode link map;
- all `libav*` symbols originate from OpenLiveReplay's approved FFmpeg 8
  XCFrameworks.

The audit emits an FFmpeg/SRT SBOM fragment from locked source and binary
hashes.

## Runtime Verification

Desktop package smoke tests launch in an isolated environment and verify:

- the process remains responsive;
- `QAudioSink` accepts 48 kHz stereo PCM, or reports an explicitly allowed
  headless no-device state;
- two distinct frames pass through application-fed `VideoOutput`;
- loaded-module inspection finds only the expected FFmpeg 8 ABI;
- logs contain no Qt FFmpeg initialization or backend fallback error.

iOS device and simulator smoke tests verify raw audio startup and preview
advancement. Link-map inspection provides static FFmpeg provenance.

## Failure Handling

Build and packaging fail closed. They do not:

- rebuild Qt as a fallback;
- retain Qt's FFmpeg plugin when filtering is uncertain;
- use system FFmpeg in release mode;
- weaken the expected ABI based on installed libraries;
- publish a package without package and runtime audit evidence.

Diagnostics identify the unexpected plugin or library, its dependency parent,
resolved path, and expected FFmpeg ABI.

## Migration Sequence

1. Remove the abandoned custom Qt bootstrap and generated custom-kit artifacts.
2. Add deterministic backend selection and API-boundary tests.
3. Add the cross-platform package/link-map audit.
4. Filter Qt's dynamic FFmpeg backend from Windows, macOS, and Linux packages.
5. Exclude Qt's static FFmpeg plugin on iOS and prove link-map provenance.
6. Add the pinned Linux FFmpeg 8/SRT build.
7. Add isolated audio/video/runtime smoke tests to CI and pre-push.

## Acceptance Criteria

- No Qt component is rebuilt.
- Every supported release package proves exactly one FFmpeg implementation.
- That implementation is OpenLiveReplay's controlled FFmpeg 8.
- Qt's FFmpeg media plugin is absent from dynamic packages and iOS static
  imports.
- Audio monitoring and every preview output pass platform smoke tests.
- Existing recording, replay, seek, and SRT tests remain green.
- No release build resolves Qt plugins, FFmpeg, or SRT from uncontrolled paths.
- The source-policy gate prevents future Qt media-backend API dependencies.
- No commit is created until the user explicitly grants permission.
