#pragma once

// The retail game's world geometry kept on the GPU (Aurora's resident display lists,
// GXPortRetainResident). Every frame otherwise sends each area model's display lists
// through the GX FIFO again, and Aurora re-reads every primitive in them: about 25,000
// a frame in the frigate's hangar, most of a PS Vita frame. Retained, a surface's
// display list is made into triangle lists once and each call is a draw or two.
//
// CCubeRenderer::AddStaticGeometry keeps an area's models when the area's geometry is
// added, and RemoveStaticGeometry releases them before it goes. Only world geometry:
// its arrays never change, unlike a skinned model's, which the CPU rewrites each frame.
//
// On with MP_RETAIL_RESIDENT=1 (the Vita build's default), in MP_RETAIL_RESIDENT_MB of
// device memory (48 MiB unless set), when Aurora could set that much aside.

class CCubeModel;

namespace PortRetailResident {

// From startup, once Aurora has said how much resident room it set aside.
void SetEnabled(bool enabled);
bool Enabled();

// Retains a model's vertex arrays and surface display lists under the area's key.
void KeepAreaModel(const void* areaKey, const CCubeModel& model);
// Releases everything kept under the key.
void ReleaseArea(const void* areaKey);
// Releases every area's (the renderer is going, with its area list).
void ReleaseAll();

} // namespace PortRetailResident
