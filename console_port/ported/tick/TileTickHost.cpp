#include "TileTickHost.h"
#include "TileProperties.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>

namespace console::sim {
Tile* Tile::tiles[256];
bool Tile::solid[256];
int Tile::lightBlock[256];
int Tile::lightEmission[256];
Tile *Tile::farmland,*Tile::sapling,*Tile::crops,*Tile::tallgrass,*Tile::flower,*Tile::rose;
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
            tile.id=id;
            tile.material=materialOf(properties->material);
            tile.ticking=properties->ticking;
            tile.cubeShaped=properties->cubeShaped;
            Tile::tiles[id]=&tile;
            Tile::solid[id]=properties->solid;
            Tile::lightBlock[id]=properties->lightBlock;
            Tile::lightEmission[id]=properties->lightEmission;
        }
        Tile::farmland=Tile::tiles[Tile::farmland_Id];Tile::sapling=Tile::tiles[Tile::sapling_Id];
        Tile::crops=Tile::tiles[Tile::crops_Id];Tile::tallgrass=Tile::tiles[Tile::tallgrass_Id];
        Tile::flower=Tile::tiles[Tile::flower_Id];Tile::rose=Tile::tiles[Tile::rose_Id];
        // StemTile(id, fruit): Tile::pumpkinStem and Tile::melonStem.
        static_cast<StemTile*>(Tile::tiles[Tile::pumpkinStem_Id])->fruit=Tile::tiles[Tile::pumpkin_Id];
        static_cast<StemTile*>(Tile::tiles[Tile::melonStem_Id])->fruit=Tile::tiles[Tile::melon_Id];
    });
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
