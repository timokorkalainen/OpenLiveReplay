# SRT Test Infrastructure (Phase 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the test build able to ingest `srt://` locally (a native macOS ffmpeg+libsrt build the test engine links), and prove the real engine records one SRT stream end-to-end.

**Architecture:** A native `build_ffmpeg_macos_srt.sh` builds ffmpeg 8.0 `--enable-libsrt` against the already-brew-installed libsrt/openssl into a gitignored `macos_build/ffmpeg-srt/`. An opt-in `-DOLR_FFMPEG_SRT_PREFIX` makes the test engine + harnesses link it (default = brew, so CI is untouched). A local-only `e2e_srt_smoke` test pipes one flash/beep stream over SRT (via `srt-live-transmit`) into `record_harness` and asserts a valid MKV.

**Tech Stack:** ffmpeg-from-source, libsrt/openssl (brew), `srt-live-transmit`, CMake/CTest, the existing record_harness, bash/ffprobe.

**Spec:** `docs/superpowers/specs/2026-06-15-srt-test-infra-design.md`

**This is infra-and-build work, not unit-TDD** — each task's verification is "build/run and observe," not a failing unit test. Note the SRT build (Task 1) takes ~10 min the first time.

---

## File Structure

| File | Change |
|---|---|
| `build-scripts/build_ffmpeg_macos_srt.sh` | native ffmpeg+libsrt build → `macos_build/ffmpeg-srt/` |
| `.gitignore` | ignore `macos_build/` |
| `tests/CMakeLists.txt` | `OLR_FFMPEG_SRT_PREFIX` opt-in linkage + BUILD_RPATH |
| `tests/e2e/run_srt_smoke.sh` | one-stream SRT proof |
| `tests/e2e/CMakeLists.txt` | register `e2e_srt_smoke` under the `srt` label |
| `tests/e2e/SRT_README.md` | one-time build + run instructions |

**Two build dirs in play:**
- `build/claude-debug` — the normal brew-ffmpeg build (verify CI/UDP path unaffected).
- `build/srt` — configured with `-DOLR_FFMPEG_SRT_PREFIX` (the SRT path).

---

## Task 1: native ffmpeg+libsrt build script

**Files:**
- Create: `build-scripts/build_ffmpeg_macos_srt.sh`
- Modify: `.gitignore`

- [ ] **Step 1: Write `build-scripts/build_ffmpeg_macos_srt.sh`**

```bash
#!/bin/bash
# Build a NATIVE macOS ffmpeg with libsrt, for the LOCAL SRT e2e (not CI).
# Links the already-brew-installed libsrt + openssl@3 (no from-source of those,
# unlike the iOS build). Output: macos_build/ffmpeg-srt/{include,lib} with an
# SRT-enabled libavformat the test engine links via -DOLR_FFMPEG_SRT_PREFIX.
#
# --enable-nonfree (openssl) => this artifact is LOCAL-TEST-ONLY, not redistributable.
set -e

FFMPEG_VERSION="8.0"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK_DIR="$ROOT_DIR/macos_build"
SRC_DIR="$WORK_DIR/src"
DIST="$WORK_DIR/ffmpeg-srt"
TARBALL="$ROOT_DIR/ios_build/src/ffmpeg-${FFMPEG_VERSION}.tar.bz2"

# Idempotent: skip the ~10-min rebuild if already built.
if [ -f "$DIST/lib/libavformat.dylib" ]; then
    echo "[macos-srt] already built at $DIST; skipping (delete macos_build/ to force)."
    exit 0
fi

BREW="$(brew --prefix)"
SRT_PC="$BREW/opt/srt/lib/pkgconfig"
SSL_PC="$BREW/opt/openssl@3/lib/pkgconfig"
[ -f "$SRT_PC/srt.pc" ] || { echo "ERROR: brew libsrt missing — run 'brew install srt'"; exit 1; }
[ -f "$SSL_PC/openssl.pc" ] || { echo "ERROR: brew openssl@3 missing — run 'brew install openssl@3'"; exit 1; }
[ -f "$TARBALL" ] || { echo "ERROR: ffmpeg source tarball missing at $TARBALL"; exit 1; }

mkdir -p "$SRC_DIR"
if [ ! -d "$SRC_DIR/ffmpeg-${FFMPEG_VERSION}" ]; then
    echo "[macos-srt] extracting ffmpeg-${FFMPEG_VERSION} (fresh native tree)..."
    tar xf "$TARBALL" -C "$SRC_DIR"
fi
cd "$SRC_DIR/ffmpeg-${FFMPEG_VERSION}"
make distclean >/dev/null 2>&1 || true

echo "[macos-srt] configuring (native, --enable-libsrt)..."
PKG_CONFIG_PATH="$SRT_PC:$SSL_PC" ./configure \
    --prefix="$DIST" \
    --enable-gpl --enable-version3 --enable-nonfree \
    --disable-static --enable-shared \
    --disable-doc --disable-programs \
    --disable-avdevice --disable-indevs --disable-outdevs \
    --enable-libsrt --enable-openssl --enable-protocol=libsrt \
    --extra-cflags="-I$BREW/opt/srt/include -I$BREW/opt/openssl@3/include" \
    --extra-ldflags="-L$BREW/opt/srt/lib -L$BREW/opt/openssl@3/lib"

echo "[macos-srt] compiling..."
make -j"$(sysctl -n hw.ncpu)"
make install

# Rewrite ffmpeg's inter-library install names to @rpath so a consumer with an
# rpath to $DIST/lib loads them (libsrt/openssl keep their brew absolute paths).
LIBS=(libavcodec libavformat libavutil libswscale libswresample libavfilter)
for lib in "${LIBS[@]}"; do
    real="$(python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$DIST/lib/$lib.dylib")"
    [ -f "$real" ] || continue
    install_name_tool -id "@rpath/$lib.dylib" "$real"
    otool -L "$real" | grep "$DIST/lib" | awk '{print $1}' | while read -r dep; do
        base="$(basename "$dep")"
        name="$(echo "$base" | sed 's/\.[0-9]*\.dylib/.dylib/')"
        install_name_tool -change "$dep" "@rpath/$name" "$real" 2>/dev/null || true
    done
done

# Self-check: libsrt MUST be linked into libavformat, else SRT support is absent.
if ! otool -L "$DIST/lib/libavformat.dylib" | grep -qi "libsrt"; then
    echo "ERROR: built libavformat does NOT link libsrt — SRT support missing"
    exit 1
fi
echo "[macos-srt] OK — ffmpeg-srt built at $DIST (libavformat links libsrt)."
```

- [ ] **Step 2: Make it executable + ignore the build dir**

```bash
chmod +x build-scripts/build_ffmpeg_macos_srt.sh
```
Append `macos_build/` to `.gitignore` (after the `ios_build*` lines):
```
macos_build/
```

- [ ] **Step 3: Run the build (~10 min, one time)**

Run: `bash build-scripts/build_ffmpeg_macos_srt.sh`
Expected: ends with `[macos-srt] OK — ffmpeg-srt built at …/macos_build/ffmpeg-srt (libavformat links libsrt).`
If `./configure` fails: read its `ffbuild/config.log` tail; the usual cause is a pkg-config path — confirm `pkg-config --exists srt openssl` with `PKG_CONFIG_PATH="$SRT_PC:$SSL_PC"`. Report BLOCKED with the configure error if it can't be resolved.

- [ ] **Step 4: Verify the artifact**

Run:
```bash
ls macos_build/ffmpeg-srt/lib/libavformat.dylib && \
otool -L macos_build/ffmpeg-srt/lib/libavformat.dylib | grep -i srt
```
Expected: the dylib exists and the `otool -L` line shows `…/libsrt.1.5.dylib`.

- [ ] **Step 5: Commit** (the build OUTPUT is gitignored; only the script + .gitignore are committed)

```bash
git add build-scripts/build_ffmpeg_macos_srt.sh .gitignore
git commit -m "build(srt): native macOS ffmpeg+libsrt build for the local SRT e2e"
```

---

## Task 2: opt-in CMake linkage against the SRT ffmpeg

**Files:**
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add the `OLR_FFMPEG_SRT_PREFIX` option + rpath.** In `tests/CMakeLists.txt`, immediately AFTER the existing block that sets `OLR_FFMPEG_INCLUDE`/`OLR_FFMPEG_LIBDIR` from `OLR_BREW_PREFIX` (the lines around `set(OLR_FFMPEG_INCLUDE "${OLR_BREW_PREFIX}/include")` / `set(OLR_FFMPEG_LIBDIR "${OLR_BREW_PREFIX}/lib")`), add:

```cmake
# Opt-in: link the test engine + harnesses against a libsrt-enabled ffmpeg built
# by build-scripts/build_ffmpeg_macos_srt.sh, so the local SRT e2e can ingest
# srt://. Default unset -> brew ffmpeg, so CI and the UDP gates are unaffected.
set(OLR_FFMPEG_SRT_PREFIX "" CACHE PATH "Path to a libsrt-enabled ffmpeg install (local SRT e2e)")
if(OLR_FFMPEG_SRT_PREFIX)
    set(OLR_FFMPEG_INCLUDE "${OLR_FFMPEG_SRT_PREFIX}/include")
    set(OLR_FFMPEG_LIBDIR  "${OLR_FFMPEG_SRT_PREFIX}/lib")
    # The ffmpeg dylibs use @rpath install names; give every test executable an
    # rpath to the SRT lib dir so they resolve at runtime (no DYLD_LIBRARY_PATH).
    list(APPEND CMAKE_BUILD_RPATH "${OLR_FFMPEG_LIBDIR}")
    message(STATUS "OpenLiveReplay tests: SRT ffmpeg prefix = ${OLR_FFMPEG_SRT_PREFIX}")
endif()
```

- [ ] **Step 2: Configure an SRT build dir + build record_harness**

```bash
cd /tmp/olr-srt
cmake -S . -B build/srt -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja \
  -DCMAKE_BUILD_TYPE=Debug -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos \
  -DOLR_FFMPEG_SRT_PREFIX="$PWD/macos_build/ffmpeg-srt"
$HOME/Qt/Tools/Ninja/ninja -C build/srt record_harness
```
Expected: configures with `SRT ffmpeg prefix = …` in the output, builds cleanly.

- [ ] **Step 3: Verify record_harness links the SRT ffmpeg**

Run:
```bash
otool -L build/srt/tests/e2e/record_harness | grep -iE "avformat|srt"
otool -l build/srt/tests/e2e/record_harness | grep -A2 LC_RPATH | grep ffmpeg-srt
```
Expected: it references `@rpath/libavformat.dylib` (or the SRT prefix path) AND an `LC_RPATH` entry pointing at `…/macos_build/ffmpeg-srt/lib`. (This confirms the harness will load the SRT-enabled avformat at runtime.)

- [ ] **Step 4: Commit**

```bash
git add tests/CMakeLists.txt
git commit -m "build(srt): opt-in -DOLR_FFMPEG_SRT_PREFIX linkage for the test engine"
```

---

## Task 3: one-stream SRT smoke script

**Files:**
- Create: `tests/e2e/run_srt_smoke.sh`

- [ ] **Step 1: Write `tests/e2e/run_srt_smoke.sh`**

```bash
#!/usr/bin/env bash
# Local SRT e2e: prove the real engine records one srt:// stream end-to-end.
#
# Stands up an SRT LISTENER carrying a flash/beep MPEG-TS via the brew ffmpeg
# (UDP) + the already-installed srt-live-transmit (UDP->SRT). record_harness
# connects as an SRT CALLER (the engine's default) and we assert the MKV is
# valid. Requires the harness to be built with -DOLR_FFMPEG_SRT_PREFIX (i.e. an
# SRT-enabled avformat); otherwise the engine cannot open srt:// and this FAILS.
#
# Usage: run_srt_smoke.sh <record_harness_exe> [srt_port]
set -uo pipefail

HARNESS="${1:?record_harness executable path required}"
SRT_PORT="${2:-23501}"
UDP_PORT=$((SRT_PORT + 1))
SECONDS_TO_RECORD=6

command -v ffmpeg  >/dev/null || { echo "SKIP: ffmpeg not found";  exit 0; }
command -v ffprobe >/dev/null || { echo "SKIP: ffprobe not found"; exit 0; }
command -v srt-live-transmit >/dev/null || { echo "SKIP: srt-live-transmit not found (brew install srt)"; exit 0; }

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT
is_num() { case "${1:-}" in '' | *[!0-9.]*) return 1 ;; *) return 0 ;; esac; }

echo "[srt-e2e] srt_port=$SRT_PORT udp_port=$UDP_PORT"

# 1. SRT listener carrying flash/beep MPEG-TS: ffmpeg(UDP) -> srt-live-transmit(SRT listener).
ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=640x480:rate=30" \
    -f lavfi -i "sine=frequency=1000:sample_rate=48000" -ac 2 \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M \
    -c:a aac -b:a 128k \
    -f mpegts "udp://127.0.0.1:${UDP_PORT}?pkt_size=1316" &
PIDS+=($!)
srt-live-transmit "udp://127.0.0.1:${UDP_PORT}?mode=listener" \
    "srt://127.0.0.1:${SRT_PORT}?mode=listener&transtype=live&latency=200" >/dev/null 2>&1 &
PIDS+=($!)
sleep 1.0  # let the producer + SRT listener come up before the caller connects

# 2. The real engine connects as SRT caller and records.
URL="srt://127.0.0.1:${SRT_PORT}?transtype=live"
OUT="$("$HARNESS" --url "$URL" --name olr_srt_smoke --outdir "$WORKDIR" \
       --seconds "$SECONDS_TO_RECORD" --width 640 --height 480 --fps 30)"
RC=$?
OUT_MKV="$(printf '%s\n' "$OUT" | tail -n 1)"
echo "[srt-e2e] harness rc=$RC out=$OUT_MKV"

if [ $RC -ne 0 ] || [ -z "$OUT_MKV" ] || [ ! -s "$OUT_MKV" ]; then
    echo "FAIL: SRT ingest produced no output (rc=$RC) — engine could not record srt:// (is the harness built with -DOLR_FFMPEG_SRT_PREFIX?)"
    exit 1
fi

# 3. Assert the MKV is valid (same checks as run_record_e2e.sh).
scalar() { ffprobe -v error "$@" -of default=noprint_wrappers=1:nokey=1 "$OUT_MKV" | head -n1; }
V_PACKETS="$(scalar -select_streams v:0 -count_packets -show_entries stream=nb_read_packets)"
A_CHANNELS="$(scalar -select_streams a:0 -show_entries stream=channels)"
echo "[srt-e2e] video_packets=${V_PACKETS:-?} audio_channels=${A_CHANNELS:-?}"

fail=0
MIN_FRAMES=$((30 * SECONDS_TO_RECORD / 2))
if ! is_num "${V_PACKETS:-}" || [ "${V_PACKETS%.*}" -lt "$MIN_FRAMES" ]; then
    echo "FAIL: too few/invalid video frames over SRT (got '${V_PACKETS:-none}', need >= ${MIN_FRAMES})"; fail=1
fi
if [ "${A_CHANNELS:-}" != "2" ]; then
    echo "FAIL: expected stereo audio over SRT, got '${A_CHANNELS:-none}'"; fail=1
fi
[ $fail -ne 0 ] && exit 1
echo "PASS: e2e SRT ingest — ${V_PACKETS} frames, stereo audio recorded from srt://"
exit 0
```

- [ ] **Step 2: Make executable + run it against the SRT harness**

```bash
cd /tmp/olr-srt
chmod +x tests/e2e/run_srt_smoke.sh
bash -n tests/e2e/run_srt_smoke.sh
bash tests/e2e/run_srt_smoke.sh build/srt/tests/e2e/record_harness 23501
```
Expected: `PASS: e2e SRT ingest — <N> frames, stereo audio recorded from srt://`.
**Teeth-check (prove it really exercises SRT):** run the SAME script against the BREW-ffmpeg harness (`build/claude-debug/tests/e2e/record_harness`, which lacks libsrt) — it MUST `FAIL` (the engine can't open `srt://`). This confirms the test genuinely depends on the SRT-enabled build. Report both results.
(If `build/claude-debug/.../record_harness` doesn't exist yet, configure+build it first with the normal brew flags.)

- [ ] **Step 3: Commit**

```bash
git add tests/e2e/run_srt_smoke.sh
git commit -m "test(srt): one-stream SRT smoke — srt:// in, valid MKV out"
```

---

## Task 4: register the `srt` CTest (local-only)

**Files:**
- Modify: `tests/e2e/CMakeLists.txt`

- [ ] **Step 1: Register `e2e_srt_smoke` under the `srt` label.** Append to `tests/e2e/CMakeLists.txt`:

```cmake
# --- Local-only SRT e2e (NOT in CI) -----------------------------------------
# Proves the real engine ingests srt://. Requires a build configured with
# -DOLR_FFMPEG_SRT_PREFIX (an SRT-enabled ffmpeg); under the default brew ffmpeg
# the engine cannot open srt:// and this fails — by design. The distinct "srt"
# label keeps it out of CI's e2e selection. Run locally with: ctest -L srt
add_test(NAME e2e_srt_smoke
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_smoke.sh" "$<TARGET_FILE:record_harness>" 23501)
set_tests_properties(e2e_srt_smoke PROPERTIES LABELS "srt" TIMEOUT 120 RUN_SERIAL TRUE)
```

- [ ] **Step 2: Reconfigure the SRT build + run via CTest**

```bash
cd /tmp/olr-srt
cmake -S . -B build/srt -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja \
  -DCMAKE_BUILD_TYPE=Debug -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos \
  -DOLR_FFMPEG_SRT_PREFIX="$PWD/macos_build/ffmpeg-srt"
$HOME/Qt/Tools/Ninja/ninja -C build/srt record_harness
( cd build/srt && ctest -N -L srt )                 # lists e2e_srt_smoke only
( cd build/srt && ctest -L srt --output-on-failure ) # passes
```
Expected: `e2e_srt_smoke` is the only `-L srt` test, and it passes.

- [ ] **Step 3: Verify the default build does NOT run it in the gating set.** In the normal brew build:
```bash
cd /tmp/olr-srt
cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
( cd build/claude-debug && ctest -N -LE 'srt|sync-report' | tail -3 )
```
Expected: the gating list is unchanged (no `e2e_srt_smoke`); `e2e_srt_smoke` only appears under `-L srt`. (CI runs `ctest -LE sync-report` already; confirm adding `e2e_srt_smoke` to the `srt` label keeps it out of CI by noting CI does NOT pass `-L srt` and the test fails under brew ffmpeg by design — so it must be excluded. **Update `ci.yml` to also exclude the `srt` label.**)

- [ ] **Step 4: Exclude `srt` from CI (`.github/workflows/ci.yml`).** Both `ctest` invocations currently end with `-LE sync-report`. Change BOTH to `-LE 'sync-report|srt'` so the SRT test (which fails under CI's brew ffmpeg by design) never runs in CI:
   - the Build+Test job's `ctest --output-on-failure --repeat until-pass:2 -LE sync-report` → `… -LE 'sync-report|srt'`
   - the sanitizer job's `ctest … -E 'e2e_play_(...)' -LE sync-report` → `… -LE 'sync-report|srt'`

- [ ] **Step 5: Commit**

```bash
git add tests/e2e/CMakeLists.txt .github/workflows/ci.yml
git commit -m "test(srt): register e2e_srt_smoke under the local-only srt label; exclude from CI"
```

---

## Task 5: docs

**Files:**
- Create: `tests/e2e/SRT_README.md`

- [ ] **Step 1: Write `tests/e2e/SRT_README.md`**

````markdown
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
and asserts the engine records a valid MKV from it. Without
`-DOLR_FFMPEG_SRT_PREFIX` the engine's avformat lacks SRT and the test fails by
design.

## Next (Phase 2)

A 4-source SRT framework (per-camera identity; inter-camera sync, per-source
trim, audio latency, connection-status, disconnect/loss injection) builds on this
infra — see `docs/superpowers/specs/`.
````

- [ ] **Step 2: Commit**

```bash
git add tests/e2e/SRT_README.md
git commit -m "docs(srt): local SRT e2e setup + run instructions"
```

---

## Task 6: final verification + PR (do NOT merge)

**Files:** none (process)

- [ ] **Step 1: Confirm the default (brew) gating suite is unaffected.**
```bash
cd /tmp/olr-srt/build/claude-debug && ctest --output-on-failure --repeat until-pass:2 -LE 'sync-report|srt'
```
Expected: the existing unit + e2e suite passes (10 e2e + units), no `e2e_srt_smoke` among them.

- [ ] **Step 2: clang-format check (only `.sh`/`.md`/`.cmake`/`.txt` changed — no C++; confirm nothing to format).**
```bash
cd /tmp/olr-srt
GCF=/opt/homebrew/opt/llvm/bin/git-clang-format CF=/opt/homebrew/opt/llvm/bin/clang-format
"$GCF" --binary "$CF" --commit "$(git merge-base origin/main HEAD)" --diff --extensions cpp,h,hpp,mm,c 2>&1 | tail -1
```
Expected: `no modified files to format` / `did not modify any files` (this branch changes no C++).

- [ ] **Step 3: Push + open PR.**
```bash
SKIP_IOS_BUILD=1 git push -u origin feat/srt-test-infra
gh pr create --base main --title "SRT test infrastructure (Phase 1): real srt:// e2e" --body "<summary: native macOS ffmpeg+libsrt build; opt-in -DOLR_FFMPEG_SRT_PREFIX; local-only e2e_srt_smoke proving the engine ingests srt://; CI untouched (srt label excluded); links spec/plan; note Phase 2 = 4-source framework>"
```

- [ ] **Step 4: Watch CI green** (`gh pr checks <n>`). Since this branch changes only build/test/CI scaffolding (no C++, and the `srt` test is excluded from CI), Build+Test/Lint/sanitizers should pass on the brew path. Leave unmerged (per the active no-merge hold).

---

## Self-Review

**Spec coverage:**
- §3.1 build script (native, brew libsrt/openssl, @rpath, self-check, idempotent) → Task 1. ✓
- §3.2 CMake `OLR_FFMPEG_SRT_PREFIX` + BUILD_RPATH → Task 2. ✓
- §3.3 `run_srt_smoke.sh` (srt-live-transmit listener → record_harness srt:// → MKV asserts; SKIP/FAIL handling) → Task 3. ✓
- §3.4 CTest `srt` label, distinct from `e2e`, CI-excluded → Task 4 (incl. the ci.yml `-LE 'sync-report|srt'` exclusion the spec implied). ✓
- §3.5 SRT_README.md → Task 5. ✓
- §4 error handling (prefix unset, missing tools SKIP, libsrt self-check) → Task 1 self-check + Task 3 SKIP/FAIL. ✓
- §5 testing (the smoke IS the test; default build unchanged) → Task 3 + Task 6 Step 1. ✓

**Placeholder scan:** the PR body (Task 6) is an intentional fill-at-time summary; all script/CMake steps are complete. No TBD/TODO.

**Type/name consistency:** `OLR_FFMPEG_SRT_PREFIX` (CMake option, Tasks 2/4/5), `macos_build/ffmpeg-srt` (output, Tasks 1/2/4/5), `build_ffmpeg_macos_srt.sh`, `run_srt_smoke.sh`, `e2e_srt_smoke` + `srt` label (Tasks 3/4/6), `build/srt` vs `build/claude-debug` — consistent throughout.

**Critical-path risk (flagged):** Task 1's ffmpeg `./configure`/build is the main risk (toolchain/pkg-config). Mitigations in Task 1 Step 3 (config.log inspection, pkg-config check) and the self-check that fails on a libsrt-less build. Task 3's teeth-check (brew harness must FAIL) confirms the test genuinely exercises SRT.
