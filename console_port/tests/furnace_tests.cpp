#include "FurnaceSimulation.h"
#include "ItemIcons.h"
#include "World.h"
#include "TutorialSchematics.h"
#include "PS3WorldStorage.h"
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <unistd.h>
using namespace console;
static void require(bool okay,const char* reason){if(!okay)throw std::runtime_error(reason);}
static void travel(World& world,double x,double z){
 for(int i=0;i<600;++i)if(world.streamAround({x,100,z},2) || !world.streaming())return;
 throw std::runtime_error("Furnace chunk stream did not finish");
}
static void item(CompoundTag& tile,int slot,int id,int count,int damage=0){
 auto* list=dynamic_cast<TagList*>(tile.get(L"Items"));if(!list){tile.put(L"Items",new TagList());list=tile.getList(L"Items");}
 auto stack=std::make_unique<CompoundTag>();stack->putByte(L"Slot",slot);stack->putShort(L"id",id);
 stack->putByte(L"Count",count);stack->putShort(L"Damage",damage);list->add(stack.get());stack.release();
}
int main(int argc,char** argv){try{
 require(argc==3,"Expected tutorial assets and scratch directory");
 require(furnaceRecipe(15).id==265 && furnaceRecipe(4).id==1 && furnaceRecipe(17).damage==1 &&
         furnaceRecipe(81).damage==2 && furnaceRecipe(21).damage==4 && furnaceRecipe(153).id==406,
         "Source FurnaceRecipes mapping");
 require(furnaceFuelDuration(263)==1600 && furnaceFuelDuration(327)==20000 &&
         furnaceFuelDuration(126)==150 && furnaceFuelDuration(280)==100 && furnaceFuelDuration(6)==100,
         "Source furnace fuel intervals");
 for(int id:{263,319,326,327,337,349,363,365,369,373,374})
  require(consoleItemIcon(id,0).tile>=0,"Held smelting and cauldron items have source icons");
 CompoundTag tile;item(tile,0,12,2);item(tile,1,263,1);
 require(tickFurnace(tile),"Coal starts furnace");
 require(furnaceProgress(tile).burn==1600 && furnaceProgress(tile).cook==1 && !containerItems(tile,3)[1].id,
         "First tick consumes one fuel and advances cook progress");
 for(int i=1;i<200;++i)tickFurnace(tile);
 require(containerItems(tile,3)[2].id==20 && containerItems(tile,3)[2].count==1 &&
         containerItems(tile,3)[0].count==1 && furnaceProgress(tile).cook==0,
         "Exactly 200 ticks smelt one input");
 for(int i=0;i<200;++i)tickFurnace(tile);
 require(containerItems(tile,3)[2].count==2 && !containerItems(tile,3)[0].id,
         "Second smelt merges output and consumes final input");
 CompoundTag blocked;item(blocked,0,12,1);item(blocked,1,263,1);item(blocked,2,20,64);
 require(!tickFurnace(blocked) && containerItems(blocked,3)[1].count==1 &&
         furnaceProgress(blocked).burn==0,"Full output does not waste fuel");
 CompoundTag bucket;item(bucket,0,12,1);item(bucket,1,327,1);
 tickFurnace(bucket);
 require(containerItems(bucket,3)[1].id==325 && furnaceProgress(bucket).burn==20000,
         "Lava fuel leaves an empty bucket");
 CompoundTag charcoal;item(charcoal,0,17,1);item(charcoal,1,263,1,1);
 tickFurnace(charcoal);
 require(charcoal.getBoolean(L"CharcoalUsed"),"Charcoal-used source flag persists");
 CompoundTag transferSource,transferTarget;item(transferSource,0,12,3);
 static_cast<CompoundTag*>(transferSource.getList(L"Items")->get(0))->putString(L"Unknown",L"retained");
 require(moveContainerStackToSlot(transferSource,1,0,transferTarget,3,1,2) &&
         containerItems(transferSource,1)[0].count==1 && containerItems(transferTarget,3)[1].count==2 &&
         static_cast<CompoundTag*>(transferTarget.getList(L"Items")->get(0))->getString(L"Unknown")==L"retained",
         "Targeted slot transfers preserve unknown NBT and split counts exactly");

 TutorialSchematics tutorial(argv[1]);std::vector<std::tuple<int,int,int>> sites;
 for(const auto& placement:tutorial.placements()){
  const auto& schematic=tutorial.schematic(placement.filename);
  auto* tiles=dynamic_cast<TagList*>(schematic.tags->get(L"TileEntities"));if(!tiles)continue;
  for(int i=0;i<tiles->size();++i)if(auto* tag=dynamic_cast<CompoundTag*>(tiles->get(i)))
   if(tag->getString(L"id")==L"Furnace"){
    int lx=tag->getInt(L"x"),ly=tag->getInt(L"y"),lz=tag->getInt(L"z");
    require(schematic.block(lx,ly,lz)==61 || schematic.block(lx,ly,lz)==62,
            "Original furnace tile data matches its schematic block");
    sites.emplace_back(placement.x+lx,placement.y+ly,placement.z+lz);
   }
 }
 require(sites.size()==2,"Original tutorial has two placed furnace tile entities");
 World tutorialWorld;tutorialWorld.generateTutorial(argv[1]);
 for(const auto& [nativeX,y,nativeZ]:sites){int x=nativeX+World::width/2,z=nativeZ+World::depth/2;
  travel(tutorialWorld,x,z);
  require(tutorialWorld.canOpenFurnace(x,y,z) && tutorialWorld.furnaceItems(x,y,z).size()==3,
          "Tutorial loader preserves interactive furnace and all three slots");
 }

 std::filesystem::create_directories(argv[2]);auto path=(std::filesystem::path(argv[2])/"furnace-XXXXXX").string();
 std::vector<char> temp(path.begin(),path.end());temp.push_back(0);
 require(mkdtemp(temp.data())!=nullptr,"Create scratch directory");
 struct Cleanup{std::filesystem::path p;~Cleanup(){std::error_code e;std::filesystem::remove_all(p,e);}}cleanup{temp.data()};
 path=(cleanup.p/"world.inner").string();
 World world;world.generate(123,true);
 require(world.placeBlock(70,200,0,static_cast<Block>(61),0,{75,200,5},0),"Place native furnace");
 require(world.canOpenFurnace(70,200,0) && world.furnaceItems(70,200,0).size()==3,
         "Placed furnace has three native slots");
 world.save(path);
 auto archive=PS3WorldStorage::readFile(path);auto record=archive->chunk(0,0,-4);
 require(record && record->extra,"Native save contains furnace chunk record");
 bool seeded=false;
 if(auto* tiles=dynamic_cast<TagList*>(record->extra->get(L"TileEntities")))
  for(int i=0;i<tiles->size();++i)if(auto* furnace=dynamic_cast<CompoundTag*>(tiles->get(i)))
   if(furnace->getString(L"id")==L"Furnace" && furnace->getInt(L"x")==6 && furnace->getInt(L"z")==-64){
    item(*furnace,0,12,2);item(*furnace,1,263,1);seeded=true;
   }
 require(seeded,"Placed block writes source Furnace tile entity");
 archive->putChunk(0,*record);archive->writeFile(path);
 require(world.load(path),"Load native furnace tile");
 for(int i=0;i<100;++i)world.tickTime();
 require(world.get(70,200,0)==static_cast<Block>(62) && world.furnaceState(70,200,0).cook==100,
         "World ticks light block and advance tile progress");
 world.save(path);World restored;require(restored.load(path),"Reload mid-cook world");
 require(restored.furnaceState(70,200,0).cook==100 && restored.furnaceState(70,200,0).burn==1501 &&
         restored.furnaceState(70,200,0).duration==1600,
         "Cook and fuel progress survive disk reload");
 travel(restored,400,400);travel(restored,70,0);
 require(restored.furnaceState(70,200,0).cook==100 && restored.furnaceItems(70,200,0)[0].count==2,
         "Streaming away and back retains a half-cooked furnace");
 for(int i=0;i<100;++i)restored.tickTime();
 require(restored.furnaceItems(70,200,0)[2].id==20 && restored.furnaceItems(70,200,0)[0].count==1,
         "Reloaded furnace completes the correct recipe");
 require(restored.transferFurnaceItem(70,200,0,2,true) && restored.carriedItems()[0].id==20,
         "Player can take cooked output");
 require(restored.transferFurnaceItem(70,200,0,0,true) && restored.carriedItems()[1].id==12,
         "Player can take unburned input");
 require(restored.transferFurnaceItem(70,200,0,1,false,0) && restored.furnaceItems(70,200,0)[0].id==12,
         "Player can replace furnace input");
 require(restored.giveCreativeItem(263),"Creative menu grants coal");
 auto carried=restored.carriedItems();int coalSlot=-1;
 for(int i=0;i<int(carried.size());++i)if(carried[i].id==263){coalSlot=i;break;}
 require(coalSlot>=0 && restored.transferFurnaceItem(70,200,0,coalSlot,false,1,1) &&
         restored.furnaceItems(70,200,0)[1].id==263,
         "Creative item selection can supply furnace fuel");
 require(restored.placeBlock(72,200,0,static_cast<Block>(118),0,{75,200,5},0),
         "Creative cauldron placement succeeds");
 require(!restored.useCauldron(72,200,0,374) && restored.getData(72,200,0)==0,
         "An empty cauldron cannot fill a bottle");
 require(restored.useCauldron(72,200,0,326) && restored.getData(72,200,0)==3,
         "Creative water bucket fills cauldron to source level three");
 require(restored.useCauldron(72,200,0,374) && restored.getData(72,200,0)==2,
         "Creative glass bottle consumes one cauldron level");
 bool foundPotion=false;for(const auto& stack:restored.carriedItems())foundPotion|=stack.id==373 && stack.damage==0;
 require(foundPotion,"Cauldron gives a water potion to the player");
 restored.save(path);World cauldronReload;require(cauldronReload.load(path) && cauldronReload.getData(72,200,0)==2,
         "Cauldron water level survives native save and reload");
 require(restored.breakBlock(70,200,0),"Mine furnace");
 require(!restored.canOpenFurnace(70,200,0),"Mined furnace no longer opens");
 restored.save(path);auto mined=PS3WorldStorage::readFile(path);auto minedRecord=mined->chunk(0,0,-4);
 if(minedRecord && minedRecord->extra)if(auto* tiles=dynamic_cast<TagList*>(minedRecord->extra->get(L"TileEntities")))
  for(int i=0;i<tiles->size();++i)if(auto* t=dynamic_cast<CompoundTag*>(tiles->get(i)))
   require(!(t->getString(L"id")==L"Furnace" && t->getInt(L"x")==6 && t->getInt(L"z")==-64),
           "Mining removes furnace tile data");
 std::cout<<"furnace tests passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
