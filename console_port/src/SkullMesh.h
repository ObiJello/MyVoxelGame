#pragma once
#include "TerrainMesh.h"
namespace console {
// SkullTileRenderer and SkeletonHeadModel's eight-pixel head cube.
std::vector<Vertex> buildSkullMesh(int x,int y,int z,int face,int rotation,int packedLight);
}
