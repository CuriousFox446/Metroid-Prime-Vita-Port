// Retail world geometry kept on the GPU. See port_retail_resident.h.
#include "port_retail_resident.h"

#include "Kyoto/Graphics/CCubeModel.hpp"

#include <dolphin/gx/GXExtra.h>

#include <unordered_map>
#include <vector>

namespace PortRetailResident {
namespace {
bool sEnabled = false;
// What each area retained, to release with it. Game thread only (as GX is).
std::unordered_map< const void*, std::vector< const void* > > sKept;

void Keep(std::vector< const void* >& kept, const void* data, uint size) {
  if (data != nullptr && size != 0) {
    GXPortRetainResident(data, size);
    kept.push_back(data);
  }
}
} // namespace

void SetEnabled(bool enabled) { sEnabled = enabled; }

bool Enabled() { return sEnabled; }

// As SetArraysCurrent and CCubeSurface::CallDisplayList hand them to GX, which is how
// Aurora recognises them (the same pointer and size); see PortRoomGeo's KeepResident.
void KeepAreaModel(const void* areaKey, const CCubeModel& model) {
  if (!sEnabled) {
    return;
  }
  std::vector< const void* >& kept = sKept[areaKey];
  const CCubeModel::ModelInstance& instance = model.GetModelInstance();
  Keep(kept, instance.GetVertexPointer(), instance.GetVertexSize());
  Keep(kept, instance.GetNormalPointer(), instance.GetNormalSize());
  Keep(kept, instance.GetColorPointer(), instance.GetColorSize());
  Keep(kept, instance.GetTCPointer(), instance.GetTCSize());
  Keep(kept, instance.GetPackedTCPointer(), instance.GetPackedTCSize());
  for (const CCubeSurface* first : {&model.GetNormalSurfaces(), &model.GetAlphaSurfaces()}) {
    for (CCubeSurface surface = *first; surface.IsValid(); surface = surface.GetNextSurface()) {
      Keep(kept, surface.GetDisplayList(), surface.GetDisplayListSize());
    }
  }
}

void ReleaseArea(const void* areaKey) {
  const auto found = sKept.find(areaKey);
  if (found == sKept.end()) {
    return;
  }
  for (const void* data : found->second) {
    GXPortReleaseResident(data);
  }
  sKept.erase(found);
}

// Models and areas share the table: their keys (a CModel, an area's instance vector) differ.
void KeepModel(const void* modelKey, const CCubeModel& model) { KeepAreaModel(modelKey, model); }

void ReleaseModel(const void* modelKey) { ReleaseArea(modelKey); }

void ReleaseAll() {
  for (const auto& [areaKey, kept] : sKept) {
    for (const void* data : kept) {
      GXPortReleaseResident(data);
    }
  }
  sKept.clear();
}

} // namespace PortRetailResident
