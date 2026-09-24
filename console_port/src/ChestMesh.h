#pragma once
#include "TerrainMesh.h"
namespace console {
// Original single ChestModel closed pose, Cube face layout and Polygon UVs.
std::vector<Vertex> buildChestMesh(int x,int y,int z,int facing,int light,bool large=false);
}
