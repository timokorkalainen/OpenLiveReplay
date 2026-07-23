cmake_minimum_required(VERSION 3.16)
project(OlrLegacyRuntimeProbe NONE)

if(NOT DEFINED OLR_SOURCE_DIR)
    message(FATAL_ERROR "OLR_SOURCE_DIR is required")
endif()

enable_testing()
add_library(Qt6::Core SHARED IMPORTED)
set_target_properties(Qt6::Core PROPERTIES
    IMPORTED_LOCATION "C:/olr-probe/qt/bin/Qt6Core.dll")
set(OLR_FFMPEG_AVFORMAT_LIBRARY "C:/olr-probe/ffmpeg/lib/libavformat.dll.a")
set(OLR_SRT_LIBRARY "C:/olr-probe/srt/lib/libsrt.dll.a")
set(QT6_INSTALL_PREFIX "C:/olr-probe/qt")
set(QT6_INSTALL_PLUGINS "plugins")
include("${OLR_SOURCE_DIR}/tests/cmake/OlrWindowsTestRuntime.cmake")

add_test(NAME legacy_runtime_probe
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_LIST_DIR}/validate_env.cmake")
set_tests_properties(legacy_runtime_probe PROPERTIES ENVIRONMENT "OLR_EXISTING=before")

function(apply_forced_legacy_runtime)
    set(CMAKE_VERSION 3.21)
    olr_apply_windows_test_runtime(legacy_runtime_probe)
endfunction()

apply_forced_legacy_runtime()
set_tests_properties(legacy_runtime_probe PROPERTIES ENVIRONMENT "OLR_EXISTING=kept")
apply_forced_legacy_runtime()
