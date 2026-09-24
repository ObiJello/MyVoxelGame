#include "BrewingSimulation.h"
#include "World.h"
#include "TutorialSchematics.h"
#include "PS3WorldStorage.h"
#include "ItemIcons.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <unistd.h>
using namespace console;
static void require(bool okay,const char* why){if(!okay)throw std::runtime_error(why);}
static void travel(World& world,double x,double z){
 for(int i=0;i<600;++i)if(world.streamAround({x,100,z},2)||!world.streaming())return;
 throw std::runtime_error("Brewing chunk stream did not finish");
}
static int carriedSlot(World& world,int id,int damage=0){
 auto items=world.carriedItems();for(int i=0;i<int(items.size());++i)
  if(items[i].id==id && items[i].damage==damage)return i;
 return -1;
}
static void item(CompoundTag& root,int slot,int id,int count,int damage=0){
 auto* list=dynamic_cast<TagList*>(root.get(L"Items"));
 if(!list){root.put(L"Items",new TagList());list=root.getList(L"Items");}
 auto entry=std::make_unique<CompoundTag>();entry->putByte(L"Slot",slot);entry->putShort(L"id",id);
 entry->putByte(L"Count",count);entry->putShort(L"Damage",damage);
 list->add(entry.get());entry.release();
}
int main(int argc,char** argv){try{
 require(argc==3,"Expected tutorial assets and scratch directory");
 require(isBrewingIngredient(372) && isBrewingIngredient(353) && isBrewingIngredient(289) &&
         !isBrewingIngredient(374),"Source brewing ingredient list");
 require(applyBrewingIngredient(0,372)==16 && applyBrewingIngredient(16,353)==8194 &&
         applyBrewingIngredient(8194,331)==8258 && applyBrewingIngredient(8194,348)==8226 &&
         applyBrewingIngredient(8194,289)==24578,"Source potion bit formulas and normalization");
 require(applyBrewingIngredient(8192,372)==0,"Nether wart rejects an already functional potion");
 for(const auto [ingredient,expected]:std::array<std::pair<int,int>,9>{{
     {370,8193},{353,8194},{378,8195},{375,8196},{382,8197},
     {396,8198},{376,8200},{377,8201},{289,16400}}})
  require(applyBrewingIngredient(16,ingredient)==expected,
          "Each source base reagent produces its documented potion damage bits");
 require(potionDisplayName(0)=="Water Bottle" && potionDisplayName(16)=="Awkward Potion" &&
         potionDisplayName(8194)=="Potion of Swiftness" &&
         potionDisplayName(24578)=="Splash Potion of Swiftness",
         "Potion names reflect the source effect and splash bits");
 require(potionColor(0)==0x385dc6 && potionColor(16)==0x385dc6 &&
         potionColor(8194)==0x7cafc6 && potionColor(24578)==0x7cafc6 &&
         potionColor(8196)==0x4e9331 && potionColor(8206)==0x7f8392,
         "Potion contents use the source effect and base colours");
 CompoundTag cancelled;item(cancelled,0,373,1);item(cancelled,3,372,1);
 require(brewingCanStart(cancelled) && tickBrewingStand(cancelled) && brewingTime(cancelled)==400,
         "Water potion and nether wart start a brew");
 static_cast<CompoundTag*>(cancelled.getList(L"Items")->get(1))->putShort(L"id",353);
 require(tickBrewingStand(cancelled) && brewingTime(cancelled)==0 &&
         containerItems(cancelled,4)[0].damage==0,"Changing ingredient cancels an active brew");
 CompoundTag noReset;item(noReset,0,373,1,8194);item(noReset,3,372,1);
 require(!brewingCanStart(noReset),"Nether wart cannot remove an existing potion effect");
 for(int id:{289,331,348,353,370,372,373,374,375,376,377,378,382,396})
  require(consoleItemIcon(id,0).tile>=0,"Brewing items have imported icons");

 TutorialSchematics tutorial(argv[1]);std::vector<std::tuple<int,int,int>> sites;
 for(const auto& placement:tutorial.placements()){
  const auto& schematic=tutorial.schematic(placement.filename);
  auto* tiles=dynamic_cast<TagList*>(schematic.tags->get(L"TileEntities"));if(!tiles)continue;
  for(int i=0;i<tiles->size();++i)if(auto* tag=dynamic_cast<CompoundTag*>(tiles->get(i)))
   if(tag->getString(L"id")==L"Cauldron"){
    int lx=tag->getInt(L"x"),ly=tag->getInt(L"y"),lz=tag->getInt(L"z");
    require(schematic.block(lx,ly,lz)==117,"Source Cauldron tile is a brewing stand block");
    sites.emplace_back(placement.x+lx,placement.y+ly,placement.z+lz);
   }
 }
 require(sites.size()==3,"Original tutorial has three brewing tile records");
 World tutorialWorld;tutorialWorld.generateTutorial(argv[1]);
 for(const auto& [nativeX,y,nativeZ]:sites){int x=nativeX+World::width/2,z=nativeZ+World::depth/2;
  travel(tutorialWorld,x,z);
  require(tutorialWorld.canOpenBrewingStand(x,y,z) && tutorialWorld.brewingItems(x,y,z).size()==4,
          "Imported tutorial brewing stand opens after streaming");
 }

 std::filesystem::create_directories(argv[2]);auto tempName=(std::filesystem::path(argv[2])/"brewing-XXXXXX").string();
 std::vector<char> temp(tempName.begin(),tempName.end());temp.push_back(0);
 require(mkdtemp(temp.data())!=nullptr,"Create brewing scratch directory");
 struct Cleanup{std::filesystem::path p;~Cleanup(){std::error_code e;std::filesystem::remove_all(p,e);}}cleanup{temp.data()};
 auto path=cleanup.p/"world.inner";
 World world;world.generate(123,true);
 require(world.placeBlock(70,200,0,static_cast<Block>(117),0,{75,200,5},0),"Place brewing stand");
 require(world.giveCreativeItem(373) && world.giveCreativeItem(372),"Supply water potion and nether wart");
 require(world.transferBrewingItem(70,200,0,carriedSlot(world,373),false,0) &&
         world.transferBrewingItem(70,200,0,carriedSlot(world,372),false,3,1),
         "Put a potion and nether wart into source slots");
 require(world.getData(70,200,0)==1 && world.brewingItems(70,200,0)[0].count==1,
         "Bottle slot caps at one and sets model bit");
 require(!world.transferBrewingItem(70,200,0,carriedSlot(world,373),false,0),
         "Occupied potion slot refuses a second bottle");
 world.tickTime();require(world.brewingProgress(70,200,0)==400,"Brewing begins at 400 ticks");
 for(int i=0;i<100;++i)world.tickTime();
 require(world.brewingProgress(70,200,0)==300,"Brewing countdown advances once per world tick");
 world.save(path);World restored;require(restored.load(path) && restored.brewingProgress(70,200,0)==300,
         "Native save keeps mid-brew progress");
 travel(restored,400,400);travel(restored,70,0);
 require(restored.brewingProgress(70,200,0)==300,"Chunk eviction retains mid-brew state");
 for(int i=0;i<300;++i)restored.tickTime();
 require(restored.brewingProgress(70,200,0)==0 && restored.brewingItems(70,200,0)[0].damage==16 &&
         !restored.brewingItems(70,200,0)[3].id,
         "Nether wart yields awkward potion and consumes one ingredient");
 require(restored.giveCreativeItem(353),"Creative menu supplies sugar");
 require(restored.transferBrewingItem(70,200,0,carriedSlot(restored,353),false,3,1),
         "Brewing ingredient slot accepts sugar");
 for(int i=0;i<401;++i)restored.tickTime();
 require(restored.brewingItems(70,200,0)[0].damage==8194,
         "Sugar brews the source speed-potion damage value");
 require(restored.transferBrewingItem(70,200,0,0,true) && restored.getData(70,200,0)==0 &&
         carriedSlot(restored,373,8194)>=0,"Taking potion clears the visible bottle bit");
 restored.save(path);
 auto archive=PS3WorldStorage::readFile(path);auto record=archive->chunk(0,0,-4);
 bool found=false;if(record && record->extra)if(auto* tiles=dynamic_cast<TagList*>(record->extra->get(L"TileEntities")))
  for(int i=0;i<tiles->size();++i)if(auto* tile=dynamic_cast<CompoundTag*>(tiles->get(i)))
   found|=tile->getString(L"id")==L"Cauldron" && tile->getInt(L"x")==6 &&
          tile->getInt(L"y")==200 && tile->getInt(L"z")==-64;
 require(found,"Placed stand saves source Cauldron tile ID");
 require(restored.breakBlock(70,200,0),"Mine brewing stand");
 require(!restored.canOpenBrewingStand(70,200,0),"Mined stand no longer opens");
 restored.save(path);archive=PS3WorldStorage::readFile(path);record=archive->chunk(0,0,-4);
 if(record && record->extra)if(auto* tiles=dynamic_cast<TagList*>(record->extra->get(L"TileEntities")))
  for(int i=0;i<tiles->size();++i)if(auto* tile=dynamic_cast<CompoundTag*>(tiles->get(i)))
   require(!(tile->getString(L"id")==L"Cauldron" && tile->getInt(L"x")==6 &&
             tile->getInt(L"y")==200 && tile->getInt(L"z")==-64),
           "Mining removes only the placed brewing tile");
 std::cout<<"brewing tests passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
