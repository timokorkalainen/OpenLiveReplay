# Execution design — the three hardest technical challenges

Turns the worked solutions in [`docs/hardest-technical-challenges.md`](../../hardest-technical-challenges.md)
(merged in PR #168) into three production-ready, independently-reviewed pull
requests. This spec is the north star for that multi-PR effort; each PR gets its
own implementation plan.

## Goal & end state

Three separate `fix/*` PRs, delivered **one at a time**, each: re-grounded
against HEAD, implemented, locally verified, CI-gate GREEN, independently
reviewed in a fresh context, rollback recorded — then handed to the maintainer
to merge. **The loop does not self-merge** (maintainer decision, 2026-07-11);
"done" = all three are merge-ready and the maintainer has been told.

These are standalone **correctness fixes**, not roadmap-registry initiatives, so
they ride `fix/*` branches and do not touch `docs/broadcast-plan/*` — the
roadmap audit stays green untouched.

## Why re-grounding is mandatory

The source document explicitly says "re-verify against HEAD before acting," and
this is already real: `timecodealigner.cpp` has drifted from the doc's cited
lines (it now takes `nominalFps` as a constructor arg, not
`Smpte12m::kTimecodeNominalFps`). Every `file:line` in the doc is treated as a
lead to confirm, never as ground truth. If a claim no longer holds — especially
a Challenge-1 protocol hole — that is **reported**, not forced into a change.

## Orchestration & model-tiered delegation

Each PR runs as a `/workflow` pipeline; the main loop sequences the three across
turns and persists through CI waits. Per-PR pipeline:

1. **Re-ground** every doc reference against current code.
2. **Implement**, tiered:
   - **Codex** (`codex:rescue`) — mechanical/average: test scaffolding, the
     6-site GPU migration edits, `boundMs` wiring, producer-seam plumbing.
   - **Opus** — above-average cores: `TimecodeAlignerV2` arithmetic, the F1/F2
     transport edits, the GPU lease header + device-loss overloads.
   - **Fable** (subagent `model: fable`) — hard correctness reasoning +
     highly-technical correctness review: int128/rounding/drop-frame,
     the re-anchor interleaving argument, the type-state "illegal states
     unrepresentable" claim.
3. **Verify locally** (see Verification).
4. **Independent review** in a fresh context by a different agent than the
   implementer; a **concurrency review** for the playback-worker change and a
   **security pass** where relevant (CLAUDE.md).
5. **Receiving-code-review** loop — verify each finding technically, fix,
   re-verify (no blind agreement).
6. **Push → CI gate GREEN**.
7. **Stop at merge-ready**; record rollback; inform the maintainer.

## The three PRs (risk order 2 → 1 → 3)

### PR-1 · `fix/timecode-rate-aware-alignment` — Challenge 2 (above-average, confirmed bug)
Replace the nominal-30 frame-differencing in `TimecodeAligner` with rate-aware
`TimecodeAlignerV2` (exact `__int128`, typed `Incomparable`). Re-wire the
producer seam (`nativesrtingestsession.cpp`, `nativertmpingestsession.cpp`) to
emit `(tcFrames, trueRate)` rather than collapsing through `to100ns(tc, 30)`;
convert the ReplayManager servo target (`offsetUs/1000`, deleting the
`frames·1000/m_fps` step); derive the `sourceoffsetestimator` `boundMs` tiers.
Unit tests incl. the falsifier (two 60p sources, anchors 10 s apart → 0 ms, not
−5000) and the 30 fps / common-TC regression that must stay byte-identical.
Self-contained; proves the pipeline first.

### PR-2 · `fix/transport-epoch-reanchor` — Challenge 1 (hard, concurrency-critical, model-derived)
**Confirm first:** port/re-run `transport_epoch_modelcheck.py` against HEAD to
independently reproduce holes H1 (swallowed reset) and H2 (commit-to-reset gap).
If a hole does not reproduce, report it and re-decide — do not force a change.
If confirmed: **F1** — `resetPlayEpoch()` (every site incl. deferred-pending)
also bumps `m_configGeneration`; **F2** — apply the epoch reset atomically inside
the reposition-commit critical section (mirroring the armed-cut path) and drop
the `resetPlayEpoch` parameter from `refreshOutputAfterSeekCommit`. Also move the
checker under `tests/formal/` + wire it into CTest so the invariant is
machine-gated forever. Mandatory Fable correctness review + independent
concurrency review.

### PR-3 · `fix/gpu-surface-lease-lifetime` — Challenge 3 (hard/large)
Add `gpusurfacelease.h` (lease/scope types + provenance-bound `DeadDeviceToken`);
make `GpuSurface::nativeHandle()` + retain virtuals `protected` with the lease
types as friends; migrate the six op-sites; split `handleGpuDeviceLoss` into
token (real) and injected (test) overloads with no no-wait free on the injected
path; add the CMake negative-compile test. The opt-in Windows real-TDR fault
lane ships as code + mutation design; **the real GPU-reset trigger is not run on
the maintainer's desktop without explicit OK** (a TDR visibly resets the
display) — verified instead by the negative-compile test + the existing injected
`devicelost` e2e. The CPU-off path stays byte-identical (typing change, not a
behavior change).

## Verification environments

- **Windows (MinGW, this box)** — `-Werror` build + delivery matrix + unit suite
  + GPU-regression e2e; owns the Windows-only real-device GPU/TDR path for PR-3.
  Already working (existing `build/`, plus `ffmpeg.exe`/`srt-live-transmit.exe`
  under `windows_build/dist/`).
- **WSL Ubuntu 26.04 (Linux CI-equivalent)** — clang **ASan/UBSan/TSan**, the
  full native `run_playback_e2e.sh` (ffmpeg + srt), and the full CTest matrix.
  Provisioned on demand (needs one-time passwordless-sudo enablement).
- **GitHub CI** — the authoritative "CI gate": macOS + Linux + Windows build,
  unit, playback + devicelost e2e, and the ASan/UBSan/TSan passes. Required
  GREEN for merge-ready.

Gate placement by PR: PR-1 is fully covered by Windows build+unit+Python proof
(sanitizers add depth via WSL/CI); PR-2's TSan runs in WSL/CI while the model
checker carries the logic-omission class TSan is blind to; PR-3 exercises the
Windows real-device path locally and sanitizers via WSL/CI.

## Definition of done (per PR)

Re-grounded · implemented · local gates green (per environment) · CI gate green ·
independent fresh-context review clean (+ concurrency/security where noted) ·
rollback recorded → handed to the maintainer to merge. The next PR starts only
once the current one is merge-ready and the maintainer has been informed.

## Risks

- **Challenge 1 is model-derived.** The repairs (F1/F2) are cheap and strictly
  hardening, but the specific interleavings need independent confirmation; the
  machine gate (CTest-wired model checker) is the durable mitigation.
- **Producer rate recovery (PR-1).** If a source's true TC rate is
  unrecoverable, the aligner never anchors → typed `Incomparable` → graceful
  fall-through to the existing clock-offset estimate. No guessing.
- **Real-TDR disruption (PR-3).** The real device-removal trigger is opt-in and
  never run against the maintainer's live desktop without explicit consent.
