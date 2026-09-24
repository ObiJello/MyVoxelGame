#pragma once
#include "ContainerItems.h"
class CompoundTag;
namespace console {
struct FurnaceRecipe { int id=0,damage=0; };
struct FurnaceProgress { int burn=0,cook=0,duration=200; };
FurnaceRecipe furnaceRecipe(int inputId);
int furnaceFuelDuration(int id);
float furnaceExperienceValue(int outputId);
FurnaceProgress furnaceProgress(CompoundTag& tile);
// Source FurnaceTileEntity::tick, using the native Items/BurnTime/CookTime NBT.
// Returns whether persistent state changed this tick.
bool tickFurnace(CompoundTag& tile);
}
