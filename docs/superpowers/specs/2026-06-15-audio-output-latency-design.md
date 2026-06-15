# Audio Output-Latency Offset (LIPSYNC-3) — Design

**Status:** approved (brainstorm) → spec
**Date:** 2026-06-15
**Branch:** `feat/audio-output-latency`

## 1. Motivation

During playback, video is presented to the screen near-immediately, but audio
travels through the OS audio buffer + DAC/HDMI/Bluetooth output chain, which adds
**100–300 ms** of latency before the listener actually hears it — so audio is
heard *late* relative to video. Qt's `QAudioSink` exposes **no API** to query that
hardware/driver/Bluetooth latency, so it cannot be auto-compensated.

PR #23 already shipped the mechanism: `AudioPlayer::kOutputLatencyOffsetMs`, a
`static constexpr` (default 0) folded into the playback alignment math, with a
documented note that "a future user-facing audio-offset setting could drive it"
([audioplayer.h:148-157](playback/audioplayer.h)). This is that setting: make the
offset a runtime-adjustable, persisted, user-facing control. It is the LIPSYNC-3
P0 quick-win from the broadcast frame-sync roadmap.

## 2. Decisions (from brainstorm)

- **Units:** milliseconds (the alignment math is ms/sample based).
- **Range:** **0…+500 ms**, positive-only. Output devices add a *positive* delay
  (audio heard late → schedule it earlier to compensate); 500 ms covers every real
  output (Bluetooth ~300 ms) with headroom. No negative offsets.
- **Global** (one setting, not per-source / not per-device): Qt exposes neither a
  device-latency API nor device enumeration here, so a single manual knob is the
  realistic scope.
- **Live-adjustable** and persisted in the app config.

## 3. The resync coupling (the one subtlety)

The playback aligner re-aligns the stream whenever the steady-state pts-vs-master
divergence exceeds `kResyncThresholdMs = 250` ms
([audioplayer.cpp:247-250](playback/audioplayer.cpp)). Raising the output-latency
offset **grows that steady-state divergence by the same amount**
([audioplayer.h:140-147](playback/audioplayer.h) documents this coupling): the
aligned branch schedules audio `offset` ms ahead of master, so `|pts − due| ≈
offset` in steady state. With a fixed 250 ms threshold, an offset near/above 250 ms
would re-align on **every push** (an audible storm), breaking the primary
Bluetooth use case.

**Fix:** make the resync trigger **scale with the offset** so the genuine-desync
headroom stays constant:

```
resyncThreshold = kResyncHeadroomMs + m_outputLatencyOffsetMs
```

where `kResyncHeadroomMs = 250` (the current constant, renamed to reflect its new
role as the *headroom above the expected offset divergence*). At offset 0 this is
identical to today's behavior (250); at offset 300 the threshold becomes 550, so a
genuine desync is still caught when it exceeds the expected 300 ms divergence by
250 ms. The re-align trigger therefore stays meaningful at every offset.

## 4. Components & data flow

| Unit | Change | Responsibility |
|---|---|---|
| `settingsmanager.h` | `int audioOutputLatencyMs = 0;` in `AppSettings` | data model |
| `settingsmanager.cpp` | `save()`: `root["audioOutputLatencyMs"] = settings.audioOutputLatencyMs;`  `load()`: `settings.audioOutputLatencyMs = root["audioOutputLatencyMs"].toInt(settings.audioOutputLatencyMs);` (mirrors `showTimeOfDay`) | persistence |
| `playback/audioplayer.h` | remove `static constexpr int kOutputLatencyOffsetMs = 0;`; add `std::atomic<int> m_outputLatencyOffsetMs{0};` + `void setOutputLatencyOffsetMs(int ms)` (clamped `qBound(0, ms, kMaxOutputLatencyMs)`, `kMaxOutputLatencyMs = 500`); rename `kResyncThresholdMs` → `kResyncHeadroomMs` (value unchanged, 250) | runtime offset state |
| `playback/audioplayer.cpp` | latency math (`:233`): use `m_outputLatencyOffsetMs.load()` instead of the constant; resync (`:248`): `resyncSamples = int64_t(kResyncHeadroomMs + m_outputLatencyOffsetMs.load()) * m_sampleRate / 1000` | apply offset + scaled resync |
| `uimanager.h/.cpp` | `Q_PROPERTY(int audioOutputLatencyMs READ audioOutputLatencyMs WRITE setAudioOutputLatencyMs NOTIFY audioOutputLatencyChanged)` (mirrors `recordWidth`); getter returns `m_currentSettings.audioOutputLatencyMs`; setter clamps 0–500, writes the setting, calls `m_audioPlayer->setOutputLatencyOffsetMs(...)`, saves config, emits; **seed** `m_audioPlayer->setOutputLatencyOffsetMs(m_currentSettings.audioOutputLatencyMs)` after settings load (the AudioPlayer is created in the ctor; settings load applies the saved value) | expose, persist, route |
| `Main.qml` | a ms `SpinBox` (`from: 0  to: 500  stepSize: 10`) in the settings area next to the record-format SpinBoxes, bound to `uiManagerRef.audioOutputLatencyMs`, with a label "Audio output latency (ms)" + tooltip | operator control |

**Live edit flow:** SpinBox → `setAudioOutputLatencyMs(ms)` → clamp →
`m_currentSettings.audioOutputLatencyMs = ms` → `m_audioPlayer->setOutputLatencyOffsetMs(ms)`
(atomic; the push/align path reads it next push) → save config → emit.

**Concurrency:** `m_outputLatencyOffsetMs` is written by the UI thread and read in
`pushSamples` (align path). `std::atomic<int>` with `memory_order_relaxed` — a
standalone scalar, no associated data to synchronize (same pattern as the
per-source trim's `m_trimOffsetMs`).

## 5. Error handling / bounds

- Both `setOutputLatencyOffsetMs` (engine) and `setAudioOutputLatencyMs` (UI) clamp
  to `[0, 500]`; an out-of-range config value loads clamped.
- A live change takes effect on the next `pushSamples`; an in-flight aligned stream
  simply re-evaluates against the new (scaled) threshold — at worst one benign
  re-align at the moment of a large change (a brief de-click, already handled by the
  existing splice fade). Acceptable for a set-once calibration knob.

## 6. Testing

- **Unit (`tests/unit/`):** `tst_settingsmanager` round-trips `audioOutputLatencyMs`
  through save/load (and clamps an out-of-range loaded value). (A standalone
  AudioPlayer align unit test is not pursued — `start()` needs a real `QAudioSink`
  backend that is unreliable headless; the resync behavior is proven by the e2e
  `resyncCount` gate instead.)
- **E2E (`tests/e2e/play_harness.cpp` + `run_playback_e2e.sh`):** there is **no
  existing resync counter** — add one. `AudioPlayer` gains an `int m_resyncCount`
  incremented each time the aligned branch re-aligns (the `m_aligned = false` at
  [audioplayer.cpp:249](playback/audioplayer.cpp)) plus an `int resyncCount() const`
  getter. `play_harness` reads `OLR_AUDIO_LATENCY_MS` (default 0) → `audio.setOutputLatencyOffsetMs(...)`,
  and appends `resyncCount=<n>` to its `COUNTERS` line. A new driver scenario
  `latency` exports `OLR_AUDIO_LATENCY_MS=300`, runs the `play1x` playback, and
  asserts **`resyncCount == 0`** (no re-align storm) and `audioPushes > 0`. This
  headlessly proves the coupling fix: without the threshold scaling, a 300 ms
  offset would storm re-aligns and `resyncCount` would climb.
  - **Note:** the e2e proves the *no-storm* direction (the change's main risk). The
    complementary "still catches a genuine >threshold desync" is a defensive
    self-heal backstop (for an uncleaned seek the normal `clear()` path never
    hits); the scaling keeps the threshold finite (`250 + offset`), so detection is
    preserved by construction — not separately e2e-tested.
- **Manual:** set the offset in-app on a Bluetooth output and confirm lip-sync.

## 7. Out of scope (YAGNI)

- Per-output-device presets / auto-detection (no Qt device-latency API; no device
  enumeration wired here).
- Negative offsets (positive-only per the brainstorm).
- Recording-side A/V offset (AUD-4) — a separate roadmap item; this knob is
  playback-output only.

## 8. Risks

- **Headless audio in CI:** `QAudioSink` may not emit real audio on the CI runner,
  so the e2e check validates the *internal* align/resync behavior (clear/push
  counters), not actual audibility. True A/V is a manual check. Documented.
- **Resync-coupling correctness** is the load-bearing claim; the unit test (offset
  tolerated, genuine desync still caught) plus the e2e no-storm gate are designed
  specifically to catch a regression in the threshold-scaling.
