// Survival play on the host World: player block destruction and drops,
// ItemEntity physics/pickup, environmental damage, death, eating and crafting.
//
// Sources: Tile::playerDestroy/spawnResources/popResource, ChestTile::onRemove,
// Player::drop/die/canEat/causeFallDamage/hurt, Mob::baseTick/hurt/
// causeFallDamage/outOfWorld, Inventory::add/dropAll, ItemInstance::hurt,
// FoodItem/BowlFoodItem/GoldenAppleItem::useTimeDepleted.
// ItemEntity::tick/merge/playerTouch (gravity .04, drag .98, ground friction
// .6*.98, bounce -.5, five-minute lifetime, lava fling, stack merging) and
// Player::aiStep's pickup box bb.grow(1,0,1); Entity::baseTick/lavaHurt/
// setOnFire/move (lava: 4 damage and 15 s of fire; burning: 1 damage every
// 20 ticks; water extinguishes; fire contact burns 1 and lights for 8 s).
#include "WorldState.h"
#include "SurvivalRules.h"
#include "TileSurvival.h"
#include "CraftingRecipes.h"
#include "LevelSettings.h"
#include "FoodAccess.h"
#include "BlockShape.h"
#include "ContainerItems.h"
#include <algorithm>
#include <cmath>

namespace console {
namespace {
constexpr double pi=3.14159265358979323846;
std::unique_ptr<CompoundTag> stackTag(int id,int count,int damage){
    auto tag=std::make_unique<CompoundTag>();
    tag->putShort(L"id",id);tag->putByte(L"Count",count);tag->putShort(L"Damage",damage);
    return tag;
}
TagList* carriedList(CompoundTag& inventory){
    if(!inventory.contains(L"Items"))inventory.put(L"Items",new TagList());
    auto* list=dynamic_cast<TagList*>(inventory.get(L"Items"));
    if(!list)throw IoError("Invalid carried item list");
    return list;
}
CompoundTag* carriedAt(CompoundTag& inventory,int slot){
    auto* list=carriedList(inventory);
    for(int i=0;i<list->size();++i)if(auto* item=dynamic_cast<CompoundTag*>(list->get(i)))
        if(static_cast<unsigned char>(item->getByte(L"Slot"))==slot)return item;
    return nullptr;
}
void removeCarried(CompoundTag& inventory,int slot){
    auto replacement=std::make_unique<TagList>();
    auto* list=carriedList(inventory);
    for(int i=0;i<list->size();++i){
        auto* item=dynamic_cast<CompoundTag*>(list->get(i));
        if(!item || static_cast<unsigned char>(item->getByte(L"Slot"))==slot)continue;
        std::unique_ptr<Tag> copy(item->copy());replacement->add(copy.get());copy.release();
    }
    inventory.put(L"Items",replacement.get());replacement.release();
}
int effectAmplifier(const std::vector<PotionEffect>& effects,int id){
    for(const auto& effect:effects)if(effect.id==id && effect.duration>0)return effect.amplifier;
    return -1;
}
// Level::isSolidBlockingTile: opaque full cubes. Glass, leaves, ice,
// glowstone and spawners are cubes but not solid-rendering.
bool fullCube(int id){
    if(id==18 || id==20 || id==52 || id==79 || id==89)return false;
    return solid(static_cast<Block>(id)) && !consoleIsPartialBlock(id);
}
}

bool World::survival()const{return state->metadata->getGameType()==GameType::SURVIVAL;}
void World::setSurvival(bool enabled){
    GameType::staticCtor();
    state->metadata->setGameType(enabled?GameType::SURVIVAL:GameType::CREATIVE);
}
const std::vector<DroppedItem>& World::droppedItems()const{return state->droppedItems;}
std::optional<Vec3> World::savedPlayerPosition()const{return state->savedPlayerPosition;}
int World::playerAir()const{return state->playerAir;}
int World::playerInvulnerableTicks()const{return state->playerHurt.invulnerableTicks;}
int World::playerLastHealth()const{return state->playerHurt.lastHealth;}
int World::playerFireTicks()const{return state->playerFire;}
bool World::playerDead()const{return state->playerHurt.health<=0;}

float World::destroyProgress(int x,int y,int z,int slot)const{
    if(!inside(x,y,z))return 0;
    const int id=get(x,y,z);
    if(id==Air)return 0;
    if(!survival())return 1; // Creative destroys instantly.
    const int held=slot>=0 && slot<9?carriedItems()[slot].id:0;
    return consoleDestroyProgress(id,held,playerEyeInWater(),
        effectAmplifier(state->playerEffects,3),effectAmplifier(state->playerEffects,4));
}
void World::spawnDroppedItem(Vec3 position,Vec3 velocity,std::unique_ptr<CompoundTag> stack,int throwTime){
    DroppedItem item;
    item.position=position;item.velocity=velocity;item.throwTime=throwTime;
    item.id=stack->getShort(L"id");item.count=static_cast<unsigned char>(stack->getByte(L"Count"));
    item.damage=stack->getShort(L"Damage");
    if(item.id<=0 || item.count<=0)return;
    item.bobOffset=float(state->survivalRandom.nextFloat()*pi*2);
    stack->remove(L"Slot");
    item.stack=std::shared_ptr<CompoundTag>(stack.release());
    state->droppedItems.push_back(std::move(item));
}
bool World::destroyBlock(int x,int y,int z,int slot){
    if(!inside(x,y,z) || slot<0 || slot>=9)return false;
    const int id=get(x,y,z),data=getData(x,y,z);
    if(id==Air || id==Bedrock)return false;
    if(!survival())return breakBlock(x,y,z);
    const auto* tile=consoleSurvivalTile(id);
    if(!tile || tile->destroyTime<0)return false;
    const int held=carriedItems()[slot].id;
    // ServerPlayerGameMode::destroyBlock: drops only when the player could
    // harvest the tile with the item held before the block was removed.
    const bool harvest=consolePlayerCanDestroy(id,held);
    // Container onRemove drops the contents before the tile disappears.
    if(CompoundTag* container=id==54?state->chest(x,y,z):id==61 || id==62?state->furnace(x,y,z):
                                id==117?state->brewingStand(x,y,z):nullptr){
        auto& random=state->survivalRandom;
        if(auto* list=dynamic_cast<TagList*>(container->get(L"Items")))
            for(int i=0;i<list->size();++i){
                auto* item=dynamic_cast<CompoundTag*>(list->get(i));if(!item)continue;
                int count=static_cast<unsigned char>(item->getByte(L"Count"));
                const double xo=random.nextFloat()*.8+.1,yo=random.nextFloat()*.8+.1,zo=random.nextFloat()*.8+.1;
                while(count>0){
                    const int part=std::min(count,random.nextInt(21)+10);count-=part;
                    std::unique_ptr<CompoundTag> copy(static_cast<CompoundTag*>(item->copy()));
                    copy->putByte(L"Count",part);
                    const Vec3 velocity{random.nextGaussian()*.05,random.nextGaussian()*.05+.2,random.nextGaussian()*.05};
                    spawnDroppedItem({x+xo,y+yo,z+zo},velocity,std::move(copy),10);
                }
            }
    }
    if(!breakBlock(x,y,z))return false;
    // ServerPlayerGameMode::destroyBlock: ItemInstance::mineBlock wears the
    // tool first (a broken tool is removed), then Tile::playerDestroy runs
    // with whatever is still selected.
    int selected=held;
    if(held>0){
        const int wear=consoleToolMineDamage(held,id),maximum=consoleItemMaxDamage(held);
        if(wear>0 && maximum>0)if(auto* tool=carriedAt(*state->inventory,slot)){
            const int damage=tool->getShort(L"Damage")+wear;
            if(damage>maximum){removeCarried(*state->inventory,slot);selected=0;}
            else tool->putShort(L"Damage",damage);
        }
    }
    if(!harvest)return true;
    state->playerFood.addExhaustion(consoleMineExhaustion());
    // A door's upper half removes the lower half, which drops the item.
    const int dropData=(id==64 || id==71) && (data&8)?0:data;
    auto& random=state->survivalRandom;
    int experience=0;
    for(const auto& drop:consoleTileDrops(id,dropData,selected,random,&experience)){
        const double xo=random.nextFloat()*.7+.15,yo=random.nextFloat()*.7+.15,zo=random.nextFloat()*.7+.15;
        const Vec3 velocity{random.nextFloat()*.2-.1,.2,random.nextFloat()*.2-.1};
        spawnDroppedItem({x+xo,y+yo,z+zo},velocity,stackTag(drop.id,drop.count,drop.damage),10);
    }
    if(experience>0)spawnExperienceOrbs({x+.5,y+.5,z+.5},experience); // Tile::popExperience
    // IceTile::playerDestroy: melts into flowing water over a solid or liquid tile.
    if(id==79 && y>0){
        const int below=get(x,y-1,z);
        if(solid(static_cast<Block>(below)) || below==8 || below==9 || below==10 || below==11){
            set(x,y,z,static_cast<Block>(8));updateLiquidNeighbors(x,y,z);
        }
    }
    return true;
}
bool World::consumeCarried(int slot,int amount){
    if(slot<0 || slot>=36 || amount<1)return false;
    if(!survival())return true; // Creative placement keeps the stack.
    auto* item=carriedAt(*state->inventory,slot);
    if(!item)return false;
    const int count=static_cast<unsigned char>(item->getByte(L"Count"));
    if(count<amount)return false;
    if(count==amount)removeCarried(*state->inventory,slot);
    else item->putByte(L"Count",count-amount);
    return true;
}
int World::addCarriedStack(CompoundTag& stack){
    CompoundTag source;auto list=std::make_unique<TagList>();
    std::unique_ptr<CompoundTag> copy(static_cast<CompoundTag*>(stack.copy()));
    copy->putByte(L"Slot",0);list->add(copy.get());copy.release();
    source.put(L"Items",list.get());list.release();
    const int before=containerItems(source,1)[0].count;
    if(!before)return 0;
    moveContainerStack(source,1,0,*state->inventory,36);
    return containerItems(source,1)[0].count;
}
int World::addCarriedItem(int id,int count,int damage){
    if(id<1 || id>32767 || count<1 || damage<0 || damage>32767)throw std::invalid_argument("Invalid carried item");
    int left=count;
    const int limit=std::max(1,std::min(64,consoleItemStackLimit(id)));
    while(left>0){
        const int part=std::min(left,limit);
        const int rest=addCarriedStack(*stackTag(id,part,damage));
        left-=part-rest;
        if(rest)break;
    }
    return left;
}
bool World::dropCarried(int slot,bool wholeStack,Vec3 eye,double yaw,double pitch){
    if(slot<0 || slot>=36)return false;
    auto* item=carriedAt(*state->inventory,slot);
    if(!item)return false;
    const int count=static_cast<unsigned char>(item->getByte(L"Count"));
    const int dropped=wholeStack?count:1;
    std::unique_ptr<CompoundTag> copy(static_cast<CompoundTag*>(item->copy()));
    copy->putByte(L"Count",dropped);
    if(dropped==count)removeCarried(*state->inventory,slot);
    else item->putByte(L"Count",count-dropped);
    // Player::drop(item,false): thrown along the view with a small spread.
    auto& random=state->survivalRandom;
    const Vec3 look{std::sin(yaw)*std::cos(pitch),std::sin(pitch),-std::cos(yaw)*std::cos(pitch)};
    Vec3 velocity{look.x*.3,look.y*.3+.1,look.z*.3};
    const double direction=random.nextFloat()*pi*2,power=.02*random.nextFloat();
    velocity.x+=std::cos(direction)*power;
    velocity.y+=(random.nextFloat()-random.nextFloat())*.1;
    velocity.z+=std::sin(direction)*power;
    spawnDroppedItem({eye.x,eye.y-.3,eye.z},velocity,std::move(copy),40);
    return true;
}
bool World::playerEyeInWater()const{
    // Entity::isUnderLiquid(Material::water) at the head height.
    const auto feet=state->playerPosition;
    const double eye=feet.y+1.62;
    const int x=int(std::floor(feet.x)),y=int(std::floor(eye)),z=int(std::floor(feet.z));
    if(!inside(x,y,z))return false;
    const int id=get(x,y,z);
    if(id!=8 && id!=9)return false;
    int data=getData(x,y,z);if(data>=8)data=0;
    const double height=(data+1)/9.0-1/9.0; // LiquidTile::getHeight - 1/9
    return eye<y+1-height;
}
bool World::playerTouches(int tileA,int tileB,double shrinkX,double shrinkY)const{
    const auto p=state->playerPosition;
    const double x0=p.x-.3+shrinkX,x1=p.x+.3-shrinkX,y0=p.y+shrinkY,y1=p.y+1.8-shrinkY;
    const double z0=p.z-.3+shrinkX,z1=p.z+.3-shrinkX;
    for(int x=int(std::floor(x0));x<=int(std::floor(x1));++x)
    for(int y=int(std::floor(y0));y<=int(std::floor(y1));++y)
    for(int z=int(std::floor(z0));z<=int(std::floor(z1));++z){
        if(!inside(x,y,z))continue;
        const int id=get(x,y,z);
        if(id==tileA || id==tileB)return true;
    }
    return false;
}
bool World::playerInWater()const{return state->hasPlayerPosition && playerTouches(8,9,.001,.4);}
bool World::hurtPlayer(int damage){return applySourceMobHurt(state->playerHurt,damage,!survival());}
void World::playerWalked(double meters,bool sprinting,bool swimming){
    if(!survival() || !std::isfinite(meters) || meters<=0)return;
    // FoodConstants: EXHAUSTION_SWIM .015, EXHAUSTION_SPRINT .1, EXHAUSTION_WALK .01 per metre.
    state->playerFood.addExhaustion(float(meters*(swimming?.015:sprinting?.1:.01)));
}
void World::playerJumped(bool sprinting){
    if(survival())state->playerFood.addExhaustion(sprinting?.8f:.2f); // EXHAUSTION_(SPRINT_)JUMP
}
void World::playerAttacked(int slot){
    if(!survival() || slot<0 || slot>=9)return;
    state->playerFood.addExhaustion(.3f); // EXHAUSTION_ATTACK
    const int held=carriedItems()[slot].id,wear=consoleToolAttackDamageCost(held),maximum=consoleItemMaxDamage(held);
    if(wear>0 && maximum>0)if(auto* tool=carriedAt(*state->inventory,slot)){
        const int damage=tool->getShort(L"Damage")+wear;
        if(damage>maximum)removeCarried(*state->inventory,slot);
        else tool->putShort(L"Damage",damage);
    }
}
void World::playerLanded(double fallDistance){
    // Player::causeFallDamage returns for players that may fly; Mob deals
    // ceil(distance-3). Landing in water never reaches here (fallDistance=0).
    if(!survival() || !std::isfinite(fallDistance))return;
    const int damage=int(std::ceil(fallDistance-3));
    if(damage>0)applySourceMobHurt(state->playerHurt,damage,false);
}
void World::tickPlayerSurvival(){
    if(!state->hasPlayerPosition)return;
    if(state->playerHurt.health<=0){handlePlayerDeath();return;}
    const bool invulnerable=!survival();
    auto hurt=[&](int damage,bool fire){
        if(fire && effectAmplifier(state->playerEffects,12)>=0)return; // fireResistance
        applySourceMobHurt(state->playerHurt,damage,invulnerable);
    };
    const auto feet=state->playerPosition;
    // Mob::baseTick: suffocation inside an opaque full block at eye height.
    const int ex=int(std::floor(feet.x)),ey=int(std::floor(feet.y+1.62)),ez=int(std::floor(feet.z));
    if(inside(ex,ey,ez) && fullCube(get(ex,ey,ez)))hurt(1,false);
    // Mob::baseTick air supply; water breathing (13) keeps the supply full.
    if(playerEyeInWater() && effectAmplifier(state->playerEffects,13)<0){
        if(--state->playerAir==-20){state->playerAir=0;hurt(2,false);}
        state->playerFire=0;
    }else state->playerAir=300;
    const bool inWater=playerInWater();
    if(inWater)state->playerFire=0;
    // Entity::lavaHurt and fire tiles.
    if(playerTouches(10,11,.1,.4)){hurt(4,true);state->playerFire=std::max(state->playerFire,300);}
    if(playerTouches(51,51,.001,.001)){hurt(1,true);if(!inWater)state->playerFire=std::max(state->playerFire,160);}
    if(state->playerFire>0){
        if(state->playerFire%20==0)hurt(1,true);
        --state->playerFire;
    }
    // Mob::outOfWorld below the void threshold.
    if(feet.y<-64)hurt(4,false);
    if(state->playerHurt.health<=0)handlePlayerDeath();
}
void World::handlePlayerDeath(){
    if(state->playerDeathHandled)return;
    state->playerDeathHandled=true;
    state->playerFire=0;
    if(!survival())return;
    // Player::die -> Inventory::dropAll uses Player::drop(item,true).
    const auto feet=state->playerPosition;
    auto& random=state->survivalRandom;
    auto* list=carriedList(*state->inventory);
    for(int i=0;i<list->size();++i){
        auto* item=dynamic_cast<CompoundTag*>(list->get(i));if(!item)continue;
        std::unique_ptr<CompoundTag> copy(static_cast<CompoundTag*>(item->copy()));
        const double power=random.nextFloat()*.5,direction=random.nextFloat()*pi*2;
        spawnDroppedItem({feet.x,feet.y+1.32,feet.z},{-std::sin(direction)*power,.2,std::cos(direction)*power},
                         std::move(copy),40);
    }
    state->inventory->put(L"Items",new TagList());
    // Mob::die drops Player::getExperienceReward as orbs; the respawned
    // player starts with no experience.
    const int reward=state->playerExperience.getExperienceReward();
    if(reward>0)spawnExperienceOrbs(feet,reward);
    state->playerExperience=PlayerExperience();
    state->playerEffects.clear();
}
void World::respawnPlayer(){
    state->playerHurt=PlayerHurtState{};
    state->playerFood=FoodData();
    state->playerAir=300;state->playerFire=0;state->playerPickupDelay=0;
    state->playerEffects.clear();
    state->playerDeathHandled=false;
}
void World::mergeDroppedItem(DroppedItem& self){
    // ItemEntity::mergeWithNeighbours over bb.grow(.5,0,.5) and ::merge.
    for(auto& other:state->droppedItems){
        if(&other==&self || other.health<=0 || other.count<=0 || self.health<=0 || self.count<=0)continue;
        if(std::abs(other.position.x-self.position.x)>=.75 || std::abs(other.position.z-self.position.z)>=.75 ||
           std::abs(other.position.y-self.position.y)>=.25)continue;
        if(other.id!=self.id || other.damage!=self.damage)continue;
        const auto* selfTag=self.stack->get(L"tag");const auto* otherTag=other.stack->get(L"tag");
        if(bool(selfTag)!=bool(otherTag) || (selfTag && !const_cast<Tag*>(selfTag)->equals(const_cast<Tag*>(otherTag))))continue;
        DroppedItem& target=other.count<self.count?self:other;
        DroppedItem& source=&target==&self?other:self;
        if(target.count+source.count>std::max(1,std::min(64,consoleItemStackLimit(target.id))))continue;
        target.count+=source.count;target.stack->putByte(L"Count",target.count);
        target.throwTime=std::max(target.throwTime,source.throwTime);
        target.age=std::min(target.age,source.age);
        source.count=0;source.health=0;
        if(&source==&self)return;
    }
}
void World::tickDroppedItems(){
    auto& random=state->survivalRandom;
    const auto player=state->playerPosition;
    const bool alive=state->playerHurt.health>0;
    for(auto& item:state->droppedItems){
        const auto& p=item.position;
        if(!inside(int(std::floor(p.x)),int(std::floor(p.y)),int(std::floor(p.z))))continue;
        if(item.throwTime>0)--item.throwTime;
        const Vec3 old=item.position;
        item.velocity.y-=.04;
        // Entity::baseTick: lavaHurt on ItemEntity::hurt.
        const int lava=get(int(std::floor(p.x)),int(std::floor(p.y)),int(std::floor(p.z)));
        if(lava==10 || lava==11)item.health-=4;
        // Entity::checkInTile: an item inside a block is pushed upward.
        if(collides(item.position,.25,.25))item.position.y+=.1;
        const bool wasGrounded=collides({p.x,p.y-.01,p.z},.25,.25);
        auto moved=item.position;moved.x+=item.velocity.x;
        if(!collides(moved,.25,.25))item.position.x=moved.x;else item.velocity.x=0;
        moved=item.position;moved.z+=item.velocity.z;
        if(!collides(moved,.25,.25))item.position.z=moved.z;else item.velocity.z=0;
        moved=item.position;moved.y+=item.velocity.y;
        bool onGround=false;
        if(moved.y>=0 && !collides(moved,.25,.25))item.position.y=moved.y;
        else{onGround=item.velocity.y<0 || wasGrounded;item.velocity.y=0;}
        double friction=.98;
        if(onGround){
            const int below=get(int(std::floor(item.position.x)),int(std::floor(item.position.y))-1,int(std::floor(item.position.z)));
            friction=(below==79?.98:.6)*.98; // Tile friction: ice .98, default .6
        }
        item.velocity.x*=friction;item.velocity.y*=.98;item.velocity.z*=friction;
        if(onGround)item.velocity.y*=-.5;
        // ItemEntity::tick: after crossing a block boundary (or every 25
        // ticks) lava flings the item and it merges with nearby stacks.
        const bool crossed=int(old.x)!=int(item.position.x) || int(old.y)!=int(item.position.y) || int(old.z)!=int(item.position.z);
        if(crossed || item.age%25==0){
            const int here=get(int(std::floor(item.position.x)),int(std::floor(item.position.y)),int(std::floor(item.position.z)));
            if(here==10 || here==11){
                const float a=random.nextFloat(),b=random.nextFloat();
                const float c=random.nextFloat(),d=random.nextFloat();
                item.velocity={(a-b)*.2,.2,(c-d)*.2};
            }
            mergeDroppedItem(item);
        }
        ++item.age;
        // Player::aiStep touches entities in bb.grow(1,0,1); ItemEntity::
        // playerTouch adds what fits once the throw delay has elapsed.
        if(alive && state->hasPlayerPosition && item.throwTime==0 && item.health>0 &&
           std::abs(item.position.x-player.x)<1.3+.125 && std::abs(item.position.z-player.z)<1.3+.125 &&
           item.position.y+.25>player.y && item.position.y<player.y+1.8){
            const int left=addCarriedStack(*item.stack);
            if(left<item.count){
                item.count=left;item.stack->putByte(L"Count",left);
                if(!left)item.health=0;
            }
        }
    }
    state->droppedItems.erase(std::remove_if(state->droppedItems.begin(),state->droppedItems.end(),
        [&](const DroppedItem& item){
            const int chunkX=Mth::intFloorDiv(int(std::floor(item.position.x))-64,16);
            const int chunkZ=Mth::intFloorDiv(int(std::floor(item.position.z))-64,16);
            return item.health<=0 || item.count<=0 || item.age>=6000 || item.position.y<-64 ||
                   !state->region.hasChunk(chunkX,chunkZ);
        }),state->droppedItems.end());
}
bool World::canEatCarried(int slot)const{
    if(slot<0 || slot>=36 || !survival() || state->playerHurt.health<=0)return false;
    const auto item=carriedItems()[slot];
    const auto* food=consoleFood(item.id);
    // Player::canEat: canAlwaysEat or FoodData::needsFood.
    return food && (food->canAlwaysEat || state->playerFood.needsFood());
}
bool World::eatCarried(int slot){
    if(!canEatCarried(slot))return false;
    const auto item=carriedItems()[slot];
    const auto& food=*consoleFood(item.id);
    state->playerFood.eat(food.nutrition,food.saturationModifier);
    if(item.id==322 && item.damage>0){
        // GoldenAppleItem::addEatEffect for the enchanted apple.
        addPotionEffects(state->playerEffects,{{10,30*20,3},{11,300*20,0},{12,300*20,0}});
    }else if(food.effectId>0 && state->survivalRandom.nextFloat()<food.effectProbability)
        addPotionEffects(state->playerEffects,{{food.effectId,food.effectSeconds*20,food.effectAmplifier}});
    consumeCarried(slot);
    if(food.returnsItem){
        if(!carriedAt(*state->inventory,slot)){
            auto bowl=stackTag(food.returnsItem,1,0);bowl->putByte(L"Slot",slot);
            carriedList(*state->inventory)->add(bowl.get());bowl.release();
        }else if(addCarriedItem(food.returnsItem,1)){
            spawnDroppedItem({state->playerPosition.x,state->playerPosition.y+1.32,state->playerPosition.z},
                             {0,.1,0},stackTag(food.returnsItem,1,0),40);
        }
    }
    return true;
}
bool World::canCraft(const CraftingRecipe& recipe)const{
    const auto carried=carriedItems();
    for(const auto& need:recipe.ingredients){
        int have=0;
        for(const auto& item:carried)
            if(item.id==need.id && (need.damage<0 || item.damage==need.damage))have+=item.count;
        if(have<need.count)return false;
    }
    return !recipe.ingredients.empty();
}
bool World::craft(const CraftingRecipe& recipe){
    if(!canCraft(recipe))return false;
    // IUIScene_CraftingMenu: remove each required item one at a time with
    // Inventory::removeResource (the first matching slot), return crafting
    // remainders, then add the result (dropping it when there is no room).
    for(const auto& need:recipe.ingredients)for(int n=0;n<need.count;++n){
        const auto carried=carriedItems();
        for(int slot=0;slot<36;++slot){
            const auto& item=carried[slot];
            if(item.id!=need.id || (need.damage>=0 && item.damage!=need.damage))continue;
            auto* tag=carriedAt(*state->inventory,slot);
            if(item.count<=1)removeCarried(*state->inventory,slot);
            else tag->putByte(L"Count",item.count-1);
            if(const int remainder=consoleCraftingRemainingItem(item.id);remainder && addCarriedItem(remainder,1)){
                const auto p=state->playerPosition;
                spawnDroppedItem({p.x,p.y+1.32,p.z},{0,.1,0},stackTag(remainder,1,0),40);
            }
            break;
        }
    }
    if(const int left=addCarriedItem(recipe.id,recipe.count,recipe.damage)){
        const auto p=state->playerPosition;
        spawnDroppedItem({p.x,p.y+1.32,p.z},{0,.1,0},stackTag(recipe.id,left,recipe.damage),40);
    }
    return true;
}
}
namespace console {
void World::setPlayerHealth(int health){
    health=std::clamp(health,0,20);
    state->playerHurt.lastHealth=health;
    state->playerHurt.health=health;
    ++revision;
}
void World::setPlayerFood(int food){
    state->playerFood.setFoodLevel(std::clamp(food,0,20));
    ++revision;
}
bool World::setCarriedItem(int slot,int id,int count,int damage,int dataTag){
    if(slot<0 || slot>=36 || id<1 || id>32767 || damage<0 || damage>32767 ||
       count<1 || count>std::min(64,consoleItemStackLimit(id)))return false;
    containerItems(*state->inventory,36);
    auto replacement=std::make_unique<TagList>();
    if(auto* previous=dynamic_cast<TagList*>(state->inventory->get(L"Items")))
        for(int i=0;i<previous->size();++i){
            auto* item=dynamic_cast<CompoundTag*>(previous->get(i));
            if(static_cast<unsigned char>(item->getByte(L"Slot"))==slot)continue;
            std::unique_ptr<Tag> copy(item->copy());replacement->add(copy.get());copy.release();
        }
    auto item=std::make_unique<CompoundTag>();
    item->putByte(L"Slot",slot);item->putShort(L"id",id);
    item->putByte(L"Count",count);item->putShort(L"Damage",damage);
    if(dataTag){auto tag=std::make_unique<CompoundTag>();tag->putInt(L"4jdata",dataTag);item->put(L"tag",tag.get());tag.release();}
    replacement->add(item.get());item.release();
    state->inventory->put(L"Items",replacement.get());replacement.release();
    ++revision;return true;
}
}
