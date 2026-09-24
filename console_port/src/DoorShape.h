#pragma once
#include "BlockShape.h"
namespace console {
int consoleDoorData(const BlockShapeAccess& level,int x,int y,int z);
BlockShape consoleDoorShape(const BlockShapeAccess& level,int x,int y,int z);
struct DoorTexture {int tile;bool flip;};
DoorTexture consoleDoorTexture(const BlockShapeAccess& level,int x,int y,int z,int face);
}
