#pragma once
#include "ConsoleSchematic.h"
#include "ConsoleGameRuleFile.h"
#include "TutorialContainers.h"
#include <filesystem>
#include <map>
namespace console {
struct TutorialPlacement {std::string filename;int x,y,z;};
extern const std::int64_t tutorialSeed;
extern const std::array<int,3> tutorialSpawn;
extern const std::vector<TutorialPlacement> tutorialPlacements;
// The structure layer of the original tutorial, separate from base terrain,
// full entity simulation, forced features and tutorial scripting. Chest action
// storage is included; its live native UI is provided by WorldContainers.
class TutorialSchematics {
    std::map<std::string,ConsoleSchematic> files_;
    std::vector<TutorialPlacement> placements_;
    std::vector<ConsoleGameRuleNode> rules_;
    std::vector<TutorialContainer> containers_;
    std::vector<TutorialSpawner> spawners_;
public:
    explicit TutorialSchematics(const std::filesystem::path& tutorialAssets);
    explicit TutorialSchematics(std::span<const unsigned char> package);
    std::unique_ptr<CompoundTag> tagsForChunk(int chunkX,int chunkZ)const;
    bool apply(ChunkStorage& chunk,int originalChunkX,int originalChunkZ)const;
    const std::vector<TutorialContainer>& containers()const{return containers_;}
    const std::vector<TutorialSpawner>& spawners()const{return spawners_;}
    const std::vector<TutorialPlacement>& placements()const{return placements_;}
    const std::vector<ConsoleGameRuleNode>& rules()const{return rules_;}
    const ConsoleSchematic& schematic(const std::string& name)const{return files_.at(name);}
};
}
