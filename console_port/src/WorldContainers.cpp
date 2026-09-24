#include "WorldState.h"
#include "ContainerItems.h"
#include "FurnaceSimulation.h"
#include "BrewingSimulation.h"
#include <algorithm>
#include <cmath>
namespace console {
CompoundTag* World::State::chest(int x,int y,int z)const{
 auto it=records.find({Mth::intFloorDiv(x-width/2,16),Mth::intFloorDiv(z-depth/2,16)});
 if(it==records.end() || !it->second->extra)return nullptr;
 auto* list=dynamic_cast<TagList*>(it->second->extra->get(L"TileEntities"));if(!list)return nullptr;
 for(int i=0;i<list->size();++i)if(auto* t=dynamic_cast<CompoundTag*>(list->get(i)))
  if(t->getString(L"id")==L"Chest" && t->getInt(L"x")==x-width/2 && t->getInt(L"y")==y && t->getInt(L"z")==z-depth/2)return t;
 return nullptr;
}
CompoundTag* World::State::furnace(int x,int y,int z)const{
 auto it=records.find({Mth::intFloorDiv(x-width/2,16),Mth::intFloorDiv(z-depth/2,16)});
 if(it==records.end() || !it->second->extra)return nullptr;
 auto* list=dynamic_cast<TagList*>(it->second->extra->get(L"TileEntities"));if(!list)return nullptr;
 for(int i=0;i<list->size();++i)if(auto* t=dynamic_cast<CompoundTag*>(list->get(i)))
  if(t->getString(L"id")==L"Furnace" && t->getInt(L"x")==x-width/2 &&
     t->getInt(L"y")==y && t->getInt(L"z")==z-depth/2)return t;
 return nullptr;
}
CompoundTag* World::State::brewingStand(int x,int y,int z)const{
 auto it=records.find({Mth::intFloorDiv(x-width/2,16),Mth::intFloorDiv(z-depth/2,16)});
 if(it==records.end() || !it->second->extra)return nullptr;
 auto* list=dynamic_cast<TagList*>(it->second->extra->get(L"TileEntities"));if(!list)return nullptr;
 for(int i=0;i<list->size();++i)if(auto* t=dynamic_cast<CompoundTag*>(list->get(i)))
  if(t->getString(L"id")==L"Cauldron" && t->getInt(L"x")==x-width/2 &&
     t->getInt(L"y")==y && t->getInt(L"z")==z-depth/2)return t;
 return nullptr;
}
void World::ensureBrewingData(int x,int y,int z){
 if(state->brewingStand(x,y,z))return;
 const int nativeX=x-width/2,nativeZ=z-depth/2;
 const auto key=std::pair{Mth::intFloorDiv(nativeX,16),Mth::intFloorDiv(nativeZ,16)};
 auto& record=state->records[key];
 if(!record){record=std::make_unique<ChunkRecord>();record->x=key.first;record->z=key.second;}
 if(!record->extra)record->extra=std::make_unique<CompoundTag>();
 auto* list=dynamic_cast<TagList*>(record->extra->get(L"TileEntities"));
 if(!list){record->extra->put(L"TileEntities",new TagList());list=record->extra->getList(L"TileEntities");}
 auto tag=std::make_unique<CompoundTag>();tag->putString(L"id",L"Cauldron");
 tag->putInt(L"x",nativeX);tag->putInt(L"y",y);tag->putInt(L"z",nativeZ);
 tag->putShort(L"BrewTime",0);tag->put(L"Items",new TagList());
 list->add(tag.get());tag.release();state->chunk(x,z).unsaved=true;
}
void World::ensureFurnaceData(int x,int y,int z){
 if(state->furnace(x,y,z))return;
 const int nativeX=x-width/2,nativeZ=z-depth/2;
 const auto key=std::pair{Mth::intFloorDiv(nativeX,16),Mth::intFloorDiv(nativeZ,16)};
 auto& record=state->records[key];
 if(!record){record=std::make_unique<ChunkRecord>();record->x=key.first;record->z=key.second;}
 if(!record->extra)record->extra=std::make_unique<CompoundTag>();
 auto* list=dynamic_cast<TagList*>(record->extra->get(L"TileEntities"));
 if(!list){record->extra->put(L"TileEntities",new TagList());list=record->extra->getList(L"TileEntities");}
 auto tag=std::make_unique<CompoundTag>();tag->putString(L"id",L"Furnace");
 tag->putInt(L"x",nativeX);tag->putInt(L"y",y);tag->putInt(L"z",nativeZ);
 tag->putShort(L"BurnTime",0);tag->putShort(L"CookTime",0);tag->putBoolean(L"CharcoalUsed",false);
 tag->put(L"Items",new TagList());list->add(tag.get());tag.release();
 state->chunk(x,z).unsaved=true;
}
void World::discardContainerData(int x,int y,int z,const wchar_t* id){
 auto it=state->records.find({Mth::intFloorDiv(x-width/2,16),Mth::intFloorDiv(z-depth/2,16)});
 if(it==state->records.end() || !it->second->extra)return;
 auto& root=*it->second->extra;auto* list=dynamic_cast<TagList*>(root.get(L"TileEntities"));if(!list)return;
 auto kept=std::make_unique<TagList>();for(int i=0;i<list->size();++i){auto* t=dynamic_cast<CompoundTag*>(list->get(i));
  if(t && t->getString(L"id")==id && t->getInt(L"x")==x-width/2 && t->getInt(L"y")==y && t->getInt(L"z")==z-depth/2)continue;
  std::unique_ptr<Tag> copy(list->get(i)->copy());kept->add(copy.get());copy.release();
 }
 root.put(L"TileEntities",kept.get());kept.release();
}
void World::ensureEnderChestData(int x,int y,int z){
 const int nativeX=x-width/2,nativeZ=z-depth/2;
 const auto key=std::pair{Mth::intFloorDiv(nativeX,16),Mth::intFloorDiv(nativeZ,16)};
 auto& record=state->records[key];
 if(!record){record=std::make_unique<ChunkRecord>();record->x=key.first;record->z=key.second;}
 if(!record->extra)record->extra=std::make_unique<CompoundTag>();
 auto* list=dynamic_cast<TagList*>(record->extra->get(L"TileEntities"));
 if(!list){record->extra->put(L"TileEntities",new TagList());list=record->extra->getList(L"TileEntities");}
 for(int i=0;i<list->size();++i)if(auto* tag=dynamic_cast<CompoundTag*>(list->get(i)))
  if(tag->getString(L"id")==L"EnderChest" && tag->getInt(L"x")==nativeX &&
     tag->getInt(L"y")==y && tag->getInt(L"z")==nativeZ)return;
 auto tag=std::make_unique<CompoundTag>();tag->putString(L"id",L"EnderChest");
 tag->putInt(L"x",nativeX);tag->putInt(L"y",y);tag->putInt(L"z",nativeZ);
 list->add(tag.get());tag.release();
}
std::vector<std::array<int,3>> World::chestParts(int x,int y,int z)const{
 if(get(x,y,z)!=54)return {};
 std::vector<std::array<int,3>> parts{{x,y,z}};
 // ChestTile::use prepends west/north and appends east/south, matching
 // CompoundContainer's slot ordering regardless of the half clicked.
 for(const auto& offset:std::array<std::array<int,2>,4>{{{-1,0},{1,0},{0,-1},{0,1}}}){
  int xx=x+offset[0],zz=z+offset[1];if(get(xx,y,zz)!=54)continue;
  if(parts.size()!=1)return {}; // Reject illegal three-or-more chest clusters.
  if(offset[0]<0 || offset[1]<0)parts.insert(parts.begin(),{xx,y,zz});else parts.push_back({xx,y,zz});
 }
 if(parts.size()==2)for(const auto& p:parts){int neighbors=0;
  for(const auto& d:std::array<std::array<int,2>,4>{{{-1,0},{1,0},{0,-1},{0,1}}})neighbors+=get(p[0]+d[0],y,p[2]+d[1])==54;
  if(neighbors!=1)return {};
 }
 return parts;
}
bool World::canOpenChest(int x,int y,int z)const{
 const auto parts=chestParts(x,y,z);if(parts.empty())return false;
 for(const auto& p:parts)if(Tile::solid[get(p[0],p[1]+1,p[2])])return false;
 return true;
}
bool World::canOpenEnderChest(int x,int y,int z)const{
 return get(x,y,z)==static_cast<Block>(130) && !Tile::solid[get(x,y+1,z)];
}
bool World::canOpenFurnace(int x,int y,int z)const{
 const int id=get(x,y,z);return id==61 || id==62;
}
bool World::canOpenBrewingStand(int x,int y,int z)const{return get(x,y,z)==static_cast<Block>(117);}
std::vector<ContainerItem> World::brewingItems(int x,int y,int z)const{
 if(!canOpenBrewingStand(x,y,z))return {};
 auto* tile=state->brewingStand(x,y,z);return tile?containerItems(*tile,4):std::vector<ContainerItem>(4);
}
int World::brewingProgress(int x,int y,int z)const{
 if(!canOpenBrewingStand(x,y,z))return 0;
 auto* tile=state->brewingStand(x,y,z);return tile?brewingTime(*tile):0;
}
bool World::transferBrewingItem(int x,int y,int z,int slot,bool take,int targetSlot,int amount){
 if(!canOpenBrewingStand(x,y,z))return false;
 ensureBrewingData(x,y,z);auto* tile=state->brewingStand(x,y,z);if(!tile)return false;
 bool moved=false;
 if(take){if(slot<0 || slot>=4)return false;
  moved=moveContainerStack(*tile,4,slot,*state->inventory,36,amount);
 }else{
  if(slot<0 || slot>=36 || targetSlot<0 || targetSlot>=4)return false;
  auto carried=carriedItems();if(!carried[slot].id)return false;
  if(targetSlot==3){if(!isBrewingIngredient(carried[slot].id))return false;}
  else{
   if(carried[slot].id!=373 && carried[slot].id!=374)return false;
   if(brewingItems(x,y,z)[targetSlot].id)return false;
   amount=1;
  }
  moved=moveContainerStackToSlot(*state->inventory,36,slot,*tile,4,targetSlot,amount);
 }
 if(moved){
  state->chunk(x,z).unsaved=true;++revision;
  int bits=brewingBottleBits(*tile);if(getData(x,y,z)!=bits)setData(x,y,z,bits);
 }
 return moved;
}
std::vector<ContainerItem> World::furnaceItems(int x,int y,int z)const{
 if(!canOpenFurnace(x,y,z))return {};
 auto* tile=state->furnace(x,y,z);return tile?containerItems(*tile,3):std::vector<ContainerItem>(3);
}
World::FurnaceState World::furnaceState(int x,int y,int z)const{
 if(!canOpenFurnace(x,y,z))return {};
 auto* tile=state->furnace(x,y,z);if(!tile)return {};
 auto progress=furnaceProgress(*tile);return {progress.burn,progress.cook,progress.duration};
}
bool World::transferFurnaceItem(int x,int y,int z,int slot,bool take,int targetSlot,int amount){
 if(!canOpenFurnace(x,y,z))return false;
 ensureFurnaceData(x,y,z);auto* tile=state->furnace(x,y,z);if(!tile)return false;
 bool moved=false;
 if(take){if(slot<0 || slot>=3)return false;
  const auto before=slot==2?containerItems(*tile,3)[2]:ContainerItem{};
  moved=moveContainerStack(*tile,3,slot,*state->inventory,36,amount);
  if(moved){
   tile->putBoolean(L"CharcoalUsed",false);
   if(slot==2){
    const auto after=containerItems(*tile,3)[2];
    const int removed=before.count-after.count;
    const float experience=float(removed)*furnaceExperienceValue(before.id);
    int reward=int(std::floor(experience));
    if(experience>reward && state->entityRandom.nextDouble()<experience-reward)
     ++reward;
    const Vec3 at=state->hasPlayerPosition?
        Vec3{state->playerPosition.x,state->playerPosition.y+.5,
             state->playerPosition.z+.5}:Vec3{x+.5,double(y)+.5,z+.5};
    spawnExperienceOrbs(at,reward);
   }
  }
 }else{
  if(slot<0 || slot>=36 || targetSlot<0 || targetSlot>1)return false;
  auto source=carriedItems();if(!source[slot].id)return false;
  if(targetSlot==0 && !furnaceRecipe(source[slot].id).id)return false;
  if(targetSlot==1 && !furnaceFuelDuration(source[slot].id))return false;
  moved=moveContainerStackToSlot(*state->inventory,36,slot,*tile,3,targetSlot,amount);
 }
 if(moved){state->chunk(x,z).unsaved=true;++revision;}
 return moved;
}
bool World::useCauldron(int x,int y,int z,int heldItemId){
 if(get(x,y,z)!=static_cast<Block>(118))return false;
 const int level=std::clamp(getData(x,y,z),0,3);
 if(heldItemId==326 && level<3)return setData(x,y,z,3);
 if(heldItemId==374 && level>0){
  // CauldronTile::use keeps the bottle in creative mode, gives one water
  // potion, and consumes one water level even with instabuild enabled.
  if(!giveCreativeItem(373))return false;
  return setData(x,y,z,level-1);
 }
 return false;
}
void World::tickFurnaces(){
 for(auto& [key,record]:state->records){
  if(!record || !record->extra || !state->region.chunks.contains(key))continue;
  auto* list=dynamic_cast<TagList*>(record->extra->get(L"TileEntities"));if(!list)continue;
  for(int i=0;i<list->size();++i){auto* tile=dynamic_cast<CompoundTag*>(list->get(i));
   if(!tile || tile->getString(L"id")!=L"Furnace")continue;
   const int x=tile->getInt(L"x")+width/2,y=tile->getInt(L"y"),z=tile->getInt(L"z")+depth/2;
   if(!inside(x,y,z))continue;
   int id=get(x,y,z);if(id!=61 && id!=62)continue;
   const bool wasLit=id==62;
   if(!tickFurnace(*tile))continue;
   state->chunk(x,z).unsaved=true;
   const bool lit=tile->getShort(L"BurnTime")>0;
   if(lit!=wasLit)set(x,y,z,static_cast<Block>(lit?62:61));
  }
 }
}
void World::tickBrewingStands(){
 for(auto& [key,record]:state->records){
  if(!record || !record->extra || !state->region.chunks.contains(key))continue;
  auto* list=dynamic_cast<TagList*>(record->extra->get(L"TileEntities"));if(!list)continue;
  for(int i=0;i<list->size();++i){auto* tile=dynamic_cast<CompoundTag*>(list->get(i));
   if(!tile || tile->getString(L"id")!=L"Cauldron")continue;
   const int x=tile->getInt(L"x")+width/2,y=tile->getInt(L"y"),z=tile->getInt(L"z")+depth/2;
   if(!inside(x,y,z) || get(x,y,z)!=static_cast<Block>(117))continue;
   if(tickBrewingStand(*tile))state->chunk(x,z).unsaved=true;
   int bits=brewingBottleBits(*tile);if(getData(x,y,z)!=bits)setData(x,y,z,bits);
  }
 }
}
std::vector<ContainerItem> World::chestItems(int x,int y,int z)const{
 std::vector<ContainerItem> result;
 for(const auto& p:chestParts(x,y,z)){
  auto* chest=state->chest(p[0],p[1],p[2]);auto items=chest?containerItems(*chest,27):std::vector<ContainerItem>(27);
  result.insert(result.end(),items.begin(),items.end());
 }
 return result;
}
std::vector<ContainerItem> World::carriedItems()const{return containerItems(*state->inventory,36);}
bool World::setCreativeHotbarItem(int slot,int id,int damage,int count){
 if(slot<0 || slot>=9 || id<1 || id>32767 || damage<0 || damage>32767 ||
    count<1 || count>std::min(64,consoleItemStackLimit(id)))return false;
 containerItems(*state->inventory,36);
 auto replacement=std::make_unique<TagList>();
 if(auto* previous=dynamic_cast<TagList*>(state->inventory->get(L"Items")))
  for(int i=0;i<previous->size();++i){
   auto* item=dynamic_cast<CompoundTag*>(previous->get(i));
   if(static_cast<unsigned char>(item->getByte(L"Slot"))==slot)continue;
   std::unique_ptr<Tag> copy(item->copy());replacement->add(copy.get());copy.release();
  }
 auto item=std::make_unique<CompoundTag>();
 item->putByte(L"Slot",slot);item->putShort(L"id",id);
 item->putByte(L"Count",count);
 item->putShort(L"Damage",damage);replacement->add(item.get());item.release();
 state->inventory->put(L"Items",replacement.get());replacement.release();
 ++revision;return true;
}
bool World::swapCarriedSlots(int first,int second){
 if(first<0 || first>=36 || second<0 || second>=36 || first==second)return false;
 containerItems(*state->inventory,36);
 auto replacement=std::make_unique<TagList>();
 if(auto* previous=dynamic_cast<TagList*>(state->inventory->get(L"Items")))
  for(int i=0;i<previous->size();++i){
   auto* old=dynamic_cast<CompoundTag*>(previous->get(i));
   auto copy=std::unique_ptr<CompoundTag>(static_cast<CompoundTag*>(old->copy()));
   const int index=static_cast<unsigned char>(copy->getByte(L"Slot"));
   if(index==first)copy->putByte(L"Slot",second);
   else if(index==second)copy->putByte(L"Slot",first);
   replacement->add(copy.get());copy.release();
  }
 state->inventory->put(L"Items",replacement.get());replacement.release();
 ++revision;return true;
}
bool World::giveCreativeItem(int id,int damage){
 if(id<1 || id>32767 || damage<0 || damage>32767)return false;
 CompoundTag source;auto list=std::make_unique<TagList>();auto stack=std::make_unique<CompoundTag>();
 stack->putByte(L"Slot",0);stack->putShort(L"id",id);
 stack->putByte(L"Count",std::min(64,consoleItemStackLimit(id)));
 stack->putShort(L"Damage",damage);list->add(stack.get());stack.release();
 source.put(L"Items",list.get());list.release();
 if(!moveContainerStack(source,1,0,*state->inventory,36))return false;
 ++revision;return true;
}
std::vector<ContainerItem> World::enderChestItems()const{return containerItems(*state->enderInventory,27);}
bool World::transferEnderChestItem(int x,int y,int z,int slot,bool take,int amount){
 if(!canOpenEnderChest(x,y,z))return false;
 ensureEnderChestData(x,y,z);
 bool moved=take?moveContainerStack(*state->enderInventory,27,slot,*state->inventory,36,amount):
     moveContainerStack(*state->inventory,36,slot,*state->enderInventory,27,amount);
 if(moved)++revision;
 return moved;
}
std::optional<SkullInfo> World::skullInfo(int x,int y,int z)const{
 if(!inside(x,y,z) || get(x,y,z)!=static_cast<Block>(144))return std::nullopt;
 const int nativeX=x-width/2,nativeZ=z-depth/2;
 auto it=state->records.find({Mth::intFloorDiv(nativeX,16),Mth::intFloorDiv(nativeZ,16)});
 if(it==state->records.end() || !it->second->extra)return SkullInfo{};
 auto* list=dynamic_cast<TagList*>(it->second->extra->get(L"TileEntities"));
 if(!list)return SkullInfo{};
 for(int i=0;i<list->size();++i)if(auto* tag=dynamic_cast<CompoundTag*>(list->get(i)))
  if(tag->getString(L"id")==L"Skull" && tag->getInt(L"x")==nativeX &&
     tag->getInt(L"y")==y && tag->getInt(L"z")==nativeZ)
   return SkullInfo{std::clamp(int(static_cast<unsigned char>(tag->getByte(L"SkullType"))),0,4),
                    int(static_cast<unsigned char>(tag->getByte(L"Rot")))&15};
 return SkullInfo{};
}
bool World::transferChestItem(int x,int y,int z,int slot,bool take,int amount){
 if(!canOpenChest(x,y,z))return false;
 auto parts=chestParts(x,y,z);
 if(take){
  if(slot<0 || slot>=int(parts.size())*27)return false;
  const auto& p=parts[slot/27];auto* chest=state->chest(p[0],p[1],p[2]);if(!chest)return false;
  bool moved=moveContainerStack(*chest,27,slot%27,*state->inventory,36,amount);
  if(moved){state->chunk(p[0],p[2]).unsaved=true;++revision;}return moved;
 }
 if(slot<0 || slot>=36 || !carriedItems()[slot].id)return false;
 std::vector<CompoundTag*> targets;
 for(const auto& p:parts){auto* chest=state->chest(p[0],p[1],p[2]);
 if(!chest){
  // Raw/new chest blocks get an empty source-format tile entity on first deposit.
  auto key=std::make_pair(Mth::intFloorDiv(p[0]-width/2,16),Mth::intFloorDiv(p[2]-depth/2,16));
  auto& record=state->records[key];if(!record){record=std::make_unique<ChunkRecord>();record->x=key.first;record->z=key.second;}
  if(!record->extra)record->extra=std::make_unique<CompoundTag>();
  auto* list=dynamic_cast<TagList*>(record->extra->get(L"TileEntities"));
  if(!list){record->extra->put(L"TileEntities",new TagList());list=record->extra->getList(L"TileEntities");}
  auto tag=std::make_unique<CompoundTag>();tag->putString(L"id",L"Chest");tag->putInt(L"x",p[0]-width/2);tag->putInt(L"y",p[1]);tag->putInt(L"z",p[2]-depth/2);
  chest=tag.get();list->add(tag.get());tag.release();
 }
 targets.push_back(chest);
 }
 bool moved=moveContainerStacks(*state->inventory,36,slot,targets,std::vector<int>(targets.size(),27),amount);
 if(moved){for(const auto& p:parts)state->chunk(p[0],p[2]).unsaved=true;++revision;}return moved;
}
}
