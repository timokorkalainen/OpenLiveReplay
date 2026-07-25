# Independent merge-readiness review — Challenge 3 GPU surface-lifetime salvage

**Date:** 2026-07-25
**Reviewed:** `feat/gpu-lifetime-recovery` @ `ec6cbe30` (175a) + `feat/gpu-fault-lane` @ `a3100e02` (175b),
the by-path re-cut of the closed PR #175, stacked on the unchanged #173/#174.
**Method:** eight fresh-context review dimensions, each blocker/important finding adversarially
re-checked by a second independent agent, plus one agent that independently configured and built both
branches from clean directories. Reviewers did not see the implementation reasoning.

## Ruling

**Ready to merge after two rider decisions; fulfills Challenge 3 partially but honestly.**

- **No code-defect blockers.** Every correctness/concurrency/UAF/deadlock finding raised was refuted on
  re-check (0 of 8 confirmed findings are code defects). The type gate, the provenance token, the
  quarantine-reclaim fix, and the telemetry fix are all sound as shipped.
- The two hardest, machine-checkable Challenge 3 criteria **pass**: forgot-to-retain at a new op site is
  a compile error (both async and sync paths), and the no-wait free requires a provenance token no
  injection path can mint.
- The falsifiability half is **half-delivered and now honestly disclosed**: mutation (b) is realized;
  mutation (a) is structurally unauthorable and uninstrumented (no ASan/debug-layer), and the docs now
  say so.
- Merge is gated on two **governance** items, not code: two non-GPU-lifetime behavior changes ride in
  the stack and need either a split or explicit maintainer sign-off.

## Independent build verification (observed, not reasoned)

| Target | Result |
|---|---|
| 175a GPU-ON full build | 1197/1197, exit 0, zero warnings |
| 175a GPU-OFF full build | 980/980, exit 0 (recovery stack links with `OLR_GPU_PIPELINE=OFF`) |
| 175a `ctest -L gpu-capability` | 34/34 (incl. all five `gpu_retirement_mutation_*` and the sync-scope-private controls) |
| 175a `ctest -L unit` | 163/164 — sole failure `tst_qtpreviewsink`, the documented pre-existing Windows/MinGW headless-QML hang |
| 175a `ctest -N \| grep gpu_capability` | empty (dropped Python lane fully gone) |
| 175b GPU-ON build | 1206/1206; the two `gpu-fault;ci` tests pass; `win_gpu_fault_real` does not run |
| Rebase onto `origin/main` (4946c920) | clean — zero file overlap with new main commits |
| `python tools/roadmap/audit.py` | 108 initiatives, 0 problems (`rel-gpu-surface-lifetime` registered) |

## Challenge 3 scorecard

| Criterion | Status | Evidence |
|---|---|---|
| Forgot-to-retain = compile error (async op path) | pass | `nativeHandle()` protected, sole friend `GpuReadLease` (`gpusurface.h:99-106`); handle reachable only inside a fenced submit callback |
| …same for the synchronous read path | pass | `GpuSyncReadScope::read()/complete()` private; `withRead()` sole entry; `gpu_sync_scope_read_private`/`_complete_private` pass |
| Backends cannot re-widen access | pass | `d3d11gpusurface.h`, `applegpusurface_apple.mm` keep overrides protected |
| No-wait free needs an unmintable provenance token | pass | `DeadDeviceToken` private ctor friended only to the monitor; `abandonAllNoWait(GpuValidatedDeadDomains)`; injection routes tokenless to bounded-wait |
| Windows sync model typed, not silent | pass | typed `GpuFrameSynchronization`; refusal booked to a distinct counter |
| Hot-path cost unchanged | pass | production `tst_gpuretireregistry_perf` retained; no new per-frame driver call/lock/alloc/wait |
| Fault lane compiled by CI, destructive excluded | pass | CI builds both binaries, runs only the two non-destructive tests |
| Mutation (b) — compile-fail at a bypassed op site | pass | diagnostic-checked negcompile controls |
| Mutation (a) — mis-route injected loss → catch live UAF | **deferred** | unauthorable by type; no ASan/`D3D11_CREATE_DEVICE_DEBUG`; now disclosed |
| Real-TDR evidence committed | **partial** | tooling + protocol self-tests ship; no provisioned runner, no committed artifact — disclosed |

## Confirmed findings and disposition

1. **DeckLink accepts fence-less `{nullptr,0,exact=true}` frames as producer-complete, skipping the
   fence wait** (`decklinksink.cpp:60`). Previously such frames took the safe CPU fallback. Safe by
   construction today (the value==0 shape is emitted only where no `GpuOpScope` submission ran), but it
   is an on-air SDI behavior change and the exact unsynchronized-producer edge this challenge guards.
   **Disposition (2026-07-27): maintainer signed off** — accepted to ride in the stack, not split.
2. **Undisclosed decoder parameter-set-prepend change** (`nativevideodecoder_mediafoundation.cpp:189`) —
   prepends SPS/PPS/VPS to the first input sample after a session (re)start. New in this stack, tested,
   plausibly correct, but not GPU-lifetime and not in the disclosed rider list.
   **Disposition (2026-07-27): disclosed in the PR body and maintainer signed off** — not split.
3. **Doc: mutation (a) unbuilt / no ASan** — addressed by this change (added disclosures to
   `hardest-technical-challenges.md` and `windows-gpu-fault-lane.md`).
4. **Doc: Challenge 3 status row overclaimed** — addressed (softened to name the compile-fail
   falsifiability and the unprovisioned destructive lane).
5. **Scope discipline: test-only strays** — a Windows test-runtime rollout across
   `tests/qml|qmlstyle|smoke|e2e`, a QML layout-test rewrite, and a `tst_controlwebsocketserver`
   ack-race fix are outside the declared Files list and were not in the rider enumeration.
   **Disposition: acknowledge in the PR body; test-only, zero production impact.**

## What is genuinely good

- The enforcement closes both op paths and is airtight: the raw handle is reachable only through a lease
  obtainable only from a scope whose exit registers retirement; 34 diagnostic-checked controls plus an
  unrelated-error-rejection meta-test that keeps the matcher honest.
- The provenance token is genuinely unmintable by injection — the spec's hardest criterion, discharged
  cleanly.
- The ~94k-line deletion is affirmatively justified: C++ access control runs after preprocessing, so the
  compiler is a sound and complete oracle for the graded criterion; the dropped negcompile controls were
  replaced and broadened.
- Commit hygiene and PR-body honesty are strong; every checkable headline number is exact and the bodies
  disclose against their own interest.

## Follow-ups filed

- MF async-shutdown leak counter (observability for the disclosed COM-leak rider).
- Retire-pool sizing (16×1024×5 vs a real bound of 17; ~168 KB drain stack workspace; +16 B/node barrier).
- A debug-layer/ASan destructive fault-lane build once a `[self-hosted, Windows, gpu-fault]` runner exists,
  to close mutation (a).
