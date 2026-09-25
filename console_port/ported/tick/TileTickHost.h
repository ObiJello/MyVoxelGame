#pragma once
// Stand-ins for the engine classes the original tile methods touch.
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
#include <deque>
#include <memory>

namespace console::sim {
using ::Direction;
using ::Facing;
using ::LightLayer;
using ::Material;
using ::Random;
using std::shared_ptr;

#include "TileConstants.inc"

// The pieces of Tile::SoundType, Player, ItemInstance and Entity the methods use.
struct SoundType {
    int getStepSound()const{return 0;}
    float getVolume()const{return 1;}
    float getPitch()const{return 1;}
};
struct Abilities { bool instabuild=false; };
class Entity { public: virtual ~Entity()=default; };
// Statistics and achievements are not ported.
struct GenericStats {
    static int portalsCreated(){return 0;}
    static int InToTheNether(){return 0;}
    static int param_noArgs(){return 0;}
    static int param_InToTheNether(){return 0;}
};
class Player:public Entity {
public:
    Abilities abilities;
    bool mayBuild(int,int,int){return true;}
    void awardStat(int,int){}
};
class ItemInstance {
public:
    int id=0,count=0,auxValue=0,damage=0;
    int getAuxValue()const{return auxValue;}
    // ItemInstance::hurt: the host wears the tool (and breaks it) afterwards.
    void hurt(int amount,shared_ptr<Player>){damage+=amount;}
};
struct AABB {};
struct LevelEvent { static const int SOUND_OPEN_DOOR=1003; };
// Sounds and particles are client effects; the ids are what the calls name.
enum eSOUND_TYPE { eSoundType_RANDOM_FIZZ,eSoundType_FIRE_IGNITE };
enum ePARTICLE_TYPE { eParticleType_largesmoke };
inline void MemSect(int){}
// Math::random: java.lang.Math's shared generator.
struct Math { static double random(){static Random generator;return generator.nextDouble();} };

struct Dimension {
    int id=0;
    bool ultraWarm=false,hasCeiling=false;
    bool isNaturalDimension()const{return id==0;}
};

class Level;
class Tile;
// LevelChunk / ChunkSource: only whether a chunk is loaded.
struct LevelChunk {
    bool empty=true;
    bool isEmpty()const{return empty;}
};
struct ChunkSource {
    Level* level=nullptr;
    LevelChunk loaded{false},missing{true};
    LevelChunk* getChunk(int chunkX,int chunkZ);
};
// FallingTile: the entity HeavyTile::checkSlide hands to Level::addEntity.
class FallingTile:public Entity {
public:
    double x,y,z;
    int tile,data;
    FallingTile(Level*,double x,double y,double z,int tile,int data):x(x),y(y),z(z),tile(tile),data(data){}
};

// Level: the calls the extracted methods make. The host supplies storage
// (the NoUpdate writes, which run the old tile's onRemove and the new tile's
// onPlace as LevelChunk::setTileAndData does), queries and scheduling; the
// update variants and neighbour notifications are the source's own.
class Level {
public:
    static constexpr int MAX_BRIGHTNESS=15,maxBuildHeight=256,genDepth=128,genDepthMinusOne=127;
    static const int MAX_LEVEL_SIZE=30000000;
    bool isClientSide=false;
    bool noNeighborUpdate=false;
    // The world's width in chunks (LevelData::getXZSize), centred on 0.
    int chunkSourceXZSize=54;
    Random* random=nullptr;
    Dimension* dimension=nullptr;
    ChunkSource ownChunkSource{this};
    ChunkSource* chunkSource=&ownChunkSource;
    virtual ~Level()=default;

    virtual int getTile(int x,int y,int z)=0;
    virtual int getData(int x,int y,int z)=0;
    virtual bool setTileAndDataNoUpdate(int x,int y,int z,int tile,int data)=0;
    virtual bool setDataNoUpdate(int x,int y,int z,int data)=0;
    bool setTileNoUpdate(int x,int y,int z,int tile){return setTileAndDataNoUpdate(x,y,z,tile,0);}
    // Level::setTile / setTileAndData / setData: store, then tileUpdated.
    bool setTile(int x,int y,int z,int tile){
        if(setTileNoUpdate(x,y,z,tile)){tileUpdated(x,y,z,tile);return true;}
        return false;
    }
    bool setTileAndData(int x,int y,int z,int tile,int data){
        if(setTileAndDataNoUpdate(x,y,z,tile,data)){tileUpdated(x,y,z,tile);return true;}
        return false;
    }
    void setData(int x,int y,int z,int data,bool forceUpdate=false){
        if(setDataNoUpdate(x,y,z,data) || forceUpdate)tileUpdated(x,y,z,getTile(x,y,z));
    }
    // Level::tileUpdated
    void tileUpdated(int x,int y,int z,int tile){updateNeighborsAt(x,y,z,tile);}
    void updateNeighborsAt(int x,int y,int z,int tile);
    void neighborChanged(int x,int y,int z,int type);
    virtual bool hasChunk(int chunkX,int chunkZ)=0;

    bool isEmptyTile(int x,int y,int z){return getTile(x,y,z)==0;}
    Material* getMaterial(int x,int y,int z);
    // Level::isSolidBlockingTile.
    bool isSolidBlockingTile(int x,int y,int z);
    bool isSolidBlockingTileInLoadedChunk(int x,int y,int z,bool valueIfNotLoaded);
    bool isTopSolidBlocking(int x,int y,int z);
    bool mayPlace(int tileId,int x,int y,int z,bool ignoreEntities,int face,shared_ptr<Entity> ignoreEntity);
    bool isUnobstructed(AABB*,shared_ptr<Entity>){return true;}
    bool getDirectSignal(int x,int y,int z,int dir);
    bool hasDirectSignal(int x,int y,int z);
    bool getSignal(int x,int y,int z,int dir);
    bool hasNeighborSignal(int x,int y,int z);

    // Level::getRawBrightness (with the slab/farmland/stair neighbour rule),
    // getDaytimeRawBrightness, getBrightness and canSeeSky.
    virtual int getRawBrightness(int x,int y,int z)=0;
    virtual int getDaytimeRawBrightness(int x,int y,int z)=0;
    virtual int getBrightness(LightLayer::variety layer,int x,int y,int z)=0;
    virtual bool canSeeSky(int x,int y,int z)=0;
    // Level::isRaining (rain level above 0.2), isRainingAt, isHumidAt.
    virtual bool isRaining(){return false;}
    virtual bool isRainingAt(int x,int y,int z)=0;
    virtual bool isHumidAt(int,int,int){return false;}
    virtual bool hasChunksAt(int x0,int y0,int z0,int x1,int y1,int z1)=0;

    // Scheduling, entities and effects.
    virtual void addToTickNextTick(int,int,int,int,int){}
    virtual bool newFallingTileAllowed(){return true;}
    virtual void addEntity(shared_ptr<FallingTile>){}
    void setTilesDirty(int,int,int,int,int,int){}
    void levelEvent(shared_ptr<Player>,int,int,int,int,int){}
    void playSound(float,float,float,int,float,float){}
    void addParticle(int,double,double,double,double,double,double){}
    // Level::getInstaTick: only world generation ticks instantly.
    bool getInstaTick(){return false;}
    // PlayerList::isTrackingTile and the Fire Spreads host option.
    virtual bool isTrackingTile(int,int,int){return true;}
    virtual bool fireSpreads(){return true;}

    // Tile::spawnResources (drops for tile and data at the position).
    virtual void spawnResources(int x,int y,int z,int tile,int data)=0;
    // Sapling::growTree's feature placement: the host runs the original tree
    // feature and returns Feature::place. The mushroom kinds are
    // HugeMushroomFeature(0) and (1) for Mushroom::growTree.
    enum class TreeKind { Tree,BigTree,Spruce,Birch,JungleTree,MegaJungle,BrownMushroom,RedMushroom };
    virtual bool placeTree(TreeKind kind,int height,Random& random,int x,int y,int z)=0;

    // The Level the extracted code is currently running against (for the
    // MinecraftServer and app stand-ins); set by the host while it runs them.
    static inline thread_local Level* current=nullptr;
};
using LevelSource=Level;
// Sets Level::current for a scope.
struct CurrentLevel {
    Level* previous;
    explicit CurrentLevel(Level* level):previous(Level::current){Level::current=level;}
    ~CurrentLevel(){Level::current=previous;}
};

// MinecraftServer::getInstance()->getPlayers() and app.GetGameHostOption.
class PlayerList {
public:
    bool isTrackingTile(int x,int y,int z,int){return Level::current?Level::current->isTrackingTile(x,y,z):true;}
};
class MinecraftServer {
public:
    static MinecraftServer* getInstance(){static MinecraftServer server;return &server;}
    PlayerList* getPlayers(){return &players;}
private:
    PlayerList players;
};
enum eGameHostOption { eGameHostOption_FireSpreads };
struct App {
    bool GetGameHostOption(eGameHostOption){return Level::current?Level::current->fireSpreads():true;}
};
inline App app;

class FireTile;
class TntTile;
class PortalTile;

// Tile: the registry and the base behaviour the methods rely on. Each
// registered id gets an instance of its class below (or Tile itself), with
// the properties of ported/TileProperties.cpp.
class Tile {
public:
#include "TileIds.inc"
    static Tile* tiles[256];
    static bool solid[256];
    static int lightBlock[256];
    static int lightEmission[256];
    static Tile *farmland,*sapling,*crops,*tallgrass,*flower,*rose,
        *water,*calmWater,*lava,*calmLava,*anvil;
    static FireTile* fire;
    static TntTile* tnt;
    static PortalTile* portalTile;

    int id=0;
    // The class has its original methods here (not the plain Tile stand-in).
    bool ported=false;
    Material* material=nullptr;
    bool ticking=false,cubeShaped=true,solidRender=true;
    SoundType* soundType=&defaultSound;
    static inline SoundType defaultSound;

    virtual ~Tile()=default;
    virtual void init(){}
    bool isTicking()const{return ticking;}
    virtual bool isCubeShaped(){return cubeShaped;}
    // The constructor-time render solidity (LeafTile answers true on the server).
    virtual bool isSolidRender(bool isServerLevel=false){(void)isServerLevel;return solidRender;}
    virtual AABB* getAABB(Level*,int,int,int){return nullptr;}
    virtual int getTickDelay(){return 10;}
    virtual void tick(Level*,int,int,int,Random*){}
    virtual bool shouldTileTick(Level*,int,int,int){return true;}
    virtual bool canSurvive(Level*,int,int,int){return true;}
    virtual bool mayPlace(Level* level,int x,int y,int z);
    virtual bool mayPlace(Level* level,int x,int y,int z,int face){(void)face;return mayPlace(level,x,y,z);}
    virtual void onPlace(Level*,int,int,int){}
    virtual void onRemove(Level*,int,int,int,int,int){}
    virtual void neighborChanged(Level*,int,int,int,int){}
    virtual void handleRain(Level*,int,int,int){}
    virtual int getPlacedOnFaceDataValue(Level*,int,int,int,int,float,float,float,int itemValue){return itemValue;}
    // Redstone (answered by the signal sources in a later batch).
    virtual bool isSignalSource(){return false;}
    virtual bool getSignal(LevelSource*,int,int,int,int){return false;}
    virtual bool getDirectSignal(Level*,int,int,int,int){return false;}
    // spawnResources(level, x, y, z, data, playerBonusLevel): the item drops
    // (ported SurvivalRules, including the leaf and cocoa overrides).
    void spawnResources(Level* level,int x,int y,int z,int data,int){level->spawnResources(x,y,z,id,data);}
};

class EntityTile:public Tile {};

class Bush:public Tile {
public:
    virtual bool mayPlaceOn(int tile);
    bool canSurvive(Level* level,int x,int y,int z)override;
    void tick(Level* level,int x,int y,int z,Random* random)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
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
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    bool isUnderCrops(Level* level,int x,int y,int z);
    bool isNearWater(Level* level,int x,int y,int z);
};
class ReedTile:public Tile {
public:
    void tick(Level* level,int x,int y,int z,Random* random)override;
    bool mayPlace(Level* level,int x,int y,int z)override;
    bool canSurvive(Level* level,int x,int y,int z)override;
    bool shouldTileTick(Level* level,int x,int y,int z)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    const void checkAlive(Level* level,int x,int y,int z);
};
class CactusTile:public Tile {
public:
    void tick(Level* level,int x,int y,int z,Random* random)override;
    bool canSurvive(Level* level,int x,int y,int z)override;
    bool shouldTileTick(Level* level,int x,int y,int z)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
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
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
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
    bool isSolidRender(bool isServerLevel)override{return isServerLevel?true:solidRender;}
    void tick(Level* level,int x,int y,int z,Random* random)override;
    void onRemove(Level* level,int x,int y,int z,int id,int data)override;
    bool shouldTileTick(Level* level,int x,int y,int z)override;
    void die(Level* level,int x,int y,int z);
};
class VineTile:public Tile {
public:
    void tick(Level* level,int x,int y,int z,Random* random)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    bool isAcceptableNeighbor(int id);
    bool updateSurvival(Level* level,int x,int y,int z);
};
class IceTile:public Tile {
public:
    void tick(Level* level,int x,int y,int z,Random* random)override;
    bool shouldTileTick(Level* level,int x,int y,int z)override;
};
class TopSnowTile:public Tile {
public:
    TILE_CONSTANTS_TopSnowTile
    void tick(Level* level,int x,int y,int z,Random* random)override;
    bool shouldTileTick(Level* level,int x,int y,int z)override;
    bool mayPlace(Level* level,int x,int y,int z)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    bool checkCanSurvive(Level* level,int x,int y,int z);
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
class WoolCarpetTile:public Tile {
public:
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    bool checkCanSurvive(Level* level,int x,int y,int z);
    bool canSurvive(Level* level,int x,int y,int z)override;
    bool mayPlace(Level* level,int x,int y,int z)override;
};
class CakeTile:public Tile {
public:
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    bool canSurvive(Level* level,int x,int y,int z)override;
    bool mayPlace(Level* level,int x,int y,int z)override;
};
class FlowerPotTile:public Tile {
public:
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
};
class SignTile:public EntityTile {
public:
    bool onGround=true;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
};
class LadderTile:public Tile {
public:
    bool mayPlace(Level* level,int x,int y,int z)override;
    int getPlacedOnFaceDataValue(Level* level,int x,int y,int z,int face,float clickX,float clickY,float clickZ,int itemValue)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
};
class TorchTile:public Tile {
public:
    bool isConnection(Level* level,int x,int y,int z);
    bool mayPlace(Level* level,int x,int y,int z)override;
    int getPlacedOnFaceDataValue(Level* level,int x,int y,int z,int face,float clickX,float clickY,float clickZ,int itemValue)override;
    void tick(Level* level,int x,int y,int z,Random* random)override;
    void onPlace(Level* level,int x,int y,int z)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    bool checkCanSurvive(Level* level,int x,int y,int z);
    bool shouldTileTick(Level* level,int x,int y,int z)override;
};
class DoorTile:public Tile {
public:
    TILE_CONSTANTS_DoorTile
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    void setOpen(Level* level,int x,int y,int z,bool shouldOpen);
    int getCompositeData(LevelSource* level,int x,int y,int z);
};
class HeavyTile:public Tile {
public:
    static bool instaFall;
    void onPlace(Level* level,int x,int y,int z)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    void tick(Level* level,int x,int y,int z,Random* random)override;
    void checkSlide(Level* level,int x,int y,int z);
    virtual void falling(shared_ptr<FallingTile> entity);
    int getTickDelay()override;
    static bool isFree(Level* level,int x,int y,int z);
    virtual void onLand(Level* level,int xt,int yt,int zt,int data);
};
class FireTile:public Tile {
public:
    TILE_CONSTANTS_FireTile
    int flameOdds[256]{},burnOdds[256]{};
    void init()override;
    void setFlammable(int id,int flame,int burn);
    int getTickDelay()override;
    void tick(Level* level,int x,int y,int z,Random* random)override;
    void checkBurnOut(Level* level,int x,int y,int z,int chance,Random* random,int age);
    bool isValidFireLocation(Level* level,int x,int y,int z);
    int getFireOdds(Level* level,int x,int y,int z);
    bool canBurn(LevelSource* level,int x,int y,int z);
    int getFlammability(Level* level,int x,int y,int z,int odds);
    bool mayPlace(Level* level,int x,int y,int z)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    void onPlace(Level* level,int x,int y,int z)override;
    bool isFlammable(int tile);
};
class TntTile:public Tile {
public:
    TILE_CONSTANTS_TntTile
    // TntTile::destroy with the explode bit primes TNT; explosions are not
    // ported yet, so the burnt TNT is simply gone.
    void destroy(Level*,int,int,int,int){}
};
class PortalTile:public Tile {
public:
    // Nether portals come with the dimension batch: fire on obsidian lights normally.
    bool trySpawnPortal(Level*,int,int,int,bool){return false;}
};
class LiquidTile:public Tile {
public:
    int getDepth(Level* level,int x,int y,int z);
    int getTickDelay()override;
    void onPlace(Level* level,int x,int y,int z)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    void updateLiquid(Level* level,int x,int y,int z);
    void fizz(Level* level,int x,int y,int z);
};
struct LiquidTickData {
    Level* level;int x,y,z;Random* random;
    LiquidTickData(Level* level,int x,int y,int z,Random* random):level(level),x(x),y(y),z(z),random(random){}
};
class LiquidTileDynamic:public LiquidTile {
public:
    int maxCount=0;
    bool result[4]{};
    int dist[4]{};
    bool m_iterativeInstatick=false;
    std::deque<LiquidTickData> m_tilesToTick;
    void setStatic(Level* level,int x,int y,int z);
    void iterativeTick(Level* level,int x,int y,int z,Random* random);
    void tick(Level* level,int x,int y,int z,Random* random)override;
    void mainTick(Level* level,int x,int y,int z,Random* random);
    void trySpreadTo(Level* level,int x,int y,int z,int neighbor);
    int getSlopeDistance(Level* level,int x,int y,int z,int pass,int from);
    bool* getSpread(Level* level,int x,int y,int z);
    bool isWaterBlocking(Level* level,int x,int y,int z);
    int getHighest(Level* level,int x,int y,int z,int current);
    bool canSpreadTo(Level* level,int x,int y,int z);
    void onPlace(Level* level,int x,int y,int z)override;
};
class LiquidTileStatic:public LiquidTile {
public:
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    void setDynamic(Level* level,int x,int y,int z);
    void tick(Level* level,int x,int y,int z,Random* random)override;
    bool isFlammable(Level* level,int x,int y,int z);
};
class StairTile:public Tile { public: TILE_CONSTANTS_StairTile };
class HalfSlabTile:public Tile { public: TILE_CONSTANTS_HalfSlabTile };

// Items whose useOn the port has from the source (hoes, seeds, bone meal,
// cocoa beans and flint and steel).
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
class FlintAndSteelItem:public Item {
public:
    bool useOn(shared_ptr<ItemInstance> instance,shared_ptr<Player> player,Level* level,int x,int y,int z,int face,float clickX,float clickY,float clickZ,bool bTestUseOnOnly=false);
};
class DyePowderItem:public Item {
public:
    static const int BROWN=3,WHITE=15;
    bool useOn(shared_ptr<ItemInstance> itemInstance,shared_ptr<Player> player,Level* level,int x,int y,int z,int face,float clickX,float clickY,float clickZ,bool bTestUseOnOnly=false);
};

// Builds the registry from ported/TileProperties.cpp (once).
void initializeTiles();
// Level::isTopSolidBlocking and FireTile::canBurn for one tile, for code
// that has no Level (the terrain mesh).
bool isTopSolidBlocking(int tile,int data);
bool fireCanBurn(int tile);
// Whether a scheduled tick of this tile runs original code here; the ticks of
// the other tiles stay in the saved chunk untouched.
bool tickPorted(int id);
// Item::useOn for the registered item id; false when the item has no
// ported useOn or does not apply. `face` is Facing (0 down ... 5 east).
bool useItemOn(Level& level,ItemInstance& item,int x,int y,int z,int face);
}
