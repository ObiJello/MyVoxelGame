#include "TerrainMesh.h"
#include "BlockFaceUV.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
 using namespace console;
 const int logs[]={20,116,117,153},planks[]={4,198,214,199};
 const int wool[]={64,210,194,178,162,146,130,114,225,209,193,177,161,145,129,113};
 for(int d=0;d<16;++d)for(int f=0;f<6;++f){
  const int axis=d/4;bool end=axis<3 && f/2==axis;
  require(textureTile(Log,f,d)==(end?21:logs[d%4]),"Log species, axis and all-bark faces");
  require(textureTile(Planks,f,d)==planks[d<4?d:0],"Plank species and original invalid-type fallback");
  require(textureTile(Wool,f,d)==wool[d],"All sixteen wool atlas colours");
  require(textureTile(Leaves,f,d)==(d%4==1?132:d%4==3?196:52),"Leaf species ignore decay flags; birch uses oak shape");
  int sandstone=f==0 || (f==1 && (d==1 || d==2))?176:f==1?208:d==1?229:d==2?230:192;
  require(textureTile(Sandstone,f,d)==sandstone,"Sandstone variants use smooth undersides and original fallback");
 }
 for(auto args:{std::pair{-1,0},std::pair{6,0},std::pair{0,-1},std::pair{0,16}}){
  bool rejected=false;try{textureTile(Wool,args.first,args.second);}catch(const std::invalid_argument&){rejected=true;}
  require(rejected,"Reject unsafe texture indices");
 }
 // Original unrotated top maps U along X and V along Z.
 const auto& top=consoleBlockFaceUV(Stone,0);
 require(top[0].u==0 && top[0].v==0 && top[1].u==0 && top[1].v==1 &&
         top[2].u==1 && top[2].v==1 && top[3].u==1 && top[3].v==0,"Original top face orientation");
 const int corners[6][4][3]={
 {{0,1,0},{0,1,1},{1,1,1},{1,1,0}},{{0,0,1},{0,0,0},{1,0,0},{1,0,1}},
 {{0,1,1},{0,1,0},{0,0,0},{0,0,1}},{{1,1,0},{1,1,1},{1,0,1},{1,0,0}},
 {{0,1,0},{1,1,0},{1,0,0},{0,0,0}},{{1,1,1},{0,1,1},{0,0,1},{1,0,1}}};
 for(int axis=0;axis<3;++axis)for(int f=0;f<6;++f){
  const auto& uv=consoleBlockFaceUV(Log,f,axis*4);
  int seen=0;for(const auto& p:uv){require((p.u==0 || p.u==1) && (p.v==0 || p.v==1),"Full cube UV endpoints");seen|=1<<(int(p.u)+2*int(p.v));}
  require(seen==15,"Each face uses all four UV corners");
  if(f/2==axis)continue; // End grain has no longitudinal bark direction.
  const int coordinate=axis==0?1:axis==1?0:2;
  for(int a=0;a<4;++a)for(int b=0;b<4;++b)
   require((uv[a].v==uv[b].v)==(corners[f][a][coordinate]==corners[f][b][coordinate]),"Bark V runs along the actual log axis");
 }
 World world;const Block types[]={Log,Planks,Leaves,Sandstone,Wool};const int data[]={6,2,3,1,14};
 for(int i=0;i<5;++i){world.set(10+i*3,180,10,types[i]);world.setData(10+i*3,180,10,data[i]);}
 const auto mesh=buildTerrainMesh(world);require(mesh.opaque.size()==180,"Five isolated blocks emit six faces each");
 for(int i=0;i<5;++i)for(int f=0;f<6;++f)for(int v=0;v<6;++v){
  const auto& vertex=mesh.opaque[i*36+f*6+v];int tile=int(std::floor(vertex.u*16))+16*int(std::floor(vertex.v*16));
  require(tile==textureTile(types[i],f,data[i]),"Terrain mesh uses actual stored block metadata");
  const int indices[]={0,1,2,0,2,3};const auto expected=consoleBlockFaceUV(types[i],f,data[i])[indices[v]];
  require(std::abs(vertex.u-(tile%16+(expected.u*.998f+.001f))/16)<.000001f &&
          std::abs(vertex.v-(tile/16+(expected.v*.998f+.001f))/16)<.000001f,"Actual mesh uses original face UV orientation");
 }
 for(int data=0;data<16;++data)for(int face=0;face<6;++face){
  const int expected[]={54,100,101,213};
  require(textureTile(static_cast<Block>(98),face,data)==expected[data<4?data:0],"Original stone brick variants and fallback");
 }
 World castle;require(castle.set(20,180,20,static_cast<Block>(98)),"Castle stone brick can be edited");castle.setData(20,180,20,2);
 require(castle.raycast({19,180.5,20.5},{1,0,0},2).hit,"Castle stone brick is pickable");
 const auto castleMesh=buildTerrainMesh(castle);
 for(const auto& v:castleMesh.opaque)require(int(std::floor(v.u*16))+16*int(std::floor(v.v*16))==101,"Cracked brick reaches actual terrain mesh");
 World plants;
 require(plants.set(10,180,10,static_cast<Block>(31)),"Tall grass is a supported natural block");
 plants.setData(10,180,10,2);
 auto plantMesh=buildTerrainMeshRegion(plants,plants.blockSnapshot(),10,10,1,1);
 require(plantMesh.opaque.size()==24,"Tall grass uses two double-sided cutout planes");
 for(const auto& v:plantMesh.opaque){
  require(v.x>10 && v.x<11 && v.z>10 && v.z<11,"Tall grass does not render as a full cube");
  require(int(std::floor(v.u*16))+16*int(std::floor(v.v*16))==56,"Fern metadata selects its atlas tile");
 }
 require(plants.set(12,180,10,static_cast<Block>(111)),"Waterlily is a supported natural block");
 auto lilyMesh=buildTerrainMeshRegion(plants,plants.blockSnapshot(),12,10,1,1);
 require(lilyMesh.opaque.size()==12,"Waterlily uses a double-sided surface");
 for(const auto& v:lilyMesh.opaque)require(std::abs(v.y-180.02f)<.0001f,"Waterlily lies flat on water");
 require(plants.set(14,180,10,static_cast<Block>(106)),"Vine is a supported natural block");
 plants.setData(14,180,10,1);
 auto vineMesh=buildTerrainMeshRegion(plants,plants.blockSnapshot(),14,10,1,1);
 require(vineMesh.opaque.size()==12,"One vine attachment uses a double-sided wall plane");
 for(const auto& v:vineMesh.opaque)require(std::abs(v.z-10.975f)<.0001f,"Vine metadata chooses its wall");
 require(plants.set(16,180,10,static_cast<Block>(81)),"Cactus is a supported natural block");
 auto cactusMesh=buildTerrainMeshRegion(plants,plants.blockSnapshot(),16,10,1,1);
 require(cactusMesh.opaque.size()==36,"Cactus renders six inset faces");
 for(const auto& v:cactusMesh.opaque)require(v.x>=16.0625f && v.x<=16.9375f && v.z>=10.0625f && v.z<=10.9375f,"Cactus is inset from its cell");
 require(validBlock(8) && validBlock(11) && textureTile(static_cast<Block>(8),0)==205 &&
         textureTile(static_cast<Block>(11),0)==237,"Lake and spring liquid IDs survive validation and atlas lookup");
 require(plants.set(18,180,10,static_cast<Block>(8)),"Flowing spring water is editable");
 auto flowingWater=buildTerrainMeshRegion(plants,plants.blockSnapshot(),18,10,1,1);
 require(flowingWater.water.size()==36,"Flowing water uses the transparent water pass");
 require(plants.set(20,180,10,static_cast<Block>(11)),"Still lake lava is editable");
 auto stillLava=buildTerrainMeshRegion(plants,plants.blockSnapshot(),20,10,1,1);
 require(stillLava.opaque.size()==36,"Still lava uses the liquid surface mesh");
 require(plants.set(22,180,10,static_cast<Block>(78)),"Source snow layer is a supported block");
 auto snowMesh=buildTerrainMeshRegion(plants,plants.blockSnapshot(),22,10,1,1);
 require(snowMesh.opaque.size()==36,"Snow layer emits an inset-height mesh");
 for(const auto& v:snowMesh.opaque)require(v.y>=180 && v.y<=180.125f,"Fresh snow is two pixels tall");
 require(!plants.collides({22.5,180,10.5}),"Fresh snow has no player collision");
 require(plants.raycast({22.5,181,10.5},{0,-1,0},2).hit,"Thin snow surface remains pickable");
 plants.setData(22,180,10,3);
 require(plants.collides({22.5,180,10.5}),"Layered snow collides at half-block height");
 require(validBlock(129) && textureTile(static_cast<Block>(129),0)==171,
         "Generated emerald ore retains its source terrain atlas slot");
 require(textureTile(static_cast<Block>(66),0,0)==128 && textureTile(static_cast<Block>(66),0,6)==112 &&
         textureTile(static_cast<Block>(103),0)==137 && textureTile(static_cast<Block>(103),3)==136 &&
         textureTile(static_cast<Block>(145),3)==215,
         "Framed rails, melon and anvil use the source terrain atlas slots");
 require(textureTile(static_cast<Block>(61),0,2)==62 && textureTile(static_cast<Block>(61),4,2)==44 &&
         textureTile(static_cast<Block>(61),5,2)==45 && textureTile(static_cast<Block>(62),4,2)==61 &&
         textureTile(static_cast<Block>(118),0)==138 && textureTile(static_cast<Block>(118),1)==155 &&
         textureTile(static_cast<Block>(118),4)==154,
         "Furnace facings and cauldron surfaces use source atlas slots");
 require(textureTile(static_cast<Block>(117),4)==157,"Brewing stand stem uses source terrain atlas slot");
 require(textureTile(static_cast<Block>(116),0)==166 && textureTile(static_cast<Block>(116),1)==183 &&
         textureTile(static_cast<Block>(116),3)==182,"Enchantment table base uses source top, bottom and side slots");
 World cauldronWorld;
 require(cauldronWorld.set(10,180,10,static_cast<Block>(118)) && cauldronWorld.setData(10,180,10,2),
         "Source cauldron accepts water-level metadata");
 auto cauldronMesh=buildTerrainMeshRegion(cauldronWorld,cauldronWorld.blockSnapshot(),10,10,1,1);
 bool inwardWall=false,basinFloor=false,waterSurface=false;
 for(const auto& vertex:cauldronMesh.opaque){
  const int tile=int(vertex.u*16)+16*int(vertex.v*16);
  inwardWall|=tile==154 && std::abs(vertex.x-(10+2.f/16-1.f/128))<.0001f;
  basinFloor|=tile==139 && std::abs(vertex.y-180.25f)<.0001f;
 }
 for(const auto& vertex:cauldronMesh.water)waterSurface|=std::abs(vertex.y-180.75f)<.0001f;
 require(inwardWall && basinFloor && waterSurface,"Cauldron basin and level-two water match the source shape");
 require(cauldronWorld.collides({10.5,180.2,10.5}) && !cauldronWorld.collides({10.5,180.35,10.5}) &&
         cauldronWorld.collides({10.2,180.35,10.5}),"Cauldron collision has floor, open basin and raised walls");
 require(cauldronWorld.set(13,180,10,static_cast<Block>(117)),"Brewing stand block is accepted");
 require(cauldronWorld.collides({13.5,180.2,10.5}) && !cauldronWorld.collides({13.9,180.2,10.5}),
         "Brewing stand has a narrow stem and low base instead of a full cube");
 require(cauldronWorld.set(16,180,10,static_cast<Block>(116)),"Enchantment table base is accepted");
 auto tableMesh=buildTerrainMeshRegion(cauldronWorld,cauldronWorld.blockSnapshot(),16,10,1,1);
 require(!tableMesh.opaque.empty() && cauldronWorld.collides({16.5,180.7,10.5}) &&
         !cauldronWorld.collides({16.5,180.8,10.5}),"Enchantment table base stops at the source 12-pixel height");
 for(const auto& vertex:tableMesh.opaque)require(vertex.y<=180.75f,"Enchantment table mesh uses its 12-pixel shape");
 std::cout<<"480 metadata/face selectors and actual terrain UVs passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
