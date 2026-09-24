#pragma once
#include <array>
#include <cstdint>
namespace console {
std::array<float,3> consoleSkyColour(int biome,std::int64_t time,float rain=0,float thunder=0,int lightningTicks=0,float partialTick=1);
std::array<float,3> consoleCloudColour(std::int64_t time,float rain=0,float thunder=0,float partialTick=1);
}
