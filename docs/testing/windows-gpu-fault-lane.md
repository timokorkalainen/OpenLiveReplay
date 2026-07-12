# Windows real GPU fault lane

This opt-in lane validates OpenLiveReplay fence retirement and recovery against a genuine
driver-observed D3D11 device removal. It is destructive to the isolated child process and may reset
the display driver. It never reads or changes Windows TDR registry policy.

## Safety boundary

- Ordinary builds and test runs skip the real-fault test because `OLR_GPU_FAULT_LANE` is unset.
- The GitHub workflow is manual-only, requires the exact confirmation `RUN REAL GPU FAULT`, and
  runs only on a dedicated self-hosted runner labeled `gpu-fault`.
- The parent rejects remote sessions, software/WARP adapters, and devices without D3D11 fence
  support before starting destructive work.
- GPU work lives in a dedicated child. The parent watchdog may terminate only that child.
- The destructive child also requires a one-run parent token; invoking its TDR mode directly fails
  before device creation.
- After capability admission, crashes, timeouts, truncated evidence, a missing removal HRESULT, or
  an invalid recovery oracle are failures, not skips.
- The workflow serializes access with the `windows-real-gpu-fault` concurrency group.

Use a provisioned hardware runner whose operator accepts a possible screen flicker/reset. Do not run
the destructive mode on an interactive production workstation.

## Local/provisioned execution

Configure a Windows GPU-on test build, then run:

```powershell
$env:OLR_GPU_FAULT_LANE = '1'
$env:OLR_GPU_FAULT_EVIDENCE = "$PWD\windows-gpu-fault-evidence.jsonl"
pwsh -NoProfile -ExecutionPolicy Bypass -File tests/e2e/run_gpu_fault_recovery.ps1 build/gpu-fault
```

With the opt-in unset, `win_gpu_fault_real` returns CTest skip code 77 without spawning the child.
`win_gpu_fault_protocol_selftest` is non-destructive and remains part of normal CI.

## Required evidence

The JSON-lines artifact records adapter name/IDs/LUID, fence signal and completion values, pending
retains, exact signed removal HRESULT, pre/post generations, real-token generation, elapsed time,
dead-fence wait count, and final pending-retain count. The parent requires:

- exactly one fence signal, an observably incomplete initial fence, and zero final retains;
- a failed `GetDeviceRemovedReason()` HRESULT from the hardware child;
- a generation advance and matching real-loss token published through `GpuRhiContext` authority;
- rejection of a frame stamped with the pre-loss GPU generation;
- zero waits on the dead fence and zero remaining retained surfaces.

Failure artifacts are uploaded even when the child crashes or the watchdog expires.
