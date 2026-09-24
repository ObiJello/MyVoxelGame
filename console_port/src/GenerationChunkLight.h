#pragma once
#include "WorldGenLevel.h"
#include "ChunkStorage.h"
#include "GenerationOptions.h"

// A storage adapter for original LevelChunk lighting methods, not the complete
// LevelChunk class. Section views keep the original 128-high get/set calls intact.
class GenerationChunkLight {
    struct CompressedTileStorage {
        console::ChunkStorage& chunk;int base;
        int get(int x,int y,int z)const{return chunk.blocks[(x*16+z)*256+base+y];}
    };
    struct SparseLightStorage {
        DataLayer& data;int base;
        int get(int x,int y,int z)const{return data.get(x,base+y,z);}
        void set(int x,int y,int z,int value){data.set(x,base+y,z,value);}
    };
    struct ColumnView {
        std::array<std::uint16_t,256>& values;
        std::uint16_t& operator[](int index){return values[(index&15)*16+(index>>4)];}
    };
    console::ChunkStorage& storage;
    Level* level;
    int x,z;
    CompressedTileStorage lowerBlockView,upperBlockView;
    SparseLightStorage lowerSkyView,upperSkyView;
    CompressedTileStorage *lowerBlocks=&lowerBlockView,*upperBlocks=&upperBlockView;
    SparseLightStorage *lowerSkyLight=&lowerSkyView,*upperSkyLight=&upperSkyView;
    ColumnView heightmap;
    std::array<unsigned char,128>& columnFlags;
    bool &hasGapsToCheck,&emissiveAdded;
    int& minHeight;
    static constexpr int eColumnFlag_recheck=1;
    void setUnsaved(bool changed){storage.unsaved=changed;}
    bool isEmpty()const{return false;} // Regular chunks, never the EmptyLevelChunk sentinel.
    int getTile(int x,int y,int z)const{return storage.blocks[(x*16+z)*256+y];}
    int getHeightmap(int x,int z)const{return storage.heightmap[x*16+z];}
    int getBrightness(LightLayer::variety layer,int x,int y,int z)const{
        return (layer==LightLayer::Sky?storage.skyLight:storage.blockLight).get(x,y,z);
    }
public:
    GenerationChunkLight(console::ChunkStorage& chunk,Level& world,int cx,int cz)
        :storage(chunk),level(&world),x(cx),z(cz),lowerBlockView{chunk,0},upperBlockView{chunk,128},
         lowerSkyView{chunk.skyLight,0},upperSkyView{chunk.skyLight,128},heightmap{chunk.heightmap},
         columnFlags(chunk.columnFlags),hasGapsToCheck(chunk.hasGapsToCheck),emissiveAdded(chunk.emissiveAdded),minHeight(chunk.minHeight){}
    void recalcHeightmap();
    void lightLava();
    void lightGaps(int x,int z);
    void recheckGaps(bool force);
    void lightGap(int x,int z,int source);
    void lightGap(int x,int z,int y1,int y2);
    void recalcHeight(int x,int yStart,int z);
};
