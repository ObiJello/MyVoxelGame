#include "FurnaceSimulation.h"
#include "CompoundTag.h"
#include <algorithm>
#include <memory>
namespace console {
FurnaceRecipe furnaceRecipe(int id){
 switch(id){
 case 15:return {265,0};case 14:return {266,0};case 56:return {264,0};
 case 12:return {20,0};case 319:return {320,0};case 363:return {364,0};
 case 365:return {366,0};case 349:return {350,0};case 4:return {1,0};
 case 337:return {336,0};case 81:return {351,2};case 17:return {263,1};
 case 129:return {388,0};case 392:return {393,0};case 87:return {405,0};
 case 16:return {263,0};case 73:return {331,0};case 21:return {351,4};
 case 153:return {406,0};default:return {};
 }
}
int furnaceFuelDuration(int id){
 // FurnaceTileEntity::getBurnDuration: 200 ticks per base interval.
 if(id==126)return 150;
 switch(id){
 case 5:case 17:case 25:case 47:case 53:case 54:case 58:case 63:case 64:
 case 68:case 72:case 84:case 85:case 96:case 99:case 100:case 107:case 125:
 case 134:case 135:case 136:return 300;
 case 268:case 269:case 270:case 271:case 290:return 200;
 case 280:case 6:return 100;
 case 263:return 1600;case 327:return 20000;case 369:return 2400;
 default:return 0;
 }
}
float furnaceExperienceValue(int id){
    // FurnaceRecipes stores reward by output ID; later coal/dye recipes
    // replace the earlier value for the same item in the source registry.
    switch(id){
    case 265:case 331:return .7f;case 266:case 264:case 388:return 1.f;
    case 20:case 1:case 405:case 263:return .1f;
    case 320:case 364:case 366:case 350:case 393:return .35f;
    case 336:return .3f;case 351:case 406:return .2f;
    default:return 0.f;
    }
}
namespace {
CompoundTag* slotTag(CompoundTag& tile,int slot){
 auto* list=dynamic_cast<TagList*>(tile.get(L"Items"));if(!list)return nullptr;
 for(int i=0;i<list->size();++i)if(auto* item=dynamic_cast<CompoundTag*>(list->get(i)))
  if(static_cast<unsigned char>(item->getByte(L"Slot"))==slot)return item;
 return nullptr;
}
void replaceSlot(CompoundTag& tile,int slot,std::unique_ptr<CompoundTag> item){
 auto list=std::make_unique<TagList>();
 if(auto* old=dynamic_cast<TagList*>(tile.get(L"Items")))for(int i=0;i<old->size();++i){
  auto* entry=dynamic_cast<CompoundTag*>(old->get(i));
  if(entry && static_cast<unsigned char>(entry->getByte(L"Slot"))==slot)continue;
  std::unique_ptr<Tag> copy(old->get(i)->copy());list->add(copy.get());copy.release();
 }
 if(item){item->putByte(L"Slot",slot);list->add(item.get());item.release();}
 tile.put(L"Items",list.get());list.release();
}
bool canCook(CompoundTag& tile,const std::vector<ContainerItem>& items){
 if(!items[0].id)return false;auto result=furnaceRecipe(items[0].id);if(!result.id)return false;
 if(!items[2].id)return true;
 auto* output=slotTag(tile,2);
 return items[2].id==result.id && items[2].damage==result.damage && output &&
        !output->contains(L"tag") && items[2].count<consoleItemStackLimit(result.id);
}
}
FurnaceProgress furnaceProgress(CompoundTag& tile){
 auto items=containerItems(tile,3);
 int duration=tile.getShort(L"console_port.LitDuration");
 if(!duration)duration=furnaceFuelDuration(items[1].id);
 if(!duration)duration=200;
 return {std::max(0,int(tile.getShort(L"BurnTime"))),std::clamp(int(tile.getShort(L"CookTime")),0,200),duration};
}
bool tickFurnace(CompoundTag& tile){
 auto items=containerItems(tile,3);
 int burn=std::max(0,int(tile.getShort(L"BurnTime")));
 int cook=std::clamp(int(tile.getShort(L"CookTime")),0,200);
 const bool wasLit=burn>0;bool changed=false;
 if(burn>0){--burn;changed=true;}
 if(burn==0 && canCook(tile,items)){
  burn=furnaceFuelDuration(items[1].id);
  if(burn>0){
   changed=true;
   // Native source saves omit litDuration, which would be lost when the last
   // fuel item is consumed. Keep it as an ignored extension for the live UI.
   tile.putShort(L"console_port.LitDuration",burn);
   if(items[1].id==263 && items[1].damage==1)tile.putBoolean(L"CharcoalUsed",true);
   auto* fuel=slotTag(tile,1);
   if(--items[1].count==0){
    if(items[1].id==327){auto bucket=std::make_unique<CompoundTag>();bucket->putShort(L"id",325);
     bucket->putByte(L"Count",1);bucket->putShort(L"Damage",0);replaceSlot(tile,1,std::move(bucket));}
    else replaceSlot(tile,1,nullptr);
   }else fuel->putByte(L"Count",items[1].count);
  }
 }
 if(burn>0 && canCook(tile,items)){
  if(++cook==200){
   cook=0;auto result=furnaceRecipe(items[0].id);
   if(auto* output=slotTag(tile,2))output->putByte(L"Count",items[2].count+1);
   else {auto product=std::make_unique<CompoundTag>();product->putShort(L"id",result.id);
    product->putByte(L"Count",1);product->putShort(L"Damage",result.damage);replaceSlot(tile,2,std::move(product));}
   if(auto* input=slotTag(tile,0)){
    if(items[0].count==1)replaceSlot(tile,0,nullptr);
    else input->putByte(L"Count",items[0].count-1);
   }
   changed=true;
  }else changed=true;
 }else if(cook){cook=0;changed=true;}
 if(changed || wasLit!=(burn>0)){
  tile.putShort(L"BurnTime",burn);tile.putShort(L"CookTime",cook);
  return true;
 }
 return false;
}
}
