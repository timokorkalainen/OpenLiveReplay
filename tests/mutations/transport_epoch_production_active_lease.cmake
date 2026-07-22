if(NOT DEFINED BASELINE_EXECUTABLE OR NOT DEFINED CONTROL_EXECUTABLE)
    message(FATAL_ERROR "active-lease baseline and altered control executables are required")
endif()

execute_process(
    COMMAND "${BASELINE_EXECUTABLE}"
    RESULT_VARIABLE baseline_result
    OUTPUT_VARIABLE baseline_stdout
    ERROR_VARIABLE baseline_stderr)
set(baseline_output "${baseline_stdout}\n${baseline_stderr}")
if(NOT baseline_result EQUAL 0 OR
   NOT baseline_output MATCHES "transport epoch active-lease proof: PASS")
    message(FATAL_ERROR
        "production active-lease baseline failed with OLR_UNIT_TEST undefined "
        "(exit ${baseline_result})\n${baseline_output}")
endif()

execute_process(
    COMMAND "${CONTROL_EXECUTABLE}"
    RESULT_VARIABLE control_result
    OUTPUT_VARIABLE control_stdout
    ERROR_VARIABLE control_stderr)
set(control_output "${control_stdout}\n${control_stderr}")
if(control_result EQUAL 0 OR
   NOT control_output MATCHES "commit blocked behind active sink lease")
    message(FATAL_ERROR
        "production active-lease blocking control survived "
        "(exit ${control_result})\n${control_output}")
endif()

message(STATUS
    "production active-lease baseline passed and blocking control was detected")
