# GammaRay setup (runtime introspection of the desktop app)

[GammaRay](https://github.com/KDAB/GammaRay) (KDAB) injects a probe into the
running OpenLiveReplay desktop app and exposes its Qt internals — the QObject
tree, the QML scene graph, signal/slot connections, timers, and live object
properties/methods. This document covers **installing** it; for the day-to-day
usage playbook see the [`gammaray` skill](../.claude/skills/gammaray/SKILL.md).

## Why a source build (the one hard rule)

GammaRay injects a probe whose ABI is keyed to the **target's Qt version**. This
app is built against **Qt 6.10.1** (`~/Qt/6.10.1/macos`), so GammaRay must be
built against that same Qt.

Do **not** `brew install gammaray`: Homebrew's build targets Homebrew's Qt (a
different minor, e.g. 6.11) and pulls in a second full Qt; a mismatched probe
will not introspect this app's QtQuick scene reliably. If you bump the project's
Qt, rebuild GammaRay against the new version.

## Prerequisites

- The project's Qt (`~/Qt/6.10.1/macos`) — the same install the app builds against.
- CMake + Ninja (already required to build the app).
- ~10–15 min for the one-time build.
- Optional: `brew install graphviz` enables GammaRay's object-graph export.

## Build & install

GammaRay is kept **outside the repo** so the checkout stays clean. Build it
against the project's Qt and install to `~/.local`:

```sh
git clone --depth 1 --branch v3.4.0 --recurse-submodules --shallow-submodules \
  https://github.com/KDAB/GammaRay.git ~/Development/tools/GammaRay

cmake -S ~/Development/tools/GammaRay -B ~/Development/tools/GammaRay/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.1/macos" \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local" \
  -DGAMMARAY_BUILD_DOCS=OFF -DBUILD_TESTING=OFF

cmake --build   ~/Development/tools/GammaRay/build
cmake --install ~/Development/tools/GammaRay/build
```

On macOS GammaRay installs as an **app bundle**, so the CLI launcher is *inside*
it (there is no `~/.local/bin/gammaray` by default):

```sh
GR=~/.local/GammaRay.app/Contents/MacOS/gammaray   # the CLI launcher
# optional convenience symlink (requires ~/.local/bin on PATH):
ln -sf ~/.local/GammaRay.app/Contents/MacOS/gammaray ~/.local/bin/gammaray
```

## Verify

The matching probe is present and the macOS injector works:

```sh
"$GR" --version            # GammaRay version 3.4.0 ...
"$GR" --list-probes        # qt6_10-arm64 (Qt 6.10 (release, arm64))   <- must match the app's Qt
"$GR" --self-test preload  # Injector preload successfully passed its self-test.
```

End-to-end — the probe actually loads into the app. Launch a **current** build
(see the stale-bundle note below) under the probe with a headless server, then
confirm the server is listening *inside* the app process:

```sh
APP="$PWD/build/c/OpenLiveReplay.app/Contents/MacOS/OpenLiveReplay"
"$GR" -i preload --inject-only --listen tcp://127.0.0.1:11732 "$APP" &
sleep 8
lsof -nP -iTCP:11732 -sTCP:LISTEN     # COMMAND should be OpenLiveR…  (server runs inside the target)
pkill -f "OpenLiveReplay.app/Contents/MacOS/OpenLiveReplay"
```

## Troubleshooting

- **`No probe found for ABI qt6_XX`, or a wrong Qt in `--list-probes`** — probe/Qt
  mismatch; rebuild against `~/Qt/6.10.1/macos`.
- **`Library not loaded: @rpath/librtmidi.7.dylib` → `Injector error: Process crashed`**
  — the app *bundle* is stale: its dynamic-library paths were baked at a different
  checkout path, not a GammaRay fault. Use a build configured at the current repo
  path, or rebuild per [CLAUDE.md](../CLAUDE.md).
- **`--pid` attach fails on macOS** — use launch mode; ensure a local unsigned Debug build.

## Uninstall

Everything is under `~/.local/GammaRay.app` (plus any `~/.local/bin` symlink) and
the source tree `~/Development/tools/GammaRay`. Remove those to uninstall. Nothing
in this repo depends on GammaRay at build or test time.
