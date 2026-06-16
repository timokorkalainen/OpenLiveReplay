# JIT-1: per-transport jitter window + fix the inert SRT options — Design

**Status:** approved (brainstorm 2026-06-16)
**Roadmap item:** framesync P0 / JIT-1 (see `broadcast-framesync-roadmap` memory)
**Base branch:** `feat/srt-jit1-jitter`, off `origin/main` (#40 + #39). Independent of JIT-5/PR #41.

## Goal

Two related SRT-timing corrections:

1. **Fix the inert SRT options (ffmpeg ingest path).** SRT-private options
   (`latency`/`rcvlatency`/`peerlatency`, `transtype`, `connect_timeout`) are currently set on
   the `avformat_open_input` options dict, where they do **not** propagate to the nested SRT
   `URLContext` — so they are silently ignored and SRT runs at its defaults (≈120 ms latency,
   and possibly *not* live mode). Move them to the URL query (where `linger=0` already correctly
   lives) so they actually apply.

2. **Right-size the engine jitter window per transport.** The engine holds every source's frames
   a fixed `kJitterBufferMs = 200 ms` before encoding — a second buffer stacked *on top of* SRT's
   own TSBPD reordering buffer. For SRT sources that double-buffers needlessly. Derive the window
   from the transport: a small floor for SRT (TSBPD already removed network jitter), the existing
   200 ms for raw UDP/RTMP (the engine is their only buffer).

## Background

The single session clock is `RecordingClock` (a free-running monotonic timer). Each source's
first frame is anchored to its arrival time; the engine then pulls frames `kJitterBufferMs` in the
past so late/jittered arrivals still land before their slot. Findings from the timing-chain map:

- **Engine jitter buffer** (`recorder_engine/streamworker.{h,cpp}`): `kJitterBufferMs = 200`,
  global, read at three tick-thread sites — the video pull-gate publish, the video frame dequeue,
  and the audio FIFO pull — applied identically to video and audio so they share one timeline.
  Per-source `trimOffsetMs` is layered additively on top.
- **ffmpeg SRT options** (`recorder_engine/ingest/ffmpegingestsession.cpp`): `latency=500`,
  `rcvlatency=500`, `peerlatency=500`, `transtype=live`, `connect_timeout=5000000` set via
  `av_dict_set(&opts, …)` → **inert**. Only `linger=0` is in the URL query → effective. (The
  `connect_timeout` value `5000000` reads like microseconds; the libsrt URL option is
  **milliseconds** — a latent unit bug to correct when moving it.)
- **native SRT options** (`recorder_engine/ingest/nativesrtingestsession.cpp`): `kSrtLatencyMs =
  500` and `kSrtConnectTimeoutMs = 5000` set via direct `srt_setsockopt` → **effective**. These
  are duplicated constants; the ffmpeg path should reuse them (DRY).
- SRT latency is **global** today (a constant per backend), not per-source. JIT-1 keeps it global
  (a shared constant); per-source latency config is out of scope.
- `liveBufferMs` (1000 ms, UIManager) is a **playback-side** seek margin — unrelated, not touched.

The post-TSBPD residual jitter the engine still sees on an SRT source is decode time +
thread-scheduling + the per-frame tick cadence — small (tens of ms) and **independent of the SRT
latency value** (whether SRT buffers 300 ms or 500 ms, TSBPD delivers paced in-order frames). So
the SRT engine window is a small fixed floor, not a function of the latency number.

## Scope

- **In scope:** moving the inert ffmpeg SRT options to the URL query; sharing the SRT latency /
  connect-timeout constants across both ingest paths; a per-transport engine jitter window (SRT
  floor vs. non-SRT default), worker-local.
- **Out of scope (YAGNI):** making SRT latency or the jitter window user-configurable (no
  settings/QML plumbing); per-source latency; dynamic measured jitter estimation; changing the
  native path's latency value; RTMP-specific buffer tuning.

## Component 1 — Fix the inert SRT options (ffmpeg path)

In `ffmpegingestsession.cpp`, the `if (scheme == "srt")` block moves every SRT-private option
from `av_dict_set(&opts, …)` into the URL query, alongside the existing `linger=0`:

- `latency`, `rcvlatency`, `peerlatency` = `kSrtLatencyMs` (500)
- `transtype` = `live`
- `connect_timeout` = `kSrtConnectTimeoutMs` (5000 — **milliseconds**, corrected from the dict's
  `5000000`)

**Shared constants.** Define `kSrtLatencyMs = 500` and `kSrtConnectTimeoutMs = 5000` once in a
shared header (`recorder_engine/ingest/ingestsession.h`) and use them in **both** the ffmpeg URL
query and the native `srt_setsockopt` calls (replacing the native file's private duplicates).

**Testable helper.** Extract the SRT URL-query augmentation into a pure free function
`QUrl augmentSrtUrl(const QUrl& url)` (in `ingestsession.{h,cpp}`, next to `selectIngestBackend`)
that adds the SRT query params iff the scheme is `srt` and the param is not already present
(respecting a user-supplied override, exactly like the existing `linger` guard). The ffmpeg
session calls it instead of building the query inline. This makes the option set unit-testable
without opening a socket.

**Empirical verification.** Each moved option's libsrt URL spelling/unit is confirmed against a
real SRT run (the `ctest -L srt` ffmpeg-path gates): the stream must still ingest, and
`transtype=live` now actually applying must not regress smoke/4cam/sync/trim/connect.

## Component 2 — Per-transport jitter window

- Keep `kJitterBufferMs = 200` as the **non-SRT default**. Add `kSrtJitterFloorMs` (start **80
  ms**, env-overridable via `OLR_SRT_JITTER_MS` for tuning and for the validation gate to sweep).
- `StreamWorker` gains `std::atomic<int> m_activeJitterWindowMs{kJitterBufferMs}`. In
  `captureLoop`, once `currentUrl` is resolved (before opening the session), set it from the
  scheme: `srt` → `kSrtJitterFloorMs` (honoring `OLR_SRT_JITTER_MS`), else `kJitterBufferMs`.
- Replace `kJitterBufferMs` at all **three** tick-thread read sites with
  `m_activeJitterWindowMs.load(std::memory_order_relaxed)`: the pull-gate publish in
  `onMasterPulse`, the video dequeue target in `processEncoderTick`, and the audio FIFO
  `jitterSamples` in `writeAudioForTick`. Video and audio read the same value each tick so the A/V
  timeline stays aligned.
- Applies to both native and ffmpeg SRT (both pre-buffer via TSBPD); keyed purely on the `srt`
  URL scheme, so backend selection is irrelevant.

A `Q_INVOKABLE`/getter is **not** added — the window is internal. (The value is exposed only via
the env override, for tuning.)

## Error handling / edge cases

- **URL change srt↔non-srt mid-record** (`changeSource`): `m_activeJitterWindowMs` updates on the
  next capture loop; one brief A/V timeline step, identical in nature to changing a per-source
  trim. Acceptable and rare.
- **Floor too small → SRT frame stutter:** the engine tick keeps the newest frame ≤ gate; if a
  frame misses its slot the previous frame repeats. The continuity gates (below) catch this; the
  env override lets us raise the floor without a rebuild. Final floor is pinned by measurement.
- **User-supplied SRT query options** (e.g. a URL already carrying `latency=`): `augmentSrtUrl`
  only adds a param when absent, so an explicit override wins — same rule as the existing `linger`
  guard.

## Testing

1. **Unit — `augmentSrtUrl()`:** an `srt://…` URL gains `latency=500`, `rcvlatency=500`,
   `peerlatency=500`, `transtype=live`, `connect_timeout=5000`, `linger=0`; a non-SRT URL is
   unchanged; a pre-existing param is preserved (override wins); no duplicate keys.
2. **Unit — jitter-window mapping** (extract a tiny **pure** helper
   `int jitterWindowMs(const QString& scheme, int srtFloorMs, int defaultMs)` so the scheme→window
   decision is testable without a running worker): `srt` → `srtFloorMs`, every other scheme →
   `defaultMs`. The worker reads `OLR_SRT_JITTER_MS` (default `kSrtJitterFloorMs`) and passes it as
   `srtFloorMs`, keeping the helper free of env/global state.
3. **E2e regression guard (the floor is safe):** run the native SRT continuity gate **scripts**
   directly with `OLR_NATIVE_SRT=1` (`run_srt_soak.sh`, `run_srt_loss.sh`, `run_srt_jitter.sh`,
   `run_srt_4cam.sh`) at the new floor — they assert no content gap > 1.5 s and full flash counts,
   so a too-small window stutters and fails. Sweep `OLR_SRT_JITTER_MS` to confirm the headroom.
   (These gate scripts exist on `main`; their CTest registration on Apple is restored by PR #41 —
   running the scripts directly does not depend on it.)
4. **E2e — ffmpeg SRT options apply / don't regress:** `ctest -L srt` (smoke/4cam/sync/trim/
   connect over the ffmpeg SRT path) stays green with the options now actually in effect.
5. The live-latency *reduction* is not headlessly measurable (offline record), so it is argued
   from the design and guarded by no-regression continuity; the payoff is lower live operating
   latency on SRT.

## Files touched

- `recorder_engine/ingest/ingestsession.{h,cpp}` — `kSrtLatencyMs`/`kSrtConnectTimeoutMs`
  constants, `augmentSrtUrl()` + `jitterWindowMs()` pure helpers.
- `recorder_engine/ingest/ffmpegingestsession.cpp` — call `augmentSrtUrl()`; drop the inert
  `av_dict_set` SRT options.
- `recorder_engine/ingest/nativesrtingestsession.cpp` — use the shared constants.
- `recorder_engine/streamworker.{h,cpp}` — `m_activeJitterWindowMs`, set in `captureLoop`, read at
  the three tick sites; `kSrtJitterFloorMs`.
- `tests/unit/tst_srt_options.cpp` (new) + `tests/unit/CMakeLists.txt` — `augmentSrtUrl` +
  `jitterWindowMs` unit tests.
- `tests/e2e/SRT_README.md` — note the per-transport jitter window + the now-effective SRT options.

## Success criteria

- The ffmpeg SRT path's constructed URL carries `latency/rcvlatency/peerlatency=500`,
  `transtype=live`, `connect_timeout=5000`, `linger=0` (unit-proven); `ctest -L srt` green.
- SRT sources record with the ~80 ms engine window and show no content gaps in the native
  continuity gate scripts; non-SRT sources keep 200 ms.
- Both ingest paths reference one shared pair of SRT latency / connect-timeout constants.
- No regression in `ctest -L srt`, `ctest -L unit`, or the native continuity scripts.
