#pragma once
#include "ConsoleGameRuleFile.h"
#include "CompoundTag.h"
#include <memory>
namespace console {
struct TutorialContainer {
 int x,y,z,dimension,facing;
 std::unique_ptr<CompoundTag> tag;
};
struct TutorialSpawner {
 int x,y,z,dimension;
 std::unique_ptr<CompoundTag> tag;
};
// The supplied tutorial's orientation-zero PlaceContainer actions.
std::vector<TutorialContainer> readTutorialContainers(const std::vector<ConsoleGameRuleNode>& rules);
// The supplied tutorial's single PlaceSpawner action and source-format tile data.
std::vector<TutorialSpawner> readTutorialSpawners(const std::vector<ConsoleGameRuleNode>& rules);
}
