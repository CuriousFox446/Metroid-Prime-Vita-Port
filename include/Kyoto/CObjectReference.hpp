#ifndef _COBJECTREFERENCE
#define _COBJECTREFERENCE

#include <Kyoto/CVParamTransfer.hpp>
#include <Kyoto/SObjectTag.hpp>
#include <rstl/auto_ptr.hpp>

#ifdef TARGET_PC
#include <atomic>
#endif

class IObj;
class IObjectStore;
class CObjectReference {
public:
  CObjectReference(const rstl::auto_ptr< IObj >& obj);
  ~CObjectReference();
  CObjectReference(IObjectStore& store, const rstl::auto_ptr< IObj >& obj, const SObjectTag& tag,
                   CVParamTransfer xfer);

  bool IsLoaded() const { return x10_object != nullptr; }

#ifdef TARGET_PC
  void AddReference() { x0_refCount.fetch_add(1, std::memory_order_relaxed); }
#else
  void AddReference() { x0_refCount++; }
#endif
  int RemoveReference();
  void Lock();
  void Unlock();
  IObj* GetObject();
  void Unload();
  void CancelLoad();
  bool IsLoading() const;
  const SObjectTag& GetTag() const { return x4_objTag; }
#ifdef TARGET_PC
  int GetLockCount() const { return x2_lockCount; }
#endif

private:
#ifdef TARGET_PC
  // 0.42.0 test 19: the side worker copies and locks the tokens of the child systems it builds
  // while the game thread copies others, of the same objects at times: the counts are atomic. A
  // count reaching zero (an unload) is only ever the game thread's: every token the side worker
  // takes is of a description some other token keeps loaded.
  std::atomic< short > x0_refCount;
  std::atomic< short > x2_lockCount;
  bool x3_loading;
#else
  short x0_refCount : 16;
  short x2_lockCount : 15;
  short x3_loading : 1;
#endif
  SObjectTag x4_objTag;
  IObjectStore* xc_objectStore;
  IObj* x10_object;
  CVParamTransfer x14_params;
};
#endif // _COBJECTREFERENCE
