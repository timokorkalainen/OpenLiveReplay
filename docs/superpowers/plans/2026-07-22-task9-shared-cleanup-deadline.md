# Task 9 Shared Cleanup Deadline Implementation Plan

> Historical planning document. Commands, branch references, checklists, and validation claims describe the plan at the time of writing; they do not establish current implementation or merge status.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep every emergency cleanup inside one original 170-second calibration-smoke ceiling.

**Architecture:** Carry separate absolute operation and cleanup deadlines. Reserve 30 seconds for cleanup in smoke runs; non-smoke runs use their existing effective deadline as both ceiling and operation bound. A single capped helper supplies all nested emergency deadlines.

**Tech Stack:** Python 3, `unittest`, fake monotonic clock, CTest.

## Global Constraints

- Do not stage, commit, or push.
- Preserve unrelated workspace files.
- Every emergency deadline is `min(now + 30.0, outer_cleanup_ceiling)`.
- The smoke outer cleanup ceiling is exactly 170 seconds from orchestration start.
- The smoke operation deadline reserves exactly 30 seconds inside that ceiling.
- General non-smoke behavior must never gain time beyond its existing effective deadline.

---

### Task 1: Lock the shared-deadline failure matrix

**Files:**
- Test: `tests/gpu/test_gpu_capability_runner.py`
- Test: `tests/gpu/test_gpu_capability_calibration.py`

**Interfaces:**
- Consumes: existing reactor/session abort and shutdown APIs.
- Produces: a fake-clock matrix asserting startup, setup, spawn, scheduling, shutdown, and pipeline cleanup receive only capped deadlines.

- [ ] Add a fake-clock test whose original ceiling is fixed and whose operation cutoff is 30 seconds earlier.
- [ ] Inject each failure boundary and record the deadline passed to `abort_and_reap_until_closed` or `abort_and_reap`.
- [ ] Assert each deadline is `<= original_cleanup_ceiling` and terminal state is reconciled or expiry is deterministic.
- [ ] Run the focused tests and confirm RED because current handlers create fresh `now + 30.0` deadlines.

### Task 2: Propagate the absolute cleanup ceiling

**Files:**
- Modify: `tests/gpu/gpu_capability_calibration.py`
- Modify: `tests/gpu/gpu_capability_runner.py`
- Modify: `tests/gpu/test_gpu_capability_calibration.py`
- Modify: `tests/gpu/test_gpu_capability_runner.py`

**Interfaces:**
- Consumes: `WorkerRuntimeContract.pipeline_deadline` as the operation cutoff.
- Produces: `emergency_cleanup_deadline(cleanup_ceiling: float) -> float`, `CalibrationSmokePreparation.operation_deadline`, and `CalibrationSmokePreparation.cleanup_deadline`.

- [ ] Compute `cleanup_deadline = started + 170.0` and `operation_deadline = cleanup_deadline - 30.0` once in `run_real_calibration_smoke`.
- [ ] Pass both timestamps through preparation, runner callback, smoke pipeline, scheduler, reactor, and session.
- [ ] Replace all six uncapped emergency deadline constructions with the capped helper.
- [ ] Cap public session/reactor cleanup inputs to their retained cleanup ceiling.
- [ ] Run the focused tests and confirm GREEN.

### Task 3: Verify and freeze Review 16

**Files:**
- Modify: `.superpowers/sdd/task-9-report.md`

**Interfaces:**
- Consumes: the completed Task 9 implementation and verification outputs.
- Produces: Review 15 correction notes, broad results, and frozen Review 16 SHA-256 hashes.

- [ ] Run the full runner, process-tree, command/calibration, provenance/live, and audit-lane suites.
- [ ] Run source mutation/performance audits, native smoke where the host permits, CTest, roadmap audit, syntax, YAML, and diff checks.
- [ ] Update the report and replace frozen hashes only after verification.
- [ ] Recheck all frozen hashes and leave the worktree unstaged and uncommitted.
