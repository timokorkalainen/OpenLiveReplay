if(NOT DEFINED INPUT_SOURCE OR NOT DEFINED OUTPUT_SOURCE OR NOT DEFINED MUTATION)
    message(FATAL_ERROR "INPUT_SOURCE, OUTPUT_SOURCE, and MUTATION are required")
endif()

file(READ "${INPUT_SOURCE}" source)
if(MUTATION STREQUAL "F1_SKIP_CONFIG_GENERATION")
    set(needle
        "    ++m_configGeneration;\n    if (m_dispatchActive) {")
    set(replacement
        "    /* compiled control: config generation omitted */\n    if (m_dispatchActive) {")
elseif(MUTATION STREQUAL "F2_SKIP_COMMIT_EPOCH_RESET")
    set(needle
        "    m_committedGeneration.store(commit.seekGeneration, std::memory_order_release);\n    resetOutputPlayEpoch();")
    set(replacement
        "    m_committedGeneration.store(commit.seekGeneration, std::memory_order_release);\n    /* compiled control: output play epoch reset omitted */")
elseif(MUTATION STREQUAL "ACTIVE_LEASE_BLOCKING_RESET")
    set(needle
        "    if (m_dispatchActive) {\n        m_pendingPlayEpochReset = true;\n        return;\n    }")
    set(replacement
        "    if (m_dispatchActive) {\n        waitForDispatchIdleLocked();\n    }")
else()
    message(FATAL_ERROR "unknown transport epoch mutation: ${MUTATION}")
endif()

string(FIND "${source}" "${needle}" needle_offset)
if(needle_offset EQUAL -1)
    message(FATAL_ERROR "transport epoch ${MUTATION} target was not found")
endif()
string(LENGTH "${source}" source_length)
string(LENGTH "${needle}" needle_length)
string(REPLACE "${needle}" "" source_without_needle "${source}")
string(LENGTH "${source_without_needle}" source_without_needle_length)
math(EXPR removed_length "${source_length} - ${source_without_needle_length}")
if(NOT removed_length EQUAL needle_length)
    message(FATAL_ERROR "transport epoch ${MUTATION} target was not unique")
endif()
string(REPLACE "${needle}" "${replacement}" altered "${source}")
file(WRITE "${OUTPUT_SOURCE}" "${altered}")
