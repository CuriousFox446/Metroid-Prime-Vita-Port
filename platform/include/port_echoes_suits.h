#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "port_dread_reskin.h"
#include "port_echoes_suits_mod.h"

// The Echoes suits import: Samus's suit bodies from the user's own Metroid Prime 2: Echoes disc
// (GameCube, G2ME01) in place of Prime's, written as the "echoes-suits" mod.
//
// Echoes keeps Prime's formats one step on: its PAKs hold LZO-compressed resources, its CMDLs are
// version 4 (matrix-index bytes in every vertex, two more words per material) and its CSKR/CINF
// carry extra data Prime does not read. Prime's own skeletons are Echoes' with fewer joints, so a
// body converts without re-skinning: every vertex keeps its weights, each Echoes joint renamed to
// Prime's joint of the same name (or its nearest ancestor that has one). Textures are the same
// format and are copied under their Echoes ids.
//
// Everything comes and goes through Io, so the same code runs in the game
// (port_echoes_suits_import.cpp) and in the tests. The output is the user's, made from their own
// copies; nothing of either game ships.
namespace PortEchoesSuits {

// LZO1X: `destLen` is the capacity on entry and the bytes written on return. False on a corrupt
// stream (what was decoded so far is still counted in destLen).
bool LzoInflate(const uint8_t* source, size_t sourceLen, uint8_t* dest, size_t& destLen);

// One compressed Echoes PAK entry: a big-endian u32 size, then segments, each a big-endian s16
// length and that many bytes: LZO1X for a positive length (it inflates to min(0x4000, what is
// left)), stored bytes for a negative one.
bool Decompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string* error);

// An Echoes CMDL (version 4) as Prime's (version 2): the materials' PBR bit (14) and the vertex
// flags' matrix-index bits (24-31) cleared and their two extra words dropped, each surface header
// without its extra word and its display list without the matrix-index bytes. `textures` gets the
// TXTR ids of every material set, sorted, without repeats.
bool ConvertCmdl(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::vector<uint32_t>& textures,
                 std::string* error);

// A Prime CMDL (version 2) in parts, for changes after conversion. WriteCmdl lays it out again,
// its section counts and sizes from the parts; each part is already padded to 32 bytes.
struct PrimeCmdl {
  std::vector<uint8_t> header;  // the 0x2C bytes before the section sizes
  std::vector<std::vector<uint8_t>> materialSets;
  std::vector<std::vector<uint8_t>> arrays;  // positions, normals, colours, UVs[, short UVs]
  std::vector<std::vector<uint8_t>> surfaces;
};
bool ConvertCmdl(const std::vector<uint8_t>& in, PrimeCmdl& out, std::vector<uint32_t>& textures,
                 std::string* error);
std::vector<uint8_t> WriteCmdl(const PrimeCmdl& model);

// Short normals (flag 0x2, s16 with 14 fraction bits, vertex format 1) as floats (format 0): Prime
// skins float normals only. Fails on a list in any other format.
bool FloatNormals(PrimeCmdl& model, std::string* error);

// `b` drawn as part of `a`: its textures, materials, arrays and surfaces after a's, its material
// texture indices, surface materials and vertex indices moved past a's. Both must have one
// material set and the same vertex flags, and b's indices must be 16-bit wherever they move.
bool MergeCmdl(PrimeCmdl& a, const PrimeCmdl& b, std::string* error);

// `model` skinned rigidly on `joint`: its positions and normals padded with zeros to one count,
// which `cskr` covers with one group of weight 1 (CSkinRules hands each group the same run of
// both arrays). The model must have float normals.
bool RigidSkin(PrimeCmdl& model, uint32_t joint, std::vector<uint8_t>& cskr, std::string* error);

// The joint of a Prime CSKR that carries the most weight over its vertices.
bool MainJoint(const std::vector<uint8_t>& cskr, uint32_t& joint, std::string* error);

struct EchoesJoint {
  uint32_t id = 0;
  uint32_t parent = 0;
  std::string name;
};
// An Echoes CINF: u32 count, {u32 id, u32 parent, f32 position[3], f32 rotation[4],
// f32 local rotation[4], u32 n, u32 links[n]}, u32 count, u32 build order[], u32 count,
// {string name, u32 id}. Joints in file order, named.
bool ParseCinf(const uint8_t* data, size_t size, std::vector<EchoesJoint>& out, std::string* error);

// An Echoes CSKR on Prime's skeleton `to`: each weight moves to the Prime joint of its Echoes
// joint's name (the Varia shoulder pads' L/R_varia_SDK to Prime's L/R_varias2_SDK), else of its
// nearest named ancestor's; weights landing on one joint add up. Echoes' footer is replaced by
// Prime's. `folded` (optional) gets "echoes->prime" for every joint that had no namesake.
bool ConvertCskr(const std::vector<uint8_t>& in, const std::vector<EchoesJoint>& from,
                 const PortDread::PrimeSkeleton& to, std::vector<uint8_t>& out, std::vector<std::string>* folded,
                 std::string* error);

// One body: Echoes' CMDL and CSKR on its CINF, converted onto Prime's CINF and written as `cmdl`
// and `cskr`: for a worn look (port_echoes_suits_mod.h), Wear of the Varia's body of that kind
// (kind 0-3 of SuitResources), which PortMods serves over the bodies of the suits that wear it.
struct Target {
  const char* suit;      // Echoes' suit, for the log
  const char* skeleton;  // "cine50", "game20", "doll49" or "grapple"
  uint32_t echoesCmdl, echoesCskr, echoesCinf;
  uint32_t cmdl, cskr, cinf;
  int worn = -1;  // WornLook, or -1 (Dark Samus)
  int kind = -1;  // the SuitResources kind
};
constexpr Target WornTarget(const char* suit, int kind, uint32_t echoesCmdl, uint32_t echoesCskr, uint32_t echoesCinf,
                            uint32_t cinf, int worn) {
  const SuitResources& varia = kSuitResources[kVariaSuit];
  return {suit,       kind == 0 ? "cine50" : kind == 1 ? "game20" : kind == 2 ? "doll49" : "grapple",
          echoesCmdl, echoesCskr,
          echoesCinf, Wear(worn, varia.cmdl[kind]),
          Wear(worn, varia.cskr[kind]), cinf,
          worn,       kind};
}
// Cutscene bodies on Echoes' A27AFE83 / Prime's 19C53902; in-game ones on F86193FE / A496DA02
// (BallTransitionsANCS); the pause screen's doll bodies (ANCS_ItemScreenSamus, CINF F76E70A6, the
// cutscene skeleton in all but a locator) from the cutscene ones. The Dark Suit is Echoes'
// Dark+Gravity body.
inline constexpr Target kTargets[] = {
    WornTarget("Varia", 0, 0xCDF1C541, 0xA6DE6A97, 0xA27AFE83, 0x19C53902, kWornVaria),
    WornTarget("Dark", 0, 0x3A93B6F2, 0x51BC1924, 0xA27AFE83, 0x19C53902, kWornDark),
    WornTarget("Light", 0, 0x726FB01A, 0x19401FCC, 0xA27AFE83, 0x19C53902, kWornLight),
    WornTarget("Varia", 1, 0xE31FD3AD, 0x88307C7B, 0xF86193FE, 0xA496DA02, kWornVaria),
    WornTarget("Dark", 1, 0xC8146EFF, 0xA33BC129, 0xF86193FE, 0xA496DA02, kWornDark),
    WornTarget("Light", 1, 0xACDFD2CA, 0xC7F07D1C, 0xF86193FE, 0xA496DA02, kWornLight),
    WornTarget("Varia", 2, 0xCDF1C541, 0xA6DE6A97, 0xA27AFE83, 0xF76E70A6, kWornVaria),
    WornTarget("Dark", 2, 0x3A93B6F2, 0x51BC1924, 0xA27AFE83, 0xF76E70A6, kWornDark),
    WornTarget("Light", 2, 0x726FB01A, 0x19401FCC, 0xA27AFE83, 0xF76E70A6, kWornLight),
    // The first-person left (grapple) arm, characters of Prime's ANCS 5808EB6A on CINF 516F4445;
    // Echoes' ANCS 670ABE2A gives every suit CINF D07D1BD9, the same 23 joints plus LGBeamLight.
    WornTarget("Varia", 3, 0x463D7A4E, 0x2D12D598, 0xD07D1BD9, 0x516F4445, kWornVaria),
    WornTarget("Dark", 3, 0x078B71CB, 0x6CA4DE1D, 0xD07D1BD9, 0x516F4445, kWornDark),
    WornTarget("Light", 3, 0xBACC46C8, 0xD1E3E91E, 0xD07D1BD9, 0x516F4445, kWornLight),
    // Dark Samus (kDarkBodies): ANCS 2B513FD9's Dark_Samus on CINF 4ABC8A50, Echoes Samus's
    // joints and bind pose less the face and GRAPPLE_LCTR.
    {"Dark Samus", "cine50", 0xD2C3805B, 0xB9EC2F8D, 0x4ABC8A50, DarkSamusId(0x19C53902), DarkSamusSkinId(0x19C53902),
     0x19C53902},
    {"Dark Samus", "game20", 0xD2C3805B, 0xB9EC2F8D, 0x4ABC8A50, DarkSamusId(0xA496DA02), DarkSamusSkinId(0xA496DA02),
     0xA496DA02},
    {"Dark Samus", "doll49", 0xD2C3805B, 0xB9EC2F8D, 0x4ABC8A50, DarkSamusId(0xF76E70A6), DarkSamusSkinId(0xF76E70A6),
     0xF76E70A6},
};
// One look's Morph Ball: Echoes' ball model (static there; `caps`, if not 0, merged into it), fit to
// each Prime ball ANCS of PortDreadSamus::kBallTargets whose `spider` matches (the plain ball's,
// the Spider Ball's, Phazon's hyper Spider Ball's): a copy of that ANCS whose character 0 takes the
// model skinned rigidly on the joint Prime's ball skin weights most, and the model unskinned as the
// low-poly ball, all under Wear(worn, Prime's ids). Only the Dark Suit has a Spider Ball of its own
// in Echoes. The other looks (the Varia, the Light Suit, the Varia in Gravity's and Phazon's colours)
// keep Prime's own Spider Balls, already in each suit's colours: the user's choice over their plain
// balls standing in for it (mod versions 7 to 14).
struct Ball {
  const char* suit;  // Echoes' ball, for the log
  int worn;          // WornLook
  bool spider;
  uint32_t echoesCmdl, caps;
};
inline constexpr Ball kBalls[] = {
    {"Varia", kWornVaria, false, 0xF57D2B74, 0},
    {"Dark", kWornDark, false, 0x07C3C0E0, 0},
    {"Light", kWornLight, false, 0x9BE5CFAD, 0},
    {"Dark", kWornDark, true, 0x6AA79226, 0x5A5C0FEC},
};

// The first-person right arm (CGunWeapon::LoadSuitArm, a static model at the gun's elbow): Echoes
// has one, VariaArm, with a material set per suit; each set alone, as Wear(worn) of Prime's Varia arm.
inline constexpr uint32_t kEchoesArm = 0x4AD9BBAC;
struct Arm {
  const char* suit;  // Echoes' suit, for the log
  uint32_t set;      // kEchoesArm's material set
  uint32_t cmdl;     // where it is written
  int worn;
};
inline constexpr Arm kArms[] = {
    {"Varia", 0, Wear(kWornVaria, kSuitResources[kVariaSuit].arm), kWornVaria},
    {"Dark", 1, Wear(kWornDark, kSuitResources[kVariaSuit].arm), kWornDark},
    {"Light", 2, Wear(kWornLight, kSuitResources[kVariaSuit].arm), kWornLight},
};

// Echoes' Varia in Prime's Gravity or Phazon colours (kWornGravity, kWornPhazon), as Prime
// recolours its own Varia: each texture the Varia's bodies, arms and ball draw, recoloured by
// whether its texels lie on a limb (the joints Prime's Gravity Suit paints purple: shoulder pads,
// arms, hands, legs and feet; the arms and ball are all limb). Gravity: limbs' warm texels purple,
// greens (lights, visor) cyan, the torso and helmet as they are. Phazon: warm texels near black (the
// torso and helmet a dark grey), greens red, the rest darker. `limb` per texel; returns whether it
// changed anything.
bool RecolourVaria(int worn, std::vector<uint8_t>& rgba, const std::vector<uint8_t>& limb);
// Whether a Prime joint name is a limb's in that sense.
bool IsLimbJoint(const std::string& name);
// A Prime CMDL (version 2) in parts.
bool ReadCmdl(const std::vector<uint8_t>& in, PrimeCmdl& model, std::string* error);
// Prime's `body` (skinned by `skin`) with its head swapped for `donor`'s, both on one Prime skeleton
// (`donorSkin`): the body's triangles with any vertex weighing most on a joint of `head` dropped, the
// donor's with one kept and merged in after them (MergeCmdl), the skins' groups one after the other
// (a group over the body's padding vertices between them, as MergeCmdl counts its arrays).
bool SwapHead(PrimeCmdl& body, std::vector<uint8_t>& skin, const PrimeCmdl& donor,
              const std::vector<uint8_t>& donorSkin, const std::vector<uint32_t>& head, std::string* error);
// A CMPR TXTR's top level as RGBA (false for another format), and RGBA as a mipmapped CMPR TXTR.
bool DecodeCmpr(const std::vector<uint8_t>& txtr, int& width, int& height, std::vector<uint8_t>& rgba);
std::vector<uint8_t> EncodeCmpr(int width, int height, const std::vector<uint8_t>& rgba, int mips);

// A beam's ANCS with each kGuns character (the 12 bytes model, skin, skeleton, found once) pointed
// at the Cannon variants. False when one is not found exactly once.
bool PatchGunAncs(std::vector<uint8_t>& ancs, std::string* error);

// An Echoes compressed gun ANIM (version 2, 0x0101 where Prime has its EVNT id; u8 joint ids, a scale
// track per channel) in Prime's layout (CFBStreamedCompression) for `to`, joints matched by name:
// every channel Echoes lists, a still rotation where it has no rotation track and a still offset
// at `to`'s skeleton place where it has no offset track (Echoes poses a listed joint so; Prime's
// pose builder would keep the joint's last offset), the keys re-packed bit for bit, `evnt` as its
// EVNT. False on a scale track or a joint with tracks that `to` lacks.
bool ConvertGunAnim(const std::vector<uint8_t>& in, const std::vector<EchoesJoint>& from, const PortDread::PrimeSkeleton& to,
                    uint32_t evnt, std::vector<uint8_t>& out, std::string* error);

// A Prime gun ANCS's missile anims: the ANIMs character 0 plays as anims 5-8 (CGunWeapon's
// kGAT_FromMissile, ToMissile, MissileShoot, MissileReload) and their EVNTs. `found` is false for a
// gun without them (Phazon's).
struct GunMissileAnims {
  bool found = false;
  uint32_t anim[4] = {};
  uint32_t evnt[4] = {};
};
bool FindGunMissileAnims(const std::vector<uint8_t>& ancs, GunMissileAnims& out, std::string* error);

// The ANCS with kMissileAnimNames added to every character and the animation set: Cannon(anim[i])
// for the four, then Cannon(kFirstMissileShot); Prime's transitions touching its missile anims
// mirrored onto them (the snaps into a missile shot), and the (ANIM, EVNT) list kept sorted.
bool AddGunMissileAnims(std::vector<uint8_t>& ancs, const GunMissileAnims& missiles, std::string* error);

// `model` with only its material set `set`; `textures` gets that set's TXTR ids.
bool KeepMaterialSet(PrimeCmdl& model, size_t set, std::vector<uint32_t>& textures, std::string* error);

struct Io {
  // A resource of the Echoes disc by type FourCC and id, decompressed.
  std::function<bool(uint32_t type, uint32_t id, std::vector<uint8_t>& out)> echoes;
  // A resource of the unmodded Prime disc ('CINF', 'TXTR').
  std::function<bool(uint32_t type, uint32_t id, std::vector<uint8_t>& out)> retail;
  // Whether any Prime disc resource has this id (an Echoes texture must not shadow one).
  std::function<bool(uint32_t id)> retailId;
  // Stores one output file ("<ID>.CMDL", "<ID>.CSKR", "<ID>.TXTR").
  std::function<bool(const std::string& name, const std::vector<uint8_t>& data)> write;
  std::function<void(const std::string& line)> log;     // optional
  std::function<bool()> cancelled;                     // optional
  std::function<void(size_t done, size_t total)> progress;  // optional, per target
};

struct Result {
  size_t converted = 0;            // bodies written
  size_t balls = 0;                // Morph Balls written
  size_t arms = 0;                 // right arms written
  size_t guns = 0;                 // beam ANCS variants written
  std::vector<std::string> files;  // every file written, in order
};

// Converts every target, then writes them, their copies and their textures. False on any failure
// or cancellation, with the reason in `error`; the caller then discards what was written.
bool Import(const Io& io, Result& result, std::string& error);

}  // namespace PortEchoesSuits
