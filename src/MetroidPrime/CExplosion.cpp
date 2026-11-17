#include "MetroidPrime/CExplosion.hpp"

#include "MetroidPrime/CGameLight.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"

#include "Kyoto/Graphics/CCubeModel.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Math/CFrustumPlanes.hpp"
#include "Kyoto/Particles/CElementGen.hpp"
#include "Kyoto/Particles/CGenDescription.hpp"
#include "Kyoto/Particles/CParticleElectric.hpp"
#include "MetaRender/CCubeRenderer.hpp"

#ifdef TARGET_PC
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CFirstPersonCamera.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "port_profile.h"

#include <dolphin/gx/GXExtra.h>

#include <chrono>

namespace {
// The window CStateManager::Update opens around its entities' Think (PortSetAsyncWindow).
bool sPortAsyncWindow = false;
// Set when the side worker refused a step (no fourth core): every Think runs whole, as before.
bool sPortNoWorker = false;
// The explosions whose Think has its rest still to run, in the order their steps began.
CExplosion* sPortPendingHead = nullptr;
CExplosion* sPortPendingTail = nullptr;
uint sPortStatStepped = 0;
uint sPortStatWaited = 0;
uint sPortStatWaitUs = 0;
} // namespace
#endif

CExplosion::CExplosion(const TLockedToken< CGenDescription >& particle, TUniqueId uid, bool active,
                       const CEntityInfo& info, const rstl::string& name, const CTransform4f& xf,
                       const uint flags, const CVector3f& scale, const CColor& color)
: CEffect(uid, info, active, name, xf)
, xe8_particleGen(rs_new CElementGen(TToken< CGenDescription >(particle), CElementGen::kMOT_Normal,
                                     flags & 0x2 ? CElementGen::kOSF_Two : CElementGen::kOSF_One))
, xec_explosionLight(kInvalidUniqueId)
, xf0_sourceId(CToken(particle).GetTag().GetId())
, xf4_24_renderThermalHot(flags & 0x4)
, xf4_25_hasRenderBounds(true)
, xf4_26_renderXray(flags & 0x8)
, xf8_time(0.0f) {
#ifdef TARGET_PC
  xPortFixedStep = (flags & 0x10) != 0;
#endif
  SetThermalFlags(flags & 0x1 ? kTF_Cold : kTF_Hot);
  xe8_particleGen->SetGlobalTranslation(xf.GetTranslation());
  xe8_particleGen->SetOrientation(xf.GetRotation());
  xe8_particleGen->SetGlobalScale(scale);
  xe8_particleGen->SetModulationColor(color);
}

CExplosion::CExplosion(const TLockedToken< CElectricDescription >& electric, TUniqueId uid,
                       bool active, const CEntityInfo& info, const rstl::string& name,
                       const CTransform4f& xf, uint flags, const CVector3f& scale,
                       const CColor& color)
: CEffect(uid, info, active, name, xf)
, xe8_particleGen(rs_new CParticleElectric(electric))
, xec_explosionLight(kInvalidUniqueId)
, xf0_sourceId(CToken(electric).GetTag().GetId())
, xf4_24_renderThermalHot(flags & 0x4)
, xf4_25_hasRenderBounds(true)
, xf4_26_renderXray(flags & 0x8)
#if NONMATCHING || defined(TARGET_PC)
, xf8_time(0.0f)
#endif
{
#ifdef TARGET_PC
  xPortFixedStep = (flags & 0x10) != 0;
#endif
  SetThermalFlags(flags & 0x1 ? kTF_Cold : kTF_Hot);
  xe8_particleGen->SetGlobalTranslation(xf.GetTranslation());
  xe8_particleGen->SetOrientation(xf.GetRotation());
  xe8_particleGen->SetGlobalScale(scale);
  xe8_particleGen->SetModulationColor(color);
}

CExplosion::~CExplosion() {
#ifdef TARGET_PC
  // A step still running reads the system: wait for it. The rest of its Think goes with it.
  if (xPortStepJob != 0) {
    while (!GXPortSideJobDone(xPortStepJob)) {
      GXPortSideJobWait(xPortStepJob);
    }
    xPortStepJob = 0;
  }
  if (xPortTailPending) {
    xPortTailPending = false;
    PortUnlinkPending();
  }
#endif
}

void CExplosion::AddToRenderer(const CFrustumPlanes& frustum, const CStateManager& mgr) const {
#ifdef TARGET_PC
  const_cast< CExplosion* >(this)->PortFinishStep();
#endif
  if (GetPreRenderClipped()) {
    return;
  }

  if ((xf4_24_renderThermalHot && mgr.GetThermalDrawFlag() == kTD_Hot) ||
      (xf4_26_renderXray && mgr.GetPlayerState()->GetActiveVisor(mgr) == CPlayerState::kPV_XRay)) {
    EnsureRendered(mgr);
  } else {
    gpRender->AddParticleGen(*xe8_particleGen);
  }
}

void CExplosion::PreRender(CStateManager& mgr, const CFrustumPlanes& frustum) {
#ifdef TARGET_PC
  PortFinishStep();
#endif
  CActor::PreRender(mgr, frustum);
  SetPreRenderClipped(!xf4_25_hasRenderBounds || !frustum.BoxInFrustumPlanes(GetRenderBoundsCached()));
}

void CExplosion::Think(float dt, CStateManager& mgr) {
#ifdef TARGET_PC
  PortFinishStep(); // one from before (one Think a tick is the rule, but the pass ends them)
#endif
  if (GetTransformDirtySpare()) {
    xe8_particleGen->SetGlobalTranslation(GetTranslation());
    xe8_particleGen->SetOrientation(GetTransform().GetRotation());
    SetTransformDirtySpare(false);
  }
#ifdef TARGET_PC
  // CExplosionMP1::Think (0xd0e300); the 15 s lifetime below still counts real time.
  const double step = xPortFixedStep ? 1.0 / 60.0 : dt;
  // 0.46.0 test 12: explosions (a pickup's flash, the Bombus' blasts, Thardus' rocks) were up to 6
  // ms a frame of test 11's Think pass, nearly all the system's step. It goes to the side worker
  // while the pass goes on, as the effects' did in test 11; the rest runs at the pass's end, or
  // first when anything reaches the system sooner.
  if (sPortAsyncWindow && !sPortNoWorker) {
    xPortStepDt = step;
    xPortTailDt = dt;
    xPortTailMgr = &mgr;
    xPortStepJob = GXPortSideJobSubmitAny(&CExplosion::PortStepJob, this);
    if (xPortStepJob != 0) {
      ++sPortStatStepped;
      xPortTailPending = true;
      PortLinkPending();
      return;
    }
    sPortNoWorker = true;
  }
  xe8_particleGen->Update(step);
  PortThinkTail(dt, mgr);
}

// What Think does after the system's step: as it was, from here.
void CExplosion::PortThinkTail(float dt, CStateManager& mgr) {
#else
  xe8_particleGen->Update(dt);
#endif

  if (xec_explosionLight != kInvalidUniqueId) {
    CGameLight* light = TCastToPtr< CGameLight >(mgr.ObjectById(xec_explosionLight));
    if (light && GetActive())
      light->SetLight(xe8_particleGen->GetLight());
  }
#ifdef TARGET_PC
  PortUpdateNestedLights(mgr);
#endif

  xf8_time += dt;

  if (xf8_time > 15.f) {
#ifdef TARGET_PC
    xPortNestedLights.Delete(mgr, GetUniqueId());
#endif
    mgr.DeleteObjectRequest(GetUniqueId());
  } else if (xe8_particleGen->IsSystemDeletable()) {
#ifdef TARGET_PC
    xPortNestedLights.Delete(mgr, GetUniqueId());
#endif
    mgr.DeleteObjectRequest(GetUniqueId());
  }
}

#ifdef TARGET_PC
bool CExplosion::PortIsRemastered() const {
  return xe8_particleGen->Get4CharId() == 'PART' &&
         static_cast< const CElementGen& >(*xe8_particleGen).PortIsRemastered();
}

const CGenDescription* CExplosion::PortFaceReflectDesc() const {
  if (!PortIsRemastered()) {
    return nullptr;
  }
  const CGenDescription* desc = static_cast< const CElementGen& >(*xe8_particleGen).PortDesc();
  return desc->xPortFrmd != 0 ? desc : nullptr;
}

void CExplosion::PortUpdateNestedLights(CStateManager& mgr) {
  xPortNestedLights.Update(mgr, xe8_particleGen.get(), GetActive(), GetUniqueId(),
                           GetCurrentAreaId(), rstl::string_l("ExplodePHDR_") + GetDebugName(),
                           PortNestedLightSourceId(GetSourceId()), 1);
}

void CExplosion::SetActive(bool active) {
  PortFinishStep();
  CActor::SetActive(active);
}

void CExplosion::PortStepJob(void* data) {
  PORT_ZONE_TIMED("explosion: particle system (side worker, core 3)");
  CExplosion& explosion = *static_cast< CExplosion* >(data);
  explosion.xe8_particleGen->Update(explosion.xPortStepDt);
}

void CExplosion::PortFinishStep() {
  if (!xPortTailPending) {
    return;
  }
  xPortTailPending = false;
  PortUnlinkPending();
  if (xPortStepJob != 0) {
    if (!GXPortSideJobDone(xPortStepJob)) {
      const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
      // Past the side jobs' limit (logged there) it waits again: the worker may still be stepping
      // the system, which nothing may read meanwhile.
      while (!GXPortSideJobDone(xPortStepJob)) {
        GXPortSideJobWait(xPortStepJob);
      }
      ++sPortStatWaited;
      sPortStatWaitUs += static_cast< uint >(
          std::chrono::duration_cast< std::chrono::microseconds >(std::chrono::steady_clock::now() - start)
              .count());
    }
    xPortStepJob = 0;
  }
  PortThinkTail(xPortTailDt, *xPortTailMgr);
}

void CExplosion::PortLinkPending() {
  xPortNextPending = nullptr;
  (sPortPendingTail != nullptr ? sPortPendingTail->xPortNextPending : sPortPendingHead) = this;
  sPortPendingTail = this;
}

void CExplosion::PortUnlinkPending() {
  CExplosion* prev = nullptr;
  for (CExplosion* e = sPortPendingHead; e != nullptr; prev = e, e = e->xPortNextPending) {
    if (e == this) {
      (prev != nullptr ? prev->xPortNextPending : sPortPendingHead) = xPortNextPending;
      if (sPortPendingTail == this) {
        sPortPendingTail = prev;
      }
      xPortNextPending = nullptr;
      return;
    }
  }
}

void CExplosion::PortSetAsyncWindow(bool open) { sPortAsyncWindow = open; }

void CExplosion::PortFinishAll() {
  while (sPortPendingHead != nullptr) {
    sPortPendingHead->PortFinishStep();
  }
}

void CExplosion::PortTakeStepStats(uint& stepped, uint& waited, uint& waitUs) {
  stepped = sPortStatStepped;
  waited = sPortStatWaited;
  waitUs = sPortStatWaitUs;
  sPortStatStepped = sPortStatWaited = sPortStatWaitUs = 0;
}
#endif

ENTITY_ACCEPT_IMPL(CExplosion)

void CExplosion::AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId sender, CStateManager& mgr) {
#ifdef TARGET_PC
  PortFinishStep();
  xPortNestedLights.Forward(msg, sender, GetUniqueId(), mgr);
#endif
  switch (msg) {
  case kSM_Registered:
    if (xe8_particleGen->SystemHasLight()) {
      xec_explosionLight = mgr.AllocateUniqueId();
      uint sourceId = xf0_sourceId;
      mgr.AddObject(rs_new CGameLight(xec_explosionLight, GetCurrentAreaId(), GetActive(),
                                      rstl::string_l("ExplodePLight_") + GetDebugName(),
                                      GetTransform(), GetUniqueId(), xe8_particleGen->GetLight(),
                                      sourceId, 1, 0.f));
    }
#ifdef TARGET_PC
    // CExplosionMP1GOC::CreateExplosionGameObject 0xd0d16c: a Remastered PART with FRMD set adds
    // the explosion to the face reflection, at its translation from the first-person camera's.
    if (const CGenDescription* desc = PortFaceReflectDesc()) {
      if (const CFirstPersonCamera* cam = TCastToConstPtr< CFirstPersonCamera >(
              mgr.GetCameraManager()->GetCurrentCamera(mgr))) {
        PortFaceReflection::AddExplosion(
            mgr.PortFaceReflectionManager(), GetUniqueId(), desc->xPortFrmd,
            (GetTranslation() - cam->GetTransform().GetTranslation()).Magnitude());
      }
    }
#endif
    break;
  case kSM_Deleted:
#ifdef TARGET_PC
    PortFaceReflection::RemoveExplosion(mgr.PortFaceReflectionManager(), GetUniqueId());
#endif
    if (xec_explosionLight != kInvalidUniqueId) {
      mgr.DeleteObjectRequest(xec_explosionLight);
      xec_explosionLight = kInvalidUniqueId;
    }
    break;

  default:
    break;
  }
  CActor::AcceptScriptMsg(msg, sender, mgr);

  if (xec_explosionLight != kInvalidUniqueId)
    mgr.SendScriptMsgAlways(xec_explosionLight, sender, msg);
}

void CExplosion::CalculateRenderBounds() {
#ifdef TARGET_PC
  PortFinishStep();
#endif
  rstl::optional_object< CAABox > bounds = xe8_particleGen->GetBounds();
  if (bounds) {
    SetRenderBounds(*bounds);
    xf4_25_hasRenderBounds = true;
  } else {
    xf4_25_hasRenderBounds = false;
    CVector3f pos = GetTransform().GetTranslation();
    SetRenderBounds(CAABox(pos, pos));
  }
}

void CExplosion::Render(const CStateManager& mgr) const {
#ifdef TARGET_PC
  const_cast< CExplosion* >(this)->PortFinishStep();
#endif
  if (mgr.GetThermalDrawFlag() == kTD_Hot && xf4_24_renderThermalHot) {
    CElementGen::SetSubtractBlend(true);
    CCubeModel::SetRenderModelBlack(true);
    xe8_particleGen->Render();
    CCubeModel::SetRenderModelBlack(false);
    CElementGen::SetSubtractBlend(false);
    return;
  }

  CElementGen::SetSubtractBlend(!xf4_24_renderThermalHot);
  CGraphics::SetFog(kRFM_PerspLin, 0.f, 75.f, CColor::Black());
  xe8_particleGen->Render();
  mgr.SetupFogForArea(GetCurrentAreaId());
  CElementGen::SetSubtractBlend(false);
}
