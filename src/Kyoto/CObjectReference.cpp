#include "Kyoto/CObjectReference.hpp"
#include "Kyoto/Alloc/CMemory.hpp"
#include "Kyoto/CResFactory.hpp"
#include "Kyoto/CSimplePool.hpp"

CObjectReference::CObjectReference(IObjectStore& store, const rstl::auto_ptr< IObj >& obj,
                                   const SObjectTag& tag, CVParamTransfer xfer)
: x0_refCount(0)
, x2_lockCount(0)
, x3_loading(false)
, x4_objTag(tag)
, xc_objectStore(&store)
, x10_object(obj.release())
, x14_params(xfer) {}

CObjectReference::CObjectReference(const rstl::auto_ptr< IObj >& obj)
: x0_refCount(0)
, x2_lockCount(0)
, x3_loading(false)
, xc_objectStore(nullptr)
, x10_object(obj.release())
, x14_params(CVParamTransfer::Null()) {}

CObjectReference::~CObjectReference() {
  if (x10_object) {
    delete x10_object;
  } else if (x3_loading) {
    xc_objectStore->GetFactory().CancelBuild(x4_objTag);
  }
}

void CObjectReference::Lock() {
  ++x2_lockCount;

  if (x10_object == nullptr && !x3_loading) {
    xc_objectStore->GetFactory().BuildAsync(x4_objTag, x14_params, &x10_object);
    x3_loading = true;
  }
}

IObj* CObjectReference::GetObject() {
  if (x10_object == nullptr) {
    x10_object = xc_objectStore->GetFactory().Build(x4_objTag, x14_params).release();
  }
  x3_loading = false;
  return x10_object;
}

void CObjectReference::Unload() {
  delete x10_object;
  x10_object = nullptr;
  x3_loading = false;
}

void CObjectReference::CancelLoad() {
  if (xc_objectStore == nullptr) {
    return;
  }

  if (!IsLoading()) {
    return;
  }
  xc_objectStore->GetFactory().CancelBuild(x4_objTag);
  x3_loading = false;
}

int CObjectReference::RemoveReference() {
#ifdef TARGET_PC
  // The count this call left (atomic: see the header), not a second read another thread could move.
  const int left = x0_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (left == 0) {
#else
  --x0_refCount;
  if (x0_refCount == 0) {
#endif
    if (x10_object != nullptr) {
      Unload();
    } else if (IsLoading()) {
      CancelLoad();
    }
    if (xc_objectStore != nullptr) {
      xc_objectStore->ObjectUnreferenced(x4_objTag);
    }
  }

#ifdef TARGET_PC
  return left;
#else
  return x0_refCount;
#endif
}

void CObjectReference::Unlock() {
#ifdef TARGET_PC
  if (x2_lockCount.fetch_sub(1, std::memory_order_acq_rel) - 1 != 0) {
    return;
  }
#else
  --x2_lockCount;

  if (x2_lockCount != 0) {
    return;
  }
#endif

  if (x10_object != nullptr && xc_objectStore != nullptr) {
    Unload();
  } else if (IsLoading()) {
    CancelLoad();
  }
}

bool CObjectReference::IsLoading() const { return x3_loading && x10_object == nullptr; }
