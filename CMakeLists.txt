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
option(OLR_WERROR "Treat warnings as errors for first-party targets" OFF)
set(OLR_SANITIZER "" CACHE STRING
    "Sanitizers to enable, e.g. address, thread, undefined, or 'address;undefined'")

# Warning flags applied to first-party targets only (never third-party deps
# like rtmidi/ffmpeg). Introduced non-fatally: -Werror is opt-in via OLR_WERROR.
add_library(olr_warnings INTERFACE)
if(MSVC)
    target_compile_options(olr_warnings INTERFACE /W4)
else()
    target_compile_options(olr_warnings INTERFACE -Wall -Wextra)
endif()
if(OLR_WERROR AND NOT MSVC)
    target_compile_options(olr_warnings INTERFACE -Werror)
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
set(RTMIDI_API_COREMIDI ON CACHE BOOL "" FORCE)
set(RTMIDI_API_ALSA OFF CACHE BOOL "" FORCE)
set(RTMIDI_API_JACK OFF CACHE BOOL "" FORCE)
set(RTMIDI_API_WINMM OFF CACHE BOOL "" FORCE)
set(RTMIDI_API_DUMMY OFF CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    rtmidi
    GIT_REPOSITORY https://github.com/thestk/rtmidi.git
    GIT_TAG 6.0.0
)
FetchContent_MakeAvailable(rtmidi)

# 1. Find Qt
find_package(Qt6 REQUIRED COMPONENTS Quick Core Multimedia Concurrent QuickControls2 Network)
qt_standard_project_setup(REQUIRES 6.8)

# 2. Create the Executable Target FIRST
qt_add_executable(OpenLiveReplay main.cpp)

# 3. Define QML Module and Sources
qt_add_qml_module(OpenLiveReplay
    URI OpenLiveReplay
    VERSION 1.0
    QML_FILES
        Main.qml
        MultiviewWindow.qml
    SOURCES
        recorder_engine/replaymanager.cpp recorder_engine/replaymanager.h
        recorder_engine/muxer.h recorder_engine/muxer.cpp
        recorder_engine/streamworker.h recorder_engine/streamworker.cpp
        recorder_engine/recordingclock.h recorder_engine/recordingclock.cpp
        recorder_engine/ingest/ingestsession.h recorder_engine/ingest/ingestsession.cpp
        recorder_engine/ingest/ffmpegingestsession.h recorder_engine/ingest/ffmpegingestsession.cpp
        settingsmanager.h settingsmanager.cpp
        uimanager.h uimanager.cpp
        playback/playbackworker.h playback/playbackworker.cpp
        playback/frameprovider.h playback/frameprovider.cpp
        playback/playbacktransport.h playback/playbacktransport.cpp
        playback/telemetrytimelinereader.h playback/telemetrytimelinereader.cpp
        playback/audioplayer.h playback/audioplayer.cpp
        playback/trackbuffer.h playback/trackbuffer.cpp
        playback/audioframequeue.h playback/audioframequeue.cpp
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
set(QMLLINT_IMPORT_LINK "${CMAKE_SOURCE_DIR}/build/qmllint-imports")
add_custom_target(qmllint_imports ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/build"
    COMMAND ${CMAKE_COMMAND} -E remove -f "${QMLLINT_IMPORT_LINK}"
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_BINARY_DIR}/OpenLiveReplay" "${QMLLINT_IMPORT_LINK}"
    COMMENT "Updating qmllint import path symlink"
)

# Ensure qmllint can find the generated qmltypes file in the module output directory.
add_custom_command(TARGET OpenLiveReplay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/OpenLiveReplay"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/.qt/qmltypes/OpenLiveReplay.qmltypes"
        "${CMAKE_BINARY_DIR}/OpenLiveReplay/OpenLiveReplay.qmltypes"
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
    set(EXPECTED_LIBS
        "${FFMPEG_OUTPUT_DIR}/libavcodec.xcframework"
        "${FFMPEG_OUTPUT_DIR}/libsrt.xcframework"
    )

    # 2. CREATE A CUSTOM BUILD STEP
    add_custom_command(
        OUTPUT ${EXPECTED_LIBS}
        COMMAND chmod +x ${FFMPEG_SCRIPT}
        COMMAND ${FFMPEG_SCRIPT}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "[FFmpeg] Binaries missing. Compiling FFmpeg+SRT for iOS... (This takes ~20 mins)"
        VERBATIM
    )

    # 3. DEFINE A TARGET
    add_custom_target(BuildFFmpegDependencies DEPENDS ${EXPECTED_LIBS})

    # ==============================================================================
    # 4. IMPORT THE LIBRARIES AS SHARED (for proper Xcode embedding)
    # ==============================================================================
    set(FFMPEG_IOS_DIR "${FFMPEG_OUTPUT_DIR}")

    # Import each FFmpeg dylib as a proper SHARED IMPORTED target
    add_library(ffmpeg_avcodec SHARED IMPORTED)
    set_target_properties(ffmpeg_avcodec PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_IOS_DIR}/libavcodec.xcframework/ios-arm64/libavcodec.dylib"
    )
    add_dependencies(ffmpeg_avcodec BuildFFmpegDependencies)

    add_library(ffmpeg_avformat SHARED IMPORTED)
    set_target_properties(ffmpeg_avformat PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_IOS_DIR}/libavformat.xcframework/ios-arm64/libavformat.dylib"
    )
    add_dependencies(ffmpeg_avformat BuildFFmpegDependencies)

    add_library(ffmpeg_avutil SHARED IMPORTED)
    set_target_properties(ffmpeg_avutil PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_IOS_DIR}/libavutil.xcframework/ios-arm64/libavutil.dylib"
    )
    add_dependencies(ffmpeg_avutil BuildFFmpegDependencies)

    add_library(ffmpeg_swscale SHARED IMPORTED)
    set_target_properties(ffmpeg_swscale PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_IOS_DIR}/libswscale.xcframework/ios-arm64/libswscale.dylib"
    )
    add_dependencies(ffmpeg_swscale BuildFFmpegDependencies)

    add_library(ffmpeg_swresample SHARED IMPORTED)
    set_target_properties(ffmpeg_swresample PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_IOS_DIR}/libswresample.xcframework/ios-arm64/libswresample.dylib"
    )
    add_dependencies(ffmpeg_swresample BuildFFmpegDependencies)

    # SRT is static
    add_library(ffmpeg_srt STATIC IMPORTED)
    set_target_properties(ffmpeg_srt PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_IOS_DIR}/libsrt.xcframework/ios-arm64/libsrt.a"
    )
    add_dependencies(ffmpeg_srt BuildFFmpegDependencies)

    # Include dirs for headers
    target_include_directories(OpenLiveReplay PRIVATE
        "${FFMPEG_IOS_DIR}/libavcodec.xcframework/ios-arm64/Headers"
        "${FFMPEG_IOS_DIR}/libavformat.xcframework/ios-arm64/Headers"
        "${FFMPEG_IOS_DIR}/libavutil.xcframework/ios-arm64/Headers"
        "${FFMPEG_IOS_DIR}/libswscale.xcframework/ios-arm64/Headers"
        "${FFMPEG_IOS_DIR}/libswresample.xcframework/ios-arm64/Headers"
    )

    # ==============================================================================
    # 5. LINK TO YOUR APP
    # ==============================================================================
    add_dependencies(OpenLiveReplay BuildFFmpegDependencies)

    target_link_libraries(OpenLiveReplay PRIVATE
        ffmpeg_avcodec
        ffmpeg_avformat
        ffmpeg_avutil
        ffmpeg_swscale
        ffmpeg_swresample
        ffmpeg_srt
        "-framework AudioToolbox"
        "-framework CoreMIDI"
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework VideoToolbox"
        "-framework CoreMedia"
        "-framework AVFoundation"
        "-framework CoreVideo"
        "-lz" "-lbz2" "-liconv" "-lc++"
    )

    # ==============================================================================
    # 6. EMBED DYLIBS INTO APP BUNDLE (Xcode native embedding)
    # ==============================================================================
    set_target_properties(OpenLiveReplay PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/Frameworks"
        XCODE_EMBED_FRAMEWORKS
            "${FFMPEG_IOS_DIR}/libavcodec.xcframework/ios-arm64/libavcodec.dylib;${FFMPEG_IOS_DIR}/libavformat.xcframework/ios-arm64/libavformat.dylib;${FFMPEG_IOS_DIR}/libavutil.xcframework/ios-arm64/libavutil.dylib;${FFMPEG_IOS_DIR}/libswscale.xcframework/ios-arm64/libswscale.dylib;${FFMPEG_IOS_DIR}/libswresample.xcframework/ios-arm64/libswresample.dylib;rtmidi"
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
    target_include_directories(OpenLiveReplay PRIVATE /opt/homebrew/include)
    target_link_directories(OpenLiveReplay PRIVATE /opt/homebrew/lib)
    target_link_libraries(OpenLiveReplay PRIVATE avformat avcodec avutil swscale swresample)
    set_target_properties(OpenLiveReplay PROPERTIES INSTALL_RPATH "/opt/homebrew/lib")
    target_link_libraries(OpenLiveReplay PRIVATE "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation")
endif()

# 5. Standard Qt Linking
target_link_libraries(OpenLiveReplay PRIVATE
    Qt6::Core
    Qt6::Quick
    Qt6::Concurrent
    Qt6::Multimedia
    Qt6::QuickControls2
    Qt6::Network
    rtmidi
    OpenLiveReplayMidi
    olr_warnings
    olr_sanitize
)

# 6. Bundle Properties
set_target_properties(OpenLiveReplay PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER com.timokorkalainen.OpenLiveReplay
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.timokorkalainen.OpenLiveReplay
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE TRUE
)

# ---------------------------------------------------------------------------
# 7. Tests (opt-in: -DOLR_BUILD_TESTS=ON). Desktop/macOS only — the test
#    targets link homebrew FFmpeg and run headless, so they are skipped for
#    the iOS cross-build.
# ---------------------------------------------------------------------------
if(OLR_BUILD_TESTS AND NOT IOS)
    enable_testing()
    add_subdirectory(tests)
endif()
