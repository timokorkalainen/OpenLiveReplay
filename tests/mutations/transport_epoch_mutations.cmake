if(NOT DEFINED BASELINE_EXECUTABLE OR NOT DEFINED BASELINE_TEST_FUNCTION OR
   NOT DEFINED MUTANT_EXECUTABLE OR NOT DEFINED TEST_FUNCTION OR
   NOT DEFINED EXPECTED_TEST_NAME OR NOT DEFINED EXPECTED_FAILURE_TOKEN)
    message(FATAL_ERROR
        "BASELINE_EXECUTABLE, BASELINE_TEST_FUNCTION, MUTANT_EXECUTABLE, "
        "TEST_FUNCTION, EXPECTED_TEST_NAME, and EXPECTED_FAILURE_TOKEN are required")
endif()

string(MAKE_C_IDENTIFIER "${MUTANT_NAME}" mutant_file_stem)
set(baseline_output_file
    "${CMAKE_CURRENT_BINARY_DIR}/${mutant_file_stem}_baseline.txt")
set(mutant_output_file "${CMAKE_CURRENT_BINARY_DIR}/${mutant_file_stem}.txt")
file(REMOVE "${baseline_output_file}" "${mutant_output_file}")
execute_process(
    COMMAND "${BASELINE_EXECUTABLE}" "${BASELINE_TEST_FUNCTION}"
        -o "${baseline_output_file},txt"
    RESULT_VARIABLE baseline_result
    OUTPUT_VARIABLE baseline_stdout
    ERROR_VARIABLE baseline_stderr)
if(EXISTS "${baseline_output_file}")
    file(READ "${baseline_output_file}" baseline_test_output)
else()
    set(baseline_test_output "")
endif()
set(baseline_output "${baseline_test_output}\n${baseline_stdout}\n${baseline_stderr}")

execute_process(
    COMMAND "${MUTANT_EXECUTABLE}" "${TEST_FUNCTION}"
        -o "${mutant_output_file},txt"
    RESULT_VARIABLE mutant_result
    OUTPUT_VARIABLE mutant_stdout
    ERROR_VARIABLE mutant_stderr)
if(EXISTS "${mutant_output_file}")
    file(READ "${mutant_output_file}" mutant_test_output)
else()
    set(mutant_test_output "")
endif()
set(mutant_output "${mutant_test_output}\n${mutant_stdout}\n${mutant_stderr}")

string(FIND "${mutant_output}" "FAIL!  : ${EXPECTED_TEST_NAME}" failure_name_offset)
string(FIND "${mutant_output}" "${EXPECTED_FAILURE_TOKEN}" failure_token_offset)
string(REGEX MATCH "Totals: [0-9]+ passed, 1 failed" totals_match "${mutant_output}")
set(mutant_killed FALSE)
if(NOT mutant_result EQUAL 0 AND NOT failure_name_offset EQUAL -1 AND
   NOT failure_token_offset EQUAL -1 AND totals_match)
    set(mutant_killed TRUE)
    set(mutant_status
        "mutant independently killed by ${EXPECTED_TEST_NAME}: ${EXPECTED_FAILURE_TOKEN}")
elseif(mutant_result EQUAL 0)
    set(mutant_status
        "mutant survived: ${TEST_FUNCTION} unexpectedly passed")
else()
    set(mutant_status
        "mutant failed for an unrelated reason (result ${mutant_result})")
endif()

string(FIND "${baseline_output}" "PASS   : ${EXPECTED_TEST_NAME}" baseline_name_offset)
string(REGEX MATCH "Totals: [0-9]+ passed, 0 failed" baseline_totals_match
    "${baseline_output}")
if(NOT baseline_result EQUAL 0 OR baseline_name_offset EQUAL -1 OR
   NOT baseline_totals_match)
    message(FATAL_ERROR
        "${MUTANT_NAME} unmutated baseline failed: ${BASELINE_TEST_FUNCTION} must pass "
        "before a mutant kill can be accepted\n${mutant_status}\n"
        "BASELINE OUTPUT:\n${baseline_output}\nMUTANT OUTPUT:\n${mutant_output}")
endif()

if(NOT mutant_killed)
    message(FATAL_ERROR
        "${MUTANT_NAME} ${mutant_status}; expected ${EXPECTED_TEST_NAME}, token "
        "'${EXPECTED_FAILURE_TOKEN}', and exactly one failed test\n${mutant_output}")
endif()

message(STATUS
    "${MUTANT_NAME} baseline passed; ${mutant_status}")
