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

if (-not $env:OLR_GPU_FAULT_EVIDENCE) {
    $env:OLR_GPU_FAULT_EVIDENCE = Join-Path (Get-Location) "windows-gpu-fault-evidence.jsonl"
}
Remove-Item -LiteralPath $env:OLR_GPU_FAULT_EVIDENCE -ErrorAction SilentlyContinue

ctest --test-dir $BuildDir -R "^win_gpu_fault_real$" --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$records = Get-Content -LiteralPath $env:OLR_GPU_FAULT_EVIDENCE | ForEach-Object {
    $_ | ConvertFrom-Json
}
$capability = $records | Where-Object { $_.mode -eq "capability" } | Select-Object -Last 1
if ($capability -and -not $capability.supported) {
    Write-Host "SKIP: $($capability.reason)"
    exit 0
}
$tdr = $records | Where-Object { $_.mode -eq "trigger-tdr" } | Select-Object -Last 1
if (-not $tdr) { throw "missing trigger-tdr evidence" }
if ($tdr.removedHresult -ge 0) { throw "device removal HRESULT was not a failure" }
if ($tdr.generationAfter -le $tdr.generationBefore) { throw "GPU generation did not advance" }
if (-not $tdr.realLossToken -or $tdr.tokenGeneration -ne $tdr.generationAfter) {
    throw "real-loss token is missing or belongs to another epoch"
}
if (-not $tdr.staleFrameRejected) { throw "pre-loss frame survived the recovery boundary" }
if ($tdr.signalCount -ne 1 -or $tdr.pendingInitially -le 0 -or $tdr.releasedRetains -le 0) {
    throw "TDR retirement evidence did not measure one signal and a released retain"
}
if ($tdr.deadFenceWaits -ne 0 -or $tdr.pendingFinally -ne 0) {
    throw "recovery waited on a dead fence or leaked retained surfaces"
}

Write-Host "Real GPU removal recovery evidence passed"
