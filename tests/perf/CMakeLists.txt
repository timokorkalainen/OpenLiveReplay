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
