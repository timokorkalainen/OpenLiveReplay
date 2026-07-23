if(OLR_GPU_PIPELINE)
qt_add_executable(tst_gpuretireregistry_perf tst_gpuretireregistry_perf.cpp)
target_compile_definitions(tst_gpuretireregistry_perf
    PRIVATE OLR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(tst_gpuretireregistry_perf
    PRIVATE Qt6::Test olr_test_gpu olr_warnings olr_sanitize)
add_test(NAME tst_gpuretireregistry_perf COMMAND tst_gpuretireregistry_perf)
set_tests_properties(tst_gpuretireregistry_perf PROPERTIES LABELS "perf;gpu")

set(saved912_commit "912663bed2eaf4016e0dc29ab44253c030d8eab3")
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --is-shallow-repository
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE saved912_shallow_status
        OUTPUT_VARIABLE saved912_is_shallow
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(saved912_shallow_status EQUAL 0 AND saved912_is_shallow STREQUAL "true")
        message(STATUS
            "Shallow checkout: frozen GPU benchmark provenance relies on pinned file hashes")
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" cat-file -e "${saved912_commit}^{commit}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE saved912_commit_status
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT saved912_commit_status EQUAL 0)
            message(FATAL_ERROR
                "Frozen GPU retirement benchmark provenance commit ${saved912_commit} is missing")
        endif()
    endif()
endif()

file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/saved912gpuopscope.h" saved912_op_scope_sha)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/saved912preparedregistry.h" saved912_registry_header_sha)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/saved912preparedregistry.cpp" saved912_registry_sha)
if(NOT saved912_op_scope_sha STREQUAL
       "2771095d97b494373ad93bbaad088f846f30dc4bbadab0c557e45d7ce4d0a40e" OR
   NOT saved912_registry_header_sha STREQUAL
       "f69b82efc2ea13f465df55385e6ae18e85ea0c61a348d0a85af3387479b5c0bc" OR
   NOT saved912_registry_sha STREQUAL
       "c7b368c4f58875a449fa1ab4c305b67a4650e9c53036653c555a5e1b791800e3")
    message(FATAL_ERROR
        "Frozen exact-912 GPU retirement benchmark snapshot drifted; verify against "
        "912663bed2eaf4016e0dc29ab44253c030d8eab3 before updating hashes")
endif()

qt_add_library(olr_production_retire_perf STATIC
    "${CMAKE_SOURCE_DIR}/playback/gpu/gpudevicelossmonitor.cpp"
    "${CMAKE_SOURCE_DIR}/playback/gpu/gpurecoverycoordinator.cpp"
    "${CMAKE_SOURCE_DIR}/playback/gpu/gpureadbackretainer.cpp"
    "${CMAKE_SOURCE_DIR}/playback/gpu/gpuretireregistry.cpp"
    "${CMAKE_SOURCE_DIR}/playback/gpu/gpuopscope.cpp"
    "${CMAKE_SOURCE_DIR}/playback/gpu/gpugeneration.cpp")
target_include_directories(olr_production_retire_perf PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(olr_production_retire_perf
    PUBLIC Qt6::Core Qt6::Gui
    PRIVATE olr_warnings olr_sanitize)
if(WIN32)
    target_sources(olr_production_retire_perf PRIVATE
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpufence_win.cpp"
        "${CMAKE_SOURCE_DIR}/playback/output/win/d3dfence.cpp")
    target_link_libraries(olr_production_retire_perf PRIVATE d3d11 dxgi ole32)
elseif(APPLE)
    target_sources(olr_production_retire_perf PRIVATE
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpufence_apple.mm")
    target_link_libraries(olr_production_retire_perf PRIVATE "-framework Metal")
else()
    target_sources(olr_production_retire_perf PRIVATE
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpufence_stub.cpp")
endif()

qt_add_executable(tst_gpuretireregistry_production_perf
    tst_gpuretireregistry_production_perf.cpp
    saved912preparedregistry.cpp)
target_compile_definitions(tst_gpuretireregistry_production_perf
    PRIVATE OLR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(tst_gpuretireregistry_production_perf
    PRIVATE Qt6::Test olr_production_retire_perf olr_warnings olr_sanitize)
add_test(NAME tst_gpuretireregistry_production_perf
    COMMAND tst_gpuretireregistry_production_perf)
set_tests_properties(tst_gpuretireregistry_production_perf PROPERTIES LABELS "perf;gpu")

get_property(_olr_perf_tests DIRECTORY PROPERTY TESTS)
olr_apply_windows_test_runtime(${_olr_perf_tests})
endif()

qt_add_executable(tst_transportcommit_perf tst_transportcommit_perf.cpp)
target_link_libraries(tst_transportcommit_perf PRIVATE
    Qt6::Test olr_test_playback olr_warnings olr_sanitize)
if(OLR_GPU_PIPELINE)
    target_link_libraries(tst_transportcommit_perf PRIVATE olr_test_gpu)
endif()

if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
    add_custom_command(TARGET tst_transportcommit_perf POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:tst_transportcommit_perf>
            $<TARGET_FILE_DIR:tst_transportcommit_perf>
        COMMAND_EXPAND_LISTS
        COMMENT "Copying tst_transportcommit_perf runtime DLLs")
endif()

olr_add_ctest(NAME tst_transportcommit_perf
    COMMAND $<TARGET_FILE:tst_transportcommit_perf>
        -o -,txt)
set_tests_properties(tst_transportcommit_perf PROPERTIES
    LABELS "perf;ci"
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
