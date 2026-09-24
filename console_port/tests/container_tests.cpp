#include "TutorialSchematics.h"
#include "ContainerItems.h"
#include "World.h"
#include "ChestMesh.h"
#include "ItemIcons.h"
#include "PS3WorldStorage.h"
#include <iostream>
#include <unistd.h>
static void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
static void travel(console::World& world,double x,double z){for(int i=0;i<600;++i)if(world.streamAround({x,100,z},2)||!world.streaming())return;throw std::runtime_error("Streaming did not finish");}
int main(int argc,char** argv){try{
 using namespace console;require(argc==3,"Expected tutorial assets and scratch directory");
 TutorialSchematics tutorial(argv[1]);require(tutorial.containers().size()==22,"Original package has 22 placed chests");int overworld=0,nether=0,discs=0,books=0;
 for(const auto& c:tutorial.containers()){
  if(c.dimension==0)++overworld;else if(c.dimension==-1)++nether;
  auto contents=containerItems(*c.tag,27);
  for(const auto& item:contents){if(item.id>=2256 && item.id<=2267){++discs;require(item.marker>=1 && item.marker<=12,"Music discs retain objective tags");}if(item.id==403){++books;require(item.enchanted,"Enchanted books retain enchantments");}}
  int cx=int(std::floor(c.x/16.)),cz=int(std::floor(c.z/16.));ChunkStorage chunk;tutorial.apply(chunk,cx,cz);
  if(c.dimension==0)require(chunk.blocks[((c.x-cx*16)*16+c.z-cz*16)*256+c.y]==54 && chunk.metadata.get(c.x-cx*16,c.y,c.z-cz*16)==c.facing,"Chest block placed with source facing");
 }
 require(overworld==21 && nether==1 && discs==12 && books==2,"Dimension filtering and original tutorial loot inventory");
 require(tutorial.spawners().size()==1,"Original tutorial includes one placed spawner");
 const auto& spawner=tutorial.spawners().front();
 require(spawner.dimension==0 && spawner.x==365 && spawner.y==74 && spawner.z==-57 && spawner.tag->getString(L"EntityId")==L"Spider","Original spider spawner action and target");
 require(textureTile(static_cast<Block>(52),2)==65,"Spawner uses the original terrain atlas tile");
 ChunkStorage spawnerChunk;tutorial.apply(spawnerChunk,22,-4);
 require(spawnerChunk.blocks[((365-22*16)*16+(-57-(-4)*16))*256+74]==52,"Spawner block placed at original action coordinate");
 auto spawnerTags=tutorial.tagsForChunk(22,-4);bool foundSpawner=false;
 for(int i=0;i<spawnerTags->getList(L"TileEntities")->size();++i)if(auto* tag=dynamic_cast<CompoundTag*>(spawnerTags->getList(L"TileEntities")->get(i)))
  if(tag->getString(L"id")==L"MobSpawner" && tag->getInt(L"x")==365 && tag->getInt(L"y")==74 && tag->getInt(L"z")==-57 && tag->getShort(L"Delay")==20 && tag->getShort(L"SpawnCount")==4)foundSpawner=true;
 require(foundSpawner,"Original spawner tile data is assigned to its owning chunk");
 CompoundTag source,destination;auto list=std::make_unique<TagList>();auto item=std::make_unique<CompoundTag>();item->putByte(L"Slot",4);item->putShort(L"id",2256);item->putByte(L"Count",1);item->putShort(L"Damage",0);item->putString(L"Unknown",L"retained");list->add(item.get());item.release();source.put(L"Items",list.get());list.release();
 require(moveContainerStack(source,27,4,destination,36),"Transfer into first empty inventory slot");
 require(!containerItems(source,27)[4].id && containerItems(destination,36)[0].id==2256,"Transfer removes source stack exactly once");
 require(static_cast<CompoundTag*>(destination.getList(L"Items")->get(0))->getString(L"Unknown")==L"retained","Unknown NBT survives transfer");
 require(!moveContainerStack(source,27,4,destination,36),"Repeating empty-slot transfer cannot duplicate items");
 require(moveContainerStack(destination,36,0,source,27),"Can return stack to chest");
 // A full destination must leave both source and destination unchanged.
 CompoundTag full;auto fullItems=std::make_unique<TagList>();for(int i=0;i<36;++i){auto entry=std::make_unique<CompoundTag>();entry->putByte(L"Slot",i);entry->putShort(L"id",346);entry->putByte(L"Count",1);entry->putShort(L"Damage",0);fullItems->add(entry.get());entry.release();}full.put(L"Items",fullItems.get());fullItems.release();
 require(!moveContainerStack(source,27,0,full,36) && containerItems(source,27)[0].id==2256,"Full inventory does not destroy source items");
 std::filesystem::create_directories(argv[2]);auto name=(std::filesystem::path(argv[2])/"container-XXXXXX").string();std::vector<char> temp(name.begin(),name.end());temp.push_back(0);require(mkdtemp(temp.data()),"Create scratch directory");
 struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code e;std::filesystem::remove_all(path,e);}}cleanup{temp.data()};
 World quickbar;quickbar.generate(77,true);
 require(!quickbar.carriedItems()[0].id && !quickbar.carriedItems()[8].id,
         "New creative quickbar reads empty source inventory slots");
 require(quickbar.setCreativeHotbarItem(4,Bricks) && quickbar.setCreativeHotbarItem(6,373,8194),
         "Creative selection writes the chosen source hotbar slots");
 require(quickbar.carriedItems()[4].id==Bricks && quickbar.carriedItems()[4].count==1 &&
         quickbar.carriedItems()[6].id==373 && quickbar.carriedItems()[6].damage==8194 &&
         quickbar.carriedItems()[6].count==1,"Quickbar uses source item stack limits and potion data");
 require(quickbar.setCreativeHotbarItem(8,Bricks,0,64) && quickbar.carriedItems()[8].count==64 &&
         !quickbar.setCreativeHotbarItem(7,373,0,64),
         "Creative quick action fills a stack but still respects unstackable items");
 require(quickbar.swapCarriedSlots(4,6) && quickbar.carriedItems()[4].id==373 &&
         quickbar.carriedItems()[6].id==Bricks,"Swapping preserves both carried stacks");
 require(quickbar.setCreativeHotbarItem(6,Stone) && quickbar.carriedItems()[4].damage==8194 &&
         quickbar.carriedItems()[6].id==Stone && !quickbar.swapCarriedSlots(6,36),
         "Replacing one creative slot leaves other slots intact and rejects invalid swaps");
 const auto quickbarPath=cleanup.path/"quickbar.inner";quickbar.save(quickbarPath);
 World quickbarReload;require(quickbarReload.load(quickbarPath) &&
         quickbarReload.carriedItems()[4].id==373 && quickbarReload.carriedItems()[4].damage==8194 &&
         quickbarReload.carriedItems()[6].id==Stone,"Selected items survive native save and reload");
 World world;world.generateTutorial(argv[1]);
 travel(world,98,30);
 auto fishing=world.chestItems(98,63,30);require(fishing.size()==27 && fishing[0].id==346 && fishing[7].id==346 && fishing[8].id==0,"Actual fishing chest contents");
 travel(world,161,-42);
 require(world.transferChestItem(112,81,-33,0,true),"Take tutorial disc from source chest");require(world.carriedItems()[0].id==2256 && world.carriedItems()[0].marker==1,"Carried disc keeps collection tag");
 require(world.set(112,82,-33,Stone),"Place solid block above source chest");
 require(!world.canOpenChest(112,81,-33) && !world.transferChestItem(112,81,-33,1,true),"Blocked chest cannot transfer items");
 world.set(112,82,-33,Air);
 auto path=cleanup.path/"world.inner";world.save(path);World restored;require(restored.load(path),"Reload chest transfer save");
 require(restored.carriedItems()[0].id==2256 && !restored.chestItems(112,81,-33)[0].id,"Save retains both sides without duplication");
 require(restored.transferChestItem(112,81,-33,0,false),"Return carried stack to chest after reload");
 require(!restored.carriedItems()[0].id && restored.chestItems(112,81,-33)[0].id==2256,"Deposit preserves stack");
 // PlayerEnderChestContainer shares one 27-slot EnderItems list across blocks.
 require(restored.transferChestItem(112,81,-33,0,true),"Take source disc for personal ender inventory");
 travel(restored,64,0);
 require(restored.canOpenEnderChest(59,85,-33) && restored.canOpenEnderChest(59,85,-27),
         "Both tutorial ender chests can access the same player inventory");
 require(restored.enderChestItems().size()==27 && restored.transferEnderChestItem(59,85,-33,0,false),
         "First ender chest accepts a carried source item");
 require(restored.enderChestItems()[0].id==2256 && !restored.carriedItems()[0].id,
         "EnderItems retains the original item identity and clears the carried slot");
 require(restored.set(59,86,-33,Stone) && !restored.canOpenEnderChest(59,85,-33) &&
         !restored.transferEnderChestItem(59,85,-33,0,true) && restored.canOpenEnderChest(59,85,-27),
         "A solid lid blocks only its own ender chest");
 restored.set(59,86,-33,Air);
 require(restored.placeBlock(70,200,0,static_cast<Block>(130),0,{75,200,5},0),
         "Creative ender chest placement succeeds away from the player");
 restored.save(path);
 {
  auto placedArchive=PS3WorldStorage::readFile(path);
  auto record=placedArchive->chunk(0,0,-4);
  bool found=false;
  if(record && record->extra)if(auto* tiles=dynamic_cast<TagList*>(record->extra->get(L"TileEntities")))
   for(int i=0;i<tiles->size();++i)if(auto* tile=dynamic_cast<CompoundTag*>(tiles->get(i)))
    found|=tile->getString(L"id")==L"EnderChest" && tile->getInt(L"x")==6 &&
           tile->getInt(L"y")==200 && tile->getInt(L"z")==-64;
  require(found,"Placed ender chest writes a native EnderChest tile entity");
 }
 require(restored.breakBlock(70,200,0),"Placed ender chest can be mined");
 restored.save(path);
 {
  auto minedArchive=PS3WorldStorage::readFile(path);
  auto record=minedArchive->chunk(0,0,-4);
  if(record && record->extra)if(auto* tiles=dynamic_cast<TagList*>(record->extra->get(L"TileEntities")))
   for(int i=0;i<tiles->size();++i)if(auto* tile=dynamic_cast<CompoundTag*>(tiles->get(i)))
    require(!(tile->getString(L"id")==L"EnderChest" && tile->getInt(L"x")==6 &&
              tile->getInt(L"y")==200 && tile->getInt(L"z")==-64),
            "Mining removes only the placed ender chest tile entity");
 }
 World enderReload;require(enderReload.load(path) && enderReload.enderChestItems()[0].id==2256,
         "Source EnderItems survive the native save and reload path");
 travel(enderReload,64,0);
 require(enderReload.transferEnderChestItem(59,85,-27,0,true) &&
         !enderReload.enderChestItems()[0].id && enderReload.carriedItems()[0].id==2256,
         "Second ender chest retrieves the first chest's item after reload");
 travel(restored,161,-42);
 require(restored.breakBlock(112,81,-33),"Can mine tutorial chest");
 restored.set(112,81,-33,static_cast<Block>(54));
 require(!restored.chestItems(112,81,-33)[0].id,"Replacing a mined chest cannot restore original loot");
 // Partial inventory changes survive the actual tutorial save path.
 require(restored.transferChestItem(106,81,-16,0,true,0),"Take half of tutorial experience bottles");
 require(restored.carriedItems()[0].count==15 && restored.chestItems(106,81,-16)[0].count==15,"Half stack retains both quantities");
 restored.save(path);restored.load(path);
 require(restored.transferChestItem(106,81,-16,0,true,1) && restored.carriedItems()[0].count==16,"Single item merges after save reload");
 require(restored.transferChestItem(106,81,-16,0,false) && restored.chestItems(106,81,-16)[0].count==30,"Returned items merge back without duplication");
 // Pair crosses an original chunk boundary; retain each half's own NBT.
 restored.set(159,200,-40,static_cast<Block>(54));restored.setData(159,200,-40,2);
 restored.set(160,200,-40,static_cast<Block>(54));restored.setData(160,200,-40,2);restored.save(path);
 auto archive=PS3WorldStorage::readFile(path);
 for(int x:{159,160}){
  auto record=archive->chunk(0,int(std::floor((x-64)/16.)),int(std::floor((-40-64)/16.)));
  auto chest=std::make_unique<CompoundTag>();chest->putString(L"id",L"Chest");chest->putInt(L"x",x-64);chest->putInt(L"y",200);chest->putInt(L"z",-104);
  auto slots=std::make_unique<TagList>();for(int i=0;i<(x==159?27:1);++i){auto stack=std::make_unique<CompoundTag>();stack->putByte(L"Slot",i);stack->putShort(L"id",x==159?346:2257);stack->putByte(L"Count",1);stack->putShort(L"Damage",0);slots->add(stack.get());stack.release();}
  chest->put(L"Items",slots.get());slots.release();auto* list=dynamic_cast<TagList*>(record->extra->get(L"TileEntities"));if(!list){record->extra->put(L"TileEntities",new TagList());list=record->extra->getList(L"TileEntities");}list->add(chest.get());chest.release();archive->putChunk(0,*record);
 }
 archive->writeFile(path);restored.load(path);
 auto left=restored.chestItems(159,200,-40),right=restored.chestItems(160,200,-40);
 require(left.size()==54 && right.size()==54 && left[0].id==346 && right[0].id==346 && left[27].id==2257 && right[27].id==2257,"Either half exposes identical source compound slot order");
 require(restored.transferChestItem(159,200,-40,27,true),"Take second-half stack through first half");
 require(restored.transferChestItem(160,200,-40,0,false),"Full first half routes deposit to second half");
 require(restored.chestItems(159,200,-40)[27].id==2257 && !restored.carriedItems()[0].id,"Deposit preserves second-half item exactly once");
 restored.set(160,201,-40,Stone);require(!restored.canOpenChest(159,200,-40),"Blocked neighbor lid blocks entire double chest");restored.set(160,201,-40,Air);
 restored.save(path);travel(restored,400,400);travel(restored,161,-42);
 require(restored.chestItems(159,200,-40)[27].id==2257,"Double chest contents survive chunk eviction");
 travel(restored,429,7);require(restored.get(429,74,7)==static_cast<Block>(52),"Streamed spider site contains the tutorial spawner");
 World pairReload;require(pairReload.load(path) && pairReload.chestItems(160,200,-40)[27].id==2257,"Both chunk records survive disk reload");
 pairReload.breakBlock(159,200,-40);require(pairReload.chestItems(160,200,-40).size()==27 && pairReload.chestItems(160,200,-40)[0].id==2257,"Mining one half preserves the other half's inventory");
 // First deposit initializes the source-format record for a new empty chest.
 pairReload.set(162,200,-40,static_cast<Block>(54));pairReload.transferChestItem(160,200,-40,0,true);
 require(pairReload.transferChestItem(162,200,-40,0,false) && pairReload.chestItems(162,200,-40)[0].id==2257,"New empty chest accepts a persistent deposit");
 for(int facing=2;facing<=5;++facing){auto mesh=buildChestMesh(0,0,0,facing,15<<20);
  require(mesh.size()==108,"Original chest model has three six-face cuboids");
  for(const auto& v:mesh){require(v.x>=0 && v.x<=1 && v.y>=0 && v.y<=.875 && v.z>=0 && v.z<=1,"Chest model fits original fourteen-pixel height");require(v.u>=0 && v.u<=1 && v.v>=0 && v.v<=1,"Original chest UVs stay on 64-square texture");}
  // The lock is the first cuboid and faces the metadata direction.
  float lx=0,lz=0;for(int i=0;i<36;++i){lx+=mesh[i].x/36;lz+=mesh[i].z/36;}
  require(facing==2?lz<.1:facing==3?lz>.9:facing==4?lx<.1:lx>.9,"Chest lock faces original direction");
 }
 for(int facing=2;facing<=5;++facing){auto mesh=buildChestMesh(0,0,0,facing,0,true);
  require(mesh.size()==108,"Large chest replaces pair with one model");
  for(const auto& v:mesh)require(v.x>=0 && v.x<=(facing<4?2:1) && v.z>=0 && v.z<=(facing<4?1:2) && v.u>=0 && v.u<=1,"Large chest spans correct axis with 128-wide atlas UVs");
 }
 World meshWorld;meshWorld.set(30,180,30,static_cast<Block>(54));meshWorld.setData(30,180,30,2);meshWorld.set(31,180,30,static_cast<Block>(54));meshWorld.setData(31,180,30,2);
 auto pairMesh=buildTerrainMesh(meshWorld);require(pairMesh.largeChests.size()==72 && pairMesh.largeChestLids.size()==36 && pairMesh.chests.empty(),"Actual terrain emits one large body and lid without duplicate halves");
 require(consoleItemIcon(346).tile==69 && consoleItemIcon(2256).tile==240,"Original rod and record icon registrations");
 std::cout<<"Original tutorial chests, item tags, transfers and persistent inventory passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
