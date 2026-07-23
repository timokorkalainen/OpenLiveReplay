param(
    [string]$BuildDir = "build/gpu-fault"
)

$ErrorActionPreference = "Stop"
foreach ($root in @($env:OLR_QT_ROOT, $env:OLR_FFMPEG_ROOT, $env:OLR_SRT_ROOT)) {
    if ($root -and (Test-Path -LiteralPath (Join-Path $root "bin"))) {
        $env:PATH = "$(Join-Path $root 'bin');$env:PATH"
    }
}
if ($env:OLR_GPU_FAULT_LANE -ne "1") {
    Write-Host "SKIP: OLR_GPU_FAULT_LANE=1 is required"
    exit 77
}
if ($env:OLR_GPU_FAULT_DEDICATED_RUNNER -ne "1") {
    Write-Host "SKIP: OLR_GPU_FAULT_DEDICATED_RUNNER=1 is required"
    exit 77
}

if (-not $env:OLR_GPU_FAULT_EVIDENCE) {
    $env:OLR_GPU_FAULT_EVIDENCE = Join-Path (Get-Location) "windows-gpu-fault-evidence.jsonl"
}
Remove-Item -LiteralPath $env:OLR_GPU_FAULT_EVIDENCE -ErrorAction SilentlyContinue

ctest --test-dir $BuildDir -R "^win_gpu_fault_real$" --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$records = Get-Content -LiteralPath $env:OLR_GPU_FAULT_EVIDENCE | ForEach-Object {
    $_ | ConvertFrom-Json
}
$hostSafety = $records | Where-Object { $_.mode -eq "host-safety" } | Select-Object -Last 1
if ($hostSafety -and -not $hostSafety.tdrPolicySafe) {
    Write-Host "SKIP: $($hostSafety.reason)"
    exit 0
}
$capability = $records | Where-Object { $_.mode -eq "capability" } | Select-Object -Last 1
if ($capability -and -not $capability.supported) {
    Write-Host "SKIP: $($capability.reason)"
    exit 0
}
$tdr = $records | Where-Object { $_.mode -eq "trigger-tdr" } | Select-Object -Last 1
if (-not $tdr) { throw "missing trigger-tdr evidence" }
if (-not $tdr.jobContained -or -not $tdr.tdrPolicySafe -or -not $tdr.dedicatedRunner) {
    throw "destructive child lacked Job Object, TDR-policy, or dedicated-runner containment"
}
if ($tdr.removedHresult -ge 0) { throw "device removal HRESULT was not a failure" }
if ($tdr.generationAfter -le $tdr.generationBefore) { throw "GPU generation did not advance" }
if (-not $tdr.realLossToken -or $tdr.tokenGeneration -ne $tdr.generationAfter) {
    throw "real-loss token is missing or belongs to another epoch"
}
if (-not $tdr.faultSurfaceBound) {
    throw "destructive dispatch did not bind the exact worker-owned retained surface"
}
if (-not $tdr.staleFrameRejected) { throw "pre-loss frame survived the recovery boundary" }
if ($tdr.signalCount -ne 1 -or $tdr.pendingInitially -le 0 -or
    -not $tdr.abandonAttempted -or -not $tdr.retainedSurfaceReleased) {
    throw "TDR retirement evidence did not prove token-authorized release of the retained surface"
}
if ($tdr.deadFenceWaits -ne 0 -or $tdr.pendingFinally -ne 0) {
    throw "recovery waited on a dead fence or leaked retained surfaces"
}
if (-not $tdr.workerRecoveryExercised -or -not $tdr.workerRealLossToken) {
    throw "real PlaybackWorker recovery was not exercised"
}
if ($tdr.workerGenerationAfter -le $tdr.workerGenerationBefore -or
    -not $tdr.workerStaleFrameRejected -or -not $tdr.workerCacheRecoveredToCpu -or
    -not $tdr.workerRetainedSurfaceReleased -or $tdr.workerAbandonedRetains -le 0) {
    throw "PlaybackWorker retained stale GPU state across removal"
}
if (-not $tdr.workerOutputResumed -or -not $tdr.workerCoherentState -or
    -not $tdr.recoveryComplete -or $tdr.workerRecoveryMs -gt 10000) {
    throw "PlaybackWorker did not resume coherent output within 10 seconds"
}

Write-Host "Real GPU removal recovery evidence passed"
