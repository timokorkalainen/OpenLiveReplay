# SRT Packet-Loss Recovery (Phase 2c-b) — Design

**Status:** approved (hardened after a 6-critic adversarial design review)
**Date:** 2026-06-16
**Depends on:** the native Apple SRT ingest (`OLR_NATIVE_SRT=1`,
`recorder_engine/ingest/nativesrtingestsession.*`) and Phase 2b/2c-a SRT test infra
(`srt_lib.sh`, `sync_harness`, the `native-apple-ingest` CTest label) — all on `main`.

## Goal

Prove the **native SRT ingest** recovers from packet loss via SRT's ARQ retransmit
(its 500 ms `SRTO_LATENCY` window): **moderate loss is recovered with no content
loss** — and *provably via retransmission*, not luck — validating SRT's value over
plain UDP; and the engine **degrades gracefully** (no crash/hang) under heavy loss.

First of the remaining Phase 2c sub-projects (2c-c long-run drift follows separately).

## Why the obvious version is wrong (review findings)

1. **Dropping "all downstream" packets breaks SRT.** SRT data *and* control
   (ACK/NAK/keepalive) travel in the **same UDP 5-tuple**; dropping all downstream
   datagrams kills the ACK/NAK flow → the connection stalls and ARQ never runs.
   **The relay must drop only downstream DATA**, identified by the SRT packet-type
   bit: the first bit of an SRT packet (MSB of the first 32-bit word, i.e.
   `firstByte & 0x80`) is **1 for control, 0 for data**. Control always passes.
2. **"relay dropped > 0" + "12 % still full" does not prove ARQ recovered it** —
   random loss might simply miss keyframes, and the gate would pass even with ARQ
   disabled. The proof must show **retransmissions actually happened and recovered
   the loss** (SRT receiver stats), anchored to a 0 %-loss baseline, with a
   quantified heavy-loss degradation teeth.
3. **Random loss has run-to-run variance** (a short run could drop ~nothing) — the
   relay seeds its RNG for **deterministic** drops.

## Architecture

Single source (loss recovery is per-link; multi-source loss interaction is a later
sub-project). Native path only (`OLR_NATIVE_SRT=1` — this validates the native SRT
ingest's recovery). Reuses `srt_lib.sh` (flash marker, bridge, `flash_pts_series`)
and `sync_harness`.

Transport chain with the relay inserted on the **SRT link**:
```
ffmpeg flash producer ──UDP──▶ srt-live-transmit (SRT listener, port S)
                                         │  SRT
                              lossy_udp_relay.py  (listen R, → S; drop downstream DATA at LOSS%)
                                         │  SRT
                                   engine (SRT caller → srt://127.0.0.1:R)
```

### Component 1 — `tests/e2e/lossy_udp_relay.py` (new, python3 stdlib)

A bidirectional UDP proxy, ~50 lines. CLI:
`lossy_udp_relay.py <listen_port R> <bridge_host:bridge_port S> <loss_pct> <stats_file> [seed]`.
- One UDP socket bound to `R` (engine side); one ephemeral socket to `S` (bridge side).
  `select()` on both.
- **Upstream** (engine→relay→S): forward every datagram unchanged; record the
  engine's address from the first upstream packet (route downstream back to it).
- **Downstream** (S→relay→engine): forward, but for **DATA** packets
  (`buf[0] & 0x80 == 0`) drop with probability `loss_pct/100` using a seeded
  `random.Random(seed)` (default fixed seed → deterministic). **Control** packets
  (`buf[0] & 0x80`) are always forwarded.
- Counters: `forwarded`, `dropped` (data dropped), `control_forwarded`. On `SIGTERM`
  (and `atexit`), write `dropped=<n> forwarded=<m> control_forwarded=<k>` to
  `<stats_file>` and exit 0 — **a file, not stdout**, so a kill can't lose the
  counts. `command -v python3` gates the test (SKIP if absent); shebang
  `#!/usr/bin/env python3`.

### Component 2 — `nativesrtingestsession`: log SRT receiver stats on stop (minimal)

On session stop, read `srt_bstats(m_socket, &perf, 0)` and log one line (logged from
the `run()` receive loop, since the socket is closed when the loop returns):
`srt_stats pktRcvRetrans=<n> pktRcvLossTotal=<n> pktRcvDropTotal=<n> pktRecvTotal=<n>`.
Read-only, ~5 lines, always logged (cheap; doubles as the roadmap's JIT-5 telemetry).

> **Implementation correction (vs the draft below):** the airtight "unrecovered loss"
> metric is **`pktRcvDropTotal`** (SRT's too-late-to-play drops), **not**
> `pktRcvLossTotal`. `pktRcvLossTotal` counts *DETECTED* loss that ARQ then
> retransmits, so it is *expectedly non-zero* under loss. Recovery is proven by
> **`pktRcvRetrans > 0`** (ARQ engaged) **and `pktRcvDropTotal == 0`** (nothing finally
> dropped). The shipped gate uses these; treat any `pktRcvLossTotal ≈ 0` assertion
> below as superseded.

### Component 3 — `tests/e2e/run_srt_loss.sh` → CTest `e2e_native_srt_loss`

Base port **23660** (outside the native 23601–23640 block). `OLR_NATIVE_SRT=1`.
A reusable `measure(loss_pct, tag, port_base)` helper — each of the three runs gets
its **own** `port_base` (`BASE`, `BASE+4`, `BASE+8`) so a lingering socket from one
run can't collide with the next. Within a run, `S = port_base` (bridge SRT listener),
`UDP = port_base+1` (producer→bridge), `R = port_base+2` (relay listen / engine URL):
1. Spawn flash producer to `UDP` + `srt_bridge UDP S` (SRT listener on `S`).
2. Spawn `lossy_udp_relay.py R → S, loss_pct, $WORKDIR/<tag>.stats`.
3. Record one source via `sync_harness --url srt://127.0.0.1:<R> --seconds <SECS>`
   (background; stdout→`<tag>.out`, stderr→`<tag>.err`); `wait`.
4. Kill the relay (`kill -TERM` → `sleep 0.5` → `kill -9`) so it flushes its stats
   file; kill producer + bridge.
5. Emit: relay `dropped`, the harness's `srt_stats` line, `flash_pts_series` count +
   max gap for the recorded view.

Three runs + assertions:
- **Baseline `measure(0, base)`** → record `B` = baseline flash count. Assert `B`
  is sane (≥ ~SECS−2). Relay `dropped == 0`.
- **Moderate `measure(12, mod)`** (RECOVERED) — assert all:
  - relay `dropped ≥ MIN_DROPS` (loss really injected on the SRT link);
  - `pktRcvRetrans > 0` (ARQ requested + received retransmissions);
  - `pktRcvLossTotal ≤ LOSS_TOL` (~0 unrecovered loss);
  - flash count ≥ `0.85·B` and **no gap > 1.5 s** (content recovered).
- **Heavy `measure(88, heavy)`** (TEETH / degradation — tuned up from 60 %; SRT
  recovers even 60 % on loopback) — assert:
  - the run **exited cleanly** (harness produced an MKV; did not hang to the CTest
    timeout);
  - **degraded**: flash count ≤ `0.5·B` **OR** at least one gap > 2 s — proving the
    injected loss is real and impactful and the gate would catch a recovery failure.
    (If 60 % still fully recovers, raise it — tuned against real runs.)

Teeth note: the moderate run's `pktRcvRetrans > 0` + near-full content is the
recovery proof; the heavy run's quantified degradation is the discrimination proof.

### Component 4 — CMake + docs

Register `e2e_native_srt_loss` inside the `if(APPLE)` block, label
`native-apple-ingest`, `ENVIRONMENT OLR_NATIVE_SRT=1`, `TIMEOUT 180` (three ~8–10 s
records + margin), `RUN_SERIAL`. Extend `SRT_README.md` with a Phase 2c-b section
(what it proves, the relay's data-only drop, the retransmit-stats proof, thresholds).

## Thresholds (tuned against real local runs)

`LOSS_MOD=12`, `LOSS_HEAVY=88` (tuned from 60), recovered floor `0.85·B`, recovered max-gap `1.5 s`,
degraded ceiling `0.5·B`, degraded min-gap `2 s`, `MIN_DROPS`/`LOSS_TOL` chosen with
margin. The relay's fixed seed makes runs deterministic; if a gate proves flaky,
widen it (never delete), and pick `LOSS_HEAVY` high enough that degradation is
reliable. Record duration / GOP (`-g 30`, 1 s, shared marker) are fixed; the
baseline-anchored ratios absorb count changes.

## Testing strategy

This script *is* the test. The proof chain — relay drop count (loss injected) →
`pktRcvRetrans>0` + `pktRcvLossTotal≈0` (ARQ recovered) → ≥0.85·B content, no gap
(recovered) — plus the heavy-loss degradation teeth, is self-contained. Manual run:
SRT-enabled build (`-DOLR_FFMPEG_SRT_PREFIX`), then
`ctest -R e2e_native_srt_loss --output-on-failure`. CI unaffected (label excluded).

## Out of scope (later)

Multi-source simultaneous loss (does one link's loss perturb another? — likely
clean on native per 2c-a, worth a dedicated test), audio-under-loss / A-V drift,
jitter/reordering injection, and long-run drift (2c-c).
