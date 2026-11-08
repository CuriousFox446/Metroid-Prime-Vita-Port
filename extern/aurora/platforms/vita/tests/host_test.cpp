// Host-side checks for the two Vita replacements, which are plain C++: the GCM
// reader (nod_gcm.cpp) on a synthetic image, and the null WebGPU driven through
// Dawn's C++ wrapper the way Aurora's gpu.cpp drives a device.
//
// From this directory:
//   g++ -std=c++20 -I../nod/include -I../webgpu_null/include host_test.cpp
//       ../nod/nod_gcm.cpp ../webgpu_null/null_webgpu.cpp ../webgpu_null/null_webgpu_stubs.cpp -o host_test

#include <nod.h>
#include <webgpu/webgpu_cpp.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                      \
      ++g_failures;                                                                                                    \
    }                                                                                                                  \
  } while (0)

namespace {

void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  v[at] = uint8_t(x >> 24);
  v[at + 1] = uint8_t(x >> 16);
  v[at + 2] = uint8_t(x >> 8);
  v[at + 3] = uint8_t(x);
}

// root/ { a.bin, sub/ { b.bin } , c.bin }
std::vector<uint8_t> makeImage() {
  std::vector<uint8_t> img(0x10000, 0);
  std::memcpy(img.data(), "GM8E01", 6);
  img[7] = 0; // disc version
  put32(img, 0x1C, 0xC2339F3D);
  // Apploader: 0x20 header, body 0x40, trailer 0x10.
  put32(img, 0x2440 + 0x14, 0x40);
  put32(img, 0x2440 + 0x18, 0x10);
  // DOL at 0x3000: one text section at file offset 0x100, 0x80 long.
  put32(img, 0x420, 0x3000);
  put32(img, 0x3000 + 0x00, 0x100);
  put32(img, 0x3000 + 0x90, 0x80);
  // FST at 0x4000.
  const uint32_t fst = 0x4000;
  const char names[] = "a.bin\0sub\0b.bin\0c.bin\0";
  const uint32_t count = 5;
  put32(img, 0x424, fst);
  put32(img, 0x428, count * 12 + sizeof(names));
  auto entry = [&](uint32_t i, bool dir, uint32_t name, uint32_t off, uint32_t len) {
    put32(img, fst + i * 12, (dir ? 0x01000000u : 0u) | name);
    put32(img, fst + i * 12 + 4, off);
    put32(img, fst + i * 12 + 8, len);
  };
  entry(0, true, 0, 0, count);
  entry(1, false, 0, 0x8000, 5);     // a.bin
  entry(2, true, 6, 0, 4);           // sub/ (parent 0, ends at 4)
  entry(3, false, 10, 0x8100, 3);    // sub/b.bin
  entry(4, false, 16, 0x8200, 0x20); // c.bin
  std::memcpy(img.data() + fst + count * 12, names, sizeof(names));
  std::memcpy(img.data() + 0x8000, "hello", 5);
  std::memcpy(img.data() + 0x8100, "abc", 3);
  for (int i = 0; i < 0x20; ++i) {
    img[0x8200 + i] = uint8_t(i);
  }
  return img;
}

struct Mem {
  std::vector<uint8_t> data;
  bool closed = false;
};

int64_t readAt(void* user, uint64_t off, void* out, size_t len) {
  auto* m = static_cast<Mem*>(user);
  if (off + len > m->data.size()) {
    return -1;
  }
  std::memcpy(out, m->data.data() + off, len);
  return int64_t(len);
}
int64_t streamLen(void* user) { return int64_t(static_cast<Mem*>(user)->data.size()); }
void closeStream(void* user) { static_cast<Mem*>(user)->closed = true; }

void testGcm() {
  Mem mem{makeImage()};
  const NodDiscStream stream{&mem, readAt, streamLen, closeStream};
  NodHandle* disc = nullptr;
  CHECK(nod_disc_open_stream(&stream, nullptr, &disc) == NOD_RESULT_OK && disc != nullptr);
  NodDiscHeader header{};
  CHECK(nod_disc_header(disc, &header) == NOD_RESULT_OK);
  CHECK(std::memcmp(header.game_id, "GM8E01", 6) == 0);

  NodHandle* part = nullptr;
  CHECK(nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_DATA, nullptr, &part) == NOD_RESULT_OK);
  NodPartitionMeta meta{};
  CHECK(nod_partition_meta(part, &meta) == NOD_RESULT_OK);
  CHECK(meta.raw_dol.size == 0x180);
  CHECK(meta.raw_apploader.size == 0x70);
  CHECK(meta.raw_fst.size == 5 * 12 + 23); // the names, with the literal's own terminator
  CHECK(!nod_partition_is_wii(part));

  // Iteration as fst.cpp does it: index + 1 each time; directories give their end.
  std::string seen;
  nod_partition_iterate_fst(
      part,
      [](uint32_t i, NodNodeKind kind, const char* name, uint32_t size, void* user) -> uint32_t {
        *static_cast<std::string*>(user) += std::to_string(i) + (kind == NOD_NODE_KIND_DIRECTORY ? "d:" : "f:") +
                                            name + ":" + std::to_string(size) + " ";
        return i + 1;
      },
      &seen);
  CHECK(seen == "1f:a.bin:5 2d:sub:4 3f:b.bin:3 4f:c.bin:32 ");

  NodHandle* file = nullptr;
  CHECK(nod_partition_open_file(part, 3, &file) == NOD_RESULT_OK);
  uint8_t buf[16] = {};
  CHECK(nod_read(file, buf, sizeof(buf)) == 3 && std::memcmp(buf, "abc", 3) == 0);
  CHECK(nod_read(file, buf, sizeof(buf)) == 0); // end of file
  nod_free(file);

  CHECK(nod_partition_open_file(part, 4, &file) == NOD_RESULT_OK);
  CHECK(nod_seek(file, -4, SEEK_END) == 28);
  CHECK(nod_read(file, buf, 4) == 4 && buf[0] == 28 && buf[3] == 31);
  CHECK(nod_seek(file, 2, SEEK_SET) == 2 && nod_read(file, buf, 1) == 1 && buf[0] == 2);
  nod_free(file);

  CHECK(nod_partition_open_file(part, 2, &file) == NOD_RESULT_ERR_NOT_FOUND); // a directory
  CHECK(nod_partition_open_file(part, 9, &file) == NOD_RESULT_ERR_NOT_FOUND);

  // The disc handle reads absolute offsets (DVDReadAbs).
  CHECK(nod_seek(disc, 0x8000, SEEK_SET) == 0x8000 && nod_read(disc, buf, 5) == 5 && std::memcmp(buf, "hello", 5) == 0);

  // The stream stays open while any handle does.
  nod_free(disc);
  CHECK(!mem.closed);
  nod_free(part);
  CHECK(mem.closed);

  // A Wii image, and garbage, are refused (and the stream closed).
  Mem wii{makeImage()};
  put32(wii.data, 0x1C, 0);
  put32(wii.data, 0x18, 0x5D1C9EA3);
  const NodDiscStream wiiStream{&wii, readAt, streamLen, closeStream};
  CHECK(nod_disc_open_stream(&wiiStream, nullptr, &disc) != NOD_RESULT_OK && disc == nullptr && wii.closed);
}

void testNullWebGpu() {
  wgpu::Instance instance = wgpu::CreateInstance(nullptr);
  CHECK(instance);
  wgpu::Adapter adapter;
  const wgpu::RequestAdapterOptions options{};
  auto future = instance.RequestAdapter(&options, wgpu::CallbackMode::WaitAnyOnly,
                                        [&](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView) {
                                          CHECK(status == wgpu::RequestAdapterStatus::Success);
                                          adapter = std::move(a);
                                        });
  CHECK(!adapter); // WaitAnyOnly: not before the wait
  CHECK(instance.WaitAny(future, 1000) == wgpu::WaitStatus::Success);
  CHECK(adapter);
  wgpu::AdapterInfo info;
  adapter.GetInfo(&info);
  CHECK(info.backendType == wgpu::BackendType::Null);
  wgpu::Limits limits{};
  adapter.GetLimits(&limits);
  CHECK(limits.maxTextureDimension2D >= 2048 && limits.minUniformBufferOffsetAlignment == 256);

  wgpu::Device device;
  wgpu::DeviceDescriptor deviceDesc{};
  future = adapter.RequestDevice(&deviceDesc, wgpu::CallbackMode::WaitAnyOnly,
                                 [&](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView) {
                                   CHECK(status == wgpu::RequestDeviceStatus::Success);
                                   device = std::move(d);
                                 });
  CHECK(instance.WaitAny(future, 1000) == wgpu::WaitStatus::Success && device);
  wgpu::Queue queue = device.GetQueue();
  CHECK(queue && queue.Get() == device.GetQueue().Get());

  // Staging buffer: mapped at creation, then MapAsync round trip.
  const wgpu::BufferDescriptor staging{.usage = wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc,
                                       .size = 256,
                                       .mappedAtCreation = true};
  wgpu::Buffer buffer = device.CreateBuffer(&staging);
  auto* mapped = static_cast<uint8_t*>(buffer.GetMappedRange());
  CHECK(mapped != nullptr && buffer.GetSize() == 256);
  mapped[255] = 7;
  buffer.Unmap();
  CHECK(buffer.GetMapState() == wgpu::BufferMapState::Unmapped && buffer.GetMappedRange() == nullptr);
  bool mappedAgain = false;
  buffer.MapAsync(wgpu::MapMode::Write, 0, 256, wgpu::CallbackMode::AllowProcessEvents,
                  [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
                    mappedAgain = status == wgpu::MapAsyncStatus::Success;
                  });
  instance.ProcessEvents();
  CHECK(mappedAgain && static_cast<uint8_t*>(buffer.GetMappedRange())[255] == 7);

  // A frame: surface texture, encoder, pass, submit, present.
  const wgpu::SurfaceDescriptor surfaceDesc{};
  wgpu::Surface surface = instance.CreateSurface(&surfaceDesc);
  wgpu::SurfaceCapabilities caps;
  CHECK(surface.GetCapabilities(adapter, &caps) == wgpu::Status::Success && caps.formatCount > 0);
  const wgpu::SurfaceConfiguration config{
      .device = device, .format = caps.formats[0], .width = 960, .height = 544, .presentMode = caps.presentModes[0]};
  surface.Configure(&config);
  wgpu::SurfaceTexture current;
  surface.GetCurrentTexture(&current);
  CHECK(current.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal && current.texture.GetWidth() == 960);
  wgpu::TextureView view = current.texture.CreateView();
  const wgpu::RenderPassColorAttachment color{.view = view, .loadOp = wgpu::LoadOp::Clear,
                                              .storeOp = wgpu::StoreOp::Store};
  const wgpu::RenderPassDescriptor passDesc{.colorAttachmentCount = 1, .colorAttachments = &color};
  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
  wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);
  pass.Draw(3);
  pass.End();
  wgpu::CommandBuffer commands = encoder.Finish();
  bool done = false;
  queue.OnSubmittedWorkDone(wgpu::CallbackMode::AllowSpontaneous,
                            [&](wgpu::QueueWorkDoneStatus, wgpu::StringView) { done = true; });
  queue.Submit(1, &commands);
  CHECK(done);
  CHECK(surface.Present() == wgpu::Status::Success);

  const wgpu::TextureDescriptor texDesc{.usage = wgpu::TextureUsage::TextureBinding,
                                        .size = {64, 32, 1},
                                        .format = wgpu::TextureFormat::RGBA8Unorm,
                                        .mipLevelCount = 3};
  wgpu::Texture texture = device.CreateTexture(&texDesc);
  CHECK(texture.GetHeight() == 32 && texture.GetMipLevelCount() == 3 &&
        texture.GetFormat() == wgpu::TextureFormat::RGBA8Unorm);
}

} // namespace

int main() {
  testGcm();
  testNullWebGpu();
  std::printf(g_failures == 0 ? "host_test: all passed\n" : "host_test: %d failed\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
