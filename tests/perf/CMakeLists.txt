qt_add_executable(tst_gpuretireregistry_perf tst_gpuretireregistry_perf.cpp)
target_compile_definitions(tst_gpuretireregistry_perf
    PRIVATE OLR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(tst_gpuretireregistry_perf
    PRIVATE Qt6::Test olr_test_gpu olr_warnings olr_sanitize)
add_test(NAME tst_gpuretireregistry_perf COMMAND tst_gpuretireregistry_perf)
set_tests_properties(tst_gpuretireregistry_perf PROPERTIES LABELS "perf;gpu")
