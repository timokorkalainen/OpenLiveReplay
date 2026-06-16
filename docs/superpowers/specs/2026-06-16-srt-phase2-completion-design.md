# Phase 2 completion — drift, multi-source loss, jitter (native SRT ingest)

**Status:** approved (autonomous — user directed "finish full Phase 2"); local-only.
**Date:** 2026-06-16
**Depends on:** Phase 2c-b infra on this branch (`lossy_udp_relay.py`, native
`srt_stats` telemetry, `srt_lib.sh`, the `native-apple-ingest` CTest label).

Three remaining native-SRT robustness gates, all `OLR_NATIVE_SRT=1`,
`native-apple-ingest` label, local-only. Each reuses `srt_lib.sh` + `sync_harness`.
The native session logs `Source <i> "srt_stats pktRcvRetrans=.. pktRcvLossTotal=..
pktRcvDropTotal=.. pktRecvTotal=.."` on stop — `pktRcvDropTotal == 0` is the
"fully recovered / nothing finally dropped" proof (NOT `pktRcvLossTotal`, which is
detected-and-retransmitted loss).

## Gate 1 — `e2e_native_srt_soak` (2c-c): long-run stability soak

> **Reframed after review:** this is NOT a drift detector. On one machine the source
> and recording share the same wall clock, so the flash-PTS slope is ≈1.0 *by
> construction* regardless of any real drift (the engine timestamps on arrival, not
> source emission) — real drift detection needs two machines (out of scope). So this
> is an honest **stability soak**: a long native-SRT record must not crash/hang/leak
> and must keep delivering content the whole time. The slope is *reported* as a
> diagnostic, not gated.

One native SRT source recorded for **30 s** (`OLR_SRT_SOAK_SECS=30`). From
`flash_pts_series`: count, first-flash PTS, max inter-flash gap, and the
least-squares slope of (pts vs flash-index) — reported as `slope=…` (diagnostic).

Assert: the run **exits cleanly** with a valid MKV (1 video track); **first flash
PTS < 3 s** (warm-up is timely); **count ≥ `SECS − 3`** (content delivered the whole
run — only warm-up flashes may be missing); **max gap ≤ 1.5 s** (no mid-run stall).
Base port **23670**, `TIMEOUT 120`.

## Gate 2 — `e2e_native_srt_loss_multi`: multi-source loss isolation

Two **independent** native SRT sources, each with its own producer + bridge + relay.
src0's relay is clean (0 %); src1's relay drops 12 % (the 2c-b lossy data-only relay).
Record both into a 2-view MKV. This extends the 2c-a isolation finding to the loss
dimension: does loss on one source perturb another on the native path?

Assert:
- **src1 recovered:** its `srt_stats pktRcvDropTotal == 0` and its view has full
  flash content (≥ 0.85·baseline-ish, i.e. ≥ `SECS−2` flashes, no gap > 1.5 s) — SRT
  ARQ recovered the 12 % loss.
- **src0 fully isolated:** its relay `dropped == 0`, its `srt_stats pktRcvDropTotal
  == 0`, and its view has full flash content with **no gap** — src1's loss did not
  touch src0.

`SECS=10`. Base port **23680** (src0 at 23680.., src1 at 23685.. — 5-wide stride
covering S/UDP/R per source). `TIMEOUT 120`.

## Gate 3 — `e2e_native_srt_jitter`: packet reordering recovery

Extend `lossy_udp_relay.py` with a **reorder mode**: instead of (or in addition to)
dropping, hold each downstream DATA packet a random `0..JITTER_MS` before forwarding,
which reorders packets on the link. SRT's TSBPD (500 ms `SRTO_LATENCY`) must
re-order them back into timestamp order. Control packets pass immediately (never
delayed) so ACK/NAK timing is preserved.

Relay CLI gains an optional 6th arg `reorder_ms` (default 0 = current drop-only
behaviour). When > 0, downstream DATA packets are scheduled for release at
`now + rand(0, reorder_ms)` via a small time-ordered queue (`select()` timeout =
the next due packet); loss_pct and reorder can combine. Counters add `reordered`.

One source through the jitter relay (`JITTER_MS=120`, loss 0 %). Assert: despite
reordering, the recorded view is **complete and in-order** — full flash content
(≥ `SECS−2`), no gap > 1.5 s, and `pktRcvDropTotal == 0` (TSBPD recovered the
reordering; nothing arrived too late to play). Base port **23690**, `TIMEOUT 120`.
Teeth: a `JITTER_MS` far beyond the latency window (e.g. 800 ms > 500 ms) would push
packets past TSBPD and show `pktRcvDropTotal > 0` / gaps — used to confirm the metric
discriminates during tuning, not gated by default.

## Common

All three: `command -v python3` SKIP guard (Gates 2–3 use the relay), bash 3.2-safe,
background-harness + `>out 2>err` + `wait`, cleanup trap with `kill -TERM`→sleep→`-9`,
seeded relay. Registered inside `if(APPLE)`, `native-apple-ingest` label,
`OLR_NATIVE_SRT=1`. Thresholds tuned against real runs (widen, never delete). Docs:
`SRT_README`. Out of scope: cross-device clock drift (needs two machines),
encryption/passphrase, IPv6.

## Review fixes adopted (from the 4-critic design review)

- **Gate 1** reframed drift→soak (above).
- **Per-source stats (Gate 2):** add `srt_stat_src() { grep "^Source $1 srt_stats" |
  tail -1 | … }` — the existing `srt_stat` uses `tail -1` and would read the WRONG
  source when two are interleaved in stderr. (Confirmed: each source owns its own
  libsrt socket; `srtLibraryMutex` guards only `srt_startup/cleanup`, not `srt_recv`,
  so isolation is structural.)
- **Track + MKV guards (all gates):** assert the MKV path is non-empty/`-f`, and
  assert the expected video-track count (1 for soak/jitter, 2 for multi) before
  extracting per-source stats — else a never-connected source false-passes.
- **Warm-up-tolerant gaps:** ignore the first flash in gap analysis (NR>2) so a
  warm-up jitter blip can't false-fail.
- **Reorder injection proof (Gate 3):** the relay must count `reordered=N` (DATA
  packets released out of arrival order) and the gate must assert `reordered > 0`
  (mirrors `dropped > 0`) — else a no-op relay false-passes "in-order".
- **Cleanup:** add producer/bridge/relay PIDs to the global `PIDS` array (not only
  killed locally) so a harness hang can't orphan them.
- **Relay reorder mode:** optional 6th CLI arg `reorder_ms` (default 0 = today's
  drop-only); a time-ordered release queue, `select()` timeout = next-due release;
  control packets never delayed; loss applied before queueing (loss+reorder compose).
