# Local SRT / native-ingest end-to-end tests

OpenLiveReplay ingests **only native `srt://` and native `rtmp://`/`rtmps://`**.
The pure-ffmpeg ingest path (and the `OLR_NATIVE_SRT` runtime gate) are gone:
native SRT is the default — and only — SRT ingest, and native RTMP is the default
RTMP ingest on both macOS and Windows. `udp://`/`file://`/`http://` and any other
scheme are **rejected** with a clean "unsupported scheme" error; there is no
ffmpeg decode fallback.

Because the standard record/sync/playback e2e used to feed the engine `udp://`
MPEG-TS, they now **bridge** their synthetic ffmpeg producers over UDP→SRT with
`srt-live-transmit` and let the engine ingest them over its native `srt://`
transport (see `run_record_e2e.sh`, `run_sync_e2e.sh`, `run_playback_e2e.sh`).
That keeps the producers (testsrc2/sine, flash/beep markers) and all their
assertions unchanged while exercising the only ingest path that exists.

These tests are **local-only** (not in CI). They need local producer tooling:
`ffmpeg`/`ffprobe`, `srt-live-transmit` (from `brew install srt`), and (for the
loss/jitter relays) `python3`. The scripts **SKIP cleanly** (exit 0) when a tool
is missing — `srt_require_tools` in `srt_lib.sh` is the shared guard.

## Build + run

No SRT-enabled ffmpeg build is required any more — the engine carries its own
native SRT/RTMP stack. Configure against the normal Homebrew FFmpeg (used only
for muxing/decoding the recorded fixtures) and run the native label:

```bash
cmake -S . -B build/native-srt -G Ninja -DOLR_BUILD_TESTS=ON \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.1/macos"
ninja -C build/native-srt record_harness sync_harness play_harness
( cd build/native-srt && ctest -L native-apple-ingest --output-on-failure )
```

The migrated udp→SRT gates run under their existing labels/selections:

```bash
( cd build/native-srt && ctest -R "e2e_record_"  --output-on-failure )  # record
( cd build/native-srt && ctest -L sync-report     --output-on-failure )  # sync scoreboard
( cd build/native-srt && ctest -R "e2e_play_"     --output-on-failure )  # playback
( cd build/native-srt && ctest -L native-rtmp     --output-on-failure )  # native RTMP
```

## Native SRT ingest

`srt://` input goes through the native pipeline:

```text
libsrt -> MPEG-TS parser -> H.264/H.265 access-unit splitter -> platform native decoder
```

Platform native video decoders:

- Apple: `VideoToolbox`
- Windows: `Media Foundation/D3D11` for H.264 and HEVC when Windows HEVC media
  support is installed

The native **AAC** decoder is `NativeAacDecoder` — AudioToolbox on Apple, Media
Foundation on Windows, and a stub elsewhere (see the `nativeaacdecoder_*` sources
in `recorder_engine/ingest/`).

The native SRT e2e scripts are shared across platforms. CTest registers the same
`e2e_native_srt_*` test names for each native platform and changes only the label
(no per-test environment — native is the default):

Shared native test names:

- `e2e_native_srt_smoke`
- `e2e_native_srt_4cam`
- `e2e_native_srt_sync`
- `e2e_native_srt_trim`
- `e2e_native_srt_connect`

Shared scripts:

- `run_srt_smoke.sh`
- `run_srt_4cam.sh`
- `run_srt_sync.sh`
- `run_srt_trim.sh`
- `run_srt_connect.sh`

Labels:

- macOS/iOS host builds: `native-apple-ingest`
- Windows host builds: `native-windows-ingest`

The producer side uses local `ffmpeg` + `srt-live-transmit`; the engine side
ingests over native SRT and needs no ffmpeg SRT support.

`e2e_native_srt_smoke` stands up one `srt://` flash/beep stream and asserts the
engine records a valid MKV **with real audio content** (the 1 kHz tone, not the
blue-fill/silence the engine emits when no source connects).

`e2e_native_srt_4cam` stands up **4** SRT cameras, each emitting a distinct audio
tone (1/2/3/4 kHz), records them into a 4-view MKV, and asserts each recorded view
carries its own camera's tone — proving 4 real SRT streams connect and **route
correctly** (view *i* = camera *i*).

Three feature-validation gates build on `srt_lib.sh` (bridge + tee'd full-frame-
flash producer + the `flash_pts_series` extractor):

- `e2e_native_srt_sync` — one flash source tee'd to **4** coincident SRT views;
  asserts every view carries the live flash (≥4 flashes — a disconnected view is
  blue-fill with 0 and FAILS) and the per-flash inter-view spread stays within a
  **generous** bound (250 ms). The bound is generous by design: the engine anchors
  each source to first-packet **arrival** (no genlock, audit REF-2), so coincident
  SRT is phase-locked-within-bounds, not zero-skew (typical measured spread ≈ 30 ms).
  Teeth: `OLR_SRT_SYNC_DROP_VIEW=<i>`.
- `e2e_native_srt_trim` — flash source tee'd to **2** coincident SRT views; trims
  view1 by `T` (default 300 ms) and asserts the measured (view0−view1) flash offset
  shifts by ≈ **−T** (the trim delays view1). Proves per-source trim (#28) works.
- `e2e_native_srt_connect` — records 4 SRT URLs with
  `sync_harness --report-connections` and asserts `connected=4`; a second run with
  the 4th URL pointed at a dead port asserts `connected=3`. Proves connection-status
  (#24) detects and discriminates. This gate also guards a teardown-robustness fix:
  a closing SRT socket used to stall `stopRecording()` for minutes (SRT's 180 s
  default linger × the global `srt_close()` lock); the engine now sets `linger=0`
  in the SRT URL query (it is receive-only) and the run completes in ~10 s.

The gate thresholds are validated against real local runs; if one proves flaky,
widen the bound (never delete the gate) — the content gates (per-view flash count,
connection count) carry the discrimination.

Windows uses the same shared H.264 parity scripts under `native-windows-ingest`,
plus separate HEVC coverage as `e2e_native_srt_hevc_smoke` under
`native-windows-hevc`:

```powershell
ctest --test-dir build/windows-native -C Debug -L native-windows-ingest --output-on-failure
ctest --test-dir build/windows-native -C Debug -L native-windows-hevc   --output-on-failure
```

The HEVC producer is capability-gated: the script skips when local `ffmpeg` has no
HEVC encoder, prefers `libx265`, then selects an available `hevc_*` encoder from a
Windows-friendly preference list. If Windows has no HEVC decoder MFT installed, the
stream is undecodable: native ingest logs the capability error and that source's
video is unavailable (no fallback — there is no ffmpeg decode path). The source
stays connected but unhealthy rather than silently switching to ffmpeg.

## Phase 2c-a: disconnect/reconnect survival

`e2e_native_srt_reconnect` (label `native-apple-ingest`) proves the engine survives
a mid-recording source drop. Two independent flash sources record (src0 control,
src1 victim); the script kills src1's `srt-live-transmit` bridge mid-record and
restarts it on the same port, then asserts:

- **src1 reconnected:** its `conn_events` timeline (from
  `sync_harness --report-connection-events`) shows `up`(before kill) → `down`(after
  kill) → `up`(reconnect) — a true reconnect, not a flaky warm-up.
- **content resumed:** src1's view has flashes both before the kill and in a late
  post-reconnect window — real frames resumed, not a frozen frame or blue-fill.
- **control fully isolated:** src0 has **no mid-record disconnect and no content
  gap** through the whole outage.

The outage must exceed the ~8 s stall window (kill@10 s, restart@20 s, record 38 s,
`TIMEOUT 240`; all env-overridable via `OLR_SRT_RECONN_*`). Teeth:
`OLR_SRT_RECONN_NO_RESTART=1` skips the restart → src1 never reconnects → FAIL.

**Why native ingest.** Building this gate uncovered a cross-source coupling in the
**now-removed legacy ffmpeg ingest**: a dead source's avformat reconnect churn
(repeated socket create → connect-fail → destroy) monopolized libsrt's *single
global receive thread*, starving packet delivery to the other healthy sources
(~2 s of content lost per reconnect cycle). The native ingest gives each source its
own libsrt socket, so a dead source's reconnect no longer perturbs the others — the
control source records 37/37 flashes with zero gaps. That coupling is the central
reason native ingest replaced the ffmpeg path.

## Phase 2c-b: packet-loss recovery

`e2e_native_srt_loss` (label `native-apple-ingest`) proves the native SRT ingest
recovers from packet loss via SRT's ARQ retransmit. A `lossy_udp_relay.py` sits on
the SRT link between the engine (SRT caller) and `srt-live-transmit` (SRT listener)
and drops a **seeded % of downstream SRT DATA packets only** — SRT control
(ACK/NAK/keepalive; first byte high bit `0x80`) always passes (no `command -v
python3` → SKIP). One source is recorded through the relay at three loss levels:

- **0 % baseline** → reference flash count `B`; relay drops nothing.
- **12 % moderate** → relay dropped data; the native session's `srt_stats` shows
  `pktRcvRetrans > 0` (ARQ retransmitted) and **`pktRcvDropTotal == 0`** (nothing
  finally unrecovered); the recorded view keeps ≥ 0.85·`B` flashes with no gap > 1.5 s.
- **88 % heavy (teeth)** → the run exits cleanly AND content degrades (≤ 0.5·`B`
  flashes or a gap ≥ 2 s) — proving the injected loss is real and the gate
  discriminates. (On loopback, SRT's 500 ms ARQ window recovers even 60 % loss, so
  the teeth needs ~88 %.)

The airtight recovery metric is **`pktRcvDropTotal`** (SRT's too-late-to-play,
finally-unrecovered loss), logged by the native ingest on stop — *not*
`pktRcvLossTotal`, which counts *detected* loss that ARQ then retransmits (so it is
expectedly non-zero under loss). Loss %, seed, and thresholds are env-overridable
(`OLR_SRT_LOSS_*`); the fixed seed makes drops deterministic.

## Phase 2c-c / 2d: soak, loss isolation, reordering

Three more native-ingest gates (`native-apple-ingest` label) complete the SRT
robustness suite:

- **`e2e_native_srt_soak`** — a 30 s native-SRT record must not crash/hang and must
  keep delivering content (first flash < 3 s, ≥ `SECS−3` flashes, no gap > 1.5 s).
  *Not* a drift detector: on one machine the source and recording share the same
  wall clock, so the flash-PTS slope is ~1.0 by construction (real drift needs two
  machines). The slope is reported as a diagnostic only.
- **`e2e_native_srt_loss_multi`** — two independent native sources, src1's link
  drops 12 % while src0's is clean. Asserts src1 recovers (`pktRcvDropTotal == 0`,
  full content) AND src0 is **fully isolated** (0 relay drops, `pktRcvDropTotal == 0`,
  full content, no gap). Extends the per-socket isolation finding to the loss
  dimension: each source owns its own libsrt socket, so one link's loss can't touch
  another.
- **`e2e_native_srt_jitter`** — `lossy_udp_relay.py`'s reorder mode delays each
  downstream SRT DATA packet a random 0–120 ms (control passes immediately),
  reordering the link; SRT's TSBPD (500 ms window) must re-order it. Asserts the
  relay actually reordered (`reordered > 0`), the recording is complete + continuous,
  and `pktRcvDropTotal == 0` (TSBPD recovered, nothing arrived too late to play).

## JIT-5: SRT link telemetry on the connection-status UI

The native SRT ingest samples `srt_bstats` ~1/sec and pushes each cumulative
snapshot (`IngestStats` with `kind=Srt`: recv / retrans / lossDetected / drop) up
the same queued-signal path as connection status: `reportStats` →
`StreamWorker::statsUpdated` → `ReplayManager::sourceStatsUpdated` → `UIManager`. The
per-source connection dot (`Main.qml`) is graded from the most-recent window:

- **green** — healthy (incl. ARQ quietly recovering everything),
- **amber** — link stressed: windowed retransmit rate > `OLR_SRT_HEALTH_AMBER_PCT`
  (default 0.02 = 2 % of received packets),
- **red** — recent **unrecovered** drops (`pktRcvDropTotal` rose this window), or
  the source is disconnected.

Hovering the dot shows cumulative `recv / retrans (%) / loss det / dropped`. SRT
stats are SRT-ingest-only — RTMP sources get the RTMP health model below.

**Automated coverage:**
- `tst_srt_health` (unit) — the pure green/amber/red classifier incl. the
  counter-reset clamp.
- `e2e_native_srt_ui_stats` (`native-apple-ingest`) — drives a source through
  `lossy_udp_relay.py` and asserts `sync_harness --report-stats` (which reads
  `ReplayManager::sourceStatsUpdated`) carries real telemetry: retrans>0 / drop==0
  under loss, ~no retrans on a clean link. Proves the whole path **except the QML
  pixels**.

## JIT-1: per-transport jitter window + effective SRT options

**Effective SRT options.** The native ingest sets `kSrtLatencyMs` /
`kSrtConnectTimeoutMs` via `srt_setsockopt` directly (those APIs are milliseconds),
plus `transtype=live` and `linger=0` (the engine is receive-only).

**Per-transport jitter window.** The engine holds frames a jitter window in the
past before encoding. SRT sources lean on SRT's TSBPD reorder buffer, so they use a
small floor (`kSrtJitterFloorMs`, default 80 ms, env-overridable via
`OLR_SRT_JITTER_MS`); RTMP keeps `kJitterBufferMs` (200 ms). The `StreamWorker`
picks the window from the URL scheme, snapshots it once per pulse, and applies it
to both video and audio (one A/V timeline).

**Tests:** the native continuity gate scripts (`run_srt_soak.sh`/`run_srt_loss.sh`/
`run_srt_jitter.sh`/`run_srt_4cam.sh`) prove the 80 ms floor keeps content
continuous (no gaps).

## AUD-4: single shared A/V anchor per source

Each source anchors audio and video to ONE timeline reference instead of two
independent first-packet-arrival anchors, which baked in a fixed ~+63 ms lip-sync
offset. The native ingest recovers the **MPEG-TS PCR** (`MpegTsParser` extracts the
90 kHz base from the PCR PID's adaptation field) and anchors both streams to it,
falling back to the first PES if no PCR has appeared. Audio maps against the shared
anchor and no longer re-anchors on its own — video/PCR owns re-anchoring on a
discontinuity.

**A native AAC decoder bug, found via the lip-sync marker:** the AudioToolbox AAC
path's input proc returned `noErr`/0-packets when out of data, which AudioConverter
reads as end-of-stream — so after the first decoded packet the reused converter went
terminal and dropped ~99 % of subsequent audio (gap-filled with silence). Fixed by
returning a non-`noErr` sentinel (`kNoMoreInputData`) so the converter yields the
partial output and stays alive. (A mostly-silent test marker won't survive native
ingest, so the migrated markers use continuous-enough content.)

**Gates:** `tst_mpegtsparser_pcr` (unit — PCR extraction) + the
`sourcePtsMsFromAnchor` mapping unit case; `e2e_av_lipsync` (`av-sync`, now over the
native SRT bridge) and `e2e_native_srt_lipsync` (`native-apple-ingest`, PCR path)
both assert mean `(audio − video)` within EBU R37 (−40..+60 ms) — the pre-AUD-4
+63 ms exceeds the +60 lag bound; the shared anchor collapses it toward 0.
Flash/beep are paired by nearest timestamp (robust to a spurious `silencedetect`
edge).

## Native RTMP parity

The native RTMP ingest (`NativeRtmpIngestSession`) is at parity with native SRT for
everything that applies to an RTMP-over-TCP transport (the SRT-protocol-only bits —
`srt_bstats` loss/retrans/drop counters, TSBPD, `SRTO_LATENCY`, linger — have no TCP
analog and are intentionally excluded). Native RTMP is enabled on **both macOS and
Windows** (Windows decodes via Media Foundation).

**Generalized health model.** The health pipe is backend-agnostic: `IngestStats`
carries a `kind` discriminator plus both the SRT loss counters and generic liveness/
throughput fields (`bytesTotal`, `lastPacketAgeMs`, `keyframeAgeMs`,
`decodeFailures`); the dot is graded by one of two pure functions — `srtHealth()`
and `rtmpHealth()` — selected in `UIManager` by `kind`. `rtmpHealth` (TCP has no
loss): **red** when the stream stalls (`>= kRtmpRedStallMs` since the last media) or
decode fails with no fresh bytes; **amber** on a decode failure this window, a brief
stall (`>= kRtmpAmberStallMs`), or a long keyframe gap (`>= kRtmpAmberKeyframeMs`);
else **green**; a counter reset (reconnect) clamps green.

**Single shared A/V anchor.** RTMP uses the same AUD-4 shared-anchor model: video
owns re-anchoring (reads `recordingClockMs()` on a forward/backward jump); audio
follows, flushing its decoder on its own discontinuity but **never moving the
anchor** — so A/V stays locked.

**Gates (`ctest -L native-rtmp`, macOS).** `e2e_native_rtmp_smoke` /
`e2e_native_rtmp_hevc_smoke` / `e2e_native_rtmps_hevc_smoke` (H.264 + HEVC, plain
and TLS), `e2e_native_rtmp_unsupported`, `e2e_native_rtmp_reconnect`,
`e2e_native_rtmp_interop`, `e2e_native_rtmp_soak` (`native-rtmp-soak`), and
`e2e_native_rtmp_ui_stats` (drives the Python RTMP fixture through
`sync_harness --report-stats` and asserts a real `kind=rtmp` snapshot — bytes > 0,
decodeFailures == 0 — over the native backend; the harness asserts the "Native RTMP
connected" marker, so an ffmpeg fallback fails the gate). Unit coverage:
`tst_rtmp_health` and the RTMP anchor cases in `tst_ingestbackendselector`.
