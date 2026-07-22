qt_add_executable(tst_gpuretireregistry_perf tst_gpuretireregistry_perf.cpp)
target_compile_definitions(tst_gpuretireregistry_perf
    PRIVATE OLR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(tst_gpuretireregistry_perf
    PRIVATE Qt6::Test olr_test_gpu olr_warnings olr_sanitize)
add_test(NAME tst_gpuretireregistry_perf COMMAND tst_gpuretireregistry_perf)
set_tests_properties(tst_gpuretireregistry_perf PROPERTIES LABELS "perf;gpu")

file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/saved912gpuopscope.h" saved912_op_scope_sha)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/saved912preparedregistry.h" saved912_registry_header_sha)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/saved912preparedregistry.cpp" saved912_registry_sha)
if(NOT saved912_op_scope_sha STREQUAL
       "2771095d97b494373ad93bbaad088f846f30dc4bbadab0c557e45d7ce4d0a40e" OR
   NOT saved912_registry_header_sha STREQUAL
       "6fcc8c50f6ee793dee3ffb43a309903a728b30806defee0df40897f2161bd0ca" OR
   NOT saved912_registry_sha STREQUAL
       "c7b368c4f58875a449fa1ab4c305b67a4650e9c53036653c555a5e1b791800e3")
    message(FATAL_ERROR
        "Frozen exact-912 GPU retirement benchmark snapshot drifted; verify against "
        "912663be1458a0bdba36fca10c0421ab79d5cbd4 before updating hashes")
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
