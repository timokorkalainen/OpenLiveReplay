function(olr_diagnostic_matches output expected_class expected_member expected_access result)
    set(matches TRUE)
    foreach(expected_token IN ITEMS
            "${expected_class}" "${expected_member}" "${expected_access}")
        if(NOT expected_token STREQUAL "" AND NOT output MATCHES "${expected_token}")
            set(matches FALSE)
        endif()
    endforeach()
    set(${result} ${matches} PARENT_SCOPE)
endfunction()

if(VERIFY_DIAGNOSTIC_FIXTURES)
    set(gcc_base
        "error: 'virtual void* GpuSurface::nativeHandle() const' is protected within this context")
    set(appleclang_base
        "error: 'nativeHandle' is a protected member of 'GpuSurface'")
    set(gcc_derived
        "error: 'virtual void* DerivedSurface::nativeHandle() const' is protected within this context")
    set(appleclang_derived
        "error: 'nativeHandle' is a protected member of 'DerivedSurface'")
    foreach(fixture IN ITEMS
            "${gcc_base}" "${appleclang_base}")
        olr_diagnostic_matches("${fixture}" "GpuSurface" "nativeHandle" "[Pp]rotected" matched)
        if(NOT matched)
            message(FATAL_ERROR "portable base diagnostic fixture was rejected: ${fixture}")
        endif()
    endforeach()
    foreach(fixture IN ITEMS
            "${gcc_derived}" "${appleclang_derived}")
        olr_diagnostic_matches("${fixture}" "DerivedSurface" "nativeHandle" "[Pp]rotected"
            matched)
        if(NOT matched)
            message(FATAL_ERROR "portable derived diagnostic fixture was rejected: ${fixture}")
        endif()
    endforeach()
    olr_diagnostic_matches("${appleclang_base}" "DerivedSurface" "nativeHandle"
        "[Pp]rotected" matched)
    if(matched)
        message(FATAL_ERROR "diagnostic fixture accepted the wrong class")
    endif()
    olr_diagnostic_matches("${appleclang_base}" "GpuSurface" "track" "[Pp]rotected" matched)
    if(matched)
        message(FATAL_ERROR "diagnostic fixture accepted the wrong member")
    endif()
    olr_diagnostic_matches("${appleclang_base}" "GpuSurface" "nativeHandle" "[Pp]rivate"
        matched)
    if(matched)
        message(FATAL_ERROR "diagnostic fixture accepted the wrong access category")
    endif()
    message(STATUS "GCC and AppleClang diagnostic fixtures matched independent tokens")
    return()
endif()

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
            "-DEXPECTED_CLASS=${EXPECTED_CLASS}"
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
olr_diagnostic_matches("${build_output}" "${EXPECTED_CLASS}" "${EXPECTED_MEMBER}"
    "${EXPECTED_ACCESS}" diagnostic_matches)
if(NOT diagnostic_matches)
    message(FATAL_ERROR
        "OLR_COMPILE_CONTROL_DIAGNOSTIC_MISMATCH\n"
        "Compile control ${TARGET} failed without independent expected tokens "
        "class='${EXPECTED_CLASS}', member='${EXPECTED_MEMBER}', "
        "access='${EXPECTED_ACCESS}':\n${build_output}")
endif()
message(STATUS "Compile-fail control ${TARGET} produced the expected diagnostic")
