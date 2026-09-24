#pragma once
class CompoundTag;
#include <vector>
namespace console {
struct ContainerItem {int id=0,count=0,damage=0,marker=0;bool enchanted=false;};
std::vector<ContainerItem> containerItems(CompoundTag& root,int capacity);
int consoleItemStackLimit(int id);
// amount=-1: whole stack, 0: rounded-up half, positive: at most that many.
// Merge matching stacks across all destinations before using empty slots.
// Copies are prepared before the source and destination lists are replaced.
bool moveContainerStacks(CompoundTag& source,int sourceCapacity,int slot,
    const std::vector<CompoundTag*>& destinations,const std::vector<int>& capacities,int amount=-1);
bool moveContainerStack(CompoundTag& source,int sourceCapacity,int slot,CompoundTag& destination,int destinationCapacity,int amount=-1);
bool moveContainerStackToSlot(CompoundTag& source,int sourceCapacity,int slot,
    CompoundTag& destination,int destinationCapacity,int targetSlot,int amount=-1);
}
