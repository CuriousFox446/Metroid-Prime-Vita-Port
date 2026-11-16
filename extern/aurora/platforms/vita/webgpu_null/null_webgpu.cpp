// The hand-written part of the null WebGPU implementation: objects with state,
// futures, and the queries the renderer sizes itself from. Everything else is
// generated into null_webgpu_stubs.cpp by gen_stubs.py, which skips any function
// defined here.

#include "null_webgpu.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <mutex>
#include <unordered_map>

WGPUAdapterImpl::~WGPUAdapterImpl() {
  if (instance != nullptr) {
    instance->unref();
  }
}

WGPUDeviceImpl::~WGPUDeviceImpl() {
  if (queue != nullptr) {
    queue->unref();
  }
  if (adapter != nullptr) {
    adapter->unref();
  }
}

namespace {

// ── Futures ──
// Every asynchronous call completes at once, but its callback is delivered the
// way WebGPU says it may be: from WaitAny for that future, or from ProcessEvents
// (and, for AllowSpontaneous, after a submit or present) unless WaitAnyOnly.
struct Pending {
  WGPUCallbackMode mode;
  std::function<void()> fire;
};

std::mutex g_futureLock;
uint64_t g_nextFuture = 1;
std::unordered_map<uint64_t, Pending> g_pending;

WGPUFuture enqueue(WGPUCallbackMode mode, std::function<void()> fire) {
  std::lock_guard guard(g_futureLock);
  const uint64_t id = g_nextFuture++;
  g_pending.emplace(id, Pending{mode, std::move(fire)});
  return WGPUFuture{id};
}

// Fires (outside the lock: callbacks re-enter the API) every pending callback
// that `select` accepts.
template <typename Select>
void fireWhere(Select&& select) {
  std::vector<std::function<void()>> ready;
  {
    std::lock_guard guard(g_futureLock);
    for (auto it = g_pending.begin(); it != g_pending.end();) {
      if (select(it->first, it->second)) {
        ready.push_back(std::move(it->second.fire));
        it = g_pending.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& fire : ready) {
    fire();
  }
}

void fireSpontaneous() {
  fireWhere([](uint64_t, const Pending& p) { return p.mode == WGPUCallbackMode_AllowSpontaneous; });
}

WGPUStringView view(const char* s) { return WGPUStringView{s, std::strlen(s)}; }
constexpr WGPUStringView kEmpty{nullptr, 0};

// ── Capabilities ──
constexpr WGPUFeatureName kFeatures[] = {
    WGPUFeatureName_CoreFeaturesAndLimits,
    WGPUFeatureName_DualSourceBlending,
};
constexpr WGPUTextureFormat kSurfaceFormats[] = {WGPUTextureFormat_BGRA8Unorm, WGPUTextureFormat_RGBA8Unorm};
constexpr WGPUPresentMode kPresentModes[] = {WGPUPresentMode_Fifo};
constexpr WGPUCompositeAlphaMode kAlphaModes[] = {WGPUCompositeAlphaMode_Opaque};
constexpr WGPUInstanceFeatureName kInstanceFeatures[] = {WGPUInstanceFeatureName_TimedWaitAny};

void fillLimits(WGPULimits* limits) {
  WGPUChainedStruct* const next = limits->nextInChain;
  *limits = WGPULimits{};
  limits->nextInChain = next;
  limits->maxTextureDimension1D = 4096;
  limits->maxTextureDimension2D = 4096;
  limits->maxTextureDimension3D = 1024;
  limits->maxTextureArrayLayers = 256;
  limits->maxBindGroups = 4;
  limits->maxBindGroupsPlusVertexBuffers = 24;
  limits->maxBindingsPerBindGroup = 1000;
  limits->maxDynamicUniformBuffersPerPipelineLayout = 8;
  limits->maxDynamicStorageBuffersPerPipelineLayout = 4;
  limits->maxSampledTexturesPerShaderStage = 16;
  limits->maxSamplersPerShaderStage = 16;
  limits->maxStorageBuffersPerShaderStage = 8;
  limits->maxStorageTexturesPerShaderStage = 4;
  limits->maxUniformBuffersPerShaderStage = 12;
  limits->maxUniformBufferBindingSize = 65536;
  limits->maxStorageBufferBindingSize = 128u << 20;
  limits->minUniformBufferOffsetAlignment = 256;
  limits->minStorageBufferOffsetAlignment = 256;
  limits->maxVertexBuffers = 8;
  limits->maxBufferSize = 256u << 20;
  limits->maxVertexAttributes = 16;
  limits->maxVertexBufferArrayStride = 2048;
  limits->maxInterStageShaderVariables = 16;
  limits->maxColorAttachments = 8;
  limits->maxColorAttachmentBytesPerSample = 32;
  limits->maxComputeWorkgroupStorageSize = 16384;
  limits->maxComputeInvocationsPerWorkgroup = 256;
  limits->maxComputeWorkgroupSizeX = 256;
  limits->maxComputeWorkgroupSizeY = 256;
  limits->maxComputeWorkgroupSizeZ = 64;
  limits->maxComputeWorkgroupsPerDimension = 65535;
  limits->maxImmediateSize = 64;
}

void fillInfo(WGPUAdapterInfo* info) {
  WGPUChainedStruct* const next = info->nextInChain;
  *info = WGPUAdapterInfo{};
  info->nextInChain = next;
  info->vendor = view("aurora");
  info->architecture = view("null");
  info->device = view("Null renderer");
  info->description = view("Headless: accepts all commands, draws nothing");
  info->backendType = WGPUBackendType_Null;
  info->adapterType = WGPUAdapterType_CPU;
}

bool mappable(const WGPUBufferImpl* b) { return (b->usage & (WGPUBufferUsage_MapRead | WGPUBufferUsage_MapWrite)) != 0; }

} // namespace

extern "C" {

// ── Instance ──
WGPUInstance wgpuCreateInstance(WGPUInstanceDescriptor const* descriptor) {
  (void)descriptor;
  return new WGPUInstanceImpl;
}

void wgpuGetInstanceFeatures(WGPUSupportedInstanceFeatures* features) {
  features->featureCount = std::size(kInstanceFeatures);
  features->features = kInstanceFeatures;
}

WGPUStatus wgpuGetInstanceLimits(WGPUInstanceLimits* limits) {
  limits->timedWaitAnyMaxCount = 64;
  return WGPUStatus_Success;
}

WGPUBool wgpuHasInstanceFeature(WGPUInstanceFeatureName feature) {
  return feature == WGPUInstanceFeatureName_TimedWaitAny;
}

WGPUFuture wgpuInstanceRequestAdapter(WGPUInstance instance, WGPURequestAdapterOptions const* options,
                                      WGPURequestAdapterCallbackInfo callbackInfo) {
  (void)options;
  auto* adapter = new WGPUAdapterImpl;
  instance->ref();
  adapter->instance = instance;
  return enqueue(callbackInfo.mode, [adapter, callbackInfo] {
    // The callback takes the reference.
    callbackInfo.callback(WGPURequestAdapterStatus_Success, adapter, kEmpty, callbackInfo.userdata1,
                          callbackInfo.userdata2);
  });
}

void wgpuInstanceProcessEvents(WGPUInstance instance) {
  (void)instance;
  fireWhere([](uint64_t, const Pending& p) { return p.mode != WGPUCallbackMode_WaitAnyOnly; });
}

WGPUWaitStatus wgpuInstanceWaitAny(WGPUInstance instance, size_t futureCount, WGPUFutureWaitInfo* futures,
                                   uint64_t timeoutNS) {
  (void)instance;
  (void)timeoutNS;
  if (futureCount == 0) {
    return WGPUWaitStatus_Success;
  }
  fireWhere([&](uint64_t id, const Pending&) {
    return std::any_of(futures, futures + futureCount, [id](const WGPUFutureWaitInfo& f) { return f.future.id == id; });
  });
  // Every future is complete once its callback has run, and all of them have now.
  for (size_t i = 0; i < futureCount; ++i) {
    futures[i].completed = futures[i].future.id != 0;
  }
  return WGPUWaitStatus_Success;
}

void wgpuInstanceGetWGSLLanguageFeatures(WGPUInstance instance, WGPUSupportedWGSLLanguageFeatures* features) {
  (void)instance;
  features->featureCount = 0;
  features->features = nullptr;
}

WGPUBool wgpuInstanceHasWGSLLanguageFeature(WGPUInstance instance, WGPUWGSLLanguageFeatureName feature) {
  (void)instance;
  (void)feature;
  return 1;
}

// ── Adapter ──
WGPUStatus wgpuAdapterGetInfo(WGPUAdapter adapter, WGPUAdapterInfo* info) {
  (void)adapter;
  fillInfo(info);
  return WGPUStatus_Success;
}

WGPUStatus wgpuAdapterGetLimits(WGPUAdapter adapter, WGPULimits* limits) {
  (void)adapter;
  fillLimits(limits);
  return WGPUStatus_Success;
}

void wgpuAdapterGetFeatures(WGPUAdapter adapter, WGPUSupportedFeatures* features) {
  (void)adapter;
  features->featureCount = std::size(kFeatures);
  features->features = kFeatures;
}

WGPUBool wgpuAdapterHasFeature(WGPUAdapter adapter, WGPUFeatureName feature) {
  (void)adapter;
  return std::find(std::begin(kFeatures), std::end(kFeatures), feature) != std::end(kFeatures);
}

WGPUInstance wgpuAdapterGetInstance(WGPUAdapter adapter) {
  adapter->instance->ref();
  return adapter->instance;
}

WGPUDevice wgpuAdapterCreateDevice(WGPUAdapter adapter, WGPUDeviceDescriptor const* descriptor) {
  (void)descriptor;
  auto* device = new WGPUDeviceImpl;
  adapter->ref();
  device->adapter = adapter;
  device->queue = new WGPUQueueImpl;
  return device;
}

WGPUFuture wgpuAdapterRequestDevice(WGPUAdapter adapter, WGPUDeviceDescriptor const* descriptor,
                                    WGPURequestDeviceCallbackInfo callbackInfo) {
  WGPUDevice device = wgpuAdapterCreateDevice(adapter, descriptor);
  return enqueue(callbackInfo.mode, [device, callbackInfo] {
    callbackInfo.callback(WGPURequestDeviceStatus_Success, device, kEmpty, callbackInfo.userdata1,
                          callbackInfo.userdata2);
  });
}

// Capability arrays are static, so the FreeMembers functions have nothing to free.
void wgpuAdapterInfoFreeMembers(WGPUAdapterInfo adapterInfo) { (void)adapterInfo; }
void wgpuSupportedFeaturesFreeMembers(WGPUSupportedFeatures supportedFeatures) { (void)supportedFeatures; }
void wgpuSupportedInstanceFeaturesFreeMembers(WGPUSupportedInstanceFeatures supportedInstanceFeatures) {
  (void)supportedInstanceFeatures;
}
void wgpuSupportedWGSLLanguageFeaturesFreeMembers(WGPUSupportedWGSLLanguageFeatures supportedWGSLLanguageFeatures) {
  (void)supportedWGSLLanguageFeatures;
}
void wgpuSurfaceCapabilitiesFreeMembers(WGPUSurfaceCapabilities surfaceCapabilities) { (void)surfaceCapabilities; }

// ── Device ──
WGPUQueue wgpuDeviceGetQueue(WGPUDevice device) {
  device->queue->ref();
  return device->queue;
}

WGPUAdapter wgpuDeviceGetAdapter(WGPUDevice device) {
  device->adapter->ref();
  return device->adapter;
}

WGPUStatus wgpuDeviceGetAdapterInfo(WGPUDevice device, WGPUAdapterInfo* adapterInfo) {
  (void)device;
  fillInfo(adapterInfo);
  return WGPUStatus_Success;
}

WGPUStatus wgpuDeviceGetLimits(WGPUDevice device, WGPULimits* limits) {
  (void)device;
  fillLimits(limits);
  return WGPUStatus_Success;
}

void wgpuDeviceGetFeatures(WGPUDevice device, WGPUSupportedFeatures* features) {
  wgpuAdapterGetFeatures(device->adapter, features);
}

WGPUBool wgpuDeviceHasFeature(WGPUDevice device, WGPUFeatureName feature) {
  return wgpuAdapterHasFeature(device->adapter, feature);
}

WGPUBuffer wgpuDeviceCreateBuffer(WGPUDevice device, WGPUBufferDescriptor const* descriptor) {
  (void)device;
  auto* buffer = new WGPUBufferImpl;
  buffer->size = descriptor->size;
  buffer->usage = descriptor->usage;
  if (descriptor->mappedAtCreation) {
    buffer->data.assign(static_cast<size_t>(descriptor->size), 0);
    buffer->mapState = WGPUBufferMapState_Mapped;
  }
  return buffer;
}

WGPUBuffer wgpuDeviceCreateErrorBuffer(WGPUDevice device, WGPUBufferDescriptor const* descriptor) {
  return wgpuDeviceCreateBuffer(device, descriptor);
}

WGPUTexture wgpuDeviceCreateTexture(WGPUDevice device, WGPUTextureDescriptor const* descriptor) {
  (void)device;
  auto* texture = new WGPUTextureImpl;
  texture->desc = *descriptor;
  texture->desc.nextInChain = nullptr;
  texture->desc.label = kEmpty;
  texture->desc.viewFormatCount = 0;
  texture->desc.viewFormats = nullptr;
  return texture;
}

WGPUTexture wgpuDeviceCreateErrorTexture(WGPUDevice device, WGPUTextureDescriptor const* descriptor) {
  return wgpuDeviceCreateTexture(device, descriptor);
}

WGPUFuture wgpuDevicePopErrorScope(WGPUDevice device, WGPUPopErrorScopeCallbackInfo callbackInfo) {
  (void)device;
  return enqueue(callbackInfo.mode, [callbackInfo] {
    callbackInfo.callback(WGPUPopErrorScopeStatus_Success, WGPUErrorType_NoError, kEmpty, callbackInfo.userdata1,
                          callbackInfo.userdata2);
  });
}

WGPUFuture wgpuDeviceCreateRenderPipelineAsync(WGPUDevice device, WGPURenderPipelineDescriptor const* descriptor,
                                               WGPUCreateRenderPipelineAsyncCallbackInfo callbackInfo) {
  (void)device;
  (void)descriptor;
  auto* pipeline = new WGPURenderPipelineImpl;
  return enqueue(callbackInfo.mode, [pipeline, callbackInfo] {
    callbackInfo.callback(WGPUCreatePipelineAsyncStatus_Success, pipeline, kEmpty, callbackInfo.userdata1,
                          callbackInfo.userdata2);
  });
}

WGPUFuture wgpuDeviceCreateComputePipelineAsync(WGPUDevice device, WGPUComputePipelineDescriptor const* descriptor,
                                                WGPUCreateComputePipelineAsyncCallbackInfo callbackInfo) {
  (void)device;
  (void)descriptor;
  auto* pipeline = new WGPUComputePipelineImpl;
  return enqueue(callbackInfo.mode, [pipeline, callbackInfo] {
    callbackInfo.callback(WGPUCreatePipelineAsyncStatus_Success, pipeline, kEmpty, callbackInfo.userdata1,
                          callbackInfo.userdata2);
  });
}

// The device is never lost: a future that is never completed.
WGPUFuture wgpuDeviceGetLostFuture(WGPUDevice device) {
  (void)device;
  return WGPUFuture{0};
}

// ── Buffer ──
WGPUFuture wgpuBufferMapAsync(WGPUBuffer buffer, WGPUMapMode mode, size_t offset, size_t size,
                              WGPUBufferMapCallbackInfo callbackInfo) {
  (void)mode;
  (void)offset;
  (void)size;
  buffer->ref();
  return enqueue(callbackInfo.mode, [buffer, callbackInfo] {
    if (buffer->data.size() != buffer->size) {
      buffer->data.assign(static_cast<size_t>(buffer->size), 0);
    }
    buffer->mapState = WGPUBufferMapState_Mapped;
    callbackInfo.callback(WGPUMapAsyncStatus_Success, kEmpty, callbackInfo.userdata1, callbackInfo.userdata2);
    buffer->unref();
  });
}

void* wgpuBufferGetMappedRange(WGPUBuffer buffer, size_t offset, size_t size) {
  if (buffer->mapState != WGPUBufferMapState_Mapped || offset > buffer->data.size()) {
    return nullptr;
  }
  if (size != WGPU_WHOLE_MAP_SIZE && size > buffer->data.size() - offset) {
    return nullptr;
  }
  return buffer->data.data() + offset;
}

void const* wgpuBufferGetConstMappedRange(WGPUBuffer buffer, size_t offset, size_t size) {
  return wgpuBufferGetMappedRange(buffer, offset, size);
}

WGPUStatus wgpuBufferReadMappedRange(WGPUBuffer buffer, size_t offset, void* data, size_t size) {
  const void* src = wgpuBufferGetMappedRange(buffer, offset, size);
  if (src == nullptr) {
    return WGPUStatus_Error;
  }
  std::memcpy(data, src, size);
  return WGPUStatus_Success;
}

WGPUStatus wgpuBufferWriteMappedRange(WGPUBuffer buffer, size_t offset, void const* data, size_t size) {
  void* dst = wgpuBufferGetMappedRange(buffer, offset, size);
  if (dst == nullptr) {
    return WGPUStatus_Error;
  }
  std::memcpy(dst, data, size);
  return WGPUStatus_Success;
}

void wgpuBufferUnmap(WGPUBuffer buffer) {
  buffer->mapState = WGPUBufferMapState_Unmapped;
  // Mappable (staging) buffers keep their memory for the next map; the rest have
  // nowhere for it to go.
  if (!mappable(buffer)) {
    std::vector<uint8_t>().swap(buffer->data);
  }
}

void wgpuBufferDestroy(WGPUBuffer buffer) {
  buffer->mapState = WGPUBufferMapState_Unmapped;
  std::vector<uint8_t>().swap(buffer->data);
}

WGPUBufferMapState wgpuBufferGetMapState(WGPUBuffer buffer) { return buffer->mapState; }
uint64_t wgpuBufferGetSize(WGPUBuffer buffer) { return buffer->size; }
WGPUBufferUsage wgpuBufferGetUsage(WGPUBuffer buffer) { return buffer->usage; }

// ── Texture ──
void wgpuTextureDestroy(WGPUTexture texture) { (void)texture; }
uint32_t wgpuTextureGetWidth(WGPUTexture texture) { return texture->desc.size.width; }
uint32_t wgpuTextureGetHeight(WGPUTexture texture) { return texture->desc.size.height; }
uint32_t wgpuTextureGetDepthOrArrayLayers(WGPUTexture texture) { return texture->desc.size.depthOrArrayLayers; }
uint32_t wgpuTextureGetMipLevelCount(WGPUTexture texture) { return texture->desc.mipLevelCount; }
uint32_t wgpuTextureGetSampleCount(WGPUTexture texture) { return texture->desc.sampleCount; }
WGPUTextureDimension wgpuTextureGetDimension(WGPUTexture texture) { return texture->desc.dimension; }
WGPUTextureFormat wgpuTextureGetFormat(WGPUTexture texture) { return texture->desc.format; }
WGPUTextureUsage wgpuTextureGetUsage(WGPUTexture texture) { return texture->desc.usage; }
WGPUTextureViewDimension wgpuTextureGetTextureBindingViewDimension(WGPUTexture texture) {
  (void)texture;
  return WGPUTextureViewDimension_Undefined;
}

// ── Queue ──
void wgpuQueueSubmit(WGPUQueue queue, size_t commandCount, WGPUCommandBuffer const* commands) {
  (void)queue;
  (void)commandCount;
  (void)commands;
  fireSpontaneous();
}

void wgpuQueueWriteBuffer(WGPUQueue queue, WGPUBuffer buffer, uint64_t bufferOffset, void const* data, size_t size) {
  (void)queue;
  (void)buffer;
  (void)bufferOffset;
  (void)data;
  (void)size;
}

WGPUFuture wgpuQueueOnSubmittedWorkDone(WGPUQueue queue, WGPUQueueWorkDoneCallbackInfo callbackInfo) {
  (void)queue;
  return enqueue(callbackInfo.mode, [callbackInfo] {
    callbackInfo.callback(WGPUQueueWorkDoneStatus_Success, kEmpty, callbackInfo.userdata1, callbackInfo.userdata2);
  });
}

// ── Surface ──
WGPUStatus wgpuSurfaceGetCapabilities(WGPUSurface surface, WGPUAdapter adapter,
                                      WGPUSurfaceCapabilities* capabilities) {
  (void)surface;
  (void)adapter;
  capabilities->usages = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
  capabilities->formatCount = std::size(kSurfaceFormats);
  capabilities->formats = kSurfaceFormats;
  capabilities->presentModeCount = std::size(kPresentModes);
  capabilities->presentModes = kPresentModes;
  capabilities->alphaModeCount = std::size(kAlphaModes);
  capabilities->alphaModes = kAlphaModes;
  return WGPUStatus_Success;
}

void wgpuSurfaceConfigure(WGPUSurface surface, WGPUSurfaceConfiguration const* config) {
  surface->width = config->width;
  surface->height = config->height;
  surface->format = config->format;
  surface->usage = config->usage;
}

void wgpuSurfaceUnconfigure(WGPUSurface surface) {
  surface->width = 0;
  surface->height = 0;
}

void wgpuSurfaceGetCurrentTexture(WGPUSurface surface, WGPUSurfaceTexture* surfaceTexture) {
  auto* texture = new WGPUTextureImpl;
  texture->desc.usage = surface->usage;
  texture->desc.dimension = WGPUTextureDimension_2D;
  texture->desc.size = {surface->width, surface->height, 1};
  texture->desc.format = surface->format;
  texture->desc.mipLevelCount = 1;
  texture->desc.sampleCount = 1;
  surfaceTexture->texture = texture;
  surfaceTexture->status = WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal;
}

WGPUStatus wgpuSurfacePresent(WGPUSurface surface) {
  (void)surface;
  fireSpontaneous();
  return WGPUStatus_Success;
}

// ── Shader module, query set ──
WGPUFuture wgpuShaderModuleGetCompilationInfo(WGPUShaderModule shaderModule,
                                              WGPUCompilationInfoCallbackInfo callbackInfo) {
  (void)shaderModule;
  return enqueue(callbackInfo.mode, [callbackInfo] {
    const WGPUCompilationInfo info{nullptr, 0, nullptr};
    callbackInfo.callback(WGPUCompilationInfoRequestStatus_Success, &info, callbackInfo.userdata1,
                          callbackInfo.userdata2);
  });
}

WGPUQuerySet wgpuDeviceCreateQuerySet(WGPUDevice device, WGPUQuerySetDescriptor const* descriptor) {
  (void)device;
  auto* querySet = new WGPUQuerySetImpl;
  querySet->type = descriptor->type;
  querySet->count = descriptor->count;
  return querySet;
}

uint32_t wgpuQuerySetGetCount(WGPUQuerySet querySet) { return querySet->count; }
WGPUQueryType wgpuQuerySetGetType(WGPUQuerySet querySet) { return querySet->type; }

} // extern "C"
