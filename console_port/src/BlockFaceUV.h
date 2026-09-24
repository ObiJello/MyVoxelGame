#pragma once
#include "World.h"
#include "BlockShape.h"
namespace console {
struct BlockUV {float u,v;};
std::array<BlockUV,4> consoleBoxFaceUV(Block block,int face,int data,BlockBox box);
// Normalized full-cube face UVs, in native terrain corner order.
const std::array<BlockUV,4>& consoleBlockFaceUV(Block block,int face,int data=0);
}
