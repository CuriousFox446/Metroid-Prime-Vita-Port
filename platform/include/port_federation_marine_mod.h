#pragma once

#include <cstdint>

#include "port_echoes_suits_mod.h"
#include "port_model_variant.h"

// What the game and the mod loader need to know of the "federation-marine" mod (port_federation_marine.h,
// the import that makes it), without the import's headers.
//
// Echoes' Federation Marine (its Galactic_Trooper, a GF trooper) in place of Samus: the Power and
// Varia Suits wear the marine, the Gravity Suit the marine in the Gravity Suit's purple, the Phazon
// Suit Echoes' Dark Trooper with the marine's whole visor. Each look covers the suit's cutscene,
// in-game and pause screen bodies, its first-person left (grapple) arm, the first-person rifle (in
// place of the arm cannon and the suit arm) and its Morph and Spider Balls, in the look's colours.
// While the mod is on it wins over the echoes-suits mod for every suit.
namespace PortFederationMarine {

// The mod's folder in the mods folder, and its stamp: kVersion, then the build's commit. Bump
// kVersion when the output changes.
inline constexpr const char* kModName = "federation-marine";
inline constexpr const char* kStampName = ".federation-marine-version";
// 1: the marine, the Gravity marine and the Dark Trooper (0.46.0 test 50).
inline constexpr int kVersion = 1;

// The looks the import writes, each once, under its own ids (Wear of Prime's Varia resources, as the
// echoes-suits mod writes its looks).
enum Look : int { kMarine, kGravityMarine, kDarkTrooper, kLookCount };
inline constexpr int kLookKeys[kLookCount] = {0x464D4D, 0x464D47, 0x464D44};  // "FMM", "FMG", "FMD"
constexpr uint32_t Wear(int look, uint32_t id) { return PortModelVariant::Id(id, kLookKeys[look]); }
inline constexpr const char* kLookNames[kLookCount] = {"Federation Marine", "Federation Marine (Gravity)",
                                                       "Dark Trooper"};
// What each suit wears (CPlayerState::EPlayerSuit: Power, Gravity, Varia, Phazon).
inline constexpr int kSuitLooks[PortEchoesSuits::kSuitLookSuits] = {kMarine, kGravityMarine, kMarine, kDarkTrooper};

// The first-person rifle of a look: a static model (the marine's right forearm, hand and rifle) with
// its muzzle at the origin and its barrel up +Y, written as Wear(look, kRifleBase) and served under
// PortDreadSamus::FirstPersonCannon(suit): the gun draws it the Dread cannon's way, on its live
// muzzle locator in place of both Prime's cannon and the suit arm.
inline constexpr uint32_t kRifleBase = 0x4AD9BBAC;  // Prime's VariaArm, the id the variants derive from
// Prime's Spider Ball glass (SamusSpiderBallGlassCMDL; its sets teal, green, aqua): a look's glass is
// written as Wear(look, kSpiderBallGlass) and served under its suit variant. The Phazon Suit's own
// Spider Ball (SamusPhazonBallANCS) and glass stay Prime's: the Dark Trooper wears them.
inline constexpr uint32_t kSpiderBallGlass = 0xAE78BEBE;

// The first-person rifle's switch (the setting marine_first_person_rifle, on by default): off, the
// gun is Prime's own (or the Echoes cannon) with the suit arm, as without the mod. Applies at once.
void SetFirstPersonRifleShown(bool shown);
bool FirstPersonRifleShown();
// Whether the loaded mods serve the marine's rifle as the suit's first-person cannon.
bool ServesFirstPersonRifle(int suit);
// Whether the loaded mods serve the look of the suit at all (its bodies).
bool ServesSuit(int suit);

}  // namespace PortFederationMarine
