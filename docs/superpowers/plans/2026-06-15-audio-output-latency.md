# Audio Output-Latency Offset Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the existing `kOutputLatencyOffsetMs` constant a runtime, persisted, user-facing setting (0–500 ms) that compensates audio-output-device latency, with the playback resync threshold scaled by the offset so a high offset doesn't storm re-aligns.

**Architecture:** An `audioOutputLatencyMs` setting flows `AppSettings` (persisted) → `UIManager` (clamp/persist/route/seed) → `AudioPlayer::setOutputLatencyOffsetMs` (atomic). `AudioPlayer` uses it in the latency math AND scales its resync threshold (`kResyncHeadroomMs + offset`). A new `resyncCount` counter + a `play_harness` high-offset run prove no re-align storm.

**Tech Stack:** Qt6 (Core/Multimedia/QML), C++ std::atomic, CMake/CTest, the playback e2e harness.

**Spec:** `docs/superpowers/specs/2026-06-15-audio-output-latency-design.md`

---

## File Structure

| File | Change |
|---|---|
| `settingsmanager.h` | `int audioOutputLatencyMs = 0;` in `AppSettings` |
| `settingsmanager.cpp` | (de)serialize `audioOutputLatencyMs` |
| `tests/unit/tst_settingsmanager.cpp` | round-trip assertion |
| `playback/audioplayer.h` | drop the const; add `m_outputLatencyOffsetMs` + setter + `kMaxOutputLatencyMs`; rename `kResyncThresholdMs`→`kResyncHeadroomMs`; add `m_resyncCount` + `resyncCount()` |
| `playback/audioplayer.cpp` | use the offset in the latency math + scaled resync; count re-aligns |
| `uimanager.h/.cpp` | `audioOutputLatencyMs` Q_PROPERTY (READ/WRITE/NOTIFY); persist+route+seed |
| `Main.qml` | a ms `SpinBox` (0–500) in the settings area |
| `tests/e2e/play_harness.cpp` | read `OLR_AUDIO_LATENCY_MS`; print `resyncCount` |
| `tests/e2e/run_playback_e2e.sh` | `latency` scenario asserting `resyncCount==0` |
| `tests/e2e/CMakeLists.txt` | register `e2e_play_latency` |

**Build/test (this machine):**
```
cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja \
  -DCMAKE_BUILD_TYPE=Debug -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug
```
(`ninja` not on PATH — use `$HOME/Qt/Tools/Ninja/ninja`. ffmpeg/ffprobe at /opt/homebrew.)

---

## Task 1: `AppSettings.audioOutputLatencyMs` + persistence (TDD)

**Files:** `settingsmanager.h`, `settingsmanager.cpp`, `tests/unit/tst_settingsmanager.cpp`

- [ ] **Step 1: Add the failing assertion.** In `tests/unit/tst_settingsmanager.cpp`, in the sample-settings setup (the block setting `s.fps`, `s.videoWidth`, etc., near the top of the round-trip test), add:
```cpp
    s.audioOutputLatencyMs = 180;
```
and after an existing scalar `QCOMPARE` (e.g. `QCOMPARE(out.fps, in.fps);`) add:
```cpp
    QCOMPARE(out.audioOutputLatencyMs, in.audioOutputLatencyMs);
```

- [ ] **Step 2: Build, confirm it FAILS to compile:**
Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_settingsmanager`
Expected: error — `AppSettings` has no member `audioOutputLatencyMs`.

- [ ] **Step 3: Add the field.** In `settingsmanager.h`, in `struct AppSettings` (near `showTimeOfDay`/`videoWidth`), add:
```cpp
    int audioOutputLatencyMs = 0;  // playback output-device latency comp, ms (0..500)
```

- [ ] **Step 4: Serialize.** In `settingsmanager.cpp` `save()`, near `root["showTimeOfDay"] = settings.showTimeOfDay;`, add:
```cpp
    root["audioOutputLatencyMs"] = settings.audioOutputLatencyMs;
```
In `load()`, near `settings.showTimeOfDay = root["showTimeOfDay"].toBool(...)`, add:
```cpp
    settings.audioOutputLatencyMs = root["audioOutputLatencyMs"].toInt(settings.audioOutputLatencyMs);
```

- [ ] **Step 5: Build + run — confirm PASS:**
Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_settingsmanager && build/claude-debug/tests/unit/tst_settingsmanager`
Expected: all pass incl. the new round-trip.

- [ ] **Step 6: Commit:**
```bash
git add settingsmanager.h settingsmanager.cpp tests/unit/tst_settingsmanager.cpp
git commit -m "feat(audio-latency): AppSettings.audioOutputLatencyMs + persistence"
```

---

## Task 2: `AudioPlayer` — runtime offset, scaled resync, resync counter

**Files:** `playback/audioplayer.h`, `playback/audioplayer.cpp`

- [ ] **Step 1: Header changes.** In `playback/audioplayer.h`:

Replace the constant `static constexpr int kOutputLatencyOffsetMs = 0;` (and keep its surrounding doc comment, edited) with a max-constant + atomic member + setter. Add near the other constants:
```cpp
    static constexpr int kMaxOutputLatencyMs = 500;  // user offset ceiling
```
Rename the resync constant `static constexpr int kResyncThresholdMs = 250;` to:
```cpp
    static constexpr int kResyncHeadroomMs = 250;  // genuine-desync margin ABOVE the
                                                   // expected steady-state offset divergence;
                                                   // resync trigger = kResyncHeadroomMs + offset
```
(Update the long COUPLING comment block to say the threshold now scales with the offset, so the storm it warned about cannot happen.)

Add the members + setter (near `m_clearCount`):
```cpp
    // Output-device latency compensation (ms, 0..kMaxOutputLatencyMs). Folded into
    // the playout latency AND the resync threshold (which scales with it). Written
    // by the UI thread, read on the push path — relaxed: standalone scalar.
    std::atomic<int> m_outputLatencyOffsetMs{0};
    int m_resyncCount = 0;  // times the aligned branch re-aligned (push thread)
```
In the public section (near `setMuted`/`clear`):
```cpp
    void setOutputLatencyOffsetMs(int ms) {
        m_outputLatencyOffsetMs.store(qBound(0, ms, kMaxOutputLatencyMs), std::memory_order_relaxed);
    }
    int resyncCount() const { return m_resyncCount; }
```
(Ensure `<atomic>` and `<QtGlobal>` are available — `<atomic>` is already used by AudioPlayer; add `#include <QtGlobal>` if `qBound` is unresolved.)

- [ ] **Step 2: Latency math + scaled resync + counter (`audioplayer.cpp`).**

In `pushSamples`, change the latency math:
```cpp
    const int64_t latencySamples =
        (m_sinkLatencyBytes + int64_t(kOutputLatencyOffsetMs) * bytesPerSecond / 1000) /
        bytesPerFrame;
```
to read the runtime offset once:
```cpp
    const int outLatencyMs = m_outputLatencyOffsetMs.load(std::memory_order_relaxed);
    const int64_t latencySamples =
        (m_sinkLatencyBytes + int64_t(outLatencyMs) * bytesPerSecond / 1000) /
        bytesPerFrame;
```
Then change the resync block:
```cpp
    if (m_aligned) {
        const int64_t resyncSamples = int64_t(kResyncThresholdMs) * m_sampleRate / 1000;
        if (qAbs(ptsSamples - dueSamples) > resyncSamples) {
            m_aligned = false;
        }
    }
```
to scale the threshold by the offset and count the re-align:
```cpp
    if (m_aligned) {
        const int64_t resyncSamples =
            int64_t(kResyncHeadroomMs + outLatencyMs) * m_sampleRate / 1000;
        if (qAbs(ptsSamples - dueSamples) > resyncSamples) {
            m_aligned = false;
            ++m_resyncCount;
        }
    }
```

- [ ] **Step 3: Build clean:**
Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug`
Expected: clean build. (Any other use of `kResyncThresholdMs`/`kOutputLatencyOffsetMs` must now be updated — grep to confirm none remain: `grep -rn "kResyncThresholdMs\|kOutputLatencyOffsetMs" playback/` returns nothing.)

- [ ] **Step 4: Commit:**
```bash
git add playback/audioplayer.h playback/audioplayer.cpp
git commit -m "feat(audio-latency): runtime output-latency offset + offset-scaled resync + resyncCount"
```

---

## Task 3: `UIManager` — expose, persist, route, seed

**Files:** `uimanager.h`, `uimanager.cpp`

- [ ] **Step 1: Header.** In `uimanager.h`:
- Near `Q_PROPERTY(int recordWidth ...)` add:
```cpp
    Q_PROPERTY(int audioOutputLatencyMs READ audioOutputLatencyMs WRITE setAudioOutputLatencyMs NOTIFY audioOutputLatencyChanged)
```
- Near `int recordWidth() const;` add:
```cpp
    int audioOutputLatencyMs() const;
    void setAudioOutputLatencyMs(int ms);
```
- Near `void recordWidthChanged();` add the signal:
```cpp
    void audioOutputLatencyChanged();
```

- [ ] **Step 2: Getter/setter (`uimanager.cpp`).** After `UIManager::setRecordWidth` (or alongside the record getters/setters), add:
```cpp
int UIManager::audioOutputLatencyMs() const { return m_currentSettings.audioOutputLatencyMs; }

void UIManager::setAudioOutputLatencyMs(int ms) {
    const int clamped = qBound(0, ms, 500);  // keep in sync with AudioPlayer::kMaxOutputLatencyMs
    if (m_currentSettings.audioOutputLatencyMs == clamped) return;
    m_currentSettings.audioOutputLatencyMs = clamped;
    if (m_audioPlayer) m_audioPlayer->setOutputLatencyOffsetMs(clamped);
    emit audioOutputLatencyChanged();
    m_settingsManager->save(m_configPath, m_currentSettings);
}
```

- [ ] **Step 3: Seed at settings load.** In `UIManager::loadSettings`, at the tail success block where it `emit recordWidthChanged();` (the "Sync QML" emits), add the seed + emit so the loaded value reaches the already-constructed AudioPlayer and the QML SpinBox:
```cpp
    if (m_audioPlayer) m_audioPlayer->setOutputLatencyOffsetMs(m_currentSettings.audioOutputLatencyMs);
    emit audioOutputLatencyChanged();
```

- [ ] **Step 4: Build clean** (MOC picks up the new property/signal):
Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug`
Expected: clean build.

- [ ] **Step 5: Commit:**
```bash
git add uimanager.h uimanager.cpp
git commit -m "feat(audio-latency): UIManager exposes/persists/routes/seeds the offset"
```

---

## Task 4: `Main.qml` — settings SpinBox

**Files:** `Main.qml`

- [ ] **Step 1: Insert the control.** In `Main.qml`, in the record-settings area (near the `SpinBox`es bound to `uiManagerRef.recordWidth`/`recordHeight`/`recordFps`), add a labelled row. Place it after the fps row (match the surrounding `RowLayout`/`Label` + `SpinBox` structure used by the record settings):
```qml
                    RowLayout {
                        Label { text: "Audio output latency (ms)" }
                        SpinBox {
                            id: audioLatencySpin
                            from: 0
                            to: 500
                            stepSize: 10
                            editable: true
                            inputMethodHints: Qt.ImhDigitsOnly
                            value: appWindow.uiManagerRef.audioOutputLatencyMs
                            onValueModified: appWindow.uiManagerRef.audioOutputLatencyMs = value
                            ToolTip.visible: hovered
                            ToolTip.text: "Compensate audio-output device delay (HDMI/Bluetooth). Raise until lip-sync is correct on this output."
                        }
                    }
```
(Match the exact indentation and the wrapping idiom of the sibling record SpinBoxes — some are bare `SpinBox` in a grid, some in `RowLayout`s; mirror whichever encloses `recordFps`.)

- [ ] **Step 2: Build (qmlcachegen compiles the QML):**
Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug OpenLiveReplay`
Expected: clean; `Main_qml.cpp` regenerates.

- [ ] **Step 3: qmllint (CI runs it; only errors fail):**
Run: `$HOME/Qt/6.10.1/macos/bin/qmllint -I $HOME/Qt/6.10.1/macos/qml Main.qml 2>&1 | grep -iE "error" || echo "no errors"`
Expected: `no errors`.

- [ ] **Step 4: Commit:**
```bash
git add Main.qml
git commit -m "feat(audio-latency): audio output-latency SpinBox in settings"
```

---

## Task 5: e2e proof — `resyncCount` + `latency` scenario

**Files:** `tests/e2e/play_harness.cpp`, `tests/e2e/run_playback_e2e.sh`, `tests/e2e/CMakeLists.txt`

- [ ] **Step 1: `play_harness` reads the env offset + prints resyncCount.** In `tests/e2e/play_harness.cpp`, after `audio.start(48000, 2);` (and before `worker.start();`), add:
```cpp
    // Optional output-latency offset for the resync-coupling e2e (default 0).
    audio.setOutputLatencyOffsetMs(qEnvironmentVariableIntValue("OLR_AUDIO_LATENCY_MS"));
```
Then in BOTH places that print the `COUNTERS ...` line (the `finish` lambda and the early no-frames path), append `resyncCount=%d` and the value. Change:
```cpp
        printf("COUNTERS reposition=%d reuseSeek=%d reverseChunkSeek=%d "
               "eofTailSeek=%d skipForward=%d audioPushes=%d framesDropped=%d\n",
               c.reposition, c.reuseSeek, c.reverseChunkSeek, c.eofTailSeek, c.skipForward,
               c.audioPushes, c.framesDropped);
```
to:
```cpp
        printf("COUNTERS reposition=%d reuseSeek=%d reverseChunkSeek=%d "
               "eofTailSeek=%d skipForward=%d audioPushes=%d framesDropped=%d resyncCount=%d\n",
               c.reposition, c.reuseSeek, c.reverseChunkSeek, c.eofTailSeek, c.skipForward,
               c.audioPushes, c.framesDropped, audio.resyncCount());
```
(Apply to both printf sites; in the early no-frames path the `audio` object is in scope.)

- [ ] **Step 2: Build the harness:**
Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug play_harness`
Expected: clean build.

- [ ] **Step 3: Driver — `latency` scenario.** In `tests/e2e/run_playback_e2e.sh`:

(a) Add a `resyncCount` parse near the other `get` lines:
```bash
resyncCount="$(get resyncCount)"
```
(b) Map the `latency` driver-scenario to `play1x` playback with the env offset, just before the `play_harness` invocation (`PLAY_OUT="$(...)"`). Add:
```bash
PH_SCENARIO="$SCENARIO"
if [ "$SCENARIO" = "latency" ]; then
    export OLR_AUDIO_LATENCY_MS="${OLR_AUDIO_LATENCY_MS:-300}"
    PH_SCENARIO="play1x"
fi
```
and change the invocation to use `$PH_SCENARIO`:
```bash
PLAY_OUT="$("$PLAY" "$FIXTURE" "$PH_SCENARIO" "$VIEWS")"
```
(c) Add the assertion case in the scenario `case` block (alongside `play1x)` etc.):
```bash
    latency)
        # 1x playback with a 300 ms output-latency offset must NOT storm re-aligns:
        # the resync threshold scales with the offset (kResyncHeadroomMs + offset),
        # so the steady offset divergence is tolerated. resyncCount must stay 0.
        if ! num "$audioPushes" || [ "$audioPushes" -le 0 ]; then
            echo "FAIL: latency produced no audio (audioPushes=$audioPushes) — audio path dead"
            fail=1
        fi
        if ! num "$resyncCount" || [ "$resyncCount" -ne 0 ]; then
            echo "FAIL: latency re-align storm (resyncCount=$resyncCount, expected 0) — resync threshold not scaled with offset"
            fail=1
        fi
        ;;
```
(d) Ensure `resyncCount` is included in the final `SUMMARY="..."` echo line (append `resyncCount=$resyncCount`).

- [ ] **Step 4: Run it and confirm no storm:**
```bash
cd /tmp/olr-audiolatency
bash -n tests/e2e/run_playback_e2e.sh
bash tests/e2e/run_playback_e2e.sh build/claude-debug/tests/e2e/play_harness build/claude-debug/tests/e2e/record_harness latency 2 23491
```
Expected: `PASS` with `resyncCount=0 audioPushes>0`. **Sanity that the gate has teeth:** temporarily edit `audioplayer.cpp`'s resync to use `kResyncHeadroomMs` WITHOUT `+ outLatencyMs`, rebuild `play_harness`, re-run — it should now FAIL with `resyncCount>0` (proving the scaling is what prevents the storm). Then revert that temporary edit and rebuild. Report both numbers.

- [ ] **Step 5: Register the CTest.** In `tests/e2e/CMakeLists.txt`, add alongside the other `e2e_play_*` cases (unique port 23491):
```cmake
add_test(NAME e2e_play_latency
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" latency 2 23491)
```
and add `e2e_play_latency` to the `set_tests_properties(... PROPERTIES LABELS "e2e" TIMEOUT 180 RUN_SERIAL TRUE)` list for the playback cases.

- [ ] **Step 6: Reconfigure + run via CTest:**
```bash
cd /tmp/olr-audiolatency
cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug play_harness record_harness
( cd build/claude-debug && ctest -R '^e2e_play_latency$' --output-on-failure )
```
Expected: `e2e_play_latency` passes.

- [ ] **Step 7: Commit:**
```bash
git add tests/e2e/play_harness.cpp tests/e2e/run_playback_e2e.sh tests/e2e/CMakeLists.txt
git commit -m "test(audio-latency): resyncCount + latency e2e proves no re-align storm at 300ms offset"
```

---

## Task 6: Full verification + clang-format + PR

**Files:** none (process)

- [ ] **Step 1: Full build + gating suite (no regression):**
```bash
cd /tmp/olr-audiolatency/build/claude-debug
ctest -L unit --output-on-failure        # incl. tst_settingsmanager
ctest -L e2e --output-on-failure         # 10/10 now (adds e2e_play_latency); e2e_play_reverse de-flaked in #29
```
Expected: unit green; e2e all pass.

- [ ] **Step 2: clang-format the changed C++ lines:**
```bash
cd /tmp/olr-audiolatency
GCF=/opt/homebrew/opt/llvm/bin/git-clang-format CF=/opt/homebrew/opt/llvm/bin/clang-format
base=$(git merge-base origin/main HEAD)
"$GCF" --binary "$CF" --commit "$base" --extensions cpp,h,hpp,mm,c --force
"$GCF" --binary "$CF" --commit "$base" --diff --extensions cpp,h,hpp,mm,c   # expect "did not modify"
```
If it reformatted, `git commit -am "clang-format: audio output latency"`.

- [ ] **Step 3: Push + open PR (do NOT merge):**
```bash
SKIP_IOS_BUILD=1 git push -u origin feat/audio-output-latency
gh pr create --base main --title "Audio output-latency offset (LIPSYNC-3)" --body "<summary: runtime 0-500ms output-latency setting; resync threshold scales with offset so high offsets don't storm re-aligns; SpinBox in settings; proven by e2e resyncCount==0 at 300ms — links spec/plan>"
```

- [ ] **Step 4: Watch CI green** (`gh pr checks <n>`): Build+Test, Lint, both sanitizers. Leave unmerged (per the active no-merge hold).

---

## Self-Review

**Spec coverage:**
- §2 ms/0–500/global/live → Task 1 (field), Task 2 (clamp ±/0–500 in setter), Task 3 (live route). ✓
- §3 resync scaling (`kResyncHeadroomMs + offset`) → Task 2 Step 2. ✓
- §4 components (settings/audioplayer/uimanager/qml) → Tasks 1–4. ✓
- §5 clamp both layers; load clamped → Task 2 setter clamp, Task 3 UI clamp; `.toInt(default)` keeps old configs. ✓
- §6 unit round-trip + e2e resyncCount gate + new counter → Task 1, Task 5. ✓
- §7 YAGNI (no presets/negative) — not implemented, correct. ✓

**Placeholder scan:** PR body (Task 6) is an intentional fill-at-time summary. All code steps are complete with exact edits. The Main.qml insertion (Task 4) says "mirror whichever encloses recordFps" — that's a concrete instruction to match an existing sibling, not a placeholder; the SpinBox body is fully specified.

**Type/name consistency:** `audioOutputLatencyMs` (settings field + UIManager property/getter/setter/signal), `m_outputLatencyOffsetMs`/`setOutputLatencyOffsetMs`/`kMaxOutputLatencyMs` (AudioPlayer), `kResyncHeadroomMs` (renamed from `kResyncThresholdMs`, used in Task 2), `m_resyncCount`/`resyncCount()` (AudioPlayer + play_harness + driver), `OLR_AUDIO_LATENCY_MS`/`latency`/`e2e_play_latency` (e2e). Clamp bound `500` literal in UIManager carries a "keep in sync with kMaxOutputLatencyMs" comment. All consistent across tasks.

**Known fragile point:** Task 5 Step 4's teeth-check (temporarily breaking the scaling to confirm the gate fails) is the key validation that the e2e actually proves the fix — flagged as a required sanity step with explicit revert.
