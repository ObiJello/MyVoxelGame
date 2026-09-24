#pragma once
// No game-rule biome override is active for the standard generator boundary.
struct LevelGenerationOptions {
    void getBiomeOverride(int,unsigned char&,unsigned char&){}
    bool checkIntersects(int,int,int,int,int,int){return false;}
};
template<class... Args> inline void PIXBeginNamedEvent(int,const char*,Args...){}
inline void PIXEndNamedEvent(){}
inline constexpr int eTerrainFeature_Ravine=0;
struct GenerationApp {
    LevelGenerationOptions* getLevelGenerationOptions(){return nullptr;}
    // The original call records a debug-map marker; it does not modify terrain.
    void AddTerrainFeaturePosition(int,int,int){}
};
inline GenerationApp app;
