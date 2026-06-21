# OpenLiveReplay test suite — enabled via -DOLR_BUILD_TESTS=ON.
#
# Desktop tests run headless. On Windows, FFmpeg and SRT paths are explicit
# cache entries; on macOS, the engine test library keeps using Homebrew FFmpeg
# the same way the desktop app does.
#
# Layout:
#   unit/   fast Qt Test unit tests (logic, no live capture)
#   smoke/  QML static-load smoke check (qmllint over the QML module)
#   e2e/    end-to-end recording harness driven by synthetic FFmpeg streams

find_package(Qt6 REQUIRED COMPONENTS Test Core Concurrent Network WebSockets)

# --- Locate FFmpeg ----------------------------------------------------------
if(WIN32)
    set(OLR_FFMPEG_ROOT "" CACHE PATH "Root directory of the Windows FFmpeg install")
    find_path(OLR_FFMPEG_INCLUDE
        NAMES libavutil/frame.h
        HINTS "${OLR_FFMPEG_ROOT}/include")
    find_library(OLR_FFMPEG_AVUTIL_LIBRARY
        NAMES avutil
        HINTS "${OLR_FFMPEG_ROOT}/lib" "${OLR_FFMPEG_ROOT}/bin")
    find_library(OLR_FFMPEG_AVFORMAT_LIBRARY
        NAMES avformat
        HINTS "${OLR_FFMPEG_ROOT}/lib" "${OLR_FFMPEG_ROOT}/bin")
    find_library(OLR_FFMPEG_AVCODEC_LIBRARY
        NAMES avcodec
        HINTS "${OLR_FFMPEG_ROOT}/lib" "${OLR_FFMPEG_ROOT}/bin")
    find_library(OLR_FFMPEG_SWSCALE_LIBRARY
        NAMES swscale
        HINTS "${OLR_FFMPEG_ROOT}/lib" "${OLR_FFMPEG_ROOT}/bin")
    find_library(OLR_FFMPEG_SWRESAMPLE_LIBRARY
        NAMES swresample
        HINTS "${OLR_FFMPEG_ROOT}/lib" "${OLR_FFMPEG_ROOT}/bin")
    if(NOT OLR_FFMPEG_INCLUDE
            OR NOT OLR_FFMPEG_AVUTIL_LIBRARY
            OR NOT OLR_FFMPEG_AVFORMAT_LIBRARY
            OR NOT OLR_FFMPEG_AVCODEC_LIBRARY
            OR NOT OLR_FFMPEG_SWSCALE_LIBRARY
            OR NOT OLR_FFMPEG_SWRESAMPLE_LIBRARY)
        message(FATAL_ERROR "OpenLiveReplay Windows tests require OLR_FFMPEG_ROOT with FFmpeg include and library paths")
    endif()
else()
    # Locate homebrew FFmpeg (Apple Silicon or Intel).
    if(NOT DEFINED OLR_BREW_PREFIX)
        execute_process(COMMAND brew --prefix
            OUTPUT_VARIABLE OLR_BREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if(NOT OLR_BREW_PREFIX OR NOT EXISTS "${OLR_BREW_PREFIX}")
        set(OLR_BREW_PREFIX /opt/homebrew)
    endif()
    set(OLR_FFMPEG_INCLUDE "${OLR_BREW_PREFIX}/include")
    set(OLR_FFMPEG_LIBDIR  "${OLR_BREW_PREFIX}/lib")
    set(OLR_FFMPEG_AVUTIL_LIBRARY avutil)
    set(OLR_FFMPEG_AVFORMAT_LIBRARY avformat)
    set(OLR_FFMPEG_AVCODEC_LIBRARY avcodec)
    set(OLR_FFMPEG_SWSCALE_LIBRARY swscale)
    set(OLR_FFMPEG_SWRESAMPLE_LIBRARY swresample)
    message(STATUS "OpenLiveReplay tests: FFmpeg prefix = ${OLR_BREW_PREFIX}")
endif()

if(WIN32)
    set(OLR_SRT_ROOT "" CACHE PATH "Root directory of the Windows SRT install")
    find_path(OLR_SRT_INCLUDE_DIR
        NAMES srt/srt.h
        HINTS "${OLR_SRT_ROOT}/include")
    find_library(OLR_SRT_LIBRARY
        NAMES srt
        HINTS "${OLR_SRT_ROOT}/lib" "${OLR_SRT_ROOT}/bin")
    if(NOT OLR_SRT_INCLUDE_DIR OR NOT OLR_SRT_LIBRARY)
        message(FATAL_ERROR "OpenLiveReplay Windows native SRT requires OLR_SRT_ROOT or SRT include/library paths")
    endif()
endif()

# --- Lightweight engine sources used by fast unit tests ---------------------
qt_add_library(olr_test_core STATIC
    "${CMAKE_SOURCE_DIR}/recorder_engine/recordingclock.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/heartbeat.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/driftestimator.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/timecode.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/smpte12m.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/timecodealigner.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/sourceoffsetestimator.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/sourceclock.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/timingreference.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/ptpservo.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/ptpreference.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/udpptpclient.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/ingestsession.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/mpegtsparser.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/h26xaccessunit.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/colorvui.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativeframecopy.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativesrtconnectdiagnostics.h"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativesrtconnectdiagnostics.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativesrtaddress.h"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativesrtaddress.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativesrturloptions.h"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativesrturloptions.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/rtmpprotocol.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/outputtargetassignment.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/broadcastoutputsettings.cpp"
    "${CMAKE_SOURCE_DIR}/settingsmanager.cpp"
    "${CMAKE_SOURCE_DIR}/project/projectsettingsimporter.cpp"
    "${CMAKE_SOURCE_DIR}/project/projectimportclient.cpp"
    "${CMAKE_SOURCE_DIR}/playback/playbacktransport.cpp"
    "${CMAKE_SOURCE_DIR}/telemetry/sseparser.cpp"
    "${CMAKE_SOURCE_DIR}/telemetry/telemetryclient.cpp"
    "${CMAKE_SOURCE_DIR}/websocket/controlprotocol.cpp"
    "${CMAKE_SOURCE_DIR}/websocket/controlstate.cpp"
    "${CMAKE_SOURCE_DIR}/websocket/controlwebsocketserver.cpp"
    "${CMAKE_SOURCE_DIR}/streamdeck/streamdeckmappingstore.cpp"
)
target_include_directories(olr_test_core PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${OLR_FFMPEG_INCLUDE}")
if(NOT WIN32)
    target_link_directories(olr_test_core PUBLIC "${OLR_FFMPEG_LIBDIR}")
endif()
target_link_libraries(olr_test_core
    PUBLIC Qt6::Core Qt6::Network Qt6::WebSockets "${OLR_FFMPEG_AVUTIL_LIBRARY}"
    PRIVATE olr_warnings olr_sanitize)
if(WIN32)
    target_link_libraries(olr_test_core PRIVATE ws2_32)
endif()

if(APPLE)
    target_sources(olr_test_core PRIVATE
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativeaacdecoder_audiotoolbox.mm")
    target_link_libraries(olr_test_core PRIVATE "-framework AudioToolbox")
elseif(WIN32)
    target_sources(olr_test_core PRIVATE
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativeaacdecoder_mediafoundation.cpp")
    target_link_libraries(olr_test_core PRIVATE mfplat mf mfuuid ole32)
else()
    target_sources(olr_test_core PRIVATE
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativeaacdecoder_stub.cpp")
endif()

# --- Engine sources that DO need FFmpeg (muxer / capture / recording) -------
qt_add_library(olr_test_engine STATIC
    "${CMAKE_SOURCE_DIR}/recorder_engine/muxer.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/streamworker.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/replaymanager.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/codec/avcc.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/benchmark/benchmarkplan.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/benchmark/codecbenchmark.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/benchmark/syntheticframes.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/benchmark/realcodecrunners.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/benchmark/benchmarkcache.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/benchmark/runcodecbenchmark.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/benchmark/recordgate.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/h26xseitimecode.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/ndiframeconvert.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativendiingestsession.cpp"
)
target_include_directories(olr_test_engine PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/recorder_engine"
    "${OLR_FFMPEG_INCLUDE}")
if(NOT WIN32)
    target_link_directories(olr_test_engine PUBLIC "${OLR_FFMPEG_LIBDIR}")
endif()
target_link_libraries(olr_test_engine
    PUBLIC olr_test_core Qt6::Core Qt6::Concurrent
           "${OLR_FFMPEG_AVFORMAT_LIBRARY}" "${OLR_FFMPEG_AVCODEC_LIBRARY}"
           "${OLR_FFMPEG_AVUTIL_LIBRARY}" "${OLR_FFMPEG_SWSCALE_LIBRARY}"
           "${OLR_FFMPEG_SWRESAMPLE_LIBRARY}"
    PRIVATE olr_warnings olr_sanitize)
# Expose the friend-class seam to every TU that sees olr_test_engine (the
# library TU itself and all linking test executables). Production app targets
# never link olr_test_engine, so this define never reaches production code.
target_compile_definitions(olr_test_engine PUBLIC OLR_UNIT_TEST=1)

if(APPLE)
    target_sources(olr_test_engine PRIVATE
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativesrtingestsession.cpp"
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativertmpingestsession.cpp"
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativevideodecoder_videotoolbox.mm"
        "${CMAKE_SOURCE_DIR}/recorder_engine/codec/nativevideoencoder_videotoolbox.mm")
    target_compile_definitions(olr_test_engine PUBLIC
        OLR_NATIVE_SRT_AVAILABLE=1
        OLR_NATIVE_RTMP_AVAILABLE=1)
    target_include_directories(olr_test_engine PUBLIC "${OLR_BREW_PREFIX}/opt/srt/include")
    target_link_directories(olr_test_engine PUBLIC "${OLR_BREW_PREFIX}/opt/srt/lib")
    target_link_libraries(olr_test_engine
        PRIVATE srt "-framework VideoToolbox" "-framework CoreMedia"
                "-framework CoreFoundation" "-framework CoreVideo")
elseif(WIN32)
    target_sources(olr_test_engine PRIVATE
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativesrtingestsession.cpp"
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativertmpingestsession.cpp"
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp"
        "${CMAKE_SOURCE_DIR}/recorder_engine/codec/nativevideoencoder_mediafoundation.cpp")
    target_compile_definitions(olr_test_engine PUBLIC
        OLR_NATIVE_SRT_AVAILABLE=1
        OLR_NATIVE_RTMP_AVAILABLE=1)
    target_include_directories(olr_test_engine PUBLIC "${OLR_SRT_INCLUDE_DIR}")
    target_link_libraries(olr_test_engine
        PRIVATE mfplat mf mfuuid mfreadwrite strmiids d3d11 dxgi ole32 ws2_32 "${OLR_SRT_LIBRARY}")
elseif(NOT WIN32)
    target_sources(olr_test_engine PRIVATE
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativevideodecoder_stub.cpp"
        "${CMAKE_SOURCE_DIR}/recorder_engine/codec/nativevideoencoder_stub.cpp")
endif()

# --- Playback sources (need Qt Multimedia for QVideoFrame; ffmpeg for the
#     worker, which #includes <libavformat/...> and demuxes the fixture). -----
qt_add_library(olr_test_playback STATIC
    "${CMAKE_SOURCE_DIR}/playback/trackbuffer.cpp"
    "${CMAKE_SOURCE_DIR}/playback/frameindex.cpp"
    "${CMAKE_SOURCE_DIR}/playback/replayplaylist.cpp"
    "${CMAKE_SOURCE_DIR}/playback/cutschedule.cpp"
    "${CMAKE_SOURCE_DIR}/playback/playlistplayout.cpp"
    "${CMAKE_SOURCE_DIR}/playback/audioframequeue.cpp"
    "${CMAKE_SOURCE_DIR}/playback/playbackworker.cpp"
    "${CMAKE_SOURCE_DIR}/playback/frameprovider.cpp"
    "${CMAKE_SOURCE_DIR}/playback/telemetrytimelinereader.cpp"
    "${CMAKE_SOURCE_DIR}/playback/audioplayer.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/outputframeclock.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/framehandle.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/colormetadatapolicy.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/outputframecache.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/outputtargetassignment.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/outputbusengine.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/gpureadbacktelemetry.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/formatcanon.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/outputdispatcher.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/broadcastoutputstatus.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/outputruntime.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/queuedoutputsink.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/yuv420pcompositor.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/qtpreviewsink.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/ndiabi.h"
    "${CMAKE_SOURCE_DIR}/playback/output/ndiruntimepaths.h"
    "${CMAKE_SOURCE_DIR}/playback/output/ndisink.cpp"
)
target_include_directories(olr_test_playback PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${OLR_FFMPEG_INCLUDE}")
if(NOT WIN32)
    target_link_directories(olr_test_playback PUBLIC "${OLR_FFMPEG_LIBDIR}")
endif()
target_link_libraries(olr_test_playback
    PUBLIC olr_test_engine Qt6::Core Qt6::Multimedia Qt6::Gui
           "${OLR_FFMPEG_AVFORMAT_LIBRARY}" "${OLR_FFMPEG_AVCODEC_LIBRARY}"
           "${OLR_FFMPEG_AVUTIL_LIBRARY}" "${OLR_FFMPEG_SWSCALE_LIBRARY}"
           "${OLR_FFMPEG_SWRESAMPLE_LIBRARY}"
    PRIVATE olr_warnings olr_sanitize)
if(APPLE)
    target_sources(olr_test_playback PRIVATE
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpusurface_apple.mm"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpupipelineconfig.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/applegpusurface_apple.mm")
    target_link_libraries(olr_test_playback
        PRIVATE "-framework CoreFoundation" "-framework CoreVideo" "-framework IOSurface")
else()
    target_sources(olr_test_playback PRIVATE
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpusurface_stub.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpupipelineconfig.cpp")
    if(WIN32)
        target_sources(olr_test_playback PRIVATE
            "${CMAKE_SOURCE_DIR}/playback/output/win/wingpuimportedge.cpp"
            "${CMAKE_SOURCE_DIR}/playback/output/win/d3d11gpusurface.cpp"
            "${CMAKE_SOURCE_DIR}/playback/output/win/d3dfence.cpp")
        target_link_libraries(olr_test_playback
            PRIVATE mfplat mf mfuuid mfreadwrite strmiids d3d11 dxgi ole32)
    endif()
endif()

if(OLR_GPU_PIPELINE)
    target_compile_definitions(olr_test_playback PRIVATE OLR_GPU_PIPELINE_BUILD=1)
    target_sources(olr_test_playback PRIVATE
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpuframedata.cpp")
    if(APPLE)
        target_sources(olr_test_playback PRIVATE
            "${CMAKE_SOURCE_DIR}/playback/gpu/gpurhicontext_apple.mm"
            "${CMAKE_SOURCE_DIR}/playback/gpu/decodedonefence_apple.mm"
            "${CMAKE_SOURCE_DIR}/playback/gpu/vtkeepsurfaceimporter_apple.mm")
        target_link_libraries(olr_test_playback
            PRIVATE Qt6::GuiPrivate "-framework Metal" "-framework QuartzCore")
    else()
        target_sources(olr_test_playback PRIVATE
            "${CMAKE_SOURCE_DIR}/playback/gpu/gpurhicontext_stub.cpp"
            "${CMAKE_SOURCE_DIR}/playback/gpu/decodedonefence_stub.cpp")
    endif()

    qt_add_library(olr_test_gpu STATIC
        "${CMAKE_SOURCE_DIR}/playback/gpu/olrrhi.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpurhicontext_stub.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpuframedata.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/decodedonefence_stub.cpp")
    target_include_directories(olr_test_gpu PUBLIC "${CMAKE_SOURCE_DIR}")
    target_link_libraries(olr_test_gpu
        PUBLIC Qt6::Core Qt6::Gui Qt6::GuiPrivate
        PRIVATE olr_warnings olr_sanitize)
    if(APPLE)
        target_sources(olr_test_gpu PRIVATE
            "${CMAKE_SOURCE_DIR}/playback/gpu/gpurhicontext_apple.mm"
            "${CMAKE_SOURCE_DIR}/playback/gpu/decodedonefence_apple.mm"
            "${CMAKE_SOURCE_DIR}/playback/gpu/vtkeepsurfaceimporter_apple.mm")
        target_link_libraries(olr_test_gpu
            PUBLIC "-framework Metal" "-framework QuartzCore" "-framework CoreVideo"
                   "-framework CoreFoundation" "-framework IOSurface")
    endif()

    qt_add_shaders(olr_test_gpu "olr_passthrough_shaders"
        PREFIX "/olr/shaders"
        BASE "${CMAKE_SOURCE_DIR}/playback/gpu/shaders"
        FILES
            "${CMAKE_SOURCE_DIR}/playback/gpu/shaders/passthrough.vert"
            "${CMAKE_SOURCE_DIR}/playback/gpu/shaders/passthrough.frag")
endif()

add_subdirectory(unit)
if(APPLE OR WIN32)  # tst_ingestbackendselector is Apple/Windows-only (native ingest)
    target_link_libraries(tst_ingestbackendselector PRIVATE olr_test_engine)
endif()
add_subdirectory(smoke)
add_subdirectory(e2e)
add_subdirectory(qml)
add_subdirectory(qmlstyle)
