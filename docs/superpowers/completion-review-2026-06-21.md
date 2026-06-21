# Plans and Specs Completion Review - 2026-06-21

Reviewed commit: `4696704` (`main`)

Scope at review time: `docs/superpowers/plans/*.md` and `docs/superpowers/specs/*.md`, cross-checked statically against source files, tests, CMake registrations, scripts, and docs. No build or test suite was run for this review.

## Summary

- Plans reviewed: 45 files.
- Specs reviewed: 32 files.
- Plan checkbox state is not reliable completion data: the plan files contain 1537 unchecked boxes and 0 checked boxes, even though most plans have landed.
- Most implementation lanes are present in code/tests, but a few planned items are still missing or should be explicitly rescoped.

## Not Implemented Yet

1. OpenAPI telemetry E2E harness

   Plan/spec: `2026-06-15-openapi-feed-settings-telemetry`.

   Present: core feed settings import, SSE client/parser, mux telemetry tracks, timeline reader, UI integration, and unit tests.

   Missing: the planned telemetry E2E harness files are not present:
   - `tests/e2e/telemetry_harness.cpp`
   - `tests/e2e/run_telemetry_e2e.sh`

2. RTMP SRT-parity multi-source gates

   Reviewed item: native RTMP broadcast ingest.

   Present: core native RTMP/RTMPS, HEVC/E-RTMP, unsupported-profile rejection, reconnect/stall behavior, optional real-server interop, opt-in soak, and UI stats gates.

   Missing: the RTMP equivalents of the SRT-style multi-source gates are not present:
   - `tests/e2e/run_rtmp_4cam.sh`
   - `tests/e2e/run_rtmp_sync.sh`
   - `tests/e2e/run_rtmp_trim.sh`
   - `tests/e2e/run_rtmp_connect.sh`

3. Tier-1 audio drift servo

   Plan/spec: `2026-06-20-tier1-broadcast-readiness`.

   Present: T1.2, T1.3/T1.6, T1.4, T1.5, and H.264 follow-up work are present by static inspection.

   Missing: the T1.1 audio drift servo described in the plan does not match the code:
   - no `m_currentSourcePpm`
   - `writeAudioForTick` still uses `srcAdvance = n`
   - `e2e_framesync_drift_avsync` is not registered in `tests/e2e/CMakeLists.txt`

## Maintained Sources

Use the current code and live docs below instead of the old planning notes:

- Native SRT: `NativeSrtIngestSession`, `MpegTsParser`, `H26xAccessUnit`, `NativeVideoDecoder`, `NativeAacDecoder`, `tests/e2e/CMakeLists.txt`, and `tests/e2e/SRT_README.md`.
- SRT local gates: `run_srt_4cam.sh`, `run_srt_sync.sh`, `run_srt_trim.sh`, `run_srt_connect.sh`, `run_srt_reconnect.sh`, `run_srt_loss.sh`, `run_srt_soak.sh`, `run_srt_loss_multi.sh`, `run_srt_jitter.sh`, `run_srt_ui_stats.sh`, and `run_srt_lipsync.sh`.
- Native RTMP: `NativeRtmpIngestSession`, `rtmpprotocol`, the `native-rtmp` CTest registrations, `tests/README.md`, and `tests/e2e/SRT_README.md`. The missing RTMP multi-source gates are listed above.
- Native-only ingest and Windows runtime follow-ups: `docs/superpowers/specs/2026-06-17-native-only-ingest-design.md`, `docs/native-ingest-workstream-remaining.md`, and `docs/windows-aac-rtmp-smoke-runbook.md`.
- Delivery push gate: `.githooks/pre-push`, the four `delivery-gate` labels in `tests/e2e/CMakeLists.txt`, the `OLR_E2E_CLIP_SECONDS` overrides in the four delivery scripts, and `tests/README.md`.

NDI, H.264 hardware, PTP, and several transport gates remain hardware/runtime dependent. This review verifies registrations and code presence, not successful local execution on every hardware/runtime combination.

## Recommendation

Use this review as the source of truth for remaining work instead of the unchecked boxes in historical implementation plans. If one of the missing items is intentionally out of scope, update the corresponding plan/spec to say so explicitly.
