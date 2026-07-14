if(NOT DEFINED MUTANT_EXECUTABLE OR NOT DEFINED TEST_FUNCTION OR
   NOT DEFINED EXPECTED_FAILURE_TOKEN)
    message(FATAL_ERROR
        "MUTANT_EXECUTABLE, TEST_FUNCTION, and EXPECTED_FAILURE_TOKEN are required")
endif()

string(MAKE_C_IDENTIFIER "${MUTANT_NAME}" mutant_file_stem)
set(mutant_output_file "${CMAKE_CURRENT_BINARY_DIR}/${mutant_file_stem}.txt")
file(REMOVE "${mutant_output_file}")
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

if(mutant_result EQUAL 0)
    message(FATAL_ERROR
        "${MUTANT_NAME} survived: ${TEST_FUNCTION} unexpectedly passed\n${mutant_output}")
endif()

string(FIND "${mutant_output}" "FAIL!  : ${EXPECTED_TEST_NAME}" failure_name_offset)
string(FIND "${mutant_output}" "${EXPECTED_FAILURE_TOKEN}" failure_token_offset)
string(REGEX MATCH "Totals: [0-9]+ passed, 1 failed" totals_match "${mutant_output}")
if(failure_name_offset EQUAL -1 OR failure_token_offset EQUAL -1 OR NOT totals_match)
    message(FATAL_ERROR
        "${MUTANT_NAME} failed for an unrelated reason (result ${mutant_result}); expected "
        "${EXPECTED_TEST_NAME}, token '${EXPECTED_FAILURE_TOKEN}', and exactly one failed test\n"
        "${mutant_output}")
endif()

message(STATUS
    "${MUTANT_NAME} killed by ${EXPECTED_TEST_NAME}: ${EXPECTED_FAILURE_TOKEN}")
