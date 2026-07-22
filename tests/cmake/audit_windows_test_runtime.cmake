foreach(required TEST_BINARY_DIR QT_RUNTIME_DIR QT_PLUGIN_DIR FFMPEG_RUNTIME_DIR SRT_RUNTIME_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

# This script is itself launched through olr_add_ctest, so its effective
# environment proves both the modern ENVIRONMENT_MODIFICATION path and the
# CMake 3.16-compatible ENVIRONMENT fallback. Normalize separators before
# comparing the controlled precedence.
string(REPLACE "\\" "/" effective_path "$ENV{PATH}")
string(REPLACE "\\" "/" effective_plugins "$ENV{QT_PLUGIN_PATH}")
string(REPLACE "\\" "/" normalized_qt_runtime "${QT_RUNTIME_DIR}")
string(REPLACE "\\" "/" normalized_ffmpeg_runtime "${FFMPEG_RUNTIME_DIR}")
string(REPLACE "\\" "/" normalized_srt_runtime "${SRT_RUNTIME_DIR}")
string(REPLACE "\\" "/" normalized_qt_plugins "${QT_PLUGIN_DIR}")
string(FIND "${effective_path}" "${normalized_qt_runtime}" qt_runtime_offset)
string(FIND "${effective_path}" "${normalized_ffmpeg_runtime}" ffmpeg_runtime_offset)
string(FIND "${effective_path}" "${normalized_srt_runtime}" srt_runtime_offset)
if(NOT qt_runtime_offset EQUAL 0 OR
   NOT qt_runtime_offset LESS ffmpeg_runtime_offset OR
   NOT ffmpeg_runtime_offset LESS srt_runtime_offset)
    message(FATAL_ERROR
        "effective PATH does not prioritize Qt, FFmpeg, then SRT: ${effective_path}")
endif()
string(FIND "${effective_plugins}" "${normalized_qt_plugins}" qt_plugin_offset)
if(NOT qt_plugin_offset EQUAL 0)
    message(FATAL_ERROR
        "effective QT_PLUGIN_PATH does not prioritize the controlled Qt plugins: "
        "${effective_plugins}")
endif()

set(required_testfiles
    "CTestTestfile.cmake"
    "e2e/CTestTestfile.cmake"
    "gpu_fault/CTestTestfile.cmake"
    "smoke/CTestTestfile.cmake"
    "unit/CTestTestfile.cmake"
    "qml/CTestTestfile.cmake"
    "qmlstyle/CTestTestfile.cmake")
if(EXISTS "${TEST_BINARY_DIR}/perf/CTestTestfile.cmake")
    list(APPEND required_testfiles "perf/CTestTestfile.cmake")
endif()
set(required_runtime_dirs
    "${QT_RUNTIME_DIR}"
    "${FFMPEG_RUNTIME_DIR}"
    "${SRT_RUNTIME_DIR}")

foreach(relative_testfile IN LISTS required_testfiles)
    set(testfile "${TEST_BINARY_DIR}/${relative_testfile}")
    if(NOT EXISTS "${testfile}")
        message(FATAL_ERROR "generated CTest file is missing: ${testfile}")
    endif()
    file(READ "${testfile}" contents)
    if(contents MATCHES "windows_test_runner\\.ps1")
        message(FATAL_ERROR "${relative_testfile} unexpectedly uses a shell test runner")
    endif()
    file(STRINGS "${testfile}" lines)
    set(current_test "")
    set(current_properties "")
    foreach(line IN LISTS lines)
        if(line MATCHES "^add_test\\(([^ ]+)")
            if(NOT current_test STREQUAL "")
                foreach(runtime_dir IN LISTS required_runtime_dirs)
                    string(FIND "${current_properties}" "${runtime_dir}" path_offset)
                    if(path_offset EQUAL -1)
                        message(FATAL_ERROR
                            "${relative_testfile}:${current_test} lacks runtime directory: "
                            "${runtime_dir}")
                    endif()
                endforeach()
                string(FIND "${current_properties}" "${QT_PLUGIN_DIR}" plugin_offset)
                if(plugin_offset EQUAL -1)
                    message(FATAL_ERROR
                        "${relative_testfile}:${current_test} lacks Qt plugin directory")
                endif()
                string(FIND "${current_properties}"
                    "QT_PLUGIN_PATH=path_list_prepend:${QT_PLUGIN_DIR}" modern_plugin_offset)
                string(FIND "${current_properties}"
                    "QT_PLUGIN_PATH=${QT_PLUGIN_DIR}" legacy_plugin_offset)
                if(modern_plugin_offset EQUAL -1 AND legacy_plugin_offset EQUAL -1)
                    message(FATAL_ERROR
                        "${relative_testfile}:${current_test} does not prioritize the controlled "
                        "Qt plugin directory")
                endif()
            endif()
            set(current_test "${CMAKE_MATCH_1}")
            set(current_properties "")
        elseif(NOT current_test STREQUAL "" AND line MATCHES "^set_tests_properties\\(")
            string(APPEND current_properties "${line}")
        endif()
    endforeach()
    if(NOT current_test STREQUAL "")
        foreach(runtime_dir IN LISTS required_runtime_dirs)
            string(FIND "${current_properties}" "${runtime_dir}" path_offset)
            if(path_offset EQUAL -1)
                message(FATAL_ERROR
                    "${relative_testfile}:${current_test} lacks runtime directory: ${runtime_dir}")
            endif()
        endforeach()
        string(FIND "${current_properties}" "${QT_PLUGIN_DIR}" plugin_offset)
        if(plugin_offset EQUAL -1)
            message(FATAL_ERROR
                "${relative_testfile}:${current_test} lacks Qt plugin directory")
        endif()
        string(FIND "${current_properties}"
            "QT_PLUGIN_PATH=path_list_prepend:${QT_PLUGIN_DIR}" modern_plugin_offset)
        string(FIND "${current_properties}"
            "QT_PLUGIN_PATH=${QT_PLUGIN_DIR}" legacy_plugin_offset)
        if(modern_plugin_offset EQUAL -1 AND legacy_plugin_offset EQUAL -1)
            message(FATAL_ERROR
                "${relative_testfile}:${current_test} does not prioritize the controlled Qt "
                "plugin directory")
        endif()
    endif()
endforeach()

message(STATUS "generated Windows CTest runtime wiring is complete")
