#include "World.h"
#include "TutorialSchematics.h"
#include "PS3WorldStorage.h"
#include "TerrainMesh.h"
#include <cmath>
#include <iostream>
#include <unistd.h>
static void require(bool b,const char* message){if(!b)throw std::runtime_error(message);}
static void travel(console::World& w,double x,double z){for(int i=0;i<300;++i)if(w.streamAround({x,150,z},2) || !w.streaming())return;throw std::runtime_error("Tutorial streaming did not finish");}
int main(int argc,char** argv){try{
 using namespace console;require(argc==3,"Expected assets and scratch paths");
 std::filesystem::create_directories(argv[2]);auto name=(std::filesystem::path(argv[2])/"tutorial-world-XXXXXX").string();std::vector<char> temp(name.begin(),name.end());temp.push_back(0);
 require(mkdtemp(temp.data()),"Tutorial fixture directory");struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);}}cleanup{temp.data()};
 World world;world.generateTutorial(argv[1]);require(world.isTutorial() && world.seed==tutorialSeed,"Original tutorial identity");auto spawn=world.spawn();
 require(spawn.x==161 && spawn.y==72 && spawn.z==-42 && world.inside(int(spawn.x),72,int(spawn.z)),"Spawn window uses original signed coordinates");
 TutorialSchematics tutorial(argv[1]);const auto& castle=tutorial.schematic("CasTes2.sch");
 for(int y=0;y<castle.height();++y)require(world.get(161,60+y,-42)==castle.block(103,y,82),"Loaded tutorial spawn column matches original castle exactly");

 require(!world.collides(spawn),"Original tutorial spawn is unobstructed");
 require(world.blockLight(161,72,-42)>=0 && world.skyLight(161,72,-42)>=0,"Tutorial lighting initialized");
 travel(world,64,0);
 const auto hangingCount=world.hangingDecorations().size();
 require(hangingCount>=30,"Tutorial paintings and item frames activate from original schematic NBT");
 int paintings=0,frames=0,filled=0;
 for(const auto& decoration:world.hangingDecorations()){
  if(decoration.kind==HangingDecoration::Kind::Painting)++paintings;
  else{++frames;if(decoration.itemId>0)++filled;}
 }
 require(paintings>=9 && frames>=25 && filled>=25,"Tutorial artwork and displayed frame items retain source identity");
 const int skullTypes[]={4,3,2,1,0};
 for(int i=0;i<5;++i){
  auto skull=world.skullInfo(32+i,97,-1);
  require(skull && skull->type==skullTypes[i] && skull->rotation==14,
          "Tutorial skull tile entities retain all five source skin types and rotations");
 }
 auto blocks=world.blockSnapshot();
 auto skullMesh=buildTerrainMeshRegion(world,blocks,32,-1,5,1);
 for(const auto& skin:skullMesh.skulls)require(skin.size()==36,"Tutorial renders each skull with its own source skin");
 auto enderMesh=buildTerrainMeshRegion(world,blocks,59,-33,1,7);
 require(enderMesh.enderChests.size()==144 && enderMesh.enderChestLids.size()==72,
         "Both tutorial ender chests use the original chest model rather than terrain cubes");
 require(world.get(39,97,-18)==static_cast<Block>(117),"Legacy Cauldron tile entity belongs to a brewing stand");
 auto brewing=buildTerrainMeshRegion(world,blocks,39,-18,1,1);
 bool base=false,arm=false;
 for(const auto& vertex:brewing.opaque){
  const int tile=int(vertex.u*16)+16*int(vertex.v*16);
  base|=tile==156;arm|=tile==157 && std::abs(vertex.y-98.f)<.0001f;
 }
 require(base && arm,"Tutorial brewing stand has source foot pieces and bottle arms");
 require(world.get(85,81,-42)==static_cast<Block>(116),"Tutorial enchanting table occupies source location");
 auto table=buildTerrainMeshRegion(world,blocks,85,-42,1,1);
 require(table.enchantTables.size()==1 && table.enchantTables[0]==std::array<int,3>{85,81,-42},
         "Tutorial enchanting table schedules its source book model");
 travel(world,400,400);travel(world,64,0);
 require(world.hangingDecorations().size()==hangingCount,"Hanging decorations survive chunk eviction without duplication");
 require(world.skullInfo(32,97,-1) && world.skullInfo(32,97,-1)->type==4,
         "Tutorial skull tile NBT survives chunk eviction");
 travel(world,161,-42);
 auto path=cleanup.path/"world.inner";world.save(path);
 auto archive=PS3WorldStorage::readFile(path);require(archive->containsEntry(L"console_port.tutorial.pck"),"Tutorial package retained for self-contained saves");
 world.set(161,200,-42,Log);world.setData(161,200,-42,6);
 require(world.set(162,200,-42,static_cast<Block>(134)) && world.setData(162,200,-42,7),"Tutorial stair edit accepted");
 require(world.set(163,200,-42,static_cast<Block>(44)) && world.setData(163,200,-42,13),"Tutorial upper slab edit accepted");
 require(world.set(164,200,-42,static_cast<Block>(64)) && world.set(164,201,-42,static_cast<Block>(64)),"Tutorial door edit accepted");
 world.setData(164,201,-42,9);world.useBlock(164,201,-42);
 travel(world,400,400);world.save(path);travel(world,161,-42);
 require(world.get(161,200,-42)==Log && world.getData(161,200,-42)==6,"Streaming never reapplies schematics over player edits");
 World restored;require(restored.load(path) && restored.isTutorial(),"Tutorial save reload identifies original content");
 require(restored.get(161,200,-42)==Log && restored.getData(161,200,-42)==6,"Tutorial edits survive disk reload");
 require(restored.get(162,200,-42)==134 && restored.getData(162,200,-42)==7,"Stair direction and inversion survive eviction and save");
 require(restored.get(163,200,-42)==44 && restored.getData(163,200,-42)==13,"Slab type and height survive eviction and save");
 require(restored.getData(164,200,-42)==4 && restored.getData(164,201,-42)==9,"Door open state and hinge survive save and eviction");
 require(restored.useBlock(164,201,-42)&&restored.getData(164,200,-42)==0,"Reloaded tutorial door can be closed");
 travel(restored,64,0);
 require(restored.skullInfo(32,97,-1) && restored.skullInfo(32,97,-1)->type==4,
         "Tutorial skull tile NBT survives native archive reload");
 require(restored.hangingDecorations().size()==hangingCount,
         "Tutorial hanging artwork survives native archive reload without duplication");
 travel(restored,-200,320);require(restored.residentChunks()<=144,"Tutorial exploration keeps resident storage bounded");
 std::cout<<"Original tutorial spawn, structures, lighting, streaming and native saves passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
