#pragma once

#include <cstdint>

#include "port_model_variant.h"

// What the game and the mod loader need to know of the "echoes-suits" mod (port_echoes_suits.h, the import that
// makes it), without the import's headers.
namespace PortEchoesSuits {

// The mod's folder in the mods folder, and its stamp: kVersion, then the build's commit. Bump
// kVersion when the output changes.
inline constexpr const char* kModName = "echoes-suits";
inline constexpr const char* kStampName = ".echoes-suits-version";
// 1: Varia, Gravity (Echoes' Dark Suit) and Phazon (its Light Suit), cutscene and in-game bodies.
// 2: the pause screen's doll bodies; the Morph Balls (Varia, Dark as Gravity, Light as Phazon) and
//    the Dark Spider Ball as Gravity's.
// 3: the first-person arms, right and left (grapple).
// 4: the first-person arm cannon and its hologram, under variant ids (Cannon).
// 5: the cannon's Light and Annihilator sets swapped (kGunSets).
// 6: Dark Samus, one body per skeleton under DarkSamusId/DarkSamusSkinId (kDarkBodies).
// 7: the Varia and Light balls also as the Varia and Phazon Spider Balls.
// 8: Dark Samus's Morph Ball (kDarkBall).
// 9: Echoes' missile anims for the cannon, added to each beam's ANCS variant (kMissileAnimNames).
// 10: the cannon's Light and Annihilator sets back on Wave and Plasma (kGunSets), as Echoes pairs them.
// 11: the missile anims pose every joint Echoes lists (the gills close for missiles, as in Echoes).
// 12: each look written once under its own ids (Wear), served over whichever suit wears it
//     (echoes_suit_looks); Echoes' Varia also in Prime's Gravity and Phazon colours.
// 13: Prime's bodies with Echoes' Varia helmet (kLookHelmet, HelmetId).
// 14: the cannon's sets 5-7, Prime's original Ice, Wave and Plasma looks (kOriginalSets).
inline constexpr int kVersion = 14;

// What each of Prime's suits wears (the setting echoes_suit_looks; the next mod reload applies a
// change), by CPlayerState::EPlayerSuit: Power, Gravity, Varia, Phazon. A look covers the suit's
// cutscene, in-game and pause screen bodies, its first-person arms and its Morph and Spider Balls.
enum SuitLookKind : int {
  kLookPrime = 0,  // Prime's own
  kLookVaria,      // Echoes' Varia Suit
  kLookDark,       // Echoes' Dark Suit
  kLookLight,      // Echoes' Light Suit
  // Echoes' Varia in the suit's own Prime colours, as Prime recolours its Varia into Gravity and
  // Phazon (Gravity's limbs purple and visor cyan; Phazon dark with a red visor). Varia and Power
  // have no colours of their own: the Varia itself.
  kLookColours,
  // Prime's own body with Echoes' Varia helmet (the helmet of the suit's colours, as kLookColours
  // gives it: Gravity's cyan visor, Phazon's dark helmet); its arms and balls stay Prime's.
  kLookHelmet,
  kLookCount,
};
inline constexpr int kSuitLookSuits = 4;
inline constexpr const char* kSuitLookKeys[kLookCount] = {"prime", "varia", "dark", "light", "colours", "helmet"};
inline constexpr const char* kSuitLookNames[kLookCount] = {"Prime's own",        "Echoes Varia Suit",
                                                           "Echoes Dark Suit",   "Echoes Light Suit",
                                                           "Echoes Varia in its colours", "Echoes helmet"};
// The two looks each suit offers, as the user set them out (0.46.0 test 30): Power Prime's own or its
// body with Echoes' helmet; Gravity the Dark Suit or Echoes' Varia in its colours; Phazon the Light
// Suit or the Varia in its colours; the Varia always Echoes' Varia. SetSuitLook takes no other.
inline constexpr int kSuitLookChoices[kSuitLookSuits][2] = {
    {kLookPrime, kLookHelmet}, {kLookDark, kLookColours}, {kLookVaria, kLookVaria}, {kLookLight, kLookColours}};
// The helmet bodies (kLookHelmet): each suit's Prime body of kinds 0-2 (cutscene, in-game, doll) with
// its head swapped for Echoes' Varia's (SwapHead), under HelmetId of Prime's body and skin ids.
inline constexpr int kHelmetKey = 0x454848;  // "EHH"
constexpr uint32_t HelmetId(uint32_t id) { return PortModelVariant::Id(id, kHelmetKey); }
// Whether the loaded mods serve a helmet body under this Prime body id: it draws its own visor (the
// pause screen's doll leaves out CMDL_InvVisor) while keeping Prime's boots and other parts.
bool HasOwnHelmet(uint32_t bodyCmdl);
int SuitLook(int suit);
void SetSuitLook(int suit, int look);
// The look the loaded mods serve for the suit (kLookPrime when none): a reload applies SuitLook.
int ServedSuitLook(int suit);

// What the import writes, once per look, under variant ids of Prime's Varia resources (Wear):
// Echoes' three suits, and its Varia in Gravity's and Phazon's colours (its recoloured textures
// under Wear of their own ids too).
enum WornLook : int { kWornVaria, kWornDark, kWornLight, kWornGravity, kWornPhazon, kWornCount };
inline constexpr int kWornKeys[kWornCount] = {0x455656, 0x455644, 0x45564C, 0x455647, 0x455650};  // "EVV".."EVP"
constexpr uint32_t Wear(int worn, uint32_t id) { return PortModelVariant::Id(id, kWornKeys[worn]); }
// The worn look a suit's look stands for; -1 for Prime's own.
constexpr int WornFor(int suit, int look) {
  return look == kLookVaria   ? kWornVaria
         : look == kLookDark  ? kWornDark
         : look == kLookLight ? kWornLight
         : look == kLookColours ? (suit == 1 ? kWornGravity : suit == 3 ? kWornPhazon : kWornVaria)
                                : -1;
}

// The resources of each suit a look stands in for (CPlayerState::EPlayerSuit order): its bodies by
// kind (the cutscene body on CINF 19C53902, the in-game one on A496DA02, the pause screen's doll on
// F76E70A6, and the first-person left arm, ANCS 5808EB6A's character on 516F4445), as CMDL and
// CSKR, and its first-person right arm (CGunWeapon::skSuitArmNames). The import writes each look
// under Wear of the Varia's.
inline constexpr int kBodyKinds = 4;
struct SuitResources {
  uint32_t cmdl[kBodyKinds], cskr[kBodyKinds];
  uint32_t arm;
};
inline constexpr SuitResources kSuitResources[kSuitLookSuits] = {
    {{0xF39476F2, 0x4A1A8FD3, 0xCC25E002, 0xBF05B77E}, {0x98BBD924, 0x21352005, 0xA70A4FD4, 0xD42A18A8}, 0x07111AA7},
    {{0x57F5C930, 0x57489AD3, 0x11A5BC8A, 0xCBFA3FF6}, {0x3CDA66E6, 0x3C673505, 0x7A8A135C, 0xA0D59020}, 0x81856809},
    {{0x776C1A96, 0xE449874A, 0xA602C954, 0x00A6EC53}, {0x1C43B540, 0x8F66289C, 0xCD2D6682, 0x6B894385}, 0x4AD9BBAC},
    {{0x2782648E, 0x891E00FD, 0x5644E7A1, 0xD6FF0F4E}, {0x4CADCB58, 0xE231AF2B, 0x3D6B4877, 0xBDD0A098}, 0x57DC8B14},
};
inline constexpr int kVariaSuit = 2;
// Copies Prime draws too: the cutscene Varia skin under ANCS 863C1B6A's 8C72FDAC, and the cutscene
// Phazon body under Tallon ANCS BAD39B59's B333E1D7. They follow the Varia's and Phazon's looks.
inline constexpr uint32_t kVariaSkinCopy = 0x8C72FDAC;
inline constexpr uint32_t kPhazonBodyCopy = 0xB333E1D7;

// The first-person arm cannon (CGunWeapon::LoadGunModels: characters 0 and 1 of each beam's ANCS).
// Echoes' gun and hologram have Prime's ids and are skinned on its own CINF 46ED31E0, the same 50
// joints by name and place as Prime's (only the ids differ), so Prime's gun animations drive them.
// Each goes under the variant id Cannon(id), its material sets reordered by kGunSets: the game
// picks a gun's set by beam (CGunWeapon x20c_shaderIdx).
struct Gun {
  const char* what;  // for the log
  uint32_t cmdl, cskr;
};
inline constexpr uint32_t kGunCinf = 0x46ED31E0;
inline constexpr Gun kGuns[] = {
    {"arm cannon", 0x19AE626C, 0x7281CDBA},
    {"cannon hologram", 0xE6F4DEFC, 0x8DDB712A},
};
// The Echoes material set each Prime beam's gun wears, by CPlayerState::EBeamId (Power, Ice, Wave,
// Plasma, Phazon). Echoes' sets are Power, Dark, Light, Annihilator, then the fifth, and Echoes keeps
// Prime's gun ANCS in the same slots ("Power", "Ice", "Wave", "Plasma"): its Dark Beam is the Ice
// slot, Light the Wave slot, Annihilator the Plasma slot. So each beam wears the set of its own slot,
// matching the ANCS (and missile anims) it brings: Ice wears Dark, Wave Light, Plasma Annihilator.
// (Mod versions 5-9 had Wave and Plasma swapped; the user's side-by-side with Echoes, 0.46.0 test 25.)
inline constexpr uint32_t kGunSets[] = {0, 1, 2, 3, 4};
// The first-person gun's ANCS of each beam (CPlayerState::EBeamId order). The mod writes a variant
// of each, Cannon(ancs), whose characters 0 (the gun) and 1 (its hologram) are Echoes'.
inline constexpr uint32_t kBeamAncs[] = {0x41B63D84, 0xC1135EA3, 0x02B4357C, 0x6AB61E5E, 0x5E7FF762};
inline constexpr int kCannonKey = 0x4D5032;  // "MP2"
inline uint32_t Cannon(uint32_t id) { return PortModelVariant::Id(id, kCannonKey); }
// Echoes' gun has its own missile anims under Prime's ids (the Power Beam's MissleOpen 29E9308B, the
// beams' genericMissleShoot C317C67C...): the nozzle turns where Prime's barrel splits open. Each
// beam's ANCS variant keeps Prime's (the gun's timing) and adds Echoes', converted under Cannon(id),
// as these character anims: CGunWeapon's kGAT_FromMissile, ToMissile, MissileShoot and MissileReload,
// then Echoes' first missile shot, fired from the beam's own pose (kFirstMissileShot).
inline constexpr const char* kMissileAnimNames[] = {"PortEchoes_FromMissile", "PortEchoes_ToMissile",
                                                    "PortEchoes_MissileShoot", "PortEchoes_MissileReload",
                                                    "PortEchoes_MissileFirstShoot"};
inline constexpr uint32_t kFirstMissileShot = 0xB9A04995;  // genericMissleFirstShoot
// The textures the Echoes cannon of Ice, Wave and Plasma wears (the setting echoes_cannon_looks, in
// Power, Ice, Wave, Plasma order): Echoes' own for that beam (Dark, Light, Annihilator) or Prime's
// original Ice, Wave or Plasma Beam's, as the user set them out (0.46.0 test 31). Only the material
// set changes: the beam keeps its own ANCS variant, so its barrel's shape and missile motions.
// Echoes built its beam sets on Prime's: its material 0 is Prime's material 0 and its material 3
// Prime's material 2 (the beam's own scrolling textures), and its material 2 adds a body texture of
// its own (Prime's body is the same grey for every beam). The import writes the originals as sets
// kOriginalSets on (Ice, Wave, Plasma): Echoes' set of the beam with Prime's textures in those two
// materials and the Power set's body and shine in material 2. Power and Phazon have no choice.
inline constexpr int kCannonLookBeams = 4;
inline constexpr uint32_t kOriginalSets = 5;
inline constexpr int kOriginalSetsVersion = 14;  // the first import with them
inline constexpr const char* kCannonEchoesKeys[kCannonLookBeams] = {"power", "dark", "light", "annihilator"};
inline constexpr const char* kCannonOriginalKey = "prime";
inline constexpr const char* kCannonEchoesNames[kCannonLookBeams] = {"Power Beam", "Dark Beam (Echoes)",
                                                                     "Light Beam (Echoes)", "Annihilator Beam (Echoes)"};
inline constexpr const char* kCannonOriginalNames[kCannonLookBeams] = {"Power Beam", "Ice Beam (original)",
                                                                       "Wave Beam (original)", "Plasma Beam (original)"};
// Whether Ice, Wave or Plasma (beams 1-3) wears Prime's original look.
bool CannonOriginal(int beam);
void SetCannonOriginal(int beam, bool original);
// The ANCS CGunWeapon::LoadGunModels loads for a beam's ANCS: Cannon(ancs) while the mod serves it
// and the setting echoes_first_person_cannon (SetFirstPersonCannonShown) is on, else ancs.
// *shaderSet gets the material set that cannon draws with (CGunWeapon x20c_shaderIdx): the beam's
// own, or its original (kOriginalSets + beam - 1) when chosen and the import has them; -1 for
// Prime's cannon.
uint32_t GunAncs(uint32_t ancs, int* shaderSet = nullptr);
void SetFirstPersonCannonShown(bool shown);
bool FirstPersonCannonShown();

// The Prime body CMDLs a look can be served over (kSuitResources' bodies and kPhazonBodyCopy):
// cutscene Varia, Gravity, Phazon, the Tallon Phazon copy, in-game Varia, Gravity, Phazon, the
// doll's Varia, Gravity, Phazon, then Power's cutscene, in-game and doll bodies. While the mod
// serves one, PortDreadSamus::Replaces holds for it: Echoes' bodies carry their own boots and
// visor. Not their cannon: the right forearm ends bare, and Prime's cannon is drawn on it
// (PortDreadSamus::HasOwnCannon).
inline constexpr uint32_t kBodies[] = {0x776C1A96, 0x57F5C930, 0x2782648E, 0xB333E1D7, 0xE449874A,
                                       0x57489AD3, 0x891E00FD, 0xA602C954, 0x11A5BC8A, 0x5644E7A1,
                                       0xF39476F2, 0x4A1A8FD3, 0xCC25E002};
inline bool IsBody(uint32_t id) {
  for (const uint32_t body : kBodies) {
    if (body == id) return true;
  }
  return false;
}

// Dark Samus (Echoes' ANCS 2B513FD9, character Dark_Samus), converted once per Prime skeleton and
// written as CMDL DarkSamusId(cinf) and CSKR DarkSamusSkinId(cinf) (an id holds one type only).
// While the setting echoes_dark_samus (SetPlayAsDarkSamus) is on, the loader serves that pair under
// every body of the skeleton below, the Power Suit's included; the Fusion suits keep theirs. Takes effect when the mods next load.
inline constexpr int kDarkSamusKey = 0x44534D;  // "DSM"
inline constexpr int kDarkSamusSkinKey = 0x44534B;  // "DSK"
constexpr uint32_t DarkSamusId(uint32_t cinf) { return PortModelVariant::Id(cinf, kDarkSamusKey); }
constexpr uint32_t DarkSamusSkinId(uint32_t cinf) { return PortModelVariant::Id(cinf, kDarkSamusSkinKey); }
struct DarkBody {
  uint32_t cinf;
  uint32_t cmdl, cskr;  // 0: none
};
// Power, Varia, Gravity and Phazon on the cutscene skeleton (with the two copies kCopies names:
// Varia's second skin, Tallon's Phazon body), in game, then on the doll.
inline constexpr DarkBody kDarkBodies[] = {
    {0x19C53902, 0xF39476F2, 0x98BBD924}, {0x19C53902, 0x776C1A96, 0x1C43B540}, {0x19C53902, 0, 0x8C72FDAC},
    {0x19C53902, 0x57F5C930, 0x3CDA66E6}, {0x19C53902, 0x2782648E, 0x4CADCB58}, {0x19C53902, 0xB333E1D7, 0},
    {0xA496DA02, 0x4A1A8FD3, 0x21352005}, {0xA496DA02, 0xE449874A, 0x8F66289C}, {0xA496DA02, 0x57489AD3, 0x3C673505},
    {0xA496DA02, 0x891E00FD, 0xE231AF2B}, {0xF76E70A6, 0xCC25E002, 0xA70A4FD4}, {0xF76E70A6, 0xA602C954, 0xCD2D6682},
    {0xF76E70A6, 0x11A5BC8A, 0x7A8A135C}, {0xF76E70A6, 0x5644E7A1, 0x3D6B4877},
};
// Her Morph Ball (Echoes' ANCS 654A3F97, character Dark_Samus_MorphBall: one joint, so rigid), fit
// once to each Prime ball of PortDreadSamus::kBallTargets as kBalls' are: ANCS DarkBallId(ancs)
// (character 0 on CMDL DarkBallId(cmdl), CSKR DarkBallSkinId(cmdl)) and low-poly CMDL
// DarkBallId(low). While Dark Samus is played, the loader serves them as every suit's ball.
inline constexpr uint32_t kDarkBall = 0x2F2C1E6F;
inline constexpr int kDarkBallKey = 0x445342;      // "DSB"
inline constexpr int kDarkBallSkinKey = 0x445343;  // "DSC"
constexpr uint32_t DarkBallId(uint32_t id) { return PortModelVariant::Id(id, kDarkBallKey); }
constexpr uint32_t DarkBallSkinId(uint32_t id) { return PortModelVariant::Id(id, kDarkBallSkinKey); }
void SetPlayAsDarkSamus(bool on);
bool PlayAsDarkSamus();
// Whether the loaded mods serve Dark Samus: the setting was on when they last loaded, and the
// import has her.
bool DarkSamusServed();

}  // namespace PortEchoesSuits
