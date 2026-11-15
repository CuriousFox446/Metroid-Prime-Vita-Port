#ifndef _CPARTICLEDATABASE
#define _CPARTICLEDATABASE

#include "types.h"

#include "Kyoto/Animation/CCharacterInfo.hpp"
#include "Kyoto/IObjectStore.hpp"
#include "Kyoto/TToken.hpp"
#include "MetroidPrime/TGameTypes.hpp"

#include "rstl/auto_ptr.hpp"
#include "rstl/map.hpp"
#include "rstl/rc_ptr.hpp"
#include "rstl/string.hpp"

class CElectricDescription;
class CGenDescription;
class CParticleGenInfo;
class CStateManager;
class CSwooshDescription;
class CCharLayoutInfo;
class CColor;
class CFrustumPlanes;
class CPoseAsTransforms;
class CTransform4f;
class CVector3f;
class CAuxiliaryParticleData;

class CParticleDatabase {
public:
  typedef rstl::map< rstl::string, rstl::auto_ptr< CParticleGenInfo > > DrawMap;

  CParticleDatabase();
  ~CParticleDatabase();

  void CacheParticleDesc(const CCharacterInfo::CParticleResData& partData);
  void CacheParticleDesc(const SObjectTag& tag);
  void SetParticleEffectState(const rstl::string& name, bool active, CStateManager& mgr);
  void SuspendAllActiveEffects(CStateManager& mgr);
  void AddParticleEffect(const rstl::string& name, int flags, const CParticleData& data,
                         const CVector3f& scale, CStateManager& mgr, TAreaId areaId, bool oneShot,
                         int particleLightIdx);
  void Update(float dt, const CPoseAsTransforms& pose, const CCharLayoutInfo& layoutInfo,
              const CTransform4f& xf, const CVector3f& scale, CStateManager& mgr);
  void AddToRendererClipped(const CFrustumPlanes&) const;
  void RenderSystemsToBeDrawnFirst() const;
  void RenderSystemsToBeDrawnLast() const;
  void DeleteAllLights(CStateManager& mgr);
  void AddToRendererClippedMasked(const CFrustumPlanes&, int mask, int target) const;
  void RenderSystemsToBeDrawnFirstMasked(int mask, int target) const;
  void RenderSystemsToBeDrawnLastMasked(int mask, int target) const;
  void SetModulationColorAllActiveEffects(const CColor& color);
  CParticleGenInfo* GetParticleEffect(const rstl::string& name);
  void SetExternalVarValue(const rstl::string& name, int index, float value);
  void AddAuxiliaryParticleEffect(const rstl::string& name, int flags,
                                  const CAuxiliaryParticleData& data, const CVector3f& scale,
                                  CStateManager& mgr, TAreaId areaId, int lightIdx);
  void SetUpdatesEnabled(bool enabled) { xb4_24_updatesEnabled = enabled; }

#ifdef TARGET_PC
  bool AreAnySystemsDrawnWithModel() const {
    PortFinishConst();
    return xb4_25_anySystemsDrawnWithModel;
  }

  // 0.42.0 test 19 (PS Vita): within the open window (CStateManager::Update around its entities'
  // Think), Update runs in three parts. On the game thread, the first: the systems' transforms from
  // the pose and their lifetimes, as before. On the side worker (the fourth core), the systems'
  // own steps, which touch nothing else. On the game thread again, the rest (the lights the systems
  // feed, the finished ones removed, the times kept), before anything else reads this database
  // (every method finishes it first) or else by the drain at the end of the tick. The parts run
  // in the order the whole did, system by system; only the second half of each system's step now
  // waits until all of the database's systems have stepped.
  static void PortSetAsyncWindow(bool open);
  // Finishes every update the window left pending, in the order they began.
  static void PortFinishAll();
  // For the log: updates since the last call that went to the side worker, those finished early
  // (something read the database before the drain) and waited there, and that wait in all.
  static void PortTakeAsyncStats(uint& async, uint& waited, uint& waitUs);
#else
  bool AreAnySystemsDrawnWithModel() const { return xb4_25_anySystemsDrawnWithModel; }
#endif

private:
#ifdef TARGET_PC
  // The three parts of UpdateParticleGenDB, per system (see Update). Prepare is false for a system
  // the update passes over (its locator is not in the pose); Finish is true when the system is done
  // and comes out of the map.
  static bool PortPrepareParticleGen(CParticleGenInfo& info, const CPoseAsTransforms& pose,
                                     const CCharLayoutInfo& layout, const CTransform4f& xf,
                                     const CVector3f& scale, CStateManager& mgr);
  static bool PortFinishParticleGen(CParticleGenInfo& info, float dt, CStateManager& mgr, bool deleteIfDone);
  static bool PortPrepareParticleGenDB(const CPoseAsTransforms& pose, const CCharLayoutInfo& layout,
                                       const CTransform4f& xf, const CVector3f& scale, CStateManager& mgr,
                                       DrawMap& map);
  static void PortStepParticleGenDB(float dt, DrawMap& map);
  static void PortFinishParticleGenDB(float dt, CStateManager& mgr, DrawMap& map, bool deleteIfDone);
  static void PortStepJob(void* database);
  void PortStepAll();
  void PortFinish();
  void PortFinishConst() const { const_cast< CParticleDatabase* >(this)->PortFinish(); }
  void PortLinkPending();
  void PortUnlinkPending();

  // The pending update: its side job (0 when it has run or never went there), its dt and manager.
  uint xPortJob = 0;
  float xPortDt = 0.f;
  CStateManager* xPortMgr = nullptr;
  bool xPortPending = false; // the third part is still to run
  CParticleDatabase* xPortNextPending = nullptr;
#endif
  static void SetModulationColorAllActiveEffectsForParticleDB(const CColor& color, DrawMap& map);
  static void SuspendAllActiveEffectsForParticleDB(CStateManager& mgr, DrawMap& map);
  static void DeleteAllLightsForParticleDB(CStateManager& mgr, DrawMap& map);
  static void RenderParticleGenMap(const DrawMap& map);
  static void RenderParticleGenMapMasked(const DrawMap& map, int mask, int target);
  void AddToRendererClippedParticleGenMap(const DrawMap& map, const CFrustumPlanes& frustum) const;
  void AddToRendererClippedParticleGenMapMasked(const DrawMap& map, const CFrustumPlanes& frustum,
                                                int mask, int target) const;
  static void UpdateParticleGenDB(float dt, const CPoseAsTransforms& pose,
                                  const CCharLayoutInfo& layout, const CTransform4f& xf,
                                  const CVector3f& scale, CStateManager& mgr, DrawMap& map,
                                  bool deleteIfDone);
  void InsertParticleGen(const bool oneShot, int flags, const rstl::string& name,
                         const rstl::auto_ptr< CParticleGenInfo >& gen);

  rstl::map< CAssetId, rstl::rc_ptr< TLockedToken< CGenDescription > > > x0_particleDescs;
  rstl::map< CAssetId, rstl::rc_ptr< TLockedToken< CSwooshDescription > > > x14_swooshDescs;
  rstl::map< CAssetId, rstl::rc_ptr< TLockedToken< CElectricDescription > > > x28_electricDescs;
  DrawMap x3c_rendererDrawLoop;
  DrawMap x50_firstDrawLoop;
  DrawMap x64_lastDrawLoop;
  DrawMap x78_rendererDraw;
  DrawMap x8c_firstDraw;
  DrawMap xa0_lastDraw;
  bool xb4_24_updatesEnabled : 1;
  bool xb4_25_anySystemsDrawnWithModel : 1;
};
CHECK_SIZEOF(CParticleDatabase, 0xb8)

#endif // _CPARTICLEDATABASE
