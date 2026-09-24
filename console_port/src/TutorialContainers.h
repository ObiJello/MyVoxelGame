#pragma once
#include "ConsoleGameRuleFile.h"
#include "CompoundTag.h"
#include "TutorialEngine.h"
#include <memory>
#include <optional>
#include <span>
namespace console {
struct TutorialContainer {
 int x,y,z,dimension,facing;
 std::unique_ptr<CompoundTag> tag;
};
struct TutorialSpawner {
 int x,y,z,dimension;
 std::unique_ptr<CompoundTag> tag;
};
// AddItemRuleDefinition (slot -1 = first free slot).
struct TutorialItem { int id=0,count=1,aux=0,dataTag=0,slot=-1; };
// UpdatePlayerRuleDefination::postProcessPlayer: applied to a new player
// (PlayerList::placeNewPlayer) and, while the food bar lesson is incomplete,
// to a respawned one.
struct TutorialUpdatePlayer {
 bool hasSpawn=false,hasYRot=false,hasHealth=false,hasFood=false;
 int spawnX=0,spawnY=0,spawnZ=0,health=0,food=0;
 float yRot=0;
 std::vector<TutorialItem> items;
};
// CollectItemRuleDefinition inside a CompleteAll goal.
struct TutorialCollectGoal { int itemId=0,aux=0,quantity=0,dataTag=0; };
// The tutorial's LevelRules: NamedArea boxes (LevelRuleset::getNamedArea),
// UpdatePlayer and the CompleteAll/CollectItem music disc goal.
struct TutorialLevelRules {
 std::map<std::wstring,TutorialArea> namedAreas;
 std::optional<TutorialUpdatePlayer> updatePlayer;
 std::wstring completeAllDescription;
 std::vector<TutorialCollectGoal> collectGoals;
};
TutorialLevelRules readTutorialLevelRules(const std::vector<ConsoleGameRuleNode>& rules);
// StringTable over a DLC languages.loc: the key/value strings of one locale
// (app.GetGameRulesString). Empty when the locale is absent.
std::map<std::wstring,std::wstring> readTutorialStrings(std::span<const unsigned char> loc,const std::wstring& locale=L"en-EN");
// The supplied tutorial's orientation-zero PlaceContainer actions.
std::vector<TutorialContainer> readTutorialContainers(const std::vector<ConsoleGameRuleNode>& rules);
// The supplied tutorial's single PlaceSpawner action and source-format tile data.
std::vector<TutorialSpawner> readTutorialSpawners(const std::vector<ConsoleGameRuleNode>& rules);
}
