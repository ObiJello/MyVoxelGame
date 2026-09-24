#include "GenerationRegion.h"
#include "Mth.h"
#include "GenerationChunkLight.h"
#include <stdexcept>
namespace console {
void GenerationRegion::insert(int x,int z,std::unique_ptr<ChunkStorage> chunk){
    if(!chunk)throw std::invalid_argument("Cannot insert an empty chunk");
    chunks[{x,z}]=std::move(chunk);
    lightingValid_=false;
}
ChunkStorage& GenerationRegion::at(int x,int z)const {
    auto it=chunks.find({Mth::intFloorDiv(x,16),Mth::intFloorDiv(z,16)});
    if(it==chunks.end())throw std::out_of_range("Feature queried a chunk outside its prepared generation region");
    return *it->second;
}
int GenerationRegion::getTile(int x,int y,int z)const {if(y<0 || y>=256)return 0;return at(x,z).blocks[((x&15)*16+(z&15))*256+y];}
int GenerationRegion::getData(int x,int y,int z)const {if(y<0 || y>=256)return 0;return at(x,z).metadata.get(x&15,y,z&15);}
bool GenerationRegion::setTileAndData(int x,int y,int z,int tile,int data){
    if(y<0 || y>=256)return false;
    auto& chunk=at(x,z);int old=getTile(x,y,z);
    bool changed=chunk.set(x&15,y,z&15,tile,data);
    // Conservatively invalidate after any non-air block change, except placement
    // or removal of zero-emission plants. Their optical properties equal air.
    auto neutral=[](int t){return t==0 || t==6 || t==31 || t==32 || t==37 || t==38 || t==40 || t==81 || t==83 || t==106 || t==111 || t==127;};
    if(changed && !(neutral(old) && neutral(tile)))lightingValid_=false;
    return changed;
}
int GenerationRegion::getHeightmap(int x,int z)const {return at(x,z).heightmap[(x&15)*16+(z&15)];}
int GenerationRegion::getDaytimeRawBrightness(int x,int y,int z)const {
    if(!lightingValid_)throw std::logic_error("Generation brightness requires prepared, current light layers");
    if(requireLightHalo_){
        for(int cx=Mth::intFloorDiv(x-17,16);cx<=Mth::intFloorDiv(x+17,16);++cx)
            for(int cz=Mth::intFloorDiv(z-17,16);cz<=Mth::intFloorDiv(z+17,16);++cz)
                if(!hasChunk(cx,cz))throw std::logic_error("Generation brightness query requires a complete lighting halo");
    }
    if(y<0 || y>=256)throw std::out_of_range("Brightness coordinate outside chunk height");
    auto& chunk=at(x,z);
    return std::max(chunk.skyLight.get(x&15,y,z&15),chunk.blockLight.get(x&15,y,z&15));
}
int GenerationRegion::getLight(LightLayer::variety layer,int x,int y,int z)const {
    if(y<0 || y>=256)throw std::out_of_range("Light coordinate outside chunk height");
    auto& chunk=at(x,z);
    return (layer==LightLayer::Sky?chunk.skyLight:chunk.blockLight).get(x&15,y,z&15);
}
void GenerationRegion::setLight(LightLayer::variety layer,int x,int y,int z,int value){
    if(y<0 || y>=256 || value<0 || value>15)throw std::out_of_range("Invalid packed light write");
    auto& chunk=at(x,z);
    if((layer==LightLayer::Sky?chunk.skyLight:chunk.blockLight).get(x&15,y,z&15)!=value)lightColumnChanged(x,z);
    (layer==LightLayer::Sky?chunk.skyLight:chunk.blockLight).set(x&15,y,z&15,value);
}
void GenerationRegion::initializeLight(Level& level){
    beginLightInitialization();
    while(!stepLightInitialization(level,std::max(1,static_cast<int>(chunks.size())))){}
}
void GenerationRegion::beginLightInitialization(){
    lightingValid_=false;
    requireLightHalo_=true;
    lightingKeys_.clear();lightingKeys_.reserve(chunks.size());
    for(const auto& [position,chunk]:chunks)lightingKeys_.push_back(position);
    lightingIndex_=0;lightingPass_=1;
}
bool GenerationRegion::stepLightInitialization(Level& level,int chunkBudget){
    if(chunkBudget<1)throw std::invalid_argument("Lighting chunk budget must be positive");
    if(lightingPass_==0)return lightingValid_;
    if(lightingKeys_.empty()){lightingPass_=0;lightingValid_=true;return true;}
    for(int done=0;done<chunkBudget && lightingPass_!=0;++done){
        if(lightingIndex_==lightingKeys_.size()){
            if(lightingPass_==1){lightingPass_=2;lightingIndex_=0;}
            else {lightingPass_=0;lightingKeys_.clear();lightingValid_=true;break;}
        }
        if(lightingPass_==0)break;
        const auto position=lightingKeys_[lightingIndex_++];
        auto& chunk=*chunks.at(position);
        GenerationChunkLight light(chunk,level,position.first,position.second);
        if(lightingPass_==1){
            chunk.skyLight.setAll(0);chunk.blockLight.setAll(0);
            for(auto tile:chunk.blocks){Tile::lightBlockFor(tile);if(Tile::lightEmission[tile]>0)chunk.emissiveAdded=true;}
            light.recalcHeightmap();
        }else{
            if(!level.dimension->hasCeiling)light.recheckGaps(true);
            light.lightLava();
        }
    }
    return lightingValid_;
}
void GenerationRegion::updateColumnLight(Level& level,int x,int y,int z,int oldHeight){
    auto& chunk=at(x,z);
    chunk.heightmap[(x&15)*16+(z&15)]=oldHeight;
    GenerationChunkLight light(chunk,level,Mth::intFloorDiv(x,16),Mth::intFloorDiv(z,16));
    light.recalcHeight(x&15,y,z&15);
}
}
