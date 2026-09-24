#pragma once
#include "GenerationBlockAccess.h"
#include "ChunkStorage.h"
#include <map>
#include <set>
#include <vector>
namespace console {
class GenerationRegion:public GenerationBlockAccess {
public:
    std::map<std::pair<int,int>,std::unique_ptr<ChunkStorage>> chunks;
    std::set<std::pair<int,int>> changedLightColumns;
    void insert(int x,int z,std::unique_ptr<ChunkStorage> chunk);
    int getTile(int x,int y,int z)const override;
    int getData(int x,int y,int z)const override;
    bool setTileAndData(int x,int y,int z,int tile,int data)override;
    int getHeightmap(int x,int z)const override;
    int getDaytimeRawBrightness(int x,int y,int z)const override;
    // Caller must populate both light layers first. Used by supplied light
    // fixtures until the original propagation engine is available.
    void acceptPreparedLight()override{lightingValid_=true;}
    bool hasPreparedLight()const override{return lightingValid_;}
    bool hasChunk(int x,int z)const override{return chunks.contains({x,z});}
    int getLight(LightLayer::variety,int x,int y,int z)const override;
    void setLight(LightLayer::variety,int x,int y,int z,int value)override;
    void initializeLight(Level&);
    // Split the two original lighting passes over multiple client frames.
    void beginLightInitialization();
    bool stepLightInitialization(Level&,int chunkBudget);
    void updateColumnLight(Level&,int x,int y,int z,int oldHeight)override;
    void lightColumnChanged(int x,int z)override{changedLightColumns.insert({x,z});}
private:
    bool lightingValid_=false;
    bool requireLightHalo_=false;
    std::vector<std::pair<int,int>> lightingKeys_;
    std::size_t lightingIndex_=0;
    int lightingPass_=0;
    ChunkStorage& at(int x,int z)const;
};
}
