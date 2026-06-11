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

# Headline gate: 2-view 1x steady playback must report 0 repositions (the old
# decode loop stormed ~90 hard-seeks/s) and run the audio path (audioPushes>0).
add_test(NAME e2e_play_storm
    COMMAND bash "${_pb_driver}"
        "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" play1x 2)
set_tests_properties(e2e_play_storm PROPERTIES
    LABELS "e2e"
    TIMEOUT 180
    RUN_SERIAL TRUE)
