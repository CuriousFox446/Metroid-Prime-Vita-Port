#pragma once

// A WebGPU implementation that accepts everything and renders nothing, so Aurora's
// renderer runs unmodified (GX translation, shader generation, command recording)
// on a target with no WebGPU driver. Used by the PS Vita headless build to measure
// the CPU cost of the game and the renderer before a real backend exists.
//
// Object lifetime is real (refcounted), mapped buffers have real memory and
// callbacks are delivered through futures, because the renderer relies on all
// three. GPU results (readbacks, timestamps) read back as zeros.

#include <webgpu/webgpu.h>

#include <atomic>
#include <cstdint>
#include <vector>

struct NullObject {
  std::atomic<uint32_t> refs{1};
  virtual ~NullObject() = default;
  void ref() { refs.fetch_add(1, std::memory_order_relaxed); }
  void unref() {
    if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      delete this;
    }
  }
};

struct WGPUInstanceImpl : NullObject {};
struct WGPUAdapterImpl : NullObject {
  WGPUInstance instance = nullptr;
  ~WGPUAdapterImpl() override;
};
struct WGPUQueueImpl : NullObject {};
struct WGPUDeviceImpl : NullObject {
  WGPUAdapter adapter = nullptr;
  WGPUQueue queue = nullptr;
  ~WGPUDeviceImpl() override;
};
struct WGPUBufferImpl : NullObject {
  uint64_t size = 0;
  WGPUBufferUsage usage = 0;
  WGPUBufferMapState mapState = WGPUBufferMapState_Unmapped;
  std::vector<uint8_t> data; // only while mapped, or kept for mappable buffers
};
struct WGPUTextureImpl : NullObject {
  WGPUTextureDescriptor desc{};
};
struct WGPUSurfaceImpl : NullObject {
  uint32_t width = 0;
  uint32_t height = 0;
  WGPUTextureFormat format = WGPUTextureFormat_BGRA8Unorm;
  WGPUTextureUsage usage = WGPUTextureUsage_RenderAttachment;
};
struct WGPUQuerySetImpl : NullObject {
  WGPUQueryType type = WGPUQueryType_Occlusion;
  uint32_t count = 0;
};
struct WGPUBindGroupImpl : NullObject {};
struct WGPUBindGroupLayoutImpl : NullObject {};
struct WGPUCommandBufferImpl : NullObject {};
struct WGPUCommandEncoderImpl : NullObject {};
struct WGPUComputePassEncoderImpl : NullObject {};
struct WGPUComputePipelineImpl : NullObject {};
struct WGPUExternalTextureImpl : NullObject {};
struct WGPUPipelineLayoutImpl : NullObject {};
struct WGPURenderBundleImpl : NullObject {};
struct WGPURenderBundleEncoderImpl : NullObject {};
struct WGPURenderPassEncoderImpl : NullObject {};
struct WGPURenderPipelineImpl : NullObject {};
struct WGPUResourceTableImpl : NullObject {};
struct WGPUSamplerImpl : NullObject {};
struct WGPUShaderModuleImpl : NullObject {};
struct WGPUSharedBufferMemoryImpl : NullObject {};
struct WGPUSharedFenceImpl : NullObject {};
struct WGPUSharedTextureMemoryImpl : NullObject {};
struct WGPUTexelBufferViewImpl : NullObject {};
struct WGPUTextureViewImpl : NullObject {};
