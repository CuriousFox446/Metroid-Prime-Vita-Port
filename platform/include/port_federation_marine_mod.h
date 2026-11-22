#pragma once

#include <cstdint>

#include "port_echoes_suits_mod.h"
#include "port_model_variant.h"

// What the game and the mod loader need to know of the Federation Marine (port_federation_marine.h,
// the import that makes it), without the import's headers.
//
// Echoes' Federation Marine (its Galactic_Trooper, a GF trooper) in place of Samus, "Play as a
// Federation Marine" under the Echoes suits' "Play as Dark Samus": the Power and Varia Suits wear the
// marine, the Gravity Suit the marine in the Gravity Suit's purple, the Phazon Suit Echoes' Dark Trooper
// with the marine's whole visor. Each look covers the suit's cutscene, in-game and pause screen bodies,
// its first-person left (grapple) arm, the first-person rifle (in place of the arm cannon and the suit
// arm) and its Morph and Spider Balls, in the look's colours. The Echoes suits import writes it into the
// echoes-suits mod (from version 19); 0.46.0 test 50 had it as a mod of its own, federation-marine.
namespace PortFederationMarine {

// The looks the import writes, each once, under its own ids (Wear of Prime's Varia resources, as the
// echoes-suits mod writes its looks).
enum Look : int { kMarine, kGravityMarine, kDarkTrooper, kLookCount };
inline constexpr int kLookKeys[kLookCount] = {0x464D4D, 0x464D47, 0x464D44};  // "FMM", "FMG", "FMD"
constexpr uint32_t Wear(int look, uint32_t id) { return PortModelVariant::Id(id, kLookKeys[look]); }
inline constexpr const char* kLookNames[kLookCount] = {"Federation Marine", "Federation Marine (Gravity)",
                                                       "Dark Trooper"};
// What each suit wears (CPlayerState::EPlayerSuit: Power, Gravity, Varia, Phazon).
inline constexpr int kSuitLooks[PortEchoesSuits::kSuitLookSuits] = {kMarine, kGravityMarine, kMarine, kDarkTrooper};

// The first-person rifle of a look: a static model (the marine's right forearm and rifle, without the
// hand) with its muzzle at the origin and its barrel up +Y, written as Wear(look, kRifleBase) and
// served under PortDreadSamus::FirstPersonCannon(suit): the gun draws it the Dread cannon's way, on its
// live muzzle locator in place of both Prime's cannon and the suit arm. Its muzzle sits on Prime's
// (0.46.0 test 51 drew it 7.5 cm lower, its shots with it; test 52 put it back up, the user's call).
inline constexpr uint32_t kRifleBase = 0x4AD9BBAC;  // Prime's VariaArm, the id the variants derive from
// Prime's Spider Ball glass (SamusSpiderBallGlassCMDL; its sets teal, green, aqua): a look's glass is
// written as Wear(look, kSpiderBallGlass) and served under its suit variant. The Phazon Suit's own
// Spider Ball (SamusPhazonBallANCS) and glass stay Prime's: the Dark Trooper wears them.
inline constexpr uint32_t kSpiderBallGlass = 0xAE78BEBE;

// "Play as a Federation Marine" (the setting echoes_federation_marine, off by default; the next mod
// reload applies it). It and "Play as Dark Samus" exclude each other: turning one on turns the other off.
void SetPlayAsMarine(bool on);
bool PlayAsMarine();
// Whether the loaded mods serve the marine: the setting was on when they last loaded, and the
// echoes-suits import has it.
bool MarineServed();
// The first-person rifle's switch (the setting marine_first_person_rifle, on by default): off, the
// gun is Prime's own (or the Echoes cannon) with the suit arm. Applies at once.
void SetFirstPersonRifleShown(bool shown);
bool FirstPersonRifleShown();
// Whether the loaded mods serve the marine's rifle as the suit's first-person cannon (and it is shown).
bool ServesFirstPersonRifle(int suit);

}  // namespace PortFederationMarine
