# Output Validation And NDI Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add frame identity, per-target output stats, and testable NDI status so the output bus can prove frame-perfect parity before hardware output work.

**Architecture:** `OutputBusEngine` stamps deterministic frame identity as part of rendering. `OutputDispatcher` owns per-target delivery stats because it sees every assignment, sink result, and bus frame. `NdiOutputSink` exposes a small status snapshot around its existing backend abstraction.

**Tech Stack:** C++17, Qt 6, Qt Test, CMake/Ninja/CTest.

---

## Files

- Modify `playback/output/outputbusengine.h/.cpp` to define and stamp `OutputFrameIdentity`.
- Modify `playback/output/outputdispatcher.h/.cpp` to add per-target stats.
- Modify `playback/output/ndisink.h/.cpp` to add NDI status snapshots.
- Modify `tests/unit/tst_outputdispatcher.cpp` for frame identity and target stats tests.
- Modify `tests/unit/tst_ndisink.cpp` for NDI status tests.

## Task 1: Frame Identity

- [ ] Write failing `tst_outputdispatcher` assertions that two targets on the same bus/tick receive equal identities.
- [ ] Run `cmake --build build --target tst_outputdispatcher && ctest --test-dir build -R tst_outputdispatcher --output-on-failure` and confirm identity fields are missing.
- [ ] Add `OutputFrameIdentity` and stamp it in `OutputBusEngine`.
- [ ] Run the dispatcher test and confirm it passes.

## Task 2: Per-Target Stats

- [ ] Write failing `tst_outputdispatcher` assertions for per-target submissions, sink failures, repeated payloads, placeholder frames, silent audio frames, and last identity.
- [ ] Run the dispatcher test and confirm target stats are missing.
- [ ] Add target stats to `OutputDispatcher`.
- [ ] Run the dispatcher test and confirm it passes.

## Task 3: NDI Status

- [ ] Write failing `tst_ndisink` assertions for runtime unavailable, create failure, active, stopped, and send failure.
- [ ] Run `cmake --build build --target tst_ndisink && ctest --test-dir build -R tst_ndisink --output-on-failure` and confirm status APIs are missing.
- [ ] Add `NdiOutputStatus` and snapshot reporting to `NdiOutputSink`.
- [ ] Run the NDI test and confirm it passes.

## Task 4: Verification

- [ ] Run `cmake --build build --target OpenLiveReplay tst_outputdispatcher tst_outputruntime tst_queuedoutputsink tst_ndisink`.
- [ ] Run `ctest --test-dir build -R 'tst_outputdispatcher|tst_outputruntime|tst_queuedoutputsink|tst_ndisink|tst_qtpreviewsink|tst_outputbusengine' --output-on-failure`.
- [ ] Run `/opt/homebrew/opt/llvm/bin/git-clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format --commit origin/main --diff --extensions cpp,h,hpp,mm,c`.
- [ ] Run `git diff --check`.
