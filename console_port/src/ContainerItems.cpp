#include "ContainerItems.h"
#include "CompoundTag.h"
#include <memory>
#include <algorithm>
namespace console {namespace {
TagList* items(CompoundTag& root){auto* t=root.get(L"Items");if(!t)return nullptr;auto* list=dynamic_cast<TagList*>(t);if(!list)throw IoError("Invalid container item list");return list;}
int slotOf(CompoundTag& item){return static_cast<unsigned char>(item.getByte(L"Slot"));}
std::unique_ptr<TagList> copied(CompoundTag& root,int omit=-1){
 auto result=std::make_unique<TagList>();if(auto* list=items(root))for(int i=0;i<list->size();++i){auto* item=dynamic_cast<CompoundTag*>(list->get(i));if(!item)throw IoError("Invalid item record");if(slotOf(*item)==omit)continue;
  std::unique_ptr<Tag> copy(item->copy());result->add(copy.get());copy.release();}return result;
}
}
std::vector<ContainerItem> containerItems(CompoundTag& root,int capacity){
 if(capacity<1 || capacity>256)throw IoError("Invalid container size");std::vector<ContainerItem> result(capacity);
 if(auto* list=items(root))for(int i=0;i<list->size();++i){auto* item=dynamic_cast<CompoundTag*>(list->get(i));if(!item)throw IoError("Invalid item record");
  int slot=slotOf(*item);if(slot>=capacity)throw IoError("Invalid item slot");
  int id=item->getShort(L"id"),count=static_cast<unsigned char>(item->getByte(L"Count"));
  if(id<=0 || count<1 || count>64 || result[slot].id)throw IoError("Invalid or duplicate item stack");
  auto* tag=dynamic_cast<CompoundTag*>(item->get(L"tag"));
  result[slot]={id,count,item->getShort(L"Damage"),tag?tag->getInt(L"4jdata"):0,tag && (tag->contains(L"ench")||tag->contains(L"StoredEnchantments"))};
 }return result;
}
bool moveContainerStacks(CompoundTag& source,int sourceCapacity,int slot,const std::vector<CompoundTag*>& destinations,const std::vector<int>& capacities,int amount){
 auto from=containerItems(source,sourceCapacity);
 if(slot<0 || slot>=sourceCapacity || !from[slot].id || destinations.empty() || destinations.size()!=capacities.size() || amount < -1)return false;
 std::vector<std::unique_ptr<CompoundTag>> copies;
 for(std::size_t i=0;i<destinations.size();++i){
  if(!destinations[i] || destinations[i]==&source)return false;
  for(std::size_t j=0;j<i;++j)if(destinations[i]==destinations[j])return false;
  containerItems(*destinations[i],capacities[i]);
  copies.emplace_back(static_cast<CompoundTag*>(destinations[i]->copy()));
  if(!copies.back()->contains(L"Items"))copies.back()->put(L"Items",new TagList());
 }
 auto original=std::unique_ptr<CompoundTag>(static_cast<CompoundTag*>(source.copy()));
 auto find=[](CompoundTag& root,int slot)->CompoundTag*{auto* list=items(root);if(!list)return nullptr;
  for(int i=0;i<list->size();++i){auto* item=static_cast<CompoundTag*>(list->get(i));if(slotOf(*item)==slot)return item;}return nullptr;};
 auto* stack=find(*original,slot);const int count=from[slot].count;
 int remaining=amount==-1?count:amount==0?(count+1)/2:std::min(amount,count),requested=remaining;
 const int limit=consoleItemStackLimit(from[slot].id);
 auto comparable=[](CompoundTag& item){auto result=std::unique_ptr<CompoundTag>(static_cast<CompoundTag*>(item.copy()));result->remove(L"Slot");result->remove(L"Count");return result;};
 auto key=comparable(*stack);
 // AbstractContainerMenu::moveItemStackTo's merge-before-empty traversal.
 // Compare all preserved fields, including Damage and unknown tags, so an
 // unported item's data can never be discarded by combining it with another.
 for(int pass=0;pass<2 && remaining;++pass)for(std::size_t d=0;d<copies.size() && remaining;++d){
  auto& root=*copies[d];if(!root.contains(L"Items"))root.put(L"Items",new TagList());
  for(int target=0;target<capacities[d] && remaining;++target){auto* item=find(root,target);
   if(pass==0){
    if(!item || limit<=1)continue;
    auto targetKey=comparable(*item);if(!key->equals(targetKey.get()))continue;
    int held=static_cast<unsigned char>(item->getByte(L"Count")),moved=std::min(remaining,std::max(0,limit-held));
    if(moved){item->putByte(L"Count",held+moved);remaining-=moved;}
   }else if(!item){
    auto added=std::unique_ptr<CompoundTag>(static_cast<CompoundTag*>(stack->copy()));int moved=std::min(remaining,limit);
    added->putByte(L"Slot",target);added->putByte(L"Count",moved);items(root)->add(added.get());added.release();remaining-=moved;
   }
  }
 }
 const int moved=requested-remaining;if(!moved)return false;
 if(moved==count){auto kept=copied(*original,slot);original->put(L"Items",kept.get());kept.release();}
 else stack->putByte(L"Count",count-moved);
 // Prepare missing map entries before the no-allocation commits.
 for(auto* destination:destinations)if(!destination->contains(L"Items"))destination->put(L"Items",new TagList());
 std::unique_ptr<Tag> sourceItems(original->take(L"Items"));source.put(L"Items",sourceItems.get());sourceItems.release();
 for(std::size_t i=0;i<copies.size();++i){std::unique_ptr<Tag> list(copies[i]->take(L"Items"));destinations[i]->put(L"Items",list.get());list.release();}
 return true;
}
bool moveContainerStack(CompoundTag& source,int sourceCapacity,int slot,CompoundTag& destination,int destinationCapacity,int amount){
 return moveContainerStacks(source,sourceCapacity,slot,{&destination},{destinationCapacity},amount);
}
bool moveContainerStackToSlot(CompoundTag& source,int sourceCapacity,int slot,
    CompoundTag& destination,int destinationCapacity,int targetSlot,int amount){
 if(&source==&destination || targetSlot<0 || targetSlot>=destinationCapacity)return false;
 containerItems(destination,destinationCapacity);
 auto sourceCopy=std::unique_ptr<CompoundTag>(static_cast<CompoundTag*>(source.copy()));
 CompoundTag one;auto single=std::make_unique<TagList>();
 if(auto* list=items(destination))for(int i=0;i<list->size();++i){
  auto* entry=static_cast<CompoundTag*>(list->get(i));if(slotOf(*entry)!=targetSlot)continue;
  auto copy=std::unique_ptr<CompoundTag>(static_cast<CompoundTag*>(entry->copy()));
  copy->putByte(L"Slot",0);single->add(copy.get());copy.release();
 }
 one.put(L"Items",single.get());single.release();
 if(!moveContainerStack(*sourceCopy,sourceCapacity,slot,one,1,amount))return false;
 auto destinationItems=copied(destination,targetSlot);
 if(auto* list=items(one))for(int i=0;i<list->size();++i){
  auto copy=std::unique_ptr<CompoundTag>(static_cast<CompoundTag*>(list->get(i)->copy()));
  copy->putByte(L"Slot",targetSlot);destinationItems->add(copy.get());copy.release();
 }
 std::unique_ptr<Tag> movedSource(sourceCopy->take(L"Items"));
 source.put(L"Items",movedSource.get());movedSource.release();
 destination.put(L"Items",destinationItems.get());destinationItems.release();
 return true;
}
}
