foreach(required TEST_BINARY_DIR QT_RUNTIME_DIR QT_PLUGIN_DIR FFMPEG_RUNTIME_DIR SRT_RUNTIME_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

set(required_testfiles
    "CTestTestfile.cmake"
    "unit/CTestTestfile.cmake"
    "qml/CTestTestfile.cmake"
    "qmlstyle/CTestTestfile.cmake")
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
    foreach(runtime_dir IN LISTS required_runtime_dirs)
        string(FIND "${contents}" "PATH=path_list_prepend:${runtime_dir}" path_offset)
        if(path_offset EQUAL -1)
            message(FATAL_ERROR
                "${relative_testfile} does not prepend runtime PATH directory: ${runtime_dir}")
        endif()
    endforeach()
    string(FIND "${contents}" "QT_PLUGIN_PATH=path_list_append:${QT_PLUGIN_DIR}" plugin_offset)
    if(plugin_offset EQUAL -1)
        message(FATAL_ERROR
            "${relative_testfile} does not append the Qt plugin directory: ${QT_PLUGIN_DIR}")
    endif()
endforeach()

message(STATUS "generated Windows CTest runtime wiring is complete")
