if(NOT DEFINED BUILD_DIR OR NOT DEFINED TARGET)
    message(FATAL_ERROR "BUILD_DIR and TARGET are required")
endif()

if(VERIFY_REJECTION)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${BUILD_DIR}"
            "-DTARGET=${TARGET}"
            "-DCONFIG=${CONFIG}"
            "-DEXPECT_COMPILE=FALSE"
            "-DEXPECTED_MEMBER=${EXPECTED_MEMBER}"
            "-DEXPECTED_ACCESS=${EXPECTED_ACCESS}"
            -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE validation_result
        OUTPUT_VARIABLE validation_stdout
        ERROR_VARIABLE validation_stderr)
    set(validation_output "${validation_stdout}\n${validation_stderr}")
    if(validation_result EQUAL 0)
        message(FATAL_ERROR
            "Diagnostic-aware compile control incorrectly accepted unrelated failure:\n"
            "${validation_output}")
    endif()
    if(NOT validation_output MATCHES "OLR_COMPILE_CONTROL_DIAGNOSTIC_MISMATCH")
        message(FATAL_ERROR
            "Compile-control mutation was rejected for an unexpected reason:\n"
            "${validation_output}")
    endif()
    message(STATUS "Diagnostic-aware compile control rejected unrelated compiler failure")
    return()
endif()

set(build_command "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${TARGET}")
if(DEFINED CONFIG AND NOT CONFIG STREQUAL "")
    list(APPEND build_command --config "${CONFIG}")
endif()
execute_process(
    COMMAND ${build_command}
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr)
set(build_output "${build_stdout}\n${build_stderr}")

if(EXPECT_COMPILE)
    if(NOT build_result EQUAL 0)
        message(FATAL_ERROR
            "Compile-pass control ${TARGET} failed to build:\n${build_output}")
    endif()
    message(STATUS "Compile-pass control ${TARGET} built successfully")
    return()
endif()

if(build_result EQUAL 0)
    message(FATAL_ERROR "Forbidden capability control ${TARGET} compiled successfully")
endif()
foreach(expected_diagnostic IN ITEMS "${EXPECTED_MEMBER}" "${EXPECTED_ACCESS}")
    if(NOT build_output MATCHES "${expected_diagnostic}")
        message(FATAL_ERROR
            "OLR_COMPILE_CONTROL_DIAGNOSTIC_MISMATCH\n"
            "Compile control ${TARGET} failed without expected diagnostic "
            "'${expected_diagnostic}':\n${build_output}")
    endif()
endforeach()
message(STATUS "Compile-fail control ${TARGET} produced the expected diagnostic")
