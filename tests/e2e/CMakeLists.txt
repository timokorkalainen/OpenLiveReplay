# End-to-end recording tests: a headless harness driving the real engine
# against synthetic FFmpeg streams. These exercise capture -> resample -> mux
# end to end and are the regression home for media-pipeline bugs.

qt_add_executable(record_harness record_harness.cpp)
target_link_libraries(record_harness PRIVATE
    Qt6::Core olr_test_engine olr_warnings olr_sanitize)

set(_driver "${CMAKE_CURRENT_SOURCE_DIR}/run_record_e2e.sh")

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
