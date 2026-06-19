# Delivery-matrix push gate — design

**Date:** 2026-06-19
**Status:** Approved (design); ready for implementation planning

## Problem

The pre-push hook ([.githooks/pre-push](../../../.githooks/pre-push)) is **advisory-only** by
default: it prints a recommendation and always `exit 0`, gating nothing. The only blocking
local gate is the opt-in `OLR_PREPUSH_FULL=1` (full CTest matrix + iOS cross-build), which is
heavy and seldom run. PR CI on GitHub runs only the short `ci` CTest label and explicitly
excludes the transport e2e tests (`native-rtmp`, `native-apple-ingest`, `native-ndi`,
`native-rtmp-soak`) because the from-source FFmpeg/SRT builds OOM hosted runners and the tests
need `ffmpeg` + `srt-live-transmit` present.

Result: nothing automatically verifies that a push can still **deliver** over the real
transports. We want a fast, always-on push gate that builds the current platform and runs a
small **transport × codec delivery matrix** with short clips.

## Goals

- An **always-on, blocking** pre-push gate (replaces the advisory-only default).
- Builds the **current-platform host** binary/harnesses (a real build gate).
- Runs a **4-cell delivery matrix** with **5-second clips**: RTMP{h264,h265} × SRT{h264,h265}.
- **Hard-fails** (blocks the push) when required tooling is missing — no silent skip-through.
- Keeps a fast escape hatch for emergencies.

## Non-goals

- **NDI** cell — deferred (NDI carries its own codec, not H.264/H.265; it is a single
  non-codec-split cell to revisit later).
- **Windows / other platforms** — out of scope this iteration. The selection mechanism (a CTest
  label) is platform-agnostic, so other platforms can opt their cells in later.
- **iOS** — stays only under `OLR_PREPUSH_FULL`.
- No changes to GitHub PR CI ([.github/workflows/ci.yml](../../../.github/workflows/ci.yml)).

## Decisions (from brainstorming)

| Question | Decision |
|---|---|
| Soak meaning | Not a unit-test loop — an **e2e delivery matrix** (RTMP/SRT × h264/h265), 5s clips, current-platform binary. |
| Matrix cells | RTMP{h264,h265} + SRT{h264,h265} = **4 cells**. SRT-h265 wired from the existing cross-platform script. NDI deferred. |
| Gate trigger | **New always-on default** (every `git push`). `OLR_PREPUSH_FULL` stays the heavier matrix+iOS gate. |
| Missing tooling | **Hard-fail, block push** with an actionable install message. |

## Design

### 1. Behavior / triggers

```
git push                  → build host (current platform) + run 4-cell delivery matrix → BLOCK on failure
OLR_PREPUSH_SKIP=1 push   → skip the gate (emergency escape hatch; git push --no-verify also bypasses)
OLR_PREPUSH_FULL=1 push   → existing heavy gate (full CTest matrix + iOS); also runs the delivery matrix
```

### 2. The matrix — selected by a new CTest label `delivery-gate`

A new `delivery-gate` CTest label is added to exactly these four macOS (`if(APPLE)`) cells in
[tests/e2e/CMakeLists.txt](../../../tests/e2e/CMakeLists.txt). A label (vs. a brittle `-R`
regex or a separate build dir) is the existing idiom in this repo and lets the matrix be run
on demand with `ctest -L delivery-gate`.

| Cell | CTest case | Today | Change |
|---|---|---|---|
| RTMP · h264 | `e2e_native_rtmp_smoke` | exists (`native-rtmp`) | add `delivery-gate` label |
| RTMP · h265 | `e2e_native_rtmp_hevc_smoke` | exists (`native-rtmp`) | add `delivery-gate` label |
| SRT · h264 | `e2e_native_srt_smoke` | exists (`native-apple-ingest`) | add `delivery-gate` label |
| SRT · h265 | `e2e_native_srt_hevc_smoke` | **script exists, wired Windows-only** | add `add_test` under `if(APPLE)` + label |

The SRT-HEVC script [tests/e2e/run_srt_hevc_smoke.sh](../../../tests/e2e/run_srt_hevc_smoke.sh)
already exists and is cross-platform (auto-selects `libx265` / `hevc_videotoolbox` / other
`hevc_*` encoders). It is only registered as a CTest case in the `elseif(WIN32)` branch today;
the macOS work is to register it under `if(APPLE)` with a unique port (clear of the existing
SRT band) and the `native-apple-ingest` + `delivery-gate` labels.

CTest cells keep `RUN_SERIAL TRUE` (each binds UDP/SRT/RTMP ports and drives ffmpeg) and the
gate runs them with `--repeat until-pass:2` to match the CI flake policy.

### 3. 5-second clips

Each script hardcodes its own duration: `run_rtmp_smoke.sh`=7, `run_rtmp_hevc_smoke.sh`=4,
`run_srt_smoke.sh`=6, `run_srt_hevc_smoke.sh`=6. Make each honor an env override while
preserving today's per-script default:

```bash
SECONDS_TO_RECORD="${OLR_E2E_CLIP_SECONDS:-7}"   # default = the script's current value
```

The hook exports `OLR_E2E_CLIP_SECONDS=5`, so the gate runs 5s clips while standalone `ctest`
runs are unchanged. The scripts already derive their frame-count thresholds
(`MIN_FRAMES=$((30 * SECONDS_TO_RECORD / 2))`) from `SECONDS_TO_RECORD`, so assertions scale
automatically.

### 4. Hard-fail preflight (the core of "no silent skip")

The skip codes are inconsistent across scripts — `run_srt_smoke.sh` skips with `exit 0`
(passes), `run_rtmp_smoke.sh` skips with `exit 77` (`SKIP_RETURN_CODE 77`). Either way a skip
would let the gate "pass" without delivering anything. So the hook **preflights** before
invoking ctest and aborts the push with an actionable message if anything is missing:

- `ffmpeg`, `ffprobe`, `srt-live-transmit` present on `PATH`
- at least one usable **H.264 encoder** and one **HEVC encoder** in `ffmpeg -encoders`
  (covers the scripts' encoder-availability skip paths)
- the **Qt host kit** at `$QT_HOST_PREFIX` (default `~/Qt/6.10.1/macos`)

Preflight passing guarantees the four cells actually execute rather than skip — that is what
makes "hard-fail" real. (No change to the scripts' own skip codes is required; preflight makes
the missing-tooling paths unreachable inside the gate.)

### 5. Build step

The gate builds the current-platform host test configuration incrementally in a dedicated dir
(ccache-warm after the first run):

```bash
cmake -S . -B build/prepush-delivery -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH="$QT_HOST_PREFIX" \
    -DOLR_BUILD_TESTS=ON
cmake --build build/prepush-delivery        # engine + app + harnesses (record_harness, ...)
```

A build failure blocks the push — this is the "current platform builds" gate. The matrix then
runs against this build dir:

```bash
OLR_E2E_CLIP_SECONDS=5 ctest --test-dir build/prepush-delivery \
    -L delivery-gate --output-on-failure --repeat until-pass:2
```

### 6. Interaction with `OLR_PREPUSH_FULL`

`OLR_PREPUSH_FULL=1` remains the heavier opt-in gate (full local CTest matrix + iOS
cross-build, with `SKIP_FULL_TESTS` / `SKIP_IOS_BUILD` sub-skips). It should also exercise the
delivery matrix (it's a strict superset). `OLR_PREPUSH_SKIP=1` skips everything.

## Components changed

| Component | Change |
|---|---|
| [.githooks/pre-push](../../../.githooks/pre-push) | Rewrite: default path = preflight → build → `ctest -L delivery-gate` (blocking). Add `OLR_PREPUSH_SKIP`. Keep `OLR_PREPUSH_FULL` (superset). |
| [tests/e2e/CMakeLists.txt](../../../tests/e2e/CMakeLists.txt) | Add `delivery-gate` label to the 3 existing cells; register `e2e_native_srt_hevc_smoke` under `if(APPLE)` with a unique port + `native-apple-ingest`/`delivery-gate` labels. |
| `tests/e2e/run_rtmp_smoke.sh`, `run_rtmp_hevc_smoke.sh`, `run_srt_smoke.sh`, `run_srt_hevc_smoke.sh` | `SECONDS_TO_RECORD` honors `OLR_E2E_CLIP_SECONDS` (default = current value). |
| [tests/README.md](../../../tests/README.md) | Document the `delivery-gate` label and the new default push gate / env vars. |

## Testing / verification

- **Manual:** `ctest -L delivery-gate` selects exactly the 4 cells and passes on a tooled
  macOS machine; with `OLR_E2E_CLIP_SECONDS=5` the recorded MKVs carry ~5s of real content.
- **Preflight:** temporarily shadow `srt-live-transmit` off PATH → hook aborts the push with
  the install message (does not run ctest).
- **Skip-through guard:** confirm no `delivery-gate` cell reports CTest "Skipped" when preflight
  passes (i.e. the gate runs 4/4).
- **Escape hatch:** `OLR_PREPUSH_SKIP=1 git push` and `git push --no-verify` both bypass.
- **Build gate:** introduce a compile error → hook blocks at the build step.

## Expected cost

~1–2 min steady-state: incremental build + 4 serial cells (~8–15s each incl. ffmpeg spin-up),
with `--repeat until-pass:2` doubling a flaky cell. First run is longer (cold build).
`OLR_PREPUSH_SKIP=1` is the release valve.

## Risks / open considerations

- **Always-on blocking changes the daily push loop.** Mitigated by `OLR_PREPUSH_SKIP=1` /
  `--no-verify`.
- **Port collisions** for the new SRT-HEVC macOS cell — must pick a port disjoint from the
  documented bands (record 23456–23459, playback 23464–23497, sync 23480–23489, av-sync 23492,
  native ≥23550, RTMP 23760–23800).
- **Encoder availability** varies by machine; preflight surfaces this as a clear failure rather
  than a silent skip.
