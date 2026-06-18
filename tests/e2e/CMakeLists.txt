# End-to-end recording tests: a headless harness driving the real engine
# against synthetic FFmpeg streams. These exercise capture -> resample -> mux
# end to end and are the regression home for media-pipeline bugs.

qt_add_executable(record_harness record_harness.cpp)
target_link_libraries(record_harness PRIVATE
    Qt6::Core olr_test_engine olr_warnings olr_sanitize)

set(OLR_E2E_BASH bash)
set(_olr_windows_tool_env)
if(WIN32)
    find_program(OLR_GIT_BASH
        NAMES bash.exe bash
        HINTS "C:/Program Files/Git/bin" "C:/Program Files/Git/usr/bin")
    if(OLR_GIT_BASH)
        set(OLR_E2E_BASH "${OLR_GIT_BASH}")
    endif()
    get_filename_component(_olr_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    list(APPEND _olr_windows_tool_env
        "OLR_FFMPEG_ROOT=${OLR_FFMPEG_ROOT}"
        "OLR_SRT_ROOT=${OLR_SRT_ROOT}"
        "OLR_MINGW_BIN=${_olr_mingw_bin}"
        "OLR_QT_BIN=$<TARGET_FILE_DIR:Qt6::Core>")
endif()

# Headless playback harness: drives the real PlaybackWorker against a recorded
# fixture and prints its telemetry counters. The storm-regression gate.
qt_add_executable(play_harness play_harness.cpp)
target_link_libraries(play_harness PRIVATE
    Qt6::Core Qt6::Multimedia Qt6::Gui olr_test_playback olr_warnings olr_sanitize)

# Headless output-bus continuity soak: drives a real OutputRuntime against a static
# synthetic cache and verifies frame/audio continuity + cadence over wall-clock time.
qt_add_executable(soak_harness soak_harness.cpp)
target_link_libraries(soak_harness PRIVATE
    Qt6::Core Qt6::Multimedia Qt6::Gui olr_test_playback olr_warnings olr_sanitize)

add_test(NAME e2e_output_soak
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_output_soak.sh" "$<TARGET_FILE:soak_harness>")
set_tests_properties(e2e_output_soak PROPERTIES
    LABELS "output-soak"
    TIMEOUT 600
    RUN_SERIAL TRUE
    ENVIRONMENT "OLR_SOAK_SECONDS=5")

# Headless multi-source sync-measurement harness: records N synthetic sources
# into an N-view MKV for the report-only frame-sync scoreboard (run_sync_e2e.sh).
qt_add_executable(sync_harness sync_harness.cpp)
target_link_libraries(sync_harness PRIVATE
    Qt6::Core olr_test_engine olr_warnings olr_sanitize)

# Runtime-loaded local NDI sender used by the native NDI ingest smoke. This
# deliberately uses the app's own NdiOutputSink instead of the SDK-only marker
# fixture so a machine with only the NDI runtime can still run the e2e gate.
qt_add_executable(ndi_runtime_sender ndi_runtime_sender.cpp)
target_link_libraries(ndi_runtime_sender PRIVATE
    Qt6::Core olr_test_playback olr_warnings olr_sanitize)

# NDI receiver probe + tier-(a) sender for the rung-5 output validation lane. Both
# runtime-load the NDI library (no SDK at build time) and link the shared marker/analysis
# modules.
qt_add_executable(ndi_recv_probe
    ndi_recv_probe.cpp
    ndi_output_marker.cpp
    ndi_recv_analysis.cpp)
target_include_directories(ndi_recv_probe PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(ndi_recv_probe PRIVATE Qt6::Core olr_warnings olr_sanitize)

qt_add_executable(ndi_output_sender
    ndi_output_sender.cpp
    ndi_output_marker.cpp)
target_include_directories(ndi_output_sender PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(ndi_output_sender PRIVATE
    Qt6::Core Qt6::Multimedia Qt6::Gui olr_test_playback olr_warnings olr_sanitize)

# Tier (b): writes the marker as raw YUV420P+S16 for muxing into a playback MKV.
qt_add_executable(ndi_marker_mkv_source
    ndi_marker_mkv_source.cpp
    ndi_output_marker.cpp)
target_include_directories(ndi_marker_mkv_source PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(ndi_marker_mkv_source PRIVATE Qt6::Core olr_warnings olr_sanitize)

add_test(NAME e2e_ndi_output
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_ndi_output_e2e.sh"
        "$<TARGET_FILE:ndi_output_sender>" "$<TARGET_FILE:ndi_recv_probe>")
set_tests_properties(e2e_ndi_output PROPERTIES
    LABELS "ndi-output"
    TIMEOUT 120
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77)

add_test(NAME e2e_ndi_playback
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_ndi_playback_e2e.sh"
        "$<TARGET_FILE:ndi_marker_mkv_source>" "$<TARGET_FILE:play_harness>"
        "$<TARGET_FILE:ndi_recv_probe>")
set_tests_properties(e2e_ndi_playback PROPERTIES
    LABELS "ndi-output"
    TIMEOUT 180
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77)

if(APPLE OR WIN32)
    # Native SRT handshake harness: proves NativeSrtIngestSession sets SRTO_STREAMID
    # before connect by reading the stream ID from a real accepted SRT socket.
    qt_add_executable(srt_streamid_harness srt_streamid_harness.cpp)
    target_link_libraries(srt_streamid_harness PRIVATE
        Qt6::Core olr_test_engine olr_warnings olr_sanitize)
    if(APPLE)
        target_include_directories(srt_streamid_harness PRIVATE "${OLR_BREW_PREFIX}/opt/srt/include")
        target_link_directories(srt_streamid_harness PRIVATE "${OLR_BREW_PREFIX}/opt/srt/lib")
        target_link_libraries(srt_streamid_harness PRIVATE srt)
    elseif(WIN32)
        target_include_directories(srt_streamid_harness PRIVATE "${OLR_SRT_INCLUDE_DIR}")
        target_link_libraries(srt_streamid_harness PRIVATE ws2_32 "${OLR_SRT_LIBRARY}")
    endif()
endif()

set(_olr_ndi_default_sdk "")
if(APPLE AND EXISTS "/Library/NDI SDK for Apple/include/Processing.NDI.Lib.h")
    set(_olr_ndi_default_sdk "/Library/NDI SDK for Apple")
endif()
set(OLR_NDI_SDK_DIR "${_olr_ndi_default_sdk}" CACHE PATH
    "Optional NDI SDK root used to build the local framesync NDI marker sender")
if(OLR_NDI_SDK_DIR)
    find_path(OLR_NDI_INCLUDE_DIR
        NAMES Processing.NDI.Lib.h
        HINTS "${OLR_NDI_SDK_DIR}/include"
        NO_DEFAULT_PATH)
    find_library(OLR_NDI_LIBRARY
        NAMES ndi libndi
        HINTS
            "${OLR_NDI_SDK_DIR}/lib/macOS"
            "${OLR_NDI_SDK_DIR}/lib"
        NO_DEFAULT_PATH)
    if(OLR_NDI_INCLUDE_DIR AND OLR_NDI_LIBRARY)
        add_executable(ndi_marker_sender
            ndi_marker_sender.cpp
            ndi_marker_pattern.cpp)
        target_include_directories(ndi_marker_sender PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${OLR_NDI_INCLUDE_DIR}")
        target_link_libraries(ndi_marker_sender PRIVATE
            Qt6::Core
            "${OLR_NDI_LIBRARY}"
            olr_warnings
            olr_sanitize)
        get_filename_component(_olr_ndi_library_dir "${OLR_NDI_LIBRARY}" DIRECTORY)
        set_target_properties(ndi_marker_sender PROPERTIES
            BUILD_RPATH "${_olr_ndi_library_dir}")
    else()
        message(STATUS "NDI marker sender disabled: NDI SDK headers/library not found")
    endif()
endif()

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

# Hardware H.264 recording smoke: records via the H.264Hardware codec path
# (VideoToolbox / MediaFoundation), then asserts the output MKV has an h264
# stream, every packet is a keyframe (all-intra), stereo audio, and non-trivial
# content (RMS > -60 dB). SKIP-return-code 77 so the test skips cleanly on CI
# runners without a hardware H.264 encoder. Ports 23462 (SRT) / 23463 (UDP) are
# clear of all other port ranges.
set(_h264_driver "${CMAKE_CURRENT_SOURCE_DIR}/run_record_h264_e2e.sh")
add_test(NAME e2e_h264_record_smoke
    COMMAND bash "${_h264_driver}" "$<TARGET_FILE:record_harness>" 23462)
set_tests_properties(e2e_h264_record_smoke PROPERTIES
    LABELS "e2e;h264"
    TIMEOUT 120
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77)

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
# Seek-flash gate: a forward seek after a warm cache must paint NO new gray
# placeholder (post-seek placeholderFramesDelta==0). Port 23494 (UDP 23495) is
# clear of the record/sync/av-sync/native ranges.
add_test(NAME e2e_play_seekflash
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" seekflash 2 23494)
# Far-backward seek-flash gate (Tier 2): play near EOF, jump all the way back to
# 0 — the worst case for the flash. The committed-playhead gate + worker
# double-buffer must keep placeholderFramesDelta==0 with cacheGeneration>=1.
# Port 23496 (UDP 23497) is the next clear port after seekflash (23494) and the
# av-sync port (23492), and is disjoint from the record/sync/native ranges.
add_test(NAME e2e_play_farback
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" farback 2 23496)
# Tier3 armed-cut gate (Tasks 9/10/12): arm a frame-perfect cut to the clip
# midpoint mid-playback; the pre-roll bank stages the target window and the cut
# atomically promotes it — placeholderFramesDelta==0 (no flash) AND reposition==0
# (the cut fired, not the coarse seek fallback). Port 23498 (UDP 23499) is the
# next clear port after farback (23496) and disjoint from the record/sync/
# av-sync/native ranges.
add_test(NAME e2e_play_armedcut
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" armedcut 2 23498)

set_tests_properties(
    e2e_play_storm e2e_play_seekplay e2e_play_stepscrub e2e_play_reverse
    e2e_play_sliderscrub e2e_play_liveedge e2e_play_4view e2e_play_latency
    e2e_play_seekflash e2e_play_farback e2e_play_armedcut
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

# --- Broadcast frame-sync acceptance rig ------------------------------------
# Opt-in instrument for lip-sync, inter-camera phase, drift and timecode
# measurements. These use a dedicated port band (>=24000) and report metrics on
# every run; lipsync and the zero-skew drift sanity cell run as gates while the
# inter-camera/timecode cells stay report-only until common-TC/tmcd work lands.
set(_framesync_driver "${CMAKE_CURRENT_SOURCE_DIR}/run_framesync_e2e.sh")
add_test(NAME e2e_framesync_lipsync
    COMMAND bash "${_framesync_driver}" "$<TARGET_FILE:sync_harness>" lipsync 24000)
add_test(NAME e2e_framesync_drift
    COMMAND bash "${_framesync_driver}" "$<TARGET_FILE:sync_harness>" drift 24010)
add_test(NAME e2e_framesync_drift_skew
    COMMAND bash "${_framesync_driver}" "$<TARGET_FILE:sync_harness>" drift_skew 24040)
add_test(NAME e2e_framesync_intercam
    COMMAND bash "${_framesync_driver}" "$<TARGET_FILE:sync_harness>" intercam 24020)
add_test(NAME e2e_framesync_timecode
    COMMAND bash "${_framesync_driver}" "$<TARGET_FILE:sync_harness>" timecode 24030)
set_tests_properties(
    e2e_framesync_lipsync e2e_framesync_drift e2e_framesync_drift_skew
    e2e_framesync_intercam e2e_framesync_timecode
    PROPERTIES
    LABELS "framesync"
    TIMEOUT 180
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77)
set_tests_properties(e2e_framesync_lipsync PROPERTIES
    ENVIRONMENT "OLR_FRAMESYNC_GATE=1")
set_tests_properties(e2e_framesync_drift PROPERTIES
    ENVIRONMENT "OLR_FRAMESYNC_GATE=1")
set_tests_properties(e2e_framesync_drift_skew PROPERTIES
    ENVIRONMENT "OLR_FRAMESYNC_SKEW_PPM=200")

# --- Local-only native SRT e2e (NOT in CI) ----------------------------------
# Prove the real engine ingests srt:// over the native SRT path (the default,
# and only, SRT ingest). The producer side uses local ffmpeg + srt-live-transmit;
# no SRT-enabled ffmpeg build is needed. The native-* labels keep these out of
# CI's e2e selection. Run locally with: ctest -L native-apple-ingest (macOS).
function(olr_add_native_srt_e2e_tests native_label)
    add_test(NAME e2e_native_srt_smoke
        COMMAND "${OLR_E2E_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_smoke.sh" "$<TARGET_FILE:record_harness>" 23601)
    add_test(NAME e2e_native_srt_4cam
        COMMAND "${OLR_E2E_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_4cam.sh" "$<TARGET_FILE:sync_harness>" 23610)
    add_test(NAME e2e_native_srt_sync
        COMMAND "${OLR_E2E_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_sync.sh" "$<TARGET_FILE:sync_harness>" 23620)
    add_test(NAME e2e_native_srt_trim
        COMMAND "${OLR_E2E_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_trim.sh" "$<TARGET_FILE:sync_harness>" 23630)
    add_test(NAME e2e_native_srt_connect
        COMMAND "${OLR_E2E_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_connect.sh" "$<TARGET_FILE:sync_harness>" 23640)
    add_test(NAME e2e_native_srt_streamid
        COMMAND "$<TARGET_FILE:srt_streamid_harness>" 23650)
    set_tests_properties(
        e2e_native_srt_smoke e2e_native_srt_4cam e2e_native_srt_sync
        e2e_native_srt_trim e2e_native_srt_connect e2e_native_srt_streamid
        PROPERTIES
        LABELS "${native_label}"
        TIMEOUT 180
        RUN_SERIAL TRUE)
    if(WIN32 AND _olr_windows_tool_env)
        set_tests_properties(
            e2e_native_srt_smoke e2e_native_srt_4cam e2e_native_srt_sync
            e2e_native_srt_trim e2e_native_srt_connect e2e_native_srt_streamid
            PROPERTIES ENVIRONMENT "${_olr_windows_tool_env}")
    endif()
endfunction()

if(APPLE)
    olr_add_native_srt_e2e_tests("native-apple-ingest")

    # --- Delivery push-gate cells (RTMP/SRT x h264/h265) ---------------------
    # Re-tag the SRT h264 smoke with the delivery-gate label (the function above
    # set it to "native-apple-ingest"; LABELS is replaced, not appended, so list
    # both). TIMEOUT/RUN_SERIAL set by the function are untouched.
    set_tests_properties(e2e_native_srt_smoke PROPERTIES
        LABELS "native-apple-ingest;delivery-gate")

    # SRT HEVC (h265) cell on macOS. run_srt_hevc_smoke.sh already exists and is
    # cross-platform (auto-selects libx265 / hevc_videotoolbox); only Windows had
    # registered it. Port 23740 sits in the gap between the SRT native band
    # (<=23730) and the RTMP band (>=23760).
    add_test(NAME e2e_native_srt_hevc_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_hevc_smoke.sh" "$<TARGET_FILE:record_harness>" 23740)
    set_tests_properties(e2e_native_srt_hevc_smoke PROPERTIES
        LABELS "native-apple-ingest;delivery-gate"
        TIMEOUT 180
        RUN_SERIAL TRUE)

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

    # Encrypted SRT (SRTO_PASSPHRASE/SRTO_PBKEYLEN): a passphrase-protected SRT
    # listener + an encrypted caller URL must decrypt + record real content. A
    # mismatch yields no frames, so this positively proves encrypted ingest. The
    # producer side needs ffmpeg + srt-live-transmit; SKIP-clean (77) otherwise.
    # Base 23720.
    add_test(NAME e2e_native_srt_encrypted
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_encrypted_smoke.sh" "$<TARGET_FILE:record_harness>" 23720)
    set_tests_properties(e2e_native_srt_encrypted PROPERTIES
        LABELS "native-apple-ingest" TIMEOUT 120 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)

    # SRT LISTENER mode (srt_bind + srt_listen + srt_accept): inverts the usual
    # roles — the engine binds + listens and srt-live-transmit connects as the
    # caller. Real decoded content proves the accept path works. Needs ffmpeg +
    # srt-live-transmit; SKIP-clean (77) otherwise. Base 23730.
    add_test(NAME e2e_native_srt_listener
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_listener_smoke.sh" "$<TARGET_FILE:record_harness>" 23730)
    set_tests_properties(e2e_native_srt_listener PROPERTIES
        LABELS "native-apple-ingest" TIMEOUT 120 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
elseif(WIN32)
    olr_add_native_srt_e2e_tests("native-windows-ingest")
    add_test(NAME e2e_native_srt_hevc_smoke
        COMMAND "${OLR_E2E_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_hevc_smoke.sh" "$<TARGET_FILE:record_harness>" 23701)
    set_tests_properties(e2e_native_srt_hevc_smoke PROPERTIES
        LABELS "native-windows-hevc"
        TIMEOUT 180
        RUN_SERIAL TRUE
        ENVIRONMENT "${_olr_windows_tool_env}")
endif()

# Native NDI ingest is cross-platform and runtime-loaded. This smoke starts a
# local runtime-loaded sender by default, or records
# OLR_NDI_TEST_SOURCE when a real external source is explicitly requested.
add_test(NAME e2e_native_ndi_smoke
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_ndi_smoke.sh"
        "$<TARGET_FILE:record_harness>" "$<TARGET_FILE:ndi_runtime_sender>")
set_tests_properties(e2e_native_ndi_smoke PROPERTIES
    LABELS "native-ndi"
    TIMEOUT 180
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77)

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
        LABELS "native-rtmp;delivery-gate"
        TIMEOUT 180
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77)
    set_tests_properties(e2e_native_rtmp_hevc_smoke PROPERTIES
        LABELS "native-rtmp;delivery-gate"
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
elseif(WIN32)
    add_test(NAME e2e_native_rtmp_smoke
        COMMAND "${OLR_E2E_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/run_rtmp_smoke.sh" "$<TARGET_FILE:record_harness>" 23701)
    set_tests_properties(e2e_native_rtmp_smoke PROPERTIES
        LABELS "native-rtmp"
        TIMEOUT 180
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        ENVIRONMENT "${_olr_windows_tool_env}")
endif()
