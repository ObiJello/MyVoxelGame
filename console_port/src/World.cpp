#include "World.h"
#include "WorldState.h"
#include "BlockPlacement.h"
#include "BlockShape.h"
#include "TutorialSchematics.h"
#include "ChunkGenerator.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace console {
void World::swapWith(World& other) noexcept {
    state.swap(other.state);
    std::swap(seed,other.seed);
    std::swap(revision,other.revision);
}
const char* blockName(Block b) {
    switch(static_cast<int>(b)) {
    case Grass:return "Grass Block"; case Stone:return "Stone"; case Cobble:return "Cobblestone";
    case Planks:return "Oak Planks"; case Log:return "Oak Wood"; case Leaves:return "Oak Leaves";
    case Sand:return "Sand"; case Glass:return "Glass"; case Bricks:return "Bricks";
    case Obsidian:return "Obsidian";
    case static_cast<Block>(14):return "Gold Ore";
    case static_cast<Block>(15):return "Iron Ore";
    case static_cast<Block>(16):return "Coal Ore";
    case static_cast<Block>(21):return "Lapis Lazuli Ore";
    case static_cast<Block>(56):return "Diamond Ore";
    case static_cast<Block>(73):return "Redstone Ore";
    case static_cast<Block>(81):return "Cactus";
    case static_cast<Block>(87):return "Netherrack";
    case static_cast<Block>(129):return "Emerald Ore";
    case static_cast<Block>(153):return "Nether Quartz Ore";
    case Fence:return "Oak Fence"; case FenceGate:return "Fence Gate"; case NetherFence:return "Nether Brick Fence";
    case static_cast<Block>(130):return "Ender Chest";
    case static_cast<Block>(61):case static_cast<Block>(62):return "Furnace";
    case static_cast<Block>(118):return "Cauldron";
    case static_cast<Block>(117):return "Brewing Stand";
    default:return "Block";
    }
}
int variantTextureTile(Block block,int face,int data);
int textureTile(Block b,int face,int data) {
    if(face<0 || face>5 || data<0 || data>15)throw std::invalid_argument("Invalid block texture face or metadata");
    if(b==52)return 65; // PreStitchedTextureMap: mobSpawner at (1,4).
    if(b==65)return 83;
    if(b==Fence || b==FenceGate)return textureTile(Planks,face,0);
    if(b==NetherFence)return 224;
    // StoneSlabTile::getTexture: bit 3 selects an all-top double slab.
    if(b==43 || b==44){
        if(b==43 && (data&8))face=0;
        switch(data&7){
        case 0:return face<2?6:5;
        case 1:return textureTile(Sandstone,face,0);case 2:return textureTile(Planks,face,0);
        case 3:return textureTile(Cobble,face,0);case 4:return textureTile(Bricks,face,0);
        case 5:return 54;case 6:return 224;case 7:return face==0?219:face==1?251:235;
        }
    }
    // StairTile delegates to its registered base tile and base metadata.
    switch(static_cast<int>(b)){
    case 53:return textureTile(Planks,face,0);case 67:return textureTile(Cobble,face,0);
    case 108:return textureTile(Bricks,face,0);case 109:return 54;case 114:return 224;
    case 128:return textureTile(Sandstone,face,0);case 134:return textureTile(Planks,face,1);
    case 135:return textureTile(Planks,face,2);case 136:return textureTile(Planks,face,3);
    case 156:return face==0?219:face==1?251:235;
    }
    const int variant=variantTextureTile(b,face,data);
    if(variant>=0)return variant;
    switch(static_cast<int>(b)) {
    case Grass:return face==0?0:face==1?2:3; case Stone:return 1; case Dirt:return 2;
    case Cobble:return 16; case Bedrock:return 17;
    case Water:return 205; case Lava:return 237; case Sand:return 18;
    case 8:return 205; case 11:return 237; // flowing water and still lava
    case Glass:return 49; case Bricks:return 7;
    case Ice:return 67;
    case 78:return 66; // TopSnowTile's "snow" icon
    case Mycelium:return face==0?78:face==1?2:77;
    case Obsidian:return 37;
    // Native TitleUpdate terrain atlas slots from PreStitchedTextureMap.cpp.
    case 6:return data==1?63:data==2?79:data==3?30:15;
    case 13:return 19; // gravel
    case 14:return 32; // gold ore
    case 15:return 33; // iron ore
    case 16:return 34; // coal ore
    case 21:return 160; // lapis ore
    case 31:return data==2?56:data==1?39:55; // grass/fern/shrub
    case 32:return 55; // dead bush
    case 37:return 13; // yellow flower
    case 38:return 12; // rose
    case 39:return 29; // brown mushroom
    case 40:return 28; // red mushroom
    case 48:return 36; // mossy cobblestone in dungeon floors
    case 56:return 50; // diamond ore
    case 73:return 51; // redstone ore
    case 129:return 171; // emerald ore
    case 81:return face==0?69:face==1?71:70; // cactus
    case 82:return 72; // clay
    case 83:return 73; // reeds
    case 86:return face<2?102:118; // pumpkin
    case 66:return data>=6?112:128; // RailTile: rail_turn / rail
    case 103:return face<2?137:136; // MelonTile: melon_top / melon_side
    case 145:return 215; // AnvilTile base; shaped upper parts need the anvil renderer
    case 61:case 62:{
        if(face<2)return 62; // FurnaceTile top on both vertical faces.
        constexpr int originalFace[]{1,0,4,5,2,3};
        return originalFace[face]==data?(b==62?61:44):45;
    }
    case 118:return face==0?138:face==1?155:154; // CauldronTile rim, bottom, side
    case 117:return 157; // BrewingStandTile stem and radial arms
    case 116:return face==0?166:face==1?183:182; // EnchantmentTableTile
    case 99:return data==10?141:data==0?142:126;
    case 100:return data==10?141:data==0?142:125;
    case 106:return 143; // vine
    case 111:return 76; // waterlily
    case 127:return (data>>2)==0?170:(data>>2)==1?169:168;
    // FarmTile, CropTile, CarrotTile, PotatoTile, NetherStalkTile, StemTile
    // getTexture over PreStitchedTextureMap's slots (face 0 is the top here).
    case 60:return face==0?(data>0?86:87):2;          // farmland_wet / farmland_dry / dirt
    case 59:return 88+(data>7?7:data);                // crops_0..7
    case 141:case 142:{
        const int stage=data<7?((data==6?5:data)>>1):3;
        static const int carrots[]{200,201,202,203},potatoes[]{200,201,202,204};
        return b==141?carrots[stage]:potatoes[stage];
    }
    case 115:return data>=3?228:data>0?227:226;       // netherStalk_0..2
    case 104:case 105:return 111;                     // stem_straight
    case 74:return 51;                                // lit redstone ore
    // PreStitchedTextureMap: torch (0,5), redtorch (3,7), redtorch_lit (3,6).
    case 50:return 80;
    case 75:return 115;
    case 76:return 99;
    case 123:return 211; // redstoneLight
    case 124:return 212; // redstoneLight_lit
    default:return 1;
    }
}
bool solid(Block b) {
    // These original tile classes return a null collision AABB. Partial solid
    // shapes are resolved by collides; doors and ladders still need adapters.
    switch(static_cast<int>(b)){
    case 0:case 8:case 9:case 10:case 11:
    case 6:case 27:case 28:case 30:case 31:case 32:case 37:case 38:case 39:case 40:
    case 50:case 51:case 55:case 59:case 63:case 66:case 68:case 69:case 70:case 72:case 75:case 76:case 77:
    case 78:case 83:case 90:case 104:case 105:case 106:case 111:case 115:case 127:
    case 131:case 132:case 141:case 142:return false;
    default:return true;
    }
}
bool validBlock(std::uint8_t b) {
    switch(b){
    // Blocks emitted by the original ore, tree, shore and foliage features.
    case 6:case 13:case 14:case 15:case 16:case 21:case 31:case 32:
    case 37:case 38:case 39:case 40:case 48:case 56:case 73:case 81:case 82:
    case 78:case 83:case 86:case 99:case 100:case 106:case 111:case 127:case 129:
    case 8:case 11:case 61:case 62:case 116:case 117:case 118:case 130:return true;
    // Farming (hoes, seeds, stems) and random ticks.
    case 59:case 60:case 74:case 103:case 104:case 105:case 115:case 141:case 142:return true;
    // Fire (flint and steel, lava, spreading), torches and redstone.
    case 50:case 51:case 55:case 69:case 70:case 72:case 75:case 76:case 77:
    case 93:case 94:case 96:case 123:case 124:case 143:return true;
    default:break;
    }
    return consoleIsStair(b) || b==43 || b==44 || b==64 || b==71 || b==65 || b==85 || b==107 || b==113 || b==98 || b==52 || b==54 || b==Air || b==Stone || b==Grass || b==Dirt || b==Cobble || b==Planks ||
        b==Bedrock || b==Water || b==Lava || b==Sand || b==Log || b==Leaves || b==Glass || b==Wool || b==Bricks ||
        b==Sandstone || b==Obsidian || b==Ice || b==Mycelium;
}
// ServerPlayerGameMode::destroyBlock: Level::setTile(x, y, z, 0), so the
// neighbours react (a door's other half, a torch or plant on it, liquids).
bool World::breakBlock(int x,int y,int z){
    const int id=get(x,y,z),data=getData(x,y,z);
    if(id==Bedrock || id==Air || !setTileAndUpdate(x,y,z,Air))return false;
    tileDestroyed(x,y,z,id,data);
    if(id==54 || id==130 || id==61 || id==62 || id==117)
        discardContainerData(x,y,z,id==54?L"Chest":id==130?L"EnderChest":id==117?L"Cauldron":L"Furnace");
    return true;
}
bool World::placeBlock(int x,int y,int z,Block block,int data,Vec3 feet,double yaw,int face) {
    if(!inside(x,y,z) || !validBlock(block) || block==Air || data<0 || data>15)return false;
    const auto old=get(x,y,z);
    if(old!=Air && old!=Water)return false;
    if(block==FenceGate && (y==0 || !solid(get(x,y-1,z))))return false;
    // Resolve and validate before changing storage. Original log placement uses
    // player position and heading, including its close-range vertical override.
    const int facing=consolePlacementFacing(x,y,z,feet,yaw);
    int placedData=block==Log?consoleLogPlacementData(data,facing):data;
    if(block==FenceGate){
        // DirectionalTile placement uses the player's four-way horizontal heading.
        if(facing==2)placedData=0;else if(facing==5)placedData=1;
        else if(facing==3)placedData=2;else if(facing==4)placedData=3;
    }
    if(block==static_cast<Block>(130) || block==static_cast<Block>(61)){
        constexpr double pi=3.14159265358979323846;
        const double degrees=std::remainder(yaw,2*pi)*180/pi+180;
        const int direction=static_cast<int>(std::floor(degrees*4/360+.5))&3;
        constexpr int horizontal[]{2,5,3,4};
        placedData=horizontal[direction];
    }
    // TileItem::useOn: Level::mayPlace against the clicked face and
    // Tile::getPlacedOnFaceDataValue for the tiles whose placement is ported.
    switch(static_cast<int>(block)){
    case 50:case 55:case 65:case 69:case 70:case 72:case 75:case 76:case 77:case 93:case 96:case 143:
        if(face>=0 && face<=5){
            placedData=placementData(x,y,z,block,face,placedData);
            if(placedData<0)return false;
        }
        break;
    default:break;
    }
    const int oldData=getData(x,y,z);
    if(!set(x,y,z,block))return false;
    setData(x,y,z,placedData);
    if(collides(feet)){
        set(x,y,z,old);setData(x,y,z,oldData);
        return false;
    }
    // TileItem::useOn places with Level::setTileAndData, then Tile::setPlacedBy.
    tileStored(x,y,z,old,oldData);
    if(get(x,y,z)!=block)return true;
    if(block==static_cast<Block>(93))tilePlacedBy(x,y,z,yaw);
    if(block==static_cast<Block>(130))ensureEnderChestData(x,y,z);
    if(block==static_cast<Block>(61))ensureFurnaceData(x,y,z);
    if(block==static_cast<Block>(117))ensureBrewingData(x,y,z);
    return true;
}
bool World::inside(int x,int y,int z) const { return x>=originX() && x<originX()+width && y>=0 && y<height && z>=originZ() && z<originZ()+depth; }
int World::surface(int x,int z) const {
    for(int y=height-1;y>=0;--y) if(solid(get(x,y,z)))return y;
    return 0;
}
Vec3 World::spawn() const {
    if(isTutorial())return {double(state->metadata->getXSpawn()+width/2),
                            double(state->metadata->getYSpawn()),
                            double(state->metadata->getZSpawn()+depth/2)};
    for(int radius=0;radius<width/2;++radius)for(int dz=-radius;dz<=radius;++dz)for(int dx=-radius;dx<=radius;++dx) {
        int x=originX()+width/2+dx,z=originZ()+depth/2+dz,y=surface(x,z);
        if(get(x,y,z)==Grass && (isFlat() || y>sea) && y+3<height)return {x+.5,y+1.001,z+.5};
    }
    return {originX()+width/2+.5,static_cast<double>(height+2),originZ()+depth/2+.5};
}
bool World::collides(Vec3 p) const {return collides(p,.6,1.8);}
bool World::collides(Vec3 p,double width,double entityHeight) const {
    if(width<=0 || entityHeight<=0 || !std::isfinite(width) || !std::isfinite(entityHeight))return true;
    const double radius=width*.5-.001,top=entityHeight-.001;
    if(p.x<originX()+radius || p.x>originX()+World::width-radius ||
       p.z<originZ()+radius || p.z>originZ()+depth-radius || p.y<0)return true;
    struct Access final:BlockShapeAccess {
        const World& world;
        explicit Access(const World& w):world(w){}
        int getTile(int x,int y,int z)const override{return world.get(x,y,z);}
        int getData(int x,int y,int z)const override{return world.getData(x,y,z);}
    } access(*this);
    for(int z=static_cast<int>(std::floor(p.z-radius));z<=std::floor(p.z+radius);++z)
    for(int x=static_cast<int>(std::floor(p.x-radius));x<=std::floor(p.x+radius);++x)
    for(int y=static_cast<int>(std::floor(p.y));y<=std::floor(p.y+top);++y)
    {
        const int id=get(x,y,z);
        if(id==78){
            if((getData(x,y,z)&7)>=3 && p.x+radius>x && p.x-radius<x+1 &&
               p.y+top>y && p.y<y+.5 && p.z+radius>z && p.z-radius<z+1)return true;
            continue;
        }
        if(!solid(static_cast<Block>(id)))continue;
        if(!consoleIsPartialBlock(id))return true;
        const auto shape=consoleCollisionShape(access,x,y,z);
        for(int i=0;i<shape.count;++i){const auto& box=shape.boxes[i];
            if(p.x+radius>x+box.x0 && p.x-radius<x+box.x1 &&
               p.y+top>y+box.y0 && p.y<y+box.y1 &&
               p.z+radius>z+box.z0 && p.z-radius<z+box.z1)return true;
        }
    }
    return false;
}
}
