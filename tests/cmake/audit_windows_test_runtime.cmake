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

function(generated_test_name command_name command_line output_name)
    if(command_line MATCHES "^${command_name}\\(\\[=\\[([^]]+)\\]=\\]")
        set(name "${CMAKE_MATCH_1}")
    elseif(command_line MATCHES "^${command_name}\\(([^ \\t\"]+)")
        set(name "${CMAKE_MATCH_1}")
    else()
        message(FATAL_ERROR "cannot parse generated CTest command: ${command_line}")
    endif()
    set(${output_name} "${name}" PARENT_SCOPE)
endfunction()

foreach(relative_testfile IN LISTS required_testfiles)
    set(testfile "${TEST_BINARY_DIR}/${relative_testfile}")
    if(NOT EXISTS "${testfile}")
        message(FATAL_ERROR "generated CTest file is missing: ${testfile}")
    endif()
    file(READ "${testfile}" contents)
    if(contents MATCHES "windows_test_runner\\.ps1")
        message(FATAL_ERROR "${relative_testfile} unexpectedly uses a shell test runner")
    endif()

    file(STRINGS "${testfile}" test_commands REGEX "^add_test\\(")
    file(STRINGS "${testfile}" property_commands REGEX "^set_tests_properties\\(")
    set(saw_ndi_runtime_smoke FALSE)
    foreach(test_command IN LISTS test_commands)
        string(REGEX MATCH "\"[^\"]+\"" executable "${test_command}")
        if(NOT executable MATCHES "\\.exe\"$")
            continue()
        endif()

        generated_test_name("add_test" "${test_command}" test_name)
        if(test_name STREQUAL "windows_test_runtime_audit")
            continue()
        endif()
        if(test_name STREQUAL "tst_ndi_runtime_smoke")
            set(saw_ndi_runtime_smoke TRUE)
        endif()

        set(test_properties "")
        foreach(property_command IN LISTS property_commands)
            generated_test_name("set_tests_properties" "${property_command}" property_name)
            if(property_name STREQUAL test_name)
                set(test_properties "${property_command}")
                break()
            endif()
        endforeach()
        if(test_properties STREQUAL "")
            message(FATAL_ERROR
                "${relative_testfile}: executable test ${test_name} has no properties")
        endif()

        foreach(runtime_dir IN LISTS required_runtime_dirs)
            string(FIND "${test_properties}" "PATH=path_list_prepend:${runtime_dir}" path_offset)
            if(path_offset EQUAL -1)
                message(FATAL_ERROR
                    "${relative_testfile}: executable test ${test_name} does not prepend "
                    "runtime PATH directory: ${runtime_dir}")
            endif()
        endforeach()
        string(FIND "${test_properties}"
            "QT_PLUGIN_PATH=path_list_append:${QT_PLUGIN_DIR}" plugin_offset)
        if(plugin_offset EQUAL -1)
            message(FATAL_ERROR
                "${relative_testfile}: executable test ${test_name} does not append "
                "the Qt plugin directory: ${QT_PLUGIN_DIR}")
        endif()
    endforeach()

    if(relative_testfile STREQUAL "unit/CTestTestfile.cmake" AND
       NOT saw_ndi_runtime_smoke)
        message(FATAL_ERROR
            "${relative_testfile}: tst_ndi_runtime_smoke executable entry is missing")
    endif()
endforeach()

message(STATUS "generated Windows CTest runtime wiring is complete")
