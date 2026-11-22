#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "port_echoes_suits.h"
#include "port_federation_marine_mod.h"

// The Federation Marine import: Echoes' Federation Marine and Dark Trooper from the user's own
// Metroid Prime 2: Echoes disc (GameCube, G2ME01, or Trilogy's MP2 folder) in place of Samus, run by
// the Echoes suits import into the echoes-suits mod ("Play as a Federation Marine",
// port_federation_marine_mod.h).
//
// Both troopers (Metroid1.pak: Galactic_Trooper A93225BD, Dark_Trooper F7507082) are skinned on a copy
// of Echoes Samus's skeleton: the same joint names (her Varia shoulder-pad joints and left-hand
// fingers among them), every head within 3.2 mm of hers. So they convert onto Prime's skeletons as the
// echoes-suits bodies do (PortEchoesSuits::ConvertCskr): the right wrist, holding the rifle, folds onto
// the elbow, where Samus's cannon rides. Built from them:
// - the bodies (cutscene, in-game, the pause screen's doll), the marine's visor glass made opaque,
//   the rifle turned about the forearm so it stands upright in Samus's poses, the right hand left out
//   (its fingers stuck out through the rifle's frame; the rifle's own grip stands for it);
// - the Dark Trooper with its broken visor taken out and the marine's whole visor merged in;
// - the Gravity marine: the limbs' plates (shoulder pads, arms, hands, thighs, knees, shins, boots:
//   the parts Prime's Gravity Suit paints purple) in its purple, the visor and lights in its aqua;
// - the first-person left arm: the body's left forearm and hand, turned into the grapple arm's frame
//   (CINF 516F4445) and skinned on its joints by name;
// - the first-person rifle: the body's right forearm and rifle (no hand, as the bodies), turned down
//   the gun's barrel axis with the muzzle at the origin, unskinned;
// - the Morph and Spider Balls: Prime's own, their shells in the look's armour colour and their lights
//   in its light colour (the Dark Trooper keeps Prime's Phazon Spider Ball, already dark).
//
// Everything comes and goes through PortEchoesSuits::Io (the same disc reader as the echoes-suits
// import's). The output is the user's, made from their own copies; nothing of either game ships.
namespace PortFederationMarine {

struct Result {
  size_t bodies = 0;   // cutscene, in-game and doll bodies written
  size_t arms = 0;     // first-person left arms
  size_t rifles = 0;   // first-person rifles
  size_t balls = 0;    // Morph and Spider Balls (and their glass)
  std::vector<std::string> files;  // every file written, in order
};

// Converts and writes every look. False on any failure or cancellation, with the reason in `error`;
// the caller then discards what was written.
bool Import(const PortEchoesSuits::Io& io, Result& result, std::string& error);

// The rotation that turns the T-posed right arm (+x, up +z) down the gun's barrel (+y, up +z), and
// the left arm (-x, thumb forward +y) into the grapple arm's frame (+y, thumb up +z): p' = R p.
void RifleFrame(const float (&in)[3], float (&out)[3]);
void GrappleFrame(const float (&in)[3], float (&out)[3]);

// A model's material set 0 without the materials no surface draws and the textures no material uses.
bool PruneMaterials(PortEchoesSuits::PrimeCmdl& model, std::string* error);

}  // namespace PortFederationMarine
