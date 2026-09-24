#pragma once
#include "World.h"
namespace console {
// Returns original Facing IDs (down/up/north/south/west/east), not mesh face IDs.
// Feet coordinates and yaw in radians use the native client camera convention.
int consolePlacementFacing(int x,int y,int z,Vec3 feet,double yaw);
int consoleLogPlacementData(int data,int facing);
}
