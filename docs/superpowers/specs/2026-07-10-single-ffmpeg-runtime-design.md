# Single FFmpeg Runtime Design

**Status:** Approved for implementation planning

## Context

OpenLiveReplay owns a controlled, SRT-enabled FFmpeg 8 build for recording,
playback demuxing and seeking, audio and fallback codec work, and frame
conversion. The Qt Online Installer's Qt Multimedia package is built with a
separate FFmpeg media backend. On platforms where that backend is deployed or
linked, the application can contain two FFmpeg versions even though
OpenLiveReplay does not use `QMediaPlayer`, `QMediaRecorder`, or their QML
counterparts.

OpenLiveReplay uses only Qt Multimedia's core device, audio-output, and video
sink surfaces:

- `QMediaDevices` and `QAudioSink` for local PCM monitoring;
- `QVideoSink` through QML `VideoOutput` for frames supplied by the application's
  own providers.

These surfaces do not require Qt's FFmpeg media-player plugin. Shipping two
FFmpeg implementations adds package size, startup work, licensing inventory,
ABI confusion, and another independently versioned media stack without adding
application functionality.

## Decision

Every supported target must contain exactly one FFmpeg implementation:
OpenLiveReplay's pinned FFmpeg 8 build. Qt Multimedia will be built from the
matching Qt source release with its FFmpeg feature disabled.

This is a build-time property, not merely a runtime environment preference.
Backend selection, package audits, runtime smoke tests, and plugin-path
isolation provide additional enforcement.

## Goals

- Ship exactly one FFmpeg implementation on Windows, macOS, desktop Linux, and
  iOS.
- Keep OpenLiveReplay's controlled FFmpeg 8 and SRT configuration unchanged in
  ownership and purpose.
- Preserve `QAudioSink`, `QMediaDevices`, `QVideoSink`, and QML `VideoOutput`
  behavior.
- Make local development and CI deterministic from a versioned Qt base kit.
- Fail configuration or packaging when a second FFmpeg can enter the product.
- Keep the shared aqt/Qt installation immutable.

## Non-Goals

- Adding Qt media playback, recording, camera, or capture-session features.
- Replacing OpenLiveReplay's own FFmpeg pipeline.
- Making Qt Multimedia use FFmpeg 8 internally.
- Supporting arbitrary system Qt or system FFmpeg installations in release
  packaging.
- Removing native Media Foundation, AVFoundation, CoreAudio, PipeWire, or
  PulseAudio integration.

## Custom Qt Kit

### Base Kit

Each platform starts from the repository's pinned Qt version installed through
aqt or the Qt installer. The version, target architecture, compiler, linkage
mode, and build configuration are inputs to a custom-kit cache key. Qt source
must have the exact same version as the base kit.

The shared base kit is never modified. A bootstrap script materializes a
project-local kit under a gitignored dependency cache. It copies the base kit
while preserving symlinks, framework layout, permissions, and platform metadata.
The resulting directory is the only Qt prefix used to configure, build, test,
and deploy OpenLiveReplay.

### Multimedia Rebuild

The bootstrap script downloads the matching `qtmultimedia` source archive,
verifies a pinned checksum, and performs an out-of-source standalone module
build using the local kit's `qt-configure-module` tool. The module is configured
with:

```text
-no-feature-ffmpeg
-nomake examples
-nomake tests
```

Platform and configuration arguments must match the base kit. The rebuilt
module is installed into the project-local kit, replacing that kit's original
Qt Multimedia libraries, QML module, plugins, CMake package metadata, and tools.
Before the replacement is accepted, the bootstrap script removes the copied
base kit's Qt FFmpeg media plugins and every FFmpeg binary shipped solely for
those plugins. It derives the purge set from the original plugin's resolved
dependency graph instead of hard-coding ABI filenames. The purge runs only
inside the project-local kit and must not match any non-FFmpeg Qt dependency.

The bootstrap step writes a machine-readable manifest containing:

- Qt version and source checksum;
- platform, architecture, compiler, and configuration;
- complete configure arguments;
- the Qt Multimedia configure summary;
- hashes of installed Qt Multimedia binaries and plugins;
- the original Qt FFmpeg plugin dependency set and purge result;
- an explicit `ffmpegFeature=false` field.

The cache is valid only when every input and installed-file hash matches.

### CMake Selection

Release and CI presets require `OLR_QT_ROOT` to identify the project-local kit.
CMake must reject a base aqt kit or a custom kit whose manifest is absent,
invalid, or reports FFmpeg enabled. Qt deployment tools are resolved from the
same `OLR_QT_ROOT`; mixing build-time and deployment-time Qt prefixes is an
error.

Developer bootstrap commands may create the custom kit automatically. CMake
itself does not download or mutate dependencies during normal configuration.

## Platform Policy

| Platform | Qt media backend policy | OpenLiveReplay FFmpeg form |
| --- | --- | --- |
| Windows | Select the `windows` backend before `QGuiApplication` construction. | Shared FFmpeg 8 DLLs plus `libsrt.dll`. |
| macOS | Select the `darwin` backend before `QGuiApplication` construction. | Shared FFmpeg 8 dylibs plus SRT, bundled with controlled rpaths. |
| Linux | Deploy no Qt media-player backend. `QAudioSink` uses PipeWire or PulseAudio; application-fed video sinks remain available. | Pinned shared FFmpeg 8 and SRT libraries built by the repository. |
| iOS | Select the `darwin` backend. The FFmpeg-disabled Qt Multimedia build contains no Qt FFmpeg static plugin or link requirement. | Existing FFmpeg 8 and SRT XCFramework slices linked statically. |

Backend selection is an application policy and overrides inherited
`QT_MEDIA_BACKEND` values on Windows and Apple platforms. Linux deliberately has
no media-player backend because OpenLiveReplay does not consume one.

## Linux Dependency Control

Linux release builds currently allow distro FFmpeg through `pkg-config`. That is
incompatible with the single controlled-runtime guarantee. A Linux dependency
builder will mirror the Windows, macOS, and iOS source-build policy:

- pin and checksum FFmpeg 8 and SRT sources;
- use the repository's curated LGPL configuration;
- install into a project-local prefix;
- configure OpenLiveReplay exclusively against that prefix;
- package those libraries with origin-relative runtime search paths.

System PipeWire or PulseAudio remains the platform audio service and is not part
of the FFmpeg runtime count.

## Packaging Isolation

All desktop packages contain a package-local `qt.conf` that limits Qt plugin and
QML lookup to the package. A deployed application must not discover plugins
from a developer machine's global Qt installation.

Deployment uses the tools from the custom Qt kit. Packaging explicitly rejects:

- any plugin whose name or metadata identifies the Qt FFmpeg media backend;
- any `libavcodec`, `libavformat`, `libavutil`, `libswscale`, or
  `libswresample` major other than the expected OpenLiveReplay FFmpeg 8 ABI;
- any FFmpeg library outside the approved application dependency prefix;
- unresolved FFmpeg or SRT dependencies;
- duplicate libraries hidden in frameworks, plugins, QML modules, or nested app
  bundles.

The iOS package audit examines the final link map and application binary rather
than looking only for files, because FFmpeg is statically linked there.

## API Boundary

The following APIs are allowed:

- `QMediaDevices`, `QAudioDevice`, `QAudioSink`, and related raw-audio types;
- `QVideoFrame`, `QVideoSink`, and QML `VideoOutput` used as application-fed
  rendering sinks.

The following APIs and QML types are prohibited unless this design is revised:

- `QMediaPlayer` / `MediaPlayer`;
- `QMediaRecorder` / `MediaRecorder`;
- `QAudioDecoder`;
- camera and capture-session APIs that require a media backend.

A source-level CI check guards this boundary. The check is narrow enough to
ignore documentation and tests that intentionally name prohibited APIs while
failing production source additions.

## Verification

### Configure-Time Gates

- Validate the custom-kit manifest and cache key.
- Validate that Qt Multimedia's configure summary reports FFmpeg disabled.
- Validate that every dependency recorded in the original Qt FFmpeg purge set
  is absent from the custom kit.
- Validate that CMake and deployment tools come from the same custom kit.
- On Linux, reject system FFmpeg resolution in release configurations.

### Package-Time Gates

- Recursively scan all binaries and plugin metadata.
- Resolve transitive dynamic dependencies, not only direct executable imports.
- Require exactly the expected OpenLiveReplay FFmpeg ABI family on desktop.
- Require no Qt FFmpeg media plugin.
- Inspect macOS install names and rpaths.
- Inspect the iOS link map and linked symbols for a single FFmpeg archive origin.
- Emit an FFmpeg/SRT SBOM fragment from the approved dependency manifests.

### Runtime Gates

Desktop smoke tests launch from an isolated package environment and verify:

- the process remains responsive;
- `QAudioSink` opens and accepts PCM;
- frames injected through each preview provider advance in QML `VideoOutput`;
- loaded-module inspection finds only the expected FFmpeg ABI;
- logs contain no media-backend fallback or plugin-load failure.

iOS device and simulator smoke tests verify audio startup and preview
advancement. Static-link provenance is established by the package-time link-map
gate.

CI runs the gates for Windows, macOS, Linux, iOS device configuration, and iOS
simulator configuration. A platform that cannot prove the invariant does not
produce a release artifact.

## Failure Handling

Dependency bootstrap, configuration, and packaging fail closed. They do not:

- fall back to the unmodified aqt Qt Multimedia module;
- fall back to a system Qt or FFmpeg;
- silently omit audio or preview verification;
- weaken the expected FFmpeg major based on what happens to be installed.

Diagnostics identify the unexpected library or plugin, its dependency parent,
its resolved path, and the expected custom-kit manifest.

## Performance And Reliability

Removing Qt's unused FFmpeg backend reduces loaded modules, relocations, package
size, and media-stack initialization. OpenLiveReplay's recording and playback
hot paths are unchanged. Native platform audio remains responsible for PCM
output, and application-fed Qt video sinks remain responsible only for display.

The custom kit is built once per cache key. Normal application builds consume
the cached kit and do not rebuild Qt Multimedia.

## Migration Sequence

1. Add the custom Qt kit bootstrap and manifest validation.
2. Add the pinned Linux FFmpeg/SRT dependency build.
3. Point local and CI presets at the custom kit.
4. Add deterministic backend selection and the production API boundary check.
5. Update desktop and iOS packaging to use the custom kit and package-local
   plugin paths.
6. Add dependency, link-map, loaded-module, audio, and preview gates.
7. Remove legacy deployment assumptions that copy Qt's FFmpeg runtime.

Each platform moves only after its audio, preview, and package-provenance gates
pass. Release artifacts must not mix old and new policies.

## Acceptance Criteria

- Every supported release package proves exactly one FFmpeg implementation.
- That implementation is the repository-pinned OpenLiveReplay FFmpeg 8 build.
- Qt Multimedia's FFmpeg feature is disabled on every target.
- Audio monitoring and all preview outputs pass platform smoke tests.
- Existing recording, replay, seek, and SRT tests remain green.
- No release build resolves Qt, FFmpeg, or SRT from an uncontrolled system path.
- CI prevents prohibited Qt media-backend APIs from entering production code.
