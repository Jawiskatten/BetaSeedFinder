#pragma once

#if defined(BSF_NVIDIA_CUDA) || defined(__CUDACC__)

#include <cuda_runtime.h>

using hipError_t = cudaError_t;
using hipEvent_t = cudaEvent_t;
using hipDeviceProp_t = cudaDeviceProp;

static constexpr hipError_t hipSuccess = cudaSuccess;
static constexpr cudaMemcpyKind hipMemcpyHostToDevice = cudaMemcpyHostToDevice;
static constexpr cudaMemcpyKind hipMemcpyDeviceToHost = cudaMemcpyDeviceToHost;
static constexpr cudaMemcpyKind hipMemcpyDeviceToDevice = cudaMemcpyDeviceToDevice;

#define hipGetErrorString cudaGetErrorString
#define hipGetLastError cudaGetLastError
#define hipGetDevice cudaGetDevice
#define hipGetDeviceProperties cudaGetDeviceProperties
#define hipDeviceSynchronize cudaDeviceSynchronize
#define hipMalloc cudaMalloc
#define hipFree cudaFree
#define hipMemcpy cudaMemcpy
#define hipMemset cudaMemset
#define hipEventCreate cudaEventCreate
#define hipEventDestroy cudaEventDestroy
#define hipEventRecord cudaEventRecord
#define hipEventSynchronize cudaEventSynchronize
#define hipEventElapsedTime cudaEventElapsedTime

// HIP exposes wall_clock64(); CUDA exposes clock64(). Keep profiling source
// shared between both backends without changing the measured search logic.
#ifndef wall_clock64
#define wall_clock64 clock64
#endif

#define hipLaunchKernelGGL(kernelName, numBlocks, dimBlocks, sharedMemBytes, streamId, ...) \
    kernelName<<<numBlocks, dimBlocks, sharedMemBytes, streamId>>>(__VA_ARGS__)

#else

#include <hip/hip_runtime.h>

#endif
