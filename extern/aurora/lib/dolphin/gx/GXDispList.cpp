#include "gx.hpp"
#include "__gx.h"

#include "../../gx/fifo.hpp"
#include "dolphin/gx/GXAurora.h"
#include "dolphin/gx/GXExtra.h"

#include <aurora/gfx.h>

#include <cstring>
#include <unordered_map>
#include <vector>

static __GXData_struct sSavedGXData;

namespace {
// What the game has retained (GXPortRetainResident), on the game thread: the processor holds
// the copies, by the same pointers.
struct Retained {
  u32 size;
  u32 count;
};
std::unordered_map<const void*, Retained> sRetained;

// aurora_take_gx_source_counts: by the source the game names, on the game thread.
struct SourceCounts {
  u32 begins = 0;
  u32 beginVertices = 0;
  u32 lists = 0;
  u64 listBytes = 0;
  u32 residentLists = 0;
};
SourceCounts sSourceCounts[AURORA_GX_SOURCES];
u32 sSource = 0;
} // namespace

namespace aurora::gx::source {
void count_begin(u16 vertices) noexcept {
  ++sSourceCounts[sSource].begins;
  sSourceCounts[sSource].beginVertices += vertices;
}
} // namespace aurora::gx::source

extern "C" {
void GXBeginDisplayList(void* list, u32 size) {
  CHECK(!aurora::gx::fifo::in_display_list(), "Display list began twice!");

  // Flush any pending dirty state before recording
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // Save current shadow register state if requested
  if (__gx->dlSaveContext != 0) {
    std::memcpy(&sSavedGXData, __gx, sizeof(sSavedGXData));
  }

  __gx->inDispList = 1;

  // Redirect FIFO writes to the user-provided buffer
  aurora::gx::fifo::begin_display_list(static_cast<u8*>(list), size);
}

u32 GXEndDisplayList() {
  // Flush any pending dirty state into the display list
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // End FIFO redirection and get the byte count (ROUNDUP32)
  u32 bytesWritten = aurora::gx::fifo::end_display_list();

  // Restore saved shadow register state
  if (__gx->dlSaveContext != 0) {
    std::memcpy(__gx, &sSavedGXData, sizeof(*__gx));
  }

  __gx->inDispList = 0;

  return bytesWritten;
}

void GXCallDisplayList(const void* data, u32 nbytes) {
  // Flush any pending dirty state before calling
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // Flush pending primitives
  if (*reinterpret_cast<u32*>(&__gx->vNum) != 0) {
    __GXSendFlushPrim();
  }
  aurora::gx::draw_tag::flush();

  if (!sRetained.empty() && !aurora::gx::fifo::in_display_list()) {
    const auto found = sRetained.find(data);
    if (found != sRetained.end() && found->second.size == nbytes) {
      // The processor draws its copy.
      GX_WRITE_AURORA(GX_AURORA_RESIDENT_CALL_DL);
      GX_WRITE_U64(reinterpret_cast<u64>(data));
      GX_WRITE_U32(nbytes);
      aurora::gx::fifo::publish();
      ++sSourceCounts[sSource].residentLists;
      return;
    }
  }

  // Write display list contents to the FIFO
  aurora::gx::fifo::write_data(data, nbytes);
  aurora::gx::fifo::publish();
#ifdef AURORA_VITA_GL
  // Its bytes may set arrays (a list recorded with GXSetArray or GDSetArray): the processor's
  // arrays are no longer known to be what was sent last. (A retained list's are draws alone.)
  aurora::gx::sent_arrays::forget();
#endif
  if (!aurora::gx::fifo::in_display_list()) {
    ++sSourceCounts[sSource].lists;
    sSourceCounts[sSource].listBytes += nbytes;
  }
}

u32 aurora_gx_set_source(u32 source) {
  const u32 previous = sSource;
  sSource = source < AURORA_GX_SOURCES ? source : 0;
  return previous;
}

void aurora_take_gx_source_counts(AuroraGxSourceCounts out[AURORA_GX_SOURCES]) {
  for (u32 i = 0; i < AURORA_GX_SOURCES; ++i) {
    const SourceCounts& counts = sSourceCounts[i];
    out[i] = AuroraGxSourceCounts{
        .begins = counts.begins,
        .beginVertices = counts.beginVertices,
        .lists = counts.lists,
        .listKiB = static_cast<u32>(counts.listBytes / 1024),
        .residentLists = counts.residentLists,
    };
    sSourceCounts[i] = {};
  }
}

void GXPortRetainResident(const void* data, u32 size) {
  if (data == nullptr || size == 0) {
    return;
  }
  auto [it, added] = sRetained.try_emplace(data, Retained{size, 0});
  if (!added && it->second.size != size) {
    // The same pointer with a different size is different data: the memory was reused
    // without a release. Retain the new data in its place.
    it->second = Retained{size, 0};
    added = true;
  }
  ++it->second.count;
  if (!added) {
    return;
  }
  // The processor takes the copy.
  auto* copy = new std::vector<u8>(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
  GX_WRITE_AURORA(GX_AURORA_RESIDENT_RETAIN);
  GX_WRITE_U64(reinterpret_cast<u64>(data));
  GX_WRITE_U64(reinterpret_cast<u64>(copy));
}

void GXPortReleaseResident(const void* data) {
  const auto found = sRetained.find(data);
  if (found == sRetained.end() || --found->second.count != 0) {
    return;
  }
  sRetained.erase(found);
  GX_WRITE_AURORA(GX_AURORA_RESIDENT_RELEASE);
  GX_WRITE_U64(reinterpret_cast<u64>(data));
}

}
