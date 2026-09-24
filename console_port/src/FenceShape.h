#pragma once
#include "BlockShape.h"
namespace console {
bool consoleIsFenceLike(int id);
BlockShape consoleFenceCollisionShape(const BlockShapeAccess& level,int x,int y,int z);
BlockShape consoleFenceRenderShape(const BlockShapeAccess& level,int x,int y,int z);
}
