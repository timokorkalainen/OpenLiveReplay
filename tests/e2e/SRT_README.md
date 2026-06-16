# Local SRT end-to-end test

The standard e2e tests feed the engine over UDP MPEG-TS. This SRT test exercises
the app's primary transport (`srt://`) against the **real** recording engine.

It is **local-only** (not in CI): the homebrew ffmpeg CI uses has no libsrt, so
the SRT test needs a one-time libsrt-enabled ffmpeg build.

## One-time setup (~10 min)

```bash
brew install srt openssl@3            # if not already present
bash build-scripts/build_ffmpeg_macos_srt.sh
```
This builds a native `macos_build/ffmpeg-srt/` (gitignored). It is
`--enable-nonfree` (openssl), so it is **for local testing only — not
redistributable**.

## Build + run the SRT e2e

```bash
cmake -S . -B build/srt -G Ninja -DOLR_BUILD_TESTS=ON \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.1/macos" \
  -DOLR_FFMPEG_SRT_PREFIX="$PWD/macos_build/ffmpeg-srt"
ninja -C build/srt record_harness
( cd build/srt && ctest -L srt --output-on-failure )
```

`e2e_srt_smoke` stands up one `srt://` flash/beep stream (via `srt-live-transmit`)
and asserts the engine records a valid MKV **with real audio content** (the 1 kHz
tone, not the blue-fill/silence the engine emits when no source connects). Without
`-DOLR_FFMPEG_SRT_PREFIX` the engine's avformat lacks SRT and the test fails by
design.

`e2e_srt_4cam` goes further: it stands up **4** SRT cameras, each emitting a
distinct audio tone (1/2/3/4 kHz), records them into a 4-view MKV, and asserts
each recorded view carries its own camera's tone — proving 4 real SRT streams
connect and **route correctly** (view *i* = camera *i*), not blue-fill silence.
Run it with the same SRT build via `ctest -L srt`.

## Phase 2b: feature validation over real SRT

Three gates validate shipped features over real `srt://` ingest (same SRT build,
`ctest -L srt`). They build on `srt_lib.sh` (bridge + tee'd full-frame-flash
producer + the `flash_pts_series` extractor reused from `run_sync_e2e.sh`):

- `e2e_srt_sync` — one flash source tee'd to **4** coincident SRT views; asserts
  every view carries the live flash (≥4 flashes — a disconnected view is blue-fill
  with 0 and FAILS) and the per-flash inter-view spread stays within a **generous**
  bound (250 ms). The bound is generous by design: the engine anchors each source
  to first-packet **arrival** (no genlock, audit REF-2), so coincident SRT is
  phase-locked-within-bounds, not zero-skew (typical measured spread ≈ 30 ms).
  Teeth: `OLR_SRT_SYNC_DROP_VIEW=<i>`.
- `e2e_srt_trim` — flash source tee'd to **2** coincident SRT views; trims view1 by
  `T` (default 300 ms) and asserts the measured (view0−view1) flash offset shifts by
  ≈ **−T** (the trim delays view1). Proves per-source trim (#28) works over SRT.
- `e2e_srt_connect` — records 4 SRT URLs with `sync_harness --report-connections`
  and asserts `connected=4`; a second run with the 4th URL pointed at a dead port
  asserts `connected=3`. Proves connection-status (#24) detects and discriminates.
  This gate also guards a teardown-robustness fix: a closing SRT socket used to
  stall `stopRecording()` for minutes (SRT's 180 s default linger × the global
  `srt_close()` lock), so the dead-source run hung ~7 min; the engine now sets
  `linger=0` in the SRT URL query (it is receive-only) and the run completes in
  ~10 s. See the `fix(srt): set linger=0` commit.

The gate thresholds are validated against real local runs; if one proves flaky,
widen the bound (never delete the gate) — the content gates (per-view flash count,
connection count) carry the discrimination.

## Native SRT path

The native ingest path can be tested without an SRT-enabled FFmpeg build. It
uses the same SRT producers, but runs the engine with `OLR_NATIVE_SRT=1`, so
`srt://` input goes through:

```text
libsrt -> MPEG-TS parser -> H.264/H.265 access-unit splitter -> platform native decoder
```

Platform native decoders:

- Apple: `VideoToolbox`
- Windows: `Media Foundation/D3D11` for H.264 and HEVC when Windows HEVC media
  support is installed

The native SRT e2e scripts are shared across platforms. CTest registers the
same `e2e_native_srt_*` test names for each native platform and changes only the
label:

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

Both labels run with `OLR_NATIVE_SRT=1`. The producer side still uses local
`ffmpeg` and `srt-live-transmit`; the engine side must not require FFmpeg SRT
support for native ingest.

Configure against the normal Homebrew FFmpeg (no `OLR_FFMPEG_SRT_PREFIX`) and run
the `native-apple-ingest` label:

```bash
cmake -S . -B build/native-srt -G Ninja -DOLR_BUILD_TESTS=ON \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.1/macos"
ninja -C build/native-srt record_harness sync_harness
( cd build/native-srt && ctest -L native-apple-ingest --output-on-failure )
```

Windows uses the same shared H.264 parity scripts and `e2e_native_srt_*` test
names under the `native-windows-ingest` label. It requires local producer tools
(`ffmpeg`, `srt-live-transmit`, and a Bash-compatible shell) and runs:

```powershell
ctest --test-dir build/windows-native -C Debug -L native-windows-ingest --output-on-failure
```

Windows also registers separate HEVC coverage as `e2e_native_srt_hevc_smoke`
under only the `native-windows-hevc` label:

```powershell
ctest --test-dir build/windows-native -C Debug -L native-windows-hevc --output-on-failure
```

Its producer is capability-gated: the script skips when local `ffmpeg` has no
HEVC encoder, prefers `libx265`, then selects an available `hevc_*` encoder from
a Windows-friendly preference list. The engine side still runs with
`OLR_NATIVE_SRT=1`; if Windows has no HEVC decoder MFT installed, native ingest
requests the normal FFmpeg fallback instead of retrying native decode forever.

This intentionally proves the native SRT path is carrying the stream: the default
Homebrew FFmpeg used by the harness does not provide `srt://`, so a fallback to
FFmpeg records blue-fill/silence and fails the content checks.

## Phase 2c-a: disconnect/reconnect survival

`e2e_native_srt_reconnect` (in the `native-apple-ingest` label) proves the engine
survives a mid-recording source drop. Two independent flash sources record (src0
control, src1 victim); the script kills src1's `srt-live-transmit` bridge mid-record
and restarts it on the same port, then asserts:

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

**Why native-only — a real ingest difference.** Building this gate uncovered a
cross-source coupling in the **legacy ffmpeg ingest**: a dead source's avformat
reconnect churn (repeated socket create → connect-fail → destroy) monopolizes
libsrt's *single global receive thread*, starving packet delivery to the other
healthy sources (~2 s of content lost per reconnect cycle — confirmed by a thread
sample parked in `srt epoll_wait`/`CGlobEvent`). No engine-side retry/throttle
mitigates it (the starvation is continuous during the churn). The **native ingest**
gives each source its own libsrt socket, so a dead source's reconnect no longer
perturbs the others — the control source records 37/37 flashes with zero gaps. So
the strict-isolation gate runs on the native path; the ffmpeg path's coupling is a
known limitation that motivates native ingest on Apple.

## Phase 2c-b: packet-loss recovery

`e2e_native_srt_loss` (label `native-apple-ingest`, `OLR_NATIVE_SRT=1`) proves the
native SRT ingest recovers from packet loss via SRT's ARQ retransmit. A
`lossy_udp_relay.py` sits on the SRT link between the engine (SRT caller) and
`srt-live-transmit` (SRT listener) and drops a **seeded % of downstream SRT DATA
packets only** — SRT control (ACK/NAK/keepalive; first byte high bit `0x80`) always
passes (no `command -v python3` → SKIP). One source is recorded through the relay at
three loss levels:

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

Three more native-ingest gates (`native-apple-ingest` label, `OLR_NATIVE_SRT=1`)
complete the SRT robustness suite:

- **`e2e_native_srt_soak`** — a 30 s native-SRT record must not crash/hang and must
  keep delivering content (first flash < 3 s, ≥ `SECS−3` flashes, no gap > 1.5 s).
  *Not* a drift detector: on one machine the source and recording share the same
  wall clock, so the flash-PTS slope is ~1.0 by construction (real drift needs two
  machines). The slope is reported as a diagnostic only.
- **`e2e_native_srt_loss_multi`** — two independent native sources, src1's link
  drops 12 % while src0's is clean. Asserts src1 recovers (`pktRcvDropTotal == 0`,
  full content) AND src0 is **fully isolated** (0 relay drops, `pktRcvDropTotal == 0`,
  full content, no gap). Extends the 2c-a per-socket isolation finding to the loss
  dimension: each source owns its own libsrt socket, so one link's loss can't touch
  another.
- **`e2e_native_srt_jitter`** — `lossy_udp_relay.py`'s reorder mode delays each
  downstream SRT DATA packet a random 0–120 ms (control passes immediately),
  reordering the link; SRT's TSBPD (500 ms window) must re-order it. Asserts the
  relay actually reordered (`reordered > 0`), the recording is complete + continuous,
  and `pktRcvDropTotal == 0` (TSBPD recovered, nothing arrived too late to play).

That completes Phase 2 (the SRT real-transport test ladder). Possible later work:
cross-device clock-drift (two machines), encrypted SRT (passphrase), and surfacing
the `srt_stats` loss telemetry on the connection-status UI (framesync roadmap JIT-5).

## JIT-5: SRT link telemetry on the connection-status UI

The native SRT ingest samples `srt_bstats` ~1/sec and pushes each cumulative
snapshot (`SrtStats`: recv / retrans / lossDetected / drop) up the same queued-
signal path as connection status: `reportStats` → `StreamWorker::statsUpdated` →
`ReplayManager::sourceStatsUpdated` → `UIManager`. The per-source connection dot
(`Main.qml`) is graded from the most-recent window:

- **green** — healthy (incl. ARQ quietly recovering everything),
- **amber** — link stressed: windowed retransmit rate > `OLR_SRT_HEALTH_AMBER_PCT`
  (default 0.02 = 2 % of received packets),
- **red** — recent **unrecovered** drops (`pktRcvDropTotal` rose this window), or
  the source is disconnected.

Hovering the dot shows cumulative `recv / retrans (%) / loss det / dropped`. SRT
stats are **native-ingest-only** — RTMP/UDP/ffmpeg-SRT sources keep the plain
green/red dot and no stats tooltip.

**Automated coverage:**
- `tst_srt_health` (unit) — the pure green/amber/red classifier incl. the
  counter-reset clamp.
- `e2e_native_srt_ui_stats` (`native-apple-ingest`) — drives a source through
  `lossy_udp_relay.py` and asserts `sync_harness --report-stats` (which reads
  `ReplayManager::sourceStatsUpdated`) carries real telemetry: retrans>0 / drop==0
  under loss, ~no retrans on a clean link. Proves the whole path **except the QML
  pixels**.

**Manual UI check** (the harness has no QML): with an SRT build, confirm a clean
source shows green, a relayed-lossy source goes amber under stress / red on induced
drops / back to green on recovery, and a non-SRT source stays plain green.

## JIT-1: per-transport jitter window + effective SRT options

**Effective SRT options (ffmpeg path).** SRT-private options must ride the URL query;
on the `avformat_open_input` opts dict they are silently dropped. `augmentSrtUrl()`
(`recorder_engine/ingest/ingestsession.cpp`) now adds them: `latency`/`rcvlatency`/
`peerlatency` (ffmpeg units are **microseconds** → `kSrtLatencyMs*1000`), `transtype=live`,
`connect_timeout` (**ms**), `linger=0`. The native path sets the same `kSrtLatencyMs` /
`kSrtConnectTimeoutMs` via `srt_setsockopt` directly (those APIs are milliseconds). The old
dict `latency=500` was doubly wrong: inert, and 500 µs ÷ 1000 = 0 ms even if it had applied.

**Per-transport jitter window.** The engine holds frames a jitter window in the past before
encoding. SRT sources lean on SRT's TSBPD reorder buffer, so they use a small floor
(`kSrtJitterFloorMs`, default 80 ms, env-overridable via `OLR_SRT_JITTER_MS`); raw UDP/RTMP
keep `kJitterBufferMs` (200 ms). The `StreamWorker` picks the window from the URL scheme,
snapshots it once per pulse, and applies it to both video and audio (one A/V timeline).

**Tests:** `tst_srt_options` (unit — `augmentSrtUrl` option/unit set, `jitterWindowMs` mapping);
`ctest -L srt` proves the ffmpeg options don't break ingest; the native continuity gate scripts
(`run_srt_soak.sh`/`run_srt_loss.sh`/`run_srt_jitter.sh`/`run_srt_4cam.sh`, `OLR_NATIVE_SRT=1`)
prove the 80 ms floor keeps content continuous (no gaps).

## AUD-4: single shared A/V anchor per source

Each source anchors audio and video to ONE timeline reference instead of two independent
first-packet-arrival anchors, which baked in a fixed ~+63 ms lip-sync offset. The native ingest
recovers the **MPEG-TS PCR** (`MpegTsParser` extracts the 90 kHz base from the PCR PID's adaptation
field) and anchors both streams to it, falling back to the first PES if no PCR has appeared; the
ffmpeg path anchors both to the first-of-either packet in a common µs timebase (PCR isn't reachable
through `avformat`). Audio maps against the shared anchor and no longer re-anchors on its own —
video/PCR owns re-anchoring on a discontinuity.

**A native AAC decoder bug, found via the lip-sync marker:** the AudioToolbox AAC path's input proc
returned `noErr`/0-packets when out of data, which AudioConverter reads as end-of-stream — so after
the first decoded packet the reused converter went terminal and dropped ~99 % of subsequent audio
(gap-filled with silence). A predominantly-silent stream (the gated beep) decoded to pure silence;
continuous tone only survived via occasional converter resets. Fixed by returning a non-`noErr`
sentinel (`kNoMoreInputData`) so the converter yields the partial output and stays alive.

**Gates:** `tst_mpegtsparser_pcr` (unit — PCR extraction) + the `sourcePtsMsFromAnchor` mapping unit
case; `e2e_av_lipsync` (`av-sync`, ffmpeg/UDP) and `e2e_native_srt_lipsync` (`native-apple-ingest`,
PCR path) both assert mean `(audio − video)` within EBU R37 (−40..+60 ms) — the pre-AUD-4 +63 ms
exceeds the +60 lag bound; the shared anchor collapses it toward 0. Flash/beep are paired by nearest
timestamp (robust to a spurious `silencedetect` edge).
