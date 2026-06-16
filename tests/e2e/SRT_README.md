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

## Native Apple SRT path

The Apple native ingest path can be tested on macOS without an SRT-enabled
FFmpeg build. It uses the same SRT producers, but runs the engine with
`OLR_NATIVE_SRT=1`, so `srt://` input goes through:

```text
libsrt -> MPEG-TS parser -> H.264/H.265 access-unit splitter -> VideoToolbox
```

Configure against the normal Homebrew FFmpeg (no `OLR_FFMPEG_SRT_PREFIX`) and run
the `native-apple-ingest` label:

```bash
cmake -S . -B build/native-srt -G Ninja -DOLR_BUILD_TESTS=ON \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.1/macos"
ninja -C build/native-srt record_harness sync_harness
( cd build/native-srt && ctest -L native-apple-ingest --output-on-failure )
```

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

## Next (Phase 2c-c)

Long-run drift over SRT, plus (later) multi-source simultaneous loss and
jitter/reordering injection — each its own spec under `docs/superpowers/specs/`.
