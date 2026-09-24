#pragma once
#include "BiomeGenerator.h"
#include "Mth.h"
#include "Biome.h"
#include "ArrayWithLength.h"
#include "GenerationBlockAccess.h"
#include "net.minecraft.world.level.tile.h"
#include <algorithm>
#include <functional>

// The generation-only Level interface. This is not the simulation Level port.
class WorldGenBiomeSource {
    console::BiomeGenerator generator_;
    int fixedBiome_=-1;
public:
    void setFixedBiome(int id){if(id<0 || id>=23)throw std::invalid_argument("Invalid fixed biome");fixedBiome_=id;}
    WorldGenBiomeSource(std::int64_t seed,console::BiomeScale scale):generator_(seed,scale){}
    void getBlock(BiomeArray& output,int x,int z,int width,int depth,bool raw){
        if(width<1 || depth<1 || width>2048 || depth>2048 || x<-30000000 || z<-30000000 ||
           x>30000000-width || z>30000000-depth)throw std::invalid_argument("Biome area outside supported bounds");
        auto ids=fixedBiome_>=0?std::vector<std::uint8_t>(std::size_t(width)*depth,static_cast<std::uint8_t>(fixedBiome_)):generator_.area(x,z,width,depth,raw);
        if(output.length<ids.size()){delete[] output.data;output=BiomeArray(ids.size());}
        for(std::size_t i=0;i<ids.size();++i)output[i]=Biome::biomes[ids[i]];
    }
    void getRawBiomeBlock(BiomeArray& output,int x,int z,int width,int depth){getBlock(output,x,z,width,depth,true);}
    void getBiomeBlock(BiomeArray& output,int x,int z,int width,int depth,bool){getBlock(output,x,z,width,depth,false);}
    Biome* getBiome(int x,int z){return Biome::biomes[fixedBiome_>=0?fixedBiome_:generator_.area(x,z,1,1)[0]];}
};
struct WorldGenLevelData { int chunks;bool hasCeiling=false;int hellScale=3;int getXZSize()const{return chunks;} int getHellScale()const{return hellScale;} };
class Level {
    using lightCache_t=std::uint64_t;
    using __uint64=std::uint64_t;
    std::int64_t seed_;
    WorldGenBiomeSource biomes_;
    WorldGenLevelData data_;
    GenerationBlockAccess* blockAccess_=nullptr;
    std::function<void(int,Level&,int,int,int,Random&)> liquidTick_;
    static inline thread_local bool instaTick_=false;
    std::mutex m_checkLightCS;
    int toCheckLevel[32*32*32]{};
    void initCache(lightCache_t* cache){assert(!cache);}
    void flushCache(lightCache_t* cache,std::uint64_t,LightLayer::variety){assert(!cache);}
    int getBrightnessCached(lightCache_t*,LightLayer::variety layer,int x,int y,int z){return getBrightness(layer,x,y,z);}
    int getBlockingCached(lightCache_t*,LightLayer::variety,int* ct,int x,int y,int z){
        int tile=getTile(x,y,z);if(ct)*ct=tile;return Tile::lightBlockFor(tile);
    }
    int getEmissionCached(lightCache_t*,int ct,int,int,int){return Tile::lightEmission[ct];}
    void setBrightnessCached(lightCache_t*,std::uint64_t*,LightLayer::variety layer,int x,int y,int z,int value){
        if(y>=0 && y<maxBuildHeight && hasChunkAt(x,y,z))blocks().setLight(layer,x,y,z,value);
    }
    int getExpectedSkyColor(lightCache_t*,int,int,int,int,int,int);
    int getExpectedBlockColor(lightCache_t*,int,int,int,int,int,int,bool);
    GenerationBlockAccess& blocks()const {if(!blockAccess_)throw std::logic_error("Generation feature requires a block region");return *blockAccess_;}
public:
    WorldGenLevelData* dimension=&data_;
    static constexpr int genDepthBits=7,genDepthBitsPlusFour=11,genDepth=128,genDepthMinusOne=127;
    static constexpr int maxBuildHeight=256;
    static constexpr int COMPRESSED_CHUNK_SECTION_HEIGHT=128,MAX_BRIGHTNESS=15;
    int seaLevel=63;
    Level(std::int64_t seed,int chunks=54,console::BiomeScale scale=console::BiomeScale::Normal):seed_(seed),biomes_(seed,scale),data_{chunks}{}
    std::int64_t getSeed()const{return seed_;}
    int getSeaLevel()const{return seaLevel;}
    WorldGenBiomeSource* getBiomeSource(){return &biomes_;}
    Biome* getBiome(int x,int z){return biomes_.getBiome(x,z);}
    WorldGenLevelData* getLevelData(){return &data_;}
    void setBlockAccess(GenerationBlockAccess& region){initializeGenerationTiles();blockAccess_=&region;}
    int getTile(int x,int y,int z)const{return blocks().getTile(x,y,z);}
    Material* getMaterial(int x,int y,int z)const{return Tile::materialFor(getTile(x,y,z));}
    void setInstaTick(bool enabled){instaTick_=enabled;}
    bool getInstaTick()const{return instaTick_;}
    // Generation-only hook for the original SpringFeature's immediate liquid
    // tick. The full live liquid simulation is outside this Level adapter.
    void setGenerationLiquidTickHandler(std::function<void(int,Level&,int,int,int,Random&)> handler){
        liquidTick_=std::move(handler);
    }
    void tickGenerationLiquid(int tile,int x,int y,int z,Random& random){
        if(liquidTick_)liquidTick_(tile,*this,x,y,z,random);
    }
    int getData(int x,int y,int z)const{return blocks().getData(x,y,z);}
    bool isEmptyTile(int x,int y,int z)const{return getTile(x,y,z)==0;}
    int getHeightmap(int x,int z)const{return blocks().getHeightmap(x,z);}
    bool canSeeSky(int x,int y,int z)const{return y>=getHeightmap(x,z);}
    int getDaytimeRawBrightness(int x,int y,int z)const{
        if(y<0)return 0;
        y=std::min(y,maxBuildHeight-1);
        int brightness=blocks().getDaytimeRawBrightness(x,y,z);
        return dimension->hasCeiling?blocks().getLight(LightLayer::Block,x,y,z):brightness;
    }
    bool isTopSolidBlocking(int x,int y,int z);
    bool shouldFreezeIgnoreNeighbors(int x,int y,int z);
    bool shouldFreeze(int x,int y,int z);
    bool shouldFreeze(int x,int y,int z,bool checkNeighbors);
    int getTopRainBlock(int x,int z);
    bool shouldSnow(int x,int y,int z);
    void lightColumnChanged(int x,int z,int,int){blocks().lightColumnChanged(x,z);}
    bool hasChunkAt(int x,int,int z)const{return blocks().hasChunk(Mth::intFloorDiv(x,16),Mth::intFloorDiv(z,16));}
    bool hasChunksAt(int x,int y,int z,int radius)const{
        if(y+radius<0 || y-radius>=maxBuildHeight)return false;
        for(int cx=Mth::intFloorDiv(x-radius,16);cx<=Mth::intFloorDiv(x+radius,16);++cx)
            for(int cz=Mth::intFloorDiv(z-radius,16);cz<=Mth::intFloorDiv(z+radius,16);++cz)
                if(!blocks().hasChunk(cx,cz))return false;
        return true;
    }
    int getBrightness(LightLayer::variety layer,int x,int y,int z){
        int cx=Mth::intFloorDiv(x,16),cz=Mth::intFloorDiv(z,16);
        if(cx< -data_.chunks/2 || cx>=data_.chunks/2 || cz< -data_.chunks/2 || cz>=data_.chunks/2)return 0;
        if(!blocks().hasChunk(cx,cz))return int(layer);
        return blocks().getLight(layer,x,std::clamp(y,0,maxBuildHeight-1),z);
    }
    void checkLight(int x,int y,int z,bool force=false,bool rootOnlyEmissive=false);
    void checkLight(LightLayer::variety layer,int x,int y,int z,bool force=false,bool rootOnlyEmissive=false);
    bool setTileAndDataNoUpdate(int x,int y,int z,int tile,int data){
        if(y<0 || y>=maxBuildHeight)return false;
        int old=getTile(x,y,z),oldHeight=getHeightmap(x,z);bool ready=blocks().hasPreparedLight();
        bool changed=blocks().setTileAndData(x,y,z,tile,data);
        if(changed && ready && hasChunksAt(x,y,z,17)){
            if(!dimension->hasCeiling && Tile::lightBlock[old]!=Tile::lightBlock[tile]){
                if((Tile::lightBlock[tile]!=0 && y>=oldHeight) || (Tile::lightBlock[tile]==0 && y==oldHeight-1))
                    blocks().updateColumnLight(*this,x,Tile::lightBlock[tile]!=0?y+1:y,z,oldHeight);
            }
            checkLight(x,y,z);blocks().acceptPreparedLight();
        }
        return changed;
    }
    bool setTileNoUpdate(int x,int y,int z,int tile){return setTileAndDataNoUpdate(x,y,z,tile,0);}
    bool setTile(int x,int y,int z,int tile){return setTileNoUpdate(x,y,z,tile);}
    bool setTileNoUpdateNoLightCheck(int x,int y,int z,int tile){return blocks().setTileAndData(x,y,z,tile,0);}
    bool setTileAndData(int,int,int,int,int){throw std::logic_error("Live block notifications have not been ported; use feature doUpdate=false during generation");}
};
