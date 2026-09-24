#pragma once
#include <array>
#include <cstdint>
namespace console {
struct LightmapInput {
    int dimension=0;
    float skyDarken=1;
    float flicker=0;
    bool lightning=false;
    int nightVisionTicks=-1;
    float partialTick=1;
};
using LightPixel=std::array<std::uint8_t,4>;
using Lightmap=std::array<LightPixel,256>;
// Rows are sky level, columns block level. Channels are explicit RGBA bytes,
// independent of the original PS3 integer's native endian representation.
Lightmap buildConsoleLightmap(const LightmapInput& input);
float consoleTimeOfDay(std::int64_t time,float partialTick=1,int dimension=0);
float consoleSkyDarken(std::int64_t time,float rain=0,float thunder=0,float partialTick=1,int dimension=0);
float consoleNightVisionScale(int duration,float partialTick);
class LightFlicker {
    float blr=0,blg=0,blrt=0,blgt=0;
public:
    void tick(const std::array<double,8>& randomValues);
    float red() const { return blr; }
    float green() const { return blg; }
};
}
