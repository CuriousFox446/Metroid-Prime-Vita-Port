#include "MetroidPrime/CParticleDatabase.hpp"

#include "Kyoto/Animation/CCharLayoutInfo.hpp"
#include "Kyoto/Animation/CPoseAsTransforms.hpp"
#include "Kyoto/Math/CFrustumPlanes.hpp"
#include "Kyoto/Particles/CElementGen.hpp"
#include "Kyoto/Particles/CParticleElectric.hpp"
#include "Kyoto/Particles/CParticleSwoosh.hpp"
#include "MetroidPrime/CParticleGenInfoGeneric.hpp"

#ifdef TARGET_PC
#include "port_profile.h"
#include <dolphin/gx/GXExtra.h>
#include <chrono>

// Every method but the description caches finishes a pending update first (see Update).
#define PORT_DB_FINISH() PortFinishConst()

namespace {
// The window CStateManager::Update opens around its entities' Think (PortSetAsyncWindow).
bool sPortAsyncWindow = false;
// Set when the side worker refused a job (no fourth core): every update runs whole, as before.
bool sPortNoWorker = false;
// The updates with their third part still to run, in the order they began.
CParticleDatabase* sPortPendingHead = nullptr;
CParticleDatabase* sPortPendingTail = nullptr;
uint sPortStatAsync = 0;
uint sPortStatWaited = 0;
uint sPortStatWaitUs = 0;
} // namespace
#else
#define PORT_DB_FINISH() (void)0
#endif

CParticleDatabase::CParticleDatabase()
: xb4_24_updatesEnabled(true), xb4_25_anySystemsDrawnWithModel(false) {}

CParticleDatabase::~CParticleDatabase() {
#ifdef TARGET_PC
  // A step still running reads these systems: wait for it. What its update had left to do (lights
  // fed, finished systems taken out) goes with the systems.
  if (xPortJob != 0) {
    GXPortSideJobWait(xPortJob);
  }
  if (xPortPending) {
    PortUnlinkPending();
  }
#endif
}

void CParticleDatabase::CacheParticleDesc(const CCharacterInfo::CParticleResData& data) {
  for (rstl::vector< CAssetId >::const_iterator it = data.x0_part.begin(), end = data.x0_part.end();
       it != end; ++it) {
    const CAssetId id = *it;
    AUTO(cached, x0_particleDescs.find(id));
    if (cached == x0_particleDescs.end()) {
      rstl::rc_ptr< TLockedToken< CGenDescription > > desc(
          rs_new TLockedToken< CGenDescription >(gpSimplePool->GetObj(SObjectTag('PART', id))));
      x0_particleDescs.insert(
          rstl::pair< CAssetId, rstl::rc_ptr< TLockedToken< CGenDescription > > >(id, desc));
    }
  }
  for (rstl::vector< CAssetId >::const_iterator it = data.x10_swhc.begin(),
                                                end = data.x10_swhc.end();
       it != end; ++it) {
    const CAssetId id = *it;
    AUTO(cached, x14_swooshDescs.find(id));
    if (cached == x14_swooshDescs.end()) {
      rstl::rc_ptr< TLockedToken< CSwooshDescription > > desc(
          rs_new TLockedToken< CSwooshDescription >(gpSimplePool->GetObj(SObjectTag('SWHC', id))));
      x14_swooshDescs.insert(
          rstl::pair< CAssetId, rstl::rc_ptr< TLockedToken< CSwooshDescription > > >(id, desc));
    }
  }
  for (rstl::vector< CAssetId >::const_iterator it = data.x20_elsc.begin(),
                                                end = data.x20_elsc.end();
       it != end; ++it) {
    const CAssetId id = *it;
    AUTO(cached, x28_electricDescs.find(id));
    if (cached == x28_electricDescs.end()) {
      rstl::rc_ptr< TLockedToken< CElectricDescription > > desc(
          rs_new TLockedToken< CElectricDescription >(
              gpSimplePool->GetObj(SObjectTag('ELSC', id))));
      x28_electricDescs.insert(
          rstl::pair< CAssetId, rstl::rc_ptr< TLockedToken< CElectricDescription > > >(id, desc));
    }
  }
}

void CParticleDatabase::CacheParticleDesc(const SObjectTag& tag) {
  const CAssetId id = tag.GetId();
  switch (tag.GetType()) {
  case 'PART': {
    AUTO(cached, x0_particleDescs.find(id));
    if (cached == x0_particleDescs.end()) {
      rstl::rc_ptr< TLockedToken< CGenDescription > > desc(
          rs_new TLockedToken< CGenDescription >(gpSimplePool->GetObj(SObjectTag('PART', id))));
      x0_particleDescs.insert(
          rstl::pair< CAssetId, rstl::rc_ptr< TLockedToken< CGenDescription > > >(id, desc));
    }
    break;
  }
  case 'SWHC': {
    AUTO(cached, x14_swooshDescs.find(id));
    if (cached == x14_swooshDescs.end()) {
      rstl::rc_ptr< TLockedToken< CSwooshDescription > > desc(
          rs_new TLockedToken< CSwooshDescription >(gpSimplePool->GetObj(SObjectTag('SWHC', id))));
      x14_swooshDescs.insert(
          rstl::pair< CAssetId, rstl::rc_ptr< TLockedToken< CSwooshDescription > > >(id, desc));
    }
    break;
  }
  case 'ELSC': {
    AUTO(cached, x28_electricDescs.find(id));
    if (cached == x28_electricDescs.end()) {
      rstl::rc_ptr< TLockedToken< CElectricDescription > > desc(
          rs_new TLockedToken< CElectricDescription >(
              gpSimplePool->GetObj(SObjectTag('ELSC', id))));
      x28_electricDescs.insert(
          rstl::pair< CAssetId, rstl::rc_ptr< TLockedToken< CElectricDescription > > >(id, desc));
    }
    break;
  }
  }
}

void CParticleDatabase::InsertParticleGen(const bool oneShot, int flags, const rstl::string& name,
                                          const rstl::auto_ptr< CParticleGenInfo >& gen) {
  DrawMap* map;
  if (oneShot) {
    switch (flags & 0x60) {
    case 0x20:
      map = &x8c_firstDraw;
      break;
    case 0x40:
      map = &xa0_lastDraw;
      break;
    default:
      map = &x78_rendererDraw;
      break;
    }
  } else {
    switch (flags & 0x60) {
    case 0x20:
      map = &x50_firstDrawLoop;
      break;
    case 0x40:
      map = &x64_lastDrawLoop;
      break;
    default:
      map = &x3c_rendererDrawLoop;
      break;
    }
  }
  map->insert(DrawMap::value_type(name, gen));
  if ((flags & 0x60) != 0) {
    xb4_25_anySystemsDrawnWithModel = true;
  }
}

template < class T, class U >
static int _getGraphicLightId(const T& system, const U& desc) {
  if (system->SystemHasLight()) {
    return desc.GetTag().GetId();
  }
  return -1;
}

void CParticleDatabase::AddParticleEffect(const rstl::string& name, int flags,
                                          const CParticleData& data, const CVector3f& scale,
                                          CStateManager& mgr, TAreaId areaId, bool oneShot,
                                          int particleLightIdx) {
  PORT_DB_FINISH();
  const SObjectTag tag = data.GetParticleAssetInfo();
  const int duration = data.GetDuration();
  const rstl::string& locator = data.GetLocatorName();
  const float scaleFactor = data.GetScale();
  const CParticleData::EParentedMode mode = data.GetParentedMode();
  CParticleGenInfo* effect = GetParticleEffect(name);
  if (effect == nullptr) {
    const FourCC type = tag.GetType();
    CVector3f particleScale(1.f, 1.f, 1.f);
    if (flags & 2) {
      particleScale *= scaleFactor;
    } else {
      particleScale = CVector3f(scaleFactor * scale.GetX(), scaleFactor * scale.GetY(),
                                scaleFactor * scale.GetZ());
    }
    rstl::auto_ptr< CParticleGenInfo > gen;
    if (type == 'PART') {
      AUTO(it, x0_particleDescs.find(tag.GetId()));
      if (it != x0_particleDescs.end()) {
        rstl::ncrc_ptr< CParticleGen > system = rs_new CElementGen(*it->second);
        const int lightId =
            particleLightIdx +
            _getGraphicLightId< rstl::ncrc_ptr< CParticleGen >, TToken< CGenDescription > >(
                system, *it->second);
        gen = rs_new CParticleGenInfoGeneric(tag, system, duration, locator, particleScale, mode,
                                             flags, mgr, areaId, lightId, kPGT_Normal);
      }
    } else if (type == 'SWHC') {
      AUTO(it, x14_swooshDescs.find(tag.GetId()));
      if (it != x14_swooshDescs.end()) {
        rstl::ncrc_ptr< CParticleGen > system = rs_new CParticleSwoosh(*it->second, 0);
        gen = rs_new CParticleGenInfoGeneric(tag, system, duration, locator, particleScale, mode,
                                             flags, mgr, areaId, -1, kPGT_Normal);
      }
    } else if (type == 'ELSC') {
      AUTO(it, x28_electricDescs.find(tag.GetId()));
      if (it != x28_electricDescs.end()) {
        rstl::ncrc_ptr< CParticleGen > system = rs_new CParticleElectric(*it->second);
        const int lightId =
            particleLightIdx +
            _getGraphicLightId< rstl::ncrc_ptr< CParticleGen >, TToken< CElectricDescription > >(
                system, *it->second);
        gen = rs_new CParticleGenInfoGeneric(tag, system, duration, locator, particleScale, mode,
                                             flags, mgr, areaId, lightId, kPGT_Normal);
      }
    }
    if (!gen.null()) {
      gen->SetIsActive(true);
      gen->SetParticleEmission(true, mgr);
      gen->SetIsGrabInitialData(true);
      InsertParticleGen(oneShot, flags, name, gen);
    }
  } else if (!effect->GetIsActive()) {
    effect->SetParticleEmission(true, mgr);
    effect->SetIsActive(true);
    effect->SetIsGrabInitialData(true);
    effect->SetFlags(flags);
  }
}

void CParticleDatabase::AddAuxiliaryParticleEffect(const rstl::string& name, int flags,
                                                   const CAuxiliaryParticleData& data,
                                                   const CVector3f& scale, CStateManager& mgr,
                                                   TAreaId areaId, int particleLightIdx) {
  PORT_DB_FINISH();
  const SObjectTag tag = data.GetParticleAssetInfo();
  const int duration = data.GetDuration();
  const float scaleFactor = data.GetScale();
  CParticleGenInfo* effect = GetParticleEffect(name);
  if (effect == nullptr) {
    const FourCC type = tag.GetType();
    CVector3f particleScale(1.f, 1.f, 1.f);
    if (flags & 2) {
      particleScale *= scaleFactor;
    } else {
      particleScale = CVector3f(scaleFactor * scale.GetX(), scaleFactor * scale.GetY(),
                                scaleFactor * scale.GetZ());
    }
    rstl::auto_ptr< CParticleGenInfo > gen;
    if (type == 'PART') {
      AUTO(it, x0_particleDescs.find(tag.GetId()));
      if (it != x0_particleDescs.end()) {
        rstl::ncrc_ptr< CParticleGen > system = rs_new CElementGen(*it->second);
        const int lightId =
            particleLightIdx +
            _getGraphicLightId< rstl::ncrc_ptr< CParticleGen >, TToken< CGenDescription > >(
                system, *it->second);
        gen = rs_new CParticleGenInfoGeneric(tag, system, duration, "NOT_A_VALID_LOCATOR",
                                             particleScale, CParticleData::kPM_Initial, flags, mgr,
                                             areaId, lightId, kPGT_Auxiliary);
        gen->SetGlobalTranslation(data.GetTranslation(), mgr);
        gen->SetIsGrabInitialData(false);
        InsertParticleGen(false, flags, name, gen);
      } else {
        SObjectTag::Type2Text(type);
      }
    }
  } else if (!effect->GetIsActive()) {
    effect->SetParticleEmission(true, mgr);
    effect->SetIsActive(true);
    effect->SetIsGrabInitialData(true);
    effect->SetFlags(flags);
  }
}

CParticleGenInfo* CParticleDatabase::GetParticleEffect(const rstl::string& name) {
  PORT_DB_FINISH();
  {
    AUTO(it, x3c_rendererDrawLoop.find(name));
    if (it != x3c_rendererDrawLoop.end()) {
      return it->second.get();
    }
  }
  {
    DrawMap::const_iterator it = x50_firstDrawLoop.find(name);
    if (it != x50_firstDrawLoop.end()) {
      return it->second.get();
    }
  }
  {
    DrawMap::const_iterator it = x64_lastDrawLoop.find(name);
    if (it != x64_lastDrawLoop.end()) {
      return it->second.get();
    }
  }
  {
    DrawMap::const_iterator it = x78_rendererDraw.find(name);
    if (it != x78_rendererDraw.end()) {
      return it->second.get();
    }
  }
  {
    DrawMap::const_iterator it = x8c_firstDraw.find(name);
    if (it != x8c_firstDraw.end()) {
      return it->second.get();
    }
  }
  {
    DrawMap::const_iterator it = xa0_lastDraw.find(name);
    if (it != xa0_lastDraw.end()) {
      return it->second.get();
    }
  }
  return nullptr;
}

void CParticleDatabase::SetParticleEffectState(const rstl::string& name, bool active,
                                               CStateManager& mgr) {
  PORT_DB_FINISH();
  CParticleGenInfo* effect = GetParticleEffect(name);
  if (effect != nullptr) {
    effect->SetParticleEmission(active, mgr);
    effect->SetIsActive(active);
    if (!active && (effect->GetFlags() & 1)) {
      effect->DestroyParticles();
    }
    effect->SetIsGrabInitialData(true);
  }
}

void CParticleDatabase::SetExternalVarValue(const rstl::string& name, int index, float value) {
  PORT_DB_FINISH();
  CParticleGenInfo* effect = GetParticleEffect(name);
  if (effect != nullptr) {
    CElementGen* system = static_cast< CElementGen* >(
        static_cast< CParticleGenInfoGeneric* >(effect)->GetParticleSystem().GetPtr());
    system->SetExternalVar(index, value);
  }
}

void CParticleDatabase::Update(float dt, const CPoseAsTransforms& pose,
                               const CCharLayoutInfo& layout, const CTransform4f& xf,
                               const CVector3f& scale, CStateManager& mgr) {
  if (!xb4_24_updatesEnabled) {
    return;
  }
#ifdef TARGET_PC
  // A previous update's third part first (one update a tick is the rule, but not every actor's).
  PortFinish();
  if (sPortAsyncWindow && !sPortNoWorker) {
    // The first part, on this thread, in the maps' order (UpdateParticleGenDB's).
    bool any = PortPrepareParticleGenDB(pose, layout, xf, scale, mgr, x3c_rendererDrawLoop);
    any = PortPrepareParticleGenDB(pose, layout, xf, scale, mgr, x50_firstDrawLoop) || any;
    any = PortPrepareParticleGenDB(pose, layout, xf, scale, mgr, x64_lastDrawLoop) || any;
    any = PortPrepareParticleGenDB(pose, layout, xf, scale, mgr, x78_rendererDraw) || any;
    any = PortPrepareParticleGenDB(pose, layout, xf, scale, mgr, x8c_firstDraw) || any;
    any = PortPrepareParticleGenDB(pose, layout, xf, scale, mgr, xa0_lastDraw) || any;
    xPortDt = dt;
    xPortMgr = &mgr;
    xPortPending = true;
    PortLinkPending();
    if (any) {
      // The second, on the side worker; without one, here (and every update whole from now on).
      xPortJob = GXPortSideJobSubmitAny(&CParticleDatabase::PortStepJob, this);
      if (xPortJob != 0) {
        ++sPortStatAsync;
      } else {
        sPortNoWorker = true;
        PortStepAll();
      }
    }
    if (xPortJob == 0) {
      PortFinish();
    }
    return;
  }
#endif
  UpdateParticleGenDB(dt, pose, layout, xf, scale, mgr, x3c_rendererDrawLoop, true);
  UpdateParticleGenDB(dt, pose, layout, xf, scale, mgr, x50_firstDrawLoop, true);
  UpdateParticleGenDB(dt, pose, layout, xf, scale, mgr, x64_lastDrawLoop, true);
  UpdateParticleGenDB(dt, pose, layout, xf, scale, mgr, x78_rendererDraw, false);
  UpdateParticleGenDB(dt, pose, layout, xf, scale, mgr, x8c_firstDraw, false);
  UpdateParticleGenDB(dt, pose, layout, xf, scale, mgr, xa0_lastDraw, false);
  xb4_25_anySystemsDrawnWithModel = x50_firstDrawLoop.size() || x64_lastDrawLoop.size() ||
                                    x8c_firstDraw.size() || xa0_lastDraw.size();
}

#ifdef TARGET_PC
// UpdateParticleGenDB's loop body before the system's Update: its transforms from the pose and its
// lifetime. False when the update passes the system over (an active one whose locator the pose has
// no data for), which then neither steps nor ages.
bool CParticleDatabase::PortPrepareParticleGen(CParticleGenInfo& info, const CPoseAsTransforms& pose,
                                               const CCharLayoutInfo& layout, const CTransform4f& xf,
                                               const CVector3f& scale, CStateManager& mgr) {
  if (info.GetIsActive()) {
    switch (info.GetType()) {
    case kPGT_Normal: {
      const CParticleData::EParentedMode mode = info.GetParentedMode();
      const CSegId seg = layout.GetSegIdFromString(info.GetLocatorName());
      if (seg == CSegId::Invalid()) {
        return false;
      }
      if (!pose.ContainsDataFor(seg)) {
        return false;
      }
      const CVector3f offset = pose.GetOffset(seg);
      switch (mode) {
      case CParticleData::kPM_Initial:
        if (info.GetIsGrabInitialData()) {
          const uint initialFlags = info.GetFlags();
          CTransform4f segXf((initialFlags & 0x10) ? CMatrix3f::Identity()
                                                   : pose.GetTransformMinusOffset(seg),
                             CVector3f(offset.GetX() * scale.GetX(), offset.GetY() * scale.GetY(),
                                       offset.GetZ() * scale.GetZ()));
          CTransform4f composed(xf * segXf);
          info.SetCurTransform(composed.GetRotation());
          info.SetCurOffset(composed.GetTranslation());
          info.ResetTime();
          info.SetIsGrabInitialData(false);
        }
        info.SetOrientation(info.GetCurTransform(), mgr);
        info.SetTranslation(info.GetCurOffset(), mgr);
        if (info.GetFlags() & 0x2000) {
          const CVector3f& particleScale = info.GetScale();
          info.SetGlobalScale(CVector3f(particleScale.GetX() * scale.GetX(),
                                        particleScale.GetY() * scale.GetY(),
                                        particleScale.GetZ() * scale.GetZ()));
        } else {
          info.SetGlobalScale(info.GetScale());
        }
        break;
      case CParticleData::kPM_ContinuousEmitter:
      case CParticleData::kPM_ContinuousSystem: {
        if (info.GetIsGrabInitialData()) {
          info.ResetTime();
          info.SetIsGrabInitialData(false);
        }
        CTransform4f segXf(pose.GetTransformMinusOffset(seg),
                           CVector3f(offset.GetX() * scale.GetX(), offset.GetY() * scale.GetY(),
                                     offset.GetZ() * scale.GetZ()));
        CTransform4f composed(xf * segXf);
        if (mode == CParticleData::kPM_ContinuousEmitter) {
          info.SetTranslation(composed.GetTranslation(), mgr);
          if (info.GetFlags() & 0x10) {
            info.SetOrientation(xf.GetRotation(), mgr);
          } else {
            info.SetOrientation(composed.GetRotation(), mgr);
          }
        } else {
          info.SetGlobalTranslation(composed.GetTranslation(), mgr);
          if (info.GetFlags() & 0x10) {
            info.SetGlobalOrientation(xf.GetRotation(), mgr);
          } else {
            info.SetGlobalOrientation(composed.GetRotation(), mgr);
          }
        }
        if (info.GetFlags() & 0x2000) {
          const CVector3f& particleScale = info.GetScale();
          info.SetGlobalScale(CVector3f(particleScale.GetX() * scale.GetX(),
                                        particleScale.GetY() * scale.GetY(),
                                        particleScale.GetZ() * scale.GetZ()));
        } else {
          info.SetGlobalScale(info.GetScale());
        }
        break;
      }
      }
      break;
    }
    case kPGT_Auxiliary:
      break;
    }
    const float duration =
        info.GetInactiveStartTime() == 0.f ? 10000000.f : info.GetInactiveStartTime();
    if (info.GetCurrentTime() > duration) {
      info.SetIsActive(false);
      info.SetParticleEmission(false, mgr);
      info.MarkFinishTime();
      if (info.GetFlags() & 1) {
        info.DestroyParticles();
      }
    }
  }
  return true;
}

// UpdateParticleGenDB's loop body after the system's Update: true when the system is done (it
// comes out of the map), else its time moves on.
bool CParticleDatabase::PortFinishParticleGen(CParticleGenInfo& info, float dt, CStateManager& mgr,
                                              bool deleteIfDone) {
  if (!info.GetIsActive()) {
    if (!info.HasActiveParticles() && info.GetCurrentTime() - info.GetFinishTime() > 5.f &&
        deleteIfDone) {
      info.DeleteLight(mgr);
      return true;
    }
  } else if (info.IsSystemDeletable()) {
    info.DeleteLight(mgr);
    return true;
  }
  info.OffsetTime(dt);
  return false;
}

void CParticleDatabase::UpdateParticleGenDB(float dt, const CPoseAsTransforms& pose,
                                            const CCharLayoutInfo& layout, const CTransform4f& xf,
                                            const CVector3f& scale, CStateManager& mgr,
                                            DrawMap& map, bool deleteIfDone) {
  AUTO(it, map.begin());
  while (it != map.end()) {
    CParticleGenInfo& info = *it->second;
    if (!PortPrepareParticleGen(info, pose, layout, xf, scale, mgr)) {
      ++it;
      continue;
    }
    info.Update(dt, mgr);
    if (PortFinishParticleGen(info, dt, mgr, deleteIfDone)) {
      it = map.erase(it);
      continue;
    }
    ++it;
  }
}

bool CParticleDatabase::PortPrepareParticleGenDB(const CPoseAsTransforms& pose,
                                                 const CCharLayoutInfo& layout, const CTransform4f& xf,
                                                 const CVector3f& scale, CStateManager& mgr, DrawMap& map) {
  bool any = false;
  for (AUTO(it, map.begin()); it != map.end(); ++it) {
    CParticleGenInfo& info = *it->second;
    info.xPortStepPending = PortPrepareParticleGen(info, pose, layout, xf, scale, mgr);
    any = any || info.xPortStepPending;
  }
  return any;
}

// The side worker's part: nothing but the systems' own steps (CParticleGenInfo::PortUpdateSystem).
void CParticleDatabase::PortStepParticleGenDB(float dt, DrawMap& map) {
  for (AUTO(it, map.begin()); it != map.end(); ++it) {
    CParticleGenInfo& info = *it->second;
    if (info.xPortStepPending) {
      info.PortUpdateSystem(dt);
    }
  }
}

void CParticleDatabase::PortFinishParticleGenDB(float dt, CStateManager& mgr, DrawMap& map,
                                                bool deleteIfDone) {
  AUTO(it, map.begin());
  while (it != map.end()) {
    CParticleGenInfo& info = *it->second;
    if (!info.xPortStepPending) {
      ++it;
      continue;
    }
    info.xPortStepPending = false;
    info.PortUpdateRest(dt, mgr);
    if (PortFinishParticleGen(info, dt, mgr, deleteIfDone)) {
      it = map.erase(it);
      continue;
    }
    ++it;
  }
}

void CParticleDatabase::PortStepJob(void* database) {
  PORT_ZONE_TIMED("CParticleDatabase: systems stepped (side worker, core 3)");
  static_cast< CParticleDatabase* >(database)->PortStepAll();
}

void CParticleDatabase::PortStepAll() {
  PortStepParticleGenDB(xPortDt, x3c_rendererDrawLoop);
  PortStepParticleGenDB(xPortDt, x50_firstDrawLoop);
  PortStepParticleGenDB(xPortDt, x64_lastDrawLoop);
  PortStepParticleGenDB(xPortDt, x78_rendererDraw);
  PortStepParticleGenDB(xPortDt, x8c_firstDraw);
  PortStepParticleGenDB(xPortDt, xa0_lastDraw);
}

void CParticleDatabase::PortFinish() {
  if (!xPortPending) {
    return;
  }
  xPortPending = false;
  PortUnlinkPending();
  if (xPortJob != 0) {
    if (!GXPortSideJobDone(xPortJob)) {
      const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
      GXPortSideJobWait(xPortJob);
      ++sPortStatWaited;
      sPortStatWaitUs += static_cast< uint >(std::chrono::duration_cast< std::chrono::microseconds >(
                                                 std::chrono::steady_clock::now() - start)
                                                 .count());
    }
    xPortJob = 0;
  }
  CStateManager& mgr = *xPortMgr;
  const float dt = xPortDt;
  PortFinishParticleGenDB(dt, mgr, x3c_rendererDrawLoop, true);
  PortFinishParticleGenDB(dt, mgr, x50_firstDrawLoop, true);
  PortFinishParticleGenDB(dt, mgr, x64_lastDrawLoop, true);
  PortFinishParticleGenDB(dt, mgr, x78_rendererDraw, false);
  PortFinishParticleGenDB(dt, mgr, x8c_firstDraw, false);
  PortFinishParticleGenDB(dt, mgr, xa0_lastDraw, false);
  xb4_25_anySystemsDrawnWithModel = x50_firstDrawLoop.size() || x64_lastDrawLoop.size() ||
                                    x8c_firstDraw.size() || xa0_lastDraw.size();
}

void CParticleDatabase::PortLinkPending() {
  xPortNextPending = nullptr;
  (sPortPendingTail != nullptr ? sPortPendingTail->xPortNextPending : sPortPendingHead) = this;
  sPortPendingTail = this;
}

void CParticleDatabase::PortUnlinkPending() {
  CParticleDatabase* prev = nullptr;
  for (CParticleDatabase* db = sPortPendingHead; db != nullptr; prev = db, db = db->xPortNextPending) {
    if (db == this) {
      (prev != nullptr ? prev->xPortNextPending : sPortPendingHead) = xPortNextPending;
      if (sPortPendingTail == this) {
        sPortPendingTail = prev;
      }
      xPortNextPending = nullptr;
      return;
    }
  }
}

void CParticleDatabase::PortSetAsyncWindow(bool open) { sPortAsyncWindow = open; }

void CParticleDatabase::PortFinishAll() {
  while (sPortPendingHead != nullptr) {
    sPortPendingHead->PortFinish();
  }
}

void CParticleDatabase::PortTakeAsyncStats(uint& async, uint& waited, uint& waitUs) {
  async = sPortStatAsync;
  waited = sPortStatWaited;
  waitUs = sPortStatWaitUs;
  sPortStatAsync = sPortStatWaited = sPortStatWaitUs = 0;
}
#else
void CParticleDatabase::UpdateParticleGenDB(float dt, const CPoseAsTransforms& pose,
                                            const CCharLayoutInfo& layout, const CTransform4f& xf,
                                            const CVector3f& scale, CStateManager& mgr,
                                            DrawMap& map, bool deleteIfDone) {
  AUTO(it, map.begin());
  while (it != map.end()) {
    CParticleGenInfo& info = *it->second;
    if (info.GetIsActive()) {
      switch (info.GetType()) {
      case kPGT_Normal: {
        const CParticleData::EParentedMode mode = info.GetParentedMode();
        const CSegId seg = layout.GetSegIdFromString(info.GetLocatorName());
        if (seg == CSegId::Invalid()) {
          ++it;
          continue;
        }
        if (!pose.ContainsDataFor(seg)) {
          ++it;
          continue;
        }
        const CVector3f offset = pose.GetOffset(seg);
        switch (mode) {
        case CParticleData::kPM_Initial:
          if (info.GetIsGrabInitialData()) {
            const uint initialFlags = info.GetFlags();
            CTransform4f segXf((initialFlags & 0x10) ? CMatrix3f::Identity()
                                                     : pose.GetTransformMinusOffset(seg),
                               CVector3f(offset.GetX() * scale.GetX(), offset.GetY() * scale.GetY(),
                                         offset.GetZ() * scale.GetZ()));
            CTransform4f composed(xf * segXf);
            info.SetCurTransform(composed.GetRotation());
            info.SetCurOffset(composed.GetTranslation());
            info.ResetTime();
            info.SetIsGrabInitialData(false);
          }
          info.SetOrientation(info.GetCurTransform(), mgr);
          info.SetTranslation(info.GetCurOffset(), mgr);
          if (info.GetFlags() & 0x2000) {
            const CVector3f& particleScale = info.GetScale();
            info.SetGlobalScale(CVector3f(particleScale.GetX() * scale.GetX(),
                                          particleScale.GetY() * scale.GetY(),
                                          particleScale.GetZ() * scale.GetZ()));
          } else {
            info.SetGlobalScale(info.GetScale());
          }
          break;
        case CParticleData::kPM_ContinuousEmitter:
        case CParticleData::kPM_ContinuousSystem: {
          if (info.GetIsGrabInitialData()) {
            info.ResetTime();
            info.SetIsGrabInitialData(false);
          }
          CTransform4f segXf(pose.GetTransformMinusOffset(seg),
                             CVector3f(offset.GetX() * scale.GetX(), offset.GetY() * scale.GetY(),
                                       offset.GetZ() * scale.GetZ()));
          CTransform4f composed(xf * segXf);
          if (mode == CParticleData::kPM_ContinuousEmitter) {
            info.SetTranslation(composed.GetTranslation(), mgr);
            if (info.GetFlags() & 0x10) {
              info.SetOrientation(xf.GetRotation(), mgr);
            } else {
              info.SetOrientation(composed.GetRotation(), mgr);
            }
          } else {
            info.SetGlobalTranslation(composed.GetTranslation(), mgr);
            if (info.GetFlags() & 0x10) {
              info.SetGlobalOrientation(xf.GetRotation(), mgr);
            } else {
              info.SetGlobalOrientation(composed.GetRotation(), mgr);
            }
          }
          if (info.GetFlags() & 0x2000) {
            const CVector3f& particleScale = info.GetScale();
            info.SetGlobalScale(CVector3f(particleScale.GetX() * scale.GetX(),
                                          particleScale.GetY() * scale.GetY(),
                                          particleScale.GetZ() * scale.GetZ()));
          } else {
            info.SetGlobalScale(info.GetScale());
          }
          break;
        }
        }
        break;
      }
      case kPGT_Auxiliary:
        break;
      }
      const float duration =
          info.GetInactiveStartTime() == 0.f ? 10000000.f : info.GetInactiveStartTime();
      if (info.GetCurrentTime() > duration) {
        info.SetIsActive(false);
        info.SetParticleEmission(false, mgr);
        info.MarkFinishTime();
        if (info.GetFlags() & 1) {
          info.DestroyParticles();
        }
      }
    }
    info.Update(dt, mgr);
    if (!info.GetIsActive()) {
      if (!info.HasActiveParticles() && info.GetCurrentTime() - info.GetFinishTime() > 5.f &&
          deleteIfDone) {
        info.DeleteLight(mgr);
        it = map.erase(it);
        continue;
      }
    } else if (info.IsSystemDeletable()) {
      info.DeleteLight(mgr);
      it = map.erase(it);
      continue;
    }
    info.OffsetTime(dt);
    ++it;
  }
}
#endif

void CParticleDatabase::AddToRendererClipped(const CFrustumPlanes& frustum) const {
  PORT_DB_FINISH();
  AddToRendererClippedParticleGenMap(x78_rendererDraw, frustum);
  AddToRendererClippedParticleGenMap(x3c_rendererDrawLoop, frustum);
}

void CParticleDatabase::AddToRendererClippedMasked(const CFrustumPlanes& frustum, int mask,
                                                   int target) const {
  PORT_DB_FINISH();
  AddToRendererClippedParticleGenMapMasked(x78_rendererDraw, frustum, mask, target);
  AddToRendererClippedParticleGenMapMasked(x3c_rendererDrawLoop, frustum, mask, target);
}

void CParticleDatabase::AddToRendererClippedParticleGenMap(const DrawMap& map,
                                                           const CFrustumPlanes& frustum) const {
  for (AUTO(it, map.begin()); it != map.end(); ++it) {
    CParticleGenInfo* const gen = it->second.get();
    if ((frustum.BoxInFrustumPlanes(gen->GetBounds()) == true)) {
      gen->AddToRenderer();
    }
  }
}

void CParticleDatabase::AddToRendererClippedParticleGenMapMasked(const DrawMap& map,
                                                                 const CFrustumPlanes& frustum,
                                                                 int mask, int target) const {
  for (AUTO(it, map.begin()); it != map.end(); ++it) {
    CParticleGenInfo* const gen = it->second.get();
    if ((gen->GetFlags() & mask) == target &&
        (frustum.BoxInFrustumPlanes(gen->GetBounds()) == true)) {
      gen->AddToRenderer();
    }
  }
}

void CParticleDatabase::RenderSystemsToBeDrawnFirst() const {
  PORT_DB_FINISH();
  RenderParticleGenMap(x8c_firstDraw);
  RenderParticleGenMap(x50_firstDrawLoop);
}

void CParticleDatabase::RenderSystemsToBeDrawnFirstMasked(int mask, int target) const {
  PORT_DB_FINISH();
  RenderParticleGenMapMasked(x8c_firstDraw, mask, target);
  RenderParticleGenMapMasked(x50_firstDrawLoop, mask, target);
}

void CParticleDatabase::RenderSystemsToBeDrawnLast() const {
  PORT_DB_FINISH();
  RenderParticleGenMap(xa0_lastDraw);
  RenderParticleGenMap(x64_lastDrawLoop);
}

void CParticleDatabase::RenderSystemsToBeDrawnLastMasked(int mask, int target) const {
  PORT_DB_FINISH();
  RenderParticleGenMapMasked(xa0_lastDraw, mask, target);
  RenderParticleGenMapMasked(x64_lastDrawLoop, mask, target);
}

void CParticleDatabase::RenderParticleGenMap(const DrawMap& map) {
  for (AUTO(it, map.begin()); it != map.end(); ++it) {
    it->second->Render();
  }
}

void CParticleDatabase::RenderParticleGenMapMasked(const DrawMap& map, int mask, int target) {
  for (AUTO(it, map.begin()); it != map.end(); ++it) {
    if ((it->second->GetFlags() & mask) == target) {
      it->second->Render();
    }
  }
}

void CParticleDatabase::DeleteAllLights(CStateManager& mgr) {
  PORT_DB_FINISH();
  DeleteAllLightsForParticleDB(mgr, x3c_rendererDrawLoop);
  DeleteAllLightsForParticleDB(mgr, x50_firstDrawLoop);
  DeleteAllLightsForParticleDB(mgr, x64_lastDrawLoop);
  DeleteAllLightsForParticleDB(mgr, x78_rendererDraw);
  DeleteAllLightsForParticleDB(mgr, x8c_firstDraw);
  DeleteAllLightsForParticleDB(mgr, xa0_lastDraw);
}

void CParticleDatabase::DeleteAllLightsForParticleDB(CStateManager& mgr, DrawMap& map) {
  for (AUTO(it, map.begin()); it != map.end(); ++it) {
    it->second->DeleteLight(mgr);
  }
}

void CParticleDatabase::SuspendAllActiveEffects(CStateManager& mgr) {
  PORT_DB_FINISH();
  SuspendAllActiveEffectsForParticleDB(mgr, x3c_rendererDrawLoop);
  SuspendAllActiveEffectsForParticleDB(mgr, x50_firstDrawLoop);
  SuspendAllActiveEffectsForParticleDB(mgr, x64_lastDrawLoop);
}

void CParticleDatabase::SuspendAllActiveEffectsForParticleDB(CStateManager& mgr, DrawMap& map) {
  for (AUTO(it, map.begin()); it != map.end(); ++it) {
    it->second->SetParticleEmission(false, mgr);
  }
}

void CParticleDatabase::SetModulationColorAllActiveEffects(const CColor& color) {
  PORT_DB_FINISH();
  SetModulationColorAllActiveEffectsForParticleDB(color, x3c_rendererDrawLoop);
  SetModulationColorAllActiveEffectsForParticleDB(color, x50_firstDrawLoop);
  SetModulationColorAllActiveEffectsForParticleDB(color, x64_lastDrawLoop);
  SetModulationColorAllActiveEffectsForParticleDB(color, x78_rendererDraw);
  SetModulationColorAllActiveEffectsForParticleDB(color, x8c_firstDraw);
  SetModulationColorAllActiveEffectsForParticleDB(color, xa0_lastDraw);
}

void CParticleDatabase::SetModulationColorAllActiveEffectsForParticleDB(const CColor& color,
                                                                        DrawMap& map) {
  for (AUTO(it, map.begin()); it != map.end(); ++it) {
    if (!it->second.null()) {
      it->second->SetModulationColor(color);
    }
  }
}
