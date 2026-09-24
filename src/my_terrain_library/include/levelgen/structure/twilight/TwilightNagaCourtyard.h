#pragma once

#include "levelgen/structure/TwilightStructures.h"

// Twilight Forest 4.9 — the Naga Courtyard ("twilightforest:naga_courtyard"):
// type/NagaCourtyardStructure.java + courtyard/*.java (CourtyardMain,
// StructureMazeGenerator, the hedge / terrace / path / wall pieces).
//
// twilight_pieces::buildNagaCourtyard and nagaCourtyardTerraformer
// (declared in TwilightStructures.h) are defined in TwilightNagaCourtyard.cpp.
//
// NagaCourtyardStructure overrides adjustForTerrain
// (WorldUtil.adjustForTerrain(context, x, z, 40, 4) + 2), so the builder
// derives its own y from x/z instead of the dispatcher's generic value.
// No boss: the courtyard/spawner template keeps its naga_boss_spawner
// palette entry, which the template loader resolves to its stand-in marker.
//
// Piece types (TFStructurePieceTypes, lower-cased registry names):
//   twilightforest:tfncmn   CourtyardMain (the maze; places nothing itself)
//   twilightforest:tfnccp   NagaCourtyardHedgeCapComponent
//   twilightforest:tfnccpp  NagaCourtyardHedgeCapPillarComponent
//   twilightforest:tfnccr   NagaCourtyardHedgeCornerComponent
//   twilightforest:tfncln   NagaCourtyardHedgeLineComponent
//   twilightforest:tfnct    NagaCourtyardHedgeTJunctionComponent
//   twilightforest:tfncis   NagaCourtyardHedgeIntersectionComponent
//   twilightforest:tfncpd   NagaCourtyardHedgePadderComponent
//   twilightforest:tfncte   CourtyardTerrace
//   twilightforest:tfnche   CourtyardTerraceHedge
//   twilightforest:tfncpa   CourtyardPathPiece
//   twilightforest:tfncwl   CourtyardWall
//   twilightforest:tfncwp   CourtyardWallPadder
//   twilightforest:tfncwc   CourtyardWallCornerOuter
//   twilightforest:tfncwa   CourtyardWallCornerInner
//   twilightforest:tfjigsawtemplate  the center (boss spawner) jigsaw piece
