#pragma once
// Stand-ins for the engine classes the original tile tick methods touch.
// ported/tick/TileTickRules.cpp holds those methods, extracted unchanged by
// tools/extract_tile_ticks.py; they compile against these declarations, which
// forward every world access to a live Level (the game's World, or a test).
// Everything is in console::sim so the generation Level/Tile stand-ins used
// by the terrain library do not clash.
#include "Direction.h"
#include "Facing.h"
#include "LightLayer.h"
#include "Material.h"
#include "Random.h"
#include <memory>

namespace console::sim {
using ::Direction;
using ::Facing;
using ::LightLayer;
using ::Material;
using ::Random;
using std::shared_ptr;

// The pieces of Tile::SoundType, Player and ItemInstance the item methods use.
struct SoundType {
    int getStepSound()const{return 0;}
    float getVolume()const{return 1;}
    float getPitch()const{return 1;}
};
struct Abilities { bool instabuild=false; };
class Player {
public:
    Abilities abilities;
    bool mayBuild(int,int,int){return true;}
};
class ItemInstance {
public:
    int id=0,count=0,auxValue=0,damage=0;
    int getAuxValue()const{return auxValue;}
    // ItemInstance::hurt: the host wears the tool (and breaks it) afterwards.
    void hurt(int amount,shared_ptr<Player>){damage+=amount;}
};

struct Dimension {
    int id=0;
    bool ultraWarm=false,hasCeiling=false;
    bool isNaturalDimension()const{return id==0;}
};

// Level: the calls the extracted tick methods make, forwarded to the host.
class Level {
public:
    static constexpr int MAX_BRIGHTNESS=15,maxBuildHeight=256,genDepth=128,genDepthMinusOne=127;
    bool isClientSide=false;
    Random* random=nullptr;
    Dimension* dimension=nullptr;
    virtual ~Level()=default;

    virtual int getTile(int x,int y,int z)=0;
    virtual int getData(int x,int y,int z)=0;
    // The update variants notify the host (lighting, meshes, neighbours as
    // far as the port has them); NoUpdate stores only, as in the source.
    virtual bool setTile(int x,int y,int z,int tile)=0;
    virtual bool setTileAndData(int x,int y,int z,int tile,int data)=0;
    virtual bool setData(int x,int y,int z,int data)=0;
    virtual bool setTileNoUpdate(int x,int y,int z,int tile)=0;
    virtual bool setTileAndDataNoUpdate(int x,int y,int z,int tile,int data)=0;
    virtual bool setDataNoUpdate(int x,int y,int z,int data)=0;
    bool isEmptyTile(int x,int y,int z){return getTile(x,y,z)==0;}
    Material* getMaterial(int x,int y,int z);
    // Level::isSolidBlockingTile.
    bool isSolidBlockingTile(int x,int y,int z);
    // Sounds are not ported.
    void playSound(float,float,float,int,float,float){}

    // Level::getRawBrightness (with the slab/farmland/stair neighbour rule),
    // getDaytimeRawBrightness, getBrightness and canSeeSky.
    virtual int getRawBrightness(int x,int y,int z)=0;
    virtual int getDaytimeRawBrightness(int x,int y,int z)=0;
    virtual int getBrightness(LightLayer::variety layer,int x,int y,int z)=0;
    virtual bool canSeeSky(int x,int y,int z)=0;
    virtual bool isRainingAt(int x,int y,int z)=0;
    virtual bool hasChunksAt(int x0,int y0,int z0,int x1,int y1,int z1)=0;

    // Tile::spawnResources (drops for tile and data at the position).
    virtual void spawnResources(int x,int y,int z,int tile,int data)=0;
    // Sapling::growTree's feature placement: the host runs the original tree
    // feature (0 TreeFeature, 1 BasicTree, 2 SpruceFeature, 3 BirchFeature,
    // 4 jungle TreeFeature, 5 MegaTreeFeature) and returns Feature::place.
    // 6 and 7: HugeMushroomFeature(0) and (1) for Mushroom::growTree.
    enum class TreeKind { Tree,BigTree,Spruce,Birch,JungleTree,MegaJungle,BrownMushroom,RedMushroom };
    virtual bool placeTree(TreeKind kind,int height,Random& random,int x,int y,int z)=0;
};

// Tile: the registry and the base behaviour the tick methods rely on. Each
// registered id gets an instance of its class below (or Tile itself), with
// the properties of ported/TileProperties.cpp.
class Tile {
public:
#include "TileIds.inc"
    static Tile* tiles[256];
    static bool solid[256];
    static int lightBlock[256];
    static int lightEmission[256];
    static Tile *farmland,*sapling,*crops,*tallgrass,*flower,*rose;

    int id=0;
    SoundType* soundType=&defaultSound;
    static inline SoundType defaultSound;
    Material* material=nullptr;
    bool ticking=false,cubeShaped=true;

    virtual ~Tile()=default;
    bool isTicking()const{return ticking;}
    virtual bool isCubeShaped(){return cubeShaped;}
    virtual void tick(Level*,int,int,int,Random*){}
    virtual bool shouldTileTick(Level*,int,int,int){return true;}
    virtual bool canSurvive(Level*,int,int,int){return true;}
    virtual bool mayPlace(Level* level,int x,int y,int z);
    virtual void onRemove(Level*,int,int,int,int,int){}
    virtual void handleRain(Level*,int,int,int){}
    virtual int getPlacedOnFaceDataValue(Level*,int,int,int,int,float,float,float,int itemValue){return itemValue;}
    // spawnResources(level, x, y, z, data, playerBonusLevel): the item drops
    // (ported SurvivalRules, including the leaf and cocoa overrides).
    void spawnResources(Level* level,int x,int y,int z,int data,int){level->spawnResources(x,y,z,id,data);}
};

class Bush:public Tile {
public:
    virtual bool mayPlaceOn(int tile);
    bool canSurvive(Level* level,int x,int y,int z)override;
    void tick(Level* level,int x,int y,int z,Random* random)override;
    void checkAlive(Level* level,int x,int y,int z);
};
class TallGrass:public Bush { public: static const int TALL_GRASS=1; };
class DeadBushTile:public Bush { public: bool mayPlaceOn(int tile)override; };
class WaterlilyTile:public Bush {
public:
    bool mayPlaceOn(int tile)override;
    bool canSurvive(Level* level,int x,int y,int z)override;
};
class Sapling:public Bush {
public:
    static const int TYPE_DEFAULT=0,TYPE_EVERGREEN=1,TYPE_BIRCH=2,TYPE_JUNGLE=3,TYPE_MASK=3,AGE_BIT=8;
    void tick(Level* level,int x,int y,int z,Random* random)override;
    void growTree(Level* level,int x,int y,int z,Random* random);
    bool isSapling(Level* level,int x,int y,int z,int type);
};
class CropTile:public Bush {
public:
    bool mayPlaceOn(int tile)override;
    void tick(Level* level,int x,int y,int z,Random* random)override;
    float getGrowthSpeed(Level* level,int x,int y,int z);
    void growCropsToMax(Level* level,int x,int y,int z);
};
class CarrotTile:public CropTile {};
class PotatoTile:public CropTile {};
class StemTile:public Bush {
public:
    Tile* fruit=nullptr;
    bool mayPlaceOn(int tile)override;
    void tick(Level* level,int x,int y,int z,Random* random)override;
    float getGrowthSpeed(Level* level,int x,int y,int z);
    void growCropsToMax(Level* level,int x,int y,int z);
};
class Mushroom:public Bush {
public:
    bool growTree(Level* level,int x,int y,int z,Random* random);
    bool mayPlaceOn(int tile)override;
    bool canSurvive(Level* level,int x,int y,int z)override;
    void tick(Level* level,int x,int y,int z,Random* random)override;
};
class NetherStalkTile:public Bush {
public:
    static const int MAX_AGE=3;
    bool mayPlaceOn(int tile)override;
    bool canSurvive(Level* level,int x,int y,int z)override;
    void tick(Level* level,int x,int y,int z,Random* random)override;
};
class GrassTile:public Tile {
public:
    static const int MIN_BRIGHTNESS=4;
    void tick(Level* level,int x,int y,int z,Random* random)override;
    bool shouldTileTick(Level* level,int x,int y,int z)override;
};
class MycelTile:public Tile {
public:
    static const int MIN_BRIGHTNESS=4;
    void tick(Level* level,int x,int y,int z,Random* random)override;
};
class FarmTile:public Tile {
public:
    void tick(Level* level,int x,int y,int z,Random* random)override;
    bool isUnderCrops(Level* level,int x,int y,int z);
    bool isNearWater(Level* level,int x,int y,int z);
};
class ReedTile:public Tile {
public:
    void tick(Level* level,int x,int y,int z,Random* random)override;
    bool mayPlace(Level* level,int x,int y,int z)override;
    bool canSurvive(Level* level,int x,int y,int z)override;
    bool shouldTileTick(Level* level,int x,int y,int z)override;
};
class CactusTile:public Tile {
public:
    void tick(Level* level,int x,int y,int z,Random* random)override;
    bool canSurvive(Level* level,int x,int y,int z)override;
    bool shouldTileTick(Level* level,int x,int y,int z)override;
};
class DirectionalTile:public Tile {
public:
    static const int DIRECTION_MASK=3,DIRECTION_INV_MASK=0xC;
    static int getDirection(int data);
};
class CocoaTile:public DirectionalTile {
public:
    void tick(Level* level,int x,int y,int z,Random* random)override;
    bool canSurvive(Level* level,int x,int y,int z)override;
    int getPlacedOnFaceDataValue(Level* level,int x,int y,int z,int face,float clickX,float clickY,float clickZ,int itemValue)override;
    static int getAge(int data);
};
class TreeTile:public Tile {
public:
    static const int DARK_TRUNK=1,BIRCH_TRUNK=2,JUNGLE_TRUNK=3,MASK_TYPE=0x3;
    static int getWoodType(int data);
    void onRemove(Level* level,int x,int y,int z,int id,int data)override;
};
class LeafTile:public Tile {
public:
    static const int REQUIRED_WOOD_RANGE=4,UPDATE_LEAF_BIT=8,PERSISTENT_LEAF_BIT=4,
        NORMAL_LEAF=0,EVERGREEN_LEAF=1,BIRCH_LEAF=2,JUNGLE_LEAF=3,LEAF_TYPE_MASK=3;
    int* checkBuffer=nullptr;
    ~LeafTile()override{delete[] checkBuffer;}
    void tick(Level* level,int x,int y,int z,Random* random)override;
    void onRemove(Level* level,int x,int y,int z,int id,int data)override;
    bool shouldTileTick(Level* level,int x,int y,int z)override;
    void die(Level* level,int x,int y,int z);
};
class VineTile:public Tile {
public:
    void tick(Level* level,int x,int y,int z,Random* random)override;
    bool isAcceptableNeighbor(int id);
};
class IceTile:public Tile {
public:
    void tick(Level* level,int x,int y,int z,Random* random)override;
    bool shouldTileTick(Level* level,int x,int y,int z)override;
};
class TopSnowTile:public Tile {
public:
    void tick(Level* level,int x,int y,int z,Random* random)override;
    bool shouldTileTick(Level* level,int x,int y,int z)override;
};
class SnowTile:public Tile {
public:
    void tick(Level* level,int x,int y,int z,Random* random)override;
    bool shouldTileTick(Level* level,int x,int y,int z)override;
};
class CauldronTile:public Tile {
public:
    void handleRain(Level* level,int x,int y,int z)override;
};
class RedStoneOreTile:public Tile {
public:
    void tick(Level* level,int x,int y,int z,Random* random)override;
    bool shouldTileTick(Level* level,int x,int y,int z)override;
};

// Items whose useOn the port has from the source (hoes, seeds, bone meal and
// cocoa beans).
class Item {
public:
    static Random* random;
    int id=0;
};
class HoeItem:public Item {
public:
    bool useOn(shared_ptr<ItemInstance> instance,shared_ptr<Player> player,Level* level,int x,int y,int z,int face,float clickX,float clickY,float clickZ,bool bTestUseOnOnly=false);
};
class SeedItem:public Item {
public:
    int resultId=0,targetLand=0;
    bool useOn(shared_ptr<ItemInstance> instance,shared_ptr<Player> player,Level* level,int x,int y,int z,int face,float clickX,float clickY,float clickZ,bool bTestUseOnOnly=false);
};
class SeedFoodItem:public Item {
public:
    int resultId=0,targetLand=0;
    bool useOn(shared_ptr<ItemInstance> instance,shared_ptr<Player> player,Level* level,int x,int y,int z,int face,float clickX,float clickY,float clickZ,bool bTestUseOnOnly=false);
};
class DyePowderItem:public Item {
public:
    static const int BROWN=3,WHITE=15;
    bool useOn(shared_ptr<ItemInstance> itemInstance,shared_ptr<Player> player,Level* level,int x,int y,int z,int face,float clickX,float clickY,float clickZ,bool bTestUseOnOnly=false);
};

// Builds the registry from ported/TileProperties.cpp (once).
void initializeTiles();
// Item::useOn for the registered item id; false when the item has no
// ported useOn or does not apply. `face` is Facing (0 down ... 5 east).
bool useItemOn(Level& level,ItemInstance& item,int x,int y,int z,int face);
}
