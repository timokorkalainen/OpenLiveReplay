# OpenLiveReplay test suite — enabled via -DOLR_BUILD_TESTS=ON.
#
# Desktop/macOS only. The engine test library links the homebrew FFmpeg the
# same way the macOS desktop app does, and every test runs headless.
#
# Layout:
#   unit/   fast Qt Test unit tests (logic, no live capture)
#   smoke/  QML static-load smoke check (qmllint over the QML module)
#   e2e/    end-to-end recording harness driven by synthetic FFmpeg streams

find_package(Qt6 REQUIRED COMPONENTS Test Core Concurrent Network)

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

# Opt-in: link the test engine + harnesses against a libsrt-enabled ffmpeg built
# by build-scripts/build_ffmpeg_macos_srt.sh, so the local SRT e2e can ingest
# srt://. Default unset -> brew ffmpeg, so CI and the UDP gates are unaffected.
set(OLR_FFMPEG_SRT_PREFIX "" CACHE PATH "Path to a libsrt-enabled ffmpeg install (local SRT e2e)")
if(OLR_FFMPEG_SRT_PREFIX)
    set(OLR_FFMPEG_INCLUDE "${OLR_FFMPEG_SRT_PREFIX}/include")
    set(OLR_FFMPEG_LIBDIR  "${OLR_FFMPEG_SRT_PREFIX}/lib")
    # The ffmpeg dylibs use @rpath install names; give every test executable an
    # rpath to the SRT lib dir so they resolve at runtime (no DYLD_LIBRARY_PATH).
    list(APPEND CMAKE_BUILD_RPATH "${OLR_FFMPEG_LIBDIR}")
    message(STATUS "OpenLiveReplay tests: SRT ffmpeg prefix = ${OLR_FFMPEG_SRT_PREFIX}")
endif()

# --- Engine sources that need NO FFmpeg (pure Qt Core) ----------------------
qt_add_library(olr_test_core STATIC
    "${CMAKE_SOURCE_DIR}/recorder_engine/recordingclock.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/ingestsession.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/mpegtsparser.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/h26xaccessunit.cpp"
    "${CMAKE_SOURCE_DIR}/settingsmanager.cpp"
    "${CMAKE_SOURCE_DIR}/project/projectsettingsimporter.cpp"
    "${CMAKE_SOURCE_DIR}/project/projectimportclient.cpp"
    "${CMAKE_SOURCE_DIR}/playback/playbacktransport.cpp"
    "${CMAKE_SOURCE_DIR}/telemetry/sseparser.cpp"
    "${CMAKE_SOURCE_DIR}/telemetry/telemetryclient.cpp"
    "${CMAKE_SOURCE_DIR}/streamdeck/streamdeckmappingstore.cpp"
)
target_include_directories(olr_test_core PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(olr_test_core
    PUBLIC Qt6::Core Qt6::Network
    PRIVATE olr_warnings olr_sanitize)

# --- Engine sources that DO need FFmpeg (muxer / capture / recording) -------
qt_add_library(olr_test_engine STATIC
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/ffmpegingestsession.cpp"
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

# --- Playback sources (need Qt Multimedia for QVideoFrame; ffmpeg for the
#     worker, which #includes <libavformat/...> and demuxes the fixture). -----
qt_add_library(olr_test_playback STATIC
    "${CMAKE_SOURCE_DIR}/playback/trackbuffer.cpp"
    "${CMAKE_SOURCE_DIR}/playback/audioframequeue.cpp"
    "${CMAKE_SOURCE_DIR}/playback/playbackworker.cpp"
    "${CMAKE_SOURCE_DIR}/playback/frameprovider.cpp"
    "${CMAKE_SOURCE_DIR}/playback/telemetrytimelinereader.cpp"
    "${CMAKE_SOURCE_DIR}/playback/audioplayer.cpp"
)
target_include_directories(olr_test_playback PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${OLR_FFMPEG_INCLUDE}")
target_link_directories(olr_test_playback PUBLIC "${OLR_FFMPEG_LIBDIR}")
target_link_libraries(olr_test_playback
    PUBLIC olr_test_core Qt6::Core Qt6::Multimedia Qt6::Gui
           avformat avcodec avutil swscale swresample
    PRIVATE olr_warnings olr_sanitize)

add_subdirectory(unit)
target_link_libraries(tst_ingestbackendselector PRIVATE olr_test_engine)
add_subdirectory(smoke)
add_subdirectory(e2e)
