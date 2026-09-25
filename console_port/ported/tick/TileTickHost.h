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
#include "Mth.h"
#include "Random.h"
#include "Vec3.h"
#include "AABB.h"
#include "CompoundTag.h"
#include "FoodConstants.h"
#include "BasicTypeContainers.h"
#include <cassert>
#include <cstring>
#include <map>
#include <cmath>
#include <cstdint>
#include <array>
#include <deque>
#include <functional>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>

// The source's iterator shorthand (stdafx.h).
#ifndef AUTO_VAR
#define AUTO_VAR(_var, _val) auto _var = _val
#endif

namespace console::sim {
using ::Mth;
using std::deque;
using std::vector;
using ::Direction;
using ::Facing;
using ::LightLayer;
using ::Material;
using ::Random;
using ::Vec3;
using ::AABB;
using ::CompoundTag;
using ::FoodConstants;
using std::shared_ptr;

#include "TileConstants.inc"

// The pieces of Tile::SoundType, Player, ItemInstance and Entity the methods use.
struct SoundType {
    int getStepSound()const{return 0;}
    float getVolume()const{return 1;}
    float getPitch()const{return 1;}
};
// The Win32 thread-local slots the source uses (TlsAlloc/TlsGetValue/TlsSetValue);
// slot 0 is Tile's shape storage.
using DWORD=unsigned;
using LPVOID=void*;
inline DWORD TlsAlloc(){static DWORD next=1;return next++;}
inline void*& tlsSlot(DWORD index){static thread_local void* slots[16]{};return slots[index%16];}
inline void TlsSetValue(DWORD index,void* value){tlsSlot(index)=value;}
struct Abilities { bool instabuild=false,invulnerable=false,flying=false; };
using AABBList=std::vector<AABB*>;
typedef unsigned char byte;
#define PI (3.141592654f)
#include "ClassTypes.inc"
#include "SoundTypes.inc"
#include "ParticleTypes.inc"
// SharedConstants.h.
struct SharedConstants { static const int TICKS_PER_SECOND=20; static const bool TEXTURE_LIGHTING=true; };
// ChatPacket's death-message ids (DamageSource keeps one; chat is not ported).
struct ChatPacket {
#include "ChatMessages.inc"
};
class Entity;
class Mob;
class Player;
class Arrow;
class Fireball;
class Level;
class ItemEntity;
class ItemInstance;
// DamageSource.h, EntityDamageSource.h, IndirectEntityDamageSource.h (the
// methods are the source's, in EntityRules.cpp; death messages are not ported).
class DamageSource {
public:
    static DamageSource *inFire,*onFire,*lava,*inWall,*drown,*starve,*cactus,*fall,*outOfWorld,*genericSource,
        *explosion,*controlledExplosion,*magic,*dragonbreath,*wither,*anvil,*fallingBlock;
    static DamageSource *mobAttack(shared_ptr<Mob> mob);
    static DamageSource *playerAttack(shared_ptr<Player> player);
    static DamageSource *arrow(shared_ptr<Arrow> arrow,shared_ptr<Entity> owner);
    static DamageSource *thrown(shared_ptr<Entity> entity,shared_ptr<Entity> owner);
    static DamageSource *indirectMagic(shared_ptr<Entity> entity,shared_ptr<Entity> owner);
    static DamageSource *thorns(shared_ptr<Entity> source);
private:
    // The constructor leaves _scalesWithDifficulty unset; false here.
    bool _bypassArmor=false,_bypassInvul=false;
    float exhaustion=0;
    bool isFireSource=false,_isProjectile=false,_scalesWithDifficulty=false,_isMagic=false;
public:
    bool isProjectile();
    DamageSource *setProjectile();
    bool isBypassArmor();
    float getFoodExhaustion();
    bool isBypassInvul();
    ChatPacket::EChatPacketMessage m_msgId;
protected:
    DamageSource(ChatPacket::EChatPacketMessage msgId);
public:
    virtual ~DamageSource(){}
    virtual shared_ptr<Entity> getDirectEntity();
    virtual shared_ptr<Entity> getEntity();
protected:
    DamageSource *bypassArmor();
    DamageSource *bypassInvul();
    DamageSource *setIsFire();
    DamageSource *setScalesWithDifficulty();
public:
    virtual bool scalesWithDifficulty();
    bool isMagic();
    DamageSource *setMagic();
    bool isFire();
    ChatPacket::EChatPacketMessage getMsgId();
};
class EntityDamageSource:public DamageSource {
protected:
    shared_ptr<Entity> entity;
public:
    EntityDamageSource(ChatPacket::EChatPacketMessage msgId,shared_ptr<Entity> entity);
    virtual ~EntityDamageSource(){}
    shared_ptr<Entity> getEntity()override;
    virtual bool scalesWithDifficulty()override;
};
class IndirectEntityDamageSource:public EntityDamageSource {
    shared_ptr<Entity> owner;
public:
    IndirectEntityDamageSource(ChatPacket::EChatPacketMessage msgId,shared_ptr<Entity> entity,shared_ptr<Entity> owner);
    virtual ~IndirectEntityDamageSource(){}
    shared_ptr<Entity> getDirectEntity()override;
    shared_ptr<Entity> getEntity()override;
};
// SynchedEntityData: the values an entity keeps in step with clients. One
// process here, so a typed map.
class SynchedEntityData {
    std::map<int,int> ints;
    std::map<int,std::wstring> strings;
    std::map<int,shared_ptr<ItemInstance>> items;
public:
    void define(int id,int value){ints[id]=value;}
    void define(int id,byte value){ints[id]=value;}
    void define(int id,short value){ints[id]=value;}
    void define(int id,const std::wstring& value){strings[id]=value;}
    void defineNULL(int id,void*){items[id]=nullptr;}
    byte getByte(int id){return static_cast<byte>(ints[id]);}
    short getShort(int id){return static_cast<short>(ints[id]);}
    int getInteger(int id){return ints[id];}
    std::wstring getString(int id){return strings[id];}
    shared_ptr<ItemInstance> getItemInstance(int id){return items[id];}
    void set(int id,int value){ints[id]=value;}
    void set(int id,byte value){ints[id]=value;}
    void set(int id,short value){ints[id]=value;}
    void set(int id,const std::wstring& value){strings[id]=value;}
    void set(int id,shared_ptr<ItemInstance> value){items[id]=value;}
    void markDirty(int){}
};
// ProtectionEnchantment::getFireAfterDampener: armour enchantments are not
// ported, so the fire time is unchanged.
struct ProtectionEnchantment { static int getFireAfterDampener(shared_ptr<Entity>,int value){return value;} };
// Entity.h: the fields and methods of the source's Entity (EntityRules.cpp
// has the methods). The pure virtuals have neutral defaults so the host's
// small stand-ins (falling tiles, primed TNT, items) can be made directly.
class Entity:public std::enable_shared_from_this<Entity> {
public:
    virtual eINSTANCEOF GetType(){return eTYPE_ENTITY;}
    static const short TOTAL_AIR_SUPPLY=20*15;
    static inline int entityCounter=2048;
    // Entity::tlsIdx: the server thread's small-id flag (not set: plain ids).
    static inline DWORD tlsIdx=TlsAlloc();
    int entityId=0;
    double viewScale=1;
    bool blocksBuilding=false;
    std::weak_ptr<Entity> rider;
    shared_ptr<Entity> riding;
    Level* level=nullptr;
    double xo=0,yo=0,zo=0;
    double x=0,y=0,z=0;
    double xd=0,yd=0,zd=0;
    float yRot=0,xRot=0;
    float yRotO=0,xRotO=0;
    AABB* bb=nullptr;
    bool onGround=false;
    bool horizontalCollision=false,verticalCollision=false;
    bool collision=false;
    bool hurtMarked=false;
protected:
    bool isStuckInWeb=false;
public:
    bool slide=true;
    bool removed=false;
    float heightOffset=0;
    float bbWidth=0.6f;
    float bbHeight=1.8f;
    float walkDistO=0;
    float walkDist=0;
    float fallDistance=0;
private:
    int nextStep=1;
public:
    double xOld=0,yOld=0,zOld=0;
    float ySlideOffset=0;
    float footSize=0;
    bool noPhysics=false;
    float pushthrough=0;
protected:
    Random* random=nullptr;
public:
    int tickCount=0;
    int flameTime=1;
private:
    int onFire=0;
protected:
    bool wasInWater=false;
public:
    int invulnerableTime=0;
private:
    bool firstTick=true;
public:
    std::wstring customTextureUrl,customTextureUrl2;
protected:
    bool fireImmune=false;
    shared_ptr<SynchedEntityData> entityData;
private:
    static const int DATA_SHARED_FLAGS_ID=0;
    static const int FLAG_ONFIRE=0,FLAG_SNEAKING=1,FLAG_RIDING=2,FLAG_SPRINTING=3,FLAG_USING_ITEM=4,
        FLAG_INVISIBLE=5,FLAG_IDLEANIM=6,FLAG_EFFECT_WEAKENED=7;
    static const int DATA_AIR_SUPPLY_ID=1;
    double xRideRotA=0,yRideRotA=0;
public:
    bool inChunk=false;
    int xChunk=0,yChunk=0,zChunk=0;
    int xp=0,yp=0,zp=0,xRotp=0,yRotp=0;
    bool noCulling=false;
    bool hasImpulse=false;
protected:
    bool m_ignoreVerticalCollisions=false;
    unsigned int m_uiAnimOverrideBitmask=0;
public:
    Entity(Level* level=nullptr,bool useSmallId=true);
    virtual ~Entity();
protected:
    void _init(bool useSmallId);
    virtual void defineSynchedData(){}
    // Entity::getSmallId / freeSmallId: network ids, not used here.
    int getSmallId(){return entityCounter++;}
    void freeSmallId(int){}
public:
    // 4J's extra wandering for protected mobs runs only for the server
    // thread's small ids, which the port does not use.
    virtual bool isDespawnProtected(){return false;}
    void considerForExtraWandering(bool){}
    bool isExtraWanderingEnabled(){return false;}
    int getWanderingQuadrant(){return 0;}
protected:
public:
    shared_ptr<SynchedEntityData> getEntityData();
protected:
    virtual void resetPos();
public:
    virtual void remove();
protected:
    virtual void setSize(float w,float h);
    void setRot(float yRot,float xRot);
public:
    void setPos(double x,double y,double z);
    void turn(float xo,float yo);
    virtual void tick();
    virtual void baseTick();
protected:
    void lavaHurt();
public:
    virtual void setOnFire(int numberOfSeconds);
    virtual void clearFire();
protected:
    virtual void outOfWorld();
public:
    bool isFree(float xa,float ya,float za,float grow);
    bool isFree(double xa,double ya,double za);
    virtual void move(double xa,double ya,double za,bool noEntityCubes=false);
protected:
    virtual void checkInsideTiles();
    // Entity::playStepSound: sounds are not ported.
    virtual void playStepSound(int,int,int,int){}
public:
    virtual void playSound(int iSound,float volume,float pitch);
protected:
    virtual bool makeStepSound();
    virtual void checkFallDamage(double ya,bool onGround);
public:
    virtual AABB* getCollideBox();
protected:
    virtual void burn(int dmg);
public:
    bool isFireImmune();
    virtual void causeFallDamage(float distance);
    bool isInWaterOrRain();
    virtual bool isInWater();
    virtual bool updateInWaterState();
    bool isUnderLiquid(Material* material);
    virtual float getHeadHeight();
    bool isInLava();
    void moveRelative(float xa,float za,float speed);
    virtual float getBrightness(float a);
    virtual void setLevel(Level* level);
    void absMoveTo(double x,double y,double z,float yRot,float xRot);
    void moveTo(double x,double y,double z,float yRot,float xRot);
    float distanceTo(shared_ptr<Entity> e);
    double distanceToSqr(double x2,double y2,double z2);
    double distanceTo(double x2,double y2,double z2);
    double distanceToSqr(shared_ptr<Entity> e);
    virtual void playerTouch(shared_ptr<Player> player);
    virtual void push(shared_ptr<Entity> e);
    virtual void push(double xa,double ya,double za);
protected:
    void markHurt();
public:
    virtual bool hurt(DamageSource* source,int damage);
    bool intersects(double x0,double y0,double z0,double x1,double y1,double z1);
    virtual bool isPickable();
    virtual bool isPushable();
    virtual bool isShootable();
    virtual void awardKillScore(shared_ptr<Entity> victim,int score);
    virtual void readAdditionalSaveData(CompoundTag*){}
    virtual void addAdditonalSaveData(CompoundTag*){}
    shared_ptr<ItemEntity> spawnAtLocation(int resource,int count);
    shared_ptr<ItemEntity> spawnAtLocation(int resource,int count,float yOffs);
    shared_ptr<ItemEntity> spawnAtLocation(shared_ptr<ItemInstance> itemInstance,float yOffs);
    virtual bool isAlive();
    virtual bool isInWall();
    virtual bool interact(shared_ptr<Player> player);
    virtual AABB* getCollideAgainstBox(shared_ptr<Entity> entity);
    virtual void handleEntityEvent(byte eventId);
    virtual void animateHurt();
    virtual bool isOnFire();
    virtual bool isRiding();
    virtual bool isSneaking();
    virtual void setSneaking(bool value);
    virtual bool isSprinting();
    virtual void setSprinting(bool value);
    virtual bool isInvisible();
    virtual void setInvisible(bool value);
protected:
    bool getSharedFlag(int flag);
    void setSharedFlag(int flag,bool value);
public:
    int getAirSupply();
    void setAirSupply(int supply);
    virtual void killed(shared_ptr<Mob> mob);
protected:
    bool checkInTile(double x,double y,double z);
public:
    virtual void makeStuckInWeb();
    virtual void rideTick();
    virtual void positionRider();
    virtual double getRidingHeight();
    virtual double getRideHeight();
    virtual bool canCreateParticles(){return true;}
    virtual bool is(shared_ptr<Entity> other);
    virtual float getYHeadRot();
    virtual void setYHeadRot(float yHeadRot);
    virtual bool isAttackable();
    virtual bool isInvulnerable();
    virtual void copyPosition(shared_ptr<Entity> target);
    unsigned int getAnimOverrideBitmask(){return m_uiAnimOverrideBitmask;}
};
// What Mob.h names beyond the entity: mob effects (only their ids and the
// map entries here; no effect reaches a mob in the port), icons and textures
// (client only), the player's inventory (for the looting bonus).
typedef unsigned char BYTE;
using ::Short;
#include "TextureNames.inc"
class Icon;
class HitResult;
class Inventory;
class MobEffect {
public:
    int id;
    explicit MobEffect(int id):id(id){}
    int getId(){return id;}
    // MobEffect.h's effects (by id).
    static MobEffect *movementSpeed,*movementSlowdown,*digSpeed,*digSlowdown,*damageBoost,*heal,*harm,*jump,*confusion,*regeneration,*damageResistance,*fireResistance,*waterBreathing,*invisibility,*blindness,*nightVision,*hunger,*weakness,*poison,*wither;
};
inline MobEffect* MobEffect::movementSpeed=new MobEffect(1);
inline MobEffect* MobEffect::movementSlowdown=new MobEffect(2);
inline MobEffect* MobEffect::digSpeed=new MobEffect(3);
inline MobEffect* MobEffect::digSlowdown=new MobEffect(4);
inline MobEffect* MobEffect::damageBoost=new MobEffect(5);
inline MobEffect* MobEffect::heal=new MobEffect(6);
inline MobEffect* MobEffect::harm=new MobEffect(7);
inline MobEffect* MobEffect::jump=new MobEffect(8);
inline MobEffect* MobEffect::confusion=new MobEffect(9);
inline MobEffect* MobEffect::regeneration=new MobEffect(10);
inline MobEffect* MobEffect::damageResistance=new MobEffect(11);
inline MobEffect* MobEffect::fireResistance=new MobEffect(12);
inline MobEffect* MobEffect::waterBreathing=new MobEffect(13);
inline MobEffect* MobEffect::invisibility=new MobEffect(14);
inline MobEffect* MobEffect::blindness=new MobEffect(15);
inline MobEffect* MobEffect::nightVision=new MobEffect(16);
inline MobEffect* MobEffect::hunger=new MobEffect(17);
inline MobEffect* MobEffect::weakness=new MobEffect(18);
inline MobEffect* MobEffect::poison=new MobEffect(19);
inline MobEffect* MobEffect::wither=new MobEffect(20);
class MobEffectInstance {
    int id,duration,amplifier;
public:
    MobEffectInstance(int id,int duration,int amplifier):id(id),duration(duration),amplifier(amplifier){}
    int getId(){return id;}
    int getDuration(){return duration;}
    int getAmplifier(){return amplifier;}
};
class Level;
using LevelSource=Level;
// JavaIntHash.h's IntKeyHash (Java's supplemental hash; unsigned here, where
// the source's signed int arithmetic overflows) and IntKeyEq.
struct IntKeyHash {
    int operator()(const int& k)const{
        unsigned h=static_cast<unsigned>(k);
        h+=~(h<<9);
        h^=h>>14;
        h+=h<<4;
        h^=h>>10;
        return static_cast<int>(h);
    }
};
struct IntKeyEq { bool operator()(const int& x,const int& y)const{return x==y;} };
// System::arraycopy (java.lang.System's, which copies as if through a temporary).
template<class T> class arrayWithLength;
struct System {
    template<class T>static void arraycopy(arrayWithLength<T> src,unsigned int srcPos,arrayWithLength<T>* dst,unsigned int dstPos,unsigned int length){
        std::vector<T> copy(src.data+srcPos,src.data+srcPos+length);
        std::copy(copy.begin(),copy.end(),dst->data+dstPos);
    }
};
#include "SourceClasses.inc"
// Wolf: only whether it is tame, for Mob::hurt (wolves are not ported yet).
class Wolf:public Mob {
public:
    using Mob::Mob;
    int getMaxHealth()override{return 8;}
    bool isTame(){return false;}
};
// EnchantmentHelper::getKillingLootBonus: the looting level of the held
// weapon (enchantments are not applied to kills yet).
struct EnchantmentHelper { static int getKillingLootBonus(Inventory*){return 0;} };
// Projectile::shoot's aim for the thrown entities the dispenser makes (they
// are not ported: the host never lets one be made).
class Projectile:public Entity {
public:
    Projectile()=default;
    Projectile(double x,double y,double z){this->x=x;this->y=y;this->z=z;}
    void shoot(double xd,double yd,double zd,float,float){this->xd=xd;this->yd=yd;this->zd=zd;}
};
class Arrow:public Projectile {
public:
    static const int PICKUP_ALLOWED=1;
    int pickup=0;
    Arrow()=default;
    Arrow(class Level*,double x,double y,double z):Projectile(x,y,z){}
};
class ThrownEgg:public Projectile { public: ThrownEgg(class Level*,double x,double y,double z):Projectile(x,y,z){} };
class Snowball:public Projectile { public: Snowball(class Level*,double x,double y,double z):Projectile(x,y,z){} };
class ThrownExpBottle:public Projectile { public: ThrownExpBottle(class Level*,double x,double y,double z):Projectile(x,y,z){} };
class ThrownPotion:public Projectile { public: ThrownPotion(class Level*,double x,double y,double z,int):Projectile(x,y,z){} };
class SmallFireball:public Projectile {
public:
    SmallFireball(class Level*,double x,double y,double z,double xa,double ya,double za):Projectile(x,y,z){xd=xa;yd=ya;zd=za;}
};
class Minecart:public Entity { public: Minecart(class Level*,double x,double y,double z,int){this->x=x;this->y=y;this->z=z;} };
class Boat:public Entity { public: Boat(class Level*,double x,double y,double z){this->x=x;this->y=y;this->z=z;} };
class Level;
// TileEntity: the fields and removal flag the piston pieces use.
class TileEntity {
public:
    Level* level=nullptr;
    int x=0,y=0,z=0;
    bool removed=false;
    virtual ~TileEntity()=default;
    virtual void tick(){}
    void setRemoved(){removed=true;}
    void clearRemoved(){removed=false;}
    bool isRemoved()const{return removed;}
};
// Statistics and achievements are not ported.
struct GenericStats {
    static int portalsCreated(){return 0;}
    static int InToTheNether(){return 0;}
    static int param_noArgs(){return 0;}
    static int param_InToTheNether(){return 0;}
    static int killMob(){return 0;}
    template<class... T>static int param_mobKill(T...){return 0;}
    static int stayinFrosty(){return 0;}
    static int param_stayinFrosty(){return 0;}
};
class ItemInstance;
class DispenserTileEntity;
class Player:public Mob {
public:
    Player():Mob(nullptr){heightOffset=1.62f;}
    eINSTANCEOF GetType()override{return eTYPE_PLAYER;}
    int getMaxHealth()override{return 20;}
    Inventory* inventory=nullptr;
    // Player::getArmorCoverPercentage (the worn armour pieces, which the
    // host sets) and hasInvisiblePrivilege (a host privilege, off).
    float armorCover=0;
    float getArmorCoverPercentage(){return armorCover;}
    bool hasInvisiblePrivilege(){return false;}
    // Player::isLocalPlayer: the server's players are not.
    virtual bool isLocalPlayer(){return false;}
    // The carried item (TntTile::use looks for flint and steel).
    shared_ptr<ItemInstance> selected;
    shared_ptr<ItemInstance> getSelectedItem(){return selected;}
    Abilities abilities;
    bool mayBuild(int,int,int){return true;}
    void awardStat(int,int){}
    // Player::openTrap: the dispenser the player opened (the client shows its menu).
    shared_ptr<DispenserTileEntity> openedTrap;
    bool openTrap(shared_ptr<DispenserTileEntity> container){openedTrap=container;return true;}
};
class Item;
class ItemInstance {
public:
    int id=0,count=0,auxValue=0,damage=0;
    ItemInstance()=default;
    ItemInstance(int id,int count,int auxValue):id(id),count(count),auxValue(auxValue){}
    explicit ItemInstance(Item* item);
    int getAuxValue()const{return auxValue;}
    Item* getItem();
    // ItemInstance::remove: a stack of count split off (with a copy of the tag).
    shared_ptr<ItemInstance> remove(int n){
        auto result=std::make_shared<ItemInstance>(id,n,auxValue);
        if(tag)result->tag.reset(static_cast<CompoundTag*>(tag->copy()));
        count-=n;
        return result;
    }
    shared_ptr<CompoundTag> tag;
    bool hasTag(){return tag!=nullptr;}
    CompoundTag* getTag(){return tag.get();}
    void setTag(CompoundTag* t){tag.reset(t);}
    int data4J=0;
    void set4JData(int data){data4J=data;}
    int get4JData(){return data4J;}
    // ItemInstance::hurt: the host wears the tool (and breaks it) afterwards.
    void hurt(int amount,shared_ptr<Player>){damage+=amount;}
};
// TilePos with the source's hash (in unsigned arithmetic: the source's signed
// multiply overflows).
class TilePos {
public:
    int x,y,z;
    TilePos(int x,int y,int z):x(x),y(y),z(z){}
    static int hash_fnct(const TilePos& k){
        return static_cast<int>(static_cast<unsigned>(k.x)*8976890u+static_cast<unsigned>(k.y)*981131u+static_cast<unsigned>(k.z));
    }
    static bool eq_test(const TilePos& a,const TilePos& b){return a.x==b.x && a.y==b.y && a.z==b.z;}
};
struct TilePosKeyHash { int operator()(const TilePos& k)const{return TilePos::hash_fnct(k);} };
struct TilePosKeyEq { bool operator()(const TilePos& a,const TilePos& b)const{return TilePos::eq_test(a,b);} };
struct LevelEvent {
    static const int SOUND_CLICK=1000,SOUND_CLICK_FAIL=1001,SOUND_LAUNCH=1002,SOUND_OPEN_DOOR=1003,
        SOUND_BLAZE_FIREBALL=1009,PARTICLES_SHOOT=2000;
};
// HitResult: only whether Level::clip hit something.
class HitResult {};
inline void MemSect(int){}
// PIX profiler markers.
inline void PIXBeginNamedEvent(int,const char*,...){}
inline void PIXEndNamedEvent(){}
// Math::random: java.lang.Math's shared generator.
struct Math { static double random(){static Random generator;return generator.nextDouble();} };

struct Dimension {
    // Dimension::brightnessRamp (updateLightRamp is the source's).
    float brightnessRamp[16]{};
    Dimension(){updateLightRamp();}
    void updateLightRamp();
    int id=0;
    bool ultraWarm=false,hasCeiling=false;
    // Dimension::getXZSize: the world's width in chunks.
    int xzSize=54;
    int getXZSize()const{return xzSize;}
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
// ItemEntity(level, x, y, z, item): ItemEntity::_init's random throw.
class ItemEntity:public Entity {
public:
    shared_ptr<ItemInstance> item;
    int throwTime=0;
    ItemEntity(Level*,double x,double y,double z,shared_ptr<ItemInstance> item):item(item){
        this->x=x;this->y=y;this->z=z;
        xd=(float)(Math::random()*0.2f-0.1f);
        yd=+0.2f;
        zd=(float)(Math::random()*0.2f-0.1f);
    }
    shared_ptr<ItemInstance> getItem(){return item;}
};
// ExperienceOrb(level, x, y, z, count): the orb a dying mob drops (the rest of
// the orb is the World's), and its value split.
class ExperienceOrb:public Entity {
public:
    int value;
    ExperienceOrb(Level*,double x,double y,double z,int count):value(count){this->x=x;this->y=y;this->z=z;}
    static int getExperienceValue(int maxValue);
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
    // What NotGateTile's toggle history is kept under: the world, not this
    // object (the host makes a Level for each call).
    const void* identity=this;
    static const int MAX_LEVEL_SIZE=30000000;
    static const int maxMovementHeight=512,minBuildHeight=0;
    // Level::boxes: getCubes' result list.
    AABBList boxes;
    // Level.h's 4J entity limits.
    static const int MAX_XBOX_BOATS=40,MAX_CONSOLE_MINECARTS=40,MAX_DISPENSABLE_FIREBALLS=200,
        MAX_DISPENSABLE_PROJECTILES=300;
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
    // The 4J overload whose last argument only concerns sending to clients.
    bool setTileAndDataNoUpdate(int x,int y,int z,int tile,int data,bool){return setTileAndDataNoUpdate(x,y,z,tile,data);}
    // Tile entities (the piston pieces) and ServerLevel::tileEvent.
    virtual shared_ptr<TileEntity> getTileEntity(int,int,int){return nullptr;}
    virtual void setTileEntity(int,int,int,shared_ptr<TileEntity>){}
    virtual void removeTileEntity(int,int,int){}
    virtual void tileEvent(int,int,int,int,int,int){}
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
    // Entity movement: the collision boxes in a box, liquids, fire.
    AABBList* getCubes(shared_ptr<Entity> source,AABB* box,bool noEntities=false,bool blockAtEdge=false);
    bool containsAnyLiquid(AABB* box);
    bool containsFireTile(AABB* box);
    bool checkAndHandleWater(AABB* box,Material* material,shared_ptr<Entity> e);
    bool containsMaterial(AABB* box,Material* material);
    bool containsLiquid(AABB* box,Material* material);
    float getBrightness(int x,int y,int z);
    bool hasChunkAt(int x,int y,int z);
    int getTileRenderShape(int x,int y,int z);
    bool reallyHasChunk(int x,int z){return hasChunk(x,z);}
    // Tile::addAABBs for a tile whose collision is not ported: the current
    // shape here; the World uses its own collision shapes.
    virtual void addTileAABBs(Tile* tile,int x,int y,int z,AABB* box,AABBList* boxes,shared_ptr<Entity>);
    Material* getMaterial(int x,int y,int z);
    // Level::isSolidBlockingTile.
    bool isSolidBlockingTile(int x,int y,int z);
    bool isSolidBlockingTileInLoadedChunk(int x,int y,int z,bool valueIfNotLoaded);
    bool isTopSolidBlocking(int x,int y,int z);
    bool mayPlace(int tileId,int x,int y,int z,bool ignoreEntities,int face,shared_ptr<Entity> ignoreEntity);
    bool isUnobstructed(AABB* aabb);
    bool isUnobstructed(AABB* aabb,shared_ptr<Entity> ignore);
    bool containsAnyLiquid_NoLoad(AABB* box);
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
    // Level::levelEvent: sounds and particles (client effects), which the
    // host may record.
    virtual void levelEvent(int,int,int,int,int){}
    // Level::countInstanceOf and addEntity for the entities the dispenser
    // makes: items and mobs (spawn eggs) are the host's; thrown entities,
    // minecarts and boats are not ported, so the host counts them at the limit.
    virtual int countInstanceOf(eINSTANCEOF,bool){return 1<<30;}
    virtual void addEntity(shared_ptr<Entity>){}
    // EntityIO::newById and Level::canCreateMore for MonsterPlacerItem::canSpawn.
    virtual bool canSpawnEgg(int){return false;}
    void playSound(double,double,double,int,float,float){}
    // Level::getTime (the game time) and the entities in a box.
    virtual std::int64_t getTime(){return 0;}
    // Level::players and the nearest-player queries (the source's).
    std::vector<shared_ptr<Player>> players;
    shared_ptr<Player> getNearestPlayer(shared_ptr<Entity> source,double maxDist,double maxYDist=-1);
    shared_ptr<Player> getNearestPlayer(double x,double y,double z,double maxDist,double maxYDist=-1);
    shared_ptr<Player> getNearestPlayer(double x,double z,double maxDist);
    shared_ptr<Player> getNearestAttackablePlayer(shared_ptr<Entity> source,double maxDist);
    shared_ptr<Player> getNearestAttackablePlayer(double x,double y,double z,double maxDist);
    // Level::findPath over a Region of the level.
    Path* findPath(shared_ptr<Entity> from,shared_ptr<Entity> to,float maxDist,bool canPassDoors,bool canOpenDoors,bool avoidWater,bool canFloat);
    Path* findPath(shared_ptr<Entity> from,int xBest,int yBest,int zBest,float maxDist,bool canPassDoors,bool canOpenDoors,bool avoidWater,bool canFloat);
    // Level::broadcastEntityEvent: client animations (hurt, death), not ported.
    void broadcastEntityEvent(shared_ptr<Entity>,byte){}
    enum class EntityClass { Any,Mob,Player,Arrow };
    // The entities in a box, as stand-ins whose move() moves the real one.
    virtual std::vector<shared_ptr<Entity>> entitiesIn(const AABB&,EntityClass){return {};}
    // Level::getEntities (the level's own list) and getEntitiesOfClass (a new
    // list the caller deletes).
    std::vector<shared_ptr<Entity>>* getEntities(shared_ptr<Entity>,AABB* box){
        found=box?entitiesIn(*box,EntityClass::Any):std::vector<shared_ptr<Entity>>{};
        return &found;
    }
    std::vector<shared_ptr<Entity>>* getEntitiesOfClass(const std::type_info& type,AABB* box){
        const auto kind=type==typeid(Player)?EntityClass::Player:type==typeid(Arrow)?EntityClass::Arrow:EntityClass::Mob;
        return new std::vector<shared_ptr<Entity>>(box?entitiesIn(*box,kind):std::vector<shared_ptr<Entity>>{});
    }
    std::vector<shared_ptr<Entity>> found;
    void addParticle(int,double,double,double,double,double,double){}
    void playSound(shared_ptr<Entity>,int,float,float){}
    // Level::clip between two points: a result when a block is in the way
    // (the caller deletes it).
    virtual HitResult* clip(Vec3*,Vec3*){return nullptr;}
    float getSeenPercent(Vec3* center,AABB* bb);
    // ServerLevel's primed TNT limit and Level::addEntity(PrimedTnt).
    virtual bool newPrimedTntAllowed(){return true;}
    virtual void addEntity(shared_ptr<class PrimedTnt>){}
    virtual bool tntExplodes(){return true;}
    // Level::getInstaTick: only world generation ticks instantly.
    bool getInstaTick(){return false;}
    // PlayerList::isTrackingTile and the Fire Spreads host option.
    virtual bool isTrackingTile(int,int,int){return true;}
    virtual bool fireSpreads(){return true;}

    // Tile::spawnResources (drops for tile and data at the position).
    // With odds below 1 (explosions) each item is kept with that chance.
    virtual void spawnResources(int x,int y,int z,int tile,int data,float odds=1)=0;
    // Sapling::growTree's feature placement: the host runs the original tree
    // feature and returns Feature::place. The mushroom kinds are
    // HugeMushroomFeature(0) and (1) for Mushroom::growTree.
    enum class TreeKind { Tree,BigTree,Spruce,Birch,JungleTree,MegaJungle,BrownMushroom,RedMushroom };
    virtual bool placeTree(TreeKind kind,int height,Random& random,int x,int y,int z)=0;

    // The Level the extracted code is currently running against (for the
    // MinecraftServer and app stand-ins); set by the host while it runs them.
    static inline thread_local Level* current=nullptr;
};
// Minecraft::GetInstance()->levelRenderer->destroyedTileManager: the client's
// removed-but-still-drawn tiles, which getCubes also blocks (none here).
struct DestroyedTileManager { void addAABBs(Level*,AABB*,AABBList*){} };
struct LevelRenderer { DestroyedTileManager ownManager;DestroyedTileManager* destroyedTileManager=&ownManager; };
struct Minecraft {
    LevelRenderer ownRenderer;LevelRenderer* levelRenderer=&ownRenderer;
    static Minecraft* GetInstance(){static Minecraft instance;return &instance;}
};
// Region: a LevelSource over part of a level. The source copies the chunks;
// the path finder only reads, so this reads the level itself.
class Region final:public Level {
    Level* level_;
public:
    Region(Level* level,int,int,int,int,int,int):level_(level){
        random=level->random;dimension=level->dimension;chunkSourceXZSize=level->chunkSourceXZSize;
    }
    int getTile(int x,int y,int z)override{return level_->getTile(x,y,z);}
    int getData(int x,int y,int z)override{return level_->getData(x,y,z);}
    bool setTileAndDataNoUpdate(int,int,int,int,int)override{return false;}
    bool setDataNoUpdate(int,int,int,int)override{return false;}
    bool hasChunk(int x,int z)override{return level_->hasChunk(x,z);}
    int getRawBrightness(int x,int y,int z)override{return level_->getRawBrightness(x,y,z);}
    int getDaytimeRawBrightness(int x,int y,int z)override{return level_->getDaytimeRawBrightness(x,y,z);}
    int getBrightness(LightLayer::variety layer,int x,int y,int z)override{return level_->getBrightness(layer,x,y,z);}
    bool canSeeSky(int x,int y,int z)override{return level_->canSeeSky(x,y,z);}
    bool isRainingAt(int x,int y,int z)override{return level_->isRainingAt(x,y,z);}
    bool hasChunksAt(int x0,int y0,int z0,int x1,int y1,int z1)override{return level_->hasChunksAt(x0,y0,z0,x1,y1,z1);}
    void spawnResources(int,int,int,int,int,float)override{}
    bool placeTree(TreeKind,int,Random&,int,int,int)override{return false;}
};
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
enum eGameHostOption { eGameHostOption_FireSpreads,eGameHostOption_TNT };
struct App {
    bool GetGameHostOption(eGameHostOption option){
        if(!Level::current)return true;
        return option==eGameHostOption_TNT?Level::current->tntExplodes():Level::current->fireSpreads();
    }
    void DebugPrintf(const char*,...){}
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
    static Tile *lightGem,*wood,*rock,*stoneSlab,*redStoneDust,*notGate_on,*notGate_off;
    static class PistonMovingPiece* pistonMovingPiece;
    // Tile::setShape writes the shape to thread storage (TlsGetValue).
    class ThreadStorage {
    public:
        double xx0=0,yy0=0,zz0=0,xx1=1,yy1=1,zz1=1;
        int tileId=0;
    };
    static inline int tlsIdxShape=0;
    static ThreadStorage* shapeStorage(){static thread_local ThreadStorage storage;return &storage;}
    void setShape(float x0,float y0,float z0,float x1,float y1,float z1);
    virtual void updateShape(LevelSource*,int,int,int,int=-1,shared_ptr<TileEntity> =nullptr){}

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
    // Tile::getAABB with the default shape: a full cube for a tile that blocks
    // motion (the non-colliding tiles return none in the source).
    virtual AABB* getAABB(Level*,int x,int y,int z){
        return material && material->blocksMotion()?AABB::newTemp(x,y,z,x+1,y+1,z+1):nullptr;
    }
    // Pistons: push reaction, destroy time, tile entity classes, tile events.
    static constexpr float INDESTRUCTIBLE_DESTROY_TIME=-1.0f;
    float destroyTime=0;
    bool entityTile=false;
    float getDestroySpeed(Level*,int,int,int){return destroyTime;}
    bool isEntityTile()const{return entityTile;}
    virtual int getPistonPushReaction();
    virtual void triggerEvent(Level*,int,int,int,int,int){}
    // Explosions: Tile::explosionResistance and wasExploded.
    float explosionResistance=0;
    float getExplosionResistance(shared_ptr<Entity> source);
    virtual void wasExploded(Level*,int,int,int){}
    // Tile::addAABBs: the current shape's box when it meets `box` (the host
    // passes none to collect them all).
    // The host decides the boxes of the tiles whose addAABBs/getAABB are not
    // ported (Level::addTileAABBs); by default the current shape.
    virtual void addAABBs(Level* level,int x,int y,int z,AABB* box,std::vector<AABB*>* boxes,shared_ptr<Entity> source);
    void addShapeAABB(int x,int y,int z,AABB* box,std::vector<AABB*>* boxes){
        auto* s=shapeStorage();
        AABB* shape=AABB::newTemp(x+s->xx0,y+s->yy0,z+s->zz0,x+s->xx1,y+s->yy1,z+s->zz1);
        if(!box || (shape->x1>box->x0 && shape->x0<box->x1 && shape->y1>box->y0 && shape->y0<box->y1 &&
                    shape->z1>box->z0 && shape->z0<box->z1))boxes->push_back(shape);
    }
    double getShapeX0(){return shapeStorage()->xx0;}
    double getShapeY0(){return shapeStorage()->yy0;}
    double getShapeZ0(){return shapeStorage()->zz0;}
    double getShapeX1(){return shapeStorage()->xx1;}
    double getShapeY1(){return shapeStorage()->yy1;}
    double getShapeZ1(){return shapeStorage()->zz1;}
    virtual int getTickDelay(){return 10;}
    // Tile::getRenderShape (the class's own, from RenderShapes.inc).
    int renderShape=SHAPE_BLOCK;
    // Tile::friction (0.6; IceTile sets 0.98).
    float friction=0.6f;
    typedef ::console::sim::SoundType SoundType;
    virtual int getRenderShape(){return renderShape;}
    // Entities on and in tiles.
    virtual void stepOn(Level*,int,int,int,shared_ptr<Entity>){}
    virtual void fallOn(Level*,int,int,int,shared_ptr<Entity>,float){}
    virtual void handleEntityInside(Level*,int,int,int,shared_ptr<Entity>,Vec3*){}
    virtual bool isSolidFace(LevelSource* level,int x,int y,int z,int face);
    virtual bool isPathfindable(LevelSource* level,int x,int y,int z);
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
    // Player interaction and placement (ServerPlayerGameMode, TileItem).
    virtual bool TestUse(){return false;}
    virtual bool use(Level*,int,int,int,shared_ptr<Player>,int,float,float,float,bool=false){return false;}
    virtual void attack(Level*,int,int,int,shared_ptr<Player>){}
    virtual void setPlacedBy(Level*,int,int,int,shared_ptr<Mob>){}
    virtual void destroy(Level*,int,int,int,int){}
    virtual void entityInside(Level*,int,int,int,shared_ptr<Entity>){}
    // Redstone (answered by the signal sources in a later batch).
    virtual bool isSignalSource(){return false;}
    virtual bool getSignal(LevelSource*,int,int,int,int){return false;}
    virtual bool getDirectSignal(Level*,int,int,int,int){return false;}
    // spawnResources(level, x, y, z, data, playerBonusLevel): the item drops
    // (ported SurvivalRules, including the leaf and cocoa overrides).
    virtual void spawnResources(Level* level,int x,int y,int z,int data,float odds,int){level->spawnResources(x,y,z,id,data,odds);}
    void spawnResources(Level* level,int x,int y,int z,int data,int bonus){spawnResources(level,x,y,z,data,1.0f,bonus);}
};

inline void Tile::addAABBs(Level* level,int x,int y,int z,AABB* box,std::vector<AABB*>* boxes,shared_ptr<Entity> source){
    level->addTileAABBs(this,x,y,z,box,boxes,source);
}
inline void Level::addTileAABBs(Tile* tile,int x,int y,int z,AABB* box,AABBList* boxes,shared_ptr<Entity>){
    tile->addShapeAABB(x,y,z,box,boxes);
}
// TlsGetValue(Tile::tlsIdxShape) is this thread's shape storage.
inline void* TlsGetValue(DWORD index){return index==0?static_cast<void*>(Tile::shapeStorage()):tlsSlot(index);}

class EntityTile:public Tile {
public:
    void onRemove(Level* level,int x,int y,int z,int id,int data)override;
};

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
    int getPistonPushReaction()override;
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
    bool isPathfindable(LevelSource* level,int x,int y,int z)override;
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
    bool TestUse()override;
    bool use(Level* level,int x,int y,int z,shared_ptr<Player> player,int clickedFace,float clickX,float clickY,float clickZ,bool soundOnly=false)override;
    int getPistonPushReaction()override;
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
    void onPlace(Level* level,int x,int y,int z)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    void wasExploded(Level* level,int x,int y,int z)override;
    void destroy(Level* level,int x,int y,int z,int data)override;
    bool use(Level* level,int x,int y,int z,shared_ptr<Player> player,int clickedFace,float clickX,float clickY,float clickZ,bool soundOnly=false)override;
};
// PrimedTnt(level, x, y, z): the lit block's start (PrimedTnt.cpp; the rest
// of the entity is the host's).
class PrimedTnt:public Entity {
public:
    int life=80;
    PrimedTnt(Level*,double x,double y,double z){
        this->x=x;this->y=y;this->z=z;
        float rot=(float)(Math::random()*3.141592654f*2);
        xd=-std::sin(rot)*0.02f;
        yd=+0.2f;
        zd=-std::cos(rot)*0.02f;
    }
};
class Explosion {
public:
    bool fire=false,destroyBlocks=true;
    int size=16;
    Random* random=nullptr;
    Level* level=nullptr;
    double x=0,y=0,z=0;
    shared_ptr<Entity> source;
    float r=0;
    std::unordered_set<TilePos,TilePosKeyHash,TilePosKeyEq> toBlow;
    typedef std::map<shared_ptr<Player>,Vec3*> playerVec3Map;
    playerVec3Map hitPlayers;
    Explosion(Level* level,shared_ptr<Entity> source,double x,double y,double z,float r);
    ~Explosion();
    void explode();
    void finalizeExplosion(bool generateParticles,vector<TilePos>* toBlowDirect=nullptr);
};
class PortalTile:public Tile {
public:
    // Nether portals come with the dimension batch: fire on obsidian lights normally.
    bool trySpawnPortal(Level*,int,int,int,bool){return false;}
};
class LiquidTile:public Tile {
public:
    int getDepth(Level* level,int x,int y,int z);
    static float getHeight(int d);
    bool isPathfindable(LevelSource* level,int x,int y,int z)override;
    int getRenderedDepth(LevelSource* level,int x,int y,int z);
    bool isSolidFace(LevelSource* level,int x,int y,int z,int face)override;
    Vec3* getFlow(LevelSource* level,int x,int y,int z);
    void handleEntityInside(Level* level,int x,int y,int z,shared_ptr<Entity> e,Vec3* current)override;
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
    bool isPathfindable(LevelSource* level,int x,int y,int z)override;
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
    bool isPathfindable(LevelSource* level,int x,int y,int z)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    void setDynamic(Level* level,int x,int y,int z);
    void tick(Level* level,int x,int y,int z,Random* random)override;
    bool isFlammable(Level* level,int x,int y,int z);
};
class WallTile:public Tile {
public:
    bool isPathfindable(LevelSource* level,int x,int y,int z)override;
};
class FenceTile:public Tile {
public:
    bool isPathfindable(LevelSource* level,int x,int y,int z)override;
    static bool isFence(int tile);
};
class RedStoneDustTile:public Tile {
public:
    bool shouldSignal=true;
    std::unordered_set<TilePos,TilePosKeyHash,TilePosKeyEq> toUpdate;
    bool mayPlace(Level* level,int x,int y,int z)override;
    void updatePowerStrength(Level* level,int x,int y,int z);
    void updatePowerStrength(Level* level,int x,int y,int z,int xFrom,int yFrom,int zFrom);
    void checkCornerChangeAt(Level* level,int x,int y,int z);
    void onPlace(Level* level,int x,int y,int z)override;
    void onRemove(Level* level,int x,int y,int z,int id,int data)override;
    int checkTarget(Level* level,int x,int y,int z,int target);
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    bool getDirectSignal(Level* level,int x,int y,int z,int dir)override;
    bool getSignal(LevelSource* level,int x,int y,int z,int dir)override;
    bool isSignalSource()override;
    static bool shouldConnectTo(LevelSource* level,int x,int y,int z,int direction);
    static bool shouldReceivePowerFrom(LevelSource* level,int x,int y,int z,int direction);
};
class NotGateTile:public TorchTile {
public:
    TILE_CONSTANTS_NotGateTile
    class Toggle {
    public:
        int x,y,z;
        std::int64_t when;
        Toggle(int x,int y,int z,std::int64_t when):x(x),y(y),z(z),when(when){}
    };
    // recentToggles, keyed by Level::identity (see Level::identity).
    struct LevelKey {
        const void* id;
        LevelKey(Level* level):id(level?level->identity:nullptr){}
        LevelKey(const void* value):id(value){}
        bool operator==(const LevelKey&)const=default;
    };
    struct LevelKeyHash { std::size_t operator()(const LevelKey& k)const{return std::hash<const void*>()(k.id);} };
    struct ToggleMap:std::unordered_map<LevelKey,std::deque<Toggle>*,LevelKeyHash> {
        ~ToggleMap(){for(auto& entry:*this)delete entry.second;}
    };
    static ToggleMap recentToggles;
    bool on=false;
    static void removeLevelReferences(Level* level);
    bool isToggledTooFrequently(Level* level,int x,int y,int z,bool add);
    int getTickDelay()override;
    void onPlace(Level* level,int x,int y,int z)override;
    void onRemove(Level* level,int x,int y,int z,int id,int data)override;
    bool getSignal(LevelSource* level,int x,int y,int z,int face)override;
    bool hasNeighborSignal(Level* level,int x,int y,int z);
    void tick(Level* level,int x,int y,int z,Random* random)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    bool getDirectSignal(Level* level,int x,int y,int z,int face)override;
    bool isSignalSource()override;
};
class LeverTile:public Tile {
public:
    bool mayPlace(Level* level,int x,int y,int z,int face)override;
    bool mayPlace(Level* level,int x,int y,int z)override;
    int getPlacedOnFaceDataValue(Level* level,int x,int y,int z,int face,float clickX,float clickY,float clickZ,int itemValue)override;
    static int getLeverFacing(int facing);
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    bool checkCanSurvive(Level* level,int x,int y,int z);
    void updateShape(LevelSource* level,int x,int y,int z,int forceData=-1,shared_ptr<TileEntity> forceEntity=nullptr)override;
    void attack(Level* level,int x,int y,int z,shared_ptr<Player> player)override;
    bool TestUse()override;
    bool use(Level* level,int x,int y,int z,shared_ptr<Player> player,int clickedFace,float clickX,float clickY,float clickZ,bool soundOnly=false)override;
    void onRemove(Level* level,int x,int y,int z,int id,int data)override;
    bool getSignal(LevelSource* level,int x,int y,int z,int dir)override;
    bool getDirectSignal(Level* level,int x,int y,int z,int dir)override;
    bool isSignalSource()override;
};
class ButtonTile:public Tile {
public:
    bool sensitive=false;
    int getTickDelay()override;
    bool mayPlace(Level* level,int x,int y,int z,int face)override;
    bool mayPlace(Level* level,int x,int y,int z)override;
    int getPlacedOnFaceDataValue(Level* level,int x,int y,int z,int face,float clickX,float clickY,float clickZ,int itemValue)override;
    int findFace(Level* level,int x,int y,int z);
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    bool checkCanSurvive(Level* level,int x,int y,int z);
    void updateShape(LevelSource* level,int x,int y,int z,int forceData=-1,shared_ptr<TileEntity> forceEntity=nullptr)override;
    void updateShape(int data);
    void attack(Level* level,int x,int y,int z,shared_ptr<Player> player)override;
    bool TestUse()override;
    bool use(Level* level,int x,int y,int z,shared_ptr<Player> player,int clickedFace,float clickX,float clickY,float clickZ,bool soundOnly=false)override;
    void onRemove(Level* level,int x,int y,int z,int id,int data)override;
    bool getSignal(LevelSource* level,int x,int y,int z,int dir)override;
    bool getDirectSignal(Level* level,int x,int y,int z,int dir)override;
    bool isSignalSource()override;
    void tick(Level* level,int x,int y,int z,Random* random)override;
    void entityInside(Level* level,int x,int y,int z,shared_ptr<Entity> entity)override;
    void checkPressed(Level* level,int x,int y,int z);
    void updateNeighbours(Level* level,int x,int y,int z,int dir);
    bool shouldTileTick(Level* level,int x,int y,int z)override;
};
class PressurePlateTile:public Tile {
public:
    bool isPathfindable(LevelSource* level,int x,int y,int z)override;
    enum Sensitivity { everything,mobs,players };
    Sensitivity sensitivity=everything;
    int getTickDelay()override;
    bool mayPlace(Level* level,int x,int y,int z)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    void tick(Level* level,int x,int y,int z,Random* random)override;
    void entityInside(Level* level,int x,int y,int z,shared_ptr<Entity> entity)override;
    void checkPressed(Level* level,int x,int y,int z);
    void onRemove(Level* level,int x,int y,int z,int id,int data)override;
    void updateShape(LevelSource* level,int x,int y,int z,int forceData=-1,shared_ptr<TileEntity> forceEntity=nullptr)override;
    bool getSignal(LevelSource* level,int x,int y,int z,int dir)override;
    bool getDirectSignal(Level* level,int x,int y,int z,int dir)override;
    bool isSignalSource()override;
    bool shouldTileTick(Level* level,int x,int y,int z)override;
    int getPistonPushReaction()override;
};
class DiodeTile:public DirectionalTile {
public:
    TILE_CONSTANTS_DiodeTile
    // DiodeTile.cpp: const int DiodeTile::DELAYS[4] = { 1, 2, 3, 4 };
    static constexpr int DELAYS[4]={1,2,3,4};
    bool on=false;
    bool mayPlace(Level* level,int x,int y,int z)override;
    bool canSurvive(Level* level,int x,int y,int z)override;
    void tick(Level* level,int x,int y,int z,Random* random)override;
    bool getDirectSignal(Level* level,int x,int y,int z,int dir)override;
    bool getSignal(LevelSource* level,int x,int y,int z,int facing)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    bool getSourceSignal(Level* level,int x,int y,int z,int data);
    bool TestUse()override;
    bool use(Level* level,int x,int y,int z,shared_ptr<Player> player,int clickedFace,float clickX,float clickY,float clickZ,bool soundOnly=false)override;
    bool isSignalSource()override;
    void setPlacedBy(Level* level,int x,int y,int z,shared_ptr<Mob> by)override;
    void onPlace(Level* level,int x,int y,int z)override;
    void destroy(Level* level,int x,int y,int z,int data)override;
};
class RedlightTile:public Tile {
public:
    bool isLit=false;
    void onPlace(Level* level,int x,int y,int z)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    void tick(Level* level,int x,int y,int z,Random* random)override;
};
class TrapDoorTile:public Tile {
public:
    bool isPathfindable(LevelSource* level,int x,int y,int z)override;
    TILE_CONSTANTS_TrapDoorTile
    void updateShape(LevelSource* level,int x,int y,int z,int forceData=-1,shared_ptr<TileEntity> forceEntity=nullptr)override;
    void setShape(int data);
    using Tile::setShape;
    void attack(Level* level,int x,int y,int z,shared_ptr<Player> player)override;
    bool TestUse()override;
    bool use(Level* level,int x,int y,int z,shared_ptr<Player> player,int clickedFace,float clickX,float clickY,float clickZ,bool soundOnly=false)override;
    void setOpen(Level* level,int x,int y,int z,bool shouldOpen);
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    static int getDir(int dir);
    int getPlacedOnFaceDataValue(Level* level,int x,int y,int z,int face,float clickX,float clickY,float clickZ,int itemValue)override;
    bool mayPlace(Level* level,int x,int y,int z,int face)override;
    using Tile::mayPlace;
    static bool isOpen(int data);
    static bool attachesTo(int id);
};
class FenceGateTile:public DirectionalTile {
public:
    bool isPathfindable(LevelSource* level,int x,int y,int z)override;
    TILE_CONSTANTS_FenceGateTile
    bool mayPlace(Level* level,int x,int y,int z)override;
    void setPlacedBy(Level* level,int x,int y,int z,shared_ptr<Mob> by)override;
    bool TestUse()override{return true;}
    bool use(Level* level,int x,int y,int z,shared_ptr<Player> player,int clickedFace,float clickX,float clickY,float clickZ,bool soundOnly=false)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    static bool isOpen(int data);
};
// Only their push reactions (and isRail): beds and rails are otherwise not ported yet.
class BedTile:public DirectionalTile { public: int getPistonPushReaction()override; };
class RailTile:public Tile {
public:
    int getPistonPushReaction()override;
    static bool isRail(Level* level,int x,int y,int z);
    static bool isRail(int id);
};
// Container and DispenserTileEntity (the items; the host saves them back).
class Container {
public:
    static const int LARGE_MAX_STACK_SIZE=64;
    virtual ~Container()=default;
    virtual unsigned int getContainerSize()=0;
    virtual shared_ptr<ItemInstance> getItem(unsigned int slot)=0;
    virtual void setItem(unsigned int slot,shared_ptr<ItemInstance> item)=0;
};
class DispenserTileEntity:public TileEntity,public Container {
public:
    ItemInstanceArray* items;
    Random* random;
    DispenserTileEntity();
    ~DispenserTileEntity()override;
    unsigned int getContainerSize()override;
    shared_ptr<ItemInstance> getItem(unsigned int slot)override;
    shared_ptr<ItemInstance> removeItem(unsigned int slot,int count);
    int getRandomSlot();
    void setItem(unsigned int slot,shared_ptr<ItemInstance> item)override;
    int addItem(shared_ptr<ItemInstance> item);
    int getMaxStackSize();
    void setChanged(){}
};
class DispenserTile:public EntityTile {
public:
    TILE_CONSTANTS_DispenserTile
    // DispenserTile::DispenserTile: random = new Random() (freed with the
    // registry, which the source never tears down).
    Random* random=new Random();
    ~DispenserTile()override{delete random;}
    int getTickDelay()override;
    void onPlace(Level* level,int x,int y,int z)override;
    void recalcLockDir(Level* level,int x,int y,int z);
    bool TestUse()override;
    bool use(Level* level,int x,int y,int z,shared_ptr<Player> player,int clickedFace,float clickX,float clickY,float clickZ,bool soundOnly=false)override;
    void fireArrow(Level* level,int x,int y,int z,Random* random);
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    void tick(Level* level,int x,int y,int z,Random* random)override;
    void setPlacedBy(Level* level,int x,int y,int z,shared_ptr<Mob> by)override;
    void onRemove(Level* level,int x,int y,int z,int id,int data)override;
    void throwItem(Level* level,shared_ptr<ItemInstance> item,Random* random,int accuracy,int xd,int zd,double xp,double yp,double zp);
    int dispenseItem(shared_ptr<DispenserTileEntity> trap,Level* level,shared_ptr<ItemInstance> item,Random* random,int x,int y,int z,int xd,int zd,double xp,double yp,double zp);
};
class PistonPieceEntity;
class PistonBaseTile:public Tile {
public:
    TILE_CONSTANTS_PistonBaseTile
    // PistonBaseTile.h: static const float PLATFORM_THICKNESS = 4.0f (defined in the .cpp).
    static constexpr float PLATFORM_THICKNESS=4.0f;
    static DWORD tlsIdx;
    bool isSticky=false;
    static bool ignoreUpdate();
    static void ignoreUpdate(bool set);
    bool use(Level* level,int x,int y,int z,shared_ptr<Player> player,int clickedFace,float clickX,float clickY,float clickZ,bool soundOnly=false)override;
    void setPlacedBy(Level* level,int x,int y,int z,shared_ptr<Mob> by)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    void onPlace(Level* level,int x,int y,int z)override;
    void checkIfExtend(Level* level,int x,int y,int z);
    bool getNeighborSignal(Level* level,int x,int y,int z,int facing);
    void triggerEvent(Level* level,int x,int y,int z,int param1,int facing)override;
    void updateShape(LevelSource* level,int x,int y,int z,int forceData=-1,shared_ptr<TileEntity> forceEntity=nullptr)override;
    static int getFacing(int data);
    static bool isExtended(int data);
    static int getNewFacing(Level* level,int x,int y,int z,shared_ptr<Player> player);
    static bool isPushable(int block,Level* level,int cx,int cy,int cz,bool allowDestroyable);
    static bool canPush(Level* level,int sx,int sy,int sz,int facing);
    // 4J: stop the client level sharing the chunk's tiles (one process here).
    static void stopSharingIfServer(Level*,int,int,int){}
    bool createPush(Level* level,int sx,int sy,int sz,int facing);
};
class PistonExtensionTile:public Tile {
public:
    TILE_CONSTANTS_PistonExtensionTile
    void addAABBs(Level* level,int x,int y,int z,AABB* box,AABBList* boxes,shared_ptr<Entity> source)override;
    void onRemove(Level* level,int x,int y,int z,int id,int data)override;
    bool mayPlace(Level* level,int x,int y,int z)override;
    bool mayPlace(Level* level,int x,int y,int z,int face)override;
    void updateShape(LevelSource* level,int x,int y,int z,int forceData=-1,shared_ptr<TileEntity> forceEntity=nullptr)override;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    static int getFacing(int data);
};
class PistonMovingPiece:public EntityTile {
public:
    void onPlace(Level* level,int x,int y,int z)override;
    void onRemove(Level* level,int x,int y,int z,int id,int data)override;
    bool mayPlace(Level* level,int x,int y,int z)override;
    bool mayPlace(Level* level,int x,int y,int z,int face)override;
    bool TestUse()override{return false;}
    bool use(Level* level,int x,int y,int z,shared_ptr<Player> player,int clickedFace,float clickX,float clickY,float clickZ,bool soundOnly=false)override;
    void spawnResources(Level* level,int x,int y,int z,int data,float odds,int playerBonus)override;
    using Tile::spawnResources;
    void neighborChanged(Level* level,int x,int y,int z,int type)override;
    static shared_ptr<TileEntity> newMovingPieceEntity(int block,int data,int facing,bool extending,bool isSourcePiston);
    AABB* getAABB(Level* level,int x,int y,int z)override;
    AABB* getAABB(Level* level,int x,int y,int z,int tile,float progress,int facing);
    void updateShape(LevelSource* level,int x,int y,int z,int forceData=-1,shared_ptr<TileEntity> forceEntity=nullptr)override;
    shared_ptr<PistonPieceEntity> getEntity(LevelSource* level,int x,int y,int z);
};
class PistonPieceEntity:public TileEntity {
public:
    int id=0,data=0,facing=0;
    bool extending=false,_isSourcePiston=false;
    float progress=0,progressO=0;
    PistonPieceEntity();
    PistonPieceEntity(int id,int data,int facing,bool extending,bool isSourcePiston);
    int getId();
    int getData();
    bool isExtending();
    int getFacing();
    bool isSourcePiston();
    float getProgress(float a);
    float getXOff(float a);
    float getYOff(float a);
    float getZOff(float a);
    void moveCollidedEntities(float progress,float amount);
    void finalTick();
    void tick()override;
};
class StairTile:public Tile { public: TILE_CONSTANTS_StairTile };
class HalfSlabTile:public Tile { public: TILE_CONSTANTS_HalfSlabTile };

// Items whose useOn the port has from the source (hoes, seeds, bone meal,
// cocoa beans and flint and steel).
class Item {
public:
#include "ItemIds.inc"
    static Random* random;
    int id=0;
    Item()=default;
    explicit Item(int id):id(id){}
    virtual ~Item()=default;
    static Item *bucket_empty,*bucket_water,*bucket_lava;
    // Item::items[id] for the classes the dispenser casts to.
    static Item* byId(int id);
};
inline ItemInstance::ItemInstance(Item* item):id(item->id),count(1),auxValue(0){}
inline Item* ItemInstance::getItem(){return Item::byId(id);}
class BucketItem:public Item {
public:
    int content=0;
    BucketItem(int id,int content):Item(id),content(content){}
    bool emptyBucket(Level* level,double x,double y,double z,int xt,int yt,int zt);
};
class MinecartItem:public Item {
public:
    int type=0;
    MinecartItem(int id,int type):Item(id),type(type){}
};
// PotionBrewing.h's throwable bit.
struct PotionBrewing { static const int THROWABLE_BIT=14,THROWABLE_MASK=(1<<THROWABLE_BIT); };
class PotionItem:public Item { public: static bool isThrowable(int auxValue); };
// MonsterPlacerItem::canSpawn: EntityIO::newById and the per-type
// Level::canCreateMore limits are the host's (Level::canSpawnEgg); the mob
// carries its egg's id to Level::addEntity.
class EggMob:public Mob {
public:
    int entityId=0;
    EggMob():Mob(nullptr){}
    int getMaxHealth()override{return 20;}
};
class MonsterPlacerItem:public Item {
public:
    static shared_ptr<Entity> canSpawn(int iAuxVal,Level* level,int*){
        if(!level->canSpawnEgg(iAuxVal))return nullptr;
        auto mob=std::make_shared<EggMob>();mob->entityId=iAuxVal;
        return mob;
    }
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
// Level::isSolidBlockingTile for one tile, RedStoneDustTile::shouldConnectTo
// over a world read through `tile`/`data`, and a tile's shape
// (Tile::updateShape, or the default shape) as x0, y0, z0, x1, y1, z1.
bool isSolidBlockingTile(int tile);
bool dustShouldConnectTo(const std::function<int(int,int,int)>& tile,const std::function<int(int,int,int)>& data,
                         int x,int y,int z,int direction);
std::array<float,6> tileShape(int tile,int data);
// Tile::addAABBs for one tile (the piston head's plate and arm), relative to
// its cell.
std::vector<std::array<float,6>> tileCollisionBoxes(int tile,int data);
// Whether a scheduled tick of this tile runs original code here; the ticks of
// the other tiles stay in the saved chunk untouched.
bool tickPorted(int id);
// Item::useOn for the registered item id; false when the item has no
// ported useOn or does not apply. `face` is Facing (0 down ... 5 east).
bool useItemOn(Level& level,ItemInstance& item,int x,int y,int z,int face);
}
