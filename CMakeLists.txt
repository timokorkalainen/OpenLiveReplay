cmake_minimum_required(VERSION 3.16)

project(OpenLiveReplay VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick Core Multimedia Concurrent)

qt_standard_project_setup(REQUIRES 6.8)


# macOS Homebrew Paths
include_directories(/opt/homebrew/include)
link_directories(/opt/homebrew/lib)

set(FFMPEG_LIBS
    avcodec
    avformat
    avutil
    swscale
)

qt_add_executable(appOpenLiveReplay
    main.cpp
)

qt_add_qml_module(appOpenLiveReplay
    URI OpenLiveReplay
    QML_FILES
        Main.qml
        SOURCES recorder_engine/replaymanager.cpp
        SOURCES recorder_engine/replaymanager.h
        SOURCES recorder_engine/muxer.h recorder_engine/muxer.cpp
        SOURCES recorder_engine/streamworker.h recorder_engine/streamworker.cpp
        SOURCES
        SOURCES recorder_engine/recordingclock.h recorder_engine/recordingclock.cpp
)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
set_target_properties(appOpenLiveReplay PROPERTIES
#    MACOSX_BUNDLE_GUI_IDENTIFIER com.example.appOpenLiveReplay
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

# Ensure the executable finds the libs at runtime
set_target_properties(appOpenLiveReplay PROPERTIES INSTALL_RPATH "/opt/homebrew/lib")

target_link_libraries(appOpenLiveReplay
    PRIVATE Qt6::Core
    PRIVATE Qt6::Quick
    PRIVATE Qt6::Concurrent
    PRIVATE Qt6::Multimedia
    ${FFMPEG_LIBS}
)

include(GNUInstallDirs)
install(TARGETS appOpenLiveReplay
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
