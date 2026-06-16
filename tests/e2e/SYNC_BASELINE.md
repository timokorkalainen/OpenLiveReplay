# Sync Scoreboard — Baseline (pre-fix)

Numbers produced by `run_sync_e2e.sh` on `main` before any frame-sync fixes.
Regenerate locally with, e.g.:
`bash tests/e2e/run_sync_e2e.sh <sync_harness> intercam_skew 23482 --write-baseline`
Report-only; these are diagnostics, not pass/fail thresholds.

## 2026-06-15 (branch feat/sync-measurement-harness)
[sync] scenario=intercam_matched flashes_paired=6 intercam_offset_ms: mean=0.0 max=0.0
[sync] scenario=intercam_skew flashes_paired=6 intercam_offset_ms: mean=-267.0 stdev=0.0 (D_injected=250)
[sync] scenario=drift_2997 flashes=58 slope=1.000220 drift_ppm=220 drift_frames_slip=0.39
[sync] scenario=lipsync pairs=7 av_offset_ms: mean=63.3 max=71.7 (EBU_R37_band=+40/-60)
[sync] scenario=intercam_trim untrimmed_ms=0.0 trimmed_ms=-266.0 (trim_applied=250; delay => trimmed ≈ untrimmed − 250)

## 2026-06-16 (branch feat/srt-aud4-anchor) — post-AUD-4 single shared A/V anchor
# The +63.3 ms lipsync lag (independent audio anchor) collapses to ~0: audio and
# video now share one anchor per source. Measured across runs: ffmpeg/UDP path
# mean ~ -7..+9 ms; PCR-anchored native SRT path mean ~ -24..+3 ms. Both are now
# GATED within EBU R37 (-40..+60): e2e_av_lipsync (av-sync), e2e_native_srt_lipsync
# (native-apple-ingest). Also fixed a pre-existing native AAC decoder bug that
# dropped ~99% of decoded audio (input-proc EOS); the lipsync marker exposed it.
[sync] scenario=lipsync pairs=7 av_offset_ms: mean=6.3 max=14.7 (EBU_R37_band=+40/-60)  # ffmpeg path, post-AUD-4
