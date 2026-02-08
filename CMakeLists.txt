cmake_minimum_required(VERSION 3.16)
project(OpenLiveReplay VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
find_package(Qt6 REQUIRED COMPONENTS Quick Core Multimedia Concurrent QuickControls2)
qt_standard_project_setup(REQUIRES 6.8)

# 2. Create the Executable Target FIRST
qt_add_executable(appOpenLiveReplay main.cpp)

# 3. Define QML Module and Sources
qt_add_qml_module(appOpenLiveReplay
    URI OpenLiveReplay
    VERSION 1.0
    QML_FILES
        Main.qml
    SOURCES
        recorder_engine/replaymanager.cpp recorder_engine/replaymanager.h
        recorder_engine/muxer.h recorder_engine/muxer.cpp
        recorder_engine/streamworker.h recorder_engine/streamworker.cpp
        recorder_engine/recordingclock.h recorder_engine/recordingclock.cpp
        settingsmanager.h settingsmanager.cpp
        uimanager.h uimanager.cpp
        playback/playbackworker.h playback/playbackworker.cpp
        playback/frameprovider.h playback/frameprovider.cpp
        playback/playbacktransport.h playback/playbacktransport.cpp
    RESOURCES
        qtquickcontrols2.conf
        resources.qrc
)

add_library(OpenLiveReplayMidi STATIC
    midi/midimanager.h
    midi/midimanager.cpp
)

target_link_libraries(OpenLiveReplayMidi PRIVATE
    Qt6::Core
    rtmidi
)

# 4. Handle Platform Specifics (FFmpeg)
if(IOS)
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
    # 4. IMPORT THE LIBRARIES (Explicitly)
    # ==============================================================================
    function(add_ffmpeg_xcframework NAME FILENAME)
        add_library(${NAME} INTERFACE IMPORTED)
        set_target_properties(${NAME} PROPERTIES
            INTERFACE_LINK_LIBRARIES "${FFMPEG_OUTPUT_DIR}/${FILENAME}"
            INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_OUTPUT_DIR}"
        )
        add_dependencies(${NAME} BuildFFmpegDependencies)
    endfunction()

    add_ffmpeg_xcframework(FFMPEG::avcodec    "libavcodec.xcframework")
    add_ffmpeg_xcframework(FFMPEG::avformat   "libavformat.xcframework")
    add_ffmpeg_xcframework(FFMPEG::avutil     "libavutil.xcframework")
    add_ffmpeg_xcframework(FFMPEG::swscale    "libswscale.xcframework")
    add_ffmpeg_xcframework(FFMPEG::swresample "libswresample.xcframework")
    add_ffmpeg_xcframework(FFMPEG::srt        "libsrt.xcframework")

    # ==============================================================================
    # 5. LINK TO YOUR APP
    # ==============================================================================
    add_dependencies(appOpenLiveReplay BuildFFmpegDependencies)

    target_link_libraries(appOpenLiveReplay PRIVATE
        FFMPEG::avcodec
        FFMPEG::avformat
        FFMPEG::avutil
        FFMPEG::swscale
        FFMPEG::swresample
        FFMPEG::srt
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

    set_target_properties(appOpenLiveReplay PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/Frameworks"
    )

elseif(APPLE) # macOS Desktop
    target_include_directories(appOpenLiveReplay PRIVATE /opt/homebrew/include)
    target_link_directories(appOpenLiveReplay PRIVATE /opt/homebrew/lib)
    target_link_libraries(appOpenLiveReplay PRIVATE avformat avcodec avutil swscale swresample)
    set_target_properties(appOpenLiveReplay PROPERTIES INSTALL_RPATH "/opt/homebrew/lib")
    target_link_libraries(appOpenLiveReplay PRIVATE "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation")
endif()

# 5. Standard Qt Linking
target_link_libraries(appOpenLiveReplay PRIVATE
    Qt6::Core
    Qt6::Quick
    Qt6::Concurrent
    Qt6::Multimedia
    Qt6::QuickControls2
    rtmidi
    OpenLiveReplayMidi
)

# 6. Bundle Properties
set_target_properties(appOpenLiveReplay PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER com.timokorkalainen.appOpenLiveReplay
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.timokorkalainen.appOpenLiveReplay
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE TRUE
)
