#ifndef _CSKINRULES
#define _CSKINRULES

#include "Kyoto/Animation/CVirtualBone.hpp"
#include "Kyoto/CFactoryFnReturn.hpp"

class CCharLayoutInfo;
class CInputStream;
class CModel;
class CPoseAsTransforms;

class CSkinRules {
public:
  CSkinRules(CInputStream& in);
  ~CSkinRules();

  void InitLockedCacheState(const CModel& model);
  static void StartNextTransaction();

  void BuildAccumulatedTransforms(const CPoseAsTransforms& pose,
                                  const CCharLayoutInfo& layoutInfo) const;
  void BuildPoints(volatile void* pipe) const;
  void BuildNormals(volatile void* pipe) const;
  void BuildNormalsFrom(const CVector3f* averageNormals, CVector3f* out) const;
#ifdef TARGET_PC
  // Port: skins straight from the model's big-endian arrays into plain memory,
  // without the locked-cache emulation; same arithmetic as BuildPoints/Normals.
  void PortBuildPointsAndNormals(const CModel& model, float* points, float* normals) const;
  // Port (PS Vita, 0.42.0 test 18): as PortBuildPointsAndNormals, then `padBytes` zeroes at `pad`,
  // on the side worker (the fourth core) when there is one, from a copy of the bones as
  // BuildAccumulatedTransforms left them. Returns the side job (GXPortSideJob*), or 0 once done
  // here (no worker, or a model the threads split).
  uint PortSubmitPointsAndNormals(const CModel& model, float* points, float* normals, uchar* pad,
                                  uint padBytes) const;
#endif
  int GetNumPoints() const { return x10_vertexCount; }
  int GetNumNormals() const { return x14_normalCount; }
  int GetNumVirtualBones() const { return x0_virtualBones.size(); }
  const rstl::vector< CVirtualBone >& GetVirtualBones() const { return x0_virtualBones; }

  
  static int ProcessingPoints(int count, ushort** buf);
  static int ProcessingNormals(int count, ushort** buf);
private:
  rstl::vector< CVirtualBone > x0_virtualBones;
  int x10_vertexCount;
  int x14_normalCount;
};

const CFactoryFnReturn FSkinRulesFactory(const SObjectTag& tag, CInputStream& in, const CVParamTransfer&);
#endif // _CSKINRULES
