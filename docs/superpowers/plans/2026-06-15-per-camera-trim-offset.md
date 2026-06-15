# Per-Camera Trim Offset Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a per-source signed timeline trim (±500 ms, live, ms units) that shifts which captured content lands on each output tick, so an operator can hand-correct residual inter-camera skew.

**Architecture:** A `trimOffsetMs` flows settings → `StreamWorker` (applied to the video jitter gate, the capture-thread pre-drain gate, and the audio source-read, so video+audio shift together while the output file timeline stays fixed) → live-routed via `ReplayManager`/`UIManager` → a `SpinBox` in the source row. Proven by an `intercam_trim` scenario in the PR #26 sync harness.

**Tech Stack:** Qt6 (Core/QML), C++ std::atomic, FFmpeg recording engine, CMake/CTest, the e2e sync harness (bash + ffmpeg).

**Spec:** `docs/superpowers/specs/2026-06-15-per-camera-trim-offset-design.md`

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `settingsmanager.h` | `int trimOffsetMs = 0;` in `SourceSettings` | data model |
| `settingsmanager.cpp` | (de)serialize `trimOffsetMs` | persistence |
| `tests/unit/tst_settingsmanager.cpp` | round-trip assertion | test |
| `recorder_engine/streamworker.h` | `kMaxTrimMs`, `m_trimOffsetMs`, `setTrimOffsetMs` | apply state |
| `recorder_engine/streamworker.cpp` | subtract trim at pre-drain gate, video gate, audio source-read | apply trim |
| `recorder_engine/replaymanager.h/.cpp` | `setSourceTrims`, `updateSourceTrim`, `m_sourceTrims`, seed worker | route |
| `uimanager.h/.cpp` | `sourceTrimOffset`/`setSourceTrimOffset`/`sourceTrimVersion` | expose + persist + route |
| `Main.qml` | `SpinBox` in source row | operator control |
| `tests/e2e/sync_harness.cpp` | `--trim <ms>` flag | e2e proof harness |
| `tests/e2e/run_sync_e2e.sh` | `intercam_trim` scenario | e2e proof |
| `tests/e2e/CMakeLists.txt` | register `sync_intercam_trim` | e2e proof |
| `tests/e2e/SYNC_BASELINE.md` | trim-scenario baseline line | snapshot |

**Build/test (this machine):**
```
cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja \
  -DCMAKE_BUILD_TYPE=Debug -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug
```
(`ninja` is NOT on PATH; use `$HOME/Qt/Tools/Ninja/ninja`. ffmpeg/ffprobe at /opt/homebrew.)

---

## Task 1: `SourceSettings.trimOffsetMs` + persistence (TDD)

**Files:**
- Modify: `settingsmanager.h` (SourceSettings struct), `settingsmanager.cpp` (save+load), `tests/unit/tst_settingsmanager.cpp`

- [ ] **Step 1: Add the failing round-trip assertion.** In `tests/unit/tst_settingsmanager.cpp`, in the source setup (the block creating `SourceSettings a;`/`b;`, ~lines 31-40) add a trim to each:

```cpp
    a.trimOffsetMs = -66;   // advance
    b.trimOffsetMs = 132;   // delay
```

and after the existing `QCOMPARE(out.sources[0].id, in.sources[0].id);` (~line 82) add:

```cpp
    QCOMPARE(out.sources[0].trimOffsetMs, in.sources[0].trimOffsetMs);
    QCOMPARE(out.sources[1].trimOffsetMs, in.sources[1].trimOffsetMs);
```

- [ ] **Step 2: Build the test and confirm it FAILS to compile** (the field does not exist yet):

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_settingsmanager`
Expected: compile error — `'struct SourceSettings' has no member named 'trimOffsetMs'`.

- [ ] **Step 3: Add the field.** In `settingsmanager.h`, in `struct SourceSettings` (currently `id`/`name`/`url`/`metadata`), add:

```cpp
    int trimOffsetMs = 0;  // per-source timeline trim (+delay / -advance), ms
```

- [ ] **Step 4: Serialize it.** In `settingsmanager.cpp` `save()`, after `obj["metadata"] = source.metadata;` (~line 70) add:

```cpp
        obj["trimOffsetMs"] = source.trimOffsetMs;
```

In `load()`, after `source.metadata = obj["metadata"].toArray();` (~line 168) add:

```cpp
        source.trimOffsetMs = obj["trimOffsetMs"].toInt(0);
```

- [ ] **Step 5: Build and run the test — confirm PASS:**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_settingsmanager && build/claude-debug/tests/unit/tst_settingsmanager`
Expected: all pass, including the new trim round-trip.

- [ ] **Step 6: Commit:**
```bash
git add settingsmanager.h settingsmanager.cpp tests/unit/tst_settingsmanager.cpp
git commit -m "feat(trim): SourceSettings.trimOffsetMs + persistence"
```

---

## Task 2: `StreamWorker` applies the trim (gate + pre-drain + audio)

**Files:**
- Modify: `recorder_engine/streamworker.h`, `recorder_engine/streamworker.cpp`

- [ ] **Step 1: Add state + setter to the header.** In `recorder_engine/streamworker.h`, near the other `static constexpr` recording constants (e.g. just after `kJitterBufferMs`), add:

```cpp
    // Max magnitude of the per-source timeline trim (ms). +delay / -advance.
    static constexpr int kMaxTrimMs = 500;
```

Add a public setter next to `setViewTrack` (which is `public:`):

```cpp
    // Per-source timeline trim in ms (+delay / -advance), clamped to ±kMaxTrimMs.
    // Read by the tick/capture threads each pulse, so it takes effect live.
    void setTrimOffsetMs(int ms) {
        m_trimOffsetMs.store(qBound(-kMaxTrimMs, ms, kMaxTrimMs), std::memory_order_relaxed);
    }
```

And add the member near `std::atomic<bool> m_connected{false};`:

```cpp
    std::atomic<int> m_trimOffsetMs{0};  // signed ms; +delay / -advance
```

(`qBound` needs `<QtGlobal>`/`<QtMinMax>`, already transitively included via existing Qt headers in this file; if the build complains, add `#include <QtGlobal>`.)

- [ ] **Step 2: Apply trim to the capture-thread pre-drain gate.** In `recorder_engine/streamworker.cpp` `onMasterPulse`, change the `m_lastTickTargetMs.store(...)` (lines ~94-96) from:

```cpp
    m_lastTickTargetMs.store(
        qMax<int64_t>(0, (frameIndex * 1000) / m_targetFps - kJitterBufferMs),
        std::memory_order_relaxed);
```

to:

```cpp
    m_lastTickTargetMs.store(
        qMax<int64_t>(0, (frameIndex * 1000) / m_targetFps - kJitterBufferMs
                             - m_trimOffsetMs.load(std::memory_order_relaxed)),
        std::memory_order_relaxed);
```

(A positive/delay trim lowers the pre-drain gate so the capture thread keeps the older frames the delayed main gate will still need.)

- [ ] **Step 3: Apply trim to the video jitter gate.** In `processEncoderTick`, change the gate (line ~149) from:

```cpp
        int64_t targetTimeMs = currentRecordingTimeMs - kJitterBufferMs;
```

to:

```cpp
        const int64_t trimMs = m_trimOffsetMs.load(std::memory_order_relaxed);
        int64_t targetTimeMs = currentRecordingTimeMs - kJitterBufferMs - trimMs;
```

- [ ] **Step 4: Apply trim to the audio source-read.** In `writeAudioForTick` (line ~1006), just after `const int64_t jitterSamples = ...;` (line ~1012) add:

```cpp
    const int64_t trimSamples =
        int64_t(m_trimOffsetMs.load(std::memory_order_relaxed)) * kAudioSampleRate / 1000;
```

Then change the unmapped-drop expression (line ~1022) from:

```cpp
            const int64_t dropSamples = (targetEnd - jitterSamples) - m_audioFifoStartSample;
```

to:

```cpp
            const int64_t dropSamples =
                (targetEnd - jitterSamples - trimSamples) - m_audioFifoStartSample;
```

And change the source-timeline mapping (line ~1042) from:

```cpp
    const int64_t srcStart = start - jitterSamples;  // source timeline
```

to:

```cpp
    const int64_t srcStart = start - jitterSamples - trimSamples;  // source timeline (+trim)
```

(The main copy + its consumed-FIFO trim at line ~1058 derive from `srcStart`, so they retain the right history automatically. The write cursor `m_audioWriteCursor` / `start` stay on the file timeline, so output audio remains gapless.)

- [ ] **Step 5: Build clean (whole app + test libs):**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug`
Expected: clean build (only the pre-existing `muxer.cpp` unused-param warning). The behavioral proof comes in Task 6's e2e; this step just verifies it compiles and links.

- [ ] **Step 6: Commit:**
```bash
git add recorder_engine/streamworker.h recorder_engine/streamworker.cpp
git commit -m "feat(trim): apply per-source trim to video gate, pre-drain, and audio read"
```

---

## Task 3: `ReplayManager` seeds + live-updates the trim

**Files:**
- Modify: `recorder_engine/replaymanager.h`, `recorder_engine/replaymanager.cpp`

- [ ] **Step 1: Add API + member to the header.** In `recorder_engine/replaymanager.h`, next to `setSourceMetadata` (~line 27) add a setter:

```cpp
    void setSourceTrims(const QList<int> &trims) { m_sourceTrims = trims; }
```

next to `updateSourceUrl` (~line 40) declare:

```cpp
    void updateSourceTrim(int sourceIndex, int ms);
```

and next to `m_sourceMetadata` (~line 85) add the member:

```cpp
    QList<int> m_sourceTrims;  // per-source initial trim ms (parallel to m_sourceUrls)
```

- [ ] **Step 2: Seed each worker at creation.** In `recorder_engine/replaymanager.cpp` `startRecording`, in the worker-create loop right after the metadata seed block (`if (s < m_sourceMetadata.size()) { worker->setSourceMetadata(...); }`, ~line 144-146) add:

```cpp
        if (s < m_sourceTrims.size()) {
            worker->setTrimOffsetMs(m_sourceTrims[s]);
        }
```

- [ ] **Step 3: Implement the live update.** In `recorder_engine/replaymanager.cpp`, after `updateSourceUrl` (ends ~line 273), add:

```cpp
void ReplayManager::updateSourceTrim(int sourceIndex, int ms) {
    if (sourceIndex < 0) return;
    if (sourceIndex < m_sourceTrims.size()) m_sourceTrims[sourceIndex] = ms;
    if (m_isRecording && sourceIndex < m_workers.size()) {
        m_workers[sourceIndex]->setTrimOffsetMs(ms);
    }
}
```

- [ ] **Step 4: Build clean:**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug`
Expected: clean build.

- [ ] **Step 5: Commit:**
```bash
git add recorder_engine/replaymanager.h recorder_engine/replaymanager.cpp
git commit -m "feat(trim): ReplayManager seeds + live-updates per-source trim"
```

---

## Task 4: `UIManager` exposes, persists, and routes the trim

**Files:**
- Modify: `uimanager.h`, `uimanager.cpp`

- [ ] **Step 1: Header — property, signal, accessors, member.** In `uimanager.h`:

After the `sourceConnectionVersion` Q_PROPERTY (~line 62-64) add:

```cpp
    // Bumped when any source's trim changes (config load / programmatic set) so
    // QML re-reads sourceTrimOffset() bindings.
    Q_PROPERTY(int sourceTrimVersion READ sourceTrimVersion NOTIFY sourceTrimChanged)
```

After `int sourceConnectionVersion() const { ... }` (~line 105) add:

```cpp
    int sourceTrimVersion() const { return m_sourceTrimVersion; }
```

Near the other `Q_INVOKABLE` source methods (~line 168-172) add:

```cpp
    Q_INVOKABLE int sourceTrimOffset(int sourceIndex) const;
    Q_INVOKABLE void setSourceTrimOffset(int sourceIndex, int ms);
```

Near `void sourceConnectionChanged();` (~line 213) add the signal:

```cpp
    void sourceTrimChanged();
```

Near `int m_sourceConnectionVersion = 0;` (~line 299) add the member:

```cpp
    int m_sourceTrimVersion = 0;
```

- [ ] **Step 2: Push initial trims to the engine alongside metadata.** In `uimanager.cpp`, in the loop that builds `urls`/`names`/`metadata` from `m_currentSettings.sources` (the block ending with `m_replayManager->setSourceMetadata(metadata);` at ~line 594), declare a `QList<int> trims;` beside the other lists, append in the loop:

```cpp
        trims.append(source.trimOffsetMs);
```

and right after `m_replayManager->setSourceMetadata(metadata);` add:

```cpp
    m_replayManager->setSourceTrims(trims);
```

Apply the **same** addition at the other source-config push site that calls `setSourceMetadata` (~line 685): build a parallel `trims` list in that loop and call `m_replayManager->setSourceTrims(trims);` after the metadata push. (If a push site does not build per-source lists from `m_currentSettings.sources`, leave it — `setSourceTrims` tolerates absence; live edits still route via Step 4.)

- [ ] **Step 3: Implement the accessors.** In `uimanager.cpp`, after `updateUrl` (~line 1193) add:

```cpp
int UIManager::sourceTrimOffset(int sourceIndex) const {
    if (sourceIndex < 0 || sourceIndex >= m_currentSettings.sources.size()) return 0;
    return m_currentSettings.sources[sourceIndex].trimOffsetMs;
}

void UIManager::setSourceTrimOffset(int sourceIndex, int ms) {
    if (sourceIndex < 0 || sourceIndex >= m_currentSettings.sources.size()) return;
    const int clamped = qBound(-500, ms, 500);
    if (m_currentSettings.sources[sourceIndex].trimOffsetMs == clamped) return;
    m_currentSettings.sources[sourceIndex].trimOffsetMs = clamped;
    m_replayManager->updateSourceTrim(sourceIndex, clamped);  // live (no-op if not recording)
    m_sourceTrimVersion++;
    emit sourceTrimChanged();
    m_settingsManager->save(m_configPath, m_currentSettings);
}
```

- [ ] **Step 4: Build clean:**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug`
Expected: clean build (MOC picks up the new Q_PROPERTY/Q_INVOKABLE/signal).

- [ ] **Step 5: Commit:**
```bash
git add uimanager.h uimanager.cpp
git commit -m "feat(trim): UIManager exposes/persists/routes per-source trim"
```

---

## Task 5: `Main.qml` source-row trim SpinBox

**Files:**
- Modify: `Main.qml`

- [ ] **Step 1: Insert the SpinBox.** In `Main.qml`, in the source-row delegate, immediately AFTER the connection-indicator `Rectangle { id: connDot ... }` block (it ends right before `TextField { ... placeholderText: "ID" ... }`, ~line 1219) insert:

```qml
                        SpinBox {
                            id: trimSpin
                            from: -500
                            to: 500
                            stepSize: 33   // ≈ 1 frame @30fps; stored value is ms
                            editable: true
                            Layout.preferredWidth: 96
                            value: appWindow.uiManagerRef.sourceTrimVersion >= 0
                                   ? appWindow.uiManagerRef.sourceTrimOffset(streamRow.index) : 0
                            onValueModified: appWindow.uiManagerRef.setSourceTrimOffset(streamRow.index, value)
                            ToolTip.visible: hovered
                            ToolTip.text: "Timeline trim (ms): + delays this camera, − advances it"
                        }
```

- [ ] **Step 2: Build (the QML compiles into the app via qmlcachegen):**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug OpenLiveReplay`
Expected: clean build; `Main_qml.cpp` regenerated without error.

- [ ] **Step 3: qmllint (CI runs this; only errors fail it):**

Run: `$HOME/Qt/6.10.1/macos/bin/qmllint -I $HOME/Qt/6.10.1/macos/qml Main.qml 2>&1 | grep -iE "error" || echo "no errors"`
Expected: `no errors` (the pre-existing layout/unqualified-access *warnings* are unchanged; our SpinBox uses `Layout.preferredWidth`, adding none).

- [ ] **Step 4: Commit:**
```bash
git add Main.qml
git commit -m "feat(trim): per-source trim SpinBox in the source row"
```

---

## Task 6: e2e proof — `sync_harness --trim` + `intercam_trim` scenario

**Files:**
- Modify: `tests/e2e/sync_harness.cpp`, `tests/e2e/run_sync_e2e.sh`, `tests/e2e/CMakeLists.txt`, `tests/e2e/SYNC_BASELINE.md`

- [ ] **Step 1: Add `--trim` to `sync_harness`.** In `tests/e2e/sync_harness.cpp`, after the existing arg parse (e.g. after `const QString outdir = ...;`) add:

```cpp
    const int trimMs = argValue(args, QStringLiteral("--trim"), QStringLiteral("0")).toInt();
```

Then, after `rm.updateViewMapping(viewSlotMap);` (just after `startRecording()` succeeds), add — trim the LAST source (view 1 in 2-source scenarios; the only source in single-source ones), matching spec §7:

```cpp
    if (trimMs != 0 && n >= 1) {
        rm.updateSourceTrim(n - 1, trimMs);
    }
```

- [ ] **Step 2: Build the harness:**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug sync_harness`
Expected: clean build.

- [ ] **Step 3: Add the `intercam_trim` scenario.** In `tests/e2e/run_sync_e2e.sh`, insert a new branch immediately AFTER the `intercam_skew) ... ;;` branch and BEFORE `drift_2997)`:

```bash
  intercam_trim)
    # Same staggered two-source setup as intercam_skew (source B started D late),
    # but apply a +D trim to source B so the engine SHOULD pull it back toward A.
    # Reports the corrected offset next to D so the correction is visible.
    D_MS=${SKEW_MS:-250}
    TRIM_MS=${TRIM_MS:-$D_MS}
    P0=$BASE_PORT; P1=$((BASE_PORT+1))
    produce "$P0" 30 0
    sleep "$(awk -v d="$D_MS" 'BEGIN{printf "%.3f", d/1000}')"
    produce "$P1" 30 0
    sleep 0.5
    MKV=$("$HARNESS" --url "$(url "$P0")" --url "$(url "$P1")" \
            --outdir "$WORKDIR" --name intercam_trim --seconds 8 --fps 30 --trim "$TRIM_MS" | tail -n1)
    if [ -z "$MKV" ] || [ ! -s "$MKV" ]; then emit "[sync] scenario=intercam_trim ERROR=no_output"; echo "PASS: report emitted (diagnostic)"; exit 0; fi
    expect_video_tracks "$MKV" 2 || { emit "[sync] scenario=intercam_trim ERROR=wrong_track_count"; echo "PASS: report emitted (diagnostic)"; exit 0; }

    flash_pts_series "$MKV" 0 > "$WORKDIR/v0.txt"
    flash_pts_series "$MKV" 1 > "$WORKDIR/v1.txt"
    STATS=$(paste "$WORKDIR/v0.txt" "$WORKDIR/v1.txt" | awk '
        NF==2 { d=($1-$2)*1000; s+=d; ss+=d*d; n++ }
        END { if(n>0){ m=s/n; v=ss/n-m*m; if(v<0)v=0; printf "%d %.1f %.1f", n, m, sqrt(v) } else printf "0 nan nan" }')
    read -r NP MEAN STDEV <<<"$STATS"
    emit "[sync] scenario=intercam_trim flashes_paired=${NP} intercam_offset_ms: mean=${MEAN} stdev=${STDEV} (skew=${D_MS} trim_applied=${TRIM_MS})"
    echo "PASS: report emitted (diagnostic, non-gating)"
    exit 0
    ;;
```

- [ ] **Step 4: Run it and compare to the untrimmed skew.** With the same injected skew, the untrimmed `intercam_skew` measured `mean≈-250..-267 ms`; applying `--trim 250` to source B should move the trimmed `mean` toward **0** (residual = skew − trim, plus quantization):

```bash
cd /tmp/olr-trim
H=build/claude-debug/tests/e2e/sync_harness
echo "--- untrimmed baseline ---"; bash tests/e2e/run_sync_e2e.sh "$H" intercam_skew 23482
echo "--- trimmed ---";           bash tests/e2e/run_sync_e2e.sh "$H" intercam_trim 23488
```
Expected: the `intercam_trim` `mean` magnitude is substantially **smaller** than `intercam_skew`'s (ideally within a few tens of ms of 0). Report the two numbers. (Run-to-run noise applies; the *shift* between the two is the signal.) Also `bash -n tests/e2e/run_sync_e2e.sh`.

- [ ] **Step 5: Confirm lip-sync is preserved under a trim.** Run the `lipsync` scenario through a trimmed source and confirm the A/V offset is essentially unchanged vs untrimmed (the trim shifts video and audio together):

```bash
echo "--- lipsync no trim ---"; bash tests/e2e/run_sync_e2e.sh "$H" lipsync 23486
# temporary manual trim check: lipsync is single-source, so --trim hits source 0
"$H" --url "$(echo 'udp://127.0.0.1:23486?fifo_size=1000000&overrun_nonfatal=1')" --outdir /tmp >/dev/null 2>&1 || true
```
(The committed `lipsync` scenario stays untrimmed; this manual check just sanity-confirms the trim doesn't break A/V — note the av_offset before/after in your report. A dedicated trimmed-lipsync CTest is out of scope; the principle is that video+audio use the same `trimSamples`.)

- [ ] **Step 6: Register the CTest (non-gating `sync-report` label).** In `tests/e2e/CMakeLists.txt`, in the sync-report block, add the test (use port 23488) and include it in the `set_tests_properties` list:

```cmake
add_test(NAME sync_intercam_trim
    COMMAND bash "${_sync_driver}" "$<TARGET_FILE:sync_harness>" intercam_trim 23488)
```
and add `sync_intercam_trim` to the `set_tests_properties(... PROPERTIES LABELS "sync-report" ...)` name list.

- [ ] **Step 7: Reconfigure + verify label separation + append baseline.**
```bash
cd /tmp/olr-trim
cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug sync_harness
( cd build/claude-debug && ctest -N -L sync-report )   # expect 5 sync_* tests incl. sync_intercam_trim
bash tests/e2e/run_sync_e2e.sh build/claude-debug/tests/e2e/sync_harness intercam_trim 23488 --write-baseline
```
Append the `[sync] scenario=intercam_trim ...` line under the dated heading in `tests/e2e/SYNC_BASELINE.md` (the `--write-baseline` run does this). Confirm it landed.

- [ ] **Step 8: Commit:**
```bash
git add tests/e2e/sync_harness.cpp tests/e2e/run_sync_e2e.sh tests/e2e/CMakeLists.txt tests/e2e/SYNC_BASELINE.md
git commit -m "test(trim): sync_harness --trim + intercam_trim scenario proves the correction"
```

---

## Task 7: Full verification + clang-format + PR

**Files:** none (process)

- [ ] **Step 1: Full build + gating suite (no regression).**
```bash
cd /tmp/olr-trim/build/claude-debug
ctest -L e2e --output-on-failure        # 9/9 (e2e_play_reverse is a known flake; rerun if it trips)
ctest -L unit --output-on-failure       # all unit incl. tst_settingsmanager
```
Expected: unit green; e2e 9/9 (if `e2e_play_reverse` flakes on reposition, re-run that one — it is pre-existing and unrelated).

- [ ] **Step 2: clang-format the changed C++ lines (pre-empt the CI lint gate).**
```bash
cd /tmp/olr-trim
GCF=/opt/homebrew/opt/llvm/bin/git-clang-format CF=/opt/homebrew/opt/llvm/bin/clang-format
base=$(git merge-base origin/main HEAD)
"$GCF" --binary "$CF" --commit "$base" --extensions cpp,h,hpp,mm,c --force
"$GCF" --binary "$CF" --commit "$base" --diff --extensions cpp,h,hpp,mm,c   # expect "did not modify"
```
If it reformatted, `git commit -am "clang-format: per-camera trim"`.

- [ ] **Step 3: Push + open PR.**
```bash
SKIP_IOS_BUILD=1 git push -u origin feat/per-camera-trim-offset
gh pr create --base main --title "Per-camera trim offset (PHASE-6)" --body "<summary: per-source ±500ms live ms trim; applied to video gate+pre-drain+audio read so A/V shift together, output timeline fixed; SpinBox in source row; proven by intercam_trim e2e — links spec/plan>"
```

- [ ] **Step 4: Watch CI green** (`gh pr checks <n>`): Build+Test, Lint, both sanitizers. The `sync-report` tests stay excluded via `-LE sync-report`; `sync_harness` still compiles.

---

## Self-Review

**Spec coverage:**
- §2 units(ms)/range(±500)/live/A-V → Task 1 (ms field), Task 2 (clamp ±500 in `setTrimOffsetMs`, live atomic, audio+video together). ✓
- §3 mechanism (video gate :149, pre-drain :94, audio srcStart :1042, output timeline fixed) → Task 2 Steps 2-4. ✓
- §4 components (settings/streamworker/replaymanager/uimanager/qml) → Tasks 1-5. ✓
- §5 UI SpinBox → Task 5. ✓
- §6 bounds/clamp (engine + UIManager), unmapped-source stored-then-applied → Task 2 setter clamp, Task 4 `setSourceTrimOffset` clamp, Task 3 seed regardless of view. ✓
- §7 testing (settings round-trip; `sync_harness --trim` on last source; `intercam_trim` scenario; lipsync-under-trim) → Task 1, Task 6. ✓

**Placeholder scan:** the PR body (Task 7 Step 3) is an intentional fill-at-time summary (not code). All code steps are complete with exact edits and line anchors. No TBD/TODO.

**Type/name consistency:** `trimOffsetMs` (settings field), `m_trimOffsetMs`/`setTrimOffsetMs`/`kMaxTrimMs` (StreamWorker), `m_sourceTrims`/`setSourceTrims`/`updateSourceTrim` (ReplayManager), `sourceTrimOffset`/`setSourceTrimOffset`/`sourceTrimVersion`/`sourceTrimChanged`/`m_sourceTrimVersion` (UIManager), `--trim`/`intercam_trim`/`sync_intercam_trim` (e2e) — used consistently across tasks. The clamp bound 500 appears as `kMaxTrimMs` in the engine and the literal `qBound(-500,ms,500)` in UIManager (both ±500, consistent with the SpinBox `from:-500 to:500`).

**Known fragile point:** Task 6 Step 4's trimmed-vs-untrimmed comparison relies on synthetic-localhost timing; the *shift* between the two numbers is robust even though each absolute number is noisy. Flagged in the step.
