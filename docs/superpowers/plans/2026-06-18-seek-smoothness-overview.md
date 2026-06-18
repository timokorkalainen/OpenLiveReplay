# Seek Smoothness — Overview & Cross-Tier Parallelization

> **For agentic workers:** This is the umbrella document for three tiered TDD plans. Read it first to understand the root cause, the execution order across tiers, and which tasks may run concurrently. Then execute the per-tier plans with superpowers:subagent-driven-development or superpowers:executing-plans.
>
> Per-tier plans:
> - `docs/superpowers/plans/2026-06-18-seek-smoothness-tier1-quick-wins.md`
> - `docs/superpowers/plans/2026-06-18-seek-smoothness-tier2-structural.md`
> - `docs/superpowers/plans/2026-06-18-seek-smoothness-tier3-frame-perfect-playlist.md`

## Root cause

The user sees a **gray flash** during scrubbing/seeking. The mechanism is a race between the two playback threads:

- **PlaybackWorker** (QThread) decodes into `m_outputCache` (and per-track `TrackBuffer`s) under `m_bufferMutex`. On every full reposition (`repositionTo`, `playbackworker.cpp:564`) and skip-forward (`:883`) it calls `clearAllBuffers()` (`:231`), which **clears `m_outputCache`** (`:237`).
- **OutputRuntime** (QThread) ticks ~1ms, snapshots the cache via `makeOutputSnapshot` (`:362`), and renders through `OutputDispatcher::dispatchTick` to the sinks.

Because the output thread paints **exclusively** from `m_outputCache`, the instant the worker wipes that cache mid-reposition the very next ~1ms tick snapshots an empty cache, `videoFrameOrPlaceholder` returns the gray 16/128/128 placeholder, and the dispatcher submits a gray frame — the flash. A secondary artifact is an **audio click**, because `AudioPlayer::setMuted(true)` and the per-move `m_audioPlayer->clear()` in `seekPlayback` hard-cut the ring buffer mid-sample.

The three tiers attack this in increasing depth:

- **Tier 1 (quick wins)** stops blanking the cache on reposition, holds the last good frame at the dispatcher if a placeholder ever slips through, delivers the target frame first, coalesces UI scrub spam, and de-clicks mute. No architecture change.
- **Tier 2 (structural)** makes the playhead itself *commit* against a provably-ready cache (generation token + `CommitGate`), double-buffers the reposition decode into staging before trimming, publishes the cache as an immutable `shared_ptr<const OutputFrameCache>` (removing the per-tick deep copy), and skips redundant identical sink submits.
- **Tier 3 (frame-perfect playlist)** adds an EVS-style timestamp playlist: a `FrameIndex` for exact `avio_seek` repositioning, a `ReplayPlaylist` cue list, and an atomic pre-rolled cut that promotes a staging cache → active exactly at a scheduled output frame, riding on Tier 2's generation token and shared_ptr swap.

## Execution order & dependencies

```
Tier 1  ──────────────►  Tier 2  ──────────────►  Tier 3
(independent,            (depends on Tier 1)      (depends on Tier 2,
 shippable alone)                                  and on Tier 1)
```

- **Tier 1 is fully independent and shippable on its own.** It introduces the SHARED SYMBOL CONTRACT the later tiers consume but depends on nothing upstream. It can be merged and released before Tier 2/3 exist.
- **Tier 2 depends on Tier 1** for these contract symbols:
  - `PlaybackWorker::clearDecoderBuffers()` — the rename of `clearAllBuffers()` that **drops the `m_outputCache->clear()`** (Tier 1 Task 1). Tier 2's double-buffer (Task 2) relies on the live cache retaining old frames across a reposition; if Tier 2 lands first, the old frames are wiped and the staging merge has nothing to hold over.
  - **hold-last-frame** + `setHoldLastFrame(bool)` + `OutputDispatchStats::heldFrames` (Tier 1 Task 2) — the defense-in-depth partner: Tier 2's CommitGate + double-buffer ensure a placeholder is never produced; Tier 1's hold-last-frame catches any that slip through. The `farback` gate (`placeholderFrames==0`) only holds with both layers.
  - The `play_harness` COUNTERS line carrying `placeholderFrames`/`heldFrames` and the `seekflash` scenario (Tier 1 Task 2) — Tier 2 **extends** that same `printf` (it must remain a printf format-string append, not stream insertion) with `skippedDuplicateFrames`/`cacheGeneration` and adds `farback`.
- **Tier 3 depends on Tier 2** for:
  - the **generation token** `std::atomic<uint64_t> m_cacheGeneration` (Tier 2 Task 1) — Tier 3's cut bumps it so the committed playhead reflects the freshly promoted, already-target-covering cache immediately (keeping `placeholderFrames==0`).
  - the **`shared_ptr<const OutputFrameCache>` published cache** (Tier 2 Task 3) — Tier 3's staging→active promotion becomes a single atomic pointer swap under `m_bufferMutex` rather than a deep copy.
  - `OutputDispatcher::nextOutputFrameIndex()` (already present, `outputdispatcher.h:82`) + Tier 2 identity-skip — `CutSchedule::shouldFireAt` reads the next index; identity-skip avoids redundant submits at the cut boundary.
- **Tier 3 also depends on Tier 1** for `clearDecoderBuffers()` (so `repositionExact` does not wipe the output cache) and hold-last-frame (covers any one-tick gap during the pre-roll fill).
- Tier 3's **pure-logic lanes (FrameIndex, ReplayPlaylist, CutSchedule — Tasks 1-6)** and the pure UI wiring (Task 8 scaffolding) have **no Tier 2 dependency** and may be built any time. Only the worker-integration cut (Tasks 9-12) requires the Tier 2 token + shared_ptr cache.

## What can run in parallel

The table maps every task to the files it touches and its concurrency constraints. The serialization hotspots are the shared files: `playbackworker.cpp`, `outputdispatcher.cpp`, `uimanager.cpp`, `Main.qml`, and the two test CMake lists (`tests/unit/CMakeLists.txt`, `tests/e2e/CMakeLists.txt` / `play_harness.cpp` / `run_playback_e2e.sh`).

| Tier.Task | Files touched | Parallel-safe? | Conflicts with |
| --- | --- | --- | --- |
| **T1.1** rename + drop cache clear | `playbackworker.h/.cpp` | No | T1.4, T1.5, T1.3, T2.1, T2.2, T2.3, T3.7, T3.9-11 (all edit `playbackworker.cpp`) |
| **T1.2** dispatcher hold-last + seekflash E2E | `outputdispatcher.h/.cpp`, new `tst_outputdispatcher_holdlast.cpp`, `play_harness.cpp`, `run_playback_e2e.sh`, both test CMake | Yes (vs worker) | T2.4 (outputdispatcher.cpp); ALL E2E-harness tasks (owns the COUNTERS-line + seekflash gate) |
| **T1.3** uninitialized plane alloc | `playbackworker.cpp` | No | every `playbackworker.cpp` task |
| **T1.4** deliver-target-first | `playbackworker.cpp` (`repositionTo`) | No | T1.1, T1.5, T2.1, T2.2, T3.7 (all edit `repositionTo`) |
| **T1.5** deliverDueFrames dead-work | `playbackworker.cpp` (`deliverDueFrames`) | No | every `playbackworker.cpp` task |
| **T1.6** scrub coalescing (+T1.8) | `seekcoalescer.h` (new), `uimanager.h/.cpp`, `Main.qml`, `tst_seekcoalescer.cpp`, unit CMake | Yes (vs worker/dispatcher) | T3.8 (uimanager.cpp / Main.qml); unit CMake append |
| **T1.7** de-click mute | `audioplayer.cpp` (+ optional test) | Yes | unit CMake append (if test added) |
| **T2.1** CommitGate + token | `commitgate.h` (new), `playbackworker.h/.cpp` (`repositionTo` tail, `makeOutputSnapshot`), unit CMake | Header parallel-safe; wiring NOT | T2.2, T2.3 (`repositionTo` + `makeOutputSnapshot`), all worker tasks |
| **T2.2** mergeFrom + staging | `outputframecache.h/.cpp`, `playbackworker.h/.cpp` (`repositionTo` fill), `tst_outputframecache.cpp` | `mergeFrom` parallel-safe; staging wiring NOT | T2.1, T2.3 (`repositionTo`), all worker tasks |
| **T2.3** SharedCacheSlot + drop deep copy | `sharedcacheslot.h` (new), `playbackworker.h/.cpp` (insert/trim/`makeOutputSnapshot`), unit CMake | Header parallel-safe; wiring NOT | T2.1, T2.2 (rewrites `makeOutputSnapshot`, adds publish calls), all worker tasks |
| **T2.4** identity-skip | `outputdispatcher.h/.cpp`, `tst_outputdispatcher.cpp` | Yes (vs worker) | T1.2 (outputdispatcher.cpp / its test) |
| **T2.5** farback E2E + counters | `play_harness.cpp`, `run_playback_e2e.sh`, `e2e CMake`, `playbackworker.h` (accessor) | No | T1.2, T3.12 (all E2E-harness; depends on T2.1-2.4) |
| **T3.1-2** FrameIndex | `frameindex.h/.cpp`, `tst_frameindex.cpp`, both CMake | Yes (own lane) | only CMake appends |
| **T3.3-4** ReplayPlaylist | `replayplaylist.h/.cpp`, `tst_replayplaylist.cpp`, both CMake | Yes (own lane) | only CMake appends |
| **T3.5-6** CutSchedule | `cutschedule.h/.cpp`, `tst_cutschedule.cpp`, both CMake | Yes (own lane) | only CMake appends |
| **T3.7** wire FrameIndex + repositionExact | `playbackworker.h/.cpp` (read loop, `repositionTo`) | No | all worker tasks (needs T3.1) |
| **T3.8** UI markIn/markOut/recall | `uimanager.h/.cpp`, `Main.qml` | No | T1.6 (uimanager.cpp / Main.qml); needs T3.3 + `armNextCut` from T3.9 |
| **T3.9** pre-roll bank | `playbackworker.h/.cpp` (`openFile`, `run`) | No | all worker tasks (needs T3.1) |
| **T3.10** atomic cut | `playbackworker.h/.cpp`, `outputruntime.h/.cpp` | No | all worker tasks (needs T3.5-6 + Tier 2 token/shared_ptr) |
| **T3.11** crossfade audio | `playbackworker.h/.cpp`, `audioplayer.*` | No | all worker tasks; T1.7 (audioplayer) |
| **T3.12** armedcut E2E | `play_harness.cpp`, `run_playback_e2e.sh`, `e2e CMake` | No | T1.2, T2.5 (E2E-harness; depends on T3.9-11) |

### Recommended concrete work-streams

Run these streams concurrently; serialize *within* a stream as noted.

- **Stream A — Dispatcher (`outputdispatcher.*`):** T1.2 (hold-last + `heldFrames`) → T2.4 (identity-skip + `skippedDuplicateFrames`). Both edit `outputdispatcher.cpp` and its unit test, so they serialize *within* the stream but run fully parallel to the worker streams. One owner for `outputdispatcher.cpp`.
- **Stream B — Worker decode (`playbackworker.cpp`):** the single most contended file. Serialize strictly: T1.1 (rename, drop clear) → T1.4 (deliver-target-first) → T1.5 (deliverDueFrames dead-work) → T1.3 (uninitialized alloc) → **[Tier 1 merged]** → T2.1 (CommitGate wiring) → T2.2 (staging) → T2.3 (shared_ptr publish) → **[Tier 2 merged]** → T3.7 (repositionExact) → T3.9 (pre-roll) → T3.10 (atomic cut) → T3.11 (crossfade). **Anything editing `playbackworker.cpp:repositionTo` (T1.1, T1.4, T2.1, T2.2, T3.7) MUST serialize on this stream — they all rewrite the reposition tail/fill loop.**
- **Stream C — UI + audio de-click (`uimanager.*` / `Main.qml` / `audioplayer.*`):** T1.6 (+T1.8 folded) + T1.7 in Tier 1; then T3.8 (markIn/markOut/recall) in Tier 3. T1.6 and T3.8 both edit `uimanager.cpp` and `Main.qml`, so serialize them within the stream; T3.8 additionally needs `armNextCut` (T3.9) to exist before it builds.
- **Stream D — Pure-logic headers (parallel to everything):** the header-only / new-file tasks have no shared-file contention beyond CMake appends and can be built immediately by spare agents — T2.1 (`commitgate.h`), T2.3 (`sharedcacheslot.h`), T1.6 (`seekcoalescer.h`), and all of Tier 3 Tasks 1-6 (`frameindex.*`, `replayplaylist.*`, `cutschedule.*`). Their *wiring* into `playbackworker.cpp` rejoins Stream B.
- **Stream E — E2E harness (single owner):** `play_harness.cpp`, `run_playback_e2e.sh`, `tests/e2e/CMakeLists.txt` are touched by T1.2 (seekflash), T2.5 (farback), T3.12 (armedcut). **One owner**, applied in tier order, because all three extend the same COUNTERS `printf` and the same `set_tests_properties(... RUN_SERIAL)` list. Allocate **disjoint SRT/UDP port pairs** (see Risk register) — the play block is full through 23478 and 23480-23489 belongs to the sync suite.

### Serialization points (must-not-parallelize)

1. `playbackworker.cpp:repositionTo` — T1.1, T1.4, T2.1, T2.2, T3.7.
2. `playbackworker.cpp:makeOutputSnapshot` — T2.1, T2.3, T3.10.
3. `outputdispatcher.cpp:dispatchTick` — T1.2, T2.4.
4. `uimanager.cpp:seekPlayback` / `Main.qml` scrubBar — T1.6, T3.8.
5. `play_harness.cpp` COUNTERS `printf` — T1.2, T2.5, T3.12 (printf format-string, NOT `<<`).
6. `tests/unit/CMakeLists.txt` and `tests/e2e/CMakeLists.txt` — append-only, one line per task, in task order to avoid line conflicts.

## Risk register

| Risk | Tier.Task | Catching test |
| --- | --- | --- |
| Worker still blanks the output cache somewhere → gray flash returns | T1.1 | `e2e_play_seekflash` (`placeholderFrames==0`) |
| Held-last-frame masks a real decode failure (stale frame shown forever) | T1.2 | `tst_outputdispatcher_holdlast` (held frame equals prior real pixels; `heldFrames` counted, `placeholderFrames` stays 0) |
| Deliver-target-first converts back-step **reuse**-seeks into full repositions | T1.4 | `e2e_play_stepscrub` (`reposition<=10 && reuseSeek>=1`) |
| Uninitialized plane alloc leaks garbage into output | T1.3 | `e2e_play_storm`, `e2e_play_seekplay` (visual decode correctness); `tst_outputframecache` |
| Mute fade / removed UI clear silences playback | T1.7 / T1.8 | `e2e_play_seekplay`, `e2e_play_sliderscrub` (`audioPushes>0`) |
| **CommitGate freezes 1x video** by pinning the playhead to the last seek target (the gate only commits seek targets, never the advancing play position) | T2.1 | `e2e_play_storm` / `e2e_play_seekplay` (steady playback must keep advancing) — **highest regression risk; reconcile gate logic before shipping** |
| Staging swap loses old frames / breaks reverse fill | T2.2 | `e2e_play_reverse` (`reverseChunkSeek<=150 && reposition<=4`), `e2e_play_seekplay` |
| shared_ptr publication data-race | T2.3 | `tst_sharedcacheslot` (concurrent publish/load), TSan leg, `e2e_play_latency` (`resyncCount==0`) |
| Identity-skip drops a frame that *should* repaint (double-counts `repeatedPayloadFrames`) | T2.4 | `tst_outputdispatcher::identicalConsecutiveTicksSkipDuplicateSubmit` + existing `targetStatsTrackRepeatedPayloadsAndFailuresIndependently` |
| `repositionExact` arity/`avio_seek` regresses seek counts | T3.7 | `e2e_play_seekplay`, `e2e_play_stepscrub` |
| Atomic cut falls back to `repositionTo` (not actually atomic) or flashes | T3.10/T3.12 | `e2e_play_armedcut` (`placeholderFrames==0 && reposition==0`) |
| Cut boundary math off-by-one (overshoot under load) | T3.5/T3.6 | `tst_cutschedule` (`shouldFireAt` on `>=`, `playheadAfterCut` overshoot) |
| **SRT/UDP port collisions** between new E2E gates and the sync suite (23480-23489) and each other (farback vs armedcut both claim 23481) | T1.2, T2.5, T3.12 | CTest run-collision / hang; fix by assigning disjoint pairs (e.g. seekflash 23494, farback 23496, armedcut 23498) passed as positional arg 5 |
| New E2E steps run **zero** tests (false pass) by filtering `e2e_play_play1x`, which does not exist — the test is `e2e_play_storm` (scenario `play1x`) | T2.1, T2.3, T3.9, T3.10 | Use `-R e2e_play_storm`; verify CTest reports a non-zero test count |

## Definition of done

### Tier 1 (quick wins) — DONE when:
- [ ] `grep -rn clearAllBuffers playback/ tests/` returns nothing (renamed to `clearDecoderBuffers`).
- [ ] `ctest -R "tst_outputdispatcher_holdlast|tst_seekcoalescer"` passes (+ `tst_audioplayer_mutefade` if the fade is observable).
- [ ] `ctest -R "^tst_outputdispatcher$"` still passes (no held substitution in the populated-cache path).
- [ ] `ctest -R e2e_play_seekflash` passes with `placeholderFrames=0`.
- [ ] No regression: `ctest -R "e2e_play_storm|e2e_play_seekplay|e2e_play_stepscrub|e2e_play_reverse|e2e_play_sliderscrub|e2e_play_liveedge|e2e_play_4view|e2e_play_latency"` all green (play1x reposition==0; seekplay<=2; stepscrub<=10 & reuseSeek>=1; sliderscrub<=20; reverse<=150/<=4; liveedge<=3/<=3; latency resyncCount==0).

### Tier 2 (structural) — DONE when:
- [ ] `ctest -R "tst_commitgate|tst_sharedcacheslot|tst_outputframecache|tst_outputdispatcher"` all pass.
- [ ] Steady 1x playback still advances (CommitGate does not freeze video): `ctest -R "e2e_play_storm|e2e_play_seekplay"` green.
- [ ] `ctest -R e2e_play_farback` passes with `placeholderFrames=0` and `cacheGeneration>=1`.
- [ ] `ctest -R e2e_play_latency` green and (if the TSan leg runs) no data race on `m_outputCache` / `SharedCacheSlot`.
- [ ] Full no-regression sweep `ctest -R e2e_play_` (all Tier 1 gates + `seekflash` + `farback`) green.

### Tier 3 (frame-perfect playlist) — DONE when:
- [ ] `ctest -R "tst_frameindex|tst_replayplaylist|tst_cutschedule"` all pass.
- [ ] `ctest -L unit` fully green (pure-logic + all prior tiers).
- [ ] `ctest -R e2e_play_armedcut` passes with `placeholderFrames==0 && reposition==0`.
- [ ] `ctest -R "e2e_play_seekplay|e2e_play_stepscrub|e2e_play_storm"` green after `repositionExact` lands (seek counts not regressed; ideally seekplay reposition→0).
- [ ] Final sweep `ctest -R e2e_play_` (every play gate including `seekflash`, `farback`, `armedcut`) green.
