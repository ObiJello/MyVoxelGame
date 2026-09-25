#include "TileTickHost.h"
#include "TileProperties.h"

#include <cstring>
#include <typeinfo>
#include <memory>
#include <mutex>
#include <string_view>

namespace console::sim {
Tile* Tile::tiles[256];
bool Tile::solid[256];
int Tile::lightBlock[256];
int Tile::lightEmission[256];
Tile *Tile::farmland,*Tile::sapling,*Tile::crops,*Tile::tallgrass,*Tile::flower,*Tile::rose,
    *Tile::water,*Tile::calmWater,*Tile::lava,*Tile::calmLava,*Tile::anvil;
FireTile* Tile::fire;
TntTile* Tile::tnt;
PortalTile* Tile::portalTile;
Tile *Tile::lightGem,*Tile::wood,*Tile::rock,*Tile::stoneSlab,*Tile::redStoneDust,*Tile::notGate_on,*Tile::notGate_off;
NotGateTile::ToggleMap NotGateTile::recentToggles;
PistonMovingPiece* Tile::pistonMovingPiece;
DWORD PistonBaseTile::tlsIdx = TlsAlloc();
bool HeavyTile::instaFall=false;
Random* Item::random=new Random();

namespace {
Material* materialOf(SurvivalMaterial m){
    Material::staticCtor();
    switch(m){
    case SurvivalMaterial::air:return Material::air;case SurvivalMaterial::grass:return Material::grass;
    case SurvivalMaterial::dirt:return Material::dirt;case SurvivalMaterial::wood:return Material::wood;
    case SurvivalMaterial::stone:return Material::stone;case SurvivalMaterial::metal:return Material::metal;
    case SurvivalMaterial::heavyMetal:return Material::heavyMetal;case SurvivalMaterial::water:return Material::water;
    case SurvivalMaterial::lava:return Material::lava;case SurvivalMaterial::leaves:return Material::leaves;
    case SurvivalMaterial::plant:return Material::plant;case SurvivalMaterial::replaceable_plant:return Material::replaceable_plant;
    case SurvivalMaterial::sponge:return Material::sponge;case SurvivalMaterial::cloth:return Material::cloth;
    case SurvivalMaterial::fire:return Material::fire;case SurvivalMaterial::sand:return Material::sand;
    case SurvivalMaterial::decoration:return Material::decoration;case SurvivalMaterial::clothDecoration:return Material::clothDecoration;
    case SurvivalMaterial::glass:return Material::glass;case SurvivalMaterial::buildable_glass:return Material::buildable_glass;
    case SurvivalMaterial::explosive:return Material::explosive;case SurvivalMaterial::coral:return Material::coral;
    case SurvivalMaterial::ice:return Material::ice;case SurvivalMaterial::topSnow:return Material::topSnow;
    case SurvivalMaterial::snow:return Material::snow;case SurvivalMaterial::cactus:return Material::cactus;
    case SurvivalMaterial::clay:return Material::clay;case SurvivalMaterial::vegetable:return Material::vegetable;
    case SurvivalMaterial::egg:return Material::egg;case SurvivalMaterial::portal:return Material::portal;
    case SurvivalMaterial::cake:return Material::cake;case SurvivalMaterial::web:return Material::web;
    case SurvivalMaterial::piston:return Material::piston;
    }
    return Material::stone;
}

std::unique_ptr<Tile> make(std::string_view cls){
    if(cls=="Bush")return std::make_unique<Bush>();
    if(cls=="TallGrass")return std::make_unique<TallGrass>();
    if(cls=="DeadBushTile")return std::make_unique<DeadBushTile>();
    if(cls=="WaterlilyTile")return std::make_unique<WaterlilyTile>();
    if(cls=="Sapling")return std::make_unique<Sapling>();
    if(cls=="CropTile")return std::make_unique<CropTile>();
    if(cls=="CarrotTile")return std::make_unique<CarrotTile>();
    if(cls=="PotatoTile")return std::make_unique<PotatoTile>();
    if(cls=="StemTile")return std::make_unique<StemTile>();
    if(cls=="Mushroom")return std::make_unique<Mushroom>();
    if(cls=="NetherStalkTile")return std::make_unique<NetherStalkTile>();
    if(cls=="GrassTile")return std::make_unique<GrassTile>();
    if(cls=="MycelTile")return std::make_unique<MycelTile>();
    if(cls=="FarmTile")return std::make_unique<FarmTile>();
    if(cls=="ReedTile")return std::make_unique<ReedTile>();
    if(cls=="CactusTile")return std::make_unique<CactusTile>();
    if(cls=="CocoaTile")return std::make_unique<CocoaTile>();
    if(cls=="TreeTile")return std::make_unique<TreeTile>();
    if(cls=="LeafTile")return std::make_unique<LeafTile>();
    if(cls=="VineTile")return std::make_unique<VineTile>();
    if(cls=="IceTile")return std::make_unique<IceTile>();
    if(cls=="TopSnowTile")return std::make_unique<TopSnowTile>();
    if(cls=="SnowTile")return std::make_unique<SnowTile>();
    if(cls=="RedStoneOreTile")return std::make_unique<RedStoneOreTile>();
    if(cls=="CauldronTile")return std::make_unique<CauldronTile>();
    if(cls=="WoolCarpetTile")return std::make_unique<WoolCarpetTile>();
    if(cls=="CakeTile")return std::make_unique<CakeTile>();
    if(cls=="FlowerPotTile")return std::make_unique<FlowerPotTile>();
    if(cls=="SignTile")return std::make_unique<SignTile>();
    if(cls=="LadderTile")return std::make_unique<LadderTile>();
    if(cls=="TorchTile")return std::make_unique<TorchTile>();
    if(cls=="DoorTile")return std::make_unique<DoorTile>();
    if(cls=="HeavyTile" || cls=="GravelTile" || cls=="AnvilTile")return std::make_unique<HeavyTile>();
    if(cls=="FireTile")return std::make_unique<FireTile>();
    if(cls=="TntTile")return std::make_unique<TntTile>();
    if(cls=="PortalTile")return std::make_unique<PortalTile>();
    if(cls=="LiquidTileDynamic")return std::make_unique<LiquidTileDynamic>();
    if(cls=="LiquidTileStatic")return std::make_unique<LiquidTileStatic>();
    if(cls=="StairTile")return std::make_unique<StairTile>();
    if(cls=="FenceTile")return std::make_unique<FenceTile>();
    if(cls=="RedStoneDustTile")return std::make_unique<RedStoneDustTile>();
    if(cls=="NotGateTile")return std::make_unique<NotGateTile>();
    if(cls=="LeverTile")return std::make_unique<LeverTile>();
    if(cls=="ButtonTile")return std::make_unique<ButtonTile>();
    if(cls=="PressurePlateTile")return std::make_unique<PressurePlateTile>();
    if(cls=="DiodeTile")return std::make_unique<DiodeTile>();
    if(cls=="RedlightTile")return std::make_unique<RedlightTile>();
    if(cls=="TrapDoorTile")return std::make_unique<TrapDoorTile>();
    if(cls=="FenceGateTile")return std::make_unique<FenceGateTile>();
    if(cls=="PistonBaseTile")return std::make_unique<PistonBaseTile>();
    if(cls=="PistonExtensionTile")return std::make_unique<PistonExtensionTile>();
    if(cls=="PistonMovingPiece")return std::make_unique<PistonMovingPiece>();
    if(cls=="BedTile")return std::make_unique<BedTile>();
    if(cls=="RailTile" || cls=="DetectorRailTile")return std::make_unique<RailTile>();
    if(cls=="StoneSlabTile" || cls=="WoodSlabTile")return std::make_unique<HalfSlabTile>();
    return std::make_unique<Tile>();
}
}

void initializeTiles(){
    static std::once_flag once;
    std::call_once(once,[]{
        static std::unique_ptr<Tile> owned[256];
        for(int id=0;id<256;++id){
            const auto* properties=consoleTileProperties(id);
            if(!properties)continue;
            owned[id]=make(properties->className);
            Tile& tile=*owned[id];
            tile.ported=typeid(tile)!=typeid(Tile);
            // The EntityTile classes (pistons may not push them).
            for(const char* entityClass:{"BrewingStandTile","ChestTile","DispenserTile","EnchantmentTableTile",
                "EnderChestTile","FurnaceTile","MobSpawnerTile","MusicTile","PistonMovingPiece","RecordPlayerTile",
                "SignTile","SkullTile","TheEndPortal"})
                if(properties->className==std::string_view(entityClass))tile.entityTile=true;
            tile.id=id;
            tile.material=materialOf(properties->material);
            tile.ticking=properties->ticking;
            tile.cubeShaped=properties->cubeShaped;
            tile.solidRender=properties->solid;
            tile.destroyTime=properties->destroyTime;
            tile.explosionResistance=properties->explosionResistance;
            Tile::tiles[id]=&tile;
            Tile::solid[id]=properties->solid;
            Tile::lightBlock[id]=properties->lightBlock;
            Tile::lightEmission[id]=properties->lightEmission;
        }
        Tile::farmland=Tile::tiles[Tile::farmland_Id];Tile::sapling=Tile::tiles[Tile::sapling_Id];
        Tile::crops=Tile::tiles[Tile::crops_Id];Tile::tallgrass=Tile::tiles[Tile::tallgrass_Id];
        Tile::flower=Tile::tiles[Tile::flower_Id];Tile::rose=Tile::tiles[Tile::rose_Id];
        Tile::water=Tile::tiles[Tile::water_Id];Tile::calmWater=Tile::tiles[Tile::calmWater_Id];
        Tile::lava=Tile::tiles[Tile::lava_Id];Tile::calmLava=Tile::tiles[Tile::calmLava_Id];
        Tile::anvil=Tile::tiles[Tile::anvil_Id];
        Tile::fire=static_cast<FireTile*>(Tile::tiles[Tile::fire_Id]);
        Tile::tnt=static_cast<TntTile*>(Tile::tiles[Tile::tnt_Id]);
        Tile::portalTile=static_cast<PortalTile*>(Tile::tiles[Tile::portalTile_Id]);
        Tile::lightGem=Tile::tiles[Tile::lightGem_Id];Tile::wood=Tile::tiles[Tile::wood_Id];
        Tile::rock=Tile::tiles[Tile::rock_Id];Tile::stoneSlab=Tile::tiles[Tile::stoneSlabHalf_Id];
        Tile::redStoneDust=Tile::tiles[Tile::redStoneDust_Id];
        Tile::notGate_on=Tile::tiles[Tile::notGate_on_Id];Tile::notGate_off=Tile::tiles[Tile::notGate_off_Id];
        static_cast<PistonBaseTile*>(Tile::tiles[Tile::pistonStickyBase_Id])->isSticky=true;
        Tile::pistonMovingPiece=static_cast<PistonMovingPiece*>(Tile::tiles[Tile::pistonMovingPiece_Id]);
        // The constructor arguments of Tile::staticCtor's redstone tiles.
        static_cast<NotGateTile*>(Tile::notGate_on)->on=true;
        static_cast<DiodeTile*>(Tile::tiles[Tile::diode_on_Id])->on=true;
        static_cast<RedlightTile*>(Tile::tiles[Tile::redstoneLight_lit_Id])->isLit=true;
        static_cast<ButtonTile*>(Tile::tiles[Tile::button_wood_Id])->sensitive=true;
        static_cast<PressurePlateTile*>(Tile::tiles[Tile::pressurePlate_stone_Id])->sensitivity=PressurePlateTile::mobs;
        static_cast<PressurePlateTile*>(Tile::tiles[Tile::pressurePlate_wood_Id])->sensitivity=PressurePlateTile::everything;
        // SignTile(id, clas, onGround): the wall sign is not on the ground.
        static_cast<SignTile*>(Tile::tiles[Tile::wallSign_Id])->onGround=false;
        // StemTile(id, fruit): Tile::pumpkinStem and Tile::melonStem.
        static_cast<StemTile*>(Tile::tiles[Tile::pumpkinStem_Id])->fruit=Tile::tiles[Tile::pumpkin_Id];
        static_cast<StemTile*>(Tile::tiles[Tile::melonStem_Id])->fruit=Tile::tiles[Tile::melon_Id];
        // Tile::staticCtor's closing loop runs every tile's init (FireTile's flammability).
        for(auto* tile:Tile::tiles)if(tile)tile->init();
    });
}

bool tickPorted(int id){
    initializeTiles();
    return id>0 && id<256 && Tile::tiles[id] && Tile::tiles[id]->ported;
}

namespace {
// A level holding one tile at every position.
struct CellLevel final:Level {
    int tile=0,data=0;
    int getTile(int,int,int)override{return tile;}
    int getData(int,int,int)override{return data;}
    bool setTileAndDataNoUpdate(int,int,int,int,int)override{return false;}
    bool setDataNoUpdate(int,int,int,int)override{return false;}
    bool hasChunk(int,int)override{return true;}
    int getRawBrightness(int,int,int)override{return 0;}
    int getDaytimeRawBrightness(int,int,int)override{return 0;}
    int getBrightness(LightLayer::variety,int,int,int)override{return 0;}
    bool canSeeSky(int,int,int)override{return false;}
    bool isRainingAt(int,int,int)override{return false;}
    bool hasChunksAt(int,int,int,int,int,int)override{return true;}
    void spawnResources(int,int,int,int,int,float)override{}
    bool placeTree(TreeKind,int,Random&,int,int,int)override{return false;}
};
}
// A read-only level over a world's tiles and data.
struct ReadLevel final:Level {
    const std::function<int(int,int,int)>& tile;
    const std::function<int(int,int,int)>& data;
    ReadLevel(const std::function<int(int,int,int)>& tile,const std::function<int(int,int,int)>& data):tile(tile),data(data){}
    int getTile(int x,int y,int z)override{return tile(x,y,z);}
    int getData(int x,int y,int z)override{return data(x,y,z);}
    bool setTileAndDataNoUpdate(int,int,int,int,int)override{return false;}
    bool setDataNoUpdate(int,int,int,int)override{return false;}
    bool hasChunk(int,int)override{return true;}
    int getRawBrightness(int,int,int)override{return 0;}
    int getDaytimeRawBrightness(int,int,int)override{return 0;}
    int getBrightness(LightLayer::variety,int,int,int)override{return 0;}
    bool canSeeSky(int,int,int)override{return false;}
    bool isRainingAt(int,int,int)override{return false;}
    bool hasChunksAt(int,int,int,int,int,int)override{return true;}
    void spawnResources(int,int,int,int,int,float)override{}
    bool placeTree(TreeKind,int,Random&,int,int,int)override{return false;}
};
bool isSolidBlockingTile(int tile){
    initializeTiles();
    CellLevel level;level.tile=tile&255;
    return level.isSolidBlockingTile(0,0,0);
}
bool dustShouldConnectTo(const std::function<int(int,int,int)>& tile,const std::function<int(int,int,int)>& data,
                         int x,int y,int z,int direction){
    initializeTiles();
    ReadLevel level(tile,data);
    return RedStoneDustTile::shouldConnectTo(&level,x,y,z,direction);
}
std::array<float,6> tileShape(int tile,int data){
    initializeTiles();
    auto* storage=Tile::shapeStorage();
    *storage=Tile::ThreadStorage{};
    switch(tile){
    // DiodeTile::updateDefaultShape.
    case Tile::diode_off_Id:case Tile::diode_on_Id:return {0,0,0,1,2.0f/16.0f,1};
    case Tile::lever_Id:case Tile::button_stone_Id:case Tile::button_wood_Id:
    case Tile::pressurePlate_stone_Id:case Tile::pressurePlate_wood_Id:case Tile::trapdoor_Id:{
        CellLevel level;level.tile=tile;level.data=data;
        Tile::tiles[tile]->updateShape(&level,0,0,0);
        break;
    }
    default:break;
    }
    return {float(storage->xx0),float(storage->yy0),float(storage->zz0),float(storage->xx1),float(storage->yy1),float(storage->zz1)};
}

std::vector<std::array<float,6>> tileCollisionBoxes(int tile,int data){
    initializeTiles();
    CellLevel level;level.tile=tile&255;level.data=data;
    std::vector<AABB*> boxes;
    *Tile::shapeStorage()=Tile::ThreadStorage{};
    if(auto* t=Tile::tiles[tile&255])t->addAABBs(&level,0,0,0,nullptr,&boxes,nullptr);
    std::vector<std::array<float,6>> result;
    for(auto* b:boxes)result.push_back({float(b->x0),float(b->y0),float(b->z0),float(b->x1),float(b->y1),float(b->z1)});
    return result;
}

bool isTopSolidBlocking(int tile,int data){
    initializeTiles();
    CellLevel level;level.tile=tile&255;level.data=data;
    return level.isTopSolidBlocking(0,0,0);
}
bool fireCanBurn(int tile){
    initializeTiles();
    CellLevel level;level.tile=tile&255;
    return Tile::fire->canBurn(&level,0,0,0);
}

LevelChunk* ChunkSource::getChunk(int chunkX,int chunkZ){
    return level->hasChunk(chunkX,chunkZ)?&loaded:&missing;
}

Material* Level::getMaterial(int x,int y,int z){
    const int id=getTile(x,y,z);
    if(id==0 || !Tile::tiles[id])return Material::air;
    return Tile::tiles[id]->material;
}

// Level::isSolidBlockingTile -> Tile::isSolidBlockingTile.
bool Level::isSolidBlockingTile(int x,int y,int z){
    Tile* tile=Tile::tiles[getTile(x,y,z)];
    if(tile==nullptr)return false;
    return tile->material->isSolidBlocking() && tile->isCubeShaped();
}

// Mushroom::growTree (HugeMushroomFeature through the host).
bool Mushroom::growTree(Level* level,int x,int y,int z,Random* random){
    int data = level->getData(x, y, z);

    level->setTileNoUpdate(x, y, z, 0);
    bool haveFeature=false;
    Level::TreeKind kind=Level::TreeKind::BrownMushroom;

    if (id == Tile::mushroom1_Id)
    {
        kind=Level::TreeKind::BrownMushroom;haveFeature=true;
    }
    else if (id == Tile::mushroom2_Id)
    {
        kind=Level::TreeKind::RedMushroom;haveFeature=true;
    }

    if (!haveFeature || !level->placeTree(kind, 0, *random, x, y, z))
    {
        level->setTileAndDataNoUpdate(x, y, z, this->id, data);
        return false;
    }
    return true;
}

// Item::staticCtor registrations of the items above (ids with the 256 shift).
bool useItemOn(Level& level,ItemInstance& item,int x,int y,int z,int face){
    initializeTiles();
    static HoeItem hoe;
    static SeedItem wheatSeeds=[]{SeedItem i;i.id=295;i.resultId=Tile::crops_Id;i.targetLand=Tile::farmland_Id;return i;}();
    static SeedItem pumpkinSeeds=[]{SeedItem i;i.id=361;i.resultId=Tile::pumpkinStem_Id;i.targetLand=Tile::farmland_Id;return i;}();
    static SeedItem melonSeeds=[]{SeedItem i;i.id=362;i.resultId=Tile::melonStem_Id;i.targetLand=Tile::farmland_Id;return i;}();
    static SeedItem netherWart=[]{SeedItem i;i.id=372;i.resultId=Tile::netherStalk_Id;i.targetLand=Tile::hellSand_Id;return i;}();
    static SeedFoodItem carrot=[]{SeedFoodItem i;i.id=391;i.resultId=Tile::carrots_Id;i.targetLand=Tile::farmland_Id;return i;}();
    static SeedFoodItem potato=[]{SeedFoodItem i;i.id=392;i.resultId=Tile::potatoes_Id;i.targetLand=Tile::farmland_Id;return i;}();
    static DyePowderItem dye;
    static FlintAndSteelItem flintAndSteel;
    auto instance=std::make_shared<ItemInstance>(item);
    auto player=std::make_shared<Player>();
    bool used=false;
    switch(item.id){
    case 290:case 291:case 292:case 293:case 294:used=hoe.useOn(instance,player,&level,x,y,z,face,0,0,0);break;
    case 295:used=wheatSeeds.useOn(instance,player,&level,x,y,z,face,0,0,0);break;
    case 361:used=pumpkinSeeds.useOn(instance,player,&level,x,y,z,face,0,0,0);break;
    case 362:used=melonSeeds.useOn(instance,player,&level,x,y,z,face,0,0,0);break;
    case 372:used=netherWart.useOn(instance,player,&level,x,y,z,face,0,0,0);break;
    case 391:used=carrot.useOn(instance,player,&level,x,y,z,face,0,0,0);break;
    case 392:used=potato.useOn(instance,player,&level,x,y,z,face,0,0,0);break;
    case 351:used=dye.useOn(instance,player,&level,x,y,z,face,0,0,0);break;
    case 259:used=flintAndSteel.useOn(instance,player,&level,x,y,z,face,0,0,0);break;
    default:return false;
    }
    item=*instance;
    return used;
}

// Sapling::growTree. The original constructs the tree feature with doUpdate
// true; the host places it through the same feature code.
void Sapling::growTree(Level* level,int x,int y,int z,Random* random){
    int data = level->getData(x, y, z) & TYPE_MASK;

    Level::TreeKind kind=Level::TreeKind::Tree;
    int height=0;
    bool haveFeature=false;

    int ox = 0, oz = 0;
    bool multiblock = false;

    if (data == TYPE_EVERGREEN)
    {
        kind=Level::TreeKind::Spruce;haveFeature=true;
    }
    else if (data == TYPE_BIRCH)
    {
        kind=Level::TreeKind::Birch;haveFeature=true;
    }
    else if (data == TYPE_JUNGLE)
    {
        // check for mega tree
        for (ox = 0; ox >= -1; ox--)
        {
            for (oz = 0; oz >= -1; oz--)
            {
                if (isSapling(level, x + ox, y, z + oz, TYPE_JUNGLE) &&
                    isSapling(level, x + ox + 1, y, z + oz, TYPE_JUNGLE) &&
                    isSapling(level, x + ox, y, z + oz + 1, TYPE_JUNGLE) &&
                    isSapling(level, x + ox + 1, y, z + oz + 1, TYPE_JUNGLE))
                {
                    kind=Level::TreeKind::MegaJungle;height=10 + random->nextInt(20);haveFeature=true;
                    multiblock = true;
                    break;
                }
            }
            if (haveFeature)
            {
                break;
            }
        }
        if (!haveFeature)
        {
            ox = oz = 0;
            kind=Level::TreeKind::JungleTree;height=4 + random->nextInt(7);haveFeature=true;
        }
    }
    else
    {
        kind=Level::TreeKind::Tree;
        if (random->nextInt(10) == 0)
        {
            kind=Level::TreeKind::BigTree;
        }
    }
    if (multiblock)
    {
        level->setTileNoUpdate(x + ox, y, z + oz, 0);
        level->setTileNoUpdate(x + ox + 1, y, z + oz, 0);
        level->setTileNoUpdate(x + ox, y, z + oz + 1, 0);
        level->setTileNoUpdate(x + ox + 1, y, z + oz + 1, 0);
    }
    else
    {
        level->setTileNoUpdate(x, y, z, 0);
    }
    if (!level->placeTree(kind, height, *random, x + ox, y, z + oz))
    {
        if (multiblock)
        {
            level->setTileAndDataNoUpdate(x + ox, y, z + oz, this->id, data);
            level->setTileAndDataNoUpdate(x + ox + 1, y, z + oz, this->id, data);
            level->setTileAndDataNoUpdate(x + ox, y, z + oz + 1, this->id, data);
            level->setTileAndDataNoUpdate(x + ox + 1, y, z + oz + 1, this->id, data);
        }
        else
        {
            level->setTileAndDataNoUpdate(x, y, z, this->id, data);
        }
    }
}
}
