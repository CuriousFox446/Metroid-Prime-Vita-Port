#pragma once

// The part of Dawn's native API (dawn/native/DawnNative.h) that Aurora calls,
// for the null WebGPU: instance options that it ignores, and memory queries that
// report nothing allocated. Field names and types match Dawn's.

#include <webgpu/webgpu_cpp.h>

#include <cstdint>

namespace dawn::platform {
class Platform;
} // namespace dawn::platform

namespace dawn::native {

enum BackendValidationLevel { Full, Partial, Disabled };

struct DawnInstanceDescriptor : wgpu::ChainedStruct {
  DawnInstanceDescriptor() { sType = wgpu::SType::DawnInstanceDescriptor; }
  uint32_t additionalRuntimeSearchPathsCount = 0;
  const char* const* additionalRuntimeSearchPaths = nullptr;
  dawn::platform::Platform* platform = nullptr;
  BackendValidationLevel backendValidationLevel = BackendValidationLevel::Disabled;
  bool beginCaptureOnStartup = false;

  // The null device logs nothing.
  template <typename L>
  void SetLoggingCallback(L) {}
};

struct MemoryUsageInfo {
  uint64_t totalUsage = 0;
  uint64_t depthStencilTexturesUsage = 0;
  uint64_t msaaTexturesUsage = 0;
  uint64_t msaaTexturesCount = 0;
  uint64_t largestMsaaTextureUsage = 0;
  uint64_t texturesUsage = 0;
  uint64_t buffersUsage = 0;
};

struct AllocatorMemoryInfo {
  uint64_t totalUsedMemory = 0;
  uint64_t totalAllocatedMemory = 0;
  uint64_t totalLazyAllocatedMemory = 0;
  uint64_t totalLazyUsedMemory = 0;
};

inline MemoryUsageInfo ComputeEstimatedMemoryUsageInfo(WGPUDevice) { return {}; }
inline AllocatorMemoryInfo GetAllocatorMemoryInfo(WGPUDevice) { return {}; }
inline bool ReduceMemoryUsage(WGPUDevice) { return false; }
inline void PerformIdleTasks(const wgpu::Device&) {}
inline bool IsDeviceLost(WGPUDevice) { return false; }

} // namespace dawn::native
