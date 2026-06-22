cmake_minimum_required(VERSION 3.16)
project(OpenLiveReplay VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Export compile_commands.json for clang-tidy / clangd / editors.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Build options: testing, strict warnings, sanitizers
# ---------------------------------------------------------------------------
option(OLR_BUILD_TESTS "Build the OpenLiveReplay test suite" OFF)
option(OLR_WERROR "Treat warnings as errors for first-party targets" ON)
option(OLR_WCONVERSION "Also enable the -Wconversion/-Wsign-conversion family (large backlog; opt-in)" OFF)
option(OLR_BUILD_FUZZERS "Build libFuzzer harnesses for the input parsers (needs clang+libFuzzer)" OFF)
option(OLR_GPU_PIPELINE "Build the GPU-resident pipeline (RHI spine, shaders), default off" OFF)
option(OLR_BUILD_GPU_PROBES "Build Phase-0 GPU measurement probes (links Qt RHI; not shipped)" OFF)
set(OLR_SANITIZER "" CACHE STRING
    "Sanitizers to enable, e.g. address, thread, undefined, or 'address;undefined'")

# Warning flags applied to first-party targets only (never third-party deps
# like rtmidi/ffmpeg). The first-party tree is -Wall -Wextra -Wshadow
# -Wnon-virtual-dtor clean, so -Werror defaults ON to keep it that way.
#
# -Wconversion/-Wsign-conversion are deliberately NOT in the default set: the
# FFmpeg-interop code carries ~1k mostly-benign int/size_t coercions, so gating
# them would be churn, not signal. Audit that backlog separately via
# -DOLR_WCONVERSION=ON (it is excluded from -Werror so it stays advisory).
add_library(olr_warnings INTERFACE)
if(MSVC)
    target_compile_options(olr_warnings INTERFACE /W4)
    if(OLR_WERROR)
        target_compile_options(olr_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(olr_warnings INTERFACE -Wall -Wextra -Wshadow -Wnon-virtual-dtor)

    # Cross-compiler high-value diagnostics (GCC and Clang both honour these):
    #   -Wformat=2             stricter printf/scanf format checking (incl.
    #                          -Wformat-security/-Wformat-nonliteral).
    #   -Wimplicit-fallthrough catches missing break/[[fallthrough]] in switches.
    target_compile_options(olr_warnings INTERFACE -Wformat=2 -Wimplicit-fallthrough)

    # GCC-only high-value diagnostics. These flags either do not exist on
    # Clang/AppleClang or behave very differently, so gate them to the GNU
    # frontend via a per-language generator expression — the macOS (Clang) and
    # iOS (AppleClang) legs never see them.
    #   -Wdangling-reference   (GCC 13+) reference bound to a destroyed temporary.
    #   -Wmaybe-uninitialized  possibly-uninitialized read.
    #   -Wnull-dereference     path-sensitive NULL dereference.
    #   -Wduplicated-cond      duplicated condition in an if/else-if chain.
    #   -Wduplicated-branches  identical bodies on both arms of a branch.
    #   -Wlogical-op           suspicious use of && / || (e.g. always-true).
    # -Wdangling-reference is silently ignored by GCC < 13, so passing it
    # unconditionally to the GNU frontend is safe.
    target_compile_options(olr_warnings INTERFACE
        $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wdangling-reference>
        $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wmaybe-uninitialized>
        $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wnull-dereference>
        $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wduplicated-cond>
        $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wduplicated-branches>
        $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wlogical-op>)

    if(OLR_WERROR)
        target_compile_options(olr_warnings INTERFACE -Werror)
    endif()
    if(OLR_WCONVERSION)
        # Advisory even under -Werror: surfaced as warnings, never fatal.
        target_compile_options(olr_warnings INTERFACE
            -Wconversion -Wsign-conversion -Wno-error=conversion -Wno-error=sign-conversion)
    endif()
endif()

# Sanitizer flags (compile + link), applied only when OLR_SANITIZER is set.
add_library(olr_sanitize INTERFACE)
if(OLR_SANITIZER)
    string(REPLACE ";" "," _olr_san "${OLR_SANITIZER}")
    target_compile_options(olr_sanitize INTERFACE
        -fsanitize=${_olr_san} -fno-omit-frame-pointer -g)
    target_link_options(olr_sanitize INTERFACE -fsanitize=${_olr_san})
    message(STATUS "OpenLiveReplay: sanitizer(s) enabled -> ${_olr_san}")
endif()

include(FetchContent)
# Select the native MIDI backend per platform: CoreMIDI on Apple, WinMM on
# Windows. (Forcing CoreMIDI everywhere leaves Windows with no MIDI API.)
if(WIN32)
    set(RTMIDI_API_COREMIDI OFF CACHE BOOL "" FORCE)
    set(RTMIDI_API_WINMM ON CACHE BOOL "" FORCE)
else()
    set(RTMIDI_API_COREMIDI ON CACHE BOOL "" FORCE)
    set(RTMIDI_API_WINMM OFF CACHE BOOL "" FORCE)
endif()
set(RTMIDI_API_ALSA OFF CACHE BOOL "" FORCE)
set(RTMIDI_API_JACK OFF CACHE BOOL "" FORCE)
set(RTMIDI_API_DUMMY OFF CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    rtmidi
    URL https://github.com/thestk/rtmidi/archive/refs/tags/6.0.0.tar.gz
    URL_HASH SHA256=ef7bcda27fee6936b651c29ebe9544c74959d0b1583b716ce80a1c6fea7617f0
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(rtmidi)

# Qt location: explicit configuration always wins. Only when nothing points at a
# Qt kit do we auto-detect the standard installer / Homebrew locations, so a
# fresh `cmake` (or VS Code CMake Tools in presets mode, which bypasses kits and
# cmake.configureSettings) just works on any machine. CI and build-scripts/*
# always export QT_ROOT_DIR / OLR_QT_ROOT, so this fallback never fires for them.
if(NOT CMAKE_PREFIX_PATH AND NOT DEFINED ENV{CMAKE_PREFIX_PATH})
    if(DEFINED ENV{OLR_QT_ROOT} AND NOT "$ENV{OLR_QT_ROOT}" STREQUAL "")
        list(APPEND CMAKE_PREFIX_PATH "$ENV{OLR_QT_ROOT}")
    elseif(DEFINED ENV{QT_ROOT_DIR} AND NOT "$ENV{QT_ROOT_DIR}" STREQUAL "")
        list(APPEND CMAKE_PREFIX_PATH "$ENV{QT_ROOT_DIR}")
    else()
        set(_olr_qt_candidates "")
        if(APPLE)
            file(GLOB _olr_qt_candidates "$ENV{HOME}/Qt/6.*/macos")
            list(APPEND _olr_qt_candidates "/opt/homebrew/opt/qt" "/usr/local/opt/qt")
        elseif(WIN32)
            file(GLOB _olr_qt_candidates
                "$ENV{USERPROFILE}/Qt/6.*/mingw_*"
                "$ENV{USERPROFILE}/Qt/6.*/msvc*")
        else()
            file(GLOB _olr_qt_candidates "$ENV{HOME}/Qt/6.*/gcc_64")
        endif()
        # Prefer the newest Qt (natural version order so 6.10 > 6.9).
        list(SORT _olr_qt_candidates COMPARE NATURAL ORDER DESCENDING)
        foreach(_olr_qt_dir IN LISTS _olr_qt_candidates)
            if(IS_DIRECTORY "${_olr_qt_dir}")
                list(APPEND CMAKE_PREFIX_PATH "${_olr_qt_dir}")
                message(STATUS "OpenLiveReplay: auto-detected Qt kit at ${_olr_qt_dir}")
                break()
            endif()
        endforeach()
        unset(_olr_qt_candidates)
    endif()
endif()

# 1. Find Qt
find_package(Qt6 REQUIRED COMPONENTS Quick Core Multimedia Concurrent QuickControls2 Network WebSockets)
if(OLR_GPU_PIPELINE)
    # RHI lives in Qt6::GuiPrivate (<rhi/qrhi.h> is a semi-public Gui header);
    # qsb / qt_add_shaders come from Qt6::ShaderTools. This path is coupled to
    # the project's Qt 6.10.x toolchain by design.
    find_package(Qt6 REQUIRED COMPONENTS Gui GuiPrivate ShaderTools)
endif()
qt_standard_project_setup(REQUIRES 6.8)

# All QML modules output under one dir so the build-time import path resolves every
# module (OpenLiveReplay, OlrTheme, OlrStyle) consistently for qmllint/qmlcachegen.
set(QT_QML_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/qml")

# UI design system: token singleton + bespoke QQC2 custom style (defined before the
# app so they are on the QML import path when Main.qml is processed).
add_subdirectory(ui/theme)
add_subdirectory(ui/style)

# 2. Create the Executable Target FIRST
qt_add_executable(OpenLiveReplay main.cpp)

# 3. Define QML Module and Sources
qt_add_qml_module(OpenLiveReplay
    URI OpenLiveReplay
    VERSION 1.0
    QML_FILES
        Main.qml
        MultiviewWindow.qml
        CodecSettingsPanel.qml
        ui/components/BindingsPanel.qml
        ui/components/ConfigDrawer.qml
        ui/components/OutputsPanel.qml
        ui/components/PgmStage.qml
        ui/components/ProjectSettingsPanel.qml
        ui/components/RundownRail.qml
        ui/components/SourceListPanel.qml
        ui/components/StatusStrip.qml
        ui/components/TransportDock.qml
    SOURCES
        websocket/controlapiadapter.h
        websocket/controlprotocol.h websocket/controlprotocol.cpp
        websocket/controlstate.h websocket/controlstate.cpp
        websocket/controlwebsocketserver.h websocket/controlwebsocketserver.cpp
        websocket/uimanagercontroladapter.h websocket/uimanagercontroladapter.cpp
        recorder_engine/replaymanager.cpp recorder_engine/replaymanager.h
        recorder_engine/heartbeat.h recorder_engine/heartbeat.cpp
        recorder_engine/timing/driftestimator.h recorder_engine/timing/driftestimator.cpp
        recorder_engine/timing/timecode.h recorder_engine/timing/timecode.cpp
        recorder_engine/timing/smpte12m.h recorder_engine/timing/smpte12m.cpp
        recorder_engine/timing/timecodealigner.h recorder_engine/timing/timecodealigner.cpp
        recorder_engine/timing/sourceoffsetestimator.h recorder_engine/timing/sourceoffsetestimator.cpp
        recorder_engine/timing/sourceclock.h recorder_engine/timing/sourceclock.cpp
        recorder_engine/timing/timingreference.h recorder_engine/timing/timingreference.cpp
        recorder_engine/timing/iptpclient.h
        recorder_engine/timing/ptpservo.h recorder_engine/timing/ptpservo.cpp
        recorder_engine/timing/ptpreference.h recorder_engine/timing/ptpreference.cpp
        recorder_engine/timing/udpptpclient.h recorder_engine/timing/udpptpclient.cpp
        recorder_engine/muxer.h recorder_engine/muxer.cpp
        recorder_engine/streamworker.h recorder_engine/streamworker.cpp
        recorder_engine/recordingclock.h recorder_engine/recordingclock.cpp
        recorder_engine/ingest/ingestsession.h recorder_engine/ingest/ingestsession.cpp
        recorder_engine/ingest/pespacket.h
        recorder_engine/ingest/mpegtsparser.h recorder_engine/ingest/mpegtsparser.cpp
        recorder_engine/ingest/h26xaccessunit.h recorder_engine/ingest/h26xaccessunit.cpp
        recorder_engine/ingest/colorvui.h recorder_engine/ingest/colorvui.cpp
        recorder_engine/codec/avcc.h recorder_engine/codec/avcc.cpp
        recorder_engine/benchmark/benchmarktypes.h
        recorder_engine/benchmark/benchmarkplan.h recorder_engine/benchmark/benchmarkplan.cpp
        recorder_engine/benchmark/codecrunner.h
        recorder_engine/benchmark/codecbenchmark.h recorder_engine/benchmark/codecbenchmark.cpp
        recorder_engine/benchmark/syntheticframes.h recorder_engine/benchmark/syntheticframes.cpp
        recorder_engine/benchmark/realcodecrunners.h recorder_engine/benchmark/realcodecrunners.cpp
        recorder_engine/benchmark/benchmarkcache.h recorder_engine/benchmark/benchmarkcache.cpp
        recorder_engine/benchmark/runcodecbenchmark.h recorder_engine/benchmark/runcodecbenchmark.cpp
        recorder_engine/benchmark/recordgate.h recorder_engine/benchmark/recordgate.cpp
        recorder_engine/ingest/h26xseitimecode.h recorder_engine/ingest/h26xseitimecode.cpp
        recorder_engine/ingest/rtmpprotocol.h recorder_engine/ingest/rtmpprotocol.cpp
        recorder_engine/ingest/nativeaacdecoder.h
        recorder_engine/ingest/nativeframecopy.h recorder_engine/ingest/nativeframecopy.cpp
        recorder_engine/ingest/ndiframeconvert.h recorder_engine/ingest/ndiframeconvert.cpp
        recorder_engine/ingest/nativevideodecoder.h
        recorder_engine/ingest/nativesrtconnectdiagnostics.h recorder_engine/ingest/nativesrtconnectdiagnostics.cpp
        recorder_engine/ingest/nativesrtaddress.h recorder_engine/ingest/nativesrtaddress.cpp
        recorder_engine/ingest/nativesrturloptions.h recorder_engine/ingest/nativesrturloptions.cpp
        recorder_engine/ingest/nativesrtingestsession.h
        recorder_engine/ingest/nativertmpingestsession.h
        recorder_engine/ingest/nativendiingestsession.h recorder_engine/ingest/nativendiingestsession.cpp
        settingsmanager.h settingsmanager.cpp
        uimanager.h uimanager.cpp
        playback/playbackworker.h playback/playbackworker.cpp
        playback/frameindex.h playback/frameindex.cpp
        playback/cutschedule.h playback/cutschedule.cpp
        playback/replayplaylist.h playback/replayplaylist.cpp
        playback/playlistentriesmodel.h playback/playlistentriesmodel.cpp
        playback/playlistplayout.h playback/playlistplayout.cpp
        playback/framerate.h
        playback/frameprovider.h playback/frameprovider.cpp
        playback/playbacktransport.h playback/playbacktransport.cpp
        playback/telemetrytimelinereader.h playback/telemetrytimelinereader.cpp
        playback/audioplayer.h playback/audioplayer.cpp
        playback/trackbuffer.h playback/trackbuffer.cpp
        playback/audioframequeue.h playback/audioframequeue.cpp
        playback/output/outputtypes.h
        playback/output/outputframeclock.h playback/output/outputframeclock.cpp
        playback/output/framepixelformat.h playback/output/colormetadata.h
        playback/output/formatcanon.h playback/output/formatcanon.cpp
        playback/output/colormetadatapolicy.h playback/output/colormetadatapolicy.cpp
        playback/output/framehandle.h playback/output/framehandle.cpp
        playback/output/mediaframe.h
        playback/output/outputframecache.h playback/output/outputframecache.cpp
        playback/output/outputtargetassignment.h playback/output/outputtargetassignment.cpp
        playback/output/broadcastoutputsettings.h playback/output/broadcastoutputsettings.cpp
        playback/output/broadcastoutputstatus.h playback/output/broadcastoutputstatus.cpp
        playback/output/outputbusengine.h playback/output/outputbusengine.cpp
        playback/output/outputsink.h
        playback/output/gpureadbacktelemetry.h playback/output/gpureadbacktelemetry.cpp
        playback/output/outputdispatcher.h playback/output/outputdispatcher.cpp
        playback/output/outputruntime.h playback/output/outputruntime.cpp
        playback/output/queuedoutputsink.h playback/output/queuedoutputsink.cpp
        playback/output/yuv420pcompositor.h playback/output/yuv420pcompositor.cpp
        playback/output/qtpreviewsink.h playback/output/qtpreviewsink.cpp
        playback/output/ndiabi.h
        playback/output/ndiruntimepaths.h
        playback/output/ndisink.h playback/output/ndisink.cpp
        project/projectsettingsimporter.h project/projectsettingsimporter.cpp
        project/projectimportclient.h project/projectimportclient.cpp
        telemetry/telemetryevent.h
        telemetry/sseparser.h telemetry/sseparser.cpp
        telemetry/telemetryclient.h telemetry/telemetryclient.cpp
    RESOURCES
        qtquickcontrols2.conf
        resources.qrc
)

# Generate qmllint.ini with stable import paths and QML dirs for all installed kits.
set(QMLLINT_IMPORT_PATHS "build/qmllint-imports")
if (Qt6_DIR)
    # Add the active kit's QML dir.
    get_filename_component(_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
    set(_qt_qml_dir "${_qt_prefix}/qml")
    if (EXISTS "${_qt_qml_dir}")
        list(APPEND QMLLINT_IMPORT_PATHS "${_qt_qml_dir}")
    endif()
    # Also add sibling kits (e.g. macos, ios) so the .ini works for both targets.
    get_filename_component(_qt_version_dir "${_qt_prefix}/.." ABSOLUTE)
    file(GLOB _qt_kit_dirs "${_qt_version_dir}/*")
    foreach(_kit_dir ${_qt_kit_dirs})
        set(_kit_qml_dir "${_kit_dir}/qml")
        if (EXISTS "${_kit_qml_dir}" AND NOT "${_kit_qml_dir}" STREQUAL "${_qt_qml_dir}")
            list(APPEND QMLLINT_IMPORT_PATHS "${_kit_qml_dir}")
        endif()
    endforeach()
endif()
string(JOIN ";" QMLLINT_IMPORT_PATHS ${QMLLINT_IMPORT_PATHS})
configure_file(
    "${CMAKE_SOURCE_DIR}/qmllint.ini.in"
    "${CMAKE_SOURCE_DIR}/qmllint.ini"
    @ONLY
)

# Keep a stable QML import path for qmllint (symlink to the active build).
# This avoids hard-coding kit-specific build directories in qmllint.ini.
# Skipped on Windows: cmake -E create_symlink needs Developer Mode/admin there,
# and qmllint is only run on the macOS CI leg, not the Windows app build.
set(QMLLINT_IMPORT_LINK "${CMAKE_SOURCE_DIR}/build/qmllint-imports")
if(NOT WIN32)
    add_custom_target(qmllint_imports ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/build"
        COMMAND ${CMAKE_COMMAND} -E remove -f "${QMLLINT_IMPORT_LINK}"
        # Point at the unified QML output dir so qmllint resolves every module
        # (OpenLiveReplay, OlrTheme, OlrStyle) under one import root.
        COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_BINARY_DIR}/qml" "${QMLLINT_IMPORT_LINK}"
        COMMENT "Updating qmllint import path symlink"
    )
endif()

# Ensure qmllint can find the generated qmltypes file in the module output directory.
add_custom_command(TARGET OpenLiveReplay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/qml/OpenLiveReplay"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/.qt/qmltypes/OpenLiveReplay.qmltypes"
        "${CMAKE_BINARY_DIR}/qml/OpenLiveReplay/OpenLiveReplay.qmltypes"
    COMMENT "Copying OpenLiveReplay.qmltypes for qmllint"
)

add_library(OpenLiveReplayMidi STATIC
    midi/midimanager.h
    midi/midimanager.cpp
)

target_link_libraries(OpenLiveReplayMidi PRIVATE
    Qt6::Core
    rtmidi
    olr_warnings
    olr_sanitize
)

# ==============================================================================
# Stream Deck (Elgato) support — iPadOS only; stub elsewhere.
#
# OFF by default: the Stream Deck driver link requires the
# com.apple.developer.driverkit.communicates-with-drivers entitlement, which
# Apple only provisions for paid Apple Developer Program teams (personal/free
# teams are refused, so the app can't be signed/installed with it enabled).
# Enable explicitly on a paid team:  -DOLR_ENABLE_STREAMDECK=ON
# ==============================================================================
if(IOS)
    option(OLR_ENABLE_STREAMDECK "Enable Elgato Stream Deck support (needs paid Apple Developer team)" OFF)
else()
    set(OLR_ENABLE_STREAMDECK OFF)
endif()

target_sources(OpenLiveReplay PRIVATE
    streamdeck/streamdeckmanager.h
    streamdeck/streamdeckmappingstore.h
    streamdeck/streamdeckmappingstore.cpp)
if(OLR_ENABLE_STREAMDECK)
    target_sources(OpenLiveReplay PRIVATE streamdeck/streamdeckmanager.mm)
else()
    target_sources(OpenLiveReplay PRIVATE streamdeck/streamdeckmanager_stub.cpp)
endif()

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
        message(FATAL_ERROR "OpenLiveReplay Windows app requires OLR_FFMPEG_ROOT with FFmpeg include and library paths")
    endif()

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

if(APPLE)
    target_sources(OpenLiveReplay PRIVATE
        recorder_engine/ingest/colortags_apple.mm
        recorder_engine/ingest/nativevideodecoder_videotoolbox.mm
        recorder_engine/ingest/nativesrtingestsession.cpp
        recorder_engine/ingest/nativertmpingestsession.cpp
        recorder_engine/ingest/nativeaacdecoder_audiotoolbox.mm
        recorder_engine/codec/nativevideoencoder_videotoolbox.mm
        playback/gpu/gpusurface.h playback/gpu/gpusurface_apple.mm
        playback/gpu/gpupipelineconfig.h playback/gpu/gpupipelineconfig.cpp
        playback/gpu/appleiosurface.h playback/gpu/applegpusurface_apple.mm)
    target_compile_definitions(OpenLiveReplay PRIVATE
        OLR_NATIVE_SRT_AVAILABLE=1
        OLR_NATIVE_RTMP_AVAILABLE=1)
    target_link_libraries(OpenLiveReplay PRIVATE
        "-framework AudioToolbox"
        "-framework VideoToolbox"
        "-framework CoreMedia"
        "-framework CoreFoundation"
        "-framework IOSurface"
        "-framework CoreVideo")
elseif(WIN32)
    target_sources(OpenLiveReplay PRIVATE
        recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp
        recorder_engine/ingest/nativesrtingestsession.cpp
        recorder_engine/ingest/nativertmpingestsession.cpp
        recorder_engine/ingest/nativeaacdecoder_mediafoundation.cpp
        recorder_engine/codec/nativevideoencoder_mediafoundation.cpp
        playback/gpu/gpusurface.h playback/gpu/gpusurface_stub.cpp
        playback/gpu/gpupipelineconfig.h playback/gpu/gpupipelineconfig.cpp
        playback/output/win/wingpuimportedge.cpp
        playback/output/win/d3d11gpusurface.cpp
        playback/output/win/d3dfence.cpp)
    target_compile_definitions(OpenLiveReplay PRIVATE
        OLR_NATIVE_SRT_AVAILABLE=1
        OLR_NATIVE_RTMP_AVAILABLE=1)
    target_include_directories(OpenLiveReplay PRIVATE
        "${OLR_FFMPEG_INCLUDE}"
        "${OLR_SRT_INCLUDE_DIR}")
    target_link_libraries(OpenLiveReplay PRIVATE
        "${OLR_FFMPEG_AVFORMAT_LIBRARY}"
        "${OLR_FFMPEG_AVCODEC_LIBRARY}"
        "${OLR_FFMPEG_AVUTIL_LIBRARY}"
        "${OLR_FFMPEG_SWSCALE_LIBRARY}"
        "${OLR_FFMPEG_SWRESAMPLE_LIBRARY}"
        mfplat mf mfuuid mfreadwrite strmiids d3d11 dxgi ole32 ws2_32 "${OLR_SRT_LIBRARY}")
elseif(NOT WIN32)
    target_sources(OpenLiveReplay PRIVATE
        recorder_engine/ingest/nativevideodecoder_stub.cpp
        recorder_engine/ingest/nativeaacdecoder_stub.cpp
        recorder_engine/codec/nativevideoencoder_stub.cpp
        playback/gpu/gpusurface.h playback/gpu/gpusurface_stub.cpp
        playback/gpu/gpupipelineconfig.h playback/gpu/gpupipelineconfig.cpp
        playback/output/win/wingpuimportedge_stub.cpp)
endif()

if(OLR_GPU_PIPELINE)
    target_compile_definitions(OpenLiveReplay PRIVATE OLR_GPU_PIPELINE_BUILD=1)
    target_link_libraries(OpenLiveReplay PRIVATE Qt6::GuiPrivate)
    if(APPLE)
        target_sources(OpenLiveReplay PRIVATE
            playback/gpu/gpurhicontext.h playback/gpu/gpurhicontext_apple.mm
            playback/gpu/gpuframedata.h playback/gpu/gpuframedata.cpp
            playback/gpu/decodedonefence.h playback/gpu/decodedonefence_apple.mm
            playback/gpu/vtkeepsurfaceimporter.h playback/gpu/vtkeepsurfaceimporter_apple.mm)
        target_link_libraries(OpenLiveReplay PRIVATE "-framework Metal" "-framework QuartzCore")
    else()
        target_sources(OpenLiveReplay PRIVATE
            playback/gpu/gpurhicontext.h playback/gpu/gpurhicontext_stub.cpp
            playback/gpu/gpuframedata.h playback/gpu/gpuframedata.cpp
            playback/gpu/decodedonefence.h playback/gpu/decodedonefence_stub.cpp)
    endif()
endif()

# 4. Handle Platform Specifics (FFmpeg)
if(IOS)
    target_sources(OpenLiveReplay PRIVATE
        ios/ios_scene.h
        ios/ios_scene.mm
    )
    set_target_properties(OpenLiveReplay PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/ios/Info.plist"
    )
    # ==============================================================================
    # INTEGRATED FFMPEG IOS BUILDER
    # ==============================================================================

    # 1. DEFINE PATHS
    set(FFMPEG_SCRIPT "${CMAKE_SOURCE_DIR}/build-scripts/build_ffmpeg_ios_srt.sh")
    set(FFMPEG_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/ios_build/xcframeworks")

    # Define the expected artifacts (used to check if build is needed)
    set(FFMPEG_CONFIG_STAMP "${FFMPEG_OUTPUT_DIR}/.ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter.stamp")
    set(FFMPEG_EXPECTED_ARCHIVES
        "${FFMPEG_OUTPUT_DIR}/libavcodec.xcframework/ios-arm64/libavcodec.a"
        "${FFMPEG_OUTPUT_DIR}/libavcodec.xcframework/ios-arm64-simulator/libavcodec.a"
        "${FFMPEG_OUTPUT_DIR}/libavformat.xcframework/ios-arm64/libavformat.a"
        "${FFMPEG_OUTPUT_DIR}/libavformat.xcframework/ios-arm64-simulator/libavformat.a"
        "${FFMPEG_OUTPUT_DIR}/libavutil.xcframework/ios-arm64/libavutil.a"
        "${FFMPEG_OUTPUT_DIR}/libavutil.xcframework/ios-arm64-simulator/libavutil.a"
        "${FFMPEG_OUTPUT_DIR}/libswscale.xcframework/ios-arm64/libswscale.a"
        "${FFMPEG_OUTPUT_DIR}/libswscale.xcframework/ios-arm64-simulator/libswscale.a"
        "${FFMPEG_OUTPUT_DIR}/libswresample.xcframework/ios-arm64/libswresample.a"
        "${FFMPEG_OUTPUT_DIR}/libswresample.xcframework/ios-arm64-simulator/libswresample.a"
        "${FFMPEG_OUTPUT_DIR}/libsrt.xcframework/ios-arm64/libsrt.a"
        "${FFMPEG_OUTPUT_DIR}/libsrt.xcframework/ios-arm64-simulator/libsrt.a"
    )

    # 2. DEFINE A CUSTOM BUILD TARGET
    # Always invoke the script from Xcode/CMake; the script owns the config-id
    # cache validation and exits quickly when the checked artifacts are current.
    add_custom_target(BuildFFmpegDependencies
        BYPRODUCTS ${FFMPEG_EXPECTED_ARCHIVES}
        COMMAND chmod +x ${FFMPEG_SCRIPT}
        COMMAND ${FFMPEG_SCRIPT}
        DEPENDS "${FFMPEG_SCRIPT}"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "[FFmpeg] Binaries missing. Compiling FFmpeg+SRT for iOS... (This takes ~20 mins)"
        VERBATIM
    )

    # ==============================================================================
    # 4. IMPORT THE LIBRARIES AS STATIC XCFRAMEWORK SLICES
    # ==============================================================================
    set(FFMPEG_IOS_DIR "${FFMPEG_OUTPUT_DIR}")
    set(FFMPEG_XCODE_SLICE "$(OLR_FFMPEG_SLICE)")

    add_library(ffmpeg_avcodec STATIC IMPORTED)
    set_target_properties(ffmpeg_avcodec PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_IOS_DIR}/libavcodec.xcframework/${FFMPEG_XCODE_SLICE}/libavcodec.a"
    )
    add_dependencies(ffmpeg_avcodec BuildFFmpegDependencies)

    add_library(ffmpeg_avformat STATIC IMPORTED)
    set_target_properties(ffmpeg_avformat PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_IOS_DIR}/libavformat.xcframework/${FFMPEG_XCODE_SLICE}/libavformat.a"
    )
    add_dependencies(ffmpeg_avformat BuildFFmpegDependencies)

    add_library(ffmpeg_avutil STATIC IMPORTED)
    set_target_properties(ffmpeg_avutil PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_IOS_DIR}/libavutil.xcframework/${FFMPEG_XCODE_SLICE}/libavutil.a"
    )
    add_dependencies(ffmpeg_avutil BuildFFmpegDependencies)

    add_library(ffmpeg_swscale STATIC IMPORTED)
    set_target_properties(ffmpeg_swscale PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_IOS_DIR}/libswscale.xcframework/${FFMPEG_XCODE_SLICE}/libswscale.a"
    )
    add_dependencies(ffmpeg_swscale BuildFFmpegDependencies)

    add_library(ffmpeg_swresample STATIC IMPORTED)
    set_target_properties(ffmpeg_swresample PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_IOS_DIR}/libswresample.xcframework/${FFMPEG_XCODE_SLICE}/libswresample.a"
    )
    add_dependencies(ffmpeg_swresample BuildFFmpegDependencies)

    add_library(ffmpeg_srt STATIC IMPORTED)
    set_target_properties(ffmpeg_srt PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_IOS_DIR}/libsrt.xcframework/${FFMPEG_XCODE_SLICE}/libsrt.a"
    )
    add_dependencies(ffmpeg_srt BuildFFmpegDependencies)

    target_include_directories(OpenLiveReplay PRIVATE
        "${FFMPEG_IOS_DIR}/libavcodec.xcframework/${FFMPEG_XCODE_SLICE}/Headers"
        "${FFMPEG_IOS_DIR}/libavformat.xcframework/${FFMPEG_XCODE_SLICE}/Headers"
        "${FFMPEG_IOS_DIR}/libavutil.xcframework/${FFMPEG_XCODE_SLICE}/Headers"
        "${FFMPEG_IOS_DIR}/libswscale.xcframework/${FFMPEG_XCODE_SLICE}/Headers"
        "${FFMPEG_IOS_DIR}/libswresample.xcframework/${FFMPEG_XCODE_SLICE}/Headers"
        "${FFMPEG_IOS_DIR}/libsrt.xcframework/${FFMPEG_XCODE_SLICE}/Headers"
    )

    set_property(TARGET OpenLiveReplay PROPERTY XCODE_ATTRIBUTE_OLR_FFMPEG_SLICE "ios-arm64")
    set_property(TARGET OpenLiveReplay PROPERTY "XCODE_ATTRIBUTE_OLR_FFMPEG_SLICE[sdk=iphoneos*]" "ios-arm64")
    set_property(TARGET OpenLiveReplay PROPERTY "XCODE_ATTRIBUTE_OLR_FFMPEG_SLICE[sdk=iphonesimulator*]" "ios-arm64-simulator")

    # ==============================================================================
    # 5. LINK TO YOUR APP
    # ==============================================================================
    add_dependencies(OpenLiveReplay BuildFFmpegDependencies)

    target_link_libraries(OpenLiveReplay PRIVATE
        ffmpeg_avformat
        ffmpeg_avcodec
        ffmpeg_swscale
        ffmpeg_swresample
        ffmpeg_avutil
        ffmpeg_srt
        "-framework AudioToolbox"
        "-framework CoreMIDI"
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework Security"
        "-framework VideoToolbox"
        "-framework CoreMedia"
        "-framework AVFoundation"
        "-framework CoreVideo"
        "-lz" "-lbz2" "-liconv" "-lc++"
    )

    # ==============================================================================
    # 6. EMBED RUNTIME DYLIBS INTO APP BUNDLE
    # ==============================================================================
    set_target_properties(OpenLiveReplay PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/Frameworks"
        XCODE_EMBED_FRAMEWORKS "rtmidi"
        XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY TRUE
    )

    # Fix rtmidi: Xcode copies librtmidi.7.0.0.dylib but install name expects librtmidi.7.dylib
    set(RTMIDI_FIX_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/fix_rtmidi_embed.sh")
    file(WRITE "${RTMIDI_FIX_SCRIPT}" "#!/bin/bash
set -euo pipefail
FW_DIR=\"\${BUILT_PRODUCTS_DIR}/\${FRAMEWORKS_FOLDER_PATH}\"
if [ -f \"\${FW_DIR}/librtmidi.7.0.0.dylib\" ]; then
    cd \"\${FW_DIR}\"
    rm -f librtmidi.7.dylib librtmidi.dylib
    mv librtmidi.7.0.0.dylib librtmidi.7.dylib
fi
")
    set_target_properties(OpenLiveReplay PROPERTIES
        XCODE_ATTRIBUTE_OPENLIVEREPLAY_FIX_RTMIDI_EMBED "YES"
    )
    add_custom_command(TARGET OpenLiveReplay POST_BUILD
        COMMAND bash "${RTMIDI_FIX_SCRIPT}"
        COMMENT "[rtmidi] Renaming librtmidi.7.0.0.dylib -> librtmidi.7.dylib in Frameworks"
    )

    # ==============================================================================
    # STREAM DECK BRIDGE (Swift framework, see ios/streamdeck-bridge)
    # ==============================================================================
    if(OLR_ENABLE_STREAMDECK)
        set(STREAMDECK_SCRIPT "${CMAKE_SOURCE_DIR}/build-scripts/build_streamdeck_bridge.sh")
        set(STREAMDECK_XCF "${CMAKE_SOURCE_DIR}/ios_build/xcframeworks/StreamDeckBridge.xcframework")
        set(STREAMDECK_FRAMEWORK "${STREAMDECK_XCF}/ios-arm64/StreamDeckBridge.framework")

        add_custom_command(
            OUTPUT "${STREAMDECK_XCF}"
            COMMAND chmod +x ${STREAMDECK_SCRIPT}
            COMMAND ${STREAMDECK_SCRIPT}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "[StreamDeckBridge] Building Swift bridge xcframework (requires xcodegen)..."
            VERBATIM
        )
        add_custom_target(BuildStreamDeckBridge DEPENDS "${STREAMDECK_XCF}")
        add_dependencies(OpenLiveReplay BuildStreamDeckBridge)

        # The .mm uses ObjC blocks capturing C++ objects — compile with ARC.
        set_source_files_properties(streamdeck/streamdeckmanager.mm PROPERTIES
            COMPILE_OPTIONS "-fobjc-arc;-F${STREAMDECK_XCF}/ios-arm64"
        )
        target_link_libraries(OpenLiveReplay PRIVATE
            "-F${STREAMDECK_XCF}/ios-arm64"
            "-framework StreamDeckBridge"
        )
        set_property(TARGET OpenLiveReplay APPEND_STRING PROPERTY
            XCODE_EMBED_FRAMEWORKS ";${STREAMDECK_FRAMEWORK}")

        # Required for talking to the Stream Deck driver. The app's signing
        # identity must have the "Communicates with Drivers" capability
        # (automatic signing adds it for Xcode-managed profiles).
        set_target_properties(OpenLiveReplay PROPERTIES
            XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_SOURCE_DIR}/ios/OpenLiveReplay.entitlements"
        )
    endif()

elseif(APPLE) # macOS Desktop
    if(OLR_FFMPEG_ROOT AND OLR_SRT_ROOT)
        # Controlled, LGPL, SRT-enabled ffmpeg built from source
        # (build-scripts/build_ffmpeg_macos_app_srt.sh). Preferred for releases.
        target_include_directories(OpenLiveReplay PRIVATE
            "${OLR_FFMPEG_ROOT}/include"
            "${OLR_SRT_ROOT}/include")
        target_link_directories(OpenLiveReplay PRIVATE
            "${OLR_FFMPEG_ROOT}/lib"
            "${OLR_SRT_ROOT}/lib")
        set_target_properties(OpenLiveReplay PROPERTIES
            INSTALL_RPATH "${OLR_FFMPEG_ROOT}/lib;${OLR_SRT_ROOT}/lib")
    else()
        # Fallback: Homebrew, for local dev convenience only. NOTE: brew ffmpeg is
        # --enable-gpl and ships libx264/libx265 — not suitable for redistribution;
        # set OLR_FFMPEG_ROOT/OLR_SRT_ROOT (from-source) for release builds.
        if(NOT DEFINED OLR_BREW_PREFIX)
            execute_process(COMMAND brew --prefix
                OUTPUT_VARIABLE OLR_BREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        endif()
        if(NOT OLR_BREW_PREFIX OR NOT EXISTS "${OLR_BREW_PREFIX}")
            set(OLR_BREW_PREFIX /opt/homebrew)
        endif()
        target_include_directories(OpenLiveReplay PRIVATE
            "${OLR_BREW_PREFIX}/include"
            "${OLR_BREW_PREFIX}/opt/srt/include")
        target_link_directories(OpenLiveReplay PRIVATE
            "${OLR_BREW_PREFIX}/lib"
            "${OLR_BREW_PREFIX}/opt/srt/lib")
        set_target_properties(OpenLiveReplay PROPERTIES INSTALL_RPATH "${OLR_BREW_PREFIX}/lib")
    endif()
    target_link_libraries(OpenLiveReplay PRIVATE avformat avcodec avutil swscale swresample srt)
    target_link_libraries(OpenLiveReplay PRIVATE "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation")
elseif(UNIX AND NOT APPLE) # Linux desktop (CI second toolchain; system FFmpeg)
    # FFmpeg from the distro (apt libav*-dev) via pkg-config. Native SRT/RTMP
    # ingest is not compiled on Linux (cross-platform stubs), so libsrt is not
    # linked into the app here.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OLR_FFMPEG REQUIRED IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale libswresample)
    target_link_libraries(OpenLiveReplay PRIVATE PkgConfig::OLR_FFMPEG)
endif()

# 5. Standard Qt Linking
target_link_libraries(OpenLiveReplay PRIVATE
    Qt6::Core
    Qt6::Quick
    Qt6::Concurrent
    Qt6::Multimedia
    Qt6::QuickControls2
    Qt6::Network
    Qt6::WebSockets
    rtmidi
    OpenLiveReplayMidi
    OlrTheme
    OlrStyle
    olr_warnings
    olr_sanitize
)
if(WIN32)
    target_link_libraries(OpenLiveReplay PRIVATE ws2_32)
endif()

# 6. Bundle Properties
set_target_properties(OpenLiveReplay PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER com.timokorkalainen.OpenLiveReplay
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.timokorkalainen.OpenLiveReplay
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE TRUE
)

# On Linux the qt_add_qml_module(URI OpenLiveReplay) output directory
# build/OpenLiveReplay/ collides with the plain executable of the same name
# ("ld: cannot open output file OpenLiveReplay: Is a directory"). macOS builds a
# .app bundle and Windows an .exe, so only Linux clashes — emit the binary into a
# subdirectory there.
if(UNIX AND NOT APPLE)
    set_target_properties(OpenLiveReplay PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# ---------------------------------------------------------------------------
# 7. Tests (opt-in: -DOLR_BUILD_TESTS=ON). Desktop/macOS only — the test
#    targets link homebrew FFmpeg and run headless, so they are skipped for
#    the iOS cross-build.
# ---------------------------------------------------------------------------
if(OLR_BUILD_TESTS AND NOT IOS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(OLR_BUILD_GPU_PROBES AND NOT IOS)
    add_subdirectory(tests/probes)
endif()

# libFuzzer harnesses for the untrusted-input parsers (opt-in; needs clang +
# libFuzzer). Independent of OLR_BUILD_TESTS and built with their own
# fuzzer+ASan+UBSan instrumentation — see tests/fuzz/CMakeLists.txt.
if(OLR_BUILD_FUZZERS AND NOT IOS)
    add_subdirectory(tests/fuzz)
endif()
