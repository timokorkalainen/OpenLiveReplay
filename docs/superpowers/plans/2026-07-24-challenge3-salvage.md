# Challenge 3 Salvage Implementation Plan

> Historical planning document. Commands, branch references, checklists, and validation claims describe the plan at the time of writing; they do not establish current implementation or merge status.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the sound ~20K lines of the Challenge 3 GPU surface-lifetime work (PRs #173/#174/#175) while deleting the ~95K lines of invented scope (the `tests/gpu/*.py` capability framework and its riders), fixing the four confirmed production defects, and closing the process holes that allowed the runaway.

**Architecture:** Merge #173 and #174 as-is (independently verified sound and proportionate). Close #175 unmerged and re-cut it **by path** off merged `main` into two honest PRs: 175a (production GPU work + C++ tests + negcompile/mutation controls, with confirmed defects fixed) and 175b (opt-in Windows TDR fault lane, wired into CI compile). Follow with a small hardening PR (`withRead`-only scope API) that closes the one real type-system hole, and a governance PR adding the scope trip-wires.

**Tech Stack:** C++17/Qt6, CMake/CTest, GitHub Actions, git worktrees.

## Global Constraints

- **Public repo** (CLAUDE.md): no internal task numbering (`Task 9`, `saved912`), no machine-specific paths or private notes, no unverifiable claims in PR bodies.
- **Never** `git push --no-verify`; merge via `gh pr merge <n> --merge` (never `--admin`).
- Format **changed lines only** (`git-clang-format`); several engine files are hand-written Allman style.
- Multi-step work in a worktree under `.claude/worktrees/<branch>`, not the root checkout.
- Follow the checked-in pre-push hook for platform-specific validation; CI is the authoritative unit gate.
- Commits end with `Co-Authored-By: Claude <noreply@anthropic.com>`; PR bodies end with the Claude Code attribution line.
- `python tools/roadmap/audit.py` must stay green (`107 initiatives, 0 problems` + whatever this plan registers).
- Every numeric claim in a PR body must be reproducible from the tree or explicitly labeled a one-off local measurement.

**Baseline refs (verify before starting):** `origin/main` = `29812ac8`; local refs `pr173`, `pr174`, `pr175` = heads of the three PRs; `pr175` head = `257bf41b`.

---

### Task 1: Merge #173 and #174

**Files:** none modified locally; GitHub PR operations only.

**Interfaces:**
- Produces: `origin/main` containing the enforcement point (`gpusurface.h` protected `nativeHandle()`), `GpuOpScope`/`GpuRetireRegistry` base, and all six call-site migrations — the base 175a re-cuts against.

- [ ] **Step 1: Confirm both PRs are still green and mergeable**

Run: `gh pr checks 173 && gh pr view 173 --json mergeable,mergeStateStatus --jq '.mergeable + " " + .mergeStateStatus'`
Expected: all checks pass; `MERGEABLE CLEAN`.

- [ ] **Step 2: Amend #174's body — strike the unreproducible perf claim**

Replace the sentence `Release medians versus #173 were within run noise (compositor +2.2%, Windows import/readback +1.1%)...` with:

> No regression measured in the retire-registry microbenchmark (`tst_gpuretireregistry_perf`). The compositor and Windows import/readback paths were not separately benchmarked; no perf harness links them.

Run: `gh pr edit 174 --body-file <edited-body.md>`

- [ ] **Step 3: Merge in order**

```bash
gh pr merge 173 --merge
git fetch origin
git merge-base --is-ancestor pr173 origin/main && echo "173 landed"
gh pr merge 174 --merge          # GitHub retargets 174 to main after 173 merges
git fetch origin
git merge-base --is-ancestor pr174 origin/main && echo "174 landed"
```

Expected: both `landed` lines print. If #174 shows conflicts after retarget, stop and report — do not rebase-force anything.

---

### Task 2: Close #175 and register the salvage in the roadmap

**Files:**
- Modify: `docs/broadcast-plan/program-state.json` (add salvage initiative with declared Files list)

- [ ] **Step 1: Close #175 with an honest comment**

```bash
gh pr close 175 --comment "Closing per review: the production GPU work is being re-cut by path into two scoped PRs (recovery+controls, fault lane). The tests/gpu Python capability framework (~91k lines), saved912 frozen baselines, and 2.67 MB .ii fixture are dropped — see docs/superpowers/plans/2026-07-24-challenge3-salvage.md."
```

Do **not** delete branch `feat/windows-gpu-fault-lane` yet (it is the provenance for the re-cut).

- [ ] **Step 2: Register the salvage initiative**

Read one existing entry in `docs/broadcast-plan/program-state.json` to copy the exact schema, then add an initiative (id suggestion: `gpu-lifetime-salvage`) whose `Files`/scope list names exactly the paths Tasks 3–8 touch. This makes the loop's diff-scope check *defined* for this work — the guardrail whose absence caused the runaway.

- [ ] **Step 3: Validate + commit (on the 175a branch created in Task 3, or a tiny standalone PR)**

Run: `python tools/roadmap/audit.py`
Expected: `roadmap audit OK: 108 initiatives, 0 problems.` (or 107+N).

---

### Task 3: Re-cut PR 175a by path

**Files:**
- Create branch `feat/gpu-lifetime-recovery` off `origin/main` in `.claude/worktrees/gpu-175a`
- Import from `pr175`: `playback/`, `recorder_engine/`, `ui/` (if diffed), `tests/unit/`, `tests/mutations/`, `tests/gpu/negcompile/`, `tests/gpu/gpu_lease_release_probe.cpp`, `tests/e2e/` (**minus** `test_gpu_scope_completion_static.py` and `run_gpu_fault_recovery.ps1`), `tests/CMakeLists.txt`, `tests/unit/CMakeLists.txt`, `tests/perf/tst_gpuretireregistry_perf.cpp` + trimmed `tests/perf/CMakeLists.txt`, `CMakeLists.txt`, `docs/hardest-technical-challenges.md`, `docs/superpowers/plans/2026-07-12-*.md`, `docs/superpowers/specs/2026-07-12-*.md`, `.gitattributes`
- **Excluded (dropped forever):** `tests/gpu/*.py`, `tests/gpu/fixtures/`, `tests/e2e/test_gpu_scope_completion_static.py`, `tests/perf/saved912*`, `tests/perf/tst_gpuretireregistry_production_perf.cpp`, `docs/superpowers/{plans,specs}/2026-07-1[46]-*.md`, `.github/workflows/ci.yml` changes (main's version stays), `tests/gpu_fault/` (goes to 175b)

**Interfaces:**
- Produces: a branch that builds GPU-on and GPU-off with the full pr175 production tree and C++ tests, no Python framework, no CI-lane changes.

- [ ] **Step 1: Create the worktree and import paths**

```bash
git fetch origin
git worktree add .claude/worktrees/gpu-175a -b feat/gpu-lifetime-recovery origin/main
cd .claude/worktrees/gpu-175a
git checkout pr175 -- playback recorder_engine tests/unit tests/mutations \
    tests/gpu/negcompile tests/gpu/gpu_lease_release_probe.cpp \
    tests/e2e tests/CMakeLists.txt CMakeLists.txt .gitattributes \
    tests/perf/tst_gpuretireregistry_perf.cpp \
    docs/hardest-technical-challenges.md
git checkout pr175 -- ui 2>/dev/null || true   # only if pr175 diffs ui/
git rm -r --cached --ignore-unmatch tests/e2e/test_gpu_scope_completion_static.py tests/e2e/run_gpu_fault_recovery.ps1
rm -f tests/e2e/test_gpu_scope_completion_static.py tests/e2e/run_gpu_fault_recovery.ps1
```

- [ ] **Step 2: Trim the Python/alias/fault wiring out of the imported CMake**

In `tests/CMakeLists.txt` (line numbers from pr175 — re-locate by content):
1. Delete the four `gpu_capability_*` test registrations (block at ~`:306-343`: `gpu_capability_source_audit`, `gpu_capability_source_audit_perf`, `gpu_capability_mingw_provenance_perf`, `gpu_capability_calibration_smoke`).
2. Delete the 11 alias registrations (`olr_add_gpu_compile_control_alias` / `olr_add_gpu_compile_pass_alias` uses, ~`:268-300`) **and** their helper definitions (~`:243-266`) — the comment says they exist to satisfy an internal report regex.
3. Delete `add_subdirectory(gpu_fault)` (~`:783`) — restored by 175b.
4. Add `-DEXPECT_COMPILE=FALSE` to the `RAW_READBACK` control (~`:133`), matching every sibling.
5. Delete registrations for the 4 dead negcompile sources and remove the files:

```bash
git rm tests/gpu/negcompile/lease_raw_access.cpp tests/gpu/negcompile/lease_return_pointer.cpp \
       tests/gpu/negcompile/lease_return_reference.cpp tests/gpu/negcompile/token_mint.cpp
```

(`lease_raw_access.cpp` asserts an invariant that no longer holds — it compiles today. `token_mint.cpp` is unwired; DeadDeviceToken construction is covered by `gpu_token_abandon.cpp` controls.)

In `tests/perf/CMakeLists.txt`: keep only the `tst_gpuretireregistry_perf` block (lines 1–8), the `olr_apply_windows_test_runtime` call, and everything from `tst_transportcommit_perf` down (lines 89–112). Delete lines 10–83 (the `saved912_commit` git probe, the three SHA256 pins, `olr_production_retire_perf`, and `tst_gpuretireregistry_production_perf`).

In `.gitattributes`: delete the `task9` fixture lines (~14–17) that reference `tests/gpu/fixtures/*` — the files no longer exist.

- [ ] **Step 3: Verify no stray references survive**

```bash
grep -rn "gpu_capability\|task9\|saved912\|scope_completion_static\|OLR_TASK9" \
    --include="*.txt" --include="*.cmake" --include="*.yml" --include="*.gitattributes" . \
    | grep -v "^\.claude" ; echo "exit=$?"
```

Expected: `exit=1` (no matches).

- [ ] **Step 4: Configure + build both flavors, run the unit suite**

Windows (adjust per `docs/windows-build.md`):
```bash
cmake -S . -B build/gpu-on -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
cmake --build build/gpu-on
ctest --test-dir build/gpu-on -L unit --output-on-failure
cmake -S . -B build/gpu-off -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=OFF
cmake --build build/gpu-off
```

Expected: both configure+build clean (no `saved912`/`gpu_capability` configure errors); unit failures limited to the ~11 known pre-existing Windows/MinGW ones. Record the pass/fail counts — they go in the PR body as *measured on this machine*.

- [ ] **Step 5: Commit the re-cut as one commit**

```bash
git add -A
git commit -m "feat(gpu): exact surface lifetimes and device-loss recovery (re-cut of #175)"
```

---

### Task 4: Fix the quarantine leak (live-device unreclaimable nodes)

**Files:**
- Modify: `playback/gpu/gpureadbackretainer.cpp` (quarantine path ~`:788-813`, drain paths `drainCompleted`/`drainWithBoundedWait`), `playback/gpu/gpureadbackretainer.h`
- Test: `tests/unit/tst_gpusurfacelease.cpp` (or `tst_devicelossmonitor.cpp` — whichever already exercises quarantine via `OLR_UNIT_TEST` probes)

**Interfaces:**
- Consumes: `RetireNode` (`state`, `fenceValue`, `identity`, `ownerCount`), `RetireNodeState::Quarantined`, `shard.activeHead`/`activeNext`, `recycleNode(shard, shardIndex, index, releases)`, `shard.quarantineOwners`, existing pattern at `abandonShardDomains` `:647-671`.

**Mechanism (confirmed defect):** `quarantine()` detaches the node from its fence group and sets `Quarantined`; both drain paths iterate `group.signaledHead` only, and the sole reclaim path (`abandonShardDomains`) requires a dead-domain token. On a **live** device (transient `Signal()` failure, `d3dfence.cpp:71`), the node and its `shared_ptr<GpuSurface>` owners are pinned for process lifetime, and the subsequent authority-epoch roll guarantees no future token matches.

**Fix rationale:** fence values are monotonic per fence and the D3D/Metal queues complete in order. If the *fence object* later reports `completedValue() >= node.fenceValue` (a later successful signal on the same fence completed), the GPU has provably drained past the quarantined submission — the read has retired and the node is safe to recycle. Keep the fence on the node at quarantine time to make that check possible.

- [ ] **Step 1: Write the failing test**

Locate the existing quarantine test (search `Quarantined` in `tests/unit/`). Add alongside it, using the file's existing fake fence + probe helpers:

```cpp
void tst_gpusurfacelease::quarantinedNodeRecyclesWhenFenceAdvances() {
    // Arrange: force a signal failure so GpuOpScope quarantines the batch
    // (use the file's existing failing-fence fixture).
    auto fence = std::make_shared<FailOnceSignalFence>();   // existing or trivial fake
    auto surface = std::make_shared<TestSurface>();
    submitAndExpectQuarantine(fence, surface);              // per existing helpers
    QCOMPARE(GpuReadbackRetainer::quarantinedOwnerCountForTest(), qsizetype(1));

    // Act: the same fence later completes a higher value (later op succeeded),
    // then a bounded drain runs.
    fence->completeUpTo(fence->lastReservedValue());
    GpuRetireRegistry{}.drainWithBoundedWait(/*perFenceTimeoutMs=*/0);

    // Assert: node recycled, owners released, no dead-domain token involved.
    QCOMPARE(GpuReadbackRetainer::quarantinedOwnerCountForTest(), qsizetype(0));
    QCOMPARE(surface.use_count(), 1);
}
```

Adapt fixture names to what the file already defines; if `quarantinedOwnerCountForTest()` does not exist, add it under `OLR_UNIT_TEST` mirroring the existing probe accessors.

- [ ] **Step 2: Run it, confirm it fails**

Run: `ctest --test-dir build/gpu-on -R tst_gpusurfacelease --output-on-failure`
Expected: the new test FAILS (count stays 1 / use_count stays 2).

- [ ] **Step 3: Implement**

1. Add `std::shared_ptr<GpuFence> quarantineFence;` to `RetireNode` (set in `quarantine()` from the group's fence before detach; cleared in `recycleNode`).
2. In `drainWithBoundedWait` (and `drainCompleted`, same helper), after the fence-group pass, add a quarantine sweep per shard, modeled on `abandonShardDomains`'s loop:

```cpp
// Quarantined nodes: the fence group is gone, but fence values are monotonic
// and the backend queue completes in order, so a later completed signal on the
// same fence proves the quarantined read has retired.
uint16_t index = shard.activeHead;
while (index != kNoNode) {
    RetireNode& node = shard.nodes[index];
    const uint16_t next = node.activeNext;
    if (node.state == RetireNodeState::Quarantined && node.quarantineFence &&
        node.fenceValue != 0 &&
        pollCompleted(node.quarantineFence) >= node.fenceValue) {
        const uint16_t ownerCount = node.ownerCount;
        recycleNode(shard, shardIndex, index, releases);
        shard.quarantineOwners -= ownerCount;
        shard.pendingOwners -= ownerCount;
        storage().metrics.pendingOwners.fetch_sub(ownerCount, std::memory_order_relaxed);
    }
    index = next;
}
```

Call `pollCompleted` **outside** the shard mutex where the file's existing pattern does (two-phase: collect candidates under lock, query outside, recycle under lock — mirror `collectFenceGroups`/`releaseCompletedGroups`).

- [ ] **Step 4: Run the test + the full gpu label**

Run: `ctest --test-dir build/gpu-on -R "tst_gpusurfacelease|tst_devicelossmonitor|tst_gpu_devicelost_worker" --output-on-failure`
Expected: all PASS, including the mutation-harness-covered suites.

- [ ] **Step 5: Add a mutation for the fix**

Extend `tests/mutations/gpu_retirement_mutations.cmake` with a mutant that deletes the sweep (patch the `node.state == RetireNodeState::Quarantined` condition to `false`) and assert the new test catches it, following the file's existing baseline-PASS/mutant-FAIL pattern.

- [ ] **Step 6: Commit**

```bash
git add playback/gpu/gpureadbackretainer.* tests/unit/ tests/mutations/
git commit -m "fix(gpu): reclaim quarantined retirements when their fence provably advances"
```

---

### Task 5: Fix refusal telemetry + placeholder policy at the mint site

**Files:**
- Modify: `playback/gpu/gpusurfaceallocator.cpp:93-116`, `playback/gpu/gpubudget.h/.cpp`
- Test: `tests/unit/tst_gpusurfaceallocator.cpp`

**Mechanism (confirmed defect):** a refused typed submission (`!result.succeeded()` at `gpusurfaceallocator.cpp:108`) returns `FrameHandle{}`, which `mintGpuOrDegrade` treats identically to OOM: `degradedResult` → `noteOomDegrade()` → (if the CPU fallback also fails) flat-black placeholder with `isPlaceholder=true`. Operator telemetry blames memory pressure for a fence/device-authority event, and during a loss window the output shows placeholders attributed to the wrong cause.

**Policy decision (locked in):** degrading on refusal is *kept* (delivering an unretired GPU surface would reintroduce the UAF class). The fix is attribution + observability, not behavior.

- [ ] **Step 1: Write the failing test**

```cpp
void tst_gpusurfaceallocator::submissionRefusalIsNotBookedAsOom() {
    GpuDeviceLossMonitor::instance().recordLossForTest();       // latch → submit refuses
    const auto oomBefore = GpuBudget::instance().oomDegradeCountForTest();
    const auto refusedBefore = GpuBudget::instance().submissionRefusalDegradeCountForTest();

    auto result = mintGpuOrDegrade(makeValidTestSurface(), makeTestRhi(), makeMeta(),
                                   makeTestFence(), cpuFallbackReturningValidPlanes(),
                                   GpuBudgetTag::Playback);

    QVERIFY(result.degradedToCpu);
    QCOMPARE(GpuBudget::instance().oomDegradeCountForTest(), oomBefore);           // unchanged
    QCOMPARE(GpuBudget::instance().submissionRefusalDegradeCountForTest(), refusedBefore + 1);
    QVERIFY(!result.handle.metadata().key.isPlaceholder);       // real CPU pixels, not black
}
```

Use the file's existing surface/RHI/fence fakes; `recordLossForTest` per existing injected-loss helpers.

- [ ] **Step 2: Run it, confirm it fails** (counter lands on OOM today).

- [ ] **Step 3: Implement**

In `gpubudget.h/.cpp`: add `void noteSubmissionRefusalDegrade();` + counter, mirroring `noteOomDegrade()` exactly (including the `ForTest` accessor pattern).

In `gpusurfaceallocator.cpp`, second overload: capture refusal out of the factory lambda and route attribution:

```cpp
GpuMintResult mintGpuOrDegrade(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence,
                               const std::function<CpuPlanes()>& cpuFallback, GpuBudgetTag tag) {
    if (!rhi || !rhi->isValid()) return degradedResult(meta, cpuFallback);
    bool submissionRefused = false;
    GpuMintResult result = mintGpuOrDegrade(
        std::move(surface), meta,
        [rhi = std::move(rhi), renderFence = std::move(renderFence), &submissionRefused](
            std::shared_ptr<GpuSurface> s, FrameMetadata m, GpuBudgetCharge charge) mutable {
            if (s && renderFence) {
                GpuRetireRegistry registry;
                GpuOpScope operation(renderFence, registry);
                auto adapter = []() noexcept { return GpuSubmitOutcome::Submitted; };
                const auto submitted = operation.submitRetained(
                    adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{s}));
                if (!submitted.succeeded()) {
                    submissionRefused = true;                 // refusal ≠ OOM
                    return FrameHandle{};
                }
                return makeGpuFrameHandle(std::move(s), std::move(rhi), m,
                                          submitted.producerFence, submitted.fenceValue,
                                          std::move(charge));
            }
            return makeGpuFrameHandle(std::move(s), std::move(rhi), m, std::move(renderFence),
                                      std::move(charge));
        },
        cpuFallback, tag);
    if (submissionRefused && result.degradedToCpu) {
        // mintGpuOrDegrade booked an OOM degrade for what was a submission refusal.
        GpuBudget::instance().reclassifyOomDegradeAsSubmissionRefusal();
    }
    return result;
}
```

If threading the flag through the first overload is cleaner than reclassifying (it is — `degradedResult` already takes `countOomDegrade`), extend `GpuMintResult`/the factory contract instead; keep whichever shape the first overload's callers tolerate. The invariant under test is the one that matters: **refusal increments the refusal counter, not the OOM counter.**

- [ ] **Step 4: Run tests** — new test PASS, full `tst_gpusurfaceallocator` + `tst_gpubudget` PASS.

- [ ] **Step 5: Commit** — `fix(gpu): attribute submission-refusal degrades distinctly from OOM`

---

### Task 6: Revert the Metal finite-timeout fence wait to event-driven

**Files:**
- Modify: `playback/gpu/gpufence_apple.mm:55-88`

**Mechanism (confirmed):** the `timeoutMs > 0` branch is a 1 ms sleep-poll loop; every production caller uses finite timeouts (compositor 100 ms per aliased input, DeckLink 2000 ms, encode pump 100 ms), so fence waits on the output cadence path now poll instead of waking on the Metal event.

- [ ] **Step 1: Replace the polling branch with the listener + `wait_for`**

```objc
bool wait(uint64_t value, int timeoutMs) override {
    if (value == 0) return true;
    if (!m_event || !m_listener) return false;
    if (completedValue() >= value) return true;
    if (timeoutMs == 0) return m_event.signaledValue >= value;

    struct WaitState {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
    };
    auto state = std::make_shared<WaitState>();
    [m_event notifyListener:m_listener
                    atValue:value
                      block:^(id<MTLSharedEvent> event, uint64_t) {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->done = event.signaledValue >= value;
                        state->cv.notify_all();
                      }];

    std::unique_lock<std::mutex> lock(state->mutex);
    if (m_event.signaledValue >= value) return true;
    if (timeoutMs < 0) {
        state->cv.wait(lock, [&] { return state->done; });
        return true;
    }
    state->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                       [&] { return state->done; });
    return m_event.signaledValue >= value;
}
```

- [ ] **Step 2: Gate = macOS CI** (this machine cannot compile .mm). Note in the commit body that `tst_gpufence` on macOS CI is the verification.

- [ ] **Step 3: Commit** — `fix(gpu): restore event-driven Metal fence waits for finite timeouts`

---

### Task 7: Documentation honesty pass (in-tree)

**Files:**
- Modify: `playback/gpu/gpudevicelossmonitor.h:124` (comment block), `docs/hardest-technical-challenges.md` (Challenge 3 status + "How to apply")

- [ ] **Step 1: Correct the safety-story comment**

The header comment explaining token routing is wrong twice (confirmed): `handleGpuDeviceLoss` never calls `realLossToken()` (it routes via `cleanupGpuRetirementsForDeviceLoss` → `withValidatedDeadDomains`), and stale-epoch real observations are **not rejected** — they take the cleanup-only proof path (`publishRealDeviceLoss` mints a stale-epoch token and runs an uncoordinated `abandonAllNoWait`, which is safe because the token names a driver-proven dead domain). Rewrite the comment to describe `withValidatedDeadDomains` and the cleanup-only branch. `realLossToken()` is test-only — say so where it is declared.

- [ ] **Step 2: Make the spec doc match what shipped**

In `docs/hardest-technical-challenges.md`: the "How to apply" step 3 promises `handleGpuDeviceLoss(DeadDeviceToken)` / `(InjectedDeviceLossForTest)` overloads. The shipped design routes the distinction through `withValidatedDeadDomains` / `withCoordinatedTokenlessRecovery` at runtime — a reviewed, tested equivalent. **Decision: amend the doc, don't add churn overloads.** Rewrite step 3 to describe the shipped mechanism and why (the token cannot exist at the worker's call site; validation happens inside the monitor under `m_epochMutex`). Keep the status row "Implemented" — it becomes true once this lands.

- [ ] **Step 3: Commit** — `docs(gpu): describe the shipped token-routing design accurately`

---

### Task 8: Open PR 175a with an honest body; independent review with artifact

**Files:** PR operation + `.claude/` nothing.

- [ ] **Step 1: Pre-push gates**

```bash
python tools/roadmap/audit.py
git push -u origin feat/gpu-lifetime-recovery   # hook runs; never --no-verify
```

- [ ] **Step 2: PR body must contain, explicitly:**

1. What it is: re-cut of #175's production content; what was dropped and why (one paragraph, link this plan).
2. **Riders requiring their own review sign-off** (coupled to this diff, kept deliberately, each with a subsection):
   - Media Foundation async-shutdown policy: deliberate COM `Detach()` (leak) on incomplete async MFT shutdown (`mediafoundationasynclifecycle.h`, decoder `reset()`), chosen over blocking/crashing on a wedged MFT; bounded per reset event.
   - DeckLink native path now accepts fence-less `{nullptr, 0, exact=true}` frames as producer-complete (VT-decoded surfaces are complete at callback time); previously these fell back to CPU. Covered by the new `tst_decklinksink` cases.
   - `OutputRuntime` immediate-dispatch (armed-cut) counters converted to entry-sampled atomics; lock discipline verified, bounded one-call dispatch deferral is possible where main re-checked under the mutex.
   - GPU-off builds now link the recovery stack (required by `playbackworker` references); the retire pool is a function-local static — constructed only on first GPU submission, so GPU-off pays code size, not RSS.
3. Fixes over #175: quarantine reclamation, refusal telemetry, Metal fence wait, doc corrections.
4. Measured numbers labeled as such ("full unit suite on maintainer Windows machine: N/M with the K known pre-existing MinGW failures; CI is the cross-platform gate").
5. Known deferred items: retire-pool sizing (16×1024×5 vs real bound 17) and the ~168 KB drain stack workspaces — follow-up initiative, with the numbers from the review.

- [ ] **Step 3: Independent review with a durable artifact**

Fresh-context reviewer (different agent), scope: the diff vs `origin/main`, explicitly including the riders. The review result must be **committed** — either as a GitHub PR review (`gh pr review --approve --body ...`) or a `docs/superpowers/reviews/2026-07-XX-gpu-175a.md` file — before the `independent-reviewed` label is applied. `tools/roadmap/merge_guard.sh` enforces the label; the artifact is what makes the label mean something.

- [ ] **Step 4: Merge** — `gh pr merge --merge` after CI gate + label. Confirm with `git merge-base --is-ancestor`.

---

### Task 9: Re-cut PR 175b — the Windows TDR fault lane, actually wired

**Files:**
- Create branch `feat/gpu-fault-lane` off updated `origin/main` in `.claude/worktrees/gpu-175b`
- Import from `pr175`: `tests/gpu_fault/`, `tests/e2e/run_gpu_fault_recovery.ps1`, `.github/workflows/windows-gpu-fault.yml`, `docs/testing/windows-gpu-fault-lane.md`
- Modify: `tests/CMakeLists.txt` (restore `add_subdirectory(gpu_fault)`), `.github/workflows/ci.yml` (2 lines), `tests/gpu_fault/CMakeLists.txt`, `tests/gpu_fault/win_gpu_fault_parent.cpp`, `tests/e2e/run_gpu_fault_recovery.ps1`

- [ ] **Step 1: Import + drop dead weight**

Delete `--probe-worker` (`win_gpu_fault_parent.cpp:439-459` + dispatch `:471-475`) and remove `win_gpu_fault_worker_oracle.cpp`, `olr_test_playback`, `Qt6::GuiPrivate` from the **parent** target's sources/link line in `tests/gpu_fault/CMakeLists.txt` (the child keeps the oracle).

- [ ] **Step 2: Make CI compile the lane and run its non-destructive tests**

In `.github/workflows/ci.yml` Windows job: append `win_gpu_fault_parent win_gpu_fault_child` to the explicit `--target` list (~`:557` on main), and add the two `ci`-labeled fault tests to the existing explicit `-R` ctest selection (`win_gpu_fault_protocol_selftest`, `win_gpu_fault_child_guard`). This makes `docs/testing/windows-gpu-fault-lane.md:47` ("remains part of normal CI") true instead of false.

- [ ] **Step 3: Harden the guard test and the skip paths**

1. `win_gpu_fault_child_guard`: replace `WILL_FAIL TRUE` with an exact assertion — the child exits 65 on unauthorized `--trigger-tdr`:

```cmake
set_tests_properties(win_gpu_fault_child_guard PROPERTIES
    PASS_REGULAR_EXPRESSION "destructive child requires parent authorization")
```

(add that exact message to the child's refusal path if it prints nothing today).
2. `run_gpu_fault_recovery.ps1`: add `--no-tests=error` to the ctest call; convert the three `exit 0` skip paths to `Write-Warning` + `exit 78` (distinct "skipped" code) so a green workflow run cannot mean "nothing ran". Update the workflow to treat 78 as neutral-with-annotation.
3. Raise the watchdog self-test budget `1000` → `5000` ms (`win_gpu_fault_parent.cpp:327`) and print which sub-check failed.

- [ ] **Step 4: Strike unverifiable prose**

Remove the GTX 1080 / HRESULT / 7.2 s narrative from `docs/testing/windows-gpu-fault-lane.md` unless the evidence JSONL from that run is committed alongside (preferred: commit it redacted under `docs/testing/evidence/`). The PR body states plainly: the destructive lane requires a self-hosted `[Windows, gpu-fault]` runner that does not exist yet; until provisioned, CI covers compile + protocol self-test + child guard only.

- [ ] **Step 5: Build, run the two non-destructive tests locally, push, PR, review-with-artifact, merge** (same gate discipline as Task 8).

---

### Task 10: `withRead`-only scope API (closes the real type hole)

**Files:**
- Modify: `playback/gpu/gpusurfacelease.h` (`GpuSyncReadScope`), the five production `withRead`/`read` call sites (`applegpusurface_apple.mm:301`, `wingpuimportedge.cpp:552,656`, `nativevideoencoder_mediafoundation.cpp:1002`, `nativevideoencoder_videotoolbox.mm:428` — re-verify list by grep), affected unit tests
- Create: `tests/gpu/negcompile/sync_scope_read_private.cpp`

**Why:** the one confirmed gap in "nothing left to forget": `GpuSyncReadScope` is public/default-constructible and `read()`/`complete()` are public, so a new site can leak `lease.nativeHandle()` into async work with no compile error. Making `withRead` the sole public entry (with `read`/`complete` private) closes the only property the deleted Python framework could even claim to police.

- [ ] **Step 1: Negcompile test first**

```cpp
// sync_scope_read_private.cpp — must NOT compile: read() is private
#include "playback/gpu/gpusurfacelease.h"
void misuse(const std::shared_ptr<GpuSurface>& s) {
    GpuSyncReadScope scope;
    auto lease = scope.read(s);   // expected error: 'read' is private
}
```

Register via `olr_add_compile_control` with expected tokens `GpuSyncReadScope` / `read` / `private`.

- [ ] **Step 2: Make `read()`/`complete()` private** in `GpuSyncReadScope`; keep both `withRead` overloads public (they already enforce void-return + auto-`complete()` via `CompleteOnExit`). Grep for external `\.read(`/`\.complete(` on sync scopes; migrate any stragglers to `withRead`.

- [ ] **Step 3: Build GPU-on, run unit + the new control, run mutation harness. Commit, PR, review, merge.**

Deferred (follow-up initiative, not this PR): non-addressable lease proxy (`operator&` deleted) and a typed `NoFenceProducerComplete` state replacing `isExact()`'s `{nullptr, 0, true}` acceptance in `framehandle.h` — record both in the roadmap initiative created in Task 2.

---

### Task 11: Governance trip-wires (the process fix)

**Files:**
- Modify: `docs/broadcast-plan/operating-loop.md`, `tools/roadmap/merge_guard.sh` (if it is the enforcement point), `CLAUDE.md` (one line)

- [ ] **Step 1: Draft the three rules into `operating-loop.md`** (maintainer ratifies via `directives.md` — agents do not author directives):

1. **No unregistered work.** Any change beyond a trivial fix requires a registered initiative with a declared Files list *before* implementation; the diff-scope check fails closed (absence of a Files list = reject, not skip).
2. **Diff-size / age trip-wire.** An unmerged slice exceeding **2,000 changed lines** or **48 h** of wall-clock work halts and requires a `directives.md` entry to continue. (The #175 runaway: 12 days, 117K lines, zero directive interactions.)
3. **Review claims require artifacts.** The `independent-reviewed` label may only be applied when a committed review artifact (GitHub review or `docs/superpowers/reviews/*.md`) exists for the exact head SHA; PR-body prose ("Critical 0 / Important 0") is not a review.

- [ ] **Step 2: Enforce what is cheaply enforceable** — extend `merge_guard.sh` to fail when `gh pr view --json reviews` is empty AND no `docs/superpowers/reviews/*<pr-number>*` file exists.

- [ ] **Step 3: Add to CLAUDE.md** (one line under the loop section): "A PR whose body does not describe the majority of its diff is rejectable on that ground alone."

- [ ] **Step 4: PR + maintainer directive entry ratifying the rules; merge.**

---

### Task 12: Cleanup

- [ ] Delete merged worktrees (`git worktree remove --force .claude/worktrees/gpu-175a` etc.) and the now-superseded branches `fix/gpu-surface-lease-lifetime`, `feat/gpu-lifetime-enforcement` (merged), keeping `feat/windows-gpu-fault-lane` until 175a+175b are both on `main`, then delete it too.
- [ ] Remove stale worktrees from the #175 effort: `gpu-surface-lease`, `gpu-surface-lease-rebase-root`, `gpu-lifetime-enforcement`, `gpu-lifetime-baseline`, `gpu-912-baseline` (check `git worktree list` for dirty state first).
- [ ] `python tools/roadmap/audit.py` green; mark the salvage initiative's status per the roadmap's lifecycle.

---

## Explicitly dropped (decision record)

| Dropped | Lines | Rationale |
|---|---|---|
| `tests/gpu/*.py` (19 files) | 86,642 | Central claim false as shipped (compiler lane audits 2 files with zero guarded sites); threat largely artificial (access control post-preprocessing); 29,661 lines unreachable; 36,808 test lines with 2 methods wired; owns the CI-budget blowout, the `g++.exe` SHA pin, and the sudo/cgroup lane. Implementing agent concurs. |
| `tests/e2e/test_gpu_scope_completion_static.py` | 4,562 | Second independent analyzer duplicating the first; superseded by Task 10's type-level fix. |
| `tests/gpu/fixtures/` incl. 2.67 MB `.ii` | — | Consuming test regenerates and byte-compares it; checked-in copy proven unnecessary. |
| `tests/perf/saved912*` + `tst_gpuretireregistry_production_perf` + git-SHA pin | 2,055 | Frozen fork of mid-branch production code; baseline meaningless post-merge; configure-time FATAL_ERROR landmine. |
| `docs/superpowers/{plans,specs}` 2026-07-14 / 2026-07-16 | 2,233 | Planning docs for the dropped framework; worst public-repo hygiene offenders. |
| ci.yml capability entries, sudo systemd-run step, Python pin | ~120 | Existed only to serve the framework. Runner-image bumps (`macos-15` etc.) are dropped with them; re-propose separately if wanted, with a body that says so. |
