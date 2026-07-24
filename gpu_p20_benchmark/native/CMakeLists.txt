cmake_minimum_required(VERSION 3.21)

option(BSF_NVIDIA_CUDA "Build the BetaSeedFinder worker with NVIDIA CUDA" OFF)

if(BSF_NVIDIA_CUDA)
    project(GpuP20Benchmark LANGUAGES CUDA CXX)
else()
    project(GpuP20Benchmark LANGUAGES HIP CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(gpu_p20_benchmark gpu_p20_benchmark.cpp)

if(BSF_NVIDIA_CUDA)
    set_source_files_properties(gpu_p20_benchmark.cpp PROPERTIES LANGUAGE CUDA)
    set_target_properties(gpu_p20_benchmark PROPERTIES
        CUDA_STANDARD 17
        CUDA_STANDARD_REQUIRED ON
        CUDA_SEPARABLE_COMPILATION OFF
    )
    target_compile_definitions(gpu_p20_benchmark PRIVATE BSF_NVIDIA_CUDA=1)
    target_compile_options(gpu_p20_benchmark PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-O3;--fmad=false;--ftz=false;--prec-div=true;--prec-sqrt=true>
    )
else()
    set_source_files_properties(gpu_p20_benchmark.cpp PROPERTIES LANGUAGE HIP)
    set_target_properties(gpu_p20_benchmark PROPERTIES
        HIP_STANDARD 17
        HIP_STANDARD_REQUIRED ON
    )
    if(NOT DEFINED CMAKE_HIP_ARCHITECTURES)
        set_property(TARGET gpu_p20_benchmark PROPERTY HIP_ARCHITECTURES gfx1101)
    endif()
    target_compile_options(gpu_p20_benchmark PRIVATE
        $<$<COMPILE_LANGUAGE:HIP>:-O3;-ffp-contract=off;-fno-fast-math;-fno-associative-math>
    )
endif()
