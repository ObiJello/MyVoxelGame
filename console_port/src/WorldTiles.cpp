// ServerLevel::tickTiles, the update thread's tile selection (ServerLevel::
// runUpdate) and Level::tickWeather over the client world. The tile rules
// themselves are the original methods in ported/tick/TileTickRules.cpp.
#include "WorldState.h"
#include "BlockShape.h"
#include "TileTickHost.h"
#include "ConsoleLightmap.h"
#include "SurvivalRules.h"
#include "CombatRules.h"
#include "Biome.h"
#include "GenerationDecorator.h"
#include "CompoundTag.h"
#include <algorithm>
#include <array>
#include <map>
#include <functional>
#include <cmath>
#include <memory>

namespace console {
// Entity::setSize widths and heights (WorldEntities.cpp).
std::pair<double,double> entitySizeOf(const std::wstring& id,int slimeSize);
namespace {
constexpr int TICKS_PER_DAY=24000;
// ServerLevel::MAX_UPDATES, Level::MAX_GRASS_TICKS / MAX_LAVA_TICKS.
constexpr int MAX_UPDATES=256,MAX_GRASS_TICKS=100,MAX_LAVA_TICKS=100;

// Biome::hasSnow / hasRain: setNoRain() biomes (desert, hell, sky, desert
// hills) never rain; the rest snow below temperature 0.15.
bool biomeRains(int id){return id!=2 && id!=8 && id!=9 && id!=17;}
bool biomeHasSnow(int id){return biomeRains(id) && Biome::biomes[id] && Biome::biomes[id]->getTemperature()<0.15f;}
bool biomeHasRain(int id){return !biomeHasSnow(id) && biomeRains(id);}
// Level::hasChunksAt over the resident region (level coordinates).
bool regionHasChunksAt(const GenerationRegion& region,int x0,int y0,int z0,int x1,int y1,int z1){
    if(y1<0 || y0>=World::height)return false;
    for(int cx=Mth::intFloorDiv(x0,16);cx<=Mth::intFloorDiv(x1,16);++cx)
        for(int cz=Mth::intFloorDiv(z0,16);cz<=Mth::intFloorDiv(z1,16);++cz)
            if(!region.hasChunk(cx,cz))return false;
    return true;
}
// ServerLevel::MAX_FALLING_TILE.
constexpr std::size_t MAX_FALLING_TILE=20;
// randValue = randValue * 3 + addend, with the console's int wrap.
int nextRand(int value,int addend){return static_cast<int>(static_cast<unsigned>(value)*3u+static_cast<unsigned>(addend));}
std::unique_ptr<CompoundTag> dropStack(int id,int count,int damage){
    auto tag=std::make_unique<CompoundTag>();
    tag->putShort(L"id",id);tag->putByte(L"Count",count);tag->putShort(L"Damage",damage);
    return tag;
}
}

// The player as the source's mobs see it (Level::players): its position,
// health and held item come from the World before each Level; being hurt
// goes to the World's player (Player::hurt's creative rule and the Normal
// difficulty scaling are CombatRules'), being moved to the client
// (World::takePlayerPush), knockback (xd/yd/zd) to World::takePlayerKnockback.
struct WorldPlayer final:sim::Player {
    PlayerHurtState* hurtState=nullptr;
    Vec3* push=nullptr;
    Vec3* position=nullptr;
    bool survival=true;
    float getHeadHeight()override{return .12f;}
    bool hurt(sim::DamageSource*,int damage)override{
        return hurtState && applySourcePlayerHurt(*hurtState,damage,2,!survival);
    }
    // Riding is not ported: a saddled pig does not carry the player yet.
    void ride(std::shared_ptr<sim::Entity>)override{}
    void move(double dx,double dy,double dz,bool)override{
        if(!push || !position)return;
        push->x+=dx;push->y+=dy;push->z+=dz;
        position->x+=dx;position->y+=dy;position->z+=dz;
    }
};
// The mobs the port runs from the source, by saved id.
std::shared_ptr<sim::Mob> newSourceMob(const std::wstring& id,sim::Level* level){
    if(id==L"Pig")return std::make_shared<sim::Pig>(level);
    if(id==L"Cow")return std::make_shared<sim::Cow>(level);
    if(id==L"Sheep")return std::make_shared<sim::Sheep>(level);
    if(id==L"Chicken")return std::make_shared<sim::Chicken>(level);
    return nullptr;
}
std::wstring sourceMobId(sim::Mob& mob){
    switch(mob.GetType()){
    case sim::eTYPE_PIG:return L"Pig";
    case sim::eTYPE_COW:return L"Cow";
    case sim::eTYPE_SHEEP:return L"Sheep";
    case sim::eTYPE_CHICKEN:return L"Chicken";
    default:return L"";
    }
}
bool sourceMobKind(const std::wstring& id){return id==L"Pig" || id==L"Cow" || id==L"Sheep" || id==L"Chicken";}
// The live sim::Level over the client world, in level coordinates (client
// coordinates less half the window, which keeps chunks aligned). The region is
// read wherever a resident chunk exists (the source reads loaded neighbours),
// and written only inside the visible window.
class WorldTickLevel final:public sim::Level {
    static constexpr int half=World::width/2;
    World& world_;
    World::State& s_;
    sim::Dimension overworld_;
    sim::CurrentLevel current_{this};
    sim::Level* previous_=nullptr;
    int skyDarken_=0;
    bool resident(int x,int z)const{return s_.region.hasChunk(Mth::intFloorDiv(x,16),Mth::intFloorDiv(z,16));}
    int raw(int x,int y,int z,bool propagate,int dampen){
        if(propagate){
            switch(getTile(x,y,z)){
            case sim::Tile::stoneSlabHalf_Id:case sim::Tile::woodSlabHalf_Id:case sim::Tile::farmland_Id:
            case sim::Tile::stairs_stone_Id:case sim::Tile::stairs_wood_Id:{
                int br=raw(x,y+1,z,false,dampen);
                br=std::max(br,raw(x+1,y,z,false,dampen));br=std::max(br,raw(x-1,y,z,false,dampen));
                br=std::max(br,raw(x,y,z+1,false,dampen));br=std::max(br,raw(x,y,z-1,false,dampen));
                return br;
            }
            default:break;
            }
        }
        if(y<0)return 0;
        if(y>=maxBuildHeight)y=maxBuildHeight-1;
        if(!resident(x,z))return 0; // EmptyLevelChunk
        // LevelChunk::getRawBrightness
        int light=s_.region.getLight(LightLayer::Sky,x,y,z)-dampen;
        return std::max(light,s_.region.getLight(LightLayer::Block,x,y,z));
    }
    // Entity::move's vertical clip for a falling tile's box (the source
    // ignores cubes it already overlaps): the furthest the feet can go.
    double clipFall(const Vec3& position,double dy)const{
        const auto at=[&](double d){return world_.collides({position.x,position.y-.49+d,position.z},.98,.98);};
        if(dy>=0 || at(0) || !at(dy))return dy;
        double free=0,blocked=dy;
        for(int i=0;i<40;++i){const double mid=(free+blocked)/2;(at(mid)?blocked:free)=mid;}
        return free;
    }
public:
    WorldTickLevel(World& world,World::State& state):world_(world),s_(state){
        sim::initializeTiles();
        random=&s_.tickRandom;
        dimension=&overworld_;
        chunkSourceXZSize=s_.metadata->getXZSize();
        identity=&s_;
        previous_=current_.previous;
        for(auto& te:s_.tickingTileEntities)te->level=this;
        skyDarken_=consoleOldSkyDarken(world.dayTime(),world.rainLevel(),world.thunderLevel());
        // The player and the source's mobs, pointed at this Level.
        if(!s_.simPlayer)s_.simPlayer=std::make_shared<WorldPlayer>();
        auto& player=static_cast<WorldPlayer&>(*s_.simPlayer);
        player.hurtState=&s_.playerHurt;player.push=&s_.playerPush;player.position=&s_.playerPosition;
        player.survival=world.survival();
        player.level=this;
        const Vec3 feet=s_.playerPosition;
        player.setPos(feet.x-half,feet.y+player.heightOffset,feet.z-half);
        player.xd=player.yd=player.zd=0;
        player.setHealth(s_.playerHurt.health);
        player.abilities.instabuild=player.abilities.invulnerable=!world.survival();
        const auto carried=world.carriedItems();
        const int held=std::clamp(s_.heldSlot,0,8);
        player.inventory->selected=0;
        player.inventory->items[0]=carried[held].id?std::make_shared<sim::ItemInstance>(carried[held].id,carried[held].count,carried[held].damage):nullptr;
        if(s_.hasPlayerPosition && s_.playerHurt.health>0)players.push_back(s_.simPlayer);
        for(auto& e:s_.entities)if(e.ai){entities.push_back(e.ai);e.ai->setLevel(this);}
    }
    int getTile(int x,int y,int z)override{
        if(y<0 || y>=maxBuildHeight || !resident(x,z))return 0;
        return s_.region.getTile(x,y,z);
    }
    int getData(int x,int y,int z)override{
        if(y<0 || y>=maxBuildHeight || !resident(x,z))return 0;
        return s_.region.getData(x,y,z);
    }
    // LevelChunk::setTileAndData: store, then the replaced tile's onRemove
    // and the new tile's onPlace.
    bool setTileAndDataNoUpdate(int x,int y,int z,int tile,int data)override{
        if(y<0 || y>=maxBuildHeight || !world_.inside(x+half,y,z+half) || !validBlock(static_cast<std::uint8_t>(tile)))return false;
        const int old=getTile(x,y,z),oldData=getData(x,y,z);
        if(old==tile && oldData==data)return false;
        world_.set(x+half,y,z+half,static_cast<Block>(tile));
        world_.setData(x+half,y,z+half,data);
        placed(x,y,z,old,oldData,tile);
        return true;
    }
    // The chunk hooks of a change already stored.
    void placed(int x,int y,int z,int old,int oldData,int tile){
        if(old!=0 && sim::Tile::tiles[old])sim::Tile::tiles[old]->onRemove(this,x,y,z,old,oldData);
        if(tile!=0 && sim::Tile::tiles[tile])sim::Tile::tiles[tile]->onPlace(this,x,y,z);
    }
    bool setDataNoUpdate(int x,int y,int z,int data)override{
        return world_.inside(x+half,y,z+half) && world_.setData(x+half,y,z+half,data);
    }
    // Chunk coordinates are the same in level and client space less 4.
    bool hasChunk(int chunkX,int chunkZ)override{return s_.region.hasChunk(chunkX,chunkZ);}
    int getRawBrightness(int x,int y,int z)override{return raw(x,y,z,true,skyDarken_);}
    int getDaytimeRawBrightness(int x,int y,int z)override{return raw(x,y,z,false,0);}
    int getBrightness(LightLayer::variety layer,int x,int y,int z)override{
        if(y<0)y=0;
        if(y>=maxBuildHeight)y=maxBuildHeight-1;
        if(!resident(x,z))return int(layer);
        return s_.region.getLight(layer,x,y,z);
    }
    // LevelChunk::isSkyLit: at or above the heightmap.
    bool canSeeSky(int x,int y,int z)override{return resident(x,z) && y>=s_.region.getHeightmap(x,z);}
    int biome(int x,int z){
        if(!resident(x,z))return 1;
        const auto& chunk=*s_.region.chunks.at({Mth::intFloorDiv(x,16),Mth::intFloorDiv(z,16)});
        return chunk.biomes[(z&15)*16+(x&15)];
    }
    int topRainBlock(int x,int z){return resident(x,z)?s_.lightLevel->getTopRainBlock(x,z):0;}
    // Level::isRaining / isThundering: the rain level above 0.2, the thunder
    // level above 0.9.
    bool isRaining()override{return world_.rainLevel()>.2f;}
    bool isThundering(){return world_.thunderLevel()>.9f;}
    bool isRainingAt(int x,int y,int z)override{
        if(!isRaining())return false;
        if(!canSeeSky(x,y,z))return false;
        if(topRainBlock(x,z)>y)return false;
        const int id=biome(x,z);
        if(biomeHasSnow(id))return false;
        return biomeHasRain(id);
    }
    std::int64_t getTime()override{return world_.time();}
    // The World's entities as stand-ins for Level::getEntities and
    // getEntitiesOfClass: the player (a Player), the mobs (Mobs), dropped
    // items (ItemEntity) and experience orbs (ExperienceOrb). Each stand-in's
    // move() moves the real one and hurt() hurts it; its push (explosion
    // knockback) is applied when the stand-in goes with this Level. Made once
    // per Level.
    template<class Base> struct Proxy final:Base {
        std::function<void(double,double,double)> moveFn,pushFn;
        std::function<bool(int)> hurtFn;
        float head=0;
        template<class... A> explicit Proxy(A&&... args):Base(std::forward<A>(args)...){}
        void move(double dx,double dy,double dz,bool)override{moveFn(dx,dy,dz);}
        bool hurt(sim::DamageSource*,int damage)override{return hurtFn && hurtFn(damage);}
        float getHeadHeight()override{return head;}
        ~Proxy()override{if(pushFn && (this->xd!=0 || this->yd!=0 || this->zd!=0))pushFn(this->xd,this->yd,this->zd);}
    };
    struct ProxyMob:sim::Mob {
        ProxyMob():Mob(nullptr){}
        int getMaxHealth()override{return 20;}
    };
    std::vector<std::shared_ptr<sim::Entity>> hostEntities_;
    bool hostEntitiesMade_=false;
    void hostEntities(std::vector<std::shared_ptr<sim::Entity>>& out)override{
        if(!hostEntitiesMade_){makeHostEntities();hostEntitiesMade_=true;}
        out.insert(out.end(),hostEntities_.begin(),hostEntities_.end());
    }
    void makeHostEntities(){
        // Entity position (feet, or the player's eye line: Player's y is its
        // feet plus heightOffset), box, head height, and what moving, hurting
        // and pushing it does to the real one.
        const auto place=[&](auto entity,const Vec3& feet,double width,double height,float yOffset,float head,
                             std::function<void(double,double,double)> moveFn,std::function<bool(int)> hurtFn,
                             std::function<void(double,double,double)> pushFn){
            entity->x=feet.x-half;entity->y=feet.y+yOffset;entity->z=feet.z-half;entity->heightOffset=yOffset;
            entity->xd=entity->yd=entity->zd=0;
            entity->bbWidth=float(width);entity->bbHeight=float(height);
            entity->bb->set(entity->x-width/2,feet.y,entity->z-width/2,entity->x+width/2,feet.y+height,entity->z+width/2);
            entity->head=head;
            entity->moveFn=std::move(moveFn);entity->hurtFn=std::move(hurtFn);entity->pushFn=std::move(pushFn);
            hostEntities_.push_back(entity);
        };
        const auto shift=[](Vec3& v,double dx,double dy,double dz){v.x+=dx;v.y+=dy;v.z+=dz;};
        if(s_.hasPlayerPosition && s_.playerHurt.health>0)hostEntities_.push_back(s_.simPlayer);
        for(std::size_t i=0;i<s_.entities.size();++i){
            const auto& entity=s_.entities[i];
            if(entity.health<=0 || entity.ai)continue;
            const auto [width,height]=entitySizeOf(entity.id,entity.slimeSize);
            place(std::make_shared<Proxy<ProxyMob>>(),entity.position,width,height,0,float(height)*.85f,
                [this,i,shift](double dx,double dy,double dz){shift(s_.entities[i].position,dx,dy,dz);},
                [this,i](int damage){
                    // Mob::hurt: the invulnerability window keeps only a larger hit's excess.
                    auto& target=s_.entities[i];
                    if(damage<=0 || target.health<=0)return false;
                    if(target.invulnerableTicks>10){
                        if(damage<=target.lastHurt)return false;
                        target.health-=damage-target.lastHurt;
                    }else{target.health-=damage;target.invulnerableTicks=20;target.hurtTicks=10;}
                    target.lastHurt=damage;
                    return true;
                },
                [this,i,shift](double dx,double dy,double dz){shift(s_.entities[i].velocity,dx,dy,dz);});
        }
        for(std::size_t i=0;i<s_.droppedItems.size();++i){
            auto stack=std::make_shared<sim::ItemInstance>(s_.droppedItems[i].id,s_.droppedItems[i].count,s_.droppedItems[i].damage);
            place(std::make_shared<Proxy<sim::ItemEntity>>(this,0.0,0.0,0.0,stack),s_.droppedItems[i].position,.25,.25,.125f,0,
                [this,i,shift](double dx,double dy,double dz){shift(s_.droppedItems[i].position,dx,dy,dz);},
                // ItemEntity::hurt
                [this,i](int damage){s_.droppedItems[i].health-=damage;return true;},
                [this,i,shift](double dx,double dy,double dz){shift(s_.droppedItems[i].velocity,dx,dy,dz);});
        }
        for(std::size_t i=0;i<s_.experienceOrbs.size();++i)
            place(std::make_shared<Proxy<sim::ExperienceOrb>>(this,0.0,0.0,0.0,s_.experienceOrbs[i].value),
                s_.experienceOrbs[i].position,.5,.5,.25f,0,
                [this,i,shift](double dx,double dy,double dz){shift(s_.experienceOrbs[i].position,dx,dy,dz);},
                // ExperienceOrb::hurt
                [this,i](int damage){s_.experienceOrbs[i].health-=damage;return true;},
                [this,i,shift](double dx,double dy,double dz){shift(s_.experienceOrbs[i].velocity,dx,dy,dz);});
    }
    // Level::clip between two points (level coordinates): the port's raycast.
    sim::HitResult* clip(::Vec3* a,::Vec3* b)override{
        const Vec3 from{a->x+half,a->y,a->z+half},dir{b->x-a->x,b->y-a->y,b->z-a->z};
        const double length=std::sqrt(dir.x*dir.x+dir.y*dir.y+dir.z*dir.z);
        if(length<1e-9)return nullptr;
        const auto hit=world_.raycast(from,dir,length);
        return hit.hit?new sim::HitResult():nullptr;
    }
    // ServerLevel::newPrimedTntAllowed (MAX_PRIMED_TNT 20) and addEntity.
    bool newPrimedTntAllowed()override{return s_.primedTnt.size()<20;}
    void addEntity(std::shared_ptr<sim::PrimedTnt> e)override{
        s_.primedTnt.push_back({{e->x+half,e->y,e->z+half},{e->xd,e->yd,e->zd},e->life});
    }
    // ServerLevel::explode: the explosion without particles, block
    // destruction included.
    void explode(double x,double y,double z,float r){
        sim::Explosion explosion(this,nullptr,x,y,z,r);
        explosion.explode();
        explosion.finalizeExplosion(false);
        // The stand-ins go now, applying their pushes.
        hostEntities_.clear();hostEntitiesMade_=false;entityChunks.clear();es.clear();
    }
    // Entity::move for a box: each axis (y, then x, then z) as far as it goes
    // before touching a block the box does not already overlap.
    double clipMove(Vec3& feet,int axis,double delta,double width,double height)const{
        const auto at=[&](double d){
            Vec3 p=feet;(axis==0?p.x:axis==1?p.y:p.z)+=d;
            return world_.collides(p,width,height);
        };
        if(delta==0 || at(0) || !at(delta)){(axis==0?feet.x:axis==1?feet.y:feet.z)+=delta;return delta;}
        double free=0,blocked=delta;
        for(int i=0;i<40;++i){const double mid=(free+blocked)/2;(at(mid)?blocked:free)=mid;}
        (axis==0?feet.x:axis==1?feet.y:feet.z)+=free;
        return free;
    }
    // Tile::addAABBs for the tiles whose collision is not ported: the World's
    // collision shapes (World::collides: solid tiles are cubes, partial
    // tiles their shape, top snow half a block from three layers). Piston
    // heads and moving pieces use the shape their extracted code set.
    void addTileAABBs(sim::Tile* tile,int x,int y,int z,AABB* box,sim::AABBList* boxes,std::shared_ptr<sim::Entity> source)override{
        if(tile->id==sim::Tile::pistonExtensionPiece_Id || tile->id==sim::Tile::pistonMovingPiece_Id){
            sim::Level::addTileAABBs(tile,x,y,z,box,boxes,source);
            return;
        }
        const auto add=[&](double x0,double y0,double z0,double x1,double y1,double z1){
            AABB* shape=AABB::newTemp(x+x0,y+y0,z+z0,x+x1,y+y1,z+z1);
            if(!box || shape->intersects(box))boxes->push_back(shape);
        };
        const int id=tile->id;
        if(id==78){if((getData(x,y,z)&7)>=3)add(0,0,0,1,.5,1);return;}
        if(!solid(static_cast<Block>(id)))return;
        if(!consoleIsPartialBlock(id)){add(0,0,0,1,1,1);return;}
        struct Access final:BlockShapeAccess {
            WorldTickLevel& level;
            explicit Access(WorldTickLevel& l):level(l){}
            int getTile(int x,int y,int z)const override{return level.getTile(x-half,y,z-half);}
            int getData(int x,int y,int z)const override{return level.getData(x-half,y,z-half);}
        } access(*this);
        const auto shape=consoleCollisionShape(access,x+half,y,z+half);
        for(int i=0;i<shape.count;++i){const auto& b=shape.boxes[i];add(b.x0,b.y0,b.z0,b.x1,b.y1,b.z1);}
    }
    // Level::getTileEntity / setTileEntity / removeTileEntity for the tile
    // entities the port keeps here (piston pieces), with LevelChunk's rule
    // that only an EntityTile holds one.
    std::shared_ptr<sim::TileEntity> getTileEntity(int x,int y,int z)override{
        // A dispenser's entity outlives its tile until removeTileEntity (as
        // LevelChunk's map does), so DispenserTile::onRemove still finds it.
        if(world_.inside(x+half,y,z+half) && (traps_.contains({x,y,z}) ||
           getTile(x,y,z)==sim::Tile::dispenser_Id || s_.trap(x+half,y,z+half)))return trap(x,y,z);
        auto it=s_.tileEntities.find({x,y,z});
        return it==s_.tileEntities.end() || it->second->isRemoved()?nullptr:it->second;
    }
    void setTileEntity(int x,int y,int z,std::shared_ptr<sim::TileEntity> te)override{
        if(!te || te->isRemoved())return;
        s_.tickingTileEntities.push_back(te);
        te->level=this;te->x=x;te->y=y;te->z=z;
        const int tile=getTile(x,y,z);
        if(tile==0 || !sim::Tile::tiles[tile] || !sim::Tile::tiles[tile]->isEntityTile())return;
        if(auto it=s_.tileEntities.find({x,y,z});it!=s_.tileEntities.end() && it->second!=te)it->second->setRemoved();
        te->clearRemoved();
        s_.tileEntities[{x,y,z}]=te;
    }
    void removeTileEntity(int x,int y,int z)override{
        if(auto it=s_.tileEntities.find({x,y,z});it!=s_.tileEntities.end()){
            it->second->setRemoved();s_.tileEntities.erase(it);
        }
        if(auto it=traps_.find({x,y,z});it!=traps_.end()){it->second->setRemoved();traps_.erase(it);}
        if(world_.inside(x+half,y,z+half))world_.discardContainerData(x+half,y,z+half,L"Trap");
    }
    // DispenserTileEntity for the extracted DispenserTile: loaded from the
    // chunk's "Trap" tag when first asked for, saved back (as
    // DispenserTileEntity::save) when this Level goes.
    std::map<std::array<int,3>,std::shared_ptr<sim::DispenserTileEntity>> traps_;
    std::shared_ptr<sim::DispenserTileEntity> trap(int x,int y,int z){
        const std::array<int,3> key{x,y,z};
        if(auto it=traps_.find(key);it!=traps_.end())return it->second;
        auto te=std::make_shared<sim::DispenserTileEntity>();
        te->level=this;te->x=x;te->y=y;te->z=z;
        if(auto* tag=s_.trap(x+half,y,z+half))
            if(auto* list=dynamic_cast<TagList*>(tag->get(L"Items")))
                for(int i=0;i<list->size();++i)if(auto* stack=dynamic_cast<CompoundTag*>(list->get(i))){
                    const int slot=static_cast<unsigned char>(stack->getByte(L"Slot"));
                    const int id=stack->getShort(L"id"),count=static_cast<unsigned char>(stack->getByte(L"Count"));
                    if(slot>=9 || id<=0 || count<=0)continue;
                    auto item=std::make_shared<sim::ItemInstance>(id,count,stack->getShort(L"Damage"));
                    if(auto* extra=dynamic_cast<CompoundTag*>(stack->get(L"tag")))
                        item->tag.reset(static_cast<CompoundTag*>(extra->copy()));
                    (*te->items)[slot]=item;
                }
        traps_[key]=te;
        return te;
    }
    static std::unique_ptr<CompoundTag> stackOf(const sim::ItemInstance& item){
        auto stack=dropStack(item.id,item.count,item.auxValue);
        if(item.tag)stack->put(L"tag",item.tag->copy());
        return stack;
    }
    void saveTrap(const std::array<int,3>& at,sim::DispenserTileEntity& te){
        const int x=at[0]+half,y=at[1],z=at[2]+half;
        if(!world_.inside(x,y,z))return;
        world_.ensureTrapData(x,y,z);
        auto* tag=s_.trap(x,y,z);if(!tag)return;
        auto list=std::make_unique<TagList>();
        for(int i=0;i<te.items->length;++i){
            const auto& item=(*te.items)[i];
            if(!item || item->id<=0 || item->count<=0)continue;
            auto stack=stackOf(*item);stack->putByte(L"Slot",i);
            list->add(stack.get());stack.release();
        }
        tag->put(L"Items",list.get());list.release();
        s_.chunk(x,z).unsaved=true;
    }
    // Level::addEntity for what a dispenser makes: an item entity (thrown
    // items, dropped contents) or a spawn egg's mob.
    void addEntity(std::shared_ptr<sim::Entity> e)override{
        // Items go into the World when this Level does: the source sets their
        // motion after adding them (Sheep::interact, DispenserTile::throwItem).
        if(auto item=std::dynamic_pointer_cast<sim::ItemEntity>(e))pendingItems_.push_back(item);
        else if(auto mob=std::dynamic_pointer_cast<sim::EggMob>(e))
            world_.spawnCreativeEgg(mob->entityId,{e->x+half,e->y,e->z+half});
        else if(auto orb=std::dynamic_pointer_cast<sim::ExperienceOrb>(e))
            world_.spawnExperienceOrbs({e->x+half,e->y,e->z+half},orb->value);
        else if(auto animal=std::dynamic_pointer_cast<sim::Mob>(e);animal && !sourceMobId(*animal).empty()){
            // A mob the source made (a calf, a chick): the World keeps it.
            SimulatedEntity entity;entity.id=sourceMobId(*animal);
            entity.ai=animal;
            s_.entities.push_back(std::move(entity));
            mirrorMob(s_.entities.back());
            entities.push_back(animal);
            ++world_.revision;
        }
    }
    // The World's record of a source mob after it has run.
    void mirrorMob(SimulatedEntity& entity){
        auto& mob=*entity.ai;
        entity.position={mob.x+half,mob.y,mob.z+half};
        entity.velocity={mob.xd,mob.yd,mob.zd};
        entity.yaw=mob.yBodyRot;entity.headYaw=mob.yHeadRot;
        entity.health=std::max(0,mob.getHealth());
        entity.hurtTicks=mob.hurtTime;entity.deathTicks=mob.deathTime;
        entity.invulnerableTicks=mob.invulnerableTime;
        if(auto* agable=dynamic_cast<sim::AgableMob*>(&mob)){entity.animalAge=agable->getAge();entity.baby=agable->isBaby();}
        if(auto* sheep=dynamic_cast<sim::Sheep*>(&mob)){entity.sheared=sheep->isSheared();entity.woolColor=sheep->getColor();}
        if(auto* pig=dynamic_cast<sim::Pig*>(&mob))entity.saddled=pig->hasSaddle();
    }
    // A source mob for a World record: the record's position, facing and
    // motion, and its saved fields (Mob, AgableMob, Animal, Sheep, Pig
    // readAdditionalSaveData) or the ones the World set.
    void adopt(SimulatedEntity& entity){
        auto mob=newSourceMob(entity.id,this);
        if(!mob)return;
        mob->moveTo(entity.position.x-half,entity.position.y,entity.position.z-half,entity.yaw,0);
        mob->yBodyRot=mob->yHeadRot=entity.yaw;
        mob->xd=entity.velocity.x;mob->yd=entity.velocity.y;mob->zd=entity.velocity.z;
        if(entity.saved)mob->readAdditionalSaveData(entity.saved.get());
        else{
            mob->setHealth(entity.health);
            if(auto* agable=dynamic_cast<sim::AgableMob*>(mob.get()))agable->setAge(entity.animalAge);
            if(auto* sheep=dynamic_cast<sim::Sheep*>(mob.get())){sheep->setSheared(entity.sheared);sheep->setColor(entity.woolColor);}
            if(auto* pig=dynamic_cast<sim::Pig*>(mob.get()))pig->setSaddle(entity.saddled);
        }
        entity.saved.reset();
        entity.ai=mob;
        entities.push_back(mob);
    }
public:
    // Level::tickEntities for the source's mobs in the window, and the
    // World's records after them. A mob that is removed has finished dying
    // (Mob::tickDeath) or despawned (Mob::checkDespawn).
    void tickSourceMobs(){
        for(auto& entity:s_.entities)if(!entity.ai && sourceMobKind(entity.id) && entity.health>0)adopt(entity);
        const auto running=entities;
        for(const auto& e:running){
            if(e->removed)continue;
            if(!world_.inside(Mth::floor(e->x+half),Mth::floor(e->y),Mth::floor(e->z+half)))continue;
            e->tick();
        }
        for(std::size_t i=0;i<s_.entities.size();++i){
            auto& entity=s_.entities[i];
            if(!entity.ai)continue;
            mirrorMob(entity);
            // The record's tick count drives the drawn gait (as the other mobs').
            ++entity.age;
            if(entity.lastHurtByPlayerTicks>0)--entity.lastHurtByPlayerTicks;
            if(entity.ai->removed){
                if(entity.native)world_.markNativeDefeated(entity);
                entity.deathTicks=std::max(entity.deathTicks,20);
                entity.health=std::min(entity.health,0);
            }
        }
        ++world_.revision;
    }
    // Player::attack: the hit as the player's (knockback, panic).
    bool attackSourceMob(SimulatedEntity& entity,int damage){
        if(!entity.ai)adopt(entity);
        if(!entity.ai)return false;
        // Player::attack: the damage source is the caller's to delete.
        sim::DamageSource* source=sim::DamageSource::playerAttack(s_.simPlayer);
        const bool hurt=entity.ai->hurt(source,damage);
        delete source;
        mirrorMob(entity);
        // The record keeps Mob::lastHurtByPlayerTime (PLAYER_HURT_EXPERIENCE_TIME),
        // which the source does not save, so a death across a save still gives XP.
        if(hurt)entity.lastHurtByPlayerTicks=sim::Mob::PLAYER_HURT_EXPERIENCE_TIME;
        return hurt;
    }
    // Mob::interact with the player's held stack; the stack's changes, items
    // added to the inventory and dropped go back to the World.
    bool interactSourceMob(SimulatedEntity& entity){
        if(!entity.ai)adopt(entity);
        if(!entity.ai)return false;
        auto& player=*s_.simPlayer;
        const auto before=player.inventory->items[0];
        const int beforeId=before?before->id:0,beforeCount=before?before->count:0,beforeDamage=before?before->damage:0;
        const auto items=pendingItems_.size();
        // Sheep::interact shears and still answers Animal::interact's false:
        // what it did counts, not only its answer.
        bool used=s_.simPlayer->interact(entity.ai);
        mirrorMob(entity);
        const int slot=std::clamp(s_.heldSlot,0,8);
        const auto after=player.inventory->items[0];
        used|=pendingItems_.size()!=items || after!=before || (after && (after->count!=beforeCount || after->damage!=beforeDamage));
        for(unsigned int i=1;i<player.inventory->items.length;++i)used|=bool(player.inventory->items[i]);
        used|=!player.dropped.empty();
        if(!used)return false;
        if(!after || after->count<=0 || after->id!=beforeId){
            if(beforeCount>0)world_.consumeCarried(slot,beforeCount);
            if(after && after->count>0)world_.addCarriedItem(after->id,after->count,after->auxValue);
        }else{
            if(after->count<beforeCount)world_.consumeCarried(slot,beforeCount-after->count);
            if(after->damage>beforeDamage)world_.wearCarried(slot,after->damage-beforeDamage);
        }
        for(unsigned int i=1;i<player.inventory->items.length;++i)
            if(auto& extra=player.inventory->items[i]){
                if(extra->count>0)world_.addCarriedItem(extra->id,extra->count,extra->auxValue);
                extra=nullptr;
            }
        for(const auto& dropped:player.dropped)
            world_.spawnDroppedItem({player.x+half,player.y-.3,player.z+half},{0,.1,0},dropStack(dropped->id,dropped->count,dropped->auxValue),40);
        player.dropped.clear();
        return true;
    }
    bool canSpawnEgg(int entityId)override{return world_.eggSpawnable(entityId);}
    // Level::countInstanceOf, with the entities the port does not run yet
    // (thrown things, minecarts, boats) counted at their limit, so the
    // dispenser keeps them rather than making one.
    int countInstanceOf(sim::eINSTANCEOF clas,bool singleType)override{
        if(clas==sim::eTYPE_PROJECTILE || clas==sim::eTYPE_SMALL_FIREBALL || clas==sim::eTYPE_MINECART || clas==sim::eTYPE_BOAT)return 1<<30;
        return sim::Level::countInstanceOf(clas,singleType);
    }
    // Level::levelEvent: the dispenser's clicks, launches and smoke (sound and
    // particles are not ported), kept for the tests.
    void levelEvent(int type,int x,int y,int z,int)override{
        if(s_.levelEvents.size()>=64)s_.levelEvents.erase(s_.levelEvents.begin());
        s_.levelEvents.push_back({type,x+half,y,z+half});
    }
    // ServerLevel::tileEvent: queued once per identical event, run by
    // World::runTileEvents.
    void tileEvent(int x,int y,int z,int tile,int b0,int b1)override{
        auto& list=s_.tileEvents[s_.activeTileEvents];
        const World::State::TileEvent event{x,y,z,tile,b0,b1};
        if(std::find(list.begin(),list.end(),event)==list.end())list.push_back(event);
    }
    std::vector<std::shared_ptr<sim::ItemEntity>> pendingItems_;
    void spawnPendingItems(){
        for(const auto& item:pendingItems_)if(item->item && item->item->count>0)
            world_.spawnDroppedItem({item->x+half,item->y,item->z+half},{item->xd,item->yd,item->zd},
                                    stackOf(*item->item),item->throwTime);
        pendingItems_.clear();
    }
    ~WorldTickLevel()override{
        spawnPendingItems();
        for(auto& [at,te]:traps_)
            // Items change in place too (the buckets), so every one goes back.
            if(!te->isRemoved() && getTile(at[0],at[1],at[2])==sim::Tile::dispenser_Id)saveTrap(at,*te);
        // The tile entities hold their level: hand them back to the level
        // that was current when this one was made.
        for(auto& te:s_.tickingTileEntities)if(te->level==this)te->level=previous_;
        for(auto& e:s_.entities)if(e.ai && e.ai->level==this)e.ai->setLevel(previous_);
        if(auto& player=*s_.simPlayer;player.level==this){
            if(player.xd!=0 || player.yd!=0 || player.zd!=0){
                s_.playerKnockback.x+=player.xd;s_.playerKnockback.y+=player.yd;s_.playerKnockback.z+=player.zd;
                player.xd=player.yd=player.zd=0;
            }
            player.level=previous_;
        }
    }
    // Biome::isHumid: downfall above 0.85.
    bool isHumidAt(int x,int,int z)override{
        const auto* b=Biome::biomes[biome(x,z)];
        return b && b->downfall>.85f;
    }
    bool hasChunksAt(int x0,int y0,int z0,int x1,int y1,int z1)override{
        return regionHasChunksAt(s_.region,x0,y0,z0,x1,y1,z1);
    }
    void addToTickNextTick(int x,int y,int z,int tile,int delay)override{
        world_.tileTicks().addToTickNextTick(x,y,z,tile,delay);
    }
    // ServerLevel::newFallingTileAllowed and addEntity(FallingTile).
    bool newFallingTileAllowed()override{return s_.fallingBlocks.size()<MAX_FALLING_TILE;}
    void addEntity(std::shared_ptr<sim::FallingTile> e)override{
        FallingBlock block;
        block.position={e->x+half,e->y,e->z+half};
        block.tile=e->tile;block.data=e->data;
        s_.fallingBlocks.push_back(block);
    }
    // PlayerList::isTrackingTile: the chunk is in the player's view.
    bool isTrackingTile(int x,int y,int z)override{return world_.inside(x+half,y,z+half);}
    // Tile::spawnResources -> popResource: each drop at a random point inside
    // the tile. Below odds 1 (an explosion) every item is its own roll
    // (level->random->nextFloat() > odds skips it) and pops alone.
    void spawnResources(int x,int y,int z,int tile,int data,float odds)override{
        if(!world_.inside(x+half,y,z+half))return;
        const auto pop=[&](int id,int count,int damage){
            const double xo=random->nextFloat()*.7+.15,yo=random->nextFloat()*.7+.15,zo=random->nextFloat()*.7+.15;
            world_.spawnDroppedItem({x+half+xo,y+yo,z+half+zo},{random->nextFloat()*.2-.1,.2,random->nextFloat()*.2-.1},
                                    dropStack(id,count,damage),10);
        };
        for(const auto& drop:consoleTileDrops(tile,data,0,*random)){
            if(odds>=1){pop(drop.id,drop.count,drop.damage);continue;}
            for(int i=0;i<drop.count;++i)if(!(random->nextFloat()>odds))pop(drop.id,1,drop.damage);
        }
    }
    bool placeTree(TreeKind kind,int height,Random& treeRandom,int x,int y,int z)override{
        // The feature writes through the world's light Level, which relights.
        const bool placed=placeSaplingTree(*s_.lightLevel,static_cast<int>(kind),height,treeRandom,x,y,z);
        ++world_.revision;
        for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx)
            s_.undecorated.erase({Mth::intFloorDiv(x,16)+dx,Mth::intFloorDiv(z,16)+dz});
        return placed;
    }
    // FallingTile::tick; false once the entity is removed.
    bool tickFalling(FallingBlock& block);
    // PrimedTnt::tick; false once it has exploded.
    bool tickPrimed(World::State::Primed& tnt);
    // ServerLevel::tickTiles and the update thread.
    void tickTiles();
    // Level::tickClientSideTiles without the cave sound itself.
    void tickClientSideTiles(int xo,int zo);
};

bool WorldTickLevel::tickFalling(FallingBlock& block){
    if(block.tile==0)return false;
    ++block.time;
    auto& p=block.position;
    const int xt=Mth::floor(p.x)-half,zt=Mth::floor(p.z)-half;
    // time 1 takes the tile out of the world (before the move here: the
    // source's move ignores the tile's own cube, which it overlaps).
    if(block.time==1){
        if(getTile(xt,Mth::floor(p.y),zt)==block.tile)setTile(xt,Mth::floor(p.y),zt,0);
        else return false;
    }
    block.velocity.y-=0.04f;
    const double dy=clipFall(p,block.velocity.y);
    const bool onGround=dy!=block.velocity.y && block.velocity.y<0;
    p.y+=dy;
    if(dy!=block.velocity.y)block.velocity.y=0;
    block.velocity.x*=0.98f;block.velocity.y*=0.98f;block.velocity.z*=0.98f;
    const int yt=Mth::floor(p.y);
    if(onGround){
        block.velocity.x*=0.7f;block.velocity.z*=0.7f;block.velocity.y*=-0.5f;
        if(getTile(xt,yt,zt)!=sim::Tile::pistonMovingPiece_Id){
            if(mayPlace(block.tile,xt,yt,zt,true,1,nullptr) && !sim::HeavyTile::isFree(this,xt,yt-1,zt) &&
               setTileAndData(xt,yt,zt,block.tile,block.data)){
                if(auto* heavy=dynamic_cast<sim::HeavyTile*>(sim::Tile::tiles[block.tile]))
                    heavy->onLand(this,xt,yt,zt,block.data);
            }else{
                // spawnAtLocation(ItemInstance(tile, 1, getSpawnResourcesAuxValue(data))):
                // AnvilTile keeps its damage (data >> 2).
                const int aux=block.tile==sim::Tile::anvil_Id?block.data>>2:0;
                world_.spawnDroppedItem(p,{random->nextFloat()*.2-.1,.2,random->nextFloat()*.2-.1},dropStack(block.tile,1,aux),10);
            }
            return false;
        }
    }else if((block.time>20*5 && (yt<1 || yt>maxBuildHeight)) || block.time>20*30){
        world_.spawnDroppedItem(p,{random->nextFloat()*.2-.1,.2,random->nextFloat()*.2-.1},dropStack(block.tile,1,0),10);
        return false;
    }
    return true;
}

bool WorldTickLevel::tickPrimed(World::State::Primed& tnt){
    // The entity position is the box centre (heightOffset bbHeight / 2).
    Vec3 feet{tnt.position.x,tnt.position.y-.49,tnt.position.z};
    tnt.velocity.y-=0.04f;
    const double wanted=tnt.velocity.y;
    const double dy=clipMove(feet,1,tnt.velocity.y,.98,.98);
    const double dx=clipMove(feet,0,tnt.velocity.x,.98,.98);
    const double dz=clipMove(feet,2,tnt.velocity.z,.98,.98);
    const bool onGround=dy!=wanted && wanted<0;
    if(dy!=wanted)tnt.velocity.y=0;
    if(dx!=tnt.velocity.x)tnt.velocity.x=0;
    if(dz!=tnt.velocity.z)tnt.velocity.z=0;
    tnt.position={feet.x,feet.y+.49,feet.z};
    tnt.velocity.x*=0.98f;tnt.velocity.y*=0.98f;tnt.velocity.z*=0.98f;
    if(onGround){tnt.velocity.x*=0.7f;tnt.velocity.z*=0.7f;tnt.velocity.y*=-0.5f;}
    if(tnt.life--<=0){
        // PrimedTnt::explode: radius 4.
        explode(tnt.position.x-half,tnt.position.y,tnt.position.z-half,4.0f);
        return false;
    }
    return true;
}

void WorldTickLevel::tickClientSideTiles(int xo,int zo){
    if(s_.delayUntilNextMoodSound==0){
        s_.randValue=nextRand(s_.randValue,s_.addend);
        const int val=s_.randValue>>2;
        int x=val&15,z=(val>>8)&15;
        const int y=(val>>16)&genDepthMinusOne;
        const int id=getTile(xo+x,y,zo+z);
        x+=xo;z+=zo;
        if(id==0 && getDaytimeRawBrightness(x,y,z)<=random->nextInt(8) && getBrightness(LightLayer::Sky,x,y,z)<=0){
            // getNearestPlayer within 8 and more than 2 away: the ambient
            // cave sound (audio is not ported) and a new delay.
            const auto& player=s_.playerPosition;
            const double dx=player.x-half-(x+.5),dy=player.y-(y+.5),dz=player.z-half-(z+.5);
            const double d2=dx*dx+dy*dy+dz*dz;
            if(s_.hasPlayerPosition && d2<=8*8 && d2>2*2)
                s_.delayUntilNextMoodSound=random->nextInt(20*60*10)+20*60*5;
        }
    }
}

void WorldTickLevel::tickTiles(){
    // The tiles the update thread chose after the previous tick.
    for(const auto& [x,y,z]:s_.updateTiles){
        if(!hasChunksAt(x,y,z,x,y,z))continue;
        auto* tile=sim::Tile::tiles[getTile(x,y,z)];
        if(tile && tile->isTicking())tile->tick(this,x,y,z,random);
    }
    s_.updateTiles.clear();

    // Level::buildAndPrepareChunksToPoll: the player's chunk, then rings out
    // to 9 (the console's interleaved order), in insertion order. Only chunks
    // inside the visible window tick.
    std::vector<std::pair<int,int>> poll;
    auto add=[&](int cx,int cz){
        const int left=cx*16+half,north=cz*16+half;
        if(left<world_.originX() || left+16>world_.originX()+World::width ||
           north<world_.originZ() || north+16>world_.originZ()+World::depth)return;
        if(std::find(poll.begin(),poll.end(),std::pair{cx,cz})==poll.end())poll.push_back({cx,cz});
    };
    if(s_.hasPlayerPosition){
        const int px=Mth::floor((s_.playerPosition.x-half)/16),pz=Mth::floor((s_.playerPosition.z-half)/16);
        add(px,pz);
        for(int r=1;r<=9;++r)for(int l=0;l<r*2;++l){
            add(px-r+l,pz-r);add(px+r,pz-r+l);add(px+r-l,pz+r);add(px-r,pz+r-l);
        }
    }
    if(s_.delayUntilNextMoodSound>0)--s_.delayUntilNextMoodSound;

    const int prob=100000;
    for(const auto& [cx,cz]:poll){
        const int xo=cx*16,zo=cz*16;
        tickClientSideTiles(xo,zo);
        if(random->nextInt(prob)==0 && isRaining() && isThundering()){
            s_.randValue=nextRand(s_.randValue,s_.addend);
            const int val=s_.randValue>>2;
            const int x=xo+(val&15),z=zo+((val>>8)&15);
            const int y=topRainBlock(x,z);
            // LightningBolt: the flash. The bolt entity (fire, damage) is not ported.
            if(isRainingAt(x,y,z))s_.lightningTime=2;
        }
        if(random->nextInt(16)==0){
            s_.randValue=nextRand(s_.randValue,s_.addend);
            const int val=s_.randValue>>2;
            const int x=val&15,z=(val>>8)&15;
            const int yy=topRainBlock(x+xo,z+zo);
            if(s_.lightLevel->shouldFreeze(x+xo,yy-1,z+zo))setTile(x+xo,yy-1,z+zo,sim::Tile::ice_Id);
            if(isRaining() && s_.lightLevel->shouldSnow(x+xo,yy,z+zo))setTile(x+xo,yy,z+zo,sim::Tile::topSnow_Id);
            if(isRaining() && biomeHasRain(biome(x+xo,z+zo))){
                const int tile=getTile(x+xo,yy-1,z+zo);
                if(tile!=0 && sim::Tile::tiles[tile])sim::Tile::tiles[tile]->handleRain(this,x+xo,yy-1,z+zo);
            }
        }
        // checkLight: the port relights on every change; keep the draws.
        random->nextInt(16);random->nextInt(128);random->nextInt(16);
    }

    // ServerLevel::runUpdate, run here after the tick instead of on a thread:
    // 80 samples per polled chunk from a copy of randValue, kept away from
    // edges without a loaded neighbour, grass and lava limited, 256 at most.
    int threadRand=s_.randValue,grassTicks=0,lavaTicks=0;
    for(const auto& [cx,cz]:poll){
        auto has=[&](int dx,int dz){return s_.region.hasChunk(cx+dx,cz+dz);};
        int minx=0,maxx=15,minz=0,maxz=15;
        if(!has(0,0))continue;
        if(!has(1,0))maxx=11;
        if(!has(0,1))maxz=11;
        if(!has(-1,0))minx=4;
        if(!has(0,-1))minz=4;
        if(!has(1,1)){maxx=11;maxz=11;}
        if(!has(1,-1)){maxx=11;minz=4;}
        if(!has(-1,-1)){minx=4;minz=4;}
        if(!has(-1,1)){minx=4;maxz=11;}
        for(int j=0;j<80;++j){
            threadRand=nextRand(threadRand,s_.addend);
            const int val=threadRand>>2;
            const int x=val&15;
            if(x<minx || x>maxx)continue;
            const int z=(val>>8)&15;
            if(z<minz || z>maxz)continue;
            const int y=(val>>16)&(maxBuildHeight-1);
            const int id=getTile(cx*16+x,y,cz*16+z);
            if(int(s_.updateTiles.size())>=MAX_UPDATES)break;
            if((id==sim::Tile::grass_Id && grassTicks>=MAX_GRASS_TICKS) ||
               (id==sim::Tile::calmLava_Id && lavaTicks>=MAX_LAVA_TICKS))continue;
            auto* tile=sim::Tile::tiles[id];
            if(tile && tile->isTicking() && tile->shouldTileTick(this,cx*16+x,y,cz*16+z)){
                if(id==sim::Tile::grass_Id)++grassTicks;
                else if(id==sim::Tile::calmLava_Id)++lavaTicks;
                s_.updateTiles.push_back({cx*16+x,y,cz*16+z});
            }
        }
    }
}

void World::tickTiles(){
    if(state->pending || !state->hasPlayerPosition)return;
    if(state->lightDirty)state->ensureLighting(seed);
    if(state->delayUntilNextMoodSound<0)state->delayUntilNextMoodSound=state->tickRandom.nextInt(20*60*10);
    WorldTickLevel level(*this,*state);
    level.tickTiles();
}

World::State::~State(){
    // NotGateTile::removeLevelReferences for this world's torch history.
    auto& toggles=sim::NotGateTile::recentToggles;
    if(auto it=toggles.find(sim::NotGateTile::LevelKey(static_cast<const void*>(this)));it!=toggles.end()){
        delete it->second;toggles.erase(it);
    }
}

void World::tickFallingBlocks(){
    if(state->pending || state->fallingBlocks.empty())return;
    WorldTickLevel level(*this,*state);
    auto& blocks=state->fallingBlocks;
    for(std::size_t i=0;i<blocks.size();){
        // Entities outside the window wait (their chunks are not ticked).
        const auto& p=blocks[i].position;
        if(!inside(Mth::floor(p.x),std::clamp(Mth::floor(p.y),0,height-1),Mth::floor(p.z))){++i;continue;}
        FallingBlock block=blocks[i];
        const bool alive=level.tickFalling(block);
        if(alive)blocks[i++]=block;
        else blocks.erase(blocks.begin()+std::ptrdiff_t(i));
    }
}
const std::vector<FallingBlock>& World::fallingBlocks()const{return state->fallingBlocks;}

void World::tickPrimedTnt(){
    if(state->pending || state->primedTnt.empty())return;
    WorldTickLevel level(*this,*state);
    auto& all=state->primedTnt;
    for(std::size_t i=0;i<all.size();){
        auto tnt=all[i];
        if(level.tickPrimed(tnt)){all[i++]=tnt;}
        else all.erase(all.begin()+std::ptrdiff_t(i));
    }
}
std::vector<PrimedTntState> World::primedTnt()const{
    std::vector<PrimedTntState> result;
    for(const auto& tnt:state->primedTnt)result.push_back({tnt.position,tnt.life});
    return result;
}
std::vector<std::array<int,4>> World::takeLevelEvents(){
    auto events=std::move(state->levelEvents);
    state->levelEvents.clear();
    return events;
}
Vec3 World::takePlayerKnockback(){
    const Vec3 knockback=state->playerKnockback;
    state->playerKnockback={};
    return knockback;
}

bool World::useItemOn(int x,int y,int z,int face,int slot){
    if(state->pending || slot<0 || slot>=36 || !inside(x,y,z))return false;
    const auto item=carriedItems()[slot];
    if(!item.id)return false;
    sim::ItemInstance instance;
    instance.id=item.id;instance.count=item.count;instance.auxValue=item.damage;
    WorldTickLevel level(*this,*state);
    // ServerPlayerGameMode::useItemOn: the tile's own use first (TNT and
    // flint and steel, which is not worn by it), then the item's.
    if(auto* tile=sim::Tile::tiles[get(x,y,z)]){
        auto player=std::make_shared<sim::Player>();
        player->inventory->items[player->inventory->selected]=std::make_shared<sim::ItemInstance>(instance);
        if(tile->use(&level,x-width/2,y,z-depth/2,player,face,0,0,0))return true;
    }
    if(!sim::useItemOn(level,instance,x-width/2,y,z-depth/2,face))return false;
    // ServerPlayerGameMode::useItemOn: creative keeps the stack.
    if(instance.count<item.count)consumeCarried(slot,item.count-instance.count);
    if(instance.damage>0)wearCarried(slot,instance.damage);
    return true;
}

// Edits are allowed while chunks stream (the region stays valid), as the raw
// set/setData are; only the ticks wait.
bool World::setTileAndUpdate(int x,int y,int z,Block tile,int data){
    if(!inside(x,y,z))return false;
    WorldTickLevel level(*this,*state);
    return level.setTileAndData(x-width/2,y,z-depth/2,tile,data);
}

bool World::setDataAndUpdate(int x,int y,int z,int data){
    if(!inside(x,y,z))return false;
    WorldTickLevel level(*this,*state);
    const bool changed=level.setDataNoUpdate(x-width/2,y,z-depth/2,data);
    if(changed)level.tileUpdated(x-width/2,y,z-depth/2,get(x,y,z));
    return changed;
}

void World::tileStored(int x,int y,int z,int oldTile,int oldData){
    if(!inside(x,y,z))return;
    WorldTickLevel level(*this,*state);
    const int tile=get(x,y,z);
    level.placed(x-width/2,y,z-depth/2,oldTile,oldData,tile);
    level.tileUpdated(x-width/2,y,z-depth/2,tile);
}

int World::placementData(int x,int y,int z,int tile,int face,int data){
    if(!inside(x,y,z))return -1;
    WorldTickLevel level(*this,*state);
    const int lx=x-width/2,lz=z-depth/2;
    if(!sim::Tile::tiles[tile] || !level.mayPlace(tile,lx,y,lz,false,face,nullptr))return -1;
    return sim::Tile::tiles[tile]->getPlacedOnFaceDataValue(&level,lx,y,lz,face,0,0,0,data);
}

namespace {
// Mob::yRot in the source's degrees from the port's heading (as the furnace
// and ender chest placement read it).
float sourceYRot(double yaw){
    constexpr double pi=3.14159265358979323846;
    return static_cast<float>(std::remainder(yaw,2*pi)*180/pi+180);
}
}

bool World::usable(int x,int y,int z)const{
    if(!inside(x,y,z))return false;
    sim::initializeTiles();
    auto* tile=sim::Tile::tiles[get(x,y,z)];
    return tile && tile->TestUse();
}

bool World::useBlock(int x,int y,int z,double yaw){
    if(!usable(x,y,z))return false;
    WorldTickLevel level(*this,*state);
    auto player=std::make_shared<sim::Player>();
    player->yRot=sourceYRot(yaw);
    sim::Tile::tiles[get(x,y,z)]->use(&level,x-width/2,y,z-depth/2,player,0,0,0,0);
    return true;
}

void World::tilePlacedBy(int x,int y,int z,double yaw,Vec3 feet){
    if(!inside(x,y,z))return;
    WorldTickLevel level(*this,*state);
    // The player: Entity y is the feet plus heightOffset.
    auto by=std::make_shared<sim::Player>();
    by->yRot=sourceYRot(yaw);
    by->x=feet.x-width/2;by->y=feet.y+by->heightOffset;by->z=feet.z-depth/2;
    if(auto* tile=sim::Tile::tiles[get(x,y,z)])tile->setPlacedBy(&level,x-width/2,y,z-depth/2,by);
}

void World::tileDestroyed(int x,int y,int z,int tile,int data){
    if(!inside(x,y,z))return;
    WorldTickLevel level(*this,*state);
    if(auto* t=sim::Tile::tiles[tile])t->destroy(&level,x-width/2,y,z-depth/2,data);
}

void World::tickInsideTiles(){
    if(state->pending)return;
    WorldTickLevel level(*this,*state);
    // Entity::checkInsideTiles: the tiles in the entity's box shrunk by 0.001.
    const auto checkInside=[&](const Vec3& feet,double boxWidth,double boxHeight){
        const int ax=Mth::floor(feet.x-boxWidth/2+.001),bx=Mth::floor(feet.x+boxWidth/2-.001);
        const int ay=Mth::floor(feet.y+.001),by=Mth::floor(feet.y+boxHeight-.001);
        const int az=Mth::floor(feet.z-boxWidth/2+.001),bz=Mth::floor(feet.z+boxWidth/2-.001);
        for(int x=ax;x<=bx;++x)for(int y=ay;y<=by;++y)for(int z=az;z<=bz;++z){
            if(!inside(x,y,z))continue;
            if(auto* tile=sim::Tile::tiles[get(x,y,z)])
                tile->entityInside(&level,x-World::width/2,y,z-World::depth/2,std::make_shared<sim::Entity>());
        }
    };
    if(state->hasPlayerPosition && state->playerHurt.health>0)checkInside(state->playerPosition,.6,1.8);
    for(std::size_t i=0;i<state->entities.size();++i){
        const auto entity=state->entities[i];
        if(entity.health<=0)continue;
        const auto [boxWidth,boxHeight]=entitySizeOf(entity.id,entity.slimeSize);
        checkInside(entity.position,boxWidth,boxHeight);
    }
    for(std::size_t i=0;i<state->droppedItems.size();++i)checkInside(state->droppedItems[i].position,.25,.25);
    for(std::size_t i=0;i<state->experienceOrbs.size();++i)checkInside(state->experienceOrbs[i].position,.5,.5);
}

void World::runTileEvents(){
    if(state->pending)return;
    WorldTickLevel level(*this,*state);
    // ServerLevel::runTileEvents: swap lists until both are empty; doTileEvent
    // runs Tile::triggerEvent when the tile is still there.
    while(!state->tileEvents[state->activeTileEvents].empty()){
        const int run=state->activeTileEvents;
        state->activeTileEvents^=1;
        const auto events=std::move(state->tileEvents[run]);
        state->tileEvents[run].clear();
        for(const auto& event:events){
            const int tile=level.getTile(event.x,event.y,event.z);
            if(tile==event.tile && tile>0 && sim::Tile::tiles[tile])
                sim::Tile::tiles[tile]->triggerEvent(&level,event.x,event.y,event.z,event.paramA,event.paramB);
        }
    }
}

void World::tickTileEntities(){
    if(state->pending || state->tickingTileEntities.empty())return;
    WorldTickLevel level(*this,*state);
    // Level::tickEntities: tick the list as it stands (pieces made meanwhile
    // wait for the next tick), then drop the removed ones.
    const auto ticking=state->tickingTileEntities;
    for(const auto& te:ticking)if(!te->isRemoved())te->tick();
    auto& list=state->tickingTileEntities;
    list.erase(std::remove_if(list.begin(),list.end(),[](const auto& te){return te->isRemoved();}),list.end());
}

void World::finishPistons(){
    if(state->tickingTileEntities.empty())return;
    WorldTickLevel level(*this,*state);
    const auto ticking=state->tickingTileEntities;
    for(const auto& te:ticking)
        if(auto piece=std::dynamic_pointer_cast<sim::PistonPieceEntity>(te))piece->finalTick();
    auto& list=state->tickingTileEntities;
    list.erase(std::remove_if(list.begin(),list.end(),[](const auto& te){return te->isRemoved();}),list.end());
}

std::vector<MovingPiece> World::movingPieces()const{
    std::vector<MovingPiece> pieces;
    for(const auto& [position,te]:state->tileEntities){
        auto piece=std::dynamic_pointer_cast<sim::PistonPieceEntity>(te);
        if(!piece || piece->isRemoved())continue;
        const auto [x,y,z]=position;
        pieces.push_back({x+width/2,y,z+depth/2,piece->getId(),piece->getData(),piece->isExtending(),
                          piece->isSourcePiston(),piece->getProgress(1),piece->getXOff(1),piece->getYOff(1),piece->getZOff(1)});
    }
    return pieces;
}

Vec3 World::takePlayerPush(){
    const Vec3 push=state->playerPush;
    state->playerPush={};
    return push;
}

ScheduledTickQueue& World::tileTicks(){
    state->tileTickHost.world=this;
    return state->tileTicks;
}

// ServerLevel's pending tile ticks over the region, in level coordinates.
std::int64_t World::State::TileTickHost::getTime()const{return state.metadata->getTime();}
void World::State::TileTickHost::setTime(std::int64_t time)noexcept{state.metadata->setTime(time);}
bool World::State::TileTickHost::hasChunksAt(int x0,int y0,int z0,int x1,int y1,int z1){
    return regionHasChunksAt(state.region,x0,y0,z0,x1,y1,z1);
}
int World::State::TileTickHost::getTile(int x,int y,int z){
    if(y<0 || y>=height || !state.region.hasChunk(Mth::intFloorDiv(x,16),Mth::intFloorDiv(z,16)))return 0;
    return state.region.getTile(x,y,z);
}
void World::State::TileTickHost::tickTile(int id,int x,int y,int z){
    if(!sim::tickPorted(id))return;
    // A chunk in the streaming halo is loaded but not ticked: the tick waits
    // for the window (its writes would be refused outside it).
    if(!world->inside(x+width/2,y,z+depth/2)){state.tileTicks.forceAddTileTick(x,y,z,id,20);return;}
    WorldTickLevel level(*world,state);
    sim::Tile::tiles[id]->tick(&level,x,y,z,level.random);
}

// Level::prepareWeather and Level::tickWeather.
void World::tickWeather(){
    auto& data=*state->metadata;
    auto& random=state->tickRandom;
    if(!state->weatherPrepared){
        state->weatherPrepared=true;
        if(data.isRaining()){state->rainLevel=1;if(data.isThundering())state->thunderLevel=1;}
    }
    if(state->lightningTime>0)--state->lightningTime;

    int thunderTime=data.getThunderTime();
    if(thunderTime<=0){
        if(data.isThundering())data.setThunderTime(random.nextInt(20*60*10)+20*60*3);
        else data.setThunderTime(random.nextInt(TICKS_PER_DAY*7)+TICKS_PER_DAY/2);
    }else{
        --thunderTime;
        data.setThunderTime(thunderTime);
        if(thunderTime<=0)data.setThundering(!data.isThundering());
    }

    int rainTime=data.getRainTime();
    if(rainTime<=0){
        if(data.isRaining())data.setRainTime(random.nextInt(TICKS_PER_DAY/2)+TICKS_PER_DAY/2);
        else data.setRainTime(random.nextInt(TICKS_PER_DAY*7)+TICKS_PER_DAY/2);
    }else{
        --rainTime;
        data.setRainTime(rainTime);
        if(rainTime<=0)data.setRaining(!data.isRaining());
    }

    state->oRainLevel=state->rainLevel;
    state->rainLevel+=data.isRaining()?0.01f:-0.01f;
    state->rainLevel=std::clamp(state->rainLevel,0.f,1.f);
    state->oThunderLevel=state->thunderLevel;
    state->thunderLevel+=data.isThundering()?0.01f:-0.01f;
    state->thunderLevel=std::clamp(state->thunderLevel,0.f,1.f);
}
}

namespace console {
void World::tickSourceMobs(){
    if(state->pending)return;
    WorldTickLevel level(*this,*state);
    level.tickSourceMobs();
}
void World::markNativeDefeated(const SimulatedEntity& entity){
    if(auto record=state->records.find({entity.nativeChunkX,entity.nativeChunkZ});
       record!=state->records.end() && record->second->extra)
        if(auto* list=dynamic_cast<TagList*>(record->second->extra->get(L"Entities"));
           list && entity.recordIndex>=0 && entity.recordIndex<list->size())
            if(auto* tag=dynamic_cast<CompoundTag*>(list->get(entity.recordIndex)))
                tag->putBoolean(L"console_port.defeated",true);
}
bool World::hurtSourceMob(SimulatedEntity& entity,int damage){
    if(state->pending)return false;
    WorldTickLevel level(*this,*state);
    return level.attackSourceMob(entity,damage);
}
void World::setHeldSlot(int slot){state->heldSlot=std::clamp(slot,0,8);}
bool World::useEntity(Vec3 eye,Vec3 direction,int slot,double reach){
    setHeldSlot(slot);
    SimulatedEntity* target=pickLiving(eye,direction,reach);
    if(!target || !(target->ai || sourceMobKind(target->id)))return false;
    WorldTickLevel level(*this,*state);
    const bool used=level.interactSourceMob(*target);
    if(used)++revision;
    return used;
}
}
