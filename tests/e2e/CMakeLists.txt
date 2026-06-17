# End-to-end recording tests: a headless harness driving the real engine
# against synthetic FFmpeg streams. These exercise capture -> resample -> mux
# end to end and are the regression home for media-pipeline bugs.

qt_add_executable(record_harness record_harness.cpp)
target_link_libraries(record_harness PRIVATE
    Qt6::Core olr_test_engine olr_warnings olr_sanitize)

# Headless playback harness: drives the real PlaybackWorker against a recorded
# fixture and prints its telemetry counters. The storm-regression gate.
qt_add_executable(play_harness play_harness.cpp)
target_link_libraries(play_harness PRIVATE
    Qt6::Core Qt6::Multimedia Qt6::Gui olr_test_playback olr_warnings olr_sanitize)

# Headless multi-source sync-measurement harness: records N synthetic sources
# into an N-view MKV for the report-only frame-sync scoreboard (run_sync_e2e.sh).
qt_add_executable(sync_harness sync_harness.cpp)
target_link_libraries(sync_harness PRIVATE
    Qt6::Core olr_test_engine olr_warnings olr_sanitize)

set(_driver "${CMAKE_CURRENT_SOURCE_DIR}/run_record_e2e.sh")
set(_pb_driver "${CMAKE_CURRENT_SOURCE_DIR}/run_playback_e2e.sh")

# Baseline happy path: stereo input. The driver bridges the ffmpeg producer over
# UDP->SRT (srt-live-transmit) and records via the native srt:// ingest; the 3rd
# arg is the SRT listener port (the UDP producer port is derived as SRT+1, so the
# two cases use disjoint port pairs: stereo {23456,23457}, mono {23458,23459}).
add_test(NAME e2e_record_stereo
    COMMAND bash "${_driver}" "$<TARGET_FILE:record_harness>" stereo 23456)

# Regression for the mono-audio SIGBUS crash (commit 3c7d9b4): a mono source
# must record without crashing and be rematrixed up to stereo.
add_test(NAME e2e_record_mono
    COMMAND bash "${_driver}" "$<TARGET_FILE:record_harness>" mono 23458)

set_tests_properties(e2e_record_stereo e2e_record_mono PROPERTIES
    LABELS "e2e"
    TIMEOUT 120
    RUN_SERIAL TRUE)  # they bind UDP/SRT ports + drive ffmpeg; don't overlap

# Playback acceptance matrix (spec §11.5) as CTest cases. Each records a fresh
# multi-track fixture (over the native srt:// ingest, via an ffmpeg->UDP->SRT
# bridge in run_playback_e2e.sh), drives the real PlaybackWorker through one
# scenario, and asserts the §11.2 counter thresholds (see run_playback_e2e.sh for
# the per-scenario numbers). They RUN_SERIAL (each binds ports + drives ffmpeg)
# and are additionally handed a UNIQUE SRT port (5th arg; the UDP producer port
# is SRT+1) so a lingering socket from one case can never bleed into the next.
# Because each gate binds BOTH the SRT port and the UDP port (SRT+1), the ports
# are spaced by 2 (23464,23466,...,23478) so no gate's SRT/UDP pair overlaps the
# next — and the whole block stays disjoint from the record (23456-23459), sync
# (23480-23489), av-sync (23492) and native (>=23550) port ranges.
#
# e2e_play_storm is the §11.6 headline gate: 2-view 1x steady playback must
# report 0 repositions (the old decode loop stormed ~90 hard-seeks/s) and run
# the audio path (audioPushes>0). e2e_play_4view confirms the multi-track decode
# path (4 video tracks) also holds repositions at 0.
add_test(NAME e2e_play_storm
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" play1x 2 23464)
add_test(NAME e2e_play_seekplay
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" seekplay 2 23466)
add_test(NAME e2e_play_stepscrub
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" stepscrub 2 23468)
add_test(NAME e2e_play_reverse
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" reverse 2 23470)
add_test(NAME e2e_play_sliderscrub
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" sliderscrub 2 23472)
add_test(NAME e2e_play_liveedge
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" liveedge 2 23474)
# 4-view multi-track decode path: the "stable" case even under the old code —
# the new scheduler must also hold it at 0 repositions.
add_test(NAME e2e_play_4view
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" play1x 4 23476)
add_test(NAME e2e_play_latency
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" latency 2 23478)

set_tests_properties(
    e2e_play_storm e2e_play_seekplay e2e_play_stepscrub e2e_play_reverse
    e2e_play_sliderscrub e2e_play_liveedge e2e_play_4view e2e_play_latency
    PROPERTIES
    LABELS "e2e"
    TIMEOUT 180
    RUN_SERIAL TRUE)

# --- Report-only frame-sync scoreboard (NON-GATING) -------------------------
# These drive sync_harness through synthetic synchronized sources and print
# measured inter-camera offset / drift / lip-sync numbers. They ALWAYS exit 0
# (diagnostic, not a gate) and carry the distinct "sync-report" label so the
# gating e2e selection and the sanitizer build-target list are unaffected.
# Run on demand with:  ctest -L sync-report
set(_sync_driver "${CMAKE_CURRENT_SOURCE_DIR}/run_sync_e2e.sh")
add_test(NAME sync_intercam_matched
    COMMAND bash "${_sync_driver}" "$<TARGET_FILE:sync_harness>" intercam_matched 23480)
add_test(NAME sync_intercam_skew
    COMMAND bash "${_sync_driver}" "$<TARGET_FILE:sync_harness>" intercam_skew 23482)
add_test(NAME sync_drift_2997
    COMMAND bash "${_sync_driver}" "$<TARGET_FILE:sync_harness>" drift_2997 23484)
add_test(NAME sync_lipsync
    COMMAND bash "${_sync_driver}" "$<TARGET_FILE:sync_harness>" lipsync 23486)
add_test(NAME sync_intercam_trim
    COMMAND bash "${_sync_driver}" "$<TARGET_FILE:sync_harness>" intercam_trim 23488)

set_tests_properties(
    sync_intercam_matched sync_intercam_skew sync_drift_2997 sync_lipsync sync_intercam_trim
    PROPERTIES LABELS "sync-report" TIMEOUT 180 RUN_SERIAL TRUE)

# AUD-4 A/V lip-sync GATE (native SRT ingest, via the UDP->SRT bridge): the shared
# anchor must keep audio within EBU R37 (-40..+60 ms). Pre-AUD-4 this measured
# ~+63 ms and FAILS. Base UDP 23492 (SRT listener bridged at 23592).
add_test(NAME e2e_av_lipsync
    COMMAND bash "${_sync_driver}" "$<TARGET_FILE:sync_harness>" lipsync 23492)
set_tests_properties(e2e_av_lipsync PROPERTIES
    LABELS "av-sync" TIMEOUT 120 RUN_SERIAL TRUE
    ENVIRONMENT "OLR_AV_SYNC_GATE=1")

# --- Local-only native SRT e2e (NOT in CI) ----------------------------------
# Prove the real engine ingests srt:// over the native SRT path (the default,
# and only, SRT ingest). The producer side uses local ffmpeg + srt-live-transmit;
# no SRT-enabled ffmpeg build is needed. The native-* labels keep these out of
# CI's e2e selection. Run locally with: ctest -L native-apple-ingest (macOS).
function(olr_add_native_srt_e2e_tests native_label)
    add_test(NAME e2e_native_srt_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_smoke.sh" "$<TARGET_FILE:record_harness>" 23601)
    add_test(NAME e2e_native_srt_4cam
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_4cam.sh" "$<TARGET_FILE:sync_harness>" 23610)
    add_test(NAME e2e_native_srt_sync
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_sync.sh" "$<TARGET_FILE:sync_harness>" 23620)
    add_test(NAME e2e_native_srt_trim
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_trim.sh" "$<TARGET_FILE:sync_harness>" 23630)
    add_test(NAME e2e_native_srt_connect
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_connect.sh" "$<TARGET_FILE:sync_harness>" 23640)
    set_tests_properties(
        e2e_native_srt_smoke e2e_native_srt_4cam e2e_native_srt_sync
        e2e_native_srt_trim e2e_native_srt_connect
        PROPERTIES
        LABELS "${native_label}"
        TIMEOUT 180
        RUN_SERIAL TRUE)
endfunction()

if(APPLE)
    olr_add_native_srt_e2e_tests("native-apple-ingest")

    # Phase 2c-a: mid-recording disconnect/reconnect survival on the NATIVE SRT
    # ingest. Kill a victim bridge mid-record, restart it, and prove the victim
    # reconnects + resumes real content WHILE the control source stays fully
    # isolated (no disconnect, no content loss). Isolation holds on the native
    # path (each source owns its libsrt socket); the legacy ffmpeg ingest couples
    # sources (a dead source's reconnect churn starves the others — see SRT_README),
    # so this gate runs native-only. Base port 23550; longer record => TIMEOUT 240.
    add_test(NAME e2e_native_srt_reconnect
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_reconnect.sh" "$<TARGET_FILE:sync_harness>" 23550)
    set_tests_properties(e2e_native_srt_reconnect PROPERTIES
        LABELS "native-apple-ingest"
        TIMEOUT 240
        RUN_SERIAL TRUE)

    # Phase 2c-b: SRT packet-loss recovery on the native ingest. A lossy_udp_relay.py
    # drops downstream SRT DATA on the link; assert ARQ recovers moderate loss (no
    # content loss, retransmits happened) and degrades under heavy loss. Base 23660.
    add_test(NAME e2e_native_srt_loss
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_loss.sh" "$<TARGET_FILE:sync_harness>" 23660)
    set_tests_properties(e2e_native_srt_loss PROPERTIES
        LABELS "native-apple-ingest"
        TIMEOUT 180
        RUN_SERIAL TRUE)

    # Phase 2c-c: long-run STABILITY SOAK (not a drift detector — loopback clocks
    # match so the flash-PTS slope is ~1.0 by construction). A 30s native-SRT record
    # must not crash/hang and must keep delivering content. Base 23670.
    add_test(NAME e2e_native_srt_soak
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_soak.sh" "$<TARGET_FILE:sync_harness>" 23670)
    set_tests_properties(e2e_native_srt_soak PROPERTIES
        LABELS "native-apple-ingest"
        TIMEOUT 120
        RUN_SERIAL TRUE)

    # Phase 2d: packet-REORDERING recovery. The lossy_udp_relay.py reorder mode
    # delays downstream SRT DATA packets to reorder the link; assert SRT's TSBPD
    # re-orders them (content in-order, pktRcvDropTotal==0) and the relay actually
    # reordered (reordered>0). Base 23690.
    add_test(NAME e2e_native_srt_jitter
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_jitter.sh" "$<TARGET_FILE:sync_harness>" 23690)
    set_tests_properties(e2e_native_srt_jitter PROPERTIES
        LABELS "native-apple-ingest"
        TIMEOUT 120
        RUN_SERIAL TRUE)

    # Phase 2d: multi-source loss ISOLATION. Two independent native sources, each own
    # relay; src1 drops 12%, src0 clean. Assert src1 recovers (pktRcvDropTotal=0) AND
    # src0 fully intact (no drops/gap) — loss on one must not perturb the other. Base 23680.
    add_test(NAME e2e_native_srt_loss_multi
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_loss_multi.sh" "$<TARGET_FILE:sync_harness>" 23680)
    set_tests_properties(e2e_native_srt_loss_multi PROPERTIES
        LABELS "native-apple-ingest"
        TIMEOUT 120
        RUN_SERIAL TRUE)

    # JIT-5: the SRT loss-telemetry DATA PATH behind the connection-status UI.
    # native ingest -> reportStats -> StreamWorker -> ReplayManager::sourceStatsUpdated;
    # sync_harness --report-stats reads it. Assert real telemetry (retrans>0, drop==0)
    # under relayed loss, ~no retrans on a clean link. Base 23700.
    add_test(NAME e2e_native_srt_ui_stats
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_ui_stats.sh" "$<TARGET_FILE:sync_harness>" 23700)
    set_tests_properties(e2e_native_srt_ui_stats PROPERTIES
        LABELS "native-apple-ingest"
        TIMEOUT 120
        RUN_SERIAL TRUE)

    # AUD-4: A/V lip-sync over the PCR-anchored native SRT ingest (EBU R37). Base 23710.
    add_test(NAME e2e_native_srt_lipsync
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_lipsync.sh" "$<TARGET_FILE:sync_harness>" 23710)
    set_tests_properties(e2e_native_srt_lipsync PROPERTIES
        LABELS "native-apple-ingest" TIMEOUT 120 RUN_SERIAL TRUE)
elseif(WIN32)
    olr_add_native_srt_e2e_tests("native-windows-ingest")
    add_test(NAME e2e_native_srt_hevc_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_hevc_smoke.sh" "$<TARGET_FILE:record_harness>" 23701)
    set_tests_properties(e2e_native_srt_hevc_smoke PROPERTIES
        LABELS "native-windows-hevc"
        TIMEOUT 180
        RUN_SERIAL TRUE)
endif()

# --- Local-only native RTMP e2e (NOT in CI) --------------------------------
# This test uses a deterministic Python RTMP fixture server and requires the
# native backend marker in the harness log, so a successful FFmpeg fallback is
# still a failure. Run locally with: ctest -L native-rtmp
if(APPLE)
    add_test(NAME e2e_native_rtmp_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_rtmp_smoke.sh" "$<TARGET_FILE:record_harness>" 23701)
    add_test(NAME e2e_native_rtmp_hevc_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_rtmp_hevc_smoke.sh" "$<TARGET_FILE:record_harness>" 23770)
    add_test(NAME e2e_native_rtmps_hevc_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_rtmps_hevc_smoke.sh" "$<TARGET_FILE:record_harness>" 23771)
    add_test(NAME e2e_native_rtmp_unsupported
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_rtmp_unsupported.sh" "$<TARGET_FILE:record_harness>" 23760)
    add_test(NAME e2e_native_rtmp_reconnect
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_rtmp_reconnect.sh" "$<TARGET_FILE:record_harness>" 23780)
    add_test(NAME e2e_native_rtmp_interop
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_rtmp_interop.sh" "$<TARGET_FILE:record_harness>")
    add_test(NAME e2e_native_rtmp_soak
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_rtmp_soak.sh" "$<TARGET_FILE:record_harness>" 23790)
    # The RTMP stats DATA PATH behind the connection-status UI (twin of
    # e2e_native_srt_ui_stats). native RTMP ingest -> reportStats -> StreamWorker ->
    # ReplayManager::sourceStatsUpdated; sync_harness --report-stats reads it. Assert a
    # kind=rtmp line exists, bytes>0, decodefail==0, and the native backend marker. Base 23800.
    add_test(NAME e2e_native_rtmp_ui_stats
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_rtmp_ui_stats.sh" "$<TARGET_FILE:sync_harness>" 23800)
    set_tests_properties(e2e_native_rtmp_smoke PROPERTIES
        LABELS "native-rtmp"
        TIMEOUT 180
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77)
    set_tests_properties(e2e_native_rtmp_hevc_smoke PROPERTIES
        LABELS "native-rtmp"
        TIMEOUT 180
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77)
    set_tests_properties(e2e_native_rtmps_hevc_smoke PROPERTIES
        LABELS "native-rtmp"
        TIMEOUT 180
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77)
    set_tests_properties(e2e_native_rtmp_unsupported PROPERTIES
        LABELS "native-rtmp"
        TIMEOUT 180
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77)
    set_tests_properties(e2e_native_rtmp_reconnect PROPERTIES
        LABELS "native-rtmp"
        TIMEOUT 180
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77)
    set_tests_properties(e2e_native_rtmp_interop PROPERTIES
        LABELS "native-rtmp"
        TIMEOUT 600
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77)
    set_tests_properties(e2e_native_rtmp_soak PROPERTIES
        LABELS "native-rtmp-soak"
        TIMEOUT 3900
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77)
    set_tests_properties(e2e_native_rtmp_ui_stats PROPERTIES
        LABELS "native-rtmp"
        TIMEOUT 180
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77)
endif()
