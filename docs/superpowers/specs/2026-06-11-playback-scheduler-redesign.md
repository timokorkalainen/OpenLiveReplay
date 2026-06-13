# Playback Scheduler Redesign — Design Spec **v3**

**Date:** 2026-06-11
**Status:** v3 — two adversarial review rounds applied. v1 (42 objections/18 blockers) → v2 (closed 33/38) → v3 (closes the 3 new blockers + 8 majors the v2 machinery introduced). Core is stable across both rounds; remaining residue is test-gated empirical tuning, not open design questions.
**Scope decision:** Option B (full scheduler redesign incl. smooth reverse) · decode **all tracks always**

> **Review provenance.** The windowed-scheduler *core* (bounded window, largest-PTS-≤-P delivery, intra-only exploitation) was never successfully attacked. What both rounds attacked was *edge machinery*. v3's changes vs v2: corrected the **cap arithmetic** (was smaller than the trim window — eviction spin); fixed the **jump-vs-lag classification** (forward overrun must skip-forward, not reposition); made **reverse chunks fill-then-deliver** (top-of-chunk first, terminate by file position); **sized the audio queue** to the real decode span and made **backward** reuse-seeks re-prime audio instead of silently re-releasing; **disambiguated PTS symbols**; made **reuse require an actual frame at the target** (not just range coverage); and moved the **audio acceptance measurement to worker-side push telemetry** so AudioPlayer stays untouched (resolves the §10/§11 contradiction).

---

## 1. Goal

Replace `PlaybackWorker`'s decode loop with a **windowed, demand-driven, bidirectional frame scheduler** so playback at any speed (0.25×–5× forward, −5× reverse), scrubbing, frame-stepping, and live-edge chase are smooth, bounded, and A/V-synced — eliminating the runtime-confirmed self-sustaining hard-seek storm and its companion defects.

## 2. Background (summary; full evidence in the audit + review artifacts)

Current loop reads as fast as the file allows, `msleep(5)` only *after* a read when decode is >100 ms ahead, and **hard-seeks** (flush all decoders + clear audio ring + wipe buffers + burst-decode `fps` frames/track) whenever `|master − lastProcessedPtsMs| > 500 ms`. Runtime-measured: 1× playback with ≤2 video tracks **locks into ~90 hard-seeks/s** within ~7 s; ≥3 tracks stay stable; seek-then-play / reverse / frame-step all trigger it on demand.

**Enabling fact:** recordings are MPEG-2 **intra-only** (`gop_size = 1`), so any frame decodes independently after a seek. Files are MKV, `live=1`, **no Cues** (verified — matroska muxer omits Cues in live mode even after the trailer), **non-interleaved** (`av_write_frame`, per-stream DTS bumped +1 ms on collision; disrupted-source spans are backfilled late in file order), one PCM-S16 audio + one subtitle track per video track, 30 fps typical.

## 3. Constants & symbols

Tunable in one header block (`playbackworker.h`). `frameDurMs = 1000 / fps`, **`fps = m_transport->fps()`** sampled per iteration (§7).

| Constant | Value | Meaning |
|---|---|---|
| `kLeadMs` | 500 | video window kept ahead of the playhead (travel dir) |
| `kTrailMs` | 300 | video window kept behind the playhead |
| `kChunkMs` | 500 | reverse backward-fetch chunk size |
| `kAudioLeadMs` | 200 | max lead of **pushed** audio over `P` (`< kRingCapMs(500) − kSinkBufferMs(60) − margin`) |
| `kAudioQueueMs` | 900 | worker audio-queue span bound (`≥ kLeadMs + kSlackMs + margin`; must hold everything between `P` and the video decode edge) |
| `kSlackMs` | 200 | trim hysteresis beyond the active window |
| `kFillBatch` | `4 × trackCount` | max packets read+decoded per **iteration** (one pass/iteration) |
| `kReverseChunkBudget` | `ceil(kChunkMs/frameDurMs) × trackCount × 2` | hard packet cap for a single reverse-chunk fill |
| `kIdleSleepMs` | 3 | sleep when window full and playing |
| `kEofSleepMs` | 10 | sleep between EOF re-checks |
| `kReadErrSleepMs` | 20 | sleep after a non-EOF read error |
| `kBackJumpSlackMs` | 150 | `P` *below* the buffered span by this ⇒ reposition |
| `kMaxLagMs` | `kLeadMs` | forward decode lag beyond this ⇒ chunked skip-forward |
| `kDecimateAbove` | 1.5 | `|speed|` above which decode-decimation engages |
| `kGlobalFrameBudget` | 256 | aggregate decoded-frame cap across all tracks (memory bound) |

**Symbols (defined once):** `refNewestPts` / `refOldestPts` = newest/oldest buffered PTS of the **reference (first) video track**. `newestPtsMin` / `oldestPtsMax` = min-newest / max-oldest **across video tracks, excluding tracks whose newest PTS lags the cross-track max by > `kLeadMs`** (a "stalled, will-backfill" track per the non-interleaved muxer). `newestPtsMax` / `oldestPtsMin` = cross-track max-newest / min-oldest (all tracks). Jump/coverage predicates use the min/max-with-staleness forms; the EOF un-latch seek target uses `refNewestPts`.

**Per-track frame cap (memory):** the trim window (§6.6) spans at most `kLeadMs + kChunkMs + kTrailMs + 2·kSlackMs` ms. `capFrames = clamp( ceil((kLeadMs + kChunkMs + kTrailMs + 2·kSlackMs)/frameDurMs) + 4, 12, max(12, kGlobalFrameBudget / trackCount) )`. By construction `capFrames ≥ trim-window occupancy + 4` **except** when the global budget binds (≥8-view extreme), where the *window* shrinks (logged) but never below 12 frames/track. This guarantees the cap never evicts a frame the active window must keep (the v2 cap-below-window bug). At 30 fps single-track ≈ 56 frames (~175 MB at 1080p YUV420); at 16 views the budget caps to 16 frames/track (~800 MB aggregate). Allocation failure drops the insert, increments `framesDropped`, never crashes.

## 4. Core model — the buffer window

Playhead `P = transport.currentPos()`; `speed = transport.speed()`; **travel direction** `dir`:
- playing: `dir = sign(speed)`;
- paused: `dir = lastMoveDir` — the sign of the most recent position delta from an explicit step/jog **or slider seek** (`sign(target − P_before)`); default `+1`.

Per video track a **`TrackBuffer`** holds decoded frames sorted ascending by PTS, unique by PTS. The scheduler keeps it filled **in the travel direction**: forward (`+1`) cover `[P − kTrailMs, P + kLeadMs]`; reverse (`−1`) cover `[P − kLeadMs, P + kTrailMs]`.

**Bounded-decode invariant:** each iteration performs at most one `kFillBatch`-bounded read/decode pass (§11 perf-7); fill stops when the just-read packet PTS crosses the window edge **or** the cap is reached at the fill edge. Decode is bounded to ≤ `kLeadMs (+ one batch)` from `P` by construction; the `|master − decoded| > 500 ms` drift trigger is **deleted**, replaced by §6.1 classification.

## 5. Delivery (direction-aware)

Display frame at `P` = **buffered frame with the largest PTS ≤ `P`**. Direction constrains dedup (`DecoderTrack.lastDeliveredPtsMs`) to forbid out-of-order paints:
- **Forward** (`+1`): deliver if `pts > lastDeliveredPtsMs` or after a reposition reset.
- **Reverse** (`−1`): deliver if `pts < lastDeliveredPtsMs` or after reset.
- `P` exactly on a buffered PTS ⇒ that frame is delivered.
- **Reset on reposition / reuse-seek:** `lastDeliveredPtsMs = −1` on every track, then one `deliverDueFrames(P)` so the target paints immediately.

Delivery runs every iteration (between fill batches). At `|speed|>1` the playhead crosses multiple frames per tick; the displayed frame is the newest decoded frame ≤ P (decimation §6.3 keeps the displayable subset — see the reuse-coverage rule §6.2 so the exact frame is present when speed returns to ≤1).

## 6. Scheduler loop

### 6.1 Classify (priority order)
Sample `P`, `playing`, `speed`, `dir`, `fps`. **Forward overrun is lag, not jump** (the v2 fix): a *jump* is a discontinuity that leaves the buffer irrelevant; forward decode falling behind is handled by skip-forward, which preserves the trail.
1. **Explicit seek** (`m_seekTargetMs ≥ 0`): coalesce to the latest target, clear it. → §6.2.
2. **Backward jump:** `P < oldestPtsMin − kBackJumpSlackMs` (the playhead fell below everything buffered — a backward scrub/jog/yank past the trail). → §6.2 with `target = P`, reverse anchor.
3. **Forward lag / overrun:** playing, `dir = +1`, `newestPtsMin < P − kMaxLagMs` — decode (or the playhead at high speed / past the live tail) is ahead of what's buffered. → §6.5 skip-forward **or** §6.8 tail-hold (if at the written tail). **Never** a reposition.
4. Otherwise → deliver (§5), fill (§6.3 / §6.4), trim (§6.6), audio (§6.7), wait (§6.9).

### 6.2 Reposition (trail-covering)
- **Reuse fast-path:** if **every** video track has an actual decoded frame within `frameDurMs/2` of `target` (not mere range coverage — guards against a decimated/sparse buffer after high-speed play), do **not** seek/flush: reset dedup, `deliverDueFrames(target)`, and (§6.7) re-prime audio if the move was backward. Frame-step / small in-window scrub land here.
- **Reposition:** clear all `TrackBuffer`s + audio queue + ring; anchor by intent — forward/paused-forward `seekAnchor = max(0, target − kTrailMs)`; backward `seekAnchor = max(0, target − kLeadMs)`; `av_seek_frame(BACKWARD)` to `seekAnchor`; decode forward (no `avcodec_flush` — intra-only) through `target + frameDurMs`, inserting all tracks; reset dedup; `deliverDueFrames(target)`. Guarantees ≥ `kTrailMs` trail (forward) / ≥ `kLeadMs` (backward) below the target so subsequent steps reuse.

### 6.3 Forward fill (`dir = +1`)
While `newestPtsMin < P + kLeadMs`, not EOF, within `kFillBatch`: `av_read_frame`. For video packets, when `|speed| > kDecimateAbove`, **count-based decimation** — keep every `⌈|speed|⌉`-th video frame *per track* (a running per-track keep-counter; not a PTS lattice, which the DTS-bumped on-disk PTS don't lie on), dropping the rest before `avcodec_send_packet`; else decode + `convertToQVideoFrame` + insert. Termination is by the just-read packet PTS crossing `P + kLeadMs + kSlackMs` **or** the batch budget **or** the cap reached at the fill edge — never by one track's extent (a ref-track file-order gap can't spin the loop; `newestPtsMin` excludes stalled tracks per §3).

### 6.4 Reverse fill (`dir = −1`) — fill-then-deliver
If `refOldestPts > P − kLeadMs` and `P > 0`, fetch the next lower chunk **atomically** (the v2 bottom-up-stall fix): record the avio position at the current oldest; `av_seek_frame(BACKWARD)` to `max(0, P − kLeadMs − kChunkMs)`; decode forward (with §6.3 decimation) **without delivering partial-chunk frames**, until the read cursor reaches the recorded avio position (file-position terminator — well-defined under non-interleaved skew, unlike per-track PTS) or `kReverseChunkBudget` is hit. The chunk is small (~`kChunkMs/frameDurMs` ≈ 15 frames/track); a fill may span ≤2 iterations, during which the **last delivered frame is held** (no out-of-order paint). Once the chunk's top frame (just below the previous oldest) is present, reverse delivery (§5, `pts < lastDeliveredPtsMs`) proceeds top-down. Abort + reclassify if `m_seekTargetMs ≥ 0`. Bounded to ≈ `|speed| × 1000/kChunkMs` seeks/s (≈10/s at −5×).

### 6.5 Forward lag / skip-forward (the forward-overrun handler)
When `dir = +1` decode can't hold speed, or the playhead has run forward past the buffer but still within written data: `av_seek_frame(BACKWARD)` to `max(0, P − kTrailMs)`, keep audio muted (we are at `|speed|>1` or past 1× tail), resume decimated forward fill. **No positive feedback** — decode never gets ahead, so no storm. Bounded rate (`skipForward` counter, ≤ a few/s). If `P` is past the written tail → §6.8 instead.

### 6.6 Trim (direction-aware) + cap
- forward: keep `[P − (kTrailMs + kSlackMs), P + (kLeadMs + kSlackMs)]`;
- reverse: keep `[P − (kLeadMs + kChunkMs + kSlackMs), P + (kTrailMs + kSlackMs)]`.
Then enforce `capFrames` (§3, ≥ this window by construction) by evicting the **farthest-from-`P`** frame, **never** one inside `[P, fill-edge]` in the travel direction. Trim runs between fill batches.

### 6.7 Audio (decoupled; AudioPlayer untouched)
- Forward decode of the active view's audio is **queued** in a worker-side PTS-tagged `AudioFrameQueue` bounded to `kAudioQueueMs` (≥ the `P`→decode-edge span, so nothing needed is dropped).
- Each iteration, at **1× forward, playing, single active view, unmuted**, release queued frames with `pts ≤ P + kAudioLeadMs` to `m_audioPlayer->pushSamples(samples, bytes, framePts, P_now)` — passing the **current `P` at release** as masterTime (not decode-time P), so AudioPlayer's alignment positions correctly. Ring stays ≤ `kAudioLeadMs + sink` < cap: no front-drop.
- **Backward** reuse-seek or any reposition, or `setActiveAudioView`: **clear** the audio queue + ring (`audioClear`), then re-prime from forward decode over the next ≤`kAudioLeadMs` (a brief audio gap, identical to today's clear-on-seek/switch). A backward in-window reuse-seek is thus an audio reposition — never a silent re-release (which AudioPlayer's overlap-trim would swallow, the v2 desync bug). Forward in-window reuse continues releasing normally.
- `|speed| ≠ 1` / reverse / multiview: audio muted (today's gate); queue discarded; on return to 1× treat as a clear+re-prime (no stale-cursor fight).

### 6.8 Tail / EOF / live growth
- **Tail-hold (no overrun storm):** if `P > newestPtsMax` and no un-read growth is pending, **hold** the last delivered frame and poll growth — do not reposition forward into unwritten space (subsumes the v2 tail-jump race: §6.1 forward overrun routes here, never to §6.2). The transport keeps its own clock; the worker shows the last real frame until data arrives.
- **Un-latch only on growth:** on `AVERROR_EOF`, `msleep(kEofSleepMs)`; compare `avio_size(pb)` to the size at the previous EOF. **Only if grown:** `pb->eof_reached = 0; pb->error = 0; avformat_flush`; then `av_seek_frame(BACKWARD)` to `refNewestPts` (clears matroska's latched `done`); **check the return** — on failure don't advance, retry next poll. If not grown: just sleep (finished file costs only the sleep; no loop).
- **Dedup-before-decode after un-latch:** discard read packets with `pts ≤ the owning track's newestPts` before `avcodec_send_packet` (re-read tail clusters cost reads only — no re-decode, no duplicate audio).
- **Non-EOF read error:** `msleep(kReadErrSleepMs)`, bounded retry; after the bound stop the worker cleanly.

### 6.9 Wait / pause
- Window full + playing: `msleep(kIdleSleepMs)`.
- Paused: block, but wake when `m_seekTargetMs ≥ 0` **or** `P` left the current delivered frame's interval; on wake run classify→deliver before re-blocking.

## 7. Integration / call-site changes (`uimanager.cpp` in scope)
- **`UIManager::seekPlayback(ms)`** (slider `onMoved`) must call `m_playbackWorker->seekTo(ms)` in addition to `m_transport->seek(ms)` (and record `lastMoveDir = sign(ms − prevPos)` for §4). The deleted drift trigger was its only — playing-only — safety net.
- **`stepFrame`/`stepFrameBack`/MIDI jog**: keep `seekTo`; reuse fast-path (§6.2) makes in-window steps 0-seek; drop the redundant UI-thread `deliverBufferedFrameAtOrBefore` (or make it a no-op while a seek is pending).
- **Reposition coalescing:** rapid `seekTo` processes only the latest target (§6.1).
- **`setFrameBufferMax` removed**; its three call sites (`startRecording`, `restartPlaybackWorker`, `setRecordFps`) updated; window math derives `fps` from `m_transport->fps()`.
- **Follow-live yank** (`onRecorderPulse`): classified by §6 — small yanks reuse (0-seek); only a backward yank past `kBackJumpSlackMs` repositions; a forward yank past the tail holds (§6.8).

## 8. Degenerate / startup states
1. **Recording start / file shorter than window / empty buffer:** §6.1 jump must not fire when the reference buffer is empty; `P > newestPtsMax` on a short/growing file is §6.8 hold, not a jump. Window edges clamp to available duration.
2. **Seek into not-yet-written region:** §6.8 hold; don't loop.
3. **`P` near 0:** reverse fill and anchors clamp at 0; transport already clamps `P` to 0 and auto-pauses.
4. **First open with `<1 s` of data + follow-live:** ≤1 initial reposition, then 0.

## 9. Audio contract restated
Audio plays only at **1× forward, single active view, unmuted** (unchanged user behavior). The change is purely *when* decoded audio reaches AudioPlayer: paced to `P + kAudioLeadMs` from the worker queue, instead of dumped at decode time. AudioPlayer's ring/alignment/fade is untouched (§10) — this PR keeps audio *within* the existing 500 ms cap rather than changing it.

## 10. Non-goals
- AudioPlayer internals (ring cap, sink-latency model, fade, alignment) — separate audio PR. **One exception for testability:** a read-only push-telemetry hook is permitted (see §11.4) — it does not alter playback behavior.
- Decode-only-visible-track optimization (user chose decode-all-tracks); the multi-track ceiling is mitigated by decimation, not removed.
- Recording-side, capture-anchor, lifecycle findings — separate PRs.

## 11. Verification

### 11.1 Deliverables (land in this PR)
- **`TrackBuffer`** (windowed per-track store, ffmpeg-free, unit-reasoned).
- **`AudioFrameQueue`** (worker-side PTS pacing queue).
- **Counters** on `PlaybackWorker`, via accessors + `SEC` stderr lines behind `OLR_PB_TELEMETRY`: `reposition`, `reuseSeek`, `reverseChunkSeek`, `eofTailSeek`, `skipForward`, `framesDelivered[track]`, `framesDropped`, and per-delivery `(track, pts, wallMs)` and **per-audio-push `(framePts, P_atRelease, wallMs)`** records. `audioClear` counter on `AudioPlayer` (the only `AudioPlayer` change besides the optional read-only push-record hook).
- **`tests/e2e/play_harness.cpp` + `run_playback_e2e.sh`** promoted into the repo; built in CI against checked-in code.

### 11.2 Counter thresholds
"Steady play" = the window after ≤3 s warm-up; ≥15 s recorded per scenario.
- **Repositions == 0** for the whole steady window of every **forward ≤2× cell** (the storm tripwire — flips 2-view 1× from ~90 seeks/s to 0).
- **`reverseChunkSeek/s ≤ 1.5 × |speed| × 1000/kChunkMs`.**
- **Skip-forward cells** (e.g. 4-view 5×): `skipForward ≤ 4/s`, repositions still 0 — asserts **graceful degradation**, not frame-perfect smoothness.
- **`eofTailSeek`** counted separately, excluded from repositions; on a **finished** file played to end, `eofTailSeek == 0` after the last growth.
- **Reads/s ≤ 2× the file's realtime packet rate** in steady play.

### 11.3 Delivery & smoothness (per cell)
Per track: delivered PTS **strictly monotonic in the travel direction**; max inter-delivery wall gap `≤ max(2 × frameDurMs / |speed|, 2 × transportTickMs)`; distinct-frame rate `≈ min(fps × |speed|, 1000/transportTickMs) ± 20%`.

### 11.4 Audio (forward 1×) — measured via worker push telemetry (no AudioPlayer accessor needed)
Run the active view **unmuted**; assert on the worker's per-audio-push records (§11.1), so no AudioPlayer occupancy accessor or null sink is required (resolves the §10 scope contradiction): `audioClear == 0` after warm-up in steady 1× play; pushed-frame PTS continuous within `kJitterTolMs`; **`framePts − P_atRelease ≤ kAudioLeadMs`** for every push (proves the ring can't exceed `kAudioLeadMs + sink < cap`); and on a **backward** in-window scrub, audio re-primes (an `audioClear` + resumed pushes whose PTS follow `P` down) rather than going silent. A real default device is used if present; assertions are on push records, independent of the device.

### 11.5 Scenario matrix & fixtures
- **Speed×views:** `{0.25, 0.5, 1, 2, 5, −5} × {1, 2, 4}`. **Fixture duration ≥ `startOffset + |speed| × observationWindow + 10 s`** (≥150 s for 5× cells, recorded by `record_harness` with `OLR_VIEWS`); reverse cells start high and end before the transport clamps to 0.
- **`sliderscrub`** (transport-only path → after §7, the `seekPlayback` path): drive in **playing and paused** states; assert the delivered frame reaches the new position within `2 × frameDurMs`, in-window scrub does 0 repositions (paused included), and a backward scrub re-primes audio.
- **`stepscrub`** (back-step reuse): 20 back-steps × `frameDurMs`; assert `repositions ≤ ceil(20 × frameDurMs / kTrailMs) + 1` (≈4) and `audioClear` only on the first reposition.
- **`speedflip`** (decimation→1× coverage): play at 5× then drop to 1× and step; assert the displayed frame is the exact target (not a stale decimated frame) — i.e. the reuse-coverage rule (§6.2) forces a reposition when the precise frame is absent.
- **`growfile`** (live-EOF — the only valid live test): run `record_harness` (or an ffmpeg writer with the muxer's exact `live=1`/`reserve_index_space` options) **concurrently**; play the in-progress `.mkv`, replicate the follow-live yank; writer logs per-flush wall-times, player logs per-delivery wall-times; assert `max(delivery_wall − flush_wall) ≤ frameDurMs + flushCadence`, no multi-hundred-ms stall, ≥60 s growth.
- **`skewed`** (non-interleave stress): a recording with a mid-session source dropout (ref-track file-order gap + late backfill); assert healthy tracks never stall or drop in-window frames and reads stay bounded.
- **Wall-time budget:** each cell ≤ ~28 s; full suite ≤ ~12 min, CI-runnable.

### 11.6 Headline acceptance gate
The **2-view 1× cell flips from "~90 seeks/s storm" to "0 repositions, ~60 delivers/s, `audioClear == 0`, every push `framePts − P ≤ kAudioLeadMs`"** — the single must-pass result proving the redesign.
