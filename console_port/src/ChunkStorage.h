#pragma once
#include "ChunkGenerator.h"
#include "DataLayer.h"
#include <array>
namespace console {
// Original 16x16x256 column order and packed nibbles, independent of rendering.
class ChunkStorage {
public:
    static constexpr int height=256,tileCount=16*16*height;
    std::array<std::uint8_t,tileCount> blocks{};
    DataLayer metadata{tileCount,8},skyLight{tileCount,8},blockLight{tileCount,8};
    std::array<std::uint8_t,256> biomes{};
    // Keep height 256 representable; original byte heights wrap at this ceiling.
    std::array<std::uint16_t,256> heightmap{};
    std::array<unsigned char,128> columnFlags{};
    bool hasGapsToCheck=false,emissiveAdded=false,unsaved=false;
    int minHeight=0;
    ChunkStorage()=default;
    explicit ChunkStorage(const GeneratedChunk& generated);
    bool set(int x,int y,int z,int tile,int data=0);
    void recalculateHeightmap();
};
}
