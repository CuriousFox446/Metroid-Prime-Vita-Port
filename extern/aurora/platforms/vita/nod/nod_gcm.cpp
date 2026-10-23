// A plain GameCube disc image reader behind nod's C API (see include/nod.h).
//
// Handles share one Disc, which owns the caller's stream. The stream's read_at
// seeks then reads (Aurora's SDL callback), so every access is serialised on the
// disc's mutex: the DVD worker and the main thread read concurrently.

#include <nod.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kGcnMagic = 0xC2339F3D;
constexpr uint32_t kWiiMagic = 0x5D1C9EA3;
constexpr size_t kBootSize = 0x440;
constexpr size_t kBi2Offset = 0x440;
constexpr size_t kBi2Size = 0x2000;
constexpr size_t kApploaderOffset = 0x2440;
// Sanity limits for sizes read from the image, so a damaged header fails the open
// instead of asking for gigabytes.
constexpr uint32_t kMaxDolSize = 64u << 20;
constexpr uint32_t kMaxFstSize = 16u << 20;
constexpr uint32_t kMaxApploaderSize = 4u << 20;

uint32_t be32(const uint8_t* p) {
  return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}

struct Disc {
  NodDiscStream stream{};
  std::mutex lock;
  uint64_t length = 0;
  std::atomic<int> refs{1};

  bool readAt(uint64_t offset, void* out, size_t len) {
    if (offset > length || len > length - offset) {
      return false;
    }
    std::lock_guard guard(lock);
    return stream.read_at(stream.user_data, offset, out, len) == static_cast<int64_t>(len);
  }

  void retain() { refs.fetch_add(1, std::memory_order_relaxed); }
  void release() {
    if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      if (stream.close != nullptr) {
        stream.close(stream.user_data);
      }
      delete this;
    }
  }
};

struct Partition {
  std::vector<uint8_t> boot;
  std::vector<uint8_t> bi2;
  std::vector<uint8_t> apploader;
  std::vector<uint8_t> dol;
  std::vector<uint8_t> fst;
  uint32_t entryCount = 0;
  size_t stringTable = 0;
};

enum class Kind { Disc, Partition, File };

} // namespace

struct NodHandle {
  Kind kind;
  Disc* disc;
  std::shared_ptr<Partition> partition;
  // A readable window of the image: the whole disc, or one file.
  uint64_t base = 0;
  uint64_t size = 0;
  uint64_t pos = 0;

  NodHandle(Kind k, Disc* d) : kind(k), disc(d) { disc->retain(); }
  ~NodHandle() { disc->release(); }
};

namespace {

std::vector<uint8_t> readBlob(Disc& disc, uint64_t offset, size_t size, bool& ok) {
  std::vector<uint8_t> out(size);
  if (size != 0 && !disc.readAt(offset, out.data(), size)) {
    ok = false;
  }
  return out;
}

// A DOL's size is the end of its furthest section: seven text and eleven data
// offsets at 0x00, their sizes at 0x90.
uint32_t dolSize(const uint8_t* header) {
  uint32_t end = 0x100;
  for (int i = 0; i < 18; ++i) {
    const uint32_t offset = be32(header + i * 4);
    const uint32_t size = be32(header + 0x90 + i * 4);
    if (offset != 0 && size != 0 && offset + size > end) {
      end = offset + size;
    }
  }
  return end;
}

const uint8_t* fstEntry(const Partition& p, uint32_t index) { return p.fst.data() + size_t{index} * 12; }

} // namespace

extern "C" {

NodResult nod_disc_open_stream(const NodDiscStream* stream, const NodDiscOptions* options, NodHandle** out) {
  (void)options;
  if (out != nullptr) {
    *out = nullptr;
  }
  if (stream == nullptr || stream->read_at == nullptr || stream->stream_len == nullptr || out == nullptr) {
    if (stream != nullptr && stream->close != nullptr) {
      stream->close(stream->user_data);
    }
    return NOD_RESULT_ERR_OTHER;
  }
  auto* disc = new Disc;
  disc->stream = *stream;
  const int64_t length = stream->stream_len(stream->user_data);
  if (length < static_cast<int64_t>(kApploaderOffset + 0x20)) {
    disc->release();
    return NOD_RESULT_ERR_FORMAT;
  }
  disc->length = static_cast<uint64_t>(length);
  uint8_t header[0x20];
  if (!disc->readAt(0, header, sizeof(header))) {
    disc->release();
    return NOD_RESULT_ERR_IO;
  }
  if (be32(header + 0x1C) != kGcnMagic) {
    // Wii discs and compressed containers (RVZ, WIA, CISO, NKit's GCZ...) are not plain GCM.
    std::fprintf(stderr, "nod-gcm: not a plain GameCube image (magic %08x, wii %08x)\n", be32(header + 0x1C),
                 be32(header + 0x18));
    disc->release();
    return be32(header + 0x18) == kWiiMagic ? NOD_RESULT_ERR_OTHER : NOD_RESULT_ERR_FORMAT;
  }
  auto* handle = new NodHandle(Kind::Disc, disc);
  disc->release(); // the handle holds it now
  handle->size = disc->length;
  *out = handle;
  return NOD_RESULT_OK;
}

NodResult nod_disc_open_partition_kind(NodHandle* discHandle, uint32_t kind, const NodPartitionOptions* options,
                                       NodHandle** out) {
  (void)options;
  if (out != nullptr) {
    *out = nullptr;
  }
  if (discHandle == nullptr || discHandle->kind != Kind::Disc || out == nullptr) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  if (kind != NOD_PARTITION_KIND_DATA) {
    return NOD_RESULT_ERR_NOT_FOUND;
  }
  Disc& disc = *discHandle->disc;
  auto p = std::make_shared<Partition>();
  bool ok = true;
  p->boot = readBlob(disc, 0, kBootSize, ok);
  p->bi2 = readBlob(disc, kBi2Offset, kBi2Size, ok);
  if (!ok) {
    return NOD_RESULT_ERR_IO;
  }

  uint8_t apploaderHeader[0x20];
  if (!disc.readAt(kApploaderOffset, apploaderHeader, sizeof(apploaderHeader))) {
    return NOD_RESULT_ERR_IO;
  }
  const uint32_t apploaderSize = 0x20 + be32(apploaderHeader + 0x14) + be32(apploaderHeader + 0x18);
  if (apploaderSize > kMaxApploaderSize) {
    return NOD_RESULT_ERR_FORMAT;
  }
  p->apploader = readBlob(disc, kApploaderOffset, apploaderSize, ok);

  const uint32_t dolOffset = be32(p->boot.data() + 0x420);
  uint8_t dolHeader[0x100];
  if (!ok || !disc.readAt(dolOffset, dolHeader, sizeof(dolHeader))) {
    return NOD_RESULT_ERR_IO;
  }
  const uint32_t dolLength = dolSize(dolHeader);
  if (dolLength > kMaxDolSize) {
    return NOD_RESULT_ERR_FORMAT;
  }
  p->dol = readBlob(disc, dolOffset, dolLength, ok);

  const uint32_t fstOffset = be32(p->boot.data() + 0x424);
  const uint32_t fstSize = be32(p->boot.data() + 0x428);
  if (fstSize < 12 || fstSize > kMaxFstSize) {
    return NOD_RESULT_ERR_FORMAT;
  }
  p->fst = readBlob(disc, fstOffset, fstSize, ok);
  if (!ok) {
    return NOD_RESULT_ERR_IO;
  }
  // Root entry: its length field is the entry count; names follow the entries.
  p->entryCount = be32(p->fst.data() + 8);
  if (p->fst[0] != 1 || p->entryCount == 0 || size_t{p->entryCount} * 12 > p->fst.size()) {
    return NOD_RESULT_ERR_FORMAT;
  }
  p->stringTable = size_t{p->entryCount} * 12;

  auto* handle = new NodHandle(Kind::Partition, discHandle->disc);
  handle->partition = std::move(p);
  handle->size = disc.length;
  *out = handle;
  return NOD_RESULT_OK;
}

NodResult nod_partition_open_file(NodHandle* partition, uint32_t fst_index, NodHandle** out) {
  if (out != nullptr) {
    *out = nullptr;
  }
  if (partition == nullptr || partition->kind != Kind::Partition || out == nullptr) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  const Partition& p = *partition->partition;
  if (fst_index == 0 || fst_index >= p.entryCount) {
    return NOD_RESULT_ERR_NOT_FOUND;
  }
  const uint8_t* entry = fstEntry(p, fst_index);
  if (entry[0] != 0) {
    return NOD_RESULT_ERR_NOT_FOUND; // a directory
  }
  const uint64_t offset = be32(entry + 4);
  const uint64_t length = be32(entry + 8);
  if (offset + length > partition->disc->length) {
    return NOD_RESULT_ERR_FORMAT;
  }
  auto* handle = new NodHandle(Kind::File, partition->disc);
  handle->base = offset;
  handle->size = length;
  *out = handle;
  return NOD_RESULT_OK;
}

void nod_free(NodHandle* handle) { delete handle; }

int64_t nod_read(NodHandle* handle, uint8_t* buf, size_t len) {
  if (handle == nullptr || (buf == nullptr && len != 0)) {
    return -1;
  }
  if (handle->pos >= handle->size) {
    return 0;
  }
  const uint64_t remaining = handle->size - handle->pos;
  const size_t count = len < remaining ? len : static_cast<size_t>(remaining);
  if (!handle->disc->readAt(handle->base + handle->pos, buf, count)) {
    return -1;
  }
  handle->pos += count;
  return static_cast<int64_t>(count);
}

int64_t nod_seek(NodHandle* handle, int64_t offset, int32_t whence) {
  if (handle == nullptr) {
    return -1;
  }
  int64_t origin;
  switch (whence) {
  case SEEK_SET:
    origin = 0;
    break;
  case SEEK_CUR:
    origin = static_cast<int64_t>(handle->pos);
    break;
  case SEEK_END:
    origin = static_cast<int64_t>(handle->size);
    break;
  default:
    return -1;
  }
  const int64_t target = origin + offset;
  if (target < 0) {
    return -1;
  }
  handle->pos = static_cast<uint64_t>(target);
  return target;
}

NodResult nod_disc_header(const NodHandle* disc, NodDiscHeader* out) {
  if (disc == nullptr || out == nullptr) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  static_assert(sizeof(NodDiscHeader) == 0x60);
  return disc->disc->readAt(0, out, sizeof(*out)) ? NOD_RESULT_OK : NOD_RESULT_ERR_IO;
}

NodResult nod_partition_meta(const NodHandle* partition, NodPartitionMeta* out) {
  if (partition == nullptr || partition->kind != Kind::Partition || out == nullptr) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  const Partition& p = *partition->partition;
  *out = {};
  out->raw_boot = {p.boot.data(), p.boot.size()};
  out->raw_bi2 = {p.bi2.data(), p.bi2.size()};
  out->raw_apploader = {p.apploader.data(), p.apploader.size()};
  out->raw_dol = {p.dol.data(), p.dol.size()};
  out->raw_fst = {p.fst.data(), p.fst.size()};
  return NOD_RESULT_OK;
}

void nod_partition_iterate_fst(const NodHandle* partition, NodFstCallback callback, void* user_data) {
  if (partition == nullptr || partition->kind != Kind::Partition || callback == nullptr) {
    return;
  }
  const Partition& p = *partition->partition;
  for (uint32_t i = 1; i < p.entryCount;) {
    const uint8_t* entry = fstEntry(p, i);
    const bool isDir = entry[0] != 0;
    const size_t nameOffset = p.stringTable + (be32(entry) & 0x00FFFFFF);
    if (nameOffset >= p.fst.size()) {
      return;
    }
    // Names are NUL-terminated within the table; bound the copy by the table's end.
    const char* nameStart = reinterpret_cast<const char*>(p.fst.data() + nameOffset);
    const std::string name(nameStart, strnlen(nameStart, p.fst.size() - nameOffset));
    const uint32_t next =
        callback(i, isDir ? NOD_NODE_KIND_DIRECTORY : NOD_NODE_KIND_FILE, name.c_str(), be32(entry + 8), user_data);
    if (next == NOD_FST_STOP || next <= i) {
      return;
    }
    i = next;
  }
}

bool nod_partition_is_wii(const NodHandle* partition) {
  (void)partition;
  return false;
}

} // extern "C"
