# Task 9 Shared Cleanup Deadline Design

> Historical planning document. Commands, branch references, checklists, and validation claims describe the plan at the time of writing; they do not establish current implementation or merge status.

## Goal

Prevent every Task 9 emergency cleanup path from extending a calibration
smoke run beyond its original 170-second outer deadline.

## Deadline contract

The smoke orchestrator owns two absolute monotonic timestamps. The outer
cleanup ceiling is `started + 170.0`; the operation deadline is exactly 30
seconds earlier. Pilot and measured work share the operation deadline. Cache,
session, reactor, process, native-lifecycle, and pipeline cleanup may consume
the reserved interval, but no cleanup call may receive a deadline later than
the outer ceiling.

General non-smoke runs preserve their current total bound: their cleanup
ceiling defaults to their existing effective pipeline deadline. The emergency
helper returns `min(time.monotonic() + 30.0, cleanup_ceiling)` and never creates
authority beyond the caller's absolute ceiling.

## Propagation

`run_real_calibration_smoke` publishes both timestamps through
`CalibrationSmokePreparation` and `RealCalibrationRunner.run`. The native
callback passes both to `run_compiler_audit_smoke_pipeline`. The pipeline core
passes the cleanup ceiling through `schedule_configuration_audits`,
`GenerationReactor`, and `_ScheduledAuditSession`. Reactor startup, setup,
spawn, scheduling, shutdown, and pipeline exception cleanup all use the same
capped helper.

## Failure behavior and tests

A fake monotonic clock places each failure near the operation deadline. The
matrix covers nested startup, setup, spawn, scheduling, shutdown, and pipeline
cleanup. It records every abort/reap deadline, asserts it is no later than the
original cleanup ceiling, and asserts cleanup either reaches the existing
terminal reconciliation receipt or deterministically reports expiry at that
ceiling. Changed or non-finite ceilings fail closed.
