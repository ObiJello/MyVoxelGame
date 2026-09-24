#include "BrewingSimulation.h"
#include "ContainerItems.h"
#include "CompoundTag.h"
#include "PotionEffects.h"
#include <algorithm>
#include <memory>
#include <string_view>
namespace console {
namespace {
std::wstring_view formula(int id){
 switch(id){
 case 372:return L"+4&!13";case 353:return L"-0+1-2-3&4-4+13";
 case 370:return L"+0-1-2-3&4-4+13";case 375:return L"-0-1+2-3&4-4+13";
 case 376:return L"-0+3-4+13";case 382:return L"+0-1+2-3&4-4+13";
 case 377:return L"+0-1-2+3&4-4+13";case 396:return L"-0+1+2-3+13&4-4";
 case 378:return L"+0+1-2-3&4-4+13";case 331:return L"-5+6-7";
 case 348:return L"+5-6-7";case 289:return L"+14";
 default:return {};
 }
}
int applyBit(int brew,int bit,bool negative,bool toggle,bool required){
 if(bit<0 || bit>14)return brew;
 const bool lit=(brew&(1<<bit))!=0;
 if(required)return lit==toggle?0:brew;
 if(negative)return brew&~(1<<bit);
 if(toggle)return brew^(1<<bit);
 return brew|(1<<bit);
}
int applyFormula(int brew,std::wstring_view text){
 int value=0;bool has=false,negative=false,toggle=false,required=false;
 auto flush=[&]{if(!has)return;
  brew=applyBit(brew,value,negative,toggle,required);
  value=0;has=negative=toggle=required=false;};
 for(wchar_t c:text){
  if(c>=L'0' && c<=L'9'){value=value*10+c-L'0';has=true;}
  else if(c==L'+' || c==L'-' || c==L'!' || c==L'&'){
   flush();if(c==L'-')negative=true;else if(c==L'!')toggle=true;else if(c==L'&')required=true;
  }
 }
 flush();return brew&0x7fff;
}
bool hasEffect(int damage){
 return !potionEffects(damage).empty();
}
CompoundTag* slot(CompoundTag& tile,int index){
 auto* list=dynamic_cast<TagList*>(tile.get(L"Items"));if(!list)return nullptr;
 for(int i=0;i<list->size();++i)if(auto* item=dynamic_cast<CompoundTag*>(list->get(i)))
  if(static_cast<unsigned char>(item->getByte(L"Slot"))==index)return item;
 return nullptr;
}
void removeSlot(CompoundTag& tile,int index){
 auto list=std::make_unique<TagList>();
 if(auto* old=dynamic_cast<TagList*>(tile.get(L"Items")))for(int i=0;i<old->size();++i){
  auto* item=dynamic_cast<CompoundTag*>(old->get(i));
  if(item && static_cast<unsigned char>(item->getByte(L"Slot"))==index)continue;
  std::unique_ptr<Tag> copy(old->get(i)->copy());list->add(copy.get());copy.release();
 }
 tile.put(L"Items",list.get());list.release();
}
bool changesPotion(int current,int next){
 if(current==next)return false;
 if((current&0x4000)==0 && (next&0x4000)!=0)return true;
 if(hasEffect(current) && !hasEffect(next))return false;
 return true;
}
}
bool isBrewingIngredient(int id){return !formula(id).empty();}
std::uint32_t potionColor(int damage){
 // PotionBrewing::getColorValue uses the effect colour from colours.xml for a
 // single simplified effect, or Potion_BaseColour when there is no effect.
 switch(damage&15){
 case 1:return 0xcd5cab; // regeneration
 case 2:return 0x7cafc6; // movement speed
 case 3:return 0xe49a3a; // fire resistance
 case 4:return 0x4e9331; // poison
 case 5:return 0xf82423; // healing
 case 6:return 0x1f1fa1; // night vision
 case 8:return 0x484d48; // weakness
 case 9:return 0x932423; // strength
 case 10:return 0x5a6c81; // slowness
 case 12:return 0x430a09; // harming
 case 14:return 0x7f8392; // invisibility
 default:return 0x385dc6;
 }
}
std::string potionDisplayName(int damage){
 if(damage==0)return "Water Bottle";
 std::string name;
 if((damage&0x2000)!=0)switch(damage&15){
 case 1:name="Potion of Regeneration";break;case 2:name="Potion of Swiftness";break;
 case 3:name="Potion of Fire Resistance";break;case 4:name="Potion of Poison";break;
 case 5:name="Potion of Healing";break;case 6:name="Potion of Night Vision";break;
 case 8:name="Potion of Weakness";break;case 9:name="Potion of Strength";break;
 case 10:name="Potion of Slowness";break;case 12:name="Potion of Harming";break;
 case 14:name="Potion of Invisibility";break;default:break;
 }
 if(name.empty())name=(damage&0x10)?"Awkward Potion":"Potion";
 if(damage&0x4000)name="Splash "+name;
 return name;
}
int applyBrewingIngredient(int damage,int ingredientId){
 auto f=formula(ingredientId);if(f.empty())return damage;
 // NORMALISE_POTION_AUXVAL retains effect byte, functional and splash bits.
 return applyFormula(damage,f)&(0xff|0x2000|0x4000);
}
bool brewingCanStart(CompoundTag& tile){
 auto items=containerItems(tile,4);if(!items[3].id || !isBrewingIngredient(items[3].id))return false;
 for(int i=0;i<3;++i)if(items[i].id==373){
  int next=applyBrewingIngredient(items[i].damage,items[3].id);
  if(changesPotion(items[i].damage,next))return true;
 }
 return false;
}
int brewingBottleBits(CompoundTag& tile){
 auto items=containerItems(tile,4);int bits=0;
 for(int i=0;i<3;++i)if(items[i].id)bits|=1<<i;
 return bits;
}
int brewingTime(CompoundTag& tile){return std::max(0,int(tile.getShort(L"BrewTime")));}
bool tickBrewingStand(CompoundTag& tile){
 int time=brewingTime(tile);auto items=containerItems(tile,4);bool changed=false;
 if(time>0){
  --time;changed=true;
  if(time==0){
   if(brewingCanStart(tile)){
    for(int i=0;i<3;++i)if(items[i].id==373){
     int next=applyBrewingIngredient(items[i].damage,items[3].id);
     if(changesPotion(items[i].damage,next))slot(tile,i)->putShort(L"Damage",next);
    }
    if(items[3].count==1)removeSlot(tile,3);
    else slot(tile,3)->putByte(L"Count",items[3].count-1);
   }
  }else if(!brewingCanStart(tile) ||
           (tile.contains(L"console_port.IngredientId") && tile.getInt(L"console_port.IngredientId")!=items[3].id))time=0;
 }else if(brewingCanStart(tile)){
  time=400;tile.putInt(L"console_port.IngredientId",items[3].id);changed=true;
 }
 if(changed)tile.putShort(L"BrewTime",time);
 return changed;
}
}
