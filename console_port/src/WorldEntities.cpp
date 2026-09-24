#include "WorldState.h"
#include "SpawnEggColors.h"
#include "LevelSettings.h"
#include "ExperienceOrbRules.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace console {
namespace {
std::unique_ptr<TagList> triple(double x,double y,double z){
    auto values=std::make_unique<TagList>();
    for(double value:{x,y,z}){auto tag=std::make_unique<DoubleTag>(L"",value);values->add(tag.get());tag.release();}
    return values;
}
bool readTriple(CompoundTag& tag,const wchar_t* name,Vec3& out){
    auto* values=dynamic_cast<TagList*>(tag.get(name));if(!values || values->size()!=3)return false;
    double result[3];for(int i=0;i<3;++i){
        auto* value=dynamic_cast<DoubleTag*>(values->get(i));
        if(!value || !std::isfinite(value->data) || std::abs(value->data)>30000000)return false;
        result[i]=value->data;
    }
    out={result[0],result[1],result[2]};return true;
}
bool hostile(const std::wstring& id){
    return id==L"Zombie" || id==L"Skeleton" || id==L"Spider" || id==L"Creeper" ||
           id==L"CaveSpider" || id==L"Silverfish" || id==L"Slime" ||
           id==L"LavaSlime" || id==L"Ghast" || id==L"Blaze" || id==L"Enderman";
}
bool animal(const std::wstring& id){
    return id==L"Cow" || id==L"Pig" || id==L"Sheep" || id==L"Chicken" ||
           id==L"Ozelot" || id==L"Wolf" || id==L"MushroomCow";
}
bool modeled(const std::wstring& id){return hostile(id) || animal(id) || id==L"PigZombie" || id==L"Villager" || id==L"Squid";}
int initialHealth(const std::wstring& id,int slimeSize=1,bool wolfTame=false){
    if(id==L"Slime" || id==L"LavaSlime")return slimeSize*slimeSize;
    if(id==L"Enderman")return 40;
    if(id==L"Spider")return 16;
    if(id==L"CaveSpider")return 12;
    if(id==L"Chicken")return 4;
    if(id==L"Silverfish" || id==L"Sheep" || (id==L"Wolf" && !wolfTame))return 8;
    if(id==L"Ghast" || id==L"Squid" || id==L"Cow" || id==L"MushroomCow" ||
       id==L"Pig" || id==L"Ozelot")return 10;
    return 20;
}
int sourceAttackDamage(int id){
    // Item's default is one. WeaponItem uses 4+tier bonus; DiggerItem uses
    // its tool-specific base plus the same tier bonus (Item.cpp).
    switch(id){
    case 268:case 283:return 4;case 272:return 5;case 267:return 6;case 276:return 7;
    case 270:case 285:return 2;case 274:return 3;case 257:return 4;case 278:return 5;
    case 271:case 286:return 3;case 275:return 4;case 258:return 5;case 279:return 6;
    case 269:case 284:return 1;case 273:return 2;case 256:return 3;case 277:return 4;
    default:return 1;
    }
}
double rayBox(Vec3 eye,Vec3 direction,Vec3 low,Vec3 high,double reach){
    double near=0,far=reach;
    for(int axis=0;axis<3;++axis){
        const double start=axis==0?eye.x:axis==1?eye.y:eye.z;
        const double speed=axis==0?direction.x:axis==1?direction.y:direction.z;
        const double a=axis==0?low.x:axis==1?low.y:low.z;
        const double b=axis==0?high.x:axis==1?high.y:high.z;
        if(std::abs(speed)<1e-12){if(start<a || start>b)return std::numeric_limits<double>::infinity();continue;}
        double first=(a-start)/speed,last=(b-start)/speed;
        if(first>last)std::swap(first,last);
        near=std::max(near,first);far=std::min(far,last);
        if(near>far)return std::numeric_limits<double>::infinity();
    }
    return near;
}
std::pair<double,double> entitySize(const std::wstring& id,int slimeSize=1){
    // Entity's default is 0.6 x 1.8. These overrides are the setSize calls
    // in the source creature constructors, not the visible model dimensions.
    if(id==L"Spider")return {1.4,.9};
    if(id==L"CaveSpider")return {.7,.5};
    if(id==L"Silverfish" || id==L"Chicken")return {.3,.7};
    if(id==L"Ozelot" || id==L"Wolf")return {.6,.8};
    if(id==L"Cow" || id==L"MushroomCow" || id==L"Sheep")return {.9,1.3};
    if(id==L"Pig")return {.9,.9};
    if(id==L"Slime" || id==L"LavaSlime")return {.6*slimeSize,.6*slimeSize};
    if(id==L"Ghast")return {4,4};
    if(id==L"Squid")return {.95,.95};
    if(id==L"Enderman")return {.6,2.9};
    return {.6,1.8};
}
bool owned(const SimulatedEntity& entity,const ChunkRecord& record){
    if(entity.native)return entity.nativeChunkX==record.x && entity.nativeChunkZ==record.z;
    const int x=int(std::floor(entity.position.x-64)),z=int(std::floor(entity.position.z-64));
    return x>=record.x*16 && x<record.x*16+16 && z>=record.z*16 && z<record.z*16+16;
}
bool owned(const ExperienceOrbState& orb,const ChunkRecord& record){
    if(orb.native)return orb.nativeChunkX==record.x && orb.nativeChunkZ==record.z;
    const int x=int(std::floor(orb.position.x-64)),z=int(std::floor(orb.position.z-64));
    return x>=record.x*16 && x<record.x*16+16 && z>=record.z*16 && z<record.z*16+16;
}
}
void World::setPlayerPosition(Vec3 position){
    if(!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))return;
    state->playerPosition=position;state->hasPlayerPosition=true;
}
bool World::spawnCreativeEgg(int entityId,Vec3 position){
    const auto egg=consoleSourceEggColors(entityId);
    const int slimeSize=egg.entityName && (std::wstring(egg.entityName)==L"Slime" ||
        std::wstring(egg.entityName)==L"LavaSlime")?1<<state->entityRandom.nextInt(3):1;
    const auto [bodyWidth,bodyHeight]=egg.entityName?entitySize(egg.entityName,slimeSize):std::pair<double,double>{.6,1.8};
    if(!egg.valid || !egg.entityName || !modeled(egg.entityName) ||
       state->pending || state->entities.size()>=60 ||
       !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
       !inside(int(std::floor(position.x)),int(std::floor(position.y)),int(std::floor(position.z))) ||
       collides(position,bodyWidth,bodyHeight))return false;
    state->entities.push_back({egg.entityName,position,{},0});
    state->entities.back().yaw=state->entityRandom.nextFloat()*360;
    state->entities.back().slimeSize=slimeSize;
    state->entities.back().health=initialHealth(state->entities.back().id,slimeSize);
    if(state->entities.back().id==L"Slime" || state->entities.back().id==L"LavaSlime")
        state->entities.back().jumpDelay=10+state->entityRandom.nextInt(20);
    if(state->entities.back().id==L"Squid")
        state->entities.back().squidPhaseSpeed=.2/(1+state->entityRandom.nextFloat());
    if(state->entities.back().id==L"Villager")
        state->entities.back().profession=state->entityRandom.nextInt(5);
    ++revision;
    return true;
}
std::optional<std::wstring> World::pickEntity(Vec3 eye,Vec3 direction,double reach)const{
    if(state->pending || !std::isfinite(reach) || reach<=0 || reach>64)return std::nullopt;
    const double length=std::hypot(direction.x,direction.y,direction.z);
    if(!std::isfinite(length) || length<1e-12)return std::nullopt;
    direction={direction.x/length,direction.y/length,direction.z/length};
    const auto block=raycast(eye,direction,reach);
    double best=block.hit?block.distance:reach;
    const SimulatedEntity* target=nullptr;
    for(const auto& entity:state->entities){
        if(entity.health<=0)continue;
        const auto [width,height]=entitySize(entity.id,entity.slimeSize);
        const double radius=width*.5;
        const double hit=rayBox(eye,direction,
            {entity.position.x-radius,entity.position.y,entity.position.z-radius},
            {entity.position.x+radius,entity.position.y+height,entity.position.z+radius},reach);
        if(hit<=best){best=hit;target=&entity;}
    }
    if(!target)return std::nullopt;
    return target->id;
}
bool World::attackEntity(Vec3 eye,Vec3 direction,int heldItemId,double reach){
    if(state->pending || !std::isfinite(reach) || reach<=0 || reach>64 ||
       !std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z))return false;
    const double length=std::hypot(direction.x,direction.y,direction.z);
    if(!std::isfinite(length) || length<1e-12)return false;
    direction={direction.x/length,direction.y/length,direction.z/length};
    const auto block=raycast(eye,direction,reach);
    const double blockDistance=block.hit?block.distance:reach;
    SimulatedEntity* target=nullptr;
    double best=blockDistance;
    for(auto& entity:state->entities){
        if(entity.health<=0)continue;
        const auto [width,height]=entitySize(entity.id,entity.slimeSize);
        const double radius=width*.5;
        const double hit=rayBox(eye,direction,
            {entity.position.x-radius,entity.position.y,entity.position.z-radius},
            {entity.position.x+radius,entity.position.y+height,entity.position.z+radius},reach);
        if(hit<=best){best=hit;target=&entity;}
    }
    if(!target)return false;
    int damage=sourceAttackDamage(heldItemId);
    for(const auto& effect:state->playerEffects){
        if(effect.id==5)damage+=3<<std::clamp(effect.amplifier,0,8);
        if(effect.id==18)damage-=2<<std::clamp(effect.amplifier,0,8);
    }
    if(damage<=0)return true;
    if(target->invulnerableTicks>10){
        if(damage<=target->lastHurt)return true;
        target->health-=damage-target->lastHurt;
    }else{
        target->health-=damage;
        target->invulnerableTicks=20;
        target->hurtTicks=10;
    }
    target->lastHurt=damage;
    target->lastHurtByPlayerTicks=60;
    // Mob::knockback halves momentum and adds a 0.4 impulse away from the hit.
    const double awayX=target->position.x-eye.x,awayZ=target->position.z-eye.z;
    const double away=std::hypot(awayX,awayZ);
    if(away>1e-9){
        target->velocity.x=target->velocity.x*.5+awayX/away*.4;
        target->velocity.z=target->velocity.z*.5+awayZ/away*.4;
        target->velocity.y=std::min(.4,target->velocity.y*.5+.4);
    }
    target->health=std::max(0,target->health);
    if(target->native){
        if(auto record=state->records.find({target->nativeChunkX,target->nativeChunkZ});
           record!=state->records.end() && record->second->extra){
            if(auto* list=dynamic_cast<TagList*>(record->second->extra->get(L"Entities"));
               list && target->recordIndex>=0 && target->recordIndex<list->size()){
                if(auto* tag=dynamic_cast<CompoundTag*>(list->get(target->recordIndex))){
                    tag->putShort(L"Health",target->health);
                    tag->putShort(L"HurtTime",target->hurtTicks);
                    tag->putInt(L"console_port.lastHurtByPlayerTicks",60);
                }
            }
        }
    }
    ++revision;
    return true;
}
const std::vector<SimulatedEntity>& World::entities()const{return state->entities;}
const std::vector<ExperienceOrbState>& World::experienceOrbs()const{return state->experienceOrbs;}
const std::vector<HangingDecoration>& World::hangingDecorations()const{return state->hangingDecorations;}
void World::spawnExperienceOrbs(Vec3 position,int reward){
    while(reward>0){
        const int value=sourceExperienceOrbValue(reward);reward-=value;
        ExperienceOrbState orb;orb.position=position;orb.value=value;
        orb.velocity={(state->entityRandom.nextFloat()*.2-.1)*2,
                      state->entityRandom.nextFloat()*.4,
                      (state->entityRandom.nextFloat()*.2-.1)*2};
        state->experienceOrbs.push_back(orb);
    }
}
void World::tickExperienceOrbs(){
    if(state->playerXpPickupDelay>0)--state->playerXpPickupDelay;
    const Vec3 player=state->playerPosition;
    const auto forgetNative=[&](const ExperienceOrbState& orb){
        if(!orb.native)return;
        if(auto record=state->records.find({orb.nativeChunkX,orb.nativeChunkZ});
           record!=state->records.end() && record->second->extra)
            if(auto* list=dynamic_cast<TagList*>(record->second->extra->get(L"Entities"));
               list && orb.recordIndex>=0 && orb.recordIndex<list->size())
                if(auto* tag=dynamic_cast<CompoundTag*>(list->get(orb.recordIndex)))
                    tag->putBoolean(L"console_port.pickedUp",true);
    };
    for(auto& orb:state->experienceOrbs){
        if(!inside(int(std::floor(orb.position.x)),int(std::floor(orb.position.y)),
                   int(std::floor(orb.position.z))))continue;
        if(orb.throwTime>0)--orb.throwTime;
        ++orb.age;
        const int bx=int(std::floor(orb.position.x)),by=int(std::floor(orb.position.y)),
            bz=int(std::floor(orb.position.z));
        const bool lava=inside(bx,by,bz) && get(bx,by,bz)==Lava;
        if(lava){
            orb.velocity.y=.2;
            orb.velocity.x=(state->entityRandom.nextFloat()-state->entityRandom.nextFloat())*.2;
            orb.velocity.z=(state->entityRandom.nextFloat()-state->entityRandom.nextFloat())*.2;
        }else orb.velocity.y-=.03;
        const double dx=player.x-orb.position.x,dy=player.y+1.62-orb.position.y,
            dz=player.z-orb.position.z;
        const double distance=std::hypot(dx,dy,dz);
        if(distance>1e-9 && distance<8){
            const double normalized=distance/8,power=(1-normalized)*(1-normalized)*.1;
            orb.velocity.x+=dx/distance*power;
            orb.velocity.y+=dy/distance*power;
            orb.velocity.z+=dz/distance*power;
        }
        const bool wasGrounded=collides({orb.position.x,orb.position.y-.05,
                                         orb.position.z},.5,.5);
        auto moved=orb.position;moved.x+=orb.velocity.x;
        if(inside(int(std::floor(moved.x)),int(std::floor(moved.y)),int(std::floor(moved.z))) &&
           !collides(moved,.5,.5))orb.position.x=moved.x;else orb.velocity.x=0;
        moved=orb.position;moved.z+=orb.velocity.z;
        if(inside(int(std::floor(moved.x)),int(std::floor(moved.y)),int(std::floor(moved.z))) &&
           !collides(moved,.5,.5))orb.position.z=moved.z;else orb.velocity.z=0;
        moved=orb.position;moved.y+=orb.velocity.y;
        if(moved.y>=0 && moved.y<height-.5 && !collides(moved,.5,.5))
            orb.position.y=moved.y;
        else orb.velocity.y=wasGrounded?-orb.velocity.y*.9:0;
        const bool onGround=collides({orb.position.x,orb.position.y-.05,
                                     orb.position.z},.5,.5);
        const double friction=onGround?.588:.98;
        orb.velocity.x*=friction;orb.velocity.z*=friction;orb.velocity.y*=.98;
        const double px=player.x-orb.position.x,pz=player.z-orb.position.z;
        if(orb.throwTime==0 && state->playerXpPickupDelay==0 &&
           std::abs(px)<.55 && std::abs(pz)<.55 &&
           player.y<orb.position.y+.5 && player.y+1.8>orb.position.y){
            state->playerExperience.increaseXp(orb.value);
            state->playerXpPickupDelay=2;
            orb.health=0;
            forgetNative(orb);
        }
        if(orb.age>=6000)forgetNative(orb);
    }
    state->experienceOrbs.erase(std::remove_if(state->experienceOrbs.begin(),
        state->experienceOrbs.end(),[&](const ExperienceOrbState& orb){
            return orb.health<=0 || orb.age>=6000;
        }),state->experienceOrbs.end());
}
void World::tickEntities(){
    if(state->pending || !state->hasPlayerPosition)return;
    const auto player=state->playerPosition;
    tickExperienceOrbs();
    tickDroppedItems();
    // Level::getNearestAttackablePlayer excludes invulnerable creative players.
    const bool playerAttackable=state->metadata->getGameType()!=GameType::CREATIVE &&
        state->playerHurt.health>0;
    for(auto& [chunkKey,record]:state->records){
        if(!record->extra)continue;
        auto* tiles=dynamic_cast<TagList*>(record->extra->get(L"TileEntities"));if(!tiles)continue;
        for(int i=0;i<tiles->size();++i){
            auto* tile=dynamic_cast<CompoundTag*>(tiles->get(i));
            if(!tile || tile->getString(L"id")!=L"MobSpawner")continue;
            const int x=tile->getInt(L"x")+64,y=tile->getInt(L"y"),z=tile->getInt(L"z")+64;
            if(!inside(x,y,z) || get(x,y,z)!=static_cast<Block>(52))continue;
            const double dx=player.x-x-.5,dy=player.y-y-.5,dz=player.z-z-.5;
            if(dx*dx+dy*dy+dz*dz>16*16)continue;
            int delay=tile->getShort(L"Delay");
            const int minDelay=tile->contains(L"MinSpawnDelay")?
                std::max(1,int(tile->getShort(L"MinSpawnDelay"))):200;
            const int maxDelay=tile->contains(L"MaxSpawnDelay")?
                std::max(minDelay+1,int(tile->getShort(L"MaxSpawnDelay"))):800;
            if(delay<0)delay=minDelay+state->entityRandom.nextInt(maxDelay-minDelay);
            if(delay>0){tile->putShort(L"Delay",--delay);continue;}
            const auto id=tile->getString(L"EntityId");
            if(!modeled(id)){
                tile->putShort(L"Delay",minDelay+state->entityRandom.nextInt(maxDelay-minDelay));
                continue;
            }
            const int count=tile->contains(L"SpawnCount")?
                std::clamp(int(tile->getShort(L"SpawnCount")),1,16):4;
            for(int attempt=0;attempt<count;++attempt){
                if(id.empty() || state->entities.size()>=60)break;
                int nearby=0;
                for(const auto& entity:state->entities)if(entity.id==id &&
                    std::abs(entity.position.x-x-.5)<=8 &&
                    std::abs(entity.position.y-y-.5)<=4 &&
                    std::abs(entity.position.z-z-.5)<=8)++nearby;
                if(nearby>=6){delay=minDelay+state->entityRandom.nextInt(maxDelay-minDelay);break;}
                const double sx=x+(state->entityRandom.nextDouble()-state->entityRandom.nextDouble())*4;
                const double sy=y+state->entityRandom.nextInt(3)-1;
                const double sz=z+(state->entityRandom.nextDouble()-state->entityRandom.nextDouble())*4;
                const int bx=int(std::floor(sx)),by=int(std::floor(sy)),bz=int(std::floor(sz));
                if(!inside(bx,by,bz) || !inside(bx,by+1,bz) || by<1 ||
                   get(bx,by,bz)!=Air || get(bx,by+1,bz)!=Air || !solid(get(bx,by-1,bz)))continue;
                state->entities.push_back({id,{sx,sy,sz},{},0});
                state->entities.back().yaw=state->entityRandom.nextFloat()*360;
                state->entities.back().health=initialHealth(id);
                delay=minDelay+state->entityRandom.nextInt(maxDelay-minDelay);
            }
            tile->putShort(L"Delay",delay);
        }
    }
    // MobSpawner::tick chooses random loaded positions, excludes the player and
    // shared spawn radius, and applies Monster's sky/raw-light checks. This
    // bounded natural-spawn subset still chooses four common enemy classes.
    int hostileCount=0;
    for(const auto& entity:state->entities)
        if(hostile(entity.id))++hostileCount;
    if(!isTutorial() && hostileCount<30 && state->entities.size()<60){
        const Vec3 worldSpawn{state->metadata->getXSpawn()+64.5,
                              double(state->metadata->getYSpawn()),
                              state->metadata->getZSpawn()+64.5};
        for(int attempt=0;attempt<8 && hostileCount<30 && state->entities.size()<60;++attempt){
            const int x=originX()+8+state->entityRandom.nextInt(width-16);
            const int z=originZ()+8+state->entityRandom.nextInt(depth-16);
            const int y=1+state->entityRandom.nextInt(height-2);
            if(get(x,y,z)!=Air || get(x,y+1,z)!=Air || !solid(get(x,y-1,z)) || get(x,y-1,z)==Bedrock)continue;
            const Vec3 at{x+.5,double(y),z+.5};
            const auto distanceSquared=[&](Vec3 target){
                const double dx=at.x-target.x,dy=at.y-target.y,dz=at.z-target.z;
                return dx*dx+dy*dy+dz*dz;
            };
            if(distanceSquared(player)<24*24 || distanceSquared(worldSpawn)<24*24)continue;
            if(skyLight(x,y,z)>state->entityRandom.nextInt(32))continue;
            const int sky=std::max(0,skyLight(x,y,z)-int((1-skyDarken())*11));
            if(std::max(sky,blockLight(x,y,z))>state->entityRandom.nextInt(8))continue;
            const int choice=state->entityRandom.nextInt(4);
            const std::wstring id=choice==0?L"Skeleton":choice==1?L"Zombie":choice==2?L"Spider":L"Creeper";
            int sameType=0;for(const auto& entity:state->entities)if(entity.id==id)++sameType;
            if(sameType>=15)continue;
            state->entities.push_back({id,at,{},0});
            state->entities.back().yaw=state->entityRandom.nextFloat()*360;
            state->entities.back().health=initialHealth(id);
            ++hostileCount;
        }
    }
    // ServerLevel permits natural friendlies once per 40 ticks. Animal's
    // grass/daytime-brightness test and the same two 24-block exclusions apply.
    if((!isTutorial() || state->tutorialSpawning) && time()%40==0 && state->entities.size()<60){
        int animals=0,chickens=0;
        for(const auto& entity:state->entities){
            if(entity.id==L"Cow" || entity.id==L"Pig" || entity.id==L"Sheep")++animals;
            else if(entity.id==L"Chicken")++chickens;
        }
        const Vec3 worldSpawn{state->metadata->getXSpawn()+64.5,
                              double(state->metadata->getYSpawn()),
                              state->metadata->getZSpawn()+64.5};
        for(int attempt=0;attempt<64 && animals<20 && state->entities.size()<60;++attempt){
            const int x=originX()+8+state->entityRandom.nextInt(width-16);
            const int z=originZ()+8+state->entityRandom.nextInt(depth-16);
            const int y=1+state->entityRandom.nextInt(height-2);
            if(get(x,y-1,z)!=Grass || get(x,y,z)!=Air || get(x,y+1,z)!=Air ||
               std::max(skyLight(x,y,z),blockLight(x,y,z))<=8)continue;
            const Vec3 at{x+.5,double(y),z+.5};
            const auto away=[&](Vec3 other){
                const double dx=at.x-other.x,dy=at.y-other.y,dz=at.z-other.z;
                return dx*dx+dy*dy+dz*dz>=24*24;
            };
            if(!away(player) || !away(worldSpawn))continue;
            const int choice=state->entityRandom.nextInt(40);
            const std::wstring id=choice<12?L"Sheep":choice<22?L"Pig":choice<30?L"Cow":L"Chicken";
            if(id==L"Chicken" && chickens>=8)continue;
            state->entities.push_back({id,at,{},0});
            auto& spawned=state->entities.back();spawned.yaw=state->entityRandom.nextFloat()*360;
            spawned.health=initialHealth(id);
            if(id==L"Sheep"){
                const int color=state->entityRandom.nextInt(100);
                spawned.woolColor=color<5?15:color<10?7:color<15?8:color<18?12:
                    state->entityRandom.nextInt(500)==0?6:0;
            }
            if(id==L"Chicken")++chickens;else ++animals;
        }
    }
    for(auto& entity:state->entities){
        // Entities in the resident halo keep their state but do not tick,
        // like mobs in loaded chunks outside the original ticking range.
        if(!inside(int(std::floor(entity.position.x)),int(std::floor(entity.position.y)),
                   int(std::floor(entity.position.z))))continue;
        ++entity.age;
        if(entity.attackTicks>0)--entity.attackTicks;
        if(entity.hurtTicks>0)--entity.hurtTicks;
        if(entity.invulnerableTicks>0)--entity.invulnerableTicks;
        if(entity.lastHurtByPlayerTicks>0)--entity.lastHurtByPlayerTicks;
        if(entity.health<=0){
            if(++entity.deathTicks==20 && entity.lastHurtByPlayerTicks>0){
                const int reward=sourceMobExperienceReward(entity.id,entity.slimeSize,
                    state->entityRandom.nextInt(3));
                spawnExperienceOrbs(entity.position,reward);
            }
            if(entity.deathTicks>=20 && entity.native)
                if(auto record=state->records.find({entity.nativeChunkX,entity.nativeChunkZ});
                   record!=state->records.end() && record->second->extra)
                    if(auto* list=dynamic_cast<TagList*>(record->second->extra->get(L"Entities"));
                       list && entity.recordIndex>=0 && entity.recordIndex<list->size())
                        if(auto* tag=dynamic_cast<CompoundTag*>(list->get(entity.recordIndex)))
                            tag->putBoolean(L"console_port.defeated",true);
            continue;
        }
        const auto [bodyWidth,bodyHeight]=entitySize(entity.id,entity.slimeSize);
        const double dx=player.x-entity.position.x,dz=player.z-entity.position.z;
        const double distance=std::hypot(dx,dz);
        const bool ghast=entity.id==L"Ghast",blaze=entity.id==L"Blaze";
        const bool swimming=entity.id==L"Squid";
        const bool slime=entity.id==L"Slime" || entity.id==L"LavaSlime";
        const int bx=int(std::floor(entity.position.x)),by=int(std::floor(entity.position.y)),
            bz=int(std::floor(entity.position.z));
        const bool inWater=inside(bx,by,bz) &&
            (get(bx,by,bz)==Water || static_cast<int>(get(bx,by,bz))==8);
        const bool onGround=collides(
            {entity.position.x,entity.position.y-.05,entity.position.z},bodyWidth,bodyHeight);
        if(ghast){
            // Ghast::serverAiStep picks a target within 16 blocks and adds a
            // 0.1 impulse every two to six ticks when the path is unobstructed.
            auto targetDelta=Vec3{entity.motionTarget.x-entity.position.x,
                                  entity.motionTarget.y-entity.position.y,
                                  entity.motionTarget.z-entity.position.z};
            double targetDistance=std::hypot(targetDelta.x,targetDelta.y,targetDelta.z);
            if(targetDistance<1 || targetDistance>60){
                entity.motionTarget={entity.position.x+(state->entityRandom.nextFloat()*2-1)*16,
                                     entity.position.y+(state->entityRandom.nextFloat()*2-1)*16,
                                     entity.position.z+(state->entityRandom.nextFloat()*2-1)*16};
                targetDelta={entity.motionTarget.x-entity.position.x,
                             entity.motionTarget.y-entity.position.y,
                             entity.motionTarget.z-entity.position.z};
                targetDistance=std::hypot(targetDelta.x,targetDelta.y,targetDelta.z);
            }
            if(entity.motionTimer--<=0 && targetDistance>0){
                entity.motionTimer+=2+state->entityRandom.nextInt(5);
                const Vec3 direction{targetDelta.x/targetDistance,
                                     targetDelta.y/targetDistance,targetDelta.z/targetDistance};
                bool clear=true;
                for(int step=1;step<targetDistance;++step){
                    const Vec3 sample{entity.position.x+direction.x*step,
                                      entity.position.y+direction.y*step,
                                      entity.position.z+direction.z*step};
                    if(collides(sample,bodyWidth,bodyHeight)){clear=false;break;}
                }
                if(clear){
                    entity.velocity.x+=direction.x*.1;
                    entity.velocity.y+=direction.y*.1;
                    entity.velocity.z+=direction.z*.1;
                }else entity.motionTarget=entity.position;
            }
            entity.velocity.x*=.91;entity.velocity.y*=.91;entity.velocity.z*=.91;
            if(playerAttackable && distance<64 && distance>.01)
                entity.yaw=float(-std::atan2(dx,dz)*180/3.14159265358979323846);
        }else if(swimming){
            // Squid::serverAiStep chooses an underwater heading at most every
            // 50 ticks. aiStep advances a pulse that powers only its first half.
            if(inWater){
                if((entity.swimDirection.x==0 && entity.swimDirection.y==0 &&
                    entity.swimDirection.z==0) || state->entityRandom.nextInt(50)==0){
                    const double angle=state->entityRandom.nextFloat()*6.2831853071795864769;
                    entity.swimDirection={std::cos(angle)*.2,
                                          -.1+state->entityRandom.nextFloat()*.2,
                                          std::sin(angle)*.2};
                }
                entity.squidPhase+=entity.squidPhaseSpeed;
                if(entity.squidPhase>6.2831853071795864769){
                    entity.squidPhase-=6.2831853071795864769;
                    if(state->entityRandom.nextInt(10)==0)
                        entity.squidPhaseSpeed=.2/(1+state->entityRandom.nextFloat());
                }
                if(entity.squidPhase<3.14159265358979323846){
                    const double phase=entity.squidPhase/3.14159265358979323846;
                    entity.squidTentacleAngle=float(std::sin(phase*phase*3.14159265358979323846)*
                        3.14159265358979323846*.25);
                    if(phase>.75)entity.squidSpeed=1;
                }else{
                    entity.squidTentacleAngle=0;
                    entity.squidSpeed*=.9;
                }
                entity.velocity={entity.swimDirection.x*entity.squidSpeed,
                                 entity.swimDirection.y*entity.squidSpeed,
                                 entity.swimDirection.z*entity.squidSpeed};
                if(std::hypot(entity.velocity.x,entity.velocity.z)>.005)
                    entity.yaw=float(-std::atan2(entity.velocity.x,entity.velocity.z)*180/
                                     3.14159265358979323846);
            }else{
                entity.velocity.x=0;entity.velocity.z=0;
                entity.squidPhase+=entity.squidPhaseSpeed;
                entity.squidTentacleAngle=float(std::abs(std::sin(entity.squidPhase))*
                    3.14159265358979323846*.25);
                entity.velocity.y=std::max(-3.92,(entity.velocity.y-.08)*.98);
            }
        }else{
            // PathfinderMob's full navigation graph remains pending; basic
            // creatures still use bounded local pursuit or a random stroll.
            double steerX=0,steerZ=0;
            if(playerAttackable && hostile(entity.id) && distance>1.5 && distance<16){
                steerX=dx/distance;steerZ=dz/distance;entity.wanderTicks=0;
            }else{
                if(entity.wanderTicks<=0 && state->entityRandom.nextInt(120)==0){
                    const double angle=state->entityRandom.nextDouble()*6.2831853071795864769;
                    entity.wanderX=std::cos(angle);entity.wanderZ=std::sin(angle);
                    entity.wanderTicks=40+state->entityRandom.nextInt(80);
                }
                if(entity.wanderTicks>0){
                    steerX=entity.wanderX;steerZ=entity.wanderZ;
                    --entity.wanderTicks;
                }
            }
            if(slime && onGround){
                // Slime::serverAiStep waits 10-29 ticks (four times that for
                // LavaSlime), shortened to one third when chasing a player.
                if(entity.jumpDelay--<=0){
                    entity.jumpDelay=(10+state->entityRandom.nextInt(20))*
                        (entity.id==L"LavaSlime"?4:1);
                    if(playerAttackable && distance<16)
                        entity.jumpDelay=std::max(1,entity.jumpDelay/3);
                    entity.velocity.y=entity.id==L"LavaSlime"?
                        .42+entity.slimeSize*.1:.42;
                }else{steerX=0;steerZ=0;}
            }
            const double speed=slime?.045:.035;
            entity.velocity.x=std::clamp(entity.velocity.x*.65+steerX*speed,-.12,.12);
            entity.velocity.z=std::clamp(entity.velocity.z*.65+steerZ*speed,-.12,.12);
            if(std::hypot(entity.velocity.x,entity.velocity.z)>.005)
                entity.yaw=float(std::atan2(entity.velocity.z,entity.velocity.x)*180/
                                 3.14159265358979323846-90);
            if(blaze){
                // Blaze::aiStep rises toward a high target and slows falls.
                if(--entity.heightOffsetTimer<=0){
                    entity.heightOffsetTimer=100;
                    entity.blazeHeightOffset=.5+state->entityRandom.nextGaussian()*3;
                }
                if(playerAttackable && distance<16 && player.y+1.62>
                   entity.position.y+1.53+entity.blazeHeightOffset)
                    entity.velocity.y+=(.3-entity.velocity.y)*.3;
                if(!onGround && entity.velocity.y<0)entity.velocity.y*=.6;
            }
        }
        Vec3 horizontal=entity.position;
        horizontal.x+=entity.velocity.x;
        if(!collides(horizontal,bodyWidth,bodyHeight))entity.position.x=horizontal.x;
        else {entity.velocity.x=0;entity.wanderTicks=0;}
        horizontal=entity.position;horizontal.z+=entity.velocity.z;
        if(!collides(horizontal,bodyWidth,bodyHeight))entity.position.z=horizontal.z;
        else {entity.velocity.z=0;entity.wanderTicks=0;}
        // Apply vertical collision in small steps so high-speed falling mobs
        // cannot tunnel through a one-block floor on a lagged tick.
        const int steps=std::max(1,int(std::ceil(std::abs(entity.velocity.y)*2)));
        const double verticalStep=entity.velocity.y/steps;
        for(int step=0;step<steps;++step){
            auto next=entity.position;next.y+=verticalStep;
            if(next.y<0 || collides(next,bodyWidth,bodyHeight)){entity.velocity.y=0;break;}
            entity.position.y=next.y;
        }
        // Mob::travel applies gravity after move(), so a ground jump travels
        // at its full .42 first and the stored velocity decays for next tick.
        if(!ghast && !swimming)entity.velocity.y=std::max(-3.92,(entity.velocity.y-.08)*.98);
        // Monster::checkHurtTarget uses a 20-tick melee cooldown and requires
        // vertical overlap. Slimes use playerTouch's size-based contact range.
        // Neutral Endermen/Pig Zombies, Skeleton arrows, Creeper fuses, and
        // fireballs wait for their separate goal/projectile ports.
        const bool contactSlime=slime && (entity.id==L"LavaSlime" || entity.slimeSize>1);
        const bool meleeMonster=entity.id==L"Zombie" || entity.id==L"Spider" ||
            entity.id==L"CaveSpider" || entity.id==L"Silverfish" || blaze;
        if(playerAttackable && (contactSlime || meleeMonster) &&
           (contactSlime || entity.attackTicks==0)){
            const double ax=player.x-entity.position.x,ay=player.y-entity.position.y,
                az=player.z-entity.position.z;
            const double reach=contactSlime?.6*entity.slimeSize:
                entity.id==L"Silverfish"?1.2:2.0;
            const bool overlap=player.y+1.8>entity.position.y &&
                player.y<entity.position.y+bodyHeight;
            if(overlap && ax*ax+ay*ay+az*az<reach*reach){
                const Vec3 eye{entity.position.x,entity.position.y+bodyHeight*.85,
                               entity.position.z};
                const Vec3 playerEye{player.x,player.y+1.62,player.z};
                const Vec3 sight{playerEye.x-eye.x,playerEye.y-eye.y,
                                 playerEye.z-eye.z};
                const double sightLength=std::hypot(sight.x,sight.y,sight.z);
                const auto obstacle=raycast(eye,sight,sightLength);
                if(!obstacle.hit || obstacle.distance>=sightLength-.01){
                    if(meleeMonster)entity.attackTicks=20;
                    const bool hurt=applySourcePlayerHurt(state->playerHurt,
                        sourceMobMeleeDamage(entity.id,entity.slimeSize),2,
                        state->metadata->getGameType()==GameType::CREATIVE);
                    if(hurt && entity.id==L"CaveSpider")
                        addPotionEffects(state->playerEffects,{{19,7*20,0}});
                }
            }
        }
    }
    // Living entities are only dropped with their chunk: eviction serializes
    // them first (saveEntities). Dropping them earlier lost spawned mobs.
    state->entities.erase(std::remove_if(state->entities.begin(),state->entities.end(),[&](const auto& entity){
        const int chunkX=entity.native?entity.nativeChunkX:Mth::intFloorDiv(int(std::floor(entity.position.x))-64,16);
        const int chunkZ=entity.native?entity.nativeChunkZ:Mth::intFloorDiv(int(std::floor(entity.position.z))-64,16);
        const double dx=entity.position.x-player.x,dz=entity.position.z-player.z;
        // Mob::checkDespawn removes mobs more than 128 blocks from the player.
        return entity.deathTicks>=20 || !state->region.hasChunk(chunkX,chunkZ) || dx*dx+dz*dz>128*128;
    }),state->entities.end());
}
void World::saveEntities(ChunkRecord& record,bool remove){
    if(!record.extra)record.extra=std::make_unique<CompoundTag>();
    auto list=std::make_unique<TagList>();
    if(auto* existing=dynamic_cast<TagList*>(record.extra->get(L"Entities")))
        for(int i=0;i<existing->size();++i)if(auto* tag=dynamic_cast<CompoundTag*>(existing->get(i)))
            if(tag->getBoolean(L"console_port.simulated") && !tag->getBoolean(L"console_port.active")){
                std::unique_ptr<Tag> copy(tag->copy());list->add(copy.get());copy.release();
            }else if(!tag->getBoolean(L"console_port.simulated") &&
               !tag->getBoolean(L"console_port.defeated") &&
               !tag->getBoolean(L"console_port.pickedUp")){
                std::unique_ptr<Tag> copy(tag->copy());
                auto* nativeCopy=dynamic_cast<CompoundTag*>(copy.get());
                for(const auto& active:state->entities)
                    if(active.native && active.nativeChunkX==record.x &&
                       active.nativeChunkZ==record.z && active.recordIndex==i &&
                       (active.health>0 || active.deathTicks<20)){
                        nativeCopy->putShort(L"Health",active.health);
                        nativeCopy->putShort(L"HurtTime",active.hurtTicks);
                        nativeCopy->putShort(L"DeathTime",active.deathTicks);
                        nativeCopy->putShort(L"AttackTime",active.attackTicks);
                        nativeCopy->putInt(L"console_port.lastHurtByPlayerTicks",
                                           active.lastHurtByPlayerTicks);
                        auto pos=triple(active.position.x-64,active.position.y,active.position.z-64);
                        auto motion=triple(active.velocity.x,active.velocity.y,active.velocity.z);
                        nativeCopy->put(L"Pos",pos.get());pos.release();
                        nativeCopy->put(L"Motion",motion.get());motion.release();
                        auto rotation=std::make_unique<TagList>();
                        for(float value:{active.yaw,0.f}){
                            auto axis=std::make_unique<FloatTag>(L"",value);
                            rotation->add(axis.get());axis.release();
                        }
                        nativeCopy->put(L"Rotation",rotation.get());rotation.release();
                        break;
                    }
                for(const auto& orb:state->experienceOrbs)
                    if(orb.native && orb.nativeChunkX==record.x &&
                       orb.nativeChunkZ==record.z && orb.recordIndex==i && orb.health>0){
                        nativeCopy->putShort(L"Health",orb.health);
                        nativeCopy->putShort(L"Age",orb.age);
                        nativeCopy->putShort(L"Value",orb.value);
                        auto pos=triple(orb.position.x-64,orb.position.y,orb.position.z-64);
                        auto motion=triple(orb.velocity.x,orb.velocity.y,orb.velocity.z);
                        nativeCopy->put(L"Pos",pos.get());pos.release();
                        nativeCopy->put(L"Motion",motion.get());motion.release();
                        break;
                    }
                list->add(copy.get());copy.release();
            }
    for(auto it=state->entities.begin();it!=state->entities.end();){
        if(owned(*it,record)){
            // Imported entities retain their full original NBT above. Until
            // the complete Mob serializer is ported, do not replace those
            // richer records with our small simulated-entity schema.
            if(!it->native && (it->health>0 || it->deathTicks<20)){
                auto tag=std::make_unique<CompoundTag>();
                tag->putString(L"id",it->id);tag->putBoolean(L"console_port.simulated",true);
                tag->putInt(L"Age",animal(it->id)?0:it->age);
                tag->putInt(L"console_port.tick_age",it->age);
                auto pos=triple(it->position.x-64,it->position.y,it->position.z-64);
                auto motion=triple(it->velocity.x,it->velocity.y,it->velocity.z);
                tag->put(L"Pos",pos.get());pos.release();tag->put(L"Motion",motion.get());motion.release();
                auto rotation=std::make_unique<TagList>();
                for(float value:{it->yaw,0.f}){auto axis=std::make_unique<FloatTag>(L"",value);rotation->add(axis.get());axis.release();}
                tag->put(L"Rotation",rotation.get());rotation.release();
                tag->putFloat(L"FallDistance",0);tag->putShort(L"Fire",0);tag->putShort(L"Air",300);
                const auto [width,height]=entitySize(it->id,it->slimeSize);
                tag->putBoolean(L"OnGround",collides({it->position.x,it->position.y-.05,
                                                       it->position.z},width,height));
                tag->putShort(L"Health",it->health);
                if(it->id==L"Sheep"){
                    tag->putBoolean(L"Sheared",it->sheared);
                    tag->putByte(L"Color",static_cast<unsigned char>(it->woolColor));
                }
                if(it->id==L"Villager")tag->putInt(L"Profession",it->profession);
                if(it->id==L"Slime" || it->id==L"LavaSlime")tag->putInt(L"Size",it->slimeSize-1);
                if(it->id==L"Ozelot")tag->putInt(L"CatType",it->catType);
                if(it->id==L"Wolf"){
                    tag->putString(L"Owner",L"");
                    tag->putBoolean(L"Sitting",it->sitting);
                    tag->putBoolean(L"Angry",it->wolfAngry);
                    tag->putByte(L"CollarColor",static_cast<unsigned char>(it->collarColor&15));
                }
                tag->putShort(L"HurtTime",it->hurtTicks);
                tag->putShort(L"DeathTime",it->deathTicks);
                tag->putShort(L"AttackTime",it->attackTicks);
                tag->putInt(L"console_port.lastHurtByPlayerTicks",
                            it->lastHurtByPlayerTicks);
                list->add(tag.get());tag.release();
            }
            if(remove){it=state->entities.erase(it);continue;}
        }
        ++it;
    }
    for(auto it=state->experienceOrbs.begin();it!=state->experienceOrbs.end();){
        if(owned(*it,record)){
            if(!it->native && it->health>0){
                auto tag=std::make_unique<CompoundTag>();
                tag->putString(L"id",L"XPOrb");
                tag->putBoolean(L"console_port.simulated",true);
                tag->putShort(L"Health",it->health);
                tag->putShort(L"Age",it->age);
                tag->putShort(L"Value",it->value);
                tag->putShort(L"console_port.throwTime",it->throwTime);
                auto pos=triple(it->position.x-64,it->position.y,it->position.z-64);
                auto motion=triple(it->velocity.x,it->velocity.y,it->velocity.z);
                tag->put(L"Pos",pos.get());pos.release();
                tag->put(L"Motion",motion.get());motion.release();
                list->add(tag.get());tag.release();
            }
            if(remove){it=state->experienceOrbs.erase(it);continue;}
        }
        ++it;
    }
    // ItemEntity::addAdditonalSaveData: Health, Age and the Item compound.
    for(auto it=state->droppedItems.begin();it!=state->droppedItems.end();){
        const int x=int(std::floor(it->position.x-64)),z=int(std::floor(it->position.z-64));
        if(x>=record.x*16 && x<record.x*16+16 && z>=record.z*16 && z<record.z*16+16){
            if(it->health>0 && it->count>0 && it->stack){
                auto tag=std::make_unique<CompoundTag>();
                tag->putString(L"id",L"Item");
                tag->putBoolean(L"console_port.simulated",true);
                tag->putShort(L"Health",it->health);
                tag->putShort(L"Age",it->age);
                tag->putShort(L"console_port.throwTime",it->throwTime);
                std::unique_ptr<Tag> item(it->stack->copy());
                tag->put(L"Item",item.get());item.release();
                auto pos=triple(it->position.x-64,it->position.y+.125,it->position.z-64);
                auto motion=triple(it->velocity.x,it->velocity.y,it->velocity.z);
                tag->put(L"Pos",pos.get());pos.release();
                tag->put(L"Motion",motion.get());motion.release();
                list->add(tag.get());tag.release();
            }
            if(remove){it=state->droppedItems.erase(it);continue;}
        }
        ++it;
    }
    record.extra->put(L"Entities",list.get());list.release();
    if(remove)state->hangingDecorations.erase(std::remove_if(state->hangingDecorations.begin(),state->hangingDecorations.end(),
        [&](const HangingDecoration& decoration){return decoration.nativeChunkX==record.x && decoration.nativeChunkZ==record.z;}),state->hangingDecorations.end());
}
void World::loadEntities(ChunkRecord& record){
    if(!record.extra)return;
    auto* list=dynamic_cast<TagList*>(record.extra->get(L"Entities"));if(!list)return;
    for(int i=0;i<list->size();++i){
        auto* tag=dynamic_cast<CompoundTag*>(list->get(i));
        if(!tag)continue;
        if(tag->getBoolean(L"console_port.defeated"))continue;
        if(tag->getBoolean(L"console_port.pickedUp"))continue;
        // An activated simulated entry now lives in World state; its record
        // copy is stale and must not be spawned again.
        if(tag->getBoolean(L"console_port.active"))continue;
        const auto id=tag->getString(L"id");
        if(id==L"XPOrb"){
            ExperienceOrbState orb;orb.native=!tag->getBoolean(L"console_port.simulated");
            orb.nativeChunkX=record.x;orb.nativeChunkZ=record.z;orb.recordIndex=i;
            orb.value=tag->getShort(L"Value");orb.age=std::max(0,int(tag->getShort(L"Age")));
            orb.health=tag->contains(L"Health")?int(tag->getShort(L"Health")):5;
            orb.throwTime=std::max(0,int(tag->getShort(L"console_port.throwTime")));
            if(orb.value<1 || orb.age>=6000 || orb.health<=0 ||
               !readTriple(*tag,L"Pos",orb.position) ||
               !readTriple(*tag,L"Motion",orb.velocity))continue;
            orb.position.x+=64;orb.position.z+=64;
            const bool loaded=std::any_of(state->experienceOrbs.begin(),
                state->experienceOrbs.end(),[&](const ExperienceOrbState& current){
                    return current.nativeChunkX==record.x && current.nativeChunkZ==record.z &&
                           current.recordIndex==i;
                });
            if(!loaded && inside(int(std::floor(orb.position.x)),
                                  int(std::floor(orb.position.y)),
                                  int(std::floor(orb.position.z)))){
                if(!orb.native)tag->putBoolean(L"console_port.active",true);
                state->experienceOrbs.push_back(orb);
            }
            continue;
        }
        if(id==L"Item"){
            // Only items this port simulated; imported ItemEntity records stay
            // opaque until the full entity loader is ported.
            if(!tag->getBoolean(L"console_port.simulated"))continue;
            auto* stack=dynamic_cast<CompoundTag*>(tag->get(L"Item"));
            DroppedItem item;
            if(!stack || !readTriple(*tag,L"Pos",item.position) || !readTriple(*tag,L"Motion",item.velocity))continue;
            item.id=stack->getShort(L"id");item.count=static_cast<unsigned char>(stack->getByte(L"Count"));
            item.damage=stack->getShort(L"Damage");
            item.health=tag->contains(L"Health")?int(static_cast<unsigned char>(tag->getShort(L"Health"))):5;
            item.age=std::max(0,int(tag->getShort(L"Age")));
            item.throwTime=std::max(0,int(tag->getShort(L"console_port.throwTime")));
            if(item.id<=0 || item.count<=0 || item.count>64 || item.health<=0 || item.age>=6000)continue;
            item.position.x+=64;item.position.z+=64;item.position.y-=.125;
            if(!inside(int(std::floor(item.position.x)),int(std::floor(item.position.y)),
                       int(std::floor(item.position.z))))continue;
            item.stack=std::shared_ptr<CompoundTag>(static_cast<CompoundTag*>(stack->copy()));
            tag->putBoolean(L"console_port.active",true);
            state->droppedItems.push_back(std::move(item));
            continue;
        }
        if(id==L"Painting" || id==L"ItemFrame"){
            if(!tag->contains(L"TileX") || !tag->contains(L"TileY") || !tag->contains(L"TileZ"))continue;
            HangingDecoration decoration;
            decoration.kind=id==L"Painting"?HangingDecoration::Kind::Painting:HangingDecoration::Kind::ItemFrame;
            decoration.tileX=tag->getInt(L"TileX")+64;
            decoration.tileY=tag->getInt(L"TileY");
            decoration.tileZ=tag->getInt(L"TileZ")+64;
            decoration.nativeChunkX=record.x;decoration.nativeChunkZ=record.z;
            decoration.recordIndex=i;
            if(tag->contains(L"Direction"))decoration.dir=static_cast<unsigned char>(tag->getByte(L"Direction"));
            else if(tag->contains(L"Dir")){
                static constexpr int oldToNew[]{2,1,0,3};
                const int old=static_cast<unsigned char>(tag->getByte(L"Dir"));
                if(old>3)continue;decoration.dir=oldToNew[old];
            }else continue;
            if(decoration.dir>3 || !inside(decoration.tileX,decoration.tileY,decoration.tileZ))continue;
            if(decoration.kind==HangingDecoration::Kind::Painting)decoration.motive=tag->getString(L"Motive");
            else{
                decoration.itemRotation=static_cast<unsigned char>(tag->getByte(L"ItemRotation"))&3;
                if(auto* item=dynamic_cast<CompoundTag*>(tag->get(L"Item"))){
                    decoration.itemId=item->getShort(L"id");
                    decoration.itemDamage=item->getShort(L"Damage");
                }
            }
            const bool loaded=std::any_of(state->hangingDecorations.begin(),state->hangingDecorations.end(),
                [&](const HangingDecoration& existing){return existing.nativeChunkX==record.x &&
                    existing.nativeChunkZ==record.z && existing.recordIndex==i;});
            if(!loaded)state->hangingDecorations.push_back(std::move(decoration));
            continue;
        }
        SimulatedEntity entity;entity.id=tag->getString(L"id");
        entity.age=std::max(0,tag->contains(L"console_port.tick_age")?
            tag->getInt(L"console_port.tick_age"):tag->getInt(L"Age"));
        entity.native=!tag->getBoolean(L"console_port.simulated");
        entity.nativeChunkX=record.x;entity.nativeChunkZ=record.z;entity.recordIndex=i;
        if(auto* rotation=dynamic_cast<TagList*>(tag->get(L"Rotation"));rotation && rotation->size()==2)
            if(auto* yaw=dynamic_cast<FloatTag*>(rotation->get(0));yaw && std::isfinite(yaw->data))entity.yaw=yaw->data;
        if(entity.native && !modeled(entity.id))continue;
        if(entity.id==L"Sheep"){
            entity.sheared=tag->getBoolean(L"Sheared");
            entity.woolColor=static_cast<unsigned char>(tag->getByte(L"Color"));
        }
        if(entity.id==L"Villager")entity.profession=tag->getInt(L"Profession");
        if(entity.id==L"Slime" || entity.id==L"LavaSlime")
            entity.slimeSize=std::clamp(tag->getInt(L"Size")+1,1,4);
        if(entity.id==L"Ozelot")entity.catType=tag->getInt(L"CatType");
        if(entity.id==L"Wolf"){
            entity.wolfTame=!tag->getString(L"Owner").empty() || !tag->getString(L"OwnerUUID").empty();
            entity.wolfAngry=tag->getBoolean(L"Angry");
            entity.sitting=tag->getBoolean(L"Sitting");
            entity.collarColor=tag->contains(L"CollarColor")?
                static_cast<unsigned char>(tag->getByte(L"CollarColor"))&15:14;
        }
        entity.health=tag->contains(L"Health")?std::max(0,int(tag->getShort(L"Health"))):
            initialHealth(entity.id,entity.slimeSize,entity.wolfTame);
        entity.hurtTicks=std::max(0,int(tag->getShort(L"HurtTime")));
        entity.deathTicks=std::max(0,int(tag->getShort(L"DeathTime")));
        entity.attackTicks=std::max(0,int(tag->getShort(L"AttackTime")));
        entity.lastHurtByPlayerTicks=std::max(0,
            tag->getInt(L"console_port.lastHurtByPlayerTicks"));
        if(entity.id.empty() || !readTriple(*tag,L"Pos",entity.position) ||
           !readTriple(*tag,L"Motion",entity.velocity))continue;
        entity.position.x+=64;entity.position.z+=64;
        const bool loaded=std::any_of(state->entities.begin(),state->entities.end(),
            [&](const SimulatedEntity& existing){return existing.nativeChunkX==record.x &&
                existing.nativeChunkZ==record.z && existing.recordIndex==i && existing.id==entity.id;});
        if(!loaded && inside(int(std::floor(entity.position.x)),int(std::floor(entity.position.y)),
                  int(std::floor(entity.position.z))) && owned(entity,record)){
            if(!entity.native)tag->putBoolean(L"console_port.active",true);
            state->entities.push_back(std::move(entity));
        }
    }
}
}
