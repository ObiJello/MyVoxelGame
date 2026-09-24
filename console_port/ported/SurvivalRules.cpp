// Survival block breaking and drops.
//
// Transferred from the supplied original sources: Tile::getDestroyProgress,
// Tile::playerDestroy/spawnResources, Player::getDestroySpeed/canDestroy,
// Inventory::getDestroySpeed/canDestroy, Item::Tier (Item.cpp), DiggerItem,
// PickaxeItem, ShovelItem, HatchetItem, WeaponItem, ShearsItem, HoeItem and the
// getResource/getResourceCount/getSpawnResourcesAuxValue overrides of the tile
// classes in original/reference-only.
//
// Also StoneTile, OreTile, RedStoneOreTile, GravelTile, ClayTile, LightGemTile,
// GlassTile, IceTile, BookshelfTile, MobSpawnerTile, WebTile, SnowTile,
// HugeMushroomTile, MycelTile, SignTile, CakeTile and WoodSlabTile from the full
// source, including popExperience amounts and the extra random draw in
// getResourceCountForLootBonus. The drop gate is ServerPlayerGameMode::destroyBlock.
#include "SurvivalRules.h"
#include "TileSurvival.h"
#include "Material.h"
#include "Random.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace console {
namespace {
// Item::Tier(level, uses, speed, damage, enchantmentValue) from Item.cpp.
struct Tier { int level, uses; float speed; };
constexpr Tier WOOD{0,59,2}, STONE{1,131,4}, IRON{2,250,6}, DIAMOND{3,1561,8}, GOLD{0,32,12};
enum class Kind { None, Shovel, Pickaxe, Hatchet, Sword, Hoe, Shears };
struct Tool { Kind kind=Kind::None; Tier tier{}; };
Tool tool(int id){
    // Item::staticCtor registrations (constructor id + 256).
    switch(id){
    case 256:return {Kind::Shovel,IRON};   case 257:return {Kind::Pickaxe,IRON};
    case 258:return {Kind::Hatchet,IRON};  case 267:return {Kind::Sword,IRON};
    case 268:return {Kind::Sword,WOOD};    case 269:return {Kind::Shovel,WOOD};
    case 270:return {Kind::Pickaxe,WOOD};  case 271:return {Kind::Hatchet,WOOD};
    case 272:return {Kind::Sword,STONE};   case 273:return {Kind::Shovel,STONE};
    case 274:return {Kind::Pickaxe,STONE}; case 275:return {Kind::Hatchet,STONE};
    case 276:return {Kind::Sword,DIAMOND}; case 277:return {Kind::Shovel,DIAMOND};
    case 278:return {Kind::Pickaxe,DIAMOND};case 279:return {Kind::Hatchet,DIAMOND};
    case 283:return {Kind::Sword,GOLD};    case 284:return {Kind::Shovel,GOLD};
    case 285:return {Kind::Pickaxe,GOLD};  case 286:return {Kind::Hatchet,GOLD};
    case 290:return {Kind::Hoe,WOOD};      case 291:return {Kind::Hoe,STONE};
    case 292:return {Kind::Hoe,IRON};      case 293:return {Kind::Hoe,DIAMOND};
    case 294:return {Kind::Hoe,GOLD};      case 359:return {Kind::Shears,{}};
    default:return {};
    }
}
bool listed(int tile,std::initializer_list<int> tiles){return std::find(tiles.begin(),tiles.end(),tile)!=tiles.end();}
// PickaxeItem/ShovelItem/HatchetItem::staticCtor diggables.
bool pickaxeDiggable(int t){return listed(t,{4,43,44,1,24,48,15,42,16,41,14,56,57,79,87,21,22,73,74,66,28,27});}
bool shovelDiggable(int t){return listed(t,{2,3,12,13,78,80,82,60,88,110});}
bool hatchetDiggable(int t){return listed(t,{5,47,17,54,43,44,86,91});}
const Material* material(SurvivalMaterial m){
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
    throw std::invalid_argument("Unknown survival material");
}
bool stonelike(SurvivalMaterial m){return m==SurvivalMaterial::stone || m==SurvivalMaterial::metal || m==SurvivalMaterial::heavyMetal;}
const SurvivalTile& registered(int tileId){
    const auto* tile=consoleSurvivalTile(tileId);
    if(!tile)throw std::invalid_argument("Unregistered survival tile");
    return *tile;
}
}

float consoleItemDestroySpeed(int itemId,int tileId){
    const auto held=tool(itemId);const auto& tile=registered(tileId);
    switch(held.kind){
    case Kind::Pickaxe:
        if(stonelike(tile.material))return held.tier.speed;
        return pickaxeDiggable(tileId)?held.tier.speed:1;
    case Kind::Shovel:return shovelDiggable(tileId)?held.tier.speed:1;
    case Kind::Hatchet:
        if(tile.material==SurvivalMaterial::wood)return held.tier.speed;
        return hatchetDiggable(tileId)?held.tier.speed:1;
    case Kind::Sword:return tileId==30?15.f:1.5f;
    case Kind::Shears:
        if(tileId==30 || tileId==18)return 15;
        if(tileId==35)return 5;
        return 1;
    default:return 1;
    }
}
bool consoleItemCanDestroySpecial(int itemId,int tileId){
    const auto held=tool(itemId);const auto& tile=registered(tileId);
    switch(held.kind){
    case Kind::Pickaxe:{
        const int level=held.tier.level;
        if(tileId==49)return level==3;
        if(tileId==57 || tileId==56 || tileId==133 || tileId==129 || tileId==41 || tileId==14)return level>=2;
        if(tileId==42 || tileId==15 || tileId==22 || tileId==21)return level>=1;
        if(tileId==73 || tileId==74)return level>=2;
        return stonelike(tile.material);
    }
    case Kind::Shovel:return tileId==78 || tileId==80;
    case Kind::Sword:return tileId==30;
    case Kind::Shears:return tileId==30 || tileId==55 || tileId==132;
    default:return false;
    }
}
bool consolePlayerCanDestroy(int tileId,int heldItemId){
    const auto& tile=registered(tileId);
    if(const_cast<Material*>(material(tile.material))->isAlwaysDestroyable())return true;
    return heldItemId>0 && consoleItemCanDestroySpecial(heldItemId,tileId);
}
float consolePlayerDestroySpeed(int tileId,int heldItemId,bool underWater,int digSpeed,int digSlow){
    float speed=1.0f;
    if(heldItemId>0)speed*=consoleItemDestroySpeed(heldItemId,tileId);
    // Efficiency and Aqua Affinity enchantments are not carried by held items yet.
    if(digSpeed>=0)speed*=1.0f+(digSpeed+1)*.2f;
    if(digSlow>=0)speed*=1.0f-(digSlow+1)*.2f;
    if(underWater)speed/=5;
    return speed;
}
float consoleDestroyProgress(int tileId,int heldItemId,bool underWater,int digSpeed,int digSlow){
    const auto* tile=consoleSurvivalTile(tileId);
    if(!tile)return 0;
    const float destroySpeed=tile->destroyTime;
    if(destroySpeed<0)return 0;
    // Java float division: 1/0 is +inf, which the caller treats as instant.
    if(!consolePlayerCanDestroy(tileId,heldItemId))return 1/destroySpeed/100.0f;
    return (consolePlayerDestroySpeed(tileId,heldItemId,underWater,digSpeed,digSlow)/destroySpeed)/30;
}

std::vector<SurvivalDrop> consoleTileDrops(int tileId,int data,int heldItemId,Random& random,int* experience){
    if(experience)*experience=0;
    // Mth::nextInt(random,min,max).
    const auto between=[&](int low,int high){return low>=high?low:random.nextInt(high-low+1)+low;};
    registered(tileId);
    if(data<0 || data>15)throw std::invalid_argument("Invalid tile data");
    std::vector<SurvivalDrop> drops;
    const auto pop=[&](int id,int damage=0){if(id>0)drops.push_back({id,1,damage});};
    const bool shears=heldItemId==359;
    // Tile::spawnResources with odds 1: one nextFloat per attempt, then getResource.
    const auto spawn=[&](int count,auto resource,int aux){
        for(int i=0;i<count;++i){
            if(random.nextFloat()>1.0f)continue;
            const int type=resource();
            if(type<=0)continue;
            pop(type,aux);
        }
    };
    const auto simple=[&](int id,int count=1,int aux=0){spawn(count,[id]{return id;},aux);};
    switch(tileId){
    // ---- Transferred overrides ----
    case 2:case 60:simple(3);break;                                    // GrassTile, FarmTile -> dirt
    case 18:{                                                          // LeafTile
        if(shears){pop(18,data&3);break;}
        const int chance=(data&3)==3?40:20;
        if(random.nextInt(chance)==0)pop(6,data&3);
        if((data&3)==0 && random.nextInt(200)==0)pop(260);
        break;
    }
    case 31:{                                                          // TallGrass
        if(shears){pop(31,data);break;}
        const int count=1+random.nextInt(1);
        spawn(count,[&]{return random.nextInt(8)==0?295:-1;},0);
        break;
    }
    case 32:if(shears)pop(32,data);else simple(-1);break;             // DeadBushTile
    case 106:if(shears)pop(106);break;                                 // VineTile count 0
    case 59:case 141:case 142:{                                        // CropTile, CarrotTile, PotatoTile
        const int seed=tileId==59?295:tileId==141?391:392;
        const int plant=tileId==59?296:seed;
        spawn(1,[&]{return data==7?plant:seed;},0);
        if(data>=7){
            for(int i=0;i<3;++i){if(random.nextInt(5*3)>data)continue;pop(seed);}
            if(tileId==142 && random.nextInt(50)==0)pop(394);
        }
        break;
    }
    case 104:case 105:{                                                // StemTile
        spawn(1,[]{return -1;},0);
        const int seed=tileId==104?361:362;
        for(int i=0;i<3;++i){if(random.nextInt(5*3)>data)continue;pop(seed);}
        break;
    }
    case 115:{                                                         // NetherStalkTile
        int count=1;
        if(data>=3)count=2+random.nextInt(3);
        for(int i=0;i<count;++i)pop(372);
        break;
    }
    case 127:{const int age=(data&12)>>2;for(int i=0;i<(age>=2?3:1);++i)pop(351,3);break;} // CocoaTile
    case 78:pop(332);break;                                            // TopSnowTile::playerDestroy
    case 43:case 44:simple(44,tileId==43?2:1,data&7);break;           // StoneSlabTile / HalfSlabTile
    case 64:case 71:if(!(data&8))simple(tileId==71?330:324);break;    // DoorTile
    case 26:if(!(data&8))simple(355);break;                            // BedTile head piece drops nothing
    case 61:case 62:simple(61);break;                                  // FurnaceTile
    case 103:{                                                         // MelonTile
        int total=3+random.nextInt(5);total+=random.nextInt(1);
        simple(360,std::min(total,9));
        break;
    }
    case 83:simple(338);break;                                         // ReedTile
    case 102:break;                                                    // ThinFenceTile(glass) dropsResources=false
    case 8:case 9:case 10:case 11:case 51:case 90:case 34:case 36:case 97:break; // count 0
    case 117:simple(379);break;case 118:simple(380);break;            // BrewingStandTile, CauldronTile
    case 130:simple(49,8);break;                                       // EnderChestTile
    case 93:case 94:simple(356);break;                                 // DiodeTile
    case 75:case 76:simple(76);break;                                  // NotGateTile
    case 55:simple(331);break;                                         // RedStoneDustTile
    case 123:case 124:simple(123);break;                               // RedlightTile
    case 132:simple(287);break;                                        // TripWireTile
    case 140:{                                                         // FlowerPotTile
        simple(390);
        switch(data){
        case 1:pop(38);break;case 2:pop(37);break;case 9:pop(81);break;
        case 8:pop(39);break;case 7:pop(40);break;case 10:pop(32);break;
        case 3:pop(6,0);break;case 5:pop(6,2);break;case 4:pop(6,1);break;
        case 6:pop(6,3);break;case 11:pop(31,2);break;
        }
        break;
    }
    case 144:break; // SkullTile drops from its tile entity in onRemove (not ported)
    case 17:case 6:simple(tileId,1,data&3);break;                     // TreeTile, Sapling aux
    case 5:case 24:case 35:case 98:case 139:case 171:simple(tileId,1,data);break; // aux = data
    case 155:simple(155,1,(data==3 || data==4)?2:data);break;         // QuartzBlockTile
    case 145:simple(145,1,data>>2);break;                              // AnvilTile
    case 1:simple(4);break;                                            // StoneTile -> cobblestone
    case 14:case 15:case 16:case 56:case 129:case 153:case 21:{      // OreTile
        const int item=tileId==16?263:tileId==56?264:tileId==21?351:tileId==129?388:tileId==153?406:tileId;
        const int count=tileId==21?4+random.nextInt(5):1;             // getResourceCount
        simple(item,count,tileId==21?4:0);                             // lapis aux DyePowderItem::BLUE
        if(item!=tileId){                                              // OreTile::spawnResources
            const int xp=tileId==16?between(0,2):tileId==56 || tileId==129?between(3,7):between(2,5);
            if(experience)*experience=xp;
        }
        break;
    }
    case 73:case 74:{                                                  // RedStoneOreTile
        int count=4+random.nextInt(2);count+=random.nextInt(1);      // getResourceCountForLootBonus(0)
        simple(331,count);
        const int xp=1+random.nextInt(5);
        if(experience)*experience=xp;
        break;
    }
    case 13:spawn(1,[&]{return random.nextInt(10)==0?318:13;},0);break; // GravelTile
    case 82:simple(337,4);break;                                       // ClayTile
    case 89:{                                                          // LightGemTile
        int count=2+random.nextInt(3);count+=random.nextInt(1);
        simple(348,std::clamp(count,1,4));
        break;
    }
    case 20:case 79:case 92:break;                                     // Glass/Ice/Cake count 0
    case 52:{const int a=random.nextInt(15);const int xp=15+a+random.nextInt(15);if(experience)*experience=xp;break;} // MobSpawnerTile
    case 47:simple(340,3);break;                                       // BookshelfTile
    case 30:simple(287);break;                                         // WebTile
    case 80:simple(332,4);break;                                       // SnowTile
    case 99:case 100:{                                                 // HugeMushroomTile
        const int count=std::max(0,random.nextInt(10)-7);
        simple(tileId==99?39:40,count);
        break;
    }
    case 110:simple(3);break;                                          // MycelTile -> dirt
    case 63:case 68:simple(323);break;                                 // SignTile
    case 125:case 126:simple(126,tileId==125?2:1,data&7);break;       // WoodSlabTile
    default:simple(tileId);break;                                      // Tile::getResource
    }
    return drops;
}
int consoleToolMineDamage(int itemId,int tileId){
    const auto held=tool(itemId);const auto& tile=registered(tileId);
    switch(held.kind){
    case Kind::Pickaxe:case Kind::Shovel:case Kind::Hatchet:return tile.destroyTime!=0?1:0;
    case Kind::Sword:return tile.destroyTime!=0?2:0;
    case Kind::Shears:return listed(tileId,{18,30,31,106,132})?1:0;
    default:return 0;
    }
}
int consoleToolAttackDamageCost(int itemId){
    switch(tool(itemId).kind){
    case Kind::Pickaxe:case Kind::Shovel:case Kind::Hatchet:return 2;
    case Kind::Sword:return 1;
    default:return 0;
    }
}
int consoleItemMaxDamage(int itemId){
    const auto held=tool(itemId);
    if(held.kind==Kind::Shears)return 238;
    return held.kind==Kind::None?0:held.tier.uses;
}
float consoleMineExhaustion(){return .025f;}
const SurvivalFood* consoleFood(int itemId){
    // FoodConstants::FOOD_SATURATION_* values.
    constexpr float POOR=.1f,LOW=.3f,NORMAL=.6f,GOOD=.8f,SUPERNATURAL=1.2f;
    static const std::pair<int,SurvivalFood> foods[]{
        {260,{4,LOW}},                                     // apple
        {282,{6,NORMAL,false,0,0,0,0,281}},                 // mushroomStew -> bowl
        {297,{5,NORMAL}},                                  // bread
        {319,{3,LOW}},{320,{8,GOOD}},                      // porkChop raw/cooked
        {322,{4,SUPERNATURAL,true,10,5,0,1.0f}},           // apple_gold: regeneration
        {349,{2,LOW}},{350,{5,NORMAL}},                    // fish raw/cooked
        {357,{2,POOR}},                                    // cookie
        {360,{2,LOW}},                                     // melon
        {363,{3,LOW}},{364,{8,GOOD}},                      // beef raw/cooked
        {365,{2,LOW,false,17,30,0,.3f}},{366,{6,NORMAL}},  // chicken raw (hunger)/cooked
        {367,{4,POOR,false,17,30,0,.8f}},                  // rotten_flesh (hunger)
        {375,{2,GOOD,false,19,5,0,1.0f}},                  // spiderEye (poison)
        {391,{4,NORMAL}},{392,{1,LOW}},{393,{6,NORMAL}},   // carrots, potato, potatoBaked
        {394,{2,LOW,false,19,5,0,.6f}},                    // potatoPoisonous (poison)
        {396,{6,SUPERNATURAL}},                            // carrotGolden
        {400,{8,LOW}},                                     // pumpkinPie
    };
    for(const auto& [id,food]:foods)if(id==itemId)return &food;
    return nullptr;
}
}
