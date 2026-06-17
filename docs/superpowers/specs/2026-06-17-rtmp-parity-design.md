# Native RTMP ingest parity with native SRT — Design (PR A)

**Status:** approved (brainstorm 2026-06-17). Decisions: generalize the stats/health model (A1); include the A/V anchor-drift fix.
**Base branch:** `feat/rtmp-parity`, off `origin/main` (`6fe8d24`, the native-RTMP landing #48).
**Stacked context:** This is **PR A** of two. PR B (separate spec) removes the pure-ffmpeg ingest path, flips SRT to native-default, migrates the udp:// tests to a native SRT bridge, and adds a Windows Media Foundation AAC decoder. PR A is purely additive on the RTMP + shared-health paths and does **not** touch ffmpeg removal.

## Goal

Bring the native RTMP ingest path (`NativeRtmpIngestSession`, landed in #48) to feature parity with the native SRT path "as applicable" — i.e. matching every shared/SRT feature that is meaningful for an RTMP-over-TCP transport, and explicitly excluding SRT-protocol-only mechanisms (libsrt `srt_bstats` loss/retrans/drop counters, TSBPD, `SRTO_LATENCY`, linger) that have no TCP analog.

## Background — what RTMP already has vs. the real gaps

Most SRT "goodies" live in `StreamWorker`/`ReplayManager` (shared across all backends), so RTMP already inherits them: the reconnect/backoff loop, the per-scheme jitter window (RTMP gets the default 200 ms), the connection-status dot, per-source trim, the sample-accurate audio FIFO, the stall watchdog, and MPEG-2 encode + Matroska mux. The RTMP session already implements: connection-status (`setConnected`), a (mostly) shared A/V anchor from FLV tag timestamps, and H264 + HEVC(e-RTMP) + AAC decode through the shared `NativeVideoDecoder` / `AudioToolboxAacDecoder`.

After excluding the SRT-protocol-only bits, the genuine parity gaps are exactly three:

1. **Health/stats telemetry (the main work).** SRT samples `srt_bstats` ~1/s, fills `SrtStats{recvTotal,retransTotal,lossTotal,dropTotal}`, ships it via `IngestCallbacks::reportStats` through `StreamWorker::statsUpdated → ReplayManager::sourceStatsUpdated → UIManager::onSourceStatsUpdated`, which grades it with `srtHealth()` into a green/amber/red dot (`uimanager.cpp:1019-1050`). **RTMP reports nothing** — only an internal ack byte-counter (`m_receivedChunkBytes`). TCP hides packet loss, so RTMP cannot produce loss/retrans/drop; it must surface a *different* health metric (liveness, throughput, keyframe cadence, decode failures).
2. **A/V anchor drift.** RTMP keeps two mirrored anchor fields — video (`m_anchorStreamTimeMs`/`m_firstDtsMs`) and audio (`m_audioAnchorStreamTimeMs`/`m_firstAudioPtsMs`, `nativertmpingestsession.cpp:999-1054`). They cross-couple at first anchor (whichever stream arrives first sets the shared zero against `recordingClockMs()`), but each **re-anchors independently** on a discontinuity (`kForwardJumpMs=3000`, `kBackwardToleranceMs=-200`), so after an independent re-anchor they can diverge → a lip-sync offset. SRT solved this in AUD-4: a single shared anchor that **video owns** and **audio follows but never re-anchors** (`nativesrtingestsession.cpp:649-689`).
3. **Codec/profile + reconnect parity verification.** Both backends decode H264 + HEVC + AAC and use the shared backoff/reconnect loop. Expected to need **no code** — only confirming tests.

## Design

### Part 1 — Generalize the stats/health model (A1)

The shared stats pipe is renamed and widened from SRT-specific to backend-agnostic, keeping the SRT path byte-identical.

**`SrtStats` → `IngestStats`** (`ingestsession.h:52-57`). Keep the existing SRT counters (zero for non-SRT backends) and add generic liveness/throughput fields:
```cpp
struct IngestStats {
    // SRT (libsrt bstats) loss-domain counters — 0 for non-SRT backends.
    uint64_t recvTotal = 0;
    uint64_t retransTotal = 0;
    uint64_t lossTotal = 0;
    uint64_t dropTotal = 0;
    // Generic liveness/throughput — any backend may populate these.
    uint64_t bytesTotal = 0;       // cumulative media payload bytes received
    int64_t  lastPacketAgeMs = 0;  // ms since the last media packet, at sample time
    int64_t  keyframeAgeMs = 0;    // ms since the last video keyframe, at sample time
    uint64_t decodeFailures = 0;   // cumulative frames the native decoder rejected
};
```

**`SrtHealth` → `SourceHealth`** (`{Green, Amber, Red}`). The enum is renamed (backend-agnostic); the dot rendering is unchanged.

**Two graders, one enum.** `srtHealth(const IngestStats& prev, const IngestStats& cur, double amberRetransRate)` keeps its exact current logic (Red if `dDrop>0`, Amber if retrans-rate over threshold, else Green; negative deltas clamp Green) — only the type names change, so `tst_srt_health` stays green. A new pure grader is added:
```cpp
SourceHealth rtmpHealth(const IngestStats& prev, const IngestStats& cur);
```
RTMP grading (pure function of prev/cur snapshots; thresholds are named constants):
- **Red** — `cur.lastPacketAgeMs >= kRtmpRedStallMs` (default **3000**): the stream has stalled; OR `cur.decodeFailures > prev.decodeFailures` **and** `cur.bytesTotal == prev.bytesTotal` (decode failing with no fresh data → sustained failure).
- **Amber** — `cur.decodeFailures > prev.decodeFailures` (sporadic decode rejects); OR `cur.lastPacketAgeMs >= kRtmpAmberStallMs` (default **1000**); OR `cur.keyframeAgeMs >= kRtmpAmberKeyframeMs` (default **5000**).
- **Green** — otherwise (fresh packets, decoding, recent keyframe). A counter reset (cur < prev) clamps Green, mirroring `srtHealth`.

**Callback + signal renames** (mechanical): `IngestCallbacks::reportStats(const IngestStats&)`; `StreamWorker::statsUpdated(int, const IngestStats&)`; `ReplayManager::sourceStatsUpdated(int, const IngestStats&)`.

**UIManager dispatch.** `onSourceStatsUpdated` selects the grader by the source's URL scheme (already known per source): `rtmp`/`rtmps` → `rtmpHealth(prev,cur)`, else `srtHealth(prev,cur,amber)`. The UI accessors `sourceSrtHealth`/`sourceHasSrtStats` are renamed to `sourceHealth`/`sourceHasStats` (and the QML binding + any tooltip text updated); the per-source `prev`/`cur` snapshot bookkeeping (`uimanager.cpp:1019-1033`) is reused unchanged. The tooltip shows the populated fields: SRT → recv/retrans/loss/drop; RTMP → received bitrate (derived from `bytesTotal` delta / interval), keyframe age, decode failures.

### Part 2 — RTMP session emits `IngestStats`

`NativeRtmpIngestSession` samples once per ~1 s inside its read loop (mirroring the SRT cadence at `nativesrtingestsession.cpp:185-202`) and calls `m_callbacks.reportStats(stats)`:
- `bytesTotal` — a cumulative media-payload byte counter (extend the existing `m_receivedChunkBytes`, or a dedicated media-bytes counter so acks and stats don't alias).
- `lastPacketAgeMs` — `now - m_lastPacketAtMs` (the field already drives the stall watchdog).
- `keyframeAgeMs` — `now - m_lastKeyframeAtMs`, a **new** field stamped whenever a video keyframe FLV tag is parsed.
- `decodeFailures` — a **new** cumulative counter incremented whenever a video access unit fails to decode (the `DecodeCapability`/decode-error path).
- SRT loss fields stay 0.
`now` is a monotonic millisecond clock consistent with `m_lastPacketAtMs` (the same source the stall watchdog uses). Sampling is driven off the read loop with a ~1 s gate so it costs nothing on a healthy stream.

### Part 3 — RTMP A/V anchor: single shared anchor (audio follows video)

Replace the two independently-re-anchoring fields with the SRT/AUD-4 model:
- **One anchor** — `m_anchorStreamTimeMs` ↔ `m_anchorMediaMs` (the source-timestamp zero). Whichever of audio/video arrives first establishes it against `recordingClockMs()`.
- **Video owns re-anchoring.** On a forward jump ≥ `kForwardJumpMs` or a backward move past `kBackwardToleranceMs`, video re-establishes the shared anchor (as today, but writing the single shared pair).
- **Audio follows, never re-anchors.** `sourcePtsMsForAudio` maps against the current shared anchor; on a large jump it **flushes the AAC decoder** (matching `nativesrtingestsession.cpp:665-673`) but does **not** move the anchor. This removes the divergence path.

This is a targeted rewrite of `sourcePtsMsForVideo`/`sourcePtsMsForAudio` (`nativertmpingestsession.cpp:999-1054`) to share one anchor; the existing first-arrival cross-coupling is preserved, only the independent audio re-anchor is removed.

### Out of scope (PR B or later)
- Removing `FfmpegIngestSession`; flipping SRT to native-default; migrating udp:// tests; the Windows MF AAC decoder; deleting the ffmpeg-SRT duplicate gates and rewriting `tst_ingestbackendselector`.
- Synthesizing RTMP loss metrics TCP can't provide (retransmit/drop) — intentionally excluded ("as applicable").

## Error handling / edge cases
- **No video yet** (audio-only warm-up): `keyframeAgeMs` reports a large value → at worst Amber via the keyframe rule; acceptable (a live video source should produce a keyframe within seconds). Guard so an as-yet-unset `m_lastKeyframeAtMs` doesn't read as a multi-day age — treat "no keyframe seen" as keyframeAge = time-since-connect.
- **Counter reset / reconnect**: a fresh session resets counters; `rtmpHealth` clamps Green when `cur < prev` (as `srtHealth` does), and UIManager already re-baselines on reset (`uimanager.cpp:1026`).
- **Backend-agnostic struct, partial fill**: SRT leaves the generic fields 0 and RTMP leaves the SRT fields 0; each grader only reads the fields its backend populates, so a 0 in an unused field never mis-grades.
- **Anchor before first keyframe**: video may receive non-IDR frames before the first keyframe; anchoring keys off DTS as today and is independent of keyframe tracking (which only feeds health).

## Testing

1. **Unit — `rtmpHealth` grader** (`tests/unit/tst_rtmp_health.cpp`, or extend `tst_srt_health.cpp`): Green (fresh, decoding, recent keyframe); Amber (decode failure bump; elevated `lastPacketAgeMs`; stale keyframe); Red (stall past `kRtmpRedStallMs`; sustained decode failure with no fresh bytes); counter-reset clamps Green. Mirrors `tst_srt_health`.
2. **Unit — `srtHealth` unchanged**: `tst_srt_health` keeps passing verbatim against `IngestStats`/`SourceHealth` (proves the rename is behavior-preserving).
3. **E2e — `e2e_native_rtmp_ui_stats`** (`native-rtmp` label, mirrors `e2e_native_srt_ui_stats`): drive the Python RTMP fixture, run `sync_harness --report-stats`, assert the engine→UI health path reports a non-empty RTMP `IngestStats` (bytes advancing, a Green health) — proving the data path end-to-end.
4. **E2e — anchor/lip-sync**: if the RTMP flash/beep fixture supports it, an A/V-offset assertion after the anchor change (reuse the lipsync harness pattern); otherwise rely on the existing `native-rtmp` reconnect/smoke gates staying green to prove no regression from the anchor rewrite.
5. **Regression**: `ctest -L native-rtmp`, `-L native-apple-ingest`, and `-L unit` all green; the SRT health/stats path is untouched behaviorally.

## Files touched
- `recorder_engine/ingest/ingestsession.h` — `SrtStats`→`IngestStats` (+ generic fields), `SrtHealth`→`SourceHealth`, `reportStats(IngestStats)`, add `rtmpHealth()` decl + `kRtmp*` constants.
- `recorder_engine/ingest/ingestsession.cpp` — rename in `srtHealth`; implement `rtmpHealth`.
- `recorder_engine/ingest/nativesrtingestsession.cpp` — fill `IngestStats` (renamed fields; SRT counters unchanged).
- `recorder_engine/ingest/nativertmpingestsession.{h,cpp}` — emit `IngestStats` (bytes/last-packet-age/keyframe-age/decode-failures, +`m_lastKeyframeAtMs`/`m_decodeFailures`); single shared A/V anchor.
- `recorder_engine/streamworker.{h,cpp}`, `recorder_engine/replaymanager.{h,cpp}` — `statsUpdated`/`sourceStatsUpdated` signal type rename.
- `uimanager.{h,cpp}` + `Main.qml` — grader dispatch by scheme; `sourceSrtHealth`/`sourceHasSrtStats` → `sourceHealth`/`sourceHasStats`; tooltip.
- `tests/unit/tst_rtmp_health.cpp` (new) + `tests/unit/CMakeLists.txt`; `tst_srt_health.cpp` / `tst_srt_options.cpp` type renames if they reference `SrtStats`.
- `tests/e2e/CMakeLists.txt` + `tests/e2e/run_rtmp_ui_stats.sh` (new, modeled on `run_srt_ui_stats.sh`).

## Success criteria
- An RTMP source shows a green/amber/red health dot driven by RTMP-meaningful signals, through the same UI pipe as SRT; `e2e_native_rtmp_ui_stats` proves the engine→UI path.
- The SRT health/stats path is behavior-identical (rename only); `tst_srt_health` + the SRT e2e gates stay green.
- RTMP A/V uses a single shared anchor (audio follows video, never re-anchors), removing the independent-re-anchor drift; native-rtmp gates stay green.
- No ffmpeg-removal, SRT-default, or Windows-AAC changes leak into PR A.
