if(NOT WIN32 OR NOT OLR_GPU_PIPELINE)
    return()
endif()

qt_add_executable(win_gpu_fault_child
    win_gpu_fault_child.cpp
    win_gpu_fault_protocol.h)
target_include_directories(win_gpu_fault_child PRIVATE "${CMAKE_SOURCE_DIR}")
target_compile_definitions(win_gpu_fault_child PRIVATE
    OLR_UNIT_TEST=1
    OLR_GPU_FAULT_LANE_BUILD=1
    OLR_GPU_FAULT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(win_gpu_fault_child PRIVATE
    olr_test_playback Qt6::Core d3d11 dxgi d3dcompiler ole32)

qt_add_executable(win_gpu_fault_parent
    win_gpu_fault_parent.cpp
    win_gpu_fault_protocol.h)
target_include_directories(win_gpu_fault_parent PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(win_gpu_fault_parent PRIVATE Qt6::Core)

add_test(NAME win_gpu_fault_protocol_selftest
    COMMAND win_gpu_fault_parent --self-test "$<TARGET_FILE:win_gpu_fault_child>")
set_tests_properties(win_gpu_fault_protocol_selftest PROPERTIES
    LABELS "gpu-fault;ci"
    TIMEOUT 30)

add_test(NAME win_gpu_fault_child_guard
    COMMAND win_gpu_fault_child --trigger-tdr)
set_tests_properties(win_gpu_fault_child_guard PROPERTIES
    LABELS "gpu-fault;ci"
    TIMEOUT 10
    WILL_FAIL TRUE)

add_test(NAME win_gpu_fault_real
    COMMAND win_gpu_fault_parent --run "$<TARGET_FILE:win_gpu_fault_child>")
set_tests_properties(win_gpu_fault_real PROPERTIES
    LABELS "gpu-fault"
    TIMEOUT 120
    RUN_SERIAL TRUE
    RESOURCE_LOCK windows_gpu_fault_device
    SKIP_RETURN_CODE 77)
