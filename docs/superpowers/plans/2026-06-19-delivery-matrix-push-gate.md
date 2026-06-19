# Delivery-matrix Push Gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `git push` an always-on blocking gate that builds the current platform and runs a 4-cell transport×codec delivery matrix (RTMP/SRT × h264/h265, 5-second clips), hard-failing when required tooling is missing.

**Architecture:** A new `delivery-gate` CTest label tags exactly the four delivery smokes (three existing + one macOS wiring of an existing cross-platform SRT-HEVC script). The four scripts gain an `OLR_E2E_CLIP_SECONDS` duration override. The pre-push hook is rewritten from advisory-only into: skip-check → tooling preflight (hard-fail) → host build → `ctest -L delivery-gate` (blocking), with `OLR_PREPUSH_FULL` kept as the heavier superset gate.

**Tech Stack:** Bash (git hook + e2e driver scripts), CMake/CTest, ffmpeg/ffprobe/srt-live-transmit, Qt 6 host kit, Ninja.

## Global Constraints

- **Platform this iteration:** macOS only (the `if(APPLE)` blocks in `tests/e2e/CMakeLists.txt`). The label mechanism is platform-agnostic; do not touch the `elseif(WIN32)` branches.
- **The 4 delivery cells, exactly:** `e2e_native_rtmp_smoke`, `e2e_native_rtmp_hevc_smoke`, `e2e_native_srt_smoke`, `e2e_native_srt_hevc_smoke`.
- **New label name (verbatim):** `delivery-gate`.
- **New env vars (verbatim):** `OLR_E2E_CLIP_SECONDS` (clip length, gate default `5`), `OLR_PREPUSH_SKIP` (`=1` bypasses the gate).
- **Clip default for the gate:** 5 seconds. Each script's own standalone default must stay its current value (rtmp h264=7, rtmp hevc=4, srt h264=6, srt hevc=6).
- **New macOS SRT-HEVC port:** `23740` (disjoint from record 23456–23459, playback 23464–23497, sync 23480–23489, av-sync 23492, SRT native 23601–23730, RTMP 23760–23800).
- **Qt host kit default:** `$QT_HOST_PREFIX` → `~/Qt/6.10.1/macos`.
- **Host build dir:** `build/prepush-delivery` (use a fresh dir if you hit stale `build/Qt_*` cache errors).
- **Encoder acceptance (must match the scripts):** H.264 = `libx264` or `h264_mf` only; HEVC = `libx265` or any `hevc_*`.
- Do not modify `.github/workflows/ci.yml`.

---

### Task 1: Add `delivery-gate` label and wire the macOS SRT-HEVC cell

**Files:**
- Modify: `tests/e2e/CMakeLists.txt` (RTMP label lines ~420-429; APPLE SRT block after line 280)

**Interfaces:**
- Consumes: existing CTest cases `e2e_native_rtmp_smoke`, `e2e_native_rtmp_hevc_smoke`, `e2e_native_srt_smoke`; existing script `tests/e2e/run_srt_hevc_smoke.sh` (cross-platform, takes `<record_harness_exe> [srt_port]`).
- Produces: a CTest label `delivery-gate` resolving to exactly 4 tests on macOS, including a new `e2e_native_srt_hevc_smoke` registered under `if(APPLE)`.

- [ ] **Step 1: Configure once, confirm the label is empty today**

```bash
cmake -S . -B build/prepush-delivery -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.1/macos" \
    -DOLR_BUILD_TESTS=ON >/dev/null
ctest --test-dir build/prepush-delivery -L delivery-gate -N
```
Expected: `Total Tests: 0` (label does not exist yet).

- [ ] **Step 2: Add the label to the two RTMP cells**

In `tests/e2e/CMakeLists.txt`, change the RTMP smoke properties:

```cmake
    set_tests_properties(e2e_native_rtmp_smoke PROPERTIES
        LABELS "native-rtmp;delivery-gate"
        TIMEOUT 180
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77)
```

and the RTMP HEVC smoke properties:

```cmake
    set_tests_properties(e2e_native_rtmp_hevc_smoke PROPERTIES
        LABELS "native-rtmp;delivery-gate"
        TIMEOUT 180
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77)
```

- [ ] **Step 3: In the `if(APPLE)` block, re-label the SRT h264 smoke and register the SRT HEVC cell**

Insert this immediately after the `olr_add_native_srt_e2e_tests("native-apple-ingest")` line (before the `e2e_native_srt_reconnect` test):

```cmake
    # --- Delivery push-gate cells (RTMP/SRT x h264/h265) ---------------------
    # Re-tag the SRT h264 smoke with the delivery-gate label (the function above
    # set it to "native-apple-ingest"; LABELS is replaced, not appended, so list
    # both). TIMEOUT/RUN_SERIAL set by the function are untouched.
    set_tests_properties(e2e_native_srt_smoke PROPERTIES
        LABELS "native-apple-ingest;delivery-gate")

    # SRT HEVC (h265) cell on macOS. run_srt_hevc_smoke.sh already exists and is
    # cross-platform (auto-selects libx265 / hevc_videotoolbox); only Windows had
    # registered it. Port 23740 sits in the gap between the SRT native band
    # (<=23730) and the RTMP band (>=23760).
    add_test(NAME e2e_native_srt_hevc_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_hevc_smoke.sh" "$<TARGET_FILE:record_harness>" 23740)
    set_tests_properties(e2e_native_srt_hevc_smoke PROPERTIES
        LABELS "native-apple-ingest;delivery-gate"
        TIMEOUT 180
        RUN_SERIAL TRUE)
```

- [ ] **Step 4: Re-configure and confirm the label resolves to exactly the 4 cells**

```bash
cmake -S . -B build/prepush-delivery -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.1/macos" \
    -DOLR_BUILD_TESTS=ON >/dev/null
ctest --test-dir build/prepush-delivery -L delivery-gate -N
```
Expected: `Total Tests: 4`, listing `e2e_native_rtmp_smoke`, `e2e_native_rtmp_hevc_smoke`, `e2e_native_srt_smoke`, `e2e_native_srt_hevc_smoke` (order may vary).

- [ ] **Step 5: Commit**

```bash
git add tests/e2e/CMakeLists.txt
git commit -m "test(e2e): add delivery-gate label + wire macOS SRT-HEVC cell"
```

---

### Task 2: Add `OLR_E2E_CLIP_SECONDS` override to the four delivery scripts

**Files:**
- Modify: `tests/e2e/run_rtmp_smoke.sh` (line 17)
- Modify: `tests/e2e/run_rtmp_hevc_smoke.sh` (line 6)
- Modify: `tests/e2e/run_srt_smoke.sh` (`SECONDS_TO_RECORD=6` line)
- Modify: `tests/e2e/run_srt_hevc_smoke.sh` (line 16)

**Interfaces:**
- Consumes: optional env var `OLR_E2E_CLIP_SECONDS`.
- Produces: each script records for `OLR_E2E_CLIP_SECONDS` seconds when set; otherwise its current per-script default. Frame-count assertions already derive from `SECONDS_TO_RECORD`, so they scale automatically.

- [ ] **Step 1: Confirm none honor the override yet**

```bash
grep -n 'SECONDS_TO_RECORD=' tests/e2e/run_rtmp_smoke.sh tests/e2e/run_rtmp_hevc_smoke.sh tests/e2e/run_srt_smoke.sh tests/e2e/run_srt_hevc_smoke.sh
```
Expected: four hardcoded assignments (`=7`, `=4`, `=6`, `=6`), none referencing `OLR_E2E_CLIP_SECONDS`.

- [ ] **Step 2: Edit each assignment to honor the env var (default preserved)**

`tests/e2e/run_rtmp_smoke.sh`:
```bash
SECONDS_TO_RECORD="${OLR_E2E_CLIP_SECONDS:-7}"
```
`tests/e2e/run_rtmp_hevc_smoke.sh`:
```bash
SECONDS_TO_RECORD="${OLR_E2E_CLIP_SECONDS:-4}"
```
`tests/e2e/run_srt_smoke.sh`:
```bash
SECONDS_TO_RECORD="${OLR_E2E_CLIP_SECONDS:-6}"
```
`tests/e2e/run_srt_hevc_smoke.sh`:
```bash
SECONDS_TO_RECORD="${OLR_E2E_CLIP_SECONDS:-6}"
```

- [ ] **Step 3: Static check — confirm the override is wired in all four**

```bash
grep -c 'SECONDS_TO_RECORD="${OLR_E2E_CLIP_SECONDS:-' \
    tests/e2e/run_rtmp_smoke.sh tests/e2e/run_rtmp_hevc_smoke.sh \
    tests/e2e/run_srt_smoke.sh tests/e2e/run_srt_hevc_smoke.sh
bash -n tests/e2e/run_rtmp_smoke.sh && bash -n tests/e2e/run_rtmp_hevc_smoke.sh \
    && bash -n tests/e2e/run_srt_smoke.sh && bash -n tests/e2e/run_srt_hevc_smoke.sh \
    && echo "syntax OK"
```
Expected: each file prints `1`, then `syntax OK` (no bash parse errors).

- [ ] **Step 4: Runtime check — a 5s clip actually records ~5s (one fast cell)**

Build the harness first if not already built, then run the RTMP h264 cell with the override and probe the output duration:

```bash
cmake --build build/prepush-delivery --target record_harness
OLR_E2E_CLIP_SECONDS=5 ctest --test-dir build/prepush-delivery \
    -R '^e2e_native_rtmp_smoke$' --output-on-failure
```
Expected: PASS, and the `[rtmp-e2e]` log reports `video_packets` ≈ `30*5` (≥ `MIN_FRAMES=75`), confirming the clip honored 5 seconds rather than the old 7.

- [ ] **Step 5: Commit**

```bash
git add tests/e2e/run_rtmp_smoke.sh tests/e2e/run_rtmp_hevc_smoke.sh \
    tests/e2e/run_srt_smoke.sh tests/e2e/run_srt_hevc_smoke.sh
git commit -m "test(e2e): honor OLR_E2E_CLIP_SECONDS in delivery smokes"
```

---

### Task 3: Rewrite the pre-push hook into the always-on delivery gate

**Files:**
- Modify (full rewrite): `.githooks/pre-push`

**Interfaces:**
- Consumes: env vars `OLR_PREPUSH_SKIP`, `OLR_PREPUSH_FULL`, `OLR_E2E_CLIP_SECONDS`, `QT_HOST_PREFIX`/`QT_ROOT_DIR`, `QT_IOS_PREFIX`, `SKIP_FULL_TESTS`, `SKIP_IOS_BUILD`; the `delivery-gate` CTest label (Task 1); the clip override (Task 2).
- Produces: a blocking pre-push gate. Default path builds the host and runs the delivery matrix; `OLR_PREPUSH_FULL=1` additionally runs the full CTest matrix + iOS build; `OLR_PREPUSH_SKIP=1` bypasses.

- [ ] **Step 1: Confirm the hook is advisory-only today**

```bash
echo "" | bash .githooks/pre-push test-remote test-url; echo "exit=$?"
```
Expected: prints the `[pre-push] Recommendation only (not a gate)` text and `exit=0` (it gates nothing).

- [ ] **Step 2: Replace the entire file with the gate implementation**

Write `.githooks/pre-push` with exactly this content:

```bash
#!/usr/bin/env bash
# Pre-push hook: an ALWAYS-ON blocking delivery gate. Every push builds the
# current-platform host (engine + app + test harnesses) and runs the
# "delivery-gate" CTest matrix -- the transport x codec delivery smokes
# (RTMP/SRT x h264/h265) with short clips. A build or matrix failure blocks the
# push.
#
# The gate HARD-FAILS the push if required local tooling is missing (it does NOT
# silently skip): ffmpeg, ffprobe, srt-live-transmit, a Python 3, an H.264 and an
# HEVC encoder in ffmpeg, and the Qt host kit. Install on macOS with:
#   brew install ffmpeg srt
#
# GitHub PR CI runs only the short "ci" CTest label and does not build iOS or run
# these transport e2e gates (the from-source FFmpeg/SRT build OOM-kills hosted
# runners), so the delivery matrix is gated locally here instead.
#
# Triggers:
#   git push                 -> build host + delivery matrix (BLOCKING default)
#   OLR_PREPUSH_SKIP=1 push  -> skip the gate entirely (git push --no-verify too)
#   OLR_PREPUSH_FULL=1 push  -> ALSO run the full local CTest matrix + iOS build
#                               (skip parts: SKIP_FULL_TESTS=1 / SKIP_IOS_BUILD=1)
#
# Tunables:
#   OLR_E2E_CLIP_SECONDS  (default 5)               delivery-matrix clip length
#   QT_HOST_PREFIX        (default ~/Qt/6.10.1/macos)
#   QT_IOS_PREFIX         (default ~/Qt/6.10.1/ios)  used only by OLR_PREPUSH_FULL
#
# Enable the hook once per clone:  git config core.hooksPath .githooks
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

if [ "${OLR_PREPUSH_SKIP:-0}" = "1" ]; then
    echo "[pre-push] OLR_PREPUSH_SKIP=1 - skipping the delivery gate. Push proceeding." >&2
    exit 0
fi

QT_HOST_PREFIX="${QT_HOST_PREFIX:-${QT_ROOT_DIR:-$HOME/Qt/6.10.1/macos}}"
CLIP_SECONDS="${OLR_E2E_CLIP_SECONDS:-5}"
DELIVERY_BUILD_DIR="${OLR_PREPUSH_DELIVERY_BUILD_DIR:-build/prepush-delivery}"

# --- 1. Preflight: hard-fail (block the push) if delivery tooling is missing ---
preflight_fail=0
note_missing() { echo "[pre-push] MISSING: $1" >&2; preflight_fail=1; }

for tool in ffmpeg ffprobe srt-live-transmit; do
    command -v "$tool" >/dev/null 2>&1 || note_missing "$tool not found on PATH"
done

if ! { command -v python3 >/dev/null 2>&1 || command -v python >/dev/null 2>&1 \
        || command -v py >/dev/null 2>&1; }; then
    note_missing "Python 3 not found (needed by the RTMP fixture server)"
fi

if command -v ffmpeg >/dev/null 2>&1; then
    ENCODERS="$(ffmpeg -hide_banner -encoders 2>/dev/null || true)"
    # H.264: the scripts accept only libx264 or h264_mf (see olr_h264_vcodec_args).
    if ! printf '%s\n' "$ENCODERS" | grep -Eq '(^|[[:space:]])(libx264|h264_mf)[[:space:]]'; then
        note_missing "no usable H.264 encoder (need libx264 or h264_mf) in ffmpeg"
    fi
    # HEVC: the scripts accept libx265 or any hevc_* encoder.
    if ! printf '%s\n' "$ENCODERS" | grep -Eq '(^|[[:space:]])(libx265|hevc_[a-z]+)[[:space:]]'; then
        note_missing "no usable HEVC encoder (need libx265 or hevc_*) in ffmpeg"
    fi
fi

[ -d "$QT_HOST_PREFIX" ] || note_missing "Qt host kit not found at $QT_HOST_PREFIX"

if [ "$preflight_fail" -ne 0 ]; then
    cat >&2 <<EOF
[pre-push] Delivery gate cannot run -- required tooling is missing (see above).
[pre-push]   macOS install:  brew install ffmpeg srt
[pre-push]   Qt host kit:    set QT_HOST_PREFIX (currently $QT_HOST_PREFIX)
[pre-push] Emergency bypass: OLR_PREPUSH_SKIP=1 git push   (or git push --no-verify)
EOF
    exit 1
fi

# --- 2. Build the current-platform host (engine + app + test harnesses) -------
echo "[pre-push] Building current-platform host in ${DELIVERY_BUILD_DIR}..."
cmake -S . -B "$DELIVERY_BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH="$QT_HOST_PREFIX" \
    -DOLR_BUILD_TESTS=ON >/dev/null
cmake --build "$DELIVERY_BUILD_DIR"

# --- 3. Sanity: the delivery-gate label must resolve to exactly 4 cells --------
EXPECTED_CELLS=4
ACTUAL_CELLS="$(ctest --test-dir "$DELIVERY_BUILD_DIR" -L delivery-gate -N 2>/dev/null \
    | awk '/Total Tests:/ {print $NF}')"
if [ "${ACTUAL_CELLS:-0}" -ne "$EXPECTED_CELLS" ]; then
    echo "[pre-push] Delivery gate misconfigured: expected ${EXPECTED_CELLS} cells, found ${ACTUAL_CELLS:-0}." >&2
    exit 1
fi

# --- 4. Run the delivery matrix (RTMP/SRT x h264/h265) as a BLOCKING gate ------
echo "[pre-push] Running delivery matrix (clip=${CLIP_SECONDS}s) -- RTMP/SRT x h264/h265..."
OLR_E2E_CLIP_SECONDS="$CLIP_SECONDS" \
    ctest --test-dir "$DELIVERY_BUILD_DIR" -L delivery-gate \
        --output-on-failure --repeat until-pass:2 --no-tests=error
echo "[pre-push] Delivery matrix OK."

# --- 5. Optional heavier gate: full local CTest matrix + iOS build -------------
if [ "${OLR_PREPUSH_FULL:-0}" != "1" ]; then
    exit 0
fi

echo "[pre-push] OLR_PREPUSH_FULL=1 - running full local CTest matrix + iOS build."

if [ "${SKIP_FULL_TESTS:-0}" = "1" ]; then
    echo "[pre-push] SKIP_FULL_TESTS=1 - skipping full local CTest gate."
else
    TEST_BUILD_DIR="${OLR_PREPUSH_TEST_BUILD_DIR:-build/prepush-tests}"
    CTEST_JOBS="${CTEST_PARALLEL_LEVEL:-$(sysctl -n hw.ncpu 2>/dev/null || echo 2)}"

    echo "[pre-push] Running full local CTest gate in ${TEST_BUILD_DIR}..."
    cmake -S . -B "$TEST_BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_PREFIX_PATH="$QT_HOST_PREFIX" \
        -DOLR_BUILD_TESTS=ON >/dev/null
    cmake --build "$TEST_BUILD_DIR"
    ctest --test-dir "$TEST_BUILD_DIR" --output-on-failure \
        --repeat until-pass:2 --parallel "$CTEST_JOBS" \
        -LE 'sync-report|srt|native-apple-ingest'
    echo "[pre-push] Full local CTest gate OK."
fi

if [ "${SKIP_IOS_BUILD:-0}" = "1" ]; then
    echo "[pre-push] SKIP_IOS_BUILD=1 - skipping iOS build."
    exit 0
fi

# Locate the Qt iOS kit. Override via env if your Qt lives elsewhere, e.g.
# QT_IOS_PREFIX=~/Qt/6.9.0/ios.
QT_IOS_PREFIX="${QT_IOS_PREFIX:-$HOME/Qt/6.10.1/ios}"
QT_CMAKE="$QT_IOS_PREFIX/bin/qt-cmake"

if [ ! -x "$QT_CMAKE" ]; then
    echo "[pre-push] Qt iOS kit not found at $QT_IOS_PREFIX - skipping iOS build."
    echo "[pre-push] (set QT_IOS_PREFIX/QT_HOST_PREFIX to enable; push allowed.)"
    exit 0
fi

echo "[pre-push] Building iOS (FFmpeg deps cached in ios_build/xcframeworks; first run ~20 min)..."
BUILD_DIR="build/ios-prepush"

"$QT_CMAKE" -S . -B "$BUILD_DIR" -G Xcode \
    -DQT_HOST_PATH="$QT_HOST_PREFIX" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null

if cmake --build "$BUILD_DIR" --config Debug -- \
        CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO; then
    echo "[pre-push] iOS build OK - push proceeding."
else
    echo "[pre-push] iOS build FAILED - push aborted (SKIP_IOS_BUILD=1 to override)." >&2
    exit 1
fi
```

- [ ] **Step 3: Verify the skip path is fast and exits 0**

```bash
echo "" | OLR_PREPUSH_SKIP=1 bash .githooks/pre-push test-remote test-url; echo "exit=$?"
```
Expected: prints `[pre-push] OLR_PREPUSH_SKIP=1 - skipping the delivery gate.` and `exit=0`; no build runs.

- [ ] **Step 4: Verify preflight hard-fails fast when tooling is missing**

Run with a PATH that hides `srt-live-transmit` (and everything else), so preflight fails before any build:

```bash
echo "" | PATH=/usr/bin bash .githooks/pre-push test-remote test-url; echo "exit=$?"
```
Expected: one or more `[pre-push] MISSING: ...` lines, the install/bypass block, and `exit=1`. No `Building current-platform host` line appears (preflight precedes the build).

- [ ] **Step 5: Verify bash syntax**

```bash
bash -n .githooks/pre-push && echo "syntax OK"
```
Expected: `syntax OK`.

- [ ] **Step 6: Commit**

```bash
git add .githooks/pre-push
git commit -m "build(hooks): always-on delivery-matrix push gate (preflight + build + ctest)"
```

---

### Task 4: Document the gate in tests/README.md

**Files:**
- Modify: `tests/README.md` (near the label/transport sections)

**Interfaces:**
- Consumes: the `delivery-gate` label, the env vars, and the hook behavior from Tasks 1–3.
- Produces: developer-facing documentation of the push gate, its tooling, and how to bypass / tune it.

- [ ] **Step 1: Confirm the gate is undocumented today**

```bash
grep -n 'delivery-gate\|OLR_PREPUSH_SKIP\|OLR_E2E_CLIP_SECONDS' tests/README.md; echo "exit=$?"
```
Expected: no matches (`exit=1`).

- [ ] **Step 2: Add a "Delivery push gate" subsection**

Insert this block in `tests/README.md` after the paragraph that describes the local-only transport labels (`native-apple-ingest`, `native-ndi`, `native-rtmp`):

```markdown
### Delivery push gate (always-on)

`git push` runs a blocking pre-push gate (`.githooks/pre-push`): it builds the
current-platform host and runs the `delivery-gate` CTest label — the four
transport×codec delivery smokes with 5-second clips:

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

Tunables / bypass:
- `OLR_E2E_CLIP_SECONDS` — clip length for the matrix (gate default `5`).
- `OLR_PREPUSH_SKIP=1 git push` — bypass the gate (so does `git push --no-verify`).
- `OLR_PREPUSH_FULL=1 git push` — also run the full local CTest matrix + iOS build.
```

- [ ] **Step 3: Confirm the docs now reference the gate**

```bash
grep -c 'delivery-gate' tests/README.md
```
Expected: `≥ 2`.

- [ ] **Step 4: Commit**

```bash
git add tests/README.md
git commit -m "docs(tests): document the delivery-matrix push gate"
```

---

### Task 5: End-to-end gate verification (integration)

**Files:** none (verification only).

**Interfaces:**
- Consumes: the complete gate from Tasks 1–4.

- [ ] **Step 1: Run the real gate exactly as `git push` would**

```bash
echo "" | bash .githooks/pre-push test-remote test-url; echo "exit=$?"
```
Expected (on a tooled macOS machine): preflight passes silently → `Building current-platform host` → the misconfig sanity check passes → `Running delivery matrix (clip=5s)` → all 4 cells PASS → `Delivery matrix OK.` → `exit=0`. Wall-clock ~1–2 min steady-state (longer on a cold build).

- [ ] **Step 2: Confirm exactly the 4 cells ran (no silent skips)**

```bash
OLR_E2E_CLIP_SECONDS=5 ctest --test-dir build/prepush-delivery \
    -L delivery-gate --output-on-failure --repeat until-pass:2 --no-tests=error
```
Expected: `100% tests passed, 0 tests failed out of 4`, and no test marked `***Skipped` / `Skipped`.

- [ ] **Step 3: Confirm the FULL superset still wires up**

Smoke only the wiring (skip the heavy parts so this stays fast):

```bash
echo "" | OLR_PREPUSH_FULL=1 SKIP_FULL_TESTS=1 SKIP_IOS_BUILD=1 \
    bash .githooks/pre-push test-remote test-url; echo "exit=$?"
```
Expected: the delivery matrix runs and passes, then `OLR_PREPUSH_FULL=1 ...`, the two `SKIP_*` notices, and `exit=0`.

- [ ] **Step 4: No commit needed (verification task).**

---

## Notes / accepted risks

- **Residual skip edge:** if an encoder is *listed* by `ffmpeg -encoders` but fails to actually encode at runtime, a script may still skip (rtmp via code 77 / srt via exit 0). Preflight covers every tooling/encoder-presence skip path the scripts have, so this edge is rare on a machine that regularly runs these gates; it is the accepted residual from the spec's preflight-only decision.
- **Port 23740** must remain disjoint from the documented bands; if a future SRT test takes it, move this cell.
- Building default targets (engine + app + harnesses) is intentional — it makes "the current platform builds" part of the gate, not just the harness the matrix needs.
