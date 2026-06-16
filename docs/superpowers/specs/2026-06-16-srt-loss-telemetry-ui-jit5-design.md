# JIT-5: SRT loss telemetry on the connection-status UI — Design

**Status:** approved (brainstorm 2026-06-16)
**Roadmap item:** framesync P0 / JIT-5 (see `broadcast-framesync-roadmap` memory)
**Base branch:** `feat/srt-jit5-loss-ui`, branched off `feat/srt-loss-2cb` (PR #40 — the
`srt_bstats` sampling this feature consumes). Rebase onto `main` once #40 merges.

## Goal

Surface per-source SRT link health — packet loss, ARQ retransmission, and finally-
unrecovered drops — on the existing per-source connection-status indicator (feature #24),
so an operator can see at a glance which camera's SRT link is healthy, stressed, or
losing content, and read exact counters on hover.

## Background

Phase 2c-b made the native SRT ingest (`NativeSrtIngestSession`, `OLR_NATIVE_SRT=1`) sample
`srt_bstats` roughly once per second and log the cumulative counters on stop
(`pktRcvRetrans` / `pktRcvLossTotal` / `pktRcvDropTotal` / `pktRecvTotal`). Today those
numbers go **to the log only** — nothing exposes them to the UI. The connection-status UI
already shows a per-source dot (grey / green / red) driven by a clean queued-signal path:
`StreamWorker::connectionChanged` → `ReplayManager::sourceConnectionChanged` → `UIManager`
(version property) → QML. JIT-5 adds a parallel periodic-telemetry path of the same shape.

Key SRT-stats semantics (from Phase 2c-b, see `srt-loss-recovery-and-stats` memory):
- `pktRcvDropTotal` = too-late-to-play, **finally unrecovered** loss. Delta > 0 ⇒ content lost.
- `pktRcvRetrans` (received retransmissions) > 0 is **normal and healthy** on any lossy link —
  ARQ routinely recovers everything; it is NOT an error condition by itself.
- `pktRcvLossTotal` = detected loss (retransmitted), expectedly non-zero under any loss.

This is why "amber on any retransmit" would light up almost permanently on real links; amber
is therefore a **rate threshold**, not a presence test.

## Scope

- **In scope:** native-SRT sources only. The whole feature is native-SRT-only *by
  construction* — only `NativeSrtIngestSession` invokes the new stats callback, so no source
  is ever asked "are you SRT?" Non-SRT sources (RTMP / UDP / RTSP / ffmpeg-SRT) simply never
  produce stats and keep the existing binary green/red dot.
- **Out of scope (YAGNI):** stats for the ffmpeg ingest (FFmpeg abstracts the SRT socket — no
  bstats access); historical sparklines / per-second history; persisting stats; a 4th dot
  color; surfacing stats anywhere other than the existing per-source dot + its tooltip.

## Architecture & data flow

A periodic-telemetry path mirroring the connection-status path exactly:

```
NativeSrtIngestSession::run()            // already samples srt_bstats ~1/sec (PR #40)
  └─ callbacks.reportStats(SrtStats)     // NEW callback in IngestCallbacks, sibling to setConnected
       └─ StreamWorker  → emit statsUpdated(int sourceIndex, SrtStats)   // queued (capture → main)
            └─ ReplayManager → emit sourceStatsUpdated(int sourceIndex, SrtStats)  // queued → UIManager
                 └─ UIManager::onSourceStatsUpdated     // store snapshot, compute color, bump version
                      └─ QML re-binds via sourceStatsVersion + Q_INVOKABLE getters
```

### Shared data type

In `recorder_engine/ingest/ingestsession.h` (the shared ingest interface), next to
`IngestCallbacks`:

```cpp
struct SrtStats {
    qint64 recvTotal    = 0;   // pktRecvTotal      (cumulative)
    qint64 retransTotal = 0;   // pktRcvRetrans     (cumulative, received retransmissions)
    qint64 lossTotal    = 0;   // pktRcvLossTotal   (cumulative, detected loss)
    qint64 dropTotal    = 0;   // pktRcvDropTotal   (cumulative, finally unrecovered)
};
```

`qRegisterMetaType<SrtStats>("SrtStats")` is called once (engine init) so the struct can ride
Qt queued signals across threads. The struct is copied through each signal — no shared mutable
state, no locking.

### Callback

`IngestCallbacks` gains `reportStats` following the **same function-pointer style** as the
existing `setConnected`. The native ingest invokes it once per `srt_bstats` sample with the
current cumulative snapshot. The legacy ffmpeg ingest never sets/calls it.

## Color computation (windowed, live)

`UIManager` keeps the previous snapshot per `sourceIndex`. The color decision is a **pure free
function** (no Qt/UI dependency, so it is unit-testable in isolation):

```cpp
// recorder_engine/srt_health.h  (new, tiny, header-only or .cpp)
enum class SrtHealth { NA = 0, Green = 1, Amber = 2, Red = 3 };

SrtHealth srtHealth(const SrtStats& prev, const SrtStats& cur, double amberRetransRate);
```

Logic (per ~1 s window between snapshots):

```
dDrop    = cur.dropTotal    - prev.dropTotal
dRetrans = cur.retransTotal - prev.retransTotal
dRecv    = cur.recvTotal    - prev.recvTotal
if dDrop > 0:                                          Red     // unrecovered loss this window
elif dRecv > 0 && (double)dRetrans/dRecv > amberRate: Amber   // link stressed
else:                                                 Green   // healthy (incl. ARQ recovering)
```

- `amberRetransRate` default **0.02** (2 % of received packets retransmitted in the window),
  overridable via env `OLR_SRT_HEALTH_AMBER_PCT` (read once, `OLR_SRT_*` convention). A value
  ≤ 0 or unset → the 0.02 default.
- The "window" is simply the interval between the two most recent snapshots (~1 s). The **first
  snapshot for a source establishes the baseline**: it is stored as `prev` and rendered Green (no
  delta is computed against a zero struct, so a source that connected with already-high cumulative
  counters does not spuriously flash Amber/Red). Health is computed from the second snapshot on.
- Health is **live**: it reflects only the most recent window and returns to Green when the
  link goes quiet. Cumulative totals are preserved for the tooltip.

## UI changes (`Main.qml` ~1355–1370)

### Dot color precedence

The existing `connDot` keeps grey/green/red, now refined when SRT health is available:

1. **not recording** → grey (`#555`) — unchanged.
2. **recording & disconnected** → red (`#d32f2f`) — unchanged, takes priority over stats.
3. **recording & connected:**
   - if `sourceHasSrtStats(index)` → map `sourceLinkHealth(index)`: Green `#2e7d32`,
     Amber `#f9a825`, Red `#d32f2f`.
   - else (non-SRT) → green `#2e7d32` (existing connected color).

"Connected but dropping" reuses the **same red** as "disconnected" (decision: a 4th color is
over-engineering); the tooltip disambiguates the two.

### Hover tooltip

A `HoverHandler` + QtQuick.Controls `ToolTip` on the dot (or its row), shown only when
`sourceHasSrtStats(index)`, rendering `sourceStatsTooltip(index)`:

```
SRT link
recv      48,210
retrans   312  (1.2%)
loss det  580
dropped   0
```

`retrans (x%)` percentage is cumulative `retransTotal / recvTotal`. `dropped` is the headline
health number. Non-SRT sources show no stats tooltip.

### New UIManager surface

- `Q_PROPERTY(int sourceStatsVersion READ sourceStatsVersion NOTIFY sourceStatsChanged)` —
  change notifier mirroring `sourceConnectionVersion`.
- `Q_INVOKABLE int sourceLinkHealth(int sourceIndex) const` — 0=N/A, 1=Green, 2=Amber, 3=Red.
- `Q_INVOKABLE bool sourceHasSrtStats(int sourceIndex) const` — true once a source has emitted
  at least one stats snapshot.
- `Q_INVOKABLE QString sourceStatsTooltip(int sourceIndex) const` — preformatted multi-line.
- Internal: `std::vector<...>` of `{ SrtStats prev, SrtStats cur, SrtHealth health, bool seen }`
  keyed by `sourceIndex` (stable per-source identity, parallel to `m_sourceConnected`). Reset on
  `startRecording()`/`stopRecording()` like the connection state.

## Error handling / edge cases

- **Counter resets / reconnect:** on reconnect the SRT socket is recreated, so cumulative
  counters restart from 0. A delta would then go negative; `srtHealth` treats any negative delta
  as 0 (clamp), yielding Green — correct (a fresh socket has no recent loss). `UIManager` resets
  the source's `prev` to the zero struct on a `sourceConnectionChanged(index, true)` after a
  prior disconnect, so the first post-reconnect snapshot re-baselines cleanly.
- **Source index bounds:** all getters bounds-check `sourceIndex` against the vector and return
  N/A / false / "" for out-of-range (QML may query during teardown).
- **No stats yet / baseline:** `sourceHasSrtStats` is false until the first snapshot → dot uses
  the plain connected color, no tooltip. The first snapshot sets `seen=true`, is stored as the
  baseline `prev`, and renders Green; health grading begins at the second snapshot.

## Testing

1. **Pure unit test** `srtHealth()` — green/amber/red boundary cases: zero deltas → Green;
   `dDrop>0` → Red (even with high retrans); retrans rate just under/over `amberRate` → Green/
   Amber; negative deltas (reset) → Green. No Qt UI needed.
2. **Engine-level headless e2e** `e2e_native_srt_ui_stats` (native-apple-ingest label,
   `OLR_NATIVE_SRT=1`): `sync_harness` gains `--report-stats`, which subscribes to
   `ReplayManager::sourceStatsUpdated` and prints each source's final cumulative `SrtStats`.
   Drive one source through `lossy_udp_relay.py` at moderate loss and assert the harness reports
   `retrans>0` and `drop==0` (the data path delivers real, non-zero stats to the RM signal,
   consistent with the existing `srt_stats` log line); a clean-link control asserts ~no retrans.
   This exercises the entire engine→UI path **except the QML pixels**.
3. **QML rendering** — manual/visual check (the headless harness has no QML), documented in
   `tests/e2e/SRT_README.md`: connect a clean SRT source (green), a relayed lossy source
   (amber under stress, red on induced drops), and a non-SRT source (plain green, no tooltip).

## Files touched

- `recorder_engine/ingest/ingestsession.h` — `SrtStats` struct + `reportStats` callback.
- `recorder_engine/ingest/nativesrtingestsession.cpp` — invoke `reportStats` at each sample.
- `recorder_engine/srt_health.{h,cpp}` — new pure `srtHealth()` + `SrtHealth` enum.
- `recorder_engine/streamworker.{h,cpp}` — `statsUpdated` signal + callback wiring +
  `qRegisterMetaType<SrtStats>`.
- `recorder_engine/replaymanager.{h,cpp}` — `sourceStatsUpdated` relay signal + connect.
- `uimanager.{h,cpp}` — snapshot store, `onSourceStatsUpdated`, color computation, getters,
  version property, reset on start/stop and reconnect.
- `Main.qml` (~1355–1370) — dot color refinement + hover tooltip.
- `tests/e2e/sync_harness.cpp` — `--report-stats`.
- `tests/e2e/run_srt_ui_stats.sh` (new) + `tests/e2e/CMakeLists.txt` — the e2e gate.
- `tests/` unit test for `srtHealth()`.
- `tests/e2e/SRT_README.md` — manual UI-check doc.

## Success criteria

- A native-SRT source with a clean link shows green; under heavy retransmission shows amber;
  when the relay induces unrecovered drops shows red; returns to green on recovery.
- Hovering a native-SRT dot shows the cumulative recv/retrans/loss/dropped figures.
- Non-SRT sources are visually unchanged and show no stats tooltip.
- `srtHealth()` unit test and `e2e_native_srt_ui_stats` gate pass; no regression in the existing
  `srt` / `native-apple-ingest` gates.
