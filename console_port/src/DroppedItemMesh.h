#pragma once
#include "TerrainMesh.h"
namespace console {
// ItemRenderer for a dropped stack: full blocks are a quarter-size spinning
// cube (terrain atlas); plants and items are camera-facing sprites. Both bob
// by sin(age/10 + bobOffs)*.1 + .1 above the entity position.
struct DroppedItemMesh {
    std::vector<Vertex> terrain,items;
};
DroppedItemMesh buildDroppedItemMesh(const DroppedItem& item,double yaw,double pitch,int packedLight);
// FallingTileRenderer: the tile's full cube centred on the entity.
std::vector<Vertex> buildFallingBlockMesh(const FallingBlock& block,int packedLight);
// LevelRenderer destroy-stage overlay: the cube at (x,y,z) textured with
// terrain destroy_<stage> (atlas tiles 240-249).
std::vector<Vertex> buildDestroyStageMesh(int x,int y,int z,int stage,int packedLight);
}
