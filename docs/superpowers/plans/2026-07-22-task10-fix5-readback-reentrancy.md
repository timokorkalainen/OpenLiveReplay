# Task 10 Fix 5 Readback Reentrancy Implementation Plan

> Historical planning document. Commands, branch references, checklists, and validation claims describe the plan at the time of writing; they do not establish current implementation or merge status.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the frozen Fix 4 C0/I2/M1 review by making first-readback fence creation deadlock-safe, correcting Apple ownership coverage, and strengthening the fallback/static-preprocessor audit.

**Architecture:** Every platform render dispatcher executes nested same-thread jobs inline. `GpuRhiContext` creates a fence candidate without holding its cache mutex, publishes the winner under the mutex, and records a terminal attempt so persistent creation failure is not retried on every frame. Runtime tests coordinate a worker first-readback with a nested render-thread readback; static mutations prove the named Apple fallback is the callback passed to minting and mixed unit/production conditions remain visible.

**Tech Stack:** C++17, Qt 6 QRhi/QThread/QtTest, Python 3 static source audit, CMake/CTest.

## Global Constraints

- Do not stage, commit, or push.
- Do not modify `tests/unit/tst_realcodecbenchmark.cpp` or the pre-existing untracked local plan/spec files.
- Preserve typed readback outcomes, exact-surface retirement/quarantine, and the private raw-readback boundary.
- Keep real Metal readback integration coverage; deterministic fake fences control only retirement completion.

---

### Task 1: Reentrant render dispatch and terminal fence cache

**Files:**
- Modify: `tests/unit/tst_gpurhicontext.cpp`
- Modify: `playback/gpu/gpurhicontext.h`
- Modify: `playback/gpu/gpurhicontext_apple.mm`
- Modify: `playback/gpu/gpurhicontext_win.cpp`
- Modify: `playback/gpu/gpurhicontext_stub.cpp`

**Interfaces:**
- Produces: `GpuRhiContextTestAuthority::setBeforeReadbackFenceCreateForTest(context, callback)` for deterministic unit coordination only.
- Preserves: `submitGpuReadback(rhi, surface, target) noexcept` and private `importAndReadback`.

- [ ] **Step 1: Write failing runtime tests**

Add a child-process-controlled test that blocks a worker immediately before its initial fence creation, enters `submitGpuReadback` from `invokeOnRenderThread`, releases the worker, and requires both calls to finish. Add a second test that calls readback twice on an invalid context and requires the creation hook to run once.

- [ ] **Step 2: Verify RED**

Run `cmake --build build-review-task10-fix4-on --target tst_gpurhicontext -j 2` and require compilation to fail because `setBeforeReadbackFenceCreateForTest` is absent.

- [ ] **Step 3: Implement minimal production behavior**

In all three render-thread `invoke` methods, execute `job()` inline when `QThread::currentThread() == this`. In `readbackFence`, inspect the cache/terminal-attempt flag under the mutex, copy the test hook, unlock, call the hook and `createFence`, then publish a successful candidate and the terminal attempt under the mutex.

- [ ] **Step 4: Verify GREEN**

Build and run `tst_gpurhicontext`; require the child to exit normally, the nested and worker submissions to return, one cached fence to remain, and failed creation to invoke its hook once across two calls.

### Task 2: Correct Apple retirement-fence ownership test

**Files:**
- Modify: `tests/unit/tst_gpuframedata.cpp`

**Interfaces:**
- Consumes: `GpuRhiContextTestAuthority::setReadbackFenceForTest`.
- Preserves: real `GpuRhiContext::importAndReadback` Metal execution.

- [ ] **Step 1: Write the corrected test assertions first**

Install a dedicated context `DeferredFence`, retain the existing producer `DeferredFence`, assert only the context fence signals, then complete the context fence and drain the registry before requiring surface destruction.

- [ ] **Step 2: Verify the focused Apple source/test contract**

Run the changed-line/static checks on Windows and retain the Apple test for macOS CI execution.

### Task 3: Bind the Apple fallback and preserve mixed production branches

**Files:**
- Modify: `tests/e2e/test_gpu_scope_completion_static.py`

**Interfaces:**
- Produces: a production preprocessor view that blanks unit-required conjunctions but conservatively preserves OR/mixed expressions.
- Produces: an Apple fallback audit requiring the named `cpuFallback` lambda to contain the helper and appear in the actual `mintGpuOrDegrade` call.

- [ ] **Step 1: Add failing mutations**

Add a dead-helper mutation that replaces the mint callback argument with an unrelated lambda, and add `#if defined(__APPLE__) || defined(OLR_UNIT_TEST)` containing a raw readback that must remain visible in the production view.

- [ ] **Step 2: Verify RED**

Run `ctest --test-dir build-review-task10-fix4-on -R ^tst_gpu_scope_completion_static$ --output-on-failure`; require the dead-helper or mixed-expression self-test to fail.

- [ ] **Step 3: Implement minimal audit changes**

Isolate the `cpuFallback` lambda body and the complete `mintGpuOrDegrade(...)` call with balanced delimiters, audit the lambda body, and require `cpuFallback` in the call arguments. Classify only unit-required conjunctions and exact unit-excluded expressions as removable; preserve mixed OR expressions.

- [ ] **Step 4: Verify GREEN**

Re-run the static test and negative compile control; require both to pass.

### Task 4: Final verification and evidence

**Files:**
- Create: `.superpowers/sdd/task-10-ci-fix5-report.md` (ignored evidence)

- [ ] **Step 1: Run focused and repeated tests**

Build affected GPU-on targets and run the readback/runtime/static/negative-compile gate, followed by at least ten repeated iterations of the critical tests.

- [ ] **Step 2: Run configuration and repository gates**

Run affected `-Werror` GPU-on and GPU-off builds, changed-line clang-format audit, `git diff --check`, `python tools/roadmap/audit.py`, and `python tools/roadmap/audit.py --frontier`.

- [ ] **Step 3: Record exact scoped identity**

Hash the tracked binary patch plus every new scoped file, document C0/I0/M0 closure evidence and the remaining macOS Metal execution gate, and leave the index untouched.
