#pragma once
#include "TerrainMesh.h"

namespace console {
// The source ModelPart/Cube geometry, expressed as CPU triangles for the
// standalone shader renderer. Coordinates are the original 1/16-block units.
std::vector<Vertex> buildMobMesh(const SimulatedEntity& entity,int packedLight,bool coatOverlay=false);
std::vector<Vertex> buildMushroomCowMushrooms(const SimulatedEntity& entity,int packedLight);
}
