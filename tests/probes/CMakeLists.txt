# Phase-0 GPU probes. Built only when OLR_BUILD_GPU_PROBES=ON. These link Qt RHI
# (Qt6::GuiPrivate) for measurement; they are NOT part of the shipping app.
find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate ShaderTools)

qt_add_executable(rhi_import_probe rhi_import_probe.mm)
target_include_directories(rhi_import_probe PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(rhi_import_probe PRIVATE
    Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::ShaderTools
    olr_warnings olr_sanitize
    "-framework Metal" "-framework CoreVideo" "-framework CoreFoundation")

if(WIN32)
    qt_add_executable(rhi_d3d_probe rhi_d3d_probe.cpp)
    target_include_directories(rhi_d3d_probe PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(rhi_d3d_probe PRIVATE
        Qt6::Core Qt6::Gui Qt6::GuiPrivate
        d3d11 dxgi mfplat mf mfuuid ole32
        olr_warnings olr_sanitize)
endif()
