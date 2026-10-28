foreach(required OLR_SOURCE_DIR PROBE_BINARY_DIR CMAKE_COMMAND_PATH CTEST_COMMAND_PATH
                 PROBE_GENERATOR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

set(probe_path "OLR_PATH_SENTINEL;$ENV{PATH}")
execute_process(
    COMMAND "${CMAKE_COMMAND_PATH}" -E env
        "PATH=${probe_path}"
        "QT_PLUGIN_PATH=OLR_PLUGIN_SENTINEL"
        "${CMAKE_COMMAND_PATH}"
        -S "${OLR_SOURCE_DIR}/tests/cmake/legacy_runtime_probe"
        -B "${PROBE_BINARY_DIR}"
        -G "${PROBE_GENERATOR}"
        "-DOLR_SOURCE_DIR=${OLR_SOURCE_DIR}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "legacy runtime probe configure failed (${configure_result})\n"
        "${configure_output}\n${configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND_PATH}" -E env
        "PATH=${probe_path}"
        "QT_PLUGIN_PATH=OLR_PLUGIN_SENTINEL"
        "${CTEST_COMMAND_PATH}" --test-dir "${PROBE_BINARY_DIR}" --output-on-failure
    RESULT_VARIABLE test_result
    OUTPUT_VARIABLE test_output
    ERROR_VARIABLE test_error)
if(NOT test_result EQUAL 0)
    message(FATAL_ERROR
        "legacy runtime probe failed (${test_result})\n${test_output}\n${test_error}")
endif()

message(STATUS "CMake 3.16-compatible runtime reapplication probe passed")
