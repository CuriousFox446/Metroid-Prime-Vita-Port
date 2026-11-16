#pragma once

// The retail game's world geometry kept on the GPU (Aurora's resident display lists,
// GXPortRetainResident). Every frame otherwise sends each area model's display lists
// through the GX FIFO again, and Aurora re-reads every primitive in them: about 25,000
// a frame in the frigate's hangar, most of a PS Vita frame. Retained, a surface's
// display list is made into triangle lists once and each call is a draw or two.
//
// CCubeRenderer::AddStaticGeometry keeps an area's models when the area's geometry is
// added, and RemoveStaticGeometry releases them before it goes; CModel keeps its own.
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

// The same for a loaded model (CModel: actors, the gun, characters), under the model's own
// key: kept when it is built, released when it goes or before its data moves (RemapData).
// A skinned model's arrays are kept too but go unused (its draws bind the skinning
// buffers), while its display lists, which never change, are drawn resident.
void KeepModel(const void* modelKey, const CCubeModel& model);
void ReleaseModel(const void* modelKey);

} // namespace PortRetailResident
