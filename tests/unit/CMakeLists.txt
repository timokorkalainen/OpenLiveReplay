# Fast Qt Test unit tests. Each is a standalone executable registered with CTest.

function(olr_add_unit_test name)
    qt_add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE Qt6::Test ${ARGN} olr_warnings olr_sanitize)
    add_test(NAME ${name} COMMAND ${name})
    # Headless + deterministic; fail fast rather than hang CI.
    set_tests_properties(${name} PROPERTIES
        LABELS "unit"
        TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

olr_add_unit_test(tst_recordingclock   olr_test_core)
olr_add_unit_test(tst_settingsmanager  olr_test_core)
olr_add_unit_test(tst_projectsettingsimporter olr_test_core)
olr_add_unit_test(tst_playbacktransport olr_test_core)
olr_add_unit_test(tst_sseparser olr_test_core)
olr_add_unit_test(tst_muxer            olr_test_engine)
olr_add_unit_test(tst_telemetrytimelinereader olr_test_engine olr_test_playback)
olr_add_unit_test(tst_replaymanager_telemetry olr_test_engine olr_test_playback)
olr_add_unit_test(tst_trackbuffer      olr_test_playback)
olr_add_unit_test(tst_audioframequeue  olr_test_playback)
olr_add_unit_test(tst_streamdeckmappingstore olr_test_core)
