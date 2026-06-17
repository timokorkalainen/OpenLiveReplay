# Fast Qt Test unit tests. Each is a standalone executable registered with CTest.

function(olr_add_unit_test name)
    qt_add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE Qt6::Test ${ARGN} olr_warnings olr_sanitize)
    add_test(NAME ${name} COMMAND ${name})
    # Headless + deterministic; fail fast rather than hang CI.
    set_tests_properties(${name} PROPERTIES
        LABELS "unit;ci"
        TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

olr_add_unit_test(tst_recordingclock   olr_test_core)
olr_add_unit_test(tst_heartbeat        olr_test_core)
olr_add_unit_test(tst_controlprotocol  olr_test_core)
olr_add_unit_test(tst_controlwebsocketserver olr_test_core)
olr_add_unit_test(tst_controlstate    olr_test_core)
olr_add_unit_test(tst_settingsmanager  olr_test_core)
olr_add_unit_test(tst_projectsettingsimporter olr_test_core)
olr_add_unit_test(tst_playbacktransport olr_test_core)
olr_add_unit_test(tst_sseparser olr_test_core)
olr_add_unit_test(tst_telemetryclient olr_test_core)
olr_add_unit_test(tst_muxer            olr_test_engine)
olr_add_unit_test(tst_ingestbackendselector olr_test_core)
olr_add_unit_test(tst_mpegtsparser     olr_test_core)
olr_add_unit_test(tst_mpegtsparser_pcr olr_test_core)
olr_add_unit_test(tst_h26xaccessunit   olr_test_core)
olr_add_unit_test(tst_nativesrtaddress olr_test_core)
olr_add_unit_test(tst_nativevideodecoder olr_test_core)
olr_add_unit_test(tst_nativeframecopy olr_test_core)
olr_add_unit_test(tst_rtmpprotocol     olr_test_core)
if(APPLE)
    add_library(olr_test_nativevideodecoder STATIC
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativevideodecoder_videotoolbox.mm")
    target_include_directories(olr_test_nativevideodecoder PRIVATE "${OLR_FFMPEG_INCLUDE}")
    target_link_directories(olr_test_nativevideodecoder PUBLIC "${OLR_FFMPEG_LIBDIR}")
    target_link_libraries(olr_test_nativevideodecoder
        PUBLIC Qt6::Core avutil "-framework VideoToolbox" "-framework CoreMedia"
                "-framework CoreFoundation" "-framework CoreVideo")
    target_link_libraries(tst_nativevideodecoder PRIVATE olr_test_nativevideodecoder)
elseif(WIN32)
    add_library(olr_test_nativevideodecoder STATIC
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp")
    target_link_libraries(olr_test_nativevideodecoder
        PUBLIC Qt6::Core
        PRIVATE mfplat mf mfuuid d3d11 dxgi ole32)
    target_link_libraries(tst_nativevideodecoder PRIVATE olr_test_nativevideodecoder)
elseif(NOT WIN32)
    add_library(olr_test_nativevideodecoder STATIC
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativevideodecoder_stub.cpp")
    target_link_libraries(olr_test_nativevideodecoder PUBLIC Qt6::Core)
    target_link_libraries(tst_nativevideodecoder PRIVATE olr_test_nativevideodecoder)
endif()
olr_add_unit_test(tst_nativeaacdecoder olr_test_core)
olr_add_unit_test(tst_telemetrytimelinereader olr_test_engine olr_test_playback)
olr_add_unit_test(tst_replaymanager_telemetry olr_test_engine olr_test_playback)
olr_add_unit_test(tst_trackbuffer      olr_test_playback)
olr_add_unit_test(tst_audioframequeue  olr_test_playback)
olr_add_unit_test(tst_outputframeclock olr_test_playback)
olr_add_unit_test(tst_outputframecache olr_test_playback)
olr_add_unit_test(tst_outputtargetassignment olr_test_playback)
olr_add_unit_test(tst_broadcastoutputsettings olr_test_core)
olr_add_unit_test(tst_outputbusengine olr_test_playback)
olr_add_unit_test(tst_yuv420pcompositor olr_test_playback)
olr_add_unit_test(tst_qtpreviewsink olr_test_playback)
olr_add_unit_test(tst_outputdispatcher olr_test_playback)
olr_add_unit_test(tst_outputruntime olr_test_playback)
olr_add_unit_test(tst_queuedoutputsink olr_test_playback)
olr_add_unit_test(tst_ndisink olr_test_playback)
qt_add_executable(tst_ndi_runtime_smoke tst_ndi_runtime_smoke.cpp)
target_link_libraries(tst_ndi_runtime_smoke PRIVATE Qt6::Test olr_test_playback olr_warnings olr_sanitize)
add_test(NAME tst_ndi_runtime_smoke COMMAND tst_ndi_runtime_smoke)
set_tests_properties(tst_ndi_runtime_smoke PROPERTIES
    LABELS "ndi-runtime"
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
olr_add_unit_test(tst_streamdeckmappingstore olr_test_core)
olr_add_unit_test(tst_srt_health olr_test_core)
olr_add_unit_test(tst_rtmp_health      olr_test_core)
