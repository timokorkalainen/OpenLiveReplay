# Windows Real GPU Fault Lane Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Verify fence ordering and application recovery against a genuine driver-observed Windows GPU removal in an opt-in isolated test lane.

**Architecture:** A non-destructive parent harness performs capability checks and starts dedicated child modes for fence ordering and TDR triggering. The destructive child owns a real `PlaybackWorker` and runs the fault shader on that worker's exact D3D11 device. A kill-on-close Job Object and parent watchdog bound the child process tree; the parent captures evidence and fails closed once an explicitly enabled supported host begins execution.

**Tech Stack:** C++17, Qt Core/Test and `QProcess`, Windows Job Objects and read-only TDR-policy admission, D3D11/DXGI, HLSL compute shader, CMake/CTest resource labels, GitHub Actions workflow dispatch.

## Global Constraints

- The lane runs only when `OLR_GPU_FAULT_LANE=1` on a runner explicitly declared dedicated. It reads the effective TDR policy and never changes Windows TDR registry values.
- WARP, Null, remote, virtual, missing-fence, and unsupported-feature environments skip before destructive dispatch with an exact reason.
- Once supported destructive execution starts, trigger failure, missing observation, timeout, or invalid recovery is a test failure.
- The watchdog may terminate only its isolated child Job Object process tree. Submitted GPU work remains adapter-wide, so the dedicated-runner and recoverable-TDR requirements are separate safety boundaries.
- Preserve adapter identity, HRESULT, generations, fence values, registry counts, timing, and exit status as artifacts.
- Stage exact paths only, never bypass hooks, and do not auto-merge.

---

### Task 1: Build the fail-closed capability gate and parent/child protocol

**Files:**
- Create: `tests/gpu_fault/win_gpu_fault_protocol.h`
- Create: `tests/gpu_fault/win_gpu_fault_parent.cpp`
- Create: `tests/gpu_fault/win_gpu_fault_child.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: child modes `--probe-fence` and `--trigger-tdr`; newline-delimited JSON evidence; parent exit codes pass/skip/fail.
- Consumes: `OLR_GPU_FAULT_LANE`, DXGI adapter descriptors, remote-session detection, D3D feature/fence probes.

- [ ] **Step 1: Add protocol parser and gate tests**

Test disabled, WARP, remote, missing-fence, malformed-child-output, timeout, and supported cases. Assert unsupported cases never start a destructive child.

- [ ] **Step 2: Implement capability evaluation as a pure function**

Return a structure containing `supported`, `destructiveAllowed`, adapter LUID/name/vendor/device IDs, and one stable skip reason. Require environment opt-in plus hardware D3D, supported session, fence support, and process/watchdog availability.

- [ ] **Step 3: Implement bounded parent supervision**

Use `QProcess`, separate stdout/stderr capture, a monotonic deadline, and a uniquely named kill-on-close Job Object. The child verifies membership before device creation; terminate the Job Object on timeout. Treat child crash, protocol truncation, or deadline expiry as failure after destructive start.

- [ ] **Step 4: Register the opt-in CTest**

Add Windows-only tests labeled `gpu-fault` with resource locking so two fault children cannot run concurrently. Default test runs report skip unless explicitly enabled.

- [ ] **Step 5: Commit containment scaffolding**

Commit `test(gpu): add isolated Windows fault harness` with only fault harness and CMake paths staged.

### Task 2: Add the real fence-ordering probe

**Files:**
- Create: `tests/gpu_fault/win_gpu_long_dispatch.hlsl`
- Modify: `tests/gpu_fault/win_gpu_fault_child.cpp`
- Test: `tests/gpu_fault/win_gpu_fault_parent.cpp`

**Interfaces:**
- Consumes: hardware D3D11 device/context, real `GpuFence`, `GpuOpScope`, and `GpuRetireRegistry`.
- Produces: evidence fields for signal count/value, initially completed value, pending counts, final completion, CPU busy-wait count, and release observation.

- [ ] **Step 1: Add evidence-oracle tests**

The parent rejects evidence unless signal count is one, initial completion is below the signal value, pending count is positive while incomplete, completion reaches the value, and final pending count is zero.

- [ ] **Step 2: Implement bounded long GPU work**

Compile a shader with a finite iteration count calibrated to keep the fence observably incomplete without approaching TDR duration. Submit it, finalize one `GpuOpScope`, inspect completion once, wait using the fence event, then collect registry entries.

- [ ] **Step 3: Emit and validate ordering evidence**

Record timestamps and counters; do not spin on the CPU. The parent fails if the work completed too quickly to demonstrate ordering and prints a capability/calibration diagnostic.

- [ ] **Step 4: Run on provisioned hardware and commit**

Run with `OLR_GPU_FAULT_LANE=1`; expected result is a passing fence probe with one signal and zero final retains. Commit `test(gpu): verify real fence retirement ordering`.

### Task 3: Trigger and observe genuine TDR device removal

**Files:**
- Create: `tests/gpu_fault/win_gpu_tdr_dispatch.hlsl`
- Modify: `tests/gpu_fault/win_gpu_fault_child.cpp`
- Modify: `playback/gpu/gpurhicontext_win.cpp` only if an observation hook is required for the test executable
- Test: `tests/gpu_fault/win_gpu_fault_parent.cpp`

**Interfaces:**
- Produces: authoritative failed `GetDeviceRemovedReason()` HRESULT and real-loss publication evidence.
- Consumes: the existing host TDR policy without modifying it.

- [ ] **Step 1: Add failure-oracle tests**

Reject success HRESULT, injected-loss evidence, unchanged generation, missing epoch-bound token, and any report of waiting on a dead fence.

Bind the worker-cache NV12 surface as a shader resource in the destructive dispatch and read it in
the immutable-condition loop. Register that same surface against the post-dispatch fence so weak
release evidence falsifies the actual in-flight lifetime invariant.

- [ ] **Step 2: Implement the destructive child dispatch**

Create a real `PlaybackWorker` on the selected adapter in the child and dispatch a non-terminating/bounded-infinite compute workload on that worker's exact D3D11 device. Poll only the driver removal status at a bounded interval; emit the exact failed HRESULT when observed.

- [ ] **Step 3: Route observation through production authority**

Exercise the same Windows detection path that publishes the `DeadDeviceToken`; do not add a public token factory or test-only mint bypass.

- [ ] **Step 4: Verify watchdog containment**

The parent bounds trigger plus recovery, captures all child output, and fails rather than skipping if removal is not observed after the capability gate admitted the host.

- [ ] **Step 5: Commit genuine-removal coverage**

Commit `test(gpu): exercise genuine Windows device removal` with shader, harness, narrowly required production hook, and oracle tests staged.

### Task 4: Verify application recovery and stale-frame exclusion

**Files:**
- Create: `tests/e2e/run_gpu_fault_recovery.ps1`
- Modify: `tests/e2e/CMakeLists.txt`
- Modify: `tests/e2e/play_harness.cpp`
- Modify: `tests/gpu_fault/win_gpu_fault_parent.cpp`

**Interfaces:**
- Consumes: genuine-removal child evidence and existing device-loss output oracle.
- Produces: recovery evidence for generation, token epoch, dead-fence waits, retain release, blackout, stale frames, and coherent resumed output.

- [ ] **Step 1: Add a deterministic evidence stream**

Flush a structured destructive checkpoint immediately after removal observation and a separate
recovery result checkpoint. Merge JSONL records on success; preserve parsed partial evidence plus
raw stdout/stderr when the child fails after destructive work begins.

Seed the production worker cache with a GPU frame carrying a CPU fallback plus sequence and generation identities, then submit it through a real output runtime before removal. Emit output observations around the real removal and record recovery timing, blackout start/end, fallback/rebuild state, and post-loss output.

- [ ] **Step 2: Enforce recovery assertions**

Require generation advance, active-epoch real token, zero dead-fence waits, zero remaining readback/frame retains, no pre-loss frame after the recovery boundary, blackout within the documented bound, CPU cache recovery, and coherent CPU fallback or rebuilt GPU output within ten seconds.

- [ ] **Step 3: Run the complete opt-in scenario**

```powershell
$env:OLR_GPU_FAULT_LANE='1'
ctest --test-dir build/gpu-fault --output-on-failure -L gpu-fault
```

Expected: fence ordering and genuine removal/recovery pass on supported provisioned hardware; unsupported hosts skip before destructive execution.

- [ ] **Step 4: Commit the application oracle**

Commit `test(gpu): prove recovery after real device loss` with e2e harness, CMake, and parent oracle staged.

### Task 5: Provision the opt-in lane, archive evidence, and review containment

**Files:**
- Create: `.github/workflows/windows-gpu-fault.yml`
- Create: `docs/testing/windows-gpu-fault-lane.md`
- Modify: `docs/build-and-run.md`

**Interfaces:**
- Produces: manual/explicit workflow on a labeled hardware runner, uploaded evidence bundle, and operator instructions.

- [ ] **Step 1: Add an explicitly dispatched hardware job**

Require a dedicated runner label, set `OLR_GPU_FAULT_LANE=1` only inside the job, serialize with a concurrency group, apply a job timeout, and upload logs even on failure. Do not attach the lane to ordinary PR events.

- [ ] **Step 2: Document safety and evidence**

Document prerequisites, expected display reset, containment boundary, exact command, skip/fail semantics, collected fields, and confirmation that registry settings remain untouched.

- [ ] **Step 3: Run all non-destructive and destructive gates**

Run the full unit/GPU/e2e/sanitizer/lint/roadmap matrix, then run the provisioned fault workflow and retain its artifact link.

- [ ] **Step 4: Obtain independent security/concurrency review**

Review child containment, watchdog behavior, device-loss lock order, token epoch publication, Apple completion placement carried from the dependency PR, and proof that accidental execution is impossible.

- [ ] **Step 5: Push and open the final dependency PR**

Push through the credential-helper hook, open the PR with hardware evidence and safety model, and stop for the user to merge.
