#pragma once
#include "TerrainMesh.h"
namespace console {
std::vector<Vertex> buildExperienceOrbMesh(const ExperienceOrbState& orb,
    double yaw,double pitch,int packedLight);
}
