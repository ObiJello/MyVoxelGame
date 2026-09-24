#include "TutorialContainers.h"
#include <algorithm>
#include <map>
namespace console {namespace {
int number(const ConsoleGameRuleNode& n,const wchar_t* key,int fallback=0){
 auto it=n.attributes.find(key);if(it==n.attributes.end())return fallback;
 std::size_t used=0;long long value=std::stoll(it->second,&used);
 if(used!=it->second.size() || value < -30000000 || value >30000000)throw IoError("Invalid tutorial container attribute");
 return int(value);
}
// Item registrations used by this tutorial. Original tools, armor, boats,
// fishing rods, buckets, records and enchanted books have maximum stack one.
int maximum(int id){
 if((id>=256 && id<=259)||id==261||(id>=267 && id<=279)||(id>=283 && id<=286)||
    (id>=290 && id<=294)||(id>=298 && id<=317)||id==326||id==333||id==346||id==403||
    (id>=2256 && id<=2267))return 1;
 return 64;
}
std::unique_ptr<CompoundTag> itemTag(const ConsoleGameRuleNode& n,int slot){
 int id=number(n,L"itemId"),count=number(n,L"quantity",1),damage=number(n,L"auxValue");
 if(id<=0 || id>32767 || count<1 || damage<0 || damage>32767)throw IoError("Invalid tutorial item");
 auto item=std::make_unique<CompoundTag>();item->putByte(L"Slot",slot);item->putShort(L"id",id);
 item->putByte(L"Count",std::min(count,maximum(id)));item->putShort(L"Damage",damage);
 auto tag=std::make_unique<CompoundTag>();bool tagged=false;
 int marker=number(n,L"dataTag");if(marker){tag->putInt(L"4jdata",marker);tagged=true;}
 auto enchants=std::make_unique<TagList>();
 for(const auto& enchant:n.children)if(enchant.name==L"AddEnchantment"){
  int eid=number(enchant,L"enchantmentId"),level=number(enchant,L"enchantmentLevel");
  if(eid<0 || eid>255 || level<1 || level>32767)throw IoError("Invalid tutorial enchantment");
  auto entry=std::make_unique<CompoundTag>();entry->putShort(L"id",eid);entry->putShort(L"lvl",level);
  enchants->add(entry.get());entry.release();
 }
 if(enchants->size()){tag->put(id==403?L"StoredEnchantments":L"ench",enchants.get());enchants.release();tagged=true;}
 if(tagged){item->put(L"tag",tag.get());tag.release();}return item;
}
}
std::vector<TutorialContainer> readTutorialContainers(const std::vector<ConsoleGameRuleNode>& rules){
 std::vector<TutorialContainer> result;
 for(const auto& root:rules)if(root.name==L"MapOptions")for(const auto& structure:root.children)if(structure.name==L"GenerateStructure"){
  for(const auto& action:structure.children)if(action.name==L"PlaceContainer"){
   // All supplied actions have zero local offsets, so both north/south world
   // coordinate conventions resolve to the exact structure origin.
   if(number(structure,L"orientation")!=0 || number(action,L"x") || number(action,L"y") || number(action,L"z"))throw IoError("Unsupported tutorial container transform");
   if(number(action,L"block",54)!=54)throw IoError("Unsupported tutorial container type");
   TutorialContainer c{number(structure,L"x"),number(structure,L"y"),number(structure,L"z"),number(structure,L"dim"),number(action,L"facing"),std::make_unique<CompoundTag>()};
   if(c.y<0 || c.y>255 || c.dimension < -1 || c.dimension>1 || c.facing<0 || c.facing>5)throw IoError("Invalid tutorial chest location");
   c.tag->putString(L"id",L"Chest");c.tag->putInt(L"x",c.x);c.tag->putInt(L"y",c.y);c.tag->putInt(L"z",c.z);
   std::map<int,std::unique_ptr<CompoundTag>> slots;int next=0;
   for(const auto& item:action.children)if(item.name==L"AddItem" && next<27){
    int slot=number(item,L"slot",-1);if(slot<0 || slot>=27)slot=next;
    slots[slot]=itemTag(item,slot);++next;
   }
   auto items=std::make_unique<TagList>();for(auto& [slot,item]:slots){items->add(item.get());item.release();}
   c.tag->put(L"Items",items.get());items.release();result.push_back(std::move(c));
  }
 }
 return result;
}
std::vector<TutorialSpawner> readTutorialSpawners(const std::vector<ConsoleGameRuleNode>& rules){
 std::vector<TutorialSpawner> result;
 for(const auto& root:rules)if(root.name==L"MapOptions")for(const auto& structure:root.children)if(structure.name==L"GenerateStructure"){
  for(const auto& action:structure.children)if(action.name==L"PlaceSpawner"){
   if(number(structure,L"orientation")!=0 || number(action,L"x") || number(action,L"y") || number(action,L"z"))throw IoError("Unsupported tutorial spawner transform");
   TutorialSpawner spawner{number(structure,L"x"),number(structure,L"y"),number(structure,L"z"),number(structure,L"dim"),std::make_unique<CompoundTag>()};
   if(spawner.y<0 || spawner.y>255 || spawner.dimension<-1 || spawner.dimension>1)throw IoError("Invalid tutorial spawner location");
   auto entity=action.attributes.find(L"entity");
   if(entity==action.attributes.end() || entity->second.empty() || entity->second.size()>128)throw IoError("Invalid tutorial spawner entity");
   // TileEntity::save and MobSpawnerTileEntity::save, with the source defaults.
   spawner.tag->putString(L"id",L"MobSpawner");spawner.tag->putInt(L"x",spawner.x);
   spawner.tag->putInt(L"y",spawner.y);spawner.tag->putInt(L"z",spawner.z);
   spawner.tag->putString(L"EntityId",entity->second);spawner.tag->putShort(L"Delay",20);
   spawner.tag->putShort(L"MinSpawnDelay",200);spawner.tag->putShort(L"MaxSpawnDelay",800);
   spawner.tag->putShort(L"SpawnCount",4);
   result.push_back(std::move(spawner));
  }
 }
 return result;
}
}
