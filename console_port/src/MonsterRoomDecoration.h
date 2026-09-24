#pragma once
#include <string>
#include <vector>
class Level;
class Random;
namespace console {
struct DungeonItem {int slot=0,id=0,count=0,damage=0,enchantmentId=-1,enchantmentLevel=0;};
struct DungeonTile {
    int x=0,y=0,z=0;
    bool spawner=false;
    std::wstring entityId;
    std::vector<DungeonItem> items;
};
// Source MonsterRoomFeature room geometry and placement RNG. The generation
// adapter records tile entities separately because its Level has no live NBT.
bool decorateMonsterRoom(Level& level,Random& random,int x,int y,int z,
                         std::vector<DungeonTile>& tiles);
}
