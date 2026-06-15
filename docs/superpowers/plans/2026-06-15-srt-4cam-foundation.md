# 4-Source SRT Routing Foundation (Phase 2a) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove 4 real SRT streams connect, record into a 4-view MKV, and route correctly — view *i* carries camera *i*'s content (distinct audio tone), not blue-fill silence.

**Architecture:** A new `run_srt_4cam.sh` stands up 4 distinct-tone SRT cameras (ffmpeg → `srt-live-transmit` listener, 4 port-pairs), records them with the existing `sync_harness` (built against the Phase-1 SRT ffmpeg), and asserts each recorded view's dominant audio band equals its camera's tone. No C++ changes. Registered as a local-only `srt` CTest (already CI-excluded).

**Tech Stack:** ffmpeg/`srt-live-transmit` (brew), the Phase-1 libsrt ffmpeg + `-DOLR_FFMPEG_SRT_PREFIX`, `sync_harness`, CMake/CTest, bash/awk.

**Spec:** `docs/superpowers/specs/2026-06-15-srt-4cam-foundation-design.md`

**This is test-infra work, not unit-TDD** — verification is "run the scenario and observe" (correct routing → PASS; the teeth-check shifted mapping → FAIL).

---

## Setup (one-time in this worktree, ~10 min — a prerequisite, commits nothing)

The Phase-1 SRT ffmpeg + an SRT-linked `sync_harness` must exist before the scenario can run. In `/tmp/olr-srt4`:
```bash
# Build the libsrt ffmpeg (idempotent; ~10 min if not already built). Output is
# gitignored macos_build/ffmpeg-srt/.
bash build-scripts/build_ffmpeg_macos_srt.sh
# Configure the SRT build dir and build sync_harness against it.
cmake -S . -B build/srt -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja \
  -DCMAKE_BUILD_TYPE=Debug -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos \
  -DOLR_FFMPEG_SRT_PREFIX="$PWD/macos_build/ffmpeg-srt"
$HOME/Qt/Tools/Ninja/ninja -C build/srt sync_harness record_harness
# Confirm sync_harness links the SRT avformat:
otool -L build/srt/tests/e2e/sync_harness | grep -i avformat   # expect @rpath/libavformat.dylib
```

---

## File Structure

| File | Change |
|---|---|
| `tests/e2e/run_srt_4cam.sh` | 4-camera SRT generation + 4-view record + per-view tone-routing assertion |
| `tests/e2e/CMakeLists.txt` | register `e2e_srt_4cam` under the `srt` label |
| `tests/e2e/SRT_README.md` | note the new scenario |

---

## Task 1: `run_srt_4cam.sh` — 4-camera SRT routing scenario

**Files:**
- Create: `tests/e2e/run_srt_4cam.sh`

- [ ] **Step 1: Write `tests/e2e/run_srt_4cam.sh`**

```bash
#!/usr/bin/env bash
# Local SRT e2e: prove 4 real SRT streams record into a 4-view MKV and ROUTE
# correctly — view i carries camera i's content. Each camera emits a distinct
# audio tone (camera i = (i+1)*1000 Hz); we detect each recorded view's dominant
# tone and assert it matches that camera. A view from a source that failed to
# connect is blue-fill SILENCE (no dominant tone) and FAILS. Requires sync_harness
# built with -DOLR_FFMPEG_SRT_PREFIX (an SRT-enabled avformat).
#
# Teeth-check: set OLR_SRT4_EXPECT_SHIFT=1 to rotate the EXPECTED mapping (view i
# expects camera i+1); a correctly-routed recording then FAILS, proving the
# routing assertion really discriminates.
#
# Usage: run_srt_4cam.sh <sync_harness_exe> [base_port]
set -uo pipefail

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23510}"
SECONDS_TO_RECORD=8
SHIFT="${OLR_SRT4_EXPECT_SHIFT:-0}"

command -v ffmpeg  >/dev/null || { echo "SKIP: ffmpeg not found";  exit 0; }
command -v ffprobe >/dev/null || { echo "SKIP: ffprobe not found"; exit 0; }
command -v srt-live-transmit >/dev/null || { echo "SKIP: srt-live-transmit not found (brew install srt)"; exit 0; }

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "[srt-4cam] base_port=$BASE shift=$SHIFT"

# --- 1. Four distinct-tone SRT cameras: camera i = sine (i+1)*1000 Hz over srt. ---
URLS=()
for i in 0 1 2 3; do
    srt_port=$((BASE + 2*i)); udp_port=$((BASE + 2*i + 1)); freq=$(((i+1)*1000))
    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "testsrc2=size=640x480:rate=30" \
        -f lavfi -i "sine=frequency=${freq}:sample_rate=48000" -ac 2 \
        -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M \
        -c:a aac -b:a 128k \
        -f mpegts "udp://127.0.0.1:${udp_port}?pkt_size=1316" &
    PIDS+=($!)
    srt-live-transmit "udp://127.0.0.1:${udp_port}?mode=listener" \
        "srt://127.0.0.1:${srt_port}?mode=listener&transtype=live&latency=200" >/dev/null 2>&1 &
    PIDS+=($!)
    URLS+=("srt://127.0.0.1:${srt_port}?transtype=live")
done
sleep 1.5  # let 4 producers + 4 SRT listeners come up before the engine connects

# --- 2. Record all four as a 4-view MKV (source i -> view i). ---
OUT="$("$HARNESS" --url "${URLS[0]}" --url "${URLS[1]}" --url "${URLS[2]}" --url "${URLS[3]}" \
       --outdir "$WORKDIR" --name srt4cam --seconds "$SECONDS_TO_RECORD" --width 640 --height 480 --fps 30)"
RC=$?
OUT_MKV="$(printf '%s\n' "$OUT" | tail -n 1)"
echo "[srt-4cam] harness rc=$RC out=$OUT_MKV"
if [ $RC -ne 0 ] || [ -z "$OUT_MKV" ] || [ ! -s "$OUT_MKV" ]; then
    echo "FAIL: no output (rc=$RC) — engine could not record the SRT sources (sync_harness built with -DOLR_FFMPEG_SRT_PREFIX?)"
    exit 1
fi

# --- 3. Assert 4 video tracks. ---
VTRACKS="$(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 "$OUT_MKV" | wc -l | tr -d ' ')"
if [ "${VTRACKS:-0}" != "4" ]; then
    echo "FAIL: expected 4 video tracks, got ${VTRACKS:-0}"; exit 1
fi

# Dominant audio band (Hz) of one recorded view, or "none" if below the -60 dB floor.
detect_band() {  # $1=mkv $2=audio-stream-index
    local mkv="$1" idx="$2" best_f="none" best_rms="-1000" f rms
    for f in 1000 2000 3000 4000; do
        rms="$(ffmpeg -hide_banner -nostats -i "$mkv" -map 0:a:$idx \
               -af "bandpass=f=$f:width_type=h:w=200,astats=metadata=1:measure_overall=RMS_level" \
               -f null - 2>&1 | awk -F': ' '/Overall/{o=1} o && /RMS level dB/{print $2; exit}')"
        [ -z "$rms" ] || [ "$rms" = "-inf" ] && rms="-1000"
        if awk -v a="$rms" -v b="$best_rms" 'BEGIN{exit !(a+0 > b+0)}'; then best_rms="$rms"; best_f="$f"; fi
    done
    awk -v r="$best_rms" 'BEGIN{exit !(r+0 > -60)}' && echo "$best_f" || echo "none"
}

# --- 4. Per-view routing assertion: view i must carry camera (i+SHIFT)'s tone. ---
fail=0; line="[srt-4cam]"
for i in 0 1 2 3; do
    expected=$(( ((( i + SHIFT ) % 4) + 1) * 1000 ))
    detected="$(detect_band "$OUT_MKV" "$i")"
    line="$line view$i=${detected}Hz(exp${expected})"
    if [ "$detected" != "$expected" ]; then
        echo "FAIL: view $i carries ${detected}Hz, expected camera tone ${expected}Hz — wrong routing or source not connected"
        fail=1
    fi
done
echo "$line"
[ $fail -ne 0 ] && exit 1
echo "PASS: 4-source SRT routing — each view carries its own camera's tone"
exit 0
```

- [ ] **Step 2: Make executable + syntax check + run it (expect PASS).**
```bash
cd /tmp/olr-srt4
chmod +x tests/e2e/run_srt_4cam.sh
bash -n tests/e2e/run_srt_4cam.sh
bash tests/e2e/run_srt_4cam.sh build/srt/tests/e2e/sync_harness 23510
```
Expected: a line like `[srt-4cam] view0=1000Hz(exp1000) view1=2000Hz(exp2000) view2=3000Hz(exp3000) view3=4000Hz(exp4000)` then `PASS: 4-source SRT routing …`. (If a view detects the wrong/none tone, the engine mis-routed or a source didn't connect — investigate; the bandpass `w=200` + `-60 dB` floor should cleanly separate the four 1-kHz-spaced tones.)

- [ ] **Step 3: TEETH-CHECK — prove the routing assertion discriminates.** Re-run with the expected mapping rotated by one; a correctly-routed recording MUST now FAIL:
```bash
cd /tmp/olr-srt4
echo "--- teeth-check (shifted expectation, must FAIL) ---"
OLR_SRT4_EXPECT_SHIFT=1 bash tests/e2e/run_srt_4cam.sh build/srt/tests/e2e/sync_harness 23520 ; echo "exit=$?"
```
Expected: `FAIL: view 0 carries 1000Hz, expected camera tone 2000Hz …` and `exit=1`. (Uses base 23520 to avoid the prior run's lingering sockets.) Report BOTH the normal PASS line and the shifted FAIL — together they prove routing is correct AND the check has teeth. If the shifted run still PASSES, STOP and report DONE_WITH_CONCERNS (the detector isn't discriminating).

- [ ] **Step 4: Commit:**
```bash
git add tests/e2e/run_srt_4cam.sh
git commit -m "test(srt): 4-source SRT routing scenario — per-view tone identity"
```

---

## Task 2: register `e2e_srt_4cam` CTest (local-only `srt` label)

**Files:**
- Modify: `tests/e2e/CMakeLists.txt`

- [ ] **Step 1: Register the test.** In `tests/e2e/CMakeLists.txt`, in the SRT section (next to the existing `e2e_srt_smoke` registration), add:
```cmake
add_test(NAME e2e_srt_4cam
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_4cam.sh" "$<TARGET_FILE:sync_harness>" 23510)
set_tests_properties(e2e_srt_4cam PROPERTIES LABELS "srt" TIMEOUT 180 RUN_SERIAL TRUE)
```

- [ ] **Step 2: Reconfigure + run via CTest.**
```bash
cd /tmp/olr-srt4
cmake -S . -B build/srt -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja \
  -DCMAKE_BUILD_TYPE=Debug -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos \
  -DOLR_FFMPEG_SRT_PREFIX="$PWD/macos_build/ffmpeg-srt"
$HOME/Qt/Tools/Ninja/ninja -C build/srt sync_harness
( cd build/srt && ctest -N -L srt )                   # lists e2e_srt_smoke AND e2e_srt_4cam
( cd build/srt && ctest -R '^e2e_srt_4cam$' --output-on-failure )  # passes
```
Expected: both `srt` tests are listed; `e2e_srt_4cam` passes.

- [ ] **Step 3: Confirm it stays out of CI.** In the default brew build:
```bash
cd /tmp/olr-srt4
cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
( cd build/claude-debug && ctest -N -LE 'sync-report|srt' | grep -c e2e_srt_4cam )  # expect 0
```
Expected: `0` (the CI selection excludes it via the `srt` label).

- [ ] **Step 4: Commit:**
```bash
git add tests/e2e/CMakeLists.txt
git commit -m "test(srt): register e2e_srt_4cam under the local-only srt label"
```

---

## Task 3: docs note + verify default build + PR

**Files:**
- Modify: `tests/e2e/SRT_README.md`

- [ ] **Step 1: Add a note to `tests/e2e/SRT_README.md`.** After the existing `e2e_srt_smoke` description (before the "Next (Phase 2)" section), insert:
```markdown
`e2e_srt_4cam` goes further: it stands up **4** SRT cameras, each emitting a
distinct audio tone (1/2/3/4 kHz), records them into a 4-view MKV, and asserts
each recorded view carries its own camera's tone — proving 4 real SRT streams
connect and **route correctly** (view *i* = camera *i*), not blue-fill silence.
Run it with the same SRT build via `ctest -L srt`.
```
And update the "Next (Phase 2)" line to note 2a is done: change it to mention "Phase 2a (4-source routing) is implemented here; 2b (sync/trim/connection) and 2c (disconnect/loss) follow."

- [ ] **Step 2: Confirm the default brew build is unaffected.**
```bash
cd /tmp/olr-srt4/build/claude-debug
$HOME/Qt/Tools/Ninja/ninja
ctest --output-on-failure --repeat until-pass:2 -LE 'sync-report|srt' 2>&1 | tail -5
```
Expected: the existing unit + e2e suite passes (18/18), no `e2e_srt_*` among them.

- [ ] **Step 3: clang-format check (no C++ changed — confirm clean).**
```bash
cd /tmp/olr-srt4
GCF=/opt/homebrew/opt/llvm/bin/git-clang-format CF=/opt/homebrew/opt/llvm/bin/clang-format
"$GCF" --binary "$CF" --commit "$(git merge-base origin/main HEAD)" --diff --extensions cpp,h,hpp,mm,c 2>&1 | tail -1
```
Expected: `no modified files to format` / `did not modify any files`.

- [ ] **Step 4: Commit docs, push, open PR (do NOT merge).**
```bash
cd /tmp/olr-srt4
git add tests/e2e/SRT_README.md
git commit -m "docs(srt): note the e2e_srt_4cam 4-source routing scenario"
SKIP_IOS_BUILD=1 git push -u origin feat/srt-4cam
gh pr create --base main --title "4-source SRT routing e2e (Phase 2a)" --body "<summary: 4 distinct-tone SRT cameras -> 4-view record via sync_harness -> per-view dominant-band routing assertion (view i = camera i, blue-fill silence fails); local-only srt gate (CI-excluded); teeth-check via OLR_SRT4_EXPECT_SHIFT; no C++ changes; links spec/plan; note 2b/2c follow>"
```

- [ ] **Step 5: Watch CI green** (`gh pr checks <n>`). Branch changes only test scaffolding (no C++; `srt`-labelled, CI-excluded), so Build+Test/Lint/sanitizers pass on the brew path. Leave unmerged (per the active no-merge hold).

---

## Self-Review

**Spec coverage:**
- §3.1 `run_srt_4cam.sh` (4 distinct-tone cameras, sync_harness 4-view record, per-view dominant-band routing assert, SKIP/FAIL, per-view report) → Task 1. ✓
- §3.2 CTest `e2e_srt_4cam` under `srt` label → Task 2. ✓
- §3.3 README note → Task 3. ✓
- §4 frequency detection (bandpass `w=200` + max-RMS over {1k,2k,3k,4k} + -60 dB floor) → Task 1 `detect_band`. ✓
- §5 error handling (missing tools SKIP; SRT-less build → all views fail; ports 23510-23517) → Task 1 SKIP guards + the routing assert; base 23510 (teeth-check uses 23520). ✓
- §6 teeth-check (`OLR_SRT4_EXPECT_SHIFT` rotates expectation → correct routing FAILS) + default build unaffected → Task 1 Step 3, Task 3 Step 2. ✓
- §7 out-of-scope (2b/2c, no C++) → respected (no engine change). ✓

**Placeholder scan:** the PR body (Task 3) is an intentional fill-at-time summary; all script/CMake steps are complete. No TBD/TODO.

**Type/name consistency:** `run_srt_4cam.sh`, `e2e_srt_4cam`, `srt` label, `OLR_SRT4_EXPECT_SHIFT`, `detect_band`, base port 23510 (teeth-check 23520), tone `(i+1)*1000`, `OLR_FFMPEG_SRT_PREFIX` — consistent across tasks and with the Phase-1 conventions.

**Critical-path notes:** the Setup build (~10 min) is a prerequisite, not a task (commits nothing). The frequency detector + the teeth-check are the load-bearing correctness pieces — Task 1 Steps 2-3 verify both the normal PASS and the shifted FAIL.
