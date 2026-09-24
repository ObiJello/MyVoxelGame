#pragma once
#include "TerrainMesh.h"
#include <functional>
namespace console {
struct HangingMesh {
    std::vector<Vertex> painting,frame,itemTerrain,itemAtlas;
};
HangingMesh buildHangingMesh(const HangingDecoration& decoration,int packedLight,
    const std::function<int(int,int,int)>& paintingLight={});
}
