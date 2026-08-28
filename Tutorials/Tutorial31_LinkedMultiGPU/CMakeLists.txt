cmake_minimum_required (VERSION 3.10)

project(Tutorial31_LinkedMultiGPU CXX)

add_sample_app(Tutorial31_LinkedMultiGPU
    IDE_FOLDER
        DiligentSamples/Tutorials
    SOURCES
        src/Tutorial31_LinkedMultiGPU.cpp
        ../Common/src/GpuInfoPanel.cpp
    INCLUDES
        src/Tutorial31_LinkedMultiGPU.hpp
        ../Common/src/GpuInfoPanel.hpp
)
