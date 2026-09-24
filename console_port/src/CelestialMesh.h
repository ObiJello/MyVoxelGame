#pragma once
#include "TerrainMesh.h"
namespace console {
struct CelestialMesh { std::vector<Vertex> sun,moon; };
// Camera-relative quads from LevelRenderer::renderSky; original atlas phase order.
CelestialMesh buildCelestialMesh(std::int64_t time,float rain,float partialTick=1);
int consoleMoonPhase(std::int64_t time);
std::vector<Vertex> buildCloudMesh(Vec3 eye,std::int32_t animationTicks,const std::array<float,3>& colour,float partialTick=1);
}
