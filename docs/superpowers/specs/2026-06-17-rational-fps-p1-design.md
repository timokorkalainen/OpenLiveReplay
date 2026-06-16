# P1: rational recording frame rate (int fps → FrameRate {num,den}) — Design

**Status:** approved-by-autonomy (the user authorized 100% autonomous execution of "the big P1 rational-fps push" while away; no interactive approval gate).
**Roadmap item:** framesync P1 (FRAC-1/2, MUX-2). See `broadcast-framesync-roadmap`.
**Base branch:** `feat/rational-fps`, **stacked on `feat/heartbeat-decouple`** (PR #49). The P1 PR targets `feat/heartbeat-decouple` until #49 merges, then rebases onto `main`.

## Goal

Represent the recording frame rate as a **rational** `num/den` everywhere in the recording engine so fractional broadcast rates — 29.97 (30000/1001), 59.94 (60000/1001), 23.976 (24000/1001) — record with **correct frame timing and a correct container `r_frame_rate`**, instead of being forced to integer 30/60. The heartbeat decouple (P0, #49) already moved the timeline to wall-clock + isolated the frame math behind `heartbeatFrameSpan`; this swaps the integer `fps` for a rational across the chain.

## Scope

**In scope (recording engine + the path to select a rational rate):**
- A pure `FrameRate {int num; int den;}` value type with the conversion helpers.
- Migrate the engine's integer fps to `FrameRate`: `ReplayManager`, `StreamWorker`, `Muxer`, the blue-frame encoder, and `heartbeatFrameSpan`.
- Encoder time_base `{den,num}` + framerate `{num,den}`; muxer `avg/r_frame_rate = {num,den}`.
- Settings persistence (`fpsNum`/`fpsDen`, backward-compatible with legacy `"fps": <int>`).
- A UI control to pick a fractional rate (a preset ComboBox) + `--fps` accepting `num/den` in the harnesses.
- An e2e gate that records a true 29.97 and proves the output `r_frame_rate`.

**Out of scope (deferred; noted as follow-ups):**
- **Drop-frame timecode** (TC-2) and timecode side-data — a separate display feature.
- **Rational playback stepping** — `PlaybackTransport` keeps an integer fps (the *rounded* rate); step-by-frame on a 29.97 file is off by ~0.1%, acceptable for now. (`UIManager::setRecordFps` forwards `rate.roundedFps()` to the transport.)
- Non-`num/1001` arbitrary rates beyond the preset list (the engine accepts any `num/den`, but the UI offers the standard presets).

## The `FrameRate` type

New pure header `recorder_engine/framerate.h` (no FFmpeg/Qt-UI deps; usable by the engine, settings, UI, and the pure `heartbeat`):

```cpp
struct FrameRate {
    int num = 30;
    int den = 1;
    // ms of one frame index on the file timeline:  f * 1000 * den / num
    int64_t msForFrame(int64_t f) const;       // replaces (f*1000)/fps
    // frame index reached at a given wall-clock ms:  ms * num / (1000*den)
    int64_t frameForMs(int64_t ms) const;      // replaces (ms*fps)/1000
    // audio samples per frame at sampleRate:  sampleRate * den / num  (truncated)
    int64_t samplesPerFrame(int sampleRate) const;
    int roundedFps() const;                    // (num + den/2) / den, for int consumers
    double toDouble() const;                   // num / den
    bool isValid() const;                      // num > 0 && den > 0
};
bool operator==(const FrameRate&, const FrameRate&);
```

Plus a small presets table (label ↔ FrameRate) in `framerate.{h,cpp}` for the UI/harness:

```cpp
struct FrameRatePreset { const char* label; FrameRate rate; };
// {"23.976",{24000,1001}},{"24",{24,1}},{"25",{25,1}},{"29.97",{30000,1001}},
// {"30",{30,1}},{"50",{50,1}},{"59.94",{60000,1001}},{"60",{60,1}}
const std::vector<FrameRatePreset>& frameRatePresets();
FrameRate parseFrameRate(const QString& s);   // "30", "29.97", "30000/1001" -> FrameRate
QString frameRateLabel(const FrameRate&);      // nearest preset label, else "num/den"
```

`framerate.cpp` is added to `olr_test_core` (pure Qt-Core) and the app target. The integer-fps math is now centralized in these helpers — the same arithmetic, expressed rationally.

## Engine migration

The integer members become `FrameRate`, and every arithmetic site calls the helper:

- **ReplayManager:** `int m_fps` → `FrameRate m_frameRate`. Keep `setFps(int)` (builds `{n,1}`) **and** add `setFrameRate(FrameRate)` / `FrameRate frameRate() const` (so existing int callers keep working through the migration). `onTimerTick`: `heartbeatFrameSpan(elapsedMs, m_frameRate, m_globalFrameCount, kMaxFramesPerTick, m_frameRate.roundedFps())`; `frameMs = m_frameRate.msForFrame(f)`. Blue encoder `time_base = {m_frameRate.den, m_frameRate.num}`, `framerate = {m_frameRate.num, m_frameRate.den}`. Blue audio cursor uses `m_frameRate.samplesPerFrame(kAudioSampleRate)`. `Muxer::init(... m_frameRate ...)`. `StreamWorker(... m_frameRate ...)`.
- **StreamWorker:** `int m_targetFps` → `FrameRate m_targetRate` (ctor param becomes `FrameRate`). `currentRecordingTimeMs = m_targetRate.msForFrame(m_internalFrameCount)`; the jitter-gate publish uses `m_targetRate.msForFrame(frameIndex)`; encoder `time_base = {den,num}`, `framerate = {num,den}`; audio cursor uses `m_targetRate.samplesPerFrame(kAudioSampleRate)`. The MPEG-2 exact-rate warning switches on `roundedFps()` and also warns for any `den != 1`.
- **Muxer:** `init(..., FrameRate rate, ...)`; `avg_frame_rate = {rate.num, rate.den}`, `r_frame_rate = {rate.num, rate.den}`; stream `time_base` stays `{1,1000}` (ms — unchanged, correct for any rate). Validate `if (!rate.isValid()) rate = {30,1}`.
- **`heartbeatFrameSpan`:** signature `int fps` → `FrameRate rate`; `derivedFrame = rate.frameForMs(elapsedMs)`; the rest unchanged. `tst_heartbeat` updates to pass `FrameRate`.

**Encoder timebase correctness:** a frame rate of `{num,den}` has one-frame duration `den/num` s, so encoder `time_base = {den,num}` and PTS = frame index; `av_packet_rescale_ts` to the muxer's `{1,1000}` then yields `frame*den/num*1000` ms — exact fractional timing. For integer 30 this is `{1,30}` / `{30,1}` — **byte-identical to today** (so all integer-fps gates stay green).

## Settings persistence (backward-compatible)

`AppSettings`: `int fps` → `int fpsNum = 30; int fpsDen = 1;`. Save: `root["fpsNum"]=fpsNum; root["fpsDen"]=fpsDen;`. Load: `fpsNum = root["fpsNum"].toInt(root["fps"].toInt(30)); fpsDen = root["fpsDen"].toInt(1);` — a legacy `"fps": 30` loads as `{30,1}`. (We stop writing the legacy `"fps"` key; old readers are not a concern — this is the same app.)

## UI

Replace the integer `SpinBox` (`Main.qml:1200-1209`) with a **ComboBox of presets**. UIManager:
- `Q_PROPERTY(QStringList frameRatePresetLabels READ frameRatePresetLabels CONSTANT)` — the labels.
- `Q_PROPERTY(int frameRateIndex READ frameRateIndex WRITE setFrameRateIndex NOTIFY frameRateChanged)` — index into the preset list of the current rate (nearest preset; falls back to appending a "num/den" entry if a loaded config matches no preset).
- `setFrameRateIndex(int)` maps to the preset `FrameRate`, sets `m_currentSettings.fpsNum/Den`, calls `m_replayManager->setFrameRate(rate)` and `m_transport->setFps(rate.roundedFps())`, guarded by not-recording.
- Keep `recordFps()` (int, = `roundedFps()`) for any display/back-compat readers.

## Harnesses

`record_harness.cpp` / `sync_harness.cpp`: `--fps` value parsed via `parseFrameRate()` (accepts `30`, `29.97`, `30000/1001`); call `rm.setFrameRate(parseFrameRate(...))`. Default `"30"` → `{30,1}` (unchanged behavior).

## Testing

1. **Unit — `FrameRate`** (`tst_framerate.cpp`, `olr_test_core`): `msForFrame`/`frameForMs` round-trip for `{30,1}` and `{30000,1001}` (frame 30 of 29.97 → 1001 ms; ms 1001 → frame 30); `samplesPerFrame`; `roundedFps()` (29.97→30, 59.94→60); `parseFrameRate("29.97"|"30000/1001"|"30")`; `frameRateLabel`.
2. **Unit — `heartbeatFrameSpan` rational** (extend `tst_heartbeat.cpp`): `{30000,1001}` at `elapsedMs=1001` → frame 30; integer-30 cases unchanged.
3. **Unit — settings round-trip** (extend `tst_settingsmanager.cpp` if present, else a focused check): save+load preserves `{30000,1001}`; legacy `"fps":30` loads as `{30,1}`.
4. **E2e — `e2e_record_2997`** (new, `e2e` label): record at `--fps 29.97`. The Matroska muxer turns `st->avg_frame_rate = {30000,1001}` into the track `DefaultDuration`, so `ffprobe -select_streams v:0 -show_entries stream=avg_frame_rate` reads back `30000/1001`. **Assert the parsed `avg_frame_rate` is in `(29.9, 30.0)`** — 29.97 passes, integer 30.0 fails (they are 0.03 apart, so the band cleanly discriminates; pre-P1 the output is 30/1 and FAILS). Also sanity-check the video packet count is in the ~29.97×duration band. This is the discriminating proof the output is a true fractional rate.
5. **No regression:** `e2e_record_stereo`/`e2e_record_mono` (int 30) and `ctest -L unit` stay green — integer 30 is byte-identical (`{1,30}`/`{30,1}`). The `srt`/`native-apple-ingest`/`sync-report` gates (all record at int 30) stay green.

## Files touched

- `recorder_engine/framerate.{h,cpp}` (new) — the type + presets + parse/label.
- `recorder_engine/heartbeat.{h,cpp}` — `int fps` → `FrameRate`.
- `recorder_engine/replaymanager.{h,cpp}` — `m_frameRate`; setFps/setFrameRate; arithmetic + timebase via FrameRate.
- `recorder_engine/streamworker.{h,cpp}` — `m_targetRate`; ctor param; arithmetic + timebase.
- `recorder_engine/muxer.{h,cpp}` — `init(FrameRate)`; `avg/r_frame_rate`.
- `settingsmanager.{h,cpp}` — `fpsNum`/`fpsDen` + back-compat load.
- `uimanager.{h,cpp}` + `Main.qml` — preset ComboBox + properties.
- `tests/e2e/record_harness.cpp`, `sync_harness.cpp` — `--fps` rational parse.
- `tests/e2e/run_record_e2e.sh` (the `2997` scenario) + `tests/e2e/CMakeLists.txt` — the gate.
- `tests/unit/tst_framerate.cpp` (new), `tst_heartbeat.cpp` (extend), `tests/unit/CMakeLists.txt`, `tests/CMakeLists.txt`, root `CMakeLists.txt` — `framerate.cpp` in both targets + the unit test.

## Success criteria

- Recording at 29.97/59.94 produces an MKV whose `r_frame_rate` is `30000/1001` / `60000/1001` and whose frame PTS spacing is the true fractional interval (e2e-proven).
- Integer 30/60 recordings are unchanged (all existing gates green; encoder timebase byte-identical).
- The frame rate is a single `FrameRate` type end-to-end in the recording engine; settings persist num/den (back-compat); the UI selects a rate from presets.
- Drop-frame TC and rational playback stepping remain explicitly deferred.
