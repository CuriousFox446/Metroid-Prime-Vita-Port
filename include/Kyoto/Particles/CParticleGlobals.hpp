#ifndef _CPARTICLEGLOBALS
#define _CPARTICLEGLOBALS

#include <cstring>

#include "types.h"

#include "Kyoto/SObjectTag.hpp"
#include "Kyoto/Particles/CElementGen.hpp"

#ifdef TARGET_PC
#include "port_thread_slot.h"
#endif

class CParticleGlobals {
public:
#ifdef TARGET_PC
  struct SPortSwooshEval;
#endif
  struct SParticleSystem {
    FourCC x0_type;
    CElementGen* x4_system;
    SParticleSystem* x8_prev;
#ifdef TARGET_PC
    // Remastered keeps the lifetime tween (UpdateLifetimeTweenValues 0x24ef38) in the system's own
    // context; the port's global is saved here so a child generator that updates or renders
    // inside this system leaves it as it was.
    float xPortSavedTween;
#endif

    SParticleSystem(FourCC type, CElementGen* system)
    : x0_type(type)
    , x4_system(system)
    , x8_prev(mCurrentParticleSystem)
#ifdef TARGET_PC
    , xPortSavedTween(xPortSystemTween)
#endif
    {
      mCurrentParticleSystem = this;
    }
    ~SParticleSystem() {
      mCurrentParticleSystem = x8_prev;
#ifdef TARGET_PC
      xPortSystemTween = xPortSavedTween;
#endif
    }
  };

  static void SetEmitterTime(int time);
  static void SetParticleLifetime(int lifetime);
  static void UpdateParticleLifetimeTweenValues(int time);

  static int GetParticleLifetime() { return mParticleLifetime; }
  static float GetParticleLifetimeReal() { return mParticleLifetimeReal; }
  static int GetEmitterTime() { return mEmitterTime; }
  static float GetEmitterTimeReal() { return mEmitterTimeReal; }
  static int GetParticleLifetimePercentage() { return mParticleLifetimePercentage; }
  static float GetParticleLifetimePercentageReal() { return mParticleLifetimePercentageReal; }
  static float GetParticleLifetimePercentageRemainder() {
    return mParticleLifetimePercentageRemainder;
  }
  static CElementGen::CParticle* GetCurrentParticle() { return mCurrentParticle; }
#ifdef TARGET_PC
  // GC reads address 0 harmlessly when no ADV values are bound; read zeros instead.
  static float* GetParticleAccessParameters() {
    static float sZeros[13] = {};
    return mParticleAccessParameters != nullptr ? mParticleAccessParameters : sZeros;
  }
  // port-only: Remastered's per-particle int variables IAV0..3 (PAP0..3), stored as their bits in
  // the four floats after ADV1..9 (CElementGen::CAdvancedValues).
  static int GetParticleAccessInt(int n) {
    int v;
    std::memcpy(&v, &GetParticleAccessParameters()[9 + n], sizeof(v));
    return v;
  }
  static void SetParticleAccessInt(float* params, int n, int v) {
    std::memcpy(&params[9 + n], &v, sizeof(v));
  }
#else
  static float* GetParticleAccessParameters() { return mParticleAccessParameters; }
#endif
  static SParticleSystem* GetCurrentParticleSystem() { return mCurrentParticleSystem; }
  static void SetCurrentParticleSystem(SParticleSystem* system) { mCurrentParticleSystem = system; }

public:
#ifdef TARGET_PC
  // 0.42.0 test 19: every one of these is per thread (port_thread_slot.h), the side worker stepping
  // actors' systems while the game thread evaluates its own. A thread's values are together in a
  // slot of their own cache lines; the fields below read and write them as the statics did.
  struct alignas(64) SPortSlot {
    int mParticleLifetime = 0;
    float mParticleLifetimeReal = 0.f;
    int mEmitterTime = 0;
    float mEmitterTimeReal = 0.f;
    int mParticleLifetimePercentage = 0;
    float mParticleLifetimePercentageReal = 0.f;
    float mParticleLifetimePercentageRemainder = 0.f;
    CElementGen::CParticle* mCurrentParticle = nullptr;
    float* mParticleAccessParameters = nullptr;
    SParticleSystem* mCurrentParticleSystem = nullptr;
    CElementGen::CParticle* xPortUVParticle = nullptr;
    const CElementGen::CParticle* xPortIrndParticle = nullptr;
    bool xPortIrnd28 = false;
    const SPortSwooshEval* xPortSwooshEval = nullptr;
    float xPortSystemTween = 0.f;
    int xPortSlot2Percentage = 0;
    float xPortSlot2Remainder = 0.f;
  };
  static SPortSlot sPortSlots[2];
  static SPortSlot& PortSlot() { return sPortSlots[PortThreadSlot()]; }
  template < typename T, T SPortSlot::*Member >
  struct PortField {
    operator T&() const { return PortSlot().*Member; }
    const PortField& operator=(const T& value) const {
      PortSlot().*Member = value;
      return *this;
    }
    T operator->() const { return PortSlot().*Member; }
  };
  static constexpr PortField< int, &SPortSlot::mParticleLifetime > mParticleLifetime{};
  static constexpr PortField< float, &SPortSlot::mParticleLifetimeReal > mParticleLifetimeReal{};
  static constexpr PortField< int, &SPortSlot::mEmitterTime > mEmitterTime{};
  static constexpr PortField< float, &SPortSlot::mEmitterTimeReal > mEmitterTimeReal{};
  static constexpr PortField< int, &SPortSlot::mParticleLifetimePercentage > mParticleLifetimePercentage{};
  static constexpr PortField< float, &SPortSlot::mParticleLifetimePercentageReal >
      mParticleLifetimePercentageReal{};
  static constexpr PortField< float, &SPortSlot::mParticleLifetimePercentageRemainder >
      mParticleLifetimePercentageRemainder{};
  static constexpr PortField< CElementGen::CParticle*, &SPortSlot::mCurrentParticle > mCurrentParticle{};
  static constexpr PortField< float*, &SPortSlot::mParticleAccessParameters > mParticleAccessParameters{};
  static constexpr PortField< SParticleSystem*, &SPortSlot::mCurrentParticleSystem > mCurrentParticleSystem{};
  // port-only: the particle a per-particle GetValueUV call is for (null = none)
  static constexpr PortField< CElementGen::CParticle*, &SPortSlot::xPortUVParticle > xPortUVParticle{};
  // port-only: the particle the elements are being evaluated for, set only while a generator with
  // PIRN is evaluating per-particle elements (null otherwise). IRND elements then give a value
  // fixed per (particle, element) instead of relying on being read at frame 0.
  static constexpr PortField< const CElementGen::CParticle*, &SPortSlot::xPortIrndParticle > xPortIrndParticle{};
  // port-only: set while a Remastered decal evaluates its elements. IRND keeps retail's
  // frame-0-only write but draws Remastered's PortFloat28 (IRND 0x2959d8) from the global random.
  static constexpr PortField< bool, &SPortSlot::xPortIrnd28 > xPortIrnd28{};
#else
  static int mParticleLifetime;
  static float mParticleLifetimeReal;
  static int mEmitterTime;
  static float mEmitterTimeReal;
  static int mParticleLifetimePercentage;
  static float mParticleLifetimePercentageReal;
  static float mParticleLifetimePercentageRemainder;
  static CElementGen::CParticle* mCurrentParticle;
  static float* mParticleAccessParameters;
  static SParticleSystem* mCurrentParticleSystem;
#endif
#ifdef TARGET_PC
  // port-only: what Remastered's DFCS/DFCP (0x299134 / 0x299064) read from a swoosh's context: the
  // swoosh's local->world transform (T(global) * R(global orientation) * S(global scale), the
  // BuildTransform4f of its registers), the swoosh's local translation (DFCS) and the position of
  // the segment being evaluated (DFCP, segment +0xc). Null outside a swoosh evaluation.
  struct SPortSwooshEval {
    CTransform4f worldFromLocal;
    CVector3f systemLocal;
    const CVector3f* segmentPos;
    // The point's swoosh node key (KEWS, Remastered 0x24f00c), when the point has one.
    bool hasNode = false;
    int nodePercentage = 0;
    float nodeRemainder = 0.f;
  };
  static constexpr PortField< const SPortSwooshEval*, &SPortSlot::xPortSwooshEval > xPortSwooshEval{};
  // port-only: the system tween Remastered keeps at ctx+0x60 (UpdateLifetimeTweenValues 0x24ef38 on
  // time slot 0, written by every system tick): clamp(system frame / system lifetime, 0, 1). A
  // lifetime of 0 reads as 1, and the infinite one (the port's PSLT default 0x7FFFFF, retail's
  // 0x1000000) gives 0. The reciprocal is a plain 1 / lifetime; the original's estimate is within
  // an ulp of it.
  static constexpr PortField< float, &SPortSlot::xPortSystemTween > xPortSystemTween{};
  static void PortSetSystemTime(int frame, int lifetime);
  // port-only: Remastered's time slot 2 (ctx+0x80), which only CParticleElectric2::UpdateBolt
  // 0xb77638 writes (node i over node count - 1, the same tween as above); the context ctor
  // 0x24f06c zeroes it, so it reads 0 everywhere else. KESP keyframes read it.
  static constexpr PortField< int, &SPortSlot::xPortSlot2Percentage > xPortSlot2Percentage{};
  static constexpr PortField< float, &SPortSlot::xPortSlot2Remainder > xPortSlot2Remainder{};
  static void PortSetSlot2Time(int frame, int lifetime);
  static void PortClearSlot2Time() {
    xPortSlot2Percentage = 0;
    xPortSlot2Remainder = 0.f;
  }
  struct PortSwooshEvalScope {
    const SPortSwooshEval* prev;
    explicit PortSwooshEvalScope(const SPortSwooshEval* eval) : prev(xPortSwooshEval) {
      xPortSwooshEval = eval;
    }
    ~PortSwooshEvalScope() { xPortSwooshEval = prev; }
  };
  // A deterministic uniform in [0, 1) from the particle's seed and the element's address, and the
  // raw 32-bit hash it comes from.
  static u32 PortIrndBits(const CElementGen::CParticle* particle, const void* element);
  static float PortIrndUnit(const CElementGen::CParticle* particle, const void* element);
  // port-only: Remastered's GetCameraTransform 0x27b304, the game camera (camera-to-world).
  // CDisplayManager::Update 0xafded0 writes once per frame and the camera elements (DFCS/DFCP,
  // CMPS, CPSS, CMRT) read at update and draw alike. The view matrix is no stand-in at update: it
  // holds whatever the last draw left (a HUD or gun view). With no world (front end) it is unset
  // and they read the view matrix.
  static void PortSetCameraTransform(const CTransform4f* xf);
  static const CTransform4f& PortCameraTransform();
#endif
};

#endif // _CPARTICLEGLOBALS
