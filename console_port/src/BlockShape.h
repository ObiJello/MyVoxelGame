#pragma once
#include <array>
namespace console {
struct BlockBox { double x0,y0,z0,x1,y1,z1; };
struct BlockShape {
    std::array<BlockBox,12> boxes{};
    int count=0;
};
struct BlockShapeAccess {
    virtual ~BlockShapeAccess()=default;
    virtual int getTile(int x,int y,int z)const=0;
    virtual int getData(int x,int y,int z)const=0;
};
bool consoleIsStair(int id);
bool consoleIsPartialBlock(int id);
BlockShape consoleCollisionShape(const BlockShapeAccess& level,int x,int y,int z);
BlockShape consoleRenderShape(const BlockShapeAccess& level,int x,int y,int z);
// Local collision boxes, using the original corner and upside-down rules.
BlockShape consoleStairShape(const BlockShapeAccess& level,int x,int y,int z);
}
