#pragma once
#include "World.h"
namespace console {
struct Vertex {
    float x,y,z,u,v,r,g,b,a;
    float lightU=0,lightV=0;
};
struct TerrainMesh {
    std::vector<Vertex> opaque,water,chests,chestLids,largeChests,largeChestLids;
    std::vector<Vertex> enderChests,enderChestLids;
    std::array<std::vector<Vertex>,5> skulls;
    std::vector<std::array<int,3>> enchantTables;
};
// CPU mesh construction is shared by the actual renderer and nonvisual tests.
// Shapes/textures remain the limited host mesh until the full TileRenderer port.
TerrainMesh buildTerrainMesh(const World& world);
// PistonPieceRenderer: the moved block drawn at its offset.
std::vector<Vertex> buildMovingPieceMesh(const World& world,const MovingPiece& piece);
// Mesh a horizontal subregion against a snapshot of the full visible window.
// Neighbor reads still use the full window, so stitching sections is seamless.
TerrainMesh buildTerrainMeshRegion(const World& world,const std::vector<std::uint8_t>& blocks,
                                   int x0,int z0,int regionWidth,int regionDepth);
}
