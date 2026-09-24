#pragma once
#include "BlockShape.h"
#include "World.h"
namespace console {
BlockShape consoleLadderShape(int data);
struct LadderVertex {float x,y,z,u,v;};
std::array<LadderVertex,4> consoleLadderQuad(int data);
Vec3 consoleLadderVelocity(Vec3 perSecond,bool sneaking);
}
