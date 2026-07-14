if(DEFINED GENERATE_MUTATION)
    if(NOT DEFINED INPUT_SOURCE OR NOT DEFINED OUTPUT_SOURCE)
        message(FATAL_ERROR "INPUT_SOURCE and OUTPUT_SOURCE are required for mutation generation")
    endif()
    file(READ "${INPUT_SOURCE}" production_source)

    if(GENERATE_MUTATION STREQUAL "COMPATIBILITY")
        set(expected_statement [=[return surface.deviceDomainId != 0 && surface.authorityEpoch != 0 && fence.instanceId != 0 &&
           fence.deviceDomainId == surface.deviceDomainId &&
           fence.authorityEpoch == surface.authorityEpoch && submissionGeneration != 0 &&
           submissionGeneration == currentGeneration;]=])
        set(mutated_statement [=[#ifdef OLR_MUTATE_GPU_COMPATIBILITY
    (void) surface;
    (void) fence;
    (void) submissionGeneration;
    (void) currentGeneration;
    return true;
#else
    return surface.deviceDomainId != 0 && surface.authorityEpoch != 0 && fence.instanceId != 0 &&
           fence.deviceDomainId == surface.deviceDomainId &&
           fence.authorityEpoch == surface.authorityEpoch && submissionGeneration != 0 &&
           submissionGeneration == currentGeneration;
#endif]=])
    elseif(GENERATE_MUTATION STREQUAL "LOSS_GENERATION")
        set(expected_statement [=[if (token.observedGeneration() != generation ||
                token.authorityEpoch() != m_deviceAuthorityEpoch)
                return {};]=])
        set(mutated_statement [=[#ifdef OLR_MUTATE_GPU_LOSS_GENERATION
            if (token.authorityEpoch() != m_deviceAuthorityEpoch) return {};
#else
            if (token.observedGeneration() != generation ||
                token.authorityEpoch() != m_deviceAuthorityEpoch)
                return {};
#endif]=])
    elseif(GENERATE_MUTATION STREQUAL "RETIRE_PUBLICATION")
        set(expected_statement [=[if (!GpuReadbackRetainer::publish(prepared.m_handle, ticket)) return false;]=])
        set(mutated_statement [=[#ifdef OLR_MUTATE_GPU_RETIRE_PUBLICATION
    GpuReadbackRetainer::release(prepared.m_handle);
#else
    if (!GpuReadbackRetainer::publish(prepared.m_handle, ticket)) return false;
#endif]=])
    elseif(GENERATE_MUTATION STREQUAL "QUARANTINE_TRANSFER")
        set(expected_statement [=[GpuReadbackRetainer::quarantine(prepared.m_handle);]=])
        set(mutated_statement [=[#ifdef OLR_MUTATE_GPU_QUARANTINE_TRANSFER
    GpuReadbackRetainer::release(prepared.m_handle);
#else
    GpuReadbackRetainer::quarantine(prepared.m_handle);
#endif]=])
    else()
        message(FATAL_ERROR "Unknown GPU retirement mutation: ${GENERATE_MUTATION}")
    endif()

    string(LENGTH "${production_source}" source_length)
    string(LENGTH "${expected_statement}" statement_length)
    string(REPLACE "${expected_statement}" "" source_without_statement "${production_source}")
    string(LENGTH "${source_without_statement}" stripped_length)
    math(EXPR removed_length "${source_length} - ${stripped_length}")
    if(statement_length EQUAL 0)
        message(FATAL_ERROR "Empty source-coupled statement for ${GENERATE_MUTATION}")
    endif()
    math(EXPR statement_count "${removed_length} / ${statement_length}")
    if(NOT statement_count EQUAL 1 OR
       NOT removed_length EQUAL statement_length)
        message(FATAL_ERROR
            "${GENERATE_MUTATION} expected its production statement exactly once, found "
            "${statement_count}: ${INPUT_SOURCE}")
    endif()
    string(REPLACE "${expected_statement}" "${mutated_statement}"
        generated_source "${production_source}")
    get_filename_component(output_directory "${OUTPUT_SOURCE}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_directory}")
    file(WRITE "${OUTPUT_SOURCE}" "${generated_source}")
    return()
endif()

if(NOT DEFINED MUTANT_NAME OR NOT DEFINED BASELINE_EXECUTABLE OR
   NOT DEFINED MUTANT_EXECUTABLE OR NOT DEFINED TEST_FUNCTION OR
   NOT DEFINED EXPECTED_TEST_NAME OR NOT DEFINED EXPECTED_FAILURE_TOKEN)
    message(FATAL_ERROR
        "MUTANT_NAME, BASELINE_EXECUTABLE, MUTANT_EXECUTABLE, TEST_FUNCTION, "
        "EXPECTED_TEST_NAME, and EXPECTED_FAILURE_TOKEN are required")
endif()

string(MAKE_C_IDENTIFIER "${MUTANT_NAME}" mutant_file_stem)
set(baseline_output_file "${CMAKE_CURRENT_BINARY_DIR}/${mutant_file_stem}_baseline.txt")
set(mutant_output_file "${CMAKE_CURRENT_BINARY_DIR}/${mutant_file_stem}_mutant.txt")
file(REMOVE "${baseline_output_file}" "${mutant_output_file}")

execute_process(
    COMMAND "${BASELINE_EXECUTABLE}" "${TEST_FUNCTION}" -o "${baseline_output_file},txt"
    RESULT_VARIABLE baseline_result
    OUTPUT_VARIABLE baseline_stdout
    ERROR_VARIABLE baseline_stderr)
if(EXISTS "${baseline_output_file}")
    file(READ "${baseline_output_file}" baseline_test_output)
endif()
set(baseline_output "${baseline_test_output}\n${baseline_stdout}\n${baseline_stderr}")

execute_process(
    COMMAND "${MUTANT_EXECUTABLE}" "${TEST_FUNCTION}" -o "${mutant_output_file},txt"
    RESULT_VARIABLE mutant_result
    OUTPUT_VARIABLE mutant_stdout
    ERROR_VARIABLE mutant_stderr)
if(EXISTS "${mutant_output_file}")
    file(READ "${mutant_output_file}" mutant_test_output)
endif()
set(mutant_output "${mutant_test_output}\n${mutant_stdout}\n${mutant_stderr}")

string(FIND "${baseline_output}" "PASS   : ${EXPECTED_TEST_NAME}" baseline_name_offset)
string(REGEX MATCH "Totals: [0-9]+ passed, 0 failed" baseline_totals "${baseline_output}")
if(NOT baseline_result EQUAL 0 OR baseline_name_offset EQUAL -1 OR NOT baseline_totals)
    message(FATAL_ERROR
        "${MUTANT_NAME} baseline did not pass its exact named test\n${baseline_output}")
endif()

string(FIND "${mutant_output}" "FAIL!  : ${EXPECTED_TEST_NAME}" mutant_name_offset)
string(FIND "${mutant_output}" "${EXPECTED_FAILURE_TOKEN}" mutant_token_offset)
string(REGEX MATCH "Totals: [0-9]+ passed, 1 failed" mutant_totals "${mutant_output}")
if(mutant_result EQUAL 0)
    message(FATAL_ERROR
        "${MUTANT_NAME} mutant survived: ${TEST_FUNCTION} unexpectedly passed\n${mutant_output}")
endif()
if(mutant_name_offset EQUAL -1 OR mutant_token_offset EQUAL -1 OR NOT mutant_totals)
    message(FATAL_ERROR
        "${MUTANT_NAME} mutant failed for an unrelated reason; expected ${EXPECTED_TEST_NAME} "
        "and '${EXPECTED_FAILURE_TOKEN}'\n${mutant_output}")
endif()

message(STATUS
    "${MUTANT_NAME} baseline passed and mutant was killed by ${EXPECTED_TEST_NAME}: "
    "${EXPECTED_FAILURE_TOKEN}")
