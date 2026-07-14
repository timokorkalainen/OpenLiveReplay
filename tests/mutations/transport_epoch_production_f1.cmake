if(NOT DEFINED BASELINE_EXECUTABLE OR NOT DEFINED MUTANT_EXECUTABLE)
    message(FATAL_ERROR "production F1 baseline and mutant executables are required")
endif()

execute_process(
    COMMAND "${BASELINE_EXECUTABLE}"
    RESULT_VARIABLE baseline_result
    OUTPUT_VARIABLE baseline_stdout
    ERROR_VARIABLE baseline_stderr)
set(baseline_output "${baseline_stdout}\n${baseline_stderr}")
if(NOT baseline_result EQUAL 0 OR
   NOT baseline_output MATCHES "production F1 baseline: PASS")
    message(FATAL_ERROR
        "production F1 baseline failed with OLR_UNIT_TEST undefined\n${baseline_output}")
endif()

execute_process(
    COMMAND "${MUTANT_EXECUTABLE}"
    RESULT_VARIABLE mutant_result
    OUTPUT_VARIABLE mutant_stdout
    ERROR_VARIABLE mutant_stderr)
set(mutant_output "${mutant_stdout}\n${mutant_stderr}")
if(mutant_result EQUAL 0 OR
   NOT mutant_output MATCHES "stale pre-reset snapshot was dispatched")
    message(FATAL_ERROR
        "production F1 mutant was not killed by stale-lease behavior\n${mutant_output}")
endif()

message(STATUS
    "production F1 baseline passed and no-OLR_UNIT_TEST config-generation mutant was killed")
