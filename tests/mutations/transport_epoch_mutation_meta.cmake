if(NOT DEFINED MUTATION_WRAPPER OR NOT DEFINED MUTANT_NAME OR
   NOT DEFINED MUTANT_EXECUTABLE OR NOT DEFINED TEST_FUNCTION OR
   NOT DEFINED EXPECTED_TEST_NAME OR NOT DEFINED EXPECTED_FAILURE_TOKEN)
    message(FATAL_ERROR "transport epoch mutation meta-test arguments are required")
endif()

# Meta-mutate the paired production baseline with the same compiled omission as
# the mutant. The mutant still has the expected focused failure, but accepting
# its kill would be invalid because the unmutated baseline selector is RED.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DMUTANT_NAME=${MUTANT_NAME}"
        "-DBASELINE_EXECUTABLE=${MUTANT_EXECUTABLE}"
        "-DBASELINE_TEST_FUNCTION=${TEST_FUNCTION}"
        "-DMUTANT_EXECUTABLE=${MUTANT_EXECUTABLE}"
        "-DTEST_FUNCTION=${TEST_FUNCTION}"
        "-DEXPECTED_TEST_NAME=${EXPECTED_TEST_NAME}"
        "-DEXPECTED_FAILURE_TOKEN=${EXPECTED_FAILURE_TOKEN}"
        -P "${MUTATION_WRAPPER}"
    RESULT_VARIABLE wrapper_result
    OUTPUT_VARIABLE wrapper_stdout
    ERROR_VARIABLE wrapper_stderr)
set(wrapper_output "${wrapper_stdout}\n${wrapper_stderr}")

if(wrapper_result EQUAL 0)
    message(FATAL_ERROR
        "mutation wrapper accepted ${MUTANT_NAME} while its paired production baseline "
        "was meta-mutated RED\n${wrapper_output}")
endif()
string(FIND "${wrapper_output}" "unmutated baseline failed" baseline_failure_offset)
string(FIND "${wrapper_output}" "mutant independently killed" mutant_failure_offset)
if(baseline_failure_offset EQUAL -1 OR mutant_failure_offset EQUAL -1)
    message(FATAL_ERROR
        "mutation wrapper rejected ${MUTANT_NAME} for the wrong reason; expected both the "
        "broken production baseline and independent mutant kill\n${wrapper_output}")
endif()

message(STATUS
    "${MUTANT_NAME} meta-mutation rejected: production baseline RED despite mutant kill")
