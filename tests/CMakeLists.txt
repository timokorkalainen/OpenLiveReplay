# OpenLiveReplay test suite — enabled via -DOLR_BUILD_TESTS=ON.
#
# Desktop/macOS only. The engine test library links the homebrew FFmpeg the
# same way the macOS desktop app does, and every test runs headless.
#
# Layout:
#   unit/   fast Qt Test unit tests (logic, no live capture)
#   smoke/  QML static-load smoke check (qmllint over the QML module)
#   e2e/    end-to-end recording harness driven by synthetic FFmpeg streams

find_package(Qt6 REQUIRED COMPONENTS Test Core Concurrent)

# --- Locate homebrew FFmpeg (Apple Silicon or Intel) ------------------------
if(NOT DEFINED OLR_BREW_PREFIX)
    execute_process(COMMAND brew --prefix
        OUTPUT_VARIABLE OLR_BREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT OLR_BREW_PREFIX OR NOT EXISTS "${OLR_BREW_PREFIX}")
    set(OLR_BREW_PREFIX /opt/homebrew)
endif()
set(OLR_FFMPEG_INCLUDE "${OLR_BREW_PREFIX}/include")
set(OLR_FFMPEG_LIBDIR  "${OLR_BREW_PREFIX}/lib")
message(STATUS "OpenLiveReplay tests: FFmpeg prefix = ${OLR_BREW_PREFIX}")

# --- Engine sources that need NO FFmpeg (pure Qt Core) ----------------------
qt_add_library(olr_test_core STATIC
    "${CMAKE_SOURCE_DIR}/recorder_engine/recordingclock.cpp"
    "${CMAKE_SOURCE_DIR}/settingsmanager.cpp"
    "${CMAKE_SOURCE_DIR}/playback/playbacktransport.cpp"
)
target_include_directories(olr_test_core PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(olr_test_core
    PUBLIC Qt6::Core
    PRIVATE olr_warnings olr_sanitize)

# --- Engine sources that DO need FFmpeg (muxer / capture / recording) -------
qt_add_library(olr_test_engine STATIC
    "${CMAKE_SOURCE_DIR}/recorder_engine/muxer.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/streamworker.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/replaymanager.cpp"
)
target_include_directories(olr_test_engine PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/recorder_engine"
    "${OLR_FFMPEG_INCLUDE}")
target_link_directories(olr_test_engine PUBLIC "${OLR_FFMPEG_LIBDIR}")
target_link_libraries(olr_test_engine
    PUBLIC olr_test_core Qt6::Core Qt6::Concurrent
           avformat avcodec avutil swscale swresample
    PRIVATE olr_warnings olr_sanitize)

add_subdirectory(unit)
add_subdirectory(smoke)
add_subdirectory(e2e)
