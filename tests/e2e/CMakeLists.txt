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

# Baseline happy path: stereo input.
add_test(NAME e2e_record_stereo
    COMMAND bash "${_driver}" "$<TARGET_FILE:record_harness>" stereo 23456)

# Regression for the mono-audio SIGBUS crash (commit 3c7d9b4): a mono source
# must record without crashing and be rematrixed up to stereo.
add_test(NAME e2e_record_mono
    COMMAND bash "${_driver}" "$<TARGET_FILE:record_harness>" mono 23457)

set_tests_properties(e2e_record_stereo e2e_record_mono PROPERTIES
    LABELS "e2e"
    TIMEOUT 120
    RUN_SERIAL TRUE)  # they bind UDP ports / drive ffmpeg; don't overlap

# Playback acceptance matrix (spec §11.5) as CTest cases. Each records a fresh
# multi-track fixture, drives the real PlaybackWorker through one scenario, and
# asserts the §11.2 counter thresholds (see run_playback_e2e.sh for the per-
# scenario numbers). They RUN_SERIAL (each binds a UDP port + drives ffmpeg) and
# are additionally handed a UNIQUE port (5th arg = base 23470 + index) so a
# lingering socket from one case can never bleed into the next.
#
# e2e_play_storm is the §11.6 headline gate: 2-view 1x steady playback must
# report 0 repositions (the old decode loop stormed ~90 hard-seeks/s) and run
# the audio path (audioPushes>0). e2e_play_4view confirms the multi-track decode
# path (4 video tracks) also holds repositions at 0.
add_test(NAME e2e_play_storm
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" play1x 2 23470)
add_test(NAME e2e_play_seekplay
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" seekplay 2 23471)
add_test(NAME e2e_play_stepscrub
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" stepscrub 2 23472)
add_test(NAME e2e_play_reverse
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" reverse 2 23473)
add_test(NAME e2e_play_sliderscrub
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" sliderscrub 2 23474)
add_test(NAME e2e_play_liveedge
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" liveedge 2 23475)
# 4-view multi-track decode path: the "stable" case even under the old code —
# the new scheduler must also hold it at 0 repositions.
add_test(NAME e2e_play_4view
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" play1x 4 23476)
add_test(NAME e2e_play_latency
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" latency 2 23491)

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

# --- Local-only SRT e2e (NOT in CI) -----------------------------------------
# Proves the real engine ingests srt://. Requires a build configured with
# -DOLR_FFMPEG_SRT_PREFIX (an SRT-enabled ffmpeg); under the default brew ffmpeg
# the engine cannot open srt:// and this fails — by design. The distinct "srt"
# label keeps it out of CI's e2e selection. Run locally with: ctest -L srt
add_test(NAME e2e_srt_smoke
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_smoke.sh" "$<TARGET_FILE:record_harness>" 23501)
set_tests_properties(e2e_srt_smoke PROPERTIES LABELS "srt" TIMEOUT 120 RUN_SERIAL TRUE)

add_test(NAME e2e_srt_4cam
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_4cam.sh" "$<TARGET_FILE:sync_harness>" 23510)
set_tests_properties(e2e_srt_4cam PROPERTIES LABELS "srt" TIMEOUT 180 RUN_SERIAL TRUE)

# Phase 2b: inter-camera sync over 4 real SRT streams (generous bound + per-view
# flash-count content gate). Base port 23520.
add_test(NAME e2e_srt_sync
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_sync.sh" "$<TARGET_FILE:sync_harness>" 23520)
set_tests_properties(e2e_srt_sync PROPERTIES LABELS "srt" TIMEOUT 180 RUN_SERIAL TRUE)

# Phase 2b: per-source trim (#28) over real SRT — +T on view1 delays it by ~T.
# Base port 23530.
add_test(NAME e2e_srt_trim
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_trim.sh" "$<TARGET_FILE:sync_harness>" 23530)
set_tests_properties(e2e_srt_trim PROPERTIES LABELS "srt" TIMEOUT 180 RUN_SERIAL TRUE)

# Phase 2b: per-source connection-status (#24) over real SRT — 4 live => 4,
# 1 dead port => 3. Base port 23540.
add_test(NAME e2e_srt_connect
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_connect.sh" "$<TARGET_FILE:sync_harness>" 23540)
set_tests_properties(e2e_srt_connect PROPERTIES LABELS "srt" TIMEOUT 180 RUN_SERIAL TRUE)

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
        RUN_SERIAL TRUE
        ENVIRONMENT "OLR_NATIVE_SRT=1")
endfunction()

if(APPLE)
    olr_add_native_srt_e2e_tests("native-apple-ingest")
elseif(WIN32)
    olr_add_native_srt_e2e_tests("native-windows-ingest")
    add_test(NAME e2e_native_srt_hevc_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_hevc_smoke.sh" "$<TARGET_FILE:record_harness>" 23701)
    set_tests_properties(e2e_native_srt_hevc_smoke PROPERTIES
        LABELS "native-windows-hevc"
        TIMEOUT 180
        RUN_SERIAL TRUE
        ENVIRONMENT "OLR_NATIVE_SRT=1")
endif()
