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

## 2026-06-17 (branch feat/rational-fps) — P1 rational frame rate (FRAC-1/2, MUX-2)
# The frame rate is now an exact rational `FrameRate {num,den}` end-to-end in the
# recording engine (was integer `fps`). Fractional broadcast rates — 29.97 =
# 30000/1001, 59.94 = 60000/1001, 23.976 = 24000/1001 — record with correct frame
# timing and a correct container rate instead of being forced to integer 30/60.
#   - The frame<->ms math is centralized in `recorder_engine/framerate.h`
#     (msForFrame / frameForMs / samplesPerFrame); the encoder time_base is
#     {den,num} and framerate {num,den}; the muxer sets avg/r_frame_rate = {num,den},
#     which Matroska carries as the track DefaultDuration.
#   - Settings persist `fpsNum`/`fpsDen` (a legacy `"fps":<int>` loads as {n,1}); the
#     UI picks a rate from a preset list (23.976/24/25/29.97/30/50/59.94/60); the
#     e2e harnesses accept a rational `--fps` ("29.97", "30000/1001").
#   - GATED proof: `e2e_record_2997` (e2e label) records a true 29.97 and asserts the
#     output avg_frame_rate is in (29.9, 30.0) — 29.97 reads 30000/1001 and PASSES,
#     integer 30.0 FAILS (teeth-verified bidirectionally). Integer 30/60 recordings
#     are byte-identical (encoder {1,30}/{30,1}), so all prior gates stay green.
#   - DEFERRED (separate roadmap items): drop-frame timecode (TC-2) and rational
#     playback stepping — PlaybackTransport still uses the rounded integer rate, so
#     step-by-frame on a 29.97 file is ~0.1% off.
# Note on drift_2997 above: that report-only scenario still records the 29.97 source
# at session --fps 30 ON PURPOSE — it measures the source-vs-session mismatch
# (slope 1.000220, 220 ppm). P1 lets a session match the source (record at 29.97);
# the discriminating proof of that capability is the gated e2e_record_2997.
