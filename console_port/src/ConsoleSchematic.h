#pragma once
#include "ChunkStorage.h"
#include "CompoundTag.h"
#include <memory>
#include <span>
namespace console {
class ConsoleSchematic {
    int width_=0,height_=0,depth_=0;
    std::vector<unsigned char> bytes_;
public:
    std::unique_ptr<CompoundTag> tags;
    static ConsoleSchematic read(std::span<const unsigned char> bytes);
    int width()const{return width_;}int height()const{return height_;}int depth()const{return depth_;}
    // Deep-copy original entity NBT into the owning chunk's world coordinates.
    // Records are preserved here; live entity behavior is a separate integration.
    std::unique_ptr<CompoundTag> tagsForChunk(int chunkX,int chunkZ,int x,int y,int z)const;
    int block(int x,int y,int z)const;
    int data(int x,int y,int z)const;
    // Original unrotated ApplySchematic placement (all tutorial placements use rot=0).
    // Copies block/data only; caller must rebuild heightmaps and lighting.
    void apply(ChunkStorage& chunk,int chunkX,int chunkZ,int x,int y,int z)const;
};
}
