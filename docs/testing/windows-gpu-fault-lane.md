# Windows real GPU fault lane

This opt-in lane validates OpenLiveReplay fence retirement and recovery against a genuine
driver-observed D3D11 device removal. It is destructive to the isolated child process and resets the
selected display adapter. It reads the effective Windows TDR policy to fail closed, but never changes
registry policy.

## Safety boundary

- Ordinary builds and test runs skip the real-fault test because `OLR_GPU_FAULT_LANE` is unset.
- The GitHub workflow is manual-only, requires the exact confirmation `RUN REAL GPU FAULT`, and
  runs only on a dedicated self-hosted runner labeled `gpu-fault`.
- The parent rejects remote sessions, software/WARP adapters, and devices without D3D11 fence
  support before starting destructive work.
- Both parent and destructive child require a recoverable TDR policy (`TdrLevel=3`, bounded
  `TdrDelay`/`TdrDdiDelay`, non-debugger recovery, conservative limit values, and no reserved
  `TdrTestMode`). Missing keys use Microsoft's documented defaults. Unsafe or unreadable policy
  skips before device creation; the harness never writes these keys.
- GPU work lives in a dedicated child assigned to a named Windows Job Object with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. The child verifies exact membership before device creation;
  watchdog expiry terminates the complete contained process tree.
- The destructive child also requires a one-run parent token and exact Job membership. These are
  accidental-execution guards, not a privilege boundary against a local user who can invoke the
  parent harness.
- After capability admission, crashes, timeouts, truncated evidence, a missing removal HRESULT, or
  an invalid recovery oracle are failures, not skips.
- The workflow serializes access with the `windows-real-gpu-fault` concurrency group.
- The operator/workflow must assert `OLR_GPU_FAULT_DEDICATED_RUNNER=1`. This is an explicit promise
  that no unrelated workload depends on the selected adapter; software containment cannot make a
  TDR process-local after GPU commands have been submitted.

Use a provisioned hardware runner whose operator accepts a possible screen flicker/reset. Do not run
the destructive mode on an interactive production workstation.

## Local/provisioned execution

Configure a Windows GPU-on test build, then run:

```powershell
$env:OLR_GPU_FAULT_LANE = '1'
$env:OLR_GPU_FAULT_DEDICATED_RUNNER = '1'
$env:OLR_GPU_FAULT_EVIDENCE = "$PWD\windows-gpu-fault-evidence.jsonl"
pwsh -NoProfile -ExecutionPolicy Bypass -File tests/e2e/run_gpu_fault_recovery.ps1 build/gpu-fault
```

With the opt-in unset, `win_gpu_fault_real` returns CTest skip code 77 without spawning the child.
`win_gpu_fault_protocol_selftest` is non-destructive and remains part of normal CI.

## Required evidence

The JSON-lines artifact records the read-only TDR-policy snapshot, Job Object membership,
adapter name/IDs/LUID, fence signal and completion values, pending
retains, exact signed removal HRESULT, pre/post generations, real-token generation, elapsed time,
dead-fence wait count, and final pending-retain count. The parent requires:

- exactly one fence signal, an observably incomplete initial fence, and zero final retains;
- a failed `GetDeviceRemovedReason()` HRESULT from the hardware child;
- a generation advance and matching real-loss token published through `GpuRhiContext` authority;
- a tokenless submission failure existed first and was upgraded by authoritative DXGI proof, while
  the loss epoch can also collect every other owned device domain killed by the same adapter reset;
- the destructive shader read the exact worker-owned surface registered against its fence;
- rejection of a frame stamped with the pre-loss GPU generation;
- production `PlaybackWorker` recovery performed the device-scoped no-wait release of a surface
  fenced and owned by its output cache, with zero waits on the dead fence, an expired backing weak
  reference, and zero remaining retained surfaces.

The destructive child flushes removal/generation/fence evidence before invoking recovery, then
emits the recovery result as a second JSONL checkpoint. The parent merges checkpoints on success;
on timeout or abnormal exit it preserves the parsed partial checkpoint together with bounded raw
stdout and stderr.
- a live `PlaybackWorker` on the same adapter observes authoritative loss, replaces its cached GPU
  frame with the cached CPU fallback, rebuilds or coherently falls back, rejects the stale pre-loss
  frame, and submits post-loss output within 10 seconds.

Failure artifacts are uploaded even when the child crashes or the watchdog expires.
