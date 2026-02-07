cmake_minimum_required(VERSION 3.16)
project(OpenLiveReplay VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
        PreviewWindow.qml
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

# 4. Handle Platform Specifics (FFmpeg)
if(IOS)
    # 1. Point this to the folder WHERE your .framework folders are located
        # Example: if your file is /Users/timo/build/libavformat.framework, set path to /Users/timo/build
        set(MY_FFMPEG_DIR "/Users/timo.korkalainen/Development/ffmpeg/installed")

        # 2. Add Include Paths
        # We point directly to the Headers folder inside each framework you built
        target_include_directories(appOpenLiveReplay PRIVATE
            "${MY_FFMPEG_DIR}/include"
        )

        # 3. Link them
        target_link_libraries(appOpenLiveReplay PRIVATE
            "${MY_FFMPEG_DIR}/framework/libavformat.framework"
            "${MY_FFMPEG_DIR}/framework/libavcodec.framework"
            "${MY_FFMPEG_DIR}/framework/libavutil.framework"
            "${MY_FFMPEG_DIR}/framework/libswscale.framework"
            "${MY_FFMPEG_DIR}/framework/libswresample.framework"
        )

        # 4. Embed them into the app bundle so it doesn't crash on launch
        set_target_properties(appOpenLiveReplay PROPERTIES
            XCODE_EMBED_FRAMEWORKS "${MY_FFMPEG_DIR}/framework/libavformat.framework;${MY_FFMPEG_DIR}/framework/libavcodec.framework;${MY_FFMPEG_DIR}/framework/libavutil.framework;${MY_FFMPEG_DIR}/framework/libswscale.framework;${MY_FFMPEG_DIR}/framework/libswresample.framework"
            XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY ON
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "@executable_path/Frameworks"
        )

elseif(APPLE) # macOS Desktop
    target_include_directories(appOpenLiveReplay PRIVATE /opt/homebrew/include)
    target_link_directories(appOpenLiveReplay PRIVATE /opt/homebrew/lib)
    target_link_libraries(appOpenLiveReplay PRIVATE avformat avcodec avutil swscale swresample)
    set_target_properties(appOpenLiveReplay PROPERTIES INSTALL_RPATH "/opt/homebrew/lib")
endif()

# 5. Standard Qt Linking
target_link_libraries(appOpenLiveReplay PRIVATE
    Qt6::Core
    Qt6::Quick
    Qt6::Concurrent
    Qt6::Multimedia
    Qt6::QuickControls2
)

# 6. Bundle Properties
set_target_properties(appOpenLiveReplay PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER com.timokorkalainen.appOpenLiveReplay
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE TRUE
)
