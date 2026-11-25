// The Federation Marine import. See port_federation_marine.h.

#include "port_federation_marine.h"

#include "port_dread_reskin.h"
#include "port_dread_samus_mod.h"
#include "port_strings.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <utility>

namespace PortFederationMarine {

using PortEchoesSuits::EchoesJoint;
using PortEchoesSuits::Io;
using PortEchoesSuits::MaterialSet;
using PortEchoesSuits::PrimeCmdl;

namespace {

using port::Hex8;

constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kCSKR = 0x43534B52;
constexpr uint32_t kCINF = 0x43494E46;
constexpr uint32_t kTXTR = 0x54585452;
constexpr uint32_t kANCS = 0x414E4353;

bool Fail(std::string* error, const std::string& why) {
  if (error != nullptr) *error = why;
  return false;
}

uint32_t Get32(const std::vector<uint8_t>& d, size_t at) {
  if (at + 4 > d.size()) return 0;
  return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | uint32_t(d[at + 3]);
}
float GetF(const std::vector<uint8_t>& d, size_t at) {
  const uint32_t bits = Get32(d, at);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}
void Set32(std::vector<uint8_t>& d, size_t at, uint32_t value) {
  d[at] = uint8_t(value >> 24);
  d[at + 1] = uint8_t(value >> 16);
  d[at + 2] = uint8_t(value >> 8);
  d[at + 3] = uint8_t(value);
}
void SetF(std::vector<uint8_t>& d, size_t at, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  Set32(d, at, bits);
}
void Put32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(uint8_t(value >> 24));
  out.push_back(uint8_t(value >> 16));
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

struct V3 {
  double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double Length(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Normalized(V3 a) {
  const double l = Length(a);
  return l > 0 ? a * (1.0 / l) : a;
}
// `v` turned by `angle` (radians) about the unit `axis` (Rodrigues).
V3 Rotate(V3 v, V3 axis, double angle) {
  const double c = std::cos(angle), s = std::sin(angle);
  return v * c + Cross(axis, v) * s + axis * (Dot(axis, v) * (1 - c));
}
V3 FromArray(const float (&a)[3]) { return {a[0], a[1], a[2]}; }
V3 Rifle(V3 v) {
  const float in[3] = {float(v.x), float(v.y), float(v.z)};
  float out[3];
  RifleFrame(in, out);
  return {out[0], out[1], out[2]};
}
V3 Grapple(V3 v) {
  const float in[3] = {float(v.x), float(v.y), float(v.z)};
  float out[3];
  GrappleFrame(in, out);
  return {out[0], out[1], out[2]};
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Echoes' troopers (Metroid1.pak, Temple Grounds): the Federation Marine (Galactic_Trooper) and the
// Dark Trooper (Dark_Trooper), each a mesh on a copy of Echoes Samus's skeleton.
struct Trooper {
  const char* name;
  uint32_t cmdl, cskr, cinf;
};
constexpr Trooper kMarineTrooper{"Federation Marine", 0xA93225BD, 0xC21D8A6B, 0x0A32C29A};
constexpr Trooper kDarkTrooperModel{"Dark Trooper", 0xF7507082, 0x9C7FDF54, 0xB69EF3E2};
// The materials both convert to, in one set: the body, the rifle, the visor's glass (blended at 64%,
// depth-sorted) and the visor's glow (added over the same triangles: the marine's teal).
constexpr uint32_t kBodyMaterial = 0, kRifleMaterial = 1, kGlassMaterial = 2, kGlowMaterial = 3;
// Prime's skeleton of each body kind (PortEchoesSuits::SuitResources: cutscene, in-game, the pause
// screen's doll, the first-person left arm).
constexpr uint32_t kKindCinf[PortEchoesSuits::kBodyKinds] = {0x19C53902, 0xA496DA02, 0xF76E70A6, 0x516F4445};
constexpr const char* kKindNames[PortEchoesSuits::kBodyKinds] = {"cutscene", "in-game", "pause screen", "left arm"};
// The arms' triangles the first-person left (grapple) arm keeps: those whose vertices all lie this far
// out from the body's middle (|x|, the T-pose's arms along x), the upper arm's last 20 cm and on (the
// elbows are at 0.53; Prime's grapple arm reaches 0.188 behind its elbow).
constexpr double kArmCut = 0.33;
// The first-person rifle's and left hand's arms go on to the shoulder joints (0.295 out; the shoulder
// pads, on joints of their own, stay out): Samus's suit arm reaches 0.231 behind the gun's elbow, and
// the user, comparing the two: "extend both of the left and right arm".
constexpr double kRifleArmCut = 0.27;
// The skeleton's locator at the rifle's muzzle (where Echoes' trooper fires from).
constexpr const char* kMuzzleJoint = "GUN_Particle_LCTR";
// The bodies' hand and rifle turned this far about the forearm (degrees, about the bind pose's
// elbow-to-wrist axis, by each vertex's weight on the right wrist): flat, the rifle's top facing
// forward in the T-pose (the user's call, 2026-11-25, echoes-suits 21). Samus's animations roll her
// forearm for her cannon: posed with them the grip hangs straight down after -51 degrees in the pause
// screen's idle, -45 standing ready, -50 walking, -49 landing, -82 running (~/marine/roll.py; tests 51
// to 62 had -50, after the user's "held sideways" at 0 in test 50), so at -90 it sits about 40 degrees
// off down in most poses. The user saw both and chose this. In third person the game turns the forearm
// back so the grip hangs down (kBodyGripRollDegrees, port_federation_marine_mod.h, is this angle).
constexpr double kGripRollDegrees = kBodyGripRollDegrees;
// Prime's spider ball glass (SamusSpiderBallGlassCMDL): its set 0 teal (Power and Gravity), set 1
// green (Varia), set 2 aqua.
constexpr uint32_t kSpiderGlass = kSpiderBallGlass;
// The first-person left hand on the rifle's front grip (BuildLeftGrip), the user's pick of three
// previews (~/marine/leftgrip.py, after 0.46.0 test 52; "A"): the fingers closed 55 and 60 degrees at
// their two joints, the thumb swung 55 under the palm and its tip 35; the gripped point 6 mm from the
// knuckles toward the wrist and 42 mm under the palm. In the rifle's space: the guard's middle, its
// axis (down the guard, the index finger at its top), the way the hand's forearm would run unbent
// (toward the left shoulder) and the way it runs after the wrist's bend (back, 14 degrees below level:
// along the rifle it "kinda looks like a floating hand", 48 degrees down was "too low").
constexpr double kPi = 3.14159265358979323846;
constexpr double kGripCurl1 = 55.0, kGripCurl2 = 60.0, kGripThumb = 55.0, kGripThumbTip = 35.0;
constexpr double kGripReach = 0.006, kGripDepth = 0.042;
constexpr V3 kGripCentre{0.0, -0.075, -0.066};
constexpr V3 kGripAxis{0.0, -0.04, -0.07};
constexpr V3 kGripForearm{-0.78, -0.48, 0.37};
constexpr V3 kGripBend{-0.60, -0.78, -0.18};
// Its triangles: those weighing most on the upper arm, the elbow, the wrist or a finger, out to the
// shoulder joint (kRifleArmCut's). The upper arm runs straight on from the forearm, as the rifle's arm
// does (test 63 bent it at the elbow toward Samus's left shoulder; the user: "Let's see what it looks
// with the left arm straightened", then "Build it straight").

// Prime's Gravity Suit, measured: its limbs' purple (the median of the purple texels: hue, saturation,
// value; RecolourVaria's), and its visor's glass texel (FE5F7B41 over the face, 38,227,194).
constexpr double kGravityHue = 246.4, kGravitySat = 0.356, kGravityValue = 0.482;
constexpr double kGravityVisorHue = 169.5, kGravityVisorSat = 0.83, kGravityVisorValue = 0.89;
// The marine's lights and visor glow (947A82B4 over the visor: 43,139,139).
constexpr double kMarineLightHue = 180.0, kMarineLightSat = 0.69, kMarineVisorValue = 0.545;

using Weights = std::vector<std::pair<uint32_t, float>>;

// A CSKR's weights per vertex (Prime's and Echoes' groups alike: u32 count, {u32 n, {u32 joint, f32
// weight} x n, u32 vertices}).
bool VertexWeights(const std::vector<uint8_t>& cskr, std::vector<Weights>& out, std::string* error) {
  const uint32_t groups = Get32(cskr, 0);
  if (cskr.size() < 4 || groups > cskr.size() / 8) return Fail(error, "CSKR cut short");
  out.clear();
  size_t at = 4;
  for (uint32_t g = 0; g < groups; ++g) {
    const uint32_t n = Get32(cskr, at);
    if (at + 8 + size_t(n) * 8 > cskr.size()) return Fail(error, "CSKR group cut short");
    Weights w;
    for (uint32_t i = 0; i < n; ++i) w.emplace_back(Get32(cskr, at + 4 + i * 8), GetF(cskr, at + 8 + i * 8));
    const uint32_t vertices = Get32(cskr, at + 4 + size_t(n) * 8);
    out.insert(out.end(), vertices, w);
    at += 8 + size_t(n) * 8;
  }
  return true;
}

// A Prime CSKR of one run per vertex weights, consecutive equal ones merged.
std::vector<uint8_t> WriteSkin(const std::vector<Weights>& weights) {
  std::vector<uint8_t> out;
  Put32(out, 0);
  uint32_t runs = 0;
  for (size_t v = 0; v < weights.size();) {
    size_t end = v + 1;
    while (end < weights.size() && weights[end] == weights[v]) ++end;
    Put32(out, uint32_t(weights[v].size()));
    for (const auto& [joint, weight] : weights[v]) {
      Put32(out, joint);
      uint32_t bits;
      std::memcpy(&bits, &weight, 4);
      Put32(out, bits);
    }
    Put32(out, uint32_t(end - v));
    ++runs;
    v = end;
  }
  Set32(out, 0, runs);
  Put32(out, 0xFFFFFFFF);
  Put32(out, uint32_t(weights.size()));
  Put32(out, 0xFFFFFFFF);
  Put32(out, uint32_t(weights.size()));
  return out;
}

uint32_t SurfaceMaterial(const std::vector<uint8_t>& surface) { return Get32(surface, 0x0C); }

// The offset of a Prime material's blend factors ((dst << 16) | src), after its konst colours.
size_t BlendAt(const std::vector<uint8_t>& material) {
  const uint32_t flags = Get32(material, 0), ntex = Get32(material, 4);
  size_t at = 8 + size_t(ntex) * 4 + 8;  // texture indices, vertex flags, group
  if (flags & 0x8) at += 4 + size_t(Get32(material, at)) * 4;
  return at;
}

// Whether a converted trooper has the four materials laid out as this import knows them: the
// visor's glass blended and depth-sorted, its glow added and depth-sorted.
bool CheckLayout(const PrimeCmdl& model, const char* what, std::string* error) {
  MaterialSet set;
  if (model.materialSets.size() != 1 || !PortEchoesSuits::ParseSet(model.materialSets[0], set, error)) {
    return Fail(error, std::string(what) + ": not one material set");
  }
  if (set.materials.size() != 4) {
    return Fail(error, std::string(what) + ": " + std::to_string(set.materials.size()) + " materials, not 4");
  }
  const std::vector<uint8_t>& glass = set.materials[kGlassMaterial];
  const std::vector<uint8_t>& glow = set.materials[kGlowMaterial];
  if ((Get32(glass, 0) & 0x10) == 0 || (Get32(glow, 0) & 0x10) == 0 ||
      Get32(glow, BlendAt(glow)) != 0x00010001 || BlendAt(glass) + 4 > glass.size()) {
    return Fail(error, std::string(what) + ": its visor is not the glass and glow this import knows");
  }
  return true;
}

// The visor's glass drawn opaque: not depth-sorted, writing depth, blending ONE/ZERO. The glow still
// adds the visor's colour over it, so the visor shows as the marine's does, never the head through it.
bool OpaqueGlass(PrimeCmdl& model, std::string* error) {
  MaterialSet set;
  if (!PortEchoesSuits::ParseSet(model.materialSets[0], set, error)) return false;
  std::vector<uint8_t>& glass = set.materials[kGlassMaterial];
  Set32(glass, 0, (Get32(glass, 0) & ~0x10u) | 0x80u);
  Set32(glass, BlendAt(glass), 0x00000001);
  model.materialSets[0] = PortEchoesSuits::WriteSet(set);
  return true;
}

void SetBounds(PrimeCmdl& model) {
  const std::vector<uint8_t>& p = model.arrays[0];
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (size_t at = 0; at + 12 <= p.size(); at += 12) {
    for (int c = 0; c < 3; ++c) {
      const float v = GetF(p, at + size_t(c) * 4);
      lo[c] = std::min(lo[c], v);
      hi[c] = std::max(hi[c], v);
    }
  }
  if (lo[0] > hi[0]) return;
  for (int c = 0; c < 3; ++c) {
    SetF(model.header, 0x0C + size_t(c) * 4, lo[c]);
    SetF(model.header, 0x18 + size_t(c) * 4, hi[c]);
  }
}

V3 Position(const PrimeCmdl& model, uint32_t i) {
  const std::vector<uint8_t>& p = model.arrays[0];
  return {GetF(p, i * 12), GetF(p, i * 12 + 4), GetF(p, i * 12 + 8)};
}

// A converted trooper body: the model (every kind has the same vertices) and its skin on the kind's
// Prime skeleton, and its Echoes skin's weights per vertex (by Echoes joint).
struct Body {
  PrimeCmdl model;
  std::vector<uint8_t> skin;
  std::vector<Weights> echoesWeights;
};

// Echoes joints by id, and the ones under `root` (itself too).
std::set<uint32_t> Subtree(const std::vector<EchoesJoint>& joints, const std::string& root) {
  std::set<uint32_t> out;
  for (const EchoesJoint& j : joints) {
    if (j.name == root) out.insert(j.id);
  }
  for (bool grew = !out.empty(); grew;) {
    grew = false;
    for (const EchoesJoint& j : joints) {
      if (!out.count(j.id) && out.count(j.parent)) {
        out.insert(j.id);
        grew = true;
      }
    }
  }
  return out;
}

const EchoesJoint* FindJoint(const std::vector<EchoesJoint>& joints, const std::string& name) {
  for (const EchoesJoint& j : joints) {
    if (j.name == name) return &j;
  }
  return nullptr;
}

// The vertices the arm's first-person model keeps: weighing most on one of `arm`'s joints and at
// least `cut` out from the middle on its side (`side` +1 right, -1 left).
std::vector<uint8_t> ArmVertices(const Body& body, const std::set<uint32_t>& arm, int side, double cut) {
  const size_t count = body.model.arrays[0].size() / 12;
  std::vector<uint8_t> keep(count, 0);
  for (size_t v = 0; v < count && v < body.echoesWeights.size(); ++v) {
    const Weights& w = body.echoesWeights[v];
    if (w.empty()) continue;
    const auto main = std::max_element(w.begin(), w.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    keep[v] = arm.count(main->first) != 0 && Position(body.model, uint32_t(v)).x * side >= cut ? 1 : 0;
  }
  return keep;
}

// The first-person rifle of a trooper body: its right arm's triangles out to the shoulder joint
// (kRifleArmCut: the upper arm, the forearm, the hand, the rifle), turned down the gun's barrel
// (RifleFrame) with the muzzle at the origin, the upper arm straight back from the elbow as Samus's
// suit arm runs (0.46.0 tests 50-62 bent it 30 degrees up, toward the gun skeleton's shoulder joint;
// the user, comparing the two: "Samus' arm is not bent... but ours"); unskinned.
bool BuildRifle(const Body& body, const std::vector<EchoesJoint>& joints, PrimeCmdl& out, std::string* error) {
  const EchoesJoint* muzzle = FindJoint(joints, kMuzzleJoint);
  if (muzzle == nullptr) return Fail(error, "the rifle's muzzle is missing");
  std::set<uint32_t> arm = Subtree(joints, "R_shoulder");
  const std::vector<uint8_t> keep = ArmVertices(body, arm, +1, kRifleArmCut);
  out = body.model;
  if (!PortEchoesSuits::FloatNormals(out, error) ||
      !PortEchoesSuits::KeepTriangles(
          out, [&keep](uint32_t a, uint32_t b, uint32_t c) {
            return a < keep.size() && b < keep.size() && c < keep.size() && keep[a] && keep[b] && keep[c];
          },
          error)) {
    return false;
  }
  if (out.surfaces.empty()) return Fail(error, "no right arm triangles");
  const V3 origin = FromArray(muzzle->position);
  const size_t count = out.arrays[0].size() / 12;
  for (size_t v = 0; v < count; ++v) {
    const V3 p = Rifle(Position(out, uint32_t(v)) - origin);
    const V3 n = Rifle({GetF(out.arrays[1], v * 12), GetF(out.arrays[1], v * 12 + 4), GetF(out.arrays[1], v * 12 + 8)});
    SetF(out.arrays[0], v * 12, float(p.x));
    SetF(out.arrays[0], v * 12 + 4, float(p.y));
    SetF(out.arrays[0], v * 12 + 8, float(p.z));
    SetF(out.arrays[1], v * 12, float(n.x));
    SetF(out.arrays[1], v * 12 + 4, float(n.y));
    SetF(out.arrays[1], v * 12 + 8, float(n.z));
  }
  // Unskinned: the vertices the lists draw, kept with a skin of one group over them all.
  std::vector<uint8_t> skin = WriteSkin(std::vector<Weights>(count, Weights{{0u, 1.f}}));
  if (!PortEchoesSuits::CompactSkinned(out, skin, error)) return false;
  Set32(out.header, 8, Get32(out.header, 8) & ~0x1u);
  SetBounds(out);
  return PruneMaterials(out, error);
}

// The first-person left arm of a trooper body on Prime's grapple arm skeleton (`arm`, CINF 516F4445):
// its left arm's triangles past kArmCut, turned into the arm's frame (GrappleFrame) with the elbows
// on each other, each vertex moved by its joints' head offsets (so the wrist and fingers bend where
// Prime's do), the upper arm bent toward the arm skeleton's shoulder; skinned on the arm's joints of
// the same names (Prime's L_index_1 for Echoes' L_Index_1; a joint without one on its nearest
// ancestor's, the shoulder at last).
bool BuildGrappleArm(const Body& body, const std::vector<EchoesJoint>& joints, const PortDread::PrimeSkeleton& arm,
                     PrimeCmdl& out, std::vector<uint8_t>& skin, std::string* error) {
  const EchoesJoint* elbow = FindJoint(joints, "L_elbow");
  const EchoesJoint* shoulder = FindJoint(joints, "L_shoulder");
  const int armElbow = arm.Find("L_elbow"), armShoulder = arm.Find("L_shoulder");
  if (elbow == nullptr || shoulder == nullptr || armElbow < 0 || armShoulder < 0) {
    return Fail(error, "the left elbow or shoulder is missing");
  }
  // Each Echoes joint's arm joint, and the Echoes joint it was matched by (itself or the ancestor).
  std::map<uint32_t, const EchoesJoint*> byId;
  for (const EchoesJoint& j : joints) byId[j.id] = &j;
  struct Match {
    int armJoint = -1;
    const EchoesJoint* anchor = nullptr;
  };
  // Only the arm skeleton's shoulder and what hangs from it: a match above (its Skeleton_Root, the
  // trooper's spine) is the shoulder's too.
  std::set<uint32_t> armChain = {arm.joints[size_t(armShoulder)].id};
  for (bool grew = true; grew;) {
    grew = false;
    for (const auto& j : arm.joints) {
      if (!armChain.count(j.id) && armChain.count(j.parent)) {
        armChain.insert(j.id);
        grew = true;
      }
    }
  }
  std::map<uint32_t, Match> match;
  for (const EchoesJoint& j : joints) {
    const EchoesJoint* walk = &j;
    int found = -1;
    for (size_t depth = 0; walk != nullptr && depth <= joints.size(); ++depth) {
      for (size_t k = 0; k < arm.joints.size() && found < 0; ++k) {
        if (Lower(arm.joints[k].name) == Lower(walk->name)) found = int(k);
      }
      if (found >= 0) break;
      const auto parent = byId.find(walk->parent);
      walk = parent != byId.end() && parent->second != walk ? parent->second : nullptr;
    }
    const bool inChain = found >= 0 && armChain.count(arm.joints[size_t(found)].id) != 0;
    match[j.id] = inChain ? Match{found, walk} : Match{armShoulder, shoulder};
  }
  const std::set<uint32_t> left = Subtree(joints, "L_shoulder");
  const std::vector<uint8_t> keep = ArmVertices(body, left, -1, kArmCut);
  out = body.model;
  if (!PortEchoesSuits::FloatNormals(out, error)) return false;
  const V3 from = FromArray(elbow->position);
  const V3 to = FromArray(arm.joints[size_t(armElbow)].head);
  const auto place = [&](V3 p) { return Grapple(p - from) + to; };
  const V3 upperNow = Normalized(Grapple(FromArray(shoulder->position) - from));
  const V3 upperWant = Normalized(FromArray(arm.joints[size_t(armShoulder)].head) - to);
  V3 axis = Cross(upperNow, upperWant);
  const double angle = std::atan2(Length(axis), Dot(upperNow, upperWant));
  axis = Normalized(axis);
  const size_t count = out.arrays[0].size() / 12;
  std::vector<Weights> weights(count);
  for (size_t v = 0; v < count; ++v) {
    double bend = 0;
    V3 offset;
    std::map<uint32_t, float> merged;
    if (v < body.echoesWeights.size()) {
      for (const auto& [joint, weight] : body.echoesWeights[v]) {
        const auto m = match.find(joint);
        if (m == match.end()) continue;
        const int k = m->second.armJoint;
        merged[arm.joints[size_t(k)].id] += weight;
        if (k == armShoulder || m->second.anchor == nullptr) {
          bend += weight;
        } else {
          offset = offset + (FromArray(arm.joints[size_t(k)].head) - place(FromArray(m->second.anchor->position))) * weight;
        }
      }
    }
    if (merged.empty()) merged[arm.joints[size_t(armShoulder)].id] = 1.f;
    for (const auto& [joint, weight] : merged) weights[v].emplace_back(joint, weight);
    V3 p = place(Position(out, uint32_t(v)));
    V3 n = Grapple({GetF(out.arrays[1], v * 12), GetF(out.arrays[1], v * 12 + 4), GetF(out.arrays[1], v * 12 + 8)});
    if (bend > 0) {
      p = to + Rotate(p - to, axis, angle * bend);
      n = Rotate(n, axis, angle * bend);
    }
    p = p + offset;
    SetF(out.arrays[0], v * 12, float(p.x));
    SetF(out.arrays[0], v * 12 + 4, float(p.y));
    SetF(out.arrays[0], v * 12 + 8, float(p.z));
    SetF(out.arrays[1], v * 12, float(n.x));
    SetF(out.arrays[1], v * 12 + 4, float(n.y));
    SetF(out.arrays[1], v * 12 + 8, float(n.z));
  }
  skin = WriteSkin(weights);
  if (!PortEchoesSuits::KeepTriangles(
          out, [&keep](uint32_t a, uint32_t b, uint32_t c) {
            return a < keep.size() && b < keep.size() && c < keep.size() && keep[a] && keep[b] && keep[c];
          },
          error)) {
    return false;
  }
  if (out.surfaces.empty()) return Fail(error, "no left arm triangles");
  if (!PortEchoesSuits::CompactSkinned(out, skin, error)) return false;
  SetBounds(out);
  return PruneMaterials(out, error);
}

// A rigid motion p -> r p + t, for posing joints.
struct Rigid {
  double r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  V3 t;
  V3 Turn(V3 v) const {
    return {r[0][0] * v.x + r[0][1] * v.y + r[0][2] * v.z, r[1][0] * v.x + r[1][1] * v.y + r[1][2] * v.z,
            r[2][0] * v.x + r[2][1] * v.y + r[2][2] * v.z};
  }
  V3 Apply(V3 p) const { return Turn(p) + t; }
};

// The turn by `angle` (radians) about the unit `axis` through `head`.
Rigid About(V3 head, V3 axis, double angle) {
  Rigid m;
  const V3 cols[3] = {Rotate({1, 0, 0}, axis, angle), Rotate({0, 1, 0}, axis, angle), Rotate({0, 0, 1}, axis, angle)};
  for (int c = 0; c < 3; ++c) {
    m.r[0][c] = cols[c].x;
    m.r[1][c] = cols[c].y;
    m.r[2][c] = cols[c].z;
  }
  m.t = head - m.Turn(head);
  return m;
}

// `a` after `b`: p -> a(b(p)).
Rigid Then(const Rigid& a, const Rigid& b) {
  Rigid m;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) m.r[i][j] = a.r[i][0] * b.r[0][j] + a.r[i][1] * b.r[1][j] + a.r[i][2] * b.r[2][j];
  }
  m.t = a.Turn(b.t) + a.t;
  return m;
}

// The first-person left hand on the rifle's front grip (the barred guard under its muzzle), a static
// model in the rifle's space (BuildRifle's: muzzle at the origin, barrel +y). The trooper's left arm out
// to the shoulder joint, posed by linear blend skinning on its Echoes skin: the fingers closed toward the
// palm about the knuckle line, the thumb swung under, the wrist bent so the arm runs straight back
// toward the left shoulder (kGripBend). Then placed with the gripped point on the guard's middle, the
// knuckle line along the guard (index finger at its top) and the palm on its left. Unskinned; the gun
// draws it with the rifle while Samus's left hand would rest on her cannon (CGunWeapon::PortSetLeftGrip).
bool BuildLeftGrip(const Body& body, const std::vector<EchoesJoint>& joints, PrimeCmdl& out, std::string* error) {
  const EchoesJoint* elbow = FindJoint(joints, "L_elbow");
  const EchoesJoint* wrist = FindJoint(joints, "L_wrist");
  if (elbow == nullptr || wrist == nullptr) return Fail(error, "the left elbow or wrist is missing");
  // The T-posed hand lies flat, its fingers out along -x, its thumb forward (+y), its palm down (-z).
  // Each finger's joints close about the knuckle line (-y), the second on the first.
  const V3 knuckleLine{0, -1, 0};
  std::map<uint32_t, Rigid> pose;
  V3 knuckles;
  for (const char* finger : {"Index", "middle", "Ring", "Pinky"}) {
    const EchoesJoint* j1 = FindJoint(joints, std::string("L_") + finger + "_1");
    const EchoesJoint* j2 = FindJoint(joints, std::string("L_") + finger + "_2");
    if (j1 == nullptr || j2 == nullptr) return Fail(error, std::string("the left hand's ") + finger + " finger is missing");
    const Rigid first = About(FromArray(j1->position), knuckleLine, kGripCurl1 * kPi / 180.0);
    pose[j1->id] = first;
    pose[j2->id] = Then(first, About(FromArray(j2->position), knuckleLine, kGripCurl2 * kPi / 180.0));
    knuckles = knuckles + FromArray(j1->position) * 0.25;
  }
  const EchoesJoint* thumb1 = FindJoint(joints, "L_thumb_1");
  const EchoesJoint* thumb2 = FindJoint(joints, "L_thumb_2");
  if (thumb1 == nullptr || thumb2 == nullptr) return Fail(error, "the left thumb is missing");
  const Rigid thumb = About(FromArray(thumb1->position), {1, 0, 0}, kGripThumb * kPi / 180.0);
  pose[thumb1->id] = thumb;
  pose[thumb2->id] = Then(thumb, About(FromArray(thumb2->position), {0, 0, 1}, kGripThumbTip * kPi / 180.0));
  std::set<uint32_t> kept = {elbow->id, wrist->id};
  for (const auto& entry : pose) kept.insert(entry.first);
  const EchoesJoint* shoulder = FindJoint(joints, "L_shoulder");
  if (shoulder != nullptr) kept.insert(shoulder->id);
  // The hand's frame (knuckle line u1 index to pinky, palm normal u2, toward the wrist u3) onto the
  // guard's (its axis a, the palm's side n, the unbent forearm f), the gripped point onto its middle.
  const V3 u1{0, -1, 0}, u2{0, 0, -1}, u3{1, 0, 0};
  const V3 gripPoint = knuckles + V3{kGripReach, 0, -kGripDepth};
  const V3 a = Normalized(kGripAxis);
  const V3 f = Normalized(kGripForearm - a * Dot(kGripForearm, a));
  const V3 n = Cross(f, a);
  const auto place = [&](V3 p) {
    const V3 d = p - gripPoint;
    return kGripCentre + a * Dot(d, u1) + n * Dot(d, u2) + f * Dot(d, u3);
  };
  const auto placeDirection = [&](V3 v) { return a * Dot(v, u1) + n * Dot(v, u2) + f * Dot(v, u3); };
  // The wrist's bend: the forearm (the elbow's weights, and the upper arm's at its end) turned about the
  // wrist from its own way (u3) to kGripBend's, the hand staying on the guard.
  const V3 want = Normalized(kGripBend);
  const V3 wantHere = u1 * Dot(want, a) + u2 * Dot(want, n) + u3 * Dot(want, f);
  const V3 bendAxis = Cross(u3, wantHere);
  const double bend = std::atan2(Length(bendAxis), Dot(u3, wantHere));
  if (bend > 1e-6) {
    const Rigid forearm = About(FromArray(wrist->position), Normalized(bendAxis), bend);
    for (const char* name : {"L_elbow", "L_shoulder", "L_varia_SDK"}) {
      if (const EchoesJoint* j = FindJoint(joints, name)) pose[j->id] = forearm;
    }
  }
  const size_t count = body.model.arrays[0].size() / 12;
  std::vector<uint8_t> keep(count, 0);
  for (size_t v = 0; v < count && v < body.echoesWeights.size(); ++v) {
    const Weights& w = body.echoesWeights[v];
    if (w.empty()) continue;
    const auto main = std::max_element(w.begin(), w.end(), [](const auto& x, const auto& y) { return x.second < y.second; });
    keep[v] = kept.count(main->first) != 0 && Position(body.model, uint32_t(v)).x < -kRifleArmCut ? 1 : 0;
  }
  out = body.model;
  if (!PortEchoesSuits::FloatNormals(out, error) ||
      !PortEchoesSuits::KeepTriangles(
          out, [&keep](uint32_t x, uint32_t y, uint32_t z) {
            return x < keep.size() && y < keep.size() && z < keep.size() && keep[x] && keep[y] && keep[z];
          },
          error)) {
    return false;
  }
  if (out.surfaces.empty()) return Fail(error, "no left hand triangles");
  const size_t posed = std::min(out.arrays[0].size() / 12, out.arrays[1].size() / 12);
  for (size_t v = 0; v < posed; ++v) {
    const V3 p0 = Position(out, uint32_t(v));
    const V3 n0{GetF(out.arrays[1], v * 12), GetF(out.arrays[1], v * 12 + 4), GetF(out.arrays[1], v * 12 + 8)};
    V3 p, nv;
    double total = 0;
    if (v < body.echoesWeights.size()) {
      for (const auto& [joint, weight] : body.echoesWeights[v]) {
        const auto it = pose.find(joint);
        p = p + (it != pose.end() ? it->second.Apply(p0) : p0) * weight;
        nv = nv + (it != pose.end() ? it->second.Turn(n0) : n0) * weight;
        total += weight;
      }
    }
    if (total <= 0) {
      p = p0;
      nv = n0;
    }
    p = place(p);
    nv = Normalized(placeDirection(nv));
    SetF(out.arrays[0], v * 12, float(p.x));
    SetF(out.arrays[0], v * 12 + 4, float(p.y));
    SetF(out.arrays[0], v * 12 + 8, float(p.z));
    SetF(out.arrays[1], v * 12, float(nv.x));
    SetF(out.arrays[1], v * 12 + 4, float(nv.y));
    SetF(out.arrays[1], v * 12 + 8, float(nv.z));
  }
  // Unskinned: the vertices the lists draw, kept with a skin of one group over them all.
  std::vector<uint8_t> skin = WriteSkin(std::vector<Weights>(posed, Weights{{0u, 1.f}}));
  if (!PortEchoesSuits::CompactSkinned(out, skin, error)) return false;
  Set32(out.header, 8, Get32(out.header, 8) & ~0x1u);
  SetBounds(out);
  return PruneMaterials(out, error);
}

// A body's hand and rifle (the right wrist's vertices, by their weight on it) turned kGripRollDegrees
// about the forearm, through the wrist: the forearm stays. Only for the bodies Samus's animations
// pose; the first-person rifle is modelled upright as it is.
bool RollGrip(Body& body, const std::vector<EchoesJoint>& joints, std::string* error) {
  const EchoesJoint* elbow = FindJoint(joints, "R_elbow");
  const EchoesJoint* wrist = FindJoint(joints, "R_wrist");
  if (elbow == nullptr || wrist == nullptr) return Fail(error, "the right elbow or wrist is missing");
  const std::set<uint32_t> hand = Subtree(joints, "R_wrist");
  const V3 pivot = FromArray(wrist->position);
  const V3 axis = Normalized(pivot - FromArray(elbow->position));
  const double angle = kGripRollDegrees * 3.14159265358979323846 / 180.0;
  PrimeCmdl& m = body.model;
  if (m.arrays.size() < 2 || (Get32(m.header, 8) & 0x2) != 0) return Fail(error, "the body has no float normals");
  const size_t count = std::min(m.arrays[0].size() / 12, m.arrays[1].size() / 12);
  for (size_t v = 0; v < count && v < body.echoesWeights.size(); ++v) {
    double w = 0;
    for (const auto& [joint, weight] : body.echoesWeights[v]) {
      if (hand.count(joint)) w += weight;
    }
    if (w <= 0) continue;
    const V3 p = pivot + Rotate(Position(m, uint32_t(v)) - pivot, axis, angle * w);
    const V3 n = Rotate({GetF(m.arrays[1], v * 12), GetF(m.arrays[1], v * 12 + 4), GetF(m.arrays[1], v * 12 + 8)}, axis,
                        angle * w);
    SetF(m.arrays[0], v * 12, float(p.x));
    SetF(m.arrays[0], v * 12 + 4, float(p.y));
    SetF(m.arrays[0], v * 12 + 8, float(p.z));
    SetF(m.arrays[1], v * 12, float(n.x));
    SetF(m.arrays[1], v * 12 + 4, float(n.y));
    SetF(m.arrays[1], v * 12 + 8, float(n.z));
  }
  return true;
}

// The Dark Trooper's broken visor (its glass and glow surfaces) out, the marine's whole one in: the
// marine's glass and glow surfaces merged into the body (MergeCmdl, the marine's opaque glass and its
// glow with them), the skins' groups one after the other (a group over the body's padding between
// them), then only the vertices the lists draw, and only the materials they use.
bool SwapVisor(PrimeCmdl& body, std::vector<uint8_t>& skin, const PrimeCmdl& donor, const std::vector<uint8_t>& donorSkin,
               std::string* error) {
  const auto visor = [](const std::vector<uint8_t>& s) {
    const uint32_t m = SurfaceMaterial(s);
    return m == kGlassMaterial || m == kGlowMaterial;
  };
  PrimeCmdl part = donor;
  part.surfaces.erase(std::remove_if(part.surfaces.begin(), part.surfaces.end(), [&](const auto& s) { return !visor(s); }),
                      part.surfaces.end());
  const size_t before = body.surfaces.size();
  body.surfaces.erase(std::remove_if(body.surfaces.begin(), body.surfaces.end(), visor), body.surfaces.end());
  if (part.surfaces.empty() || body.surfaces.size() == before) return Fail(error, "no visor surfaces to swap");
  if (!PortEchoesSuits::FloatNormals(body, error) || !PortEchoesSuits::FloatNormals(part, error)) return false;
  const size_t bodyVertices = body.arrays[0].size() / 12;
  if (body.arrays[1].size() / 12 != bodyVertices) return Fail(error, "the body's positions and normals differ in count");
  std::vector<uint8_t> bodyGroups, donorGroups;
  uint32_t bodyCount, bodyCovered, donorCount, donorCovered;
  std::vector<uint32_t> bodyJoints;
  if (!PortEchoesSuits::SkinGroups(skin, bodyGroups, bodyCount, bodyCovered, error) ||
      !PortEchoesSuits::SkinGroups(donorSkin, donorGroups, donorCount, donorCovered, error) ||
      !PortEchoesSuits::MainJoints(skin, bodyJoints, error)) {
    return false;
  }
  if (bodyCovered > bodyVertices || bodyJoints.empty()) return Fail(error, "the body's skin does not fit it");
  if (!PortEchoesSuits::MergeCmdl(body, part, error)) return false;
  const uint32_t pad = uint32_t(bodyVertices - bodyCovered);
  std::vector<uint8_t> out;
  Put32(out, bodyCount + (pad != 0 ? 1 : 0) + donorCount);
  out.insert(out.end(), bodyGroups.begin(), bodyGroups.end());
  if (pad != 0) {
    Put32(out, 1);
    Put32(out, bodyJoints.front());
    Put32(out, 0x3F800000);
    Put32(out, pad);
  }
  out.insert(out.end(), donorGroups.begin(), donorGroups.end());
  const uint32_t total = uint32_t(bodyVertices) + donorCovered;
  Put32(out, 0xFFFFFFFF);
  Put32(out, total);
  Put32(out, 0xFFFFFFFF);
  Put32(out, total);
  skin = std::move(out);
  return PortEchoesSuits::CompactSkinned(body, skin, error) && PruneMaterials(body, error);
}

// A decoded CMPR texture, and its texels' votes.
struct Texture {
  int width = 0, height = 0, mips = 1;
  std::vector<uint8_t> rgba;
  PortEchoesSuits::TexelVotes votes;
  std::vector<uint8_t> visor;  // texels the visor's glow covers
};

bool Decode(const std::vector<uint8_t>& data, Texture& t) {
  if (!PortEchoesSuits::DecodeCmpr(data, t.width, t.height, t.rgba)) return false;
  t.mips = int(std::max<uint32_t>(Get32(data, 8), 1));
  t.votes.width = t.width;
  t.votes.height = t.height;
  t.votes.limb.assign(size_t(t.width) * t.height, 0);
  t.votes.other = t.votes.limb;
  t.votes.panelSide.assign(size_t(t.width) * t.height, 0);
  t.visor.assign(size_t(t.width) * t.height, 0);
  return true;
}

// The texels a triangle covers (generous at the edges, as the Echoes recolours' votes).
void Mark(std::vector<uint8_t>& grid, int w, int h, const float (&uv)[3][2]) {
  PortEchoesSuits::TexelVotes votes;
  votes.width = w;
  votes.height = h;
  votes.limb.assign(size_t(w) * h, 0);
  votes.other = votes.limb;
  PortEchoesSuits::VoteTriangle(votes, uv, true);
  for (size_t k = 0; k < grid.size(); ++k) {
    if (votes.limb[k]) grid[k] = 1;
  }
}

// The median of a set of values (0 for none).
double Median(std::vector<double> values) {
  if (values.empty()) return 0;
  std::nth_element(values.begin(), values.begin() + std::ptrdiff_t(values.size() / 2), values.end());
  return values[values.size() / 2];
}

bool IsTeal(double h, double s, double v) { return h >= 150.0 && h < 210.0 && s > 0.3 && v > 0.12; }

// The marine's texture in the Gravity Suit's colours: on the limbs, the white plates (texels of low
// saturation) in Prime's Gravity purple at a value scaled by `scale` (the plates' median to its
// purple's); the visor's glow in its visor's aqua, brighter as its glass is; the teal lights aqua.
bool RecolourGravity(Texture& t, const std::vector<uint8_t>& limb, double scale) {
  bool changed = false;
  for (size_t k = 0; k * 4 + 3 < t.rgba.size(); ++k) {
    uint8_t* c = &t.rgba[k * 4];
    double h, s, v;
    PortEchoesSuits::RgbToHsv(c, h, s, v);
    uint8_t next[3] = {c[0], c[1], c[2]};
    if (t.visor[k] && IsTeal(h, s, v)) {
      PortEchoesSuits::HsvToRgb(kGravityVisorHue, kGravityVisorSat, v * kGravityVisorValue / kMarineVisorValue, next);
    } else if (IsTeal(h, s, v)) {
      PortEchoesSuits::HsvToRgb(kGravityVisorHue, std::max(s, kGravityVisorSat), v, next);
    } else if (k < limb.size() && limb[k] && s < 0.30) {
      PortEchoesSuits::HsvToRgb(kGravityHue, kGravitySat, v * scale, next);
    }
    if (std::memcmp(next, c, 3) != 0) {
      std::memcpy(c, next, 3);
      changed = true;
    }
  }
  return changed;
}

// A ball look's colours: its shell (armour) and its lights.
struct BallPalette {
  double armourHue, armourSat, armourValue;  // the shell's median goes to this
  bool hueFollows;                           // the shell's hue spread kept (a third of it)
  bool lights;                               // its lights recoloured to lightHue
  double lightHue, lightSatMax;
};

// Prime's ball texture in a look's colours: the shell (yellow to orange, 13 to 70 degrees) at the
// palette's armour colour, its median there and its shading kept; the lights (red, below 13 degrees;
// the low-poly spider ball's green, 70 to 110) at the palette's light hue. Black stays.
bool RecolourBall(Texture& t, const BallPalette& p) {
  std::vector<double> hues, sats, values;
  for (size_t k = 0; k * 4 + 3 < t.rgba.size(); ++k) {
    double h, s, v;
    PortEchoesSuits::RgbToHsv(&t.rgba[k * 4], h, s, v);
    if (h >= 13.0 && h < 70.0 && s > 0.25 && v > 0.12) {
      hues.push_back(h);
      sats.push_back(s);
      values.push_back(v);
    }
  }
  const double mh = Median(hues), ms = Median(sats), mv = Median(values);
  bool changed = false;
  for (size_t k = 0; k * 4 + 3 < t.rgba.size(); ++k) {
    uint8_t* c = &t.rgba[k * 4];
    double h, s, v;
    PortEchoesSuits::RgbToHsv(c, h, s, v);
    uint8_t next[3] = {c[0], c[1], c[2]};
    if (v <= 0.12) continue;
    if (h >= 13.0 && h < 70.0 && s > 0.25 && ms > 0 && mv > 0) {
      const double hue = p.armourHue + (p.hueFollows ? 0.35 * (h - mh) : 0.0);
      PortEchoesSuits::HsvToRgb(hue, p.armourSat * std::min(1.5, s / ms), std::min(1.0, v * p.armourValue / mv), next);
    } else if (p.lights && s > 0.45 && (h < 13.0 || h >= 330.0 || (h >= 70.0 && h < 110.0))) {
      PortEchoesSuits::HsvToRgb(p.lightHue, std::min(s, p.lightSatMax), v, next);
    }
    if (std::memcmp(next, c, 3) != 0) {
      std::memcpy(c, next, 3);
      changed = true;
    }
  }
  return changed;
}

}  // namespace

void RifleFrame(const float (&in)[3], float (&out)[3]) {
  out[0] = -in[1];
  out[1] = in[0];
  out[2] = in[2];
}

void GrappleFrame(const float (&in)[3], float (&out)[3]) {
  out[0] = -in[2];
  out[1] = -in[0];
  out[2] = in[1];
}

bool PruneMaterials(PrimeCmdl& model, std::string* error) {
  if (model.materialSets.empty()) return Fail(error, "model has no material set");
  std::vector<MaterialSet> sets(model.materialSets.size());
  for (size_t i = 0; i < sets.size(); ++i) {
    if (!PortEchoesSuits::ParseSet(model.materialSets[i], sets[i], error)) return false;
    if (sets[i].materials.size() != sets[0].materials.size()) return Fail(error, "material sets differ in size");
  }
  std::vector<int> used(sets[0].materials.size(), -1);
  for (const auto& surface : model.surfaces) {
    const uint32_t m = SurfaceMaterial(surface);
    if (m >= used.size()) return Fail(error, "surface material out of range");
    used[m] = 0;
  }
  int next = 0;
  for (int& slot : used) {
    if (slot == 0) slot = next++;
  }
  for (auto& surface : model.surfaces) Set32(surface, 0x0C, uint32_t(used[SurfaceMaterial(surface)]));
  for (size_t i = 0; i < sets.size(); ++i) {
    MaterialSet kept;
    for (size_t m = 0; m < sets[i].materials.size(); ++m) {
      if (used[m] < 0) continue;
      std::vector<uint8_t> material = sets[i].materials[m];
      const uint32_t ntex = Get32(material, 4);
      for (uint32_t t = 0; t < ntex; ++t) {
        const uint32_t index = Get32(material, 8 + size_t(t) * 4);
        if (index >= sets[i].textures.size()) return Fail(error, "material texture out of range");
        const uint32_t id = sets[i].textures[index];
        auto it = std::find(kept.textures.begin(), kept.textures.end(), id);
        if (it == kept.textures.end()) it = kept.textures.insert(kept.textures.end(), id);
        Set32(material, 8 + size_t(t) * 4, uint32_t(it - kept.textures.begin()));
      }
      kept.materials.push_back(std::move(material));
    }
    model.materialSets[i] = PortEchoesSuits::WriteSet(kept);
  }
  return true;
}

bool Import(const Io& io, Result& result, std::string& error) {
  const auto log = [&io](const std::string& line) {
    if (io.log) io.log(line);
  };
  const auto cancelled = [&io, &error] {
    if (io.cancelled && io.cancelled()) {
      error = "Cancelled.";
      return true;
    }
    return false;
  };
  std::set<std::string> written;
  const auto write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    if (!io.write(name, data)) {
      error = "Could not write " + name + ".";
      return false;
    }
    if (written.insert(name).second) result.files.push_back(name);
    return true;
  };
  // Every output id is new: never one of Prime's.
  const auto fresh = [&](uint32_t id, const std::string& what) {
    if (id == 0 || io.retailId(id)) {
      error = what + ": output id " + Hex8(id) + " is a Prime resource.";
      return false;
    }
    return true;
  };
  const PortEchoesSuits::SuitResources& varia = PortEchoesSuits::kSuitResources[PortEchoesSuits::kVariaSuit];
  constexpr size_t kSteps = 8;
  size_t step = 0;
  const auto progress = [&] {
    if (io.progress) io.progress(step, kSteps);
    ++step;
  };

  // The skeletons: the troopers' (Echoes) and Prime's of each kind, and the gun's.
  progress();
  std::map<uint32_t, std::vector<EchoesJoint>> echoesCinfs;
  for (const Trooper* t : {&kMarineTrooper, &kDarkTrooperModel}) {
    std::vector<uint8_t> data;
    std::string why;
    if (!io.echoes(kCINF, t->cinf, data)) {
      error = "The Echoes disc has no " + std::string(t->name) + " skeleton (" + Hex8(t->cinf) +
              "): is it Metroid Prime 2: Echoes (USA)?";
      return false;
    }
    if (!PortEchoesSuits::ParseCinf(data.data(), data.size(), echoesCinfs[t->cinf], &why)) {
      error = std::string(t->name) + " skeleton " + Hex8(t->cinf) + ": " + why;
      return false;
    }
  }
  std::map<uint32_t, PortDread::PrimeSkeleton> primeCinfs;
  for (const uint32_t id : {kKindCinf[0], kKindCinf[1], kKindCinf[2], kKindCinf[3]}) {
    std::vector<uint8_t> data;
    std::string why;
    if (!io.retail(kCINF, id, data) || !PortDread::ParseCinf(data.data(), data.size(), primeCinfs[id], &why)) {
      error = "Prime's skeleton " + Hex8(id) + " is missing or unreadable" + (why.empty() ? "" : ": " + why);
      return false;
    }
  }

  // Both troopers converted onto each body kind's skeleton (the left arm's is built below), the
  // visor's glass made opaque.
  progress();
  std::set<uint32_t> echoesTextures;
  std::map<const Trooper*, std::array<Body, 3>> bodies;
  for (const Trooper* t : {&kMarineTrooper, &kDarkTrooperModel}) {
    std::vector<uint8_t> cmdl, cskr;
    if (!io.echoes(kCMDL, t->cmdl, cmdl) || !io.echoes(kCSKR, t->cskr, cskr)) {
      error = "The Echoes disc has no " + std::string(t->name) + " (" + Hex8(t->cmdl) +
              "): is it Metroid Prime 2: Echoes (USA)?";
      return false;
    }
    std::vector<Weights> echoesWeights;
    std::string why;
    if (!VertexWeights(cskr, echoesWeights, &why)) {
      error = std::string(t->name) + " skin " + Hex8(t->cskr) + ": " + why;
      return false;
    }
    for (int kind = 0; kind < 3; ++kind) {
      if (cancelled()) return false;
      Body& body = bodies[t][size_t(kind)];
      std::vector<uint32_t> textures;
      std::vector<std::string> folded;
      if (!PortEchoesSuits::ConvertCmdl(cmdl, body.model, textures, &why) || !CheckLayout(body.model, t->name, &why) ||
          !OpaqueGlass(body.model, &why) || !PortEchoesSuits::FloatNormals(body.model, &why) ||
          !PortEchoesSuits::ConvertCskr(cskr, echoesCinfs[t->cinf], primeCinfs[kKindCinf[kind]], body.skin, &folded,
                                        &why)) {
        error = std::string(t->name) + " (" + kKindNames[kind] + "): " + why;
        return false;
      }
      body.echoesWeights = echoesWeights;
      if (kind == 0) {
        std::string line = std::string("marine: ") + t->name + ", " + std::to_string(textures.size()) + " textures";
        if (!folded.empty()) {
          line += ", folded:";
          for (const std::string& f : folded) line += " " + f;
        }
        log(line);
      }
    }
  }

  // The looks' bodies: the marine's; the Dark Trooper's with the marine's visor.
  progress();
  std::array<std::array<Body, 3>, kLookCount> looks;
  for (int kind = 0; kind < 3; ++kind) {
    // The hand and rifle upright in Samus's poses (RollGrip), before the visor swap renumbers the
    // Dark Trooper's vertices.
    Body marine = bodies[&kMarineTrooper][size_t(kind)];
    Body dark = bodies[&kDarkTrooperModel][size_t(kind)];
    std::string why;
    if (!RollGrip(marine, echoesCinfs[kMarineTrooper.cinf], &why) ||
        !RollGrip(dark, echoesCinfs[kDarkTrooperModel.cinf], &why)) {
      error = std::string("the bodies' grip (") + kKindNames[kind] + "): " + why;
      return false;
    }
    if (!SwapVisor(dark.model, dark.skin, marine.model, marine.skin, &why)) {
      error = std::string("Dark Trooper with the marine's visor (") + kKindNames[kind] + "): " + why;
      return false;
    }
    looks[kMarine][size_t(kind)] = marine;
    looks[kGravityMarine][size_t(kind)] = marine;
    looks[kDarkTrooper][size_t(kind)] = std::move(dark);
    // The pause screen's doll draws a depth pass first: the sorted glow writes none there.
    for (auto& look : looks) {
      if (kind == 2 && !PortEchoesSuits::SortedWithoutDepth(look[2].model, &why)) {
        error = std::string("the pause screen body: ") + why;
        return false;
      }
    }
  }

  // The first-person models of each trooper (built from the converted, unswapped bodies: the visor is
  // nowhere near the arms).
  progress();
  struct FirstPerson {
    PrimeCmdl rifle, arm, grip;
    std::vector<uint8_t> armSkin;
  };
  std::map<const Trooper*, FirstPerson> firstPerson;
  for (const Trooper* t : {&kMarineTrooper, &kDarkTrooperModel}) {
    if (cancelled()) return false;
    std::string why;
    FirstPerson& fp = firstPerson[t];
    if (!BuildRifle(bodies[t][0], echoesCinfs[t->cinf], fp.rifle, &why)) {
      error = std::string(t->name) + " first-person rifle: " + why;
      return false;
    }
    if (!BuildGrappleArm(bodies[t][0], echoesCinfs[t->cinf], primeCinfs[kKindCinf[3]], fp.arm, fp.armSkin, &why)) {
      error = std::string(t->name) + " first-person left arm: " + why;
      return false;
    }
    if (!BuildLeftGrip(bodies[t][0], echoesCinfs[t->cinf], fp.grip, &why)) {
      error = std::string(t->name) + " first-person left hand on the rifle: " + why;
      return false;
    }
    log(std::string("marine: ") + t->name + " first person: rifle " + std::to_string(fp.rifle.arrays[0].size() / 12) +
        " vertices, left arm " + std::to_string(fp.arm.arrays[0].size() / 12) + ", left hand on the rifle " +
        std::to_string(fp.grip.arrays[0].size() / 12));
  }

  // The Gravity marine's textures: the marine's, its texels voted limb or not by the body's triangles
  // over them (by the joints their vertices weigh most on: the shoulder pads, arms, hands, thighs,
  // knees, shins and feet, as Prime's Gravity Suit paints them), the visor's by its glow's triangles.
  progress();
  std::map<uint32_t, uint32_t> gravityTextures;
  {
    const Body& marine = bodies[&kMarineTrooper][0];
    std::vector<uint32_t> joints;
    std::string why;
    if (!PortEchoesSuits::MainJoints(marine.skin, joints, &why)) {
      error = "the marine's skin: " + why;
      return false;
    }
    std::map<uint32_t, bool> limbJoint;
    std::set<uint32_t> hipJoints;
    double hipHeight = 1e30;
    for (const auto& j : primeCinfs[kKindCinf[0]].joints) {
      limbJoint[j.id] = PortEchoesSuits::IsLimbJoint(j.name);
      if (j.name == "L_hip" || j.name == "R_hip") {
        hipJoints.insert(j.id);
        hipHeight = std::min(hipHeight, double(j.head[2]));
      }
    }
    // The body's triangles, and which hip ones are the hip guards: the hips' texture pieces (their
    // triangles joined through shared UVs) reaching above the hip joints belong to the waist, not the
    // thighs (the user circled the thigh plates from below them down).
    struct Tri {
      std::vector<uint32_t> ids;
      float uv[3][2];
      uint32_t pos[3];
      bool limb, hip;
    };
    std::vector<Tri> tris;
    std::vector<std::pair<std::vector<uint32_t>, std::array<std::array<float, 2>, 3>>> glows;
    const bool listed = PortEchoesSuits::ForEachMaterialUvTriangle(
        marine.model,
        [&](uint32_t material, const std::vector<uint32_t>& ids, const float (&uv)[3][2], const uint32_t (&pos)[3]) {
          if (material == kGlowMaterial) {
            glows.push_back({ids, {{{uv[0][0], uv[0][1]}, {uv[1][0], uv[1][1]}, {uv[2][0], uv[2][1]}}}});
            return;
          }
          if (material != kBodyMaterial) return;
          Tri t;
          // Its colour (slot 0) and glow (slot 1); not its reflection's mask or sphere map.
          t.ids.assign(ids.begin(), ids.begin() + std::ptrdiff_t(std::min<size_t>(ids.size(), 2)));
          int limbs = 0, hips = 0;
          for (int i = 0; i < 3; ++i) {
            t.uv[i][0] = uv[i][0];
            t.uv[i][1] = uv[i][1];
            t.pos[i] = pos[i];
            const uint32_t joint = pos[i] < joints.size() ? joints[pos[i]] : 0;
            const auto found = limbJoint.find(joint);
            limbs += found != limbJoint.end() && found->second ? 1 : 0;
            hips += hipJoints.count(joint) != 0 ? 1 : 0;
          }
          t.limb = limbs >= 2;
          t.hip = hips >= 2;
          tris.push_back(std::move(t));
        },
        &why);
    if (!listed) {
      error = "the marine's textures: " + why;
      return false;
    }
    {
      std::vector<size_t> parent(tris.size());
      for (size_t i = 0; i < parent.size(); ++i) parent[i] = i;
      // Iterative (the import's thread has a fixed stack on the Vita), halving the path as it goes.
      const auto find = [&parent](size_t i) {
        while (parent[i] != i) {
          parent[i] = parent[parent[i]];
          i = parent[i];
        }
        return i;
      };
      std::map<std::pair<uint32_t, uint32_t>, size_t> byUv;
      for (size_t i = 0; i < tris.size(); ++i) {
        if (!tris[i].hip) continue;
        for (const auto& uv : tris[i].uv) {
          uint32_t u, v;
          std::memcpy(&u, &uv[0], 4);
          std::memcpy(&v, &uv[1], 4);
          const auto [it, added] = byUv.emplace(std::pair{u, v}, i);
          if (!added) parent[find(i)] = find(it->second);
        }
      }
      std::map<size_t, double> top;
      for (size_t i = 0; i < tris.size(); ++i) {
        if (!tris[i].hip) continue;
        double& z = top.try_emplace(find(i), -1e30).first->second;
        for (const uint32_t p : tris[i].pos) z = std::max(z, Position(marine.model, p).z);
      }
      size_t guards = 0;
      for (size_t i = 0; i < tris.size(); ++i) {
        if (tris[i].hip && tris[i].limb && top[find(i)] > hipHeight) {
          tris[i].limb = false;
          ++guards;
        }
      }
      log("marine: Gravity marine: " + std::to_string(guards) + " hip guard triangles keep the waist's colour");
    }
    std::map<uint32_t, Texture> maps;
    std::set<uint32_t> notCmpr;
    const auto use = [&](uint32_t id) -> Texture* {
      if (notCmpr.count(id)) return nullptr;
      const auto found = maps.find(id);
      if (found != maps.end()) return &found->second;
      std::vector<uint8_t> data;
      Texture t;
      if (!io.echoes(kTXTR, id, data) || !Decode(data, t)) {
        notCmpr.insert(id);
        return nullptr;
      }
      return &maps.emplace(id, std::move(t)).first->second;
    };
    for (const Tri& tri : tris) {
      for (const uint32_t id : tri.ids) {
        if (Texture* t = use(id)) PortEchoesSuits::VoteTriangle(t->votes, tri.uv, tri.limb);
      }
    }
    for (const auto& [ids, corners] : glows) {
      const float uv[3][2] = {{corners[0][0], corners[0][1]}, {corners[1][0], corners[1][1]}, {corners[2][0], corners[2][1]}};
      for (const uint32_t id : ids) {
        if (Texture* t = use(id)) Mark(t->visor, t->width, t->height, uv);
      }
    }
    // The plates' value (the median of the limbs' unsaturated, lit texels) to the Gravity purple's.
    std::vector<double> plates;
    for (auto& [id, t] : maps) {
      const std::vector<uint8_t> limb = PortEchoesSuits::LimbTexels(t.votes);
      for (size_t k = 0; k < limb.size(); ++k) {
        double h, s, v;
        PortEchoesSuits::RgbToHsv(&t.rgba[k * 4], h, s, v);
        if (limb[k] && s < 0.30 && v > 0.45) plates.push_back(v);
      }
    }
    const double plate = Median(plates);
    const double scale = plate > 0 ? kGravityValue / plate : 1.0;
    for (auto& [id, t] : maps) {
      const std::vector<uint8_t> limb = PortEchoesSuits::LimbTexels(t.votes);
      if (!RecolourGravity(t, limb, scale)) continue;
      const uint32_t out = Wear(kGravityMarine, id);
      if (!fresh(out, "Gravity marine texture " + Hex8(id)) ||
          !write(Hex8(out) + ".TXTR", PortEchoesSuits::EncodeCmpr(t.width, t.height, t.rgba, t.mips))) {
        return false;
      }
      gravityTextures[id] = out;
    }
    char line[160];
    std::snprintf(line, sizeof(line), "marine: Gravity marine: %zu textures recoloured, plates' value %.3f (x%.3f)",
                  gravityTextures.size(), plate, scale);
    log(line);
  }
  const auto remap = [](PrimeCmdl& model, const std::map<uint32_t, uint32_t>& to, std::string* why) {
    for (auto& bytes : model.materialSets) {
      MaterialSet set;
      if (!PortEchoesSuits::ParseSet(bytes, set, why)) return false;
      for (uint32_t& id : set.textures) {
        const auto found = to.find(id);
        if (found != to.end()) id = found->second;
      }
      bytes = PortEchoesSuits::WriteSet(set);
    }
    return true;
  };
  // Echoes textures a written model draws (the recoloured ones are written already).
  const auto collect = [&](const PrimeCmdl& model) {
    for (const auto& bytes : model.materialSets) {
      MaterialSet set;
      std::string why;
      if (!PortEchoesSuits::ParseSet(bytes, set, &why)) continue;
      for (const uint32_t id : set.textures) {
        bool recoloured = false;
        for (const auto& [from, to] : gravityTextures) recoloured = recoloured || to == id;
        if (!recoloured) echoesTextures.insert(id);
      }
    }
  };

  // Each look's bodies, left arm and rifle, under Wear(look, Prime's Varia ids).
  progress();
  for (int look = 0; look < kLookCount; ++look) {
    if (cancelled()) return false;
    const Trooper* source = look == kDarkTrooper ? &kDarkTrooperModel : &kMarineTrooper;
    std::string why;
    for (int kind = 0; kind < PortEchoesSuits::kBodyKinds; ++kind) {
      PrimeCmdl model = kind < 3 ? looks[size_t(look)][size_t(kind)].model : firstPerson[source].arm;
      const std::vector<uint8_t>& skin = kind < 3 ? looks[size_t(look)][size_t(kind)].skin : firstPerson[source].armSkin;
      if (look == kGravityMarine && !remap(model, gravityTextures, &why)) {
        error = std::string(kLookNames[look]) + ": " + why;
        return false;
      }
      const uint32_t cmdlId = Wear(look, varia.cmdl[kind]);
      const uint32_t cskrId = Wear(look, varia.cskr[kind]);
      const std::string what = std::string(kLookNames[look]) + " " + kKindNames[kind];
      if (!fresh(cmdlId, what) || !fresh(cskrId, what) || !write(Hex8(cmdlId) + ".CMDL", PortEchoesSuits::WriteCmdl(model)) ||
          !write(Hex8(cskrId) + ".CSKR", skin)) {
        return false;
      }
      collect(model);
      ++(kind < 3 ? result.bodies : result.arms);
    }
    PrimeCmdl rifle = firstPerson[source].rifle;
    if (look == kGravityMarine && !remap(rifle, gravityTextures, &why)) {
      error = std::string(kLookNames[look]) + " rifle: " + why;
      return false;
    }
    const uint32_t rifleId = Wear(look, kRifleBase);
    if (!fresh(rifleId, std::string(kLookNames[look]) + " rifle") ||
        !write(Hex8(rifleId) + ".CMDL", PortEchoesSuits::WriteCmdl(rifle))) {
      return false;
    }
    collect(rifle);
    ++result.rifles;
    PrimeCmdl grip = firstPerson[source].grip;
    if (look == kGravityMarine && !remap(grip, gravityTextures, &why)) {
      error = std::string(kLookNames[look]) + " left hand on the rifle: " + why;
      return false;
    }
    const uint32_t gripId = Wear(look, kLeftGripBase);
    if (!fresh(gripId, std::string(kLookNames[look]) + " left hand on the rifle") ||
        !write(Hex8(gripId) + ".CMDL", PortEchoesSuits::WriteCmdl(grip))) {
      return false;
    }
    collect(grip);
    ++result.grips;
    log(std::string("marine: ") + kLookNames[look] + ": bodies " + Hex8(Wear(look, varia.cmdl[0])) + " " +
        Hex8(Wear(look, varia.cmdl[1])) + " " + Hex8(Wear(look, varia.cmdl[2])) + ", left arm " +
        Hex8(Wear(look, varia.cmdl[3])) + ", rifle " + Hex8(rifleId) + ", left hand on it " + Hex8(gripId));
  }

  // The balls: Prime's own Morph Ball (from its Power Suit set) and Spider Ball (from its Varia Suit
  // set) in each look's colours, their low-poly models alike, and the Spider Ball's glass of the
  // look's light (teal, the Gravity marine's aqua). The Dark Trooper keeps Prime's Phazon Spider Ball
  // and its glass, dark already.
  progress();
  {
    // The Dark Trooper's armour, measured: the median of its body's plates (lit, unsaturated texels).
    double darkHue = 350.0, darkSat = 0.10, darkValue = 0.35;
    {
      std::vector<uint8_t> data;
      Texture t;
      MaterialSet set;
      std::string why;
      if (PortEchoesSuits::ParseSet(bodies[&kDarkTrooperModel][0].model.materialSets[0], set, &why) &&
          !set.textures.empty() && io.echoes(kTXTR, set.textures[0], data) && Decode(data, t)) {
        std::vector<double> hs, ss, vs;
        for (size_t k = 0; k * 4 + 3 < t.rgba.size(); ++k) {
          double h, s, v;
          PortEchoesSuits::RgbToHsv(&t.rgba[k * 4], h, s, v);
          if (v > 0.25 && s < 0.35) {
            hs.push_back(h);
            ss.push_back(s);
            vs.push_back(v);
          }
        }
        if (!vs.empty()) {
          darkHue = Median(hs);
          darkSat = Median(ss);
          darkValue = Median(vs);
        }
      }
      char line[128];
      std::snprintf(line, sizeof(line), "marine: Dark Trooper armour hue %.0f saturation %.2f value %.2f", darkHue,
                    darkSat, darkValue);
      log(line);
    }
    // The marine's plates' value, for its white balls: the median of its body's lit, unsaturated texels.
    double marineValue = 0.80;
    {
      std::vector<uint8_t> data;
      Texture t;
      MaterialSet set;
      std::string why;
      if (PortEchoesSuits::ParseSet(bodies[&kMarineTrooper][0].model.materialSets[0], set, &why) &&
          !set.textures.empty() && io.echoes(kTXTR, set.textures[0], data) && Decode(data, t)) {
        std::vector<double> vs;
        for (size_t k = 0; k * 4 + 3 < t.rgba.size(); ++k) {
          double h, s, v;
          PortEchoesSuits::RgbToHsv(&t.rgba[k * 4], h, s, v);
          if (v > 0.45 && s < 0.30) vs.push_back(v);
        }
        if (!vs.empty()) marineValue = Median(vs);
      }
    }
    const BallPalette palettes[kLookCount] = {
        {195.0, 0.05, marineValue, false, true, kMarineLightHue, 0.75},
        {kGravityHue, kGravitySat, kGravityValue, true, true, kGravityVisorHue, 0.95},
        {darkHue, darkSat, darkValue, false, false, 0.0, 0.0},
    };
    std::map<std::pair<int, uint32_t>, uint32_t> ballTextures;  // (look, Prime id) -> recoloured id
    std::string why;
    // Prime's model `id` with only its set `set`, its textures in the look's colours.
    const auto recolourModel = [&](int look, uint32_t id, size_t set, PrimeCmdl& model) {
      std::vector<uint8_t> data;
      std::vector<uint32_t> textures;
      if (!io.retail(kCMDL, id, data) || !PortEchoesSuits::ReadCmdl(data, model, &why) ||
          !PortEchoesSuits::KeepMaterialSet(model, std::min(set, model.materialSets.size() - 1), textures, &why)) {
        error = "Prime's ball model " + Hex8(id) + " is missing or unreadable" + (why.empty() ? "" : ": " + why);
        return false;
      }
      std::map<uint32_t, uint32_t> to;
      for (const uint32_t tex : textures) {
        const auto done = ballTextures.find({look, tex});
        if (done != ballTextures.end()) {
          if (done->second != 0) to[tex] = done->second;
          continue;
        }
        std::vector<uint8_t> bytes;
        Texture t;
        uint32_t out = 0;
        if (io.retail(kTXTR, tex, bytes) && Decode(bytes, t) && RecolourBall(t, palettes[look])) {
          out = Wear(look, tex);
          if (!fresh(out, std::string(kLookNames[look]) + " ball texture " + Hex8(tex)) ||
              !write(Hex8(out) + ".TXTR", PortEchoesSuits::EncodeCmpr(t.width, t.height, t.rgba, t.mips))) {
            return false;
          }
          to[tex] = out;
        }
        ballTextures[{look, tex}] = out;
      }
      return remap(model, to, &why);
    };
    for (int look = 0; look < kLookCount; ++look) {
      if (cancelled()) return false;
      std::set<uint32_t> done;
      for (const auto& t : PortDreadSamus::kBallTargets) {
        if (kSuitLooks[t.suit] != look || (look == kDarkTrooper && t.spider) || !done.insert(t.ancs).second) continue;
        const std::string what = std::string(kLookNames[look]) + (t.spider ? " Spider Ball" : " Morph Ball");
        // Prime's ANCS: its character 0's model (after the name from byte 14), its skin kept.
        std::vector<uint8_t> ancs;
        if (!io.retail(kANCS, t.ancs, ancs)) {
          error = "Prime's ball " + Hex8(t.ancs) + " is missing.";
          return false;
        }
        size_t at = 14;
        while (at < ancs.size() && ancs[at] != 0) ++at;
        ++at;
        if (at + 8 > ancs.size() || Get32(ancs, at) != t.cmdl) {
          error = "Prime's ball " + Hex8(t.ancs) + " is not the one this importer knows.";
          return false;
        }
        // The Morph Ball from Prime's Power Suit set (0, yellow), the Spider Ball from its Varia Suit
        // set (1, orange), the low-poly models from the same.
        const size_t set = t.spider ? 1 : 0;
        PrimeCmdl main, low;
        if (!recolourModel(look, t.cmdl, set, main) || !recolourModel(look, t.low, set, low)) return false;
        const uint32_t mainId = Wear(look, t.cmdl), lowId = Wear(look, t.low), ancsId = Wear(look, t.ancs);
        if (!fresh(mainId, what) || !fresh(lowId, what) || !fresh(ancsId, what)) return false;
        Set32(ancs, at, mainId);
        if (!write(Hex8(mainId) + ".CMDL", PortEchoesSuits::WriteCmdl(main)) ||
            !write(Hex8(lowId) + ".CMDL", PortEchoesSuits::WriteCmdl(low)) || !write(Hex8(ancsId) + ".ANCS", ancs)) {
          return false;
        }
        ++result.balls;
        log("marine: " + what + " -> " + Hex8(ancsId) + " (" + Hex8(mainId) + ", low " + Hex8(lowId) + ")");
        if (t.spider) {
          // The glass: Prime's set of the look's light, teal (0) or the Gravity marine's aqua (2).
          std::vector<uint8_t> data;
          std::vector<uint32_t> textures;
          PrimeCmdl glass;
          const size_t glassSet = look == kGravityMarine ? 2 : 0;
          if (!io.retail(kCMDL, kSpiderGlass, data) || !PortEchoesSuits::ReadCmdl(data, glass, &why) ||
              glass.materialSets.size() <= glassSet ||
              !PortEchoesSuits::KeepMaterialSet(glass, glassSet, textures, &why)) {
            error = "Prime's spider ball glass " + Hex8(kSpiderGlass) + " is missing or unreadable" +
                    (why.empty() ? "" : ": " + why);
            return false;
          }
          const uint32_t glassId = Wear(look, kSpiderGlass);
          if (!fresh(glassId, what + " glass") || !write(Hex8(glassId) + ".CMDL", PortEchoesSuits::WriteCmdl(glass))) {
            return false;
          }
          ++result.balls;
        }
      }
    }
    size_t recoloured = 0;
    for (const auto& [key, out] : ballTextures) recoloured += out != 0 ? 1 : 0;
    log("marine: balls: " + std::to_string(recoloured) + " textures recoloured");
  }

  // The Echoes textures the models draw, as they are.
  progress();
  for (const uint32_t id : echoesTextures) {
    if (cancelled()) return false;
    std::vector<uint8_t> data;
    if (!io.echoes(kTXTR, id, data)) {
      error = "The Echoes disc has no texture " + Hex8(id) + ".";
      return false;
    }
    // A texture id Prime also uses would replace Prime's texture everywhere: only when they are the
    // same picture.
    if (io.retailId(id)) {
      std::vector<uint8_t> retail;
      if (!io.retail(kTXTR, id, retail) || retail != data) {
        error = "Echoes texture " + Hex8(id) + " has the id of a different Prime resource.";
        return false;
      }
      continue;
    }
    if (!write(Hex8(id) + ".TXTR", data)) return false;
  }
  if (io.progress) io.progress(kSteps, kSteps);
  log("marine: " + std::to_string(result.bodies) + " bodies, " + std::to_string(result.arms) + " left arms, " +
      std::to_string(result.rifles) + " rifles, " + std::to_string(result.grips) + " left hands on them, " +
      std::to_string(result.balls) + " balls and glass, " +
      std::to_string(echoesTextures.size()) + " Echoes textures, " + std::to_string(result.files.size()) + " files");
  return true;
}

}  // namespace PortFederationMarine
