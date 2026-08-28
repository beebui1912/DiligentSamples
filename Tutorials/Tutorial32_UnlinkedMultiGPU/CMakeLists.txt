cmake_minimum_required (VERSION 3.10)

project(Tutorial32_UnlinkedMultiGPU CXX)

add_sample_app(Tutorial32_UnlinkedMultiGPU
    IDE_FOLDER
        DiligentSamples/Tutorials
    SOURCES
        src/Tutorial32_UnlinkedMultiGPU.cpp
        ../Common/src/GpuInfoPanel.cpp
    INCLUDES
        src/Tutorial32_UnlinkedMultiGPU.hpp
        ../Common/src/GpuInfoPanel.hpp
)

# CrossDeviceTransferManager lives in Diligent-GraphicsTools.
target_link_libraries(Tutorial32_UnlinkedMultiGPU PRIVATE Diligent-GraphicsTools)
