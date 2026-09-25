#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ContainerItems.h"
#include "PotionEffects.h"
namespace console {
enum Block : std::uint8_t { Air=0, Stone=1, Grass=2, Dirt=3, Cobble=4, Planks=5,
    Bedrock=7, Water=9, Lava=10, Sand=12, Log=17, Leaves=18, Glass=20, Sandstone=24,
    Wool=35, Bricks=45, Obsidian=49, Ice=79, Fence=85, FenceGate=107, Mycelium=110, NetherFence=113 };
constexpr std::array<Block,9> palette{Grass,Stone,Cobble,Planks,Log,Leaves,Sand,Glass,Bricks};
constexpr std::array<Block,16> creativePalette{Grass,Stone,Cobble,Planks,Log,Leaves,Sand,Glass,Bricks,Fence,FenceGate,NetherFence,static_cast<Block>(130),static_cast<Block>(61),static_cast<Block>(118),static_cast<Block>(117)};
const char* blockName(Block b);
int textureTile(Block b, int face, int data=0);
bool solid(Block b);
bool validBlock(std::uint8_t b);
struct Vec3 { double x=0,y=0,z=0; };
struct Hit { bool hit=false; int x=0,y=0,z=0, px=0,py=0,pz=0; double distance=0; };
struct SimulatedEntity {
    std::wstring id;Vec3 position,velocity;int age=0;
    bool native=false;int nativeChunkX=0,nativeChunkZ=0,recordIndex=-1;float yaw=0;
    bool sheared=false;int woolColor=0,profession=0,catType=0,collarColor=14;
    bool wolfTame=false,wolfAngry=false,sitting=false;
    int slimeSize=1,jumpDelay=0;
    int health=20,hurtTicks=0,deathTicks=0,invulnerableTicks=0,lastHurt=0,attackTicks=0;
    int lastHurtByPlayerTicks=0;
    Vec3 motionTarget{},swimDirection{};
    int motionTimer=0,heightOffsetTimer=0;
    double blazeHeightOffset=.5,squidPhase=0,squidPhaseSpeed=.15,squidSpeed=0;
    float squidTentacleAngle=0;
    double wanderX=0,wanderZ=0;int wanderTicks=0;
};
struct ExperienceOrbState {
    Vec3 position,velocity;
    int value=1,age=0,health=5,throwTime=0;
    bool native=false;
    int nativeChunkX=0,nativeChunkZ=0,recordIndex=-1;
};
struct HangingDecoration {
    enum class Kind { Painting, ItemFrame } kind=Kind::Painting;
    int tileX=0,tileY=0,tileZ=0,dir=0;
    int nativeChunkX=0,nativeChunkZ=0,recordIndex=-1;
    std::wstring motive;
    int itemId=0,itemDamage=0,itemRotation=0;
};
struct SkullInfo {int type=0,rotation=0;};
// ItemEntity: a dropped stack. `stack` is the complete item compound (id,
// Count, Damage and any tag) so enchantments survive being dropped.
struct DroppedItem {
    Vec3 position,velocity;
    int id=0,count=1,damage=0;
    int age=0,throwTime=0,health=5;
    float bobOffset=0;
    std::shared_ptr<class CompoundTag> stack;
};
struct CraftingRecipe;
struct TutorialLevelRules;

// Bounded resident window over the original finite console world.
// Generation uses the original biome, density, surface, cave and canyon stages.
class World {
    struct State;
    std::unique_ptr<State> state;
    std::vector<std::array<int,3>> chestParts(int x,int y,int z)const;
    void discardContainerData(int x,int y,int z,const wchar_t* id);
    void ensureEnderChestData(int x,int y,int z);
    void ensureFurnaceData(int x,int y,int z);
    void tickFurnaces();
    void ensureBrewingData(int x,int y,int z);
    void tickBrewingStands();
    void scheduleFluid(int x,int y,int z,int delay);
    void activateFluidChunks();
    void activateFluidChunk(int chunkX,int chunkZ);
    void tickFluids();
    void putFluid(int x,int y,int z,int id,int data);
    void flowFluid(int x,int y,int z);
    // ServerLevel::tickTiles (random tile ticks, freezing, snow, rain) and
    // Level::tickWeather; WorldTiles.cpp.
    void tickTiles();
    void tickWeather();
    // Tile::onRemove for a replaced tile (trunks and leaves flag decay).
    void tileRemoved(int x,int y,int z,int tile,int data);
    friend class WorldTickLevel;
    void saveFluidTicks(class ChunkRecord& record,bool remove,bool keepSavedFluids=false);
    void loadFluidTicks(const class ChunkRecord& record);
    void tickEntities();
    void tickExperienceOrbs();
    void spawnExperienceOrbs(Vec3 position,int reward);
    void tickPlayerEffects();
    void saveEntities(class ChunkRecord& record,bool remove);
    void tickDroppedItems();
    void mergeDroppedItem(DroppedItem& item);
    void tickPlayerSurvival();
    void handlePlayerDeath();
    void spawnDroppedItem(Vec3 position,Vec3 velocity,std::unique_ptr<class CompoundTag> stack,int throwTime);
    int addCarriedStack(class CompoundTag& stack);
    bool playerEyeInWater()const;
    bool playerTouches(int tileA,int tileB,double shrinkX,double shrinkY)const;
    // Capture a chunk for the archive. A chunk already archived by the current
    // eviction pass is re-captured from that archived record, which holds the
    // entities and fluid ticks the first pass removed from live state.
    std::unique_ptr<class ChunkRecord> captureChunk(std::pair<int,int> key,bool remove);
    void loadEntities(class ChunkRecord& record);
public:
    static constexpr int width=128, height=256, depth=128, sea=63;
    std::int64_t seed=0;
    std::uint64_t revision=0;
    World();
    ~World();
    World(const World&)=delete;
    World& operator=(const World&)=delete;
    void swapWith(World& other) noexcept;
    std::vector<std::uint8_t> blockSnapshot()const;
    void generate(std::int64_t seedValue,bool flat=false);
    // Import the supplied tutorial terrain, schematics, tile records, and entities.
    void generateTutorial(const std::filesystem::path& tutorialAssets);
    void generateArchivedTutorial(const std::filesystem::path& tutorialAssets);
    bool isTutorial()const;
    // The supplied tutorial's LevelRules and languages.loc strings (null for
    // other worlds, including the archived tutorial).
    const TutorialLevelRules* tutorialRules()const;
    const std::map<std::wstring,std::wstring>* tutorialStrings()const;
    // MinecraftServer::setSpawnSettings(... && !Minecraft::isTutorial()):
    // natural spawning stays off in a tutorial world until the player leaves
    // the tutorial.
    void setTutorialSpawning(bool enabled);
    bool isFlat()const;
    int originX()const;
    int originZ()const;
    int worldMin()const;
    int worldMax()const; // exclusive, in client coordinates
    // Prepare at most chunkBudget missing chunks, then atomically move the window.
    // Returns true when the visible window changes and its mesh needs rebuilding.
    bool streamAround(Vec3 player,int chunkBudget=2);
    bool streaming()const;
    std::size_t residentChunks()const;
    bool inside(int x,int y,int z) const;
    Block get(int x,int y,int z) const;
    bool set(int x,int y,int z,Block block);
    bool canOpenChest(int x,int y,int z)const;
    bool canOpenEnderChest(int x,int y,int z)const;
    bool canOpenFurnace(int x,int y,int z)const;
    bool canOpenBrewingStand(int x,int y,int z)const;
    std::vector<ContainerItem> chestItems(int x,int y,int z)const;
    std::vector<ContainerItem> enderChestItems()const;
    std::vector<ContainerItem> furnaceItems(int x,int y,int z)const;
    std::vector<ContainerItem> brewingItems(int x,int y,int z)const;
    int brewingProgress(int x,int y,int z)const;
    struct FurnaceState {int burn=0,cook=0,duration=200;};
    FurnaceState furnaceState(int x,int y,int z)const;
    std::vector<ContainerItem> carriedItems()const;
    bool setCreativeHotbarItem(int slot,int id,int damage=0,int count=1);
    bool swapCarriedSlots(int first,int second);
    bool drinkPotion(int damage);
    const std::vector<PotionEffect>& activePotionEffects()const;
    int potionEffectDuration(int id)const;
    double potionSpeedMultiplier()const;
    int playerHealth()const;
    int playerFoodLevel()const;
    float playerSaturation()const;
    int playerExperienceLevel()const;
    int playerTotalExperience()const;
    float playerExperienceProgress()const;
    bool giveCreativeItem(int id,int damage=0);
    bool transferChestItem(int x,int y,int z,int slot,bool take,int amount=-1);
    bool transferEnderChestItem(int x,int y,int z,int slot,bool take,int amount=-1);
    bool transferFurnaceItem(int x,int y,int z,int slot,bool take,int targetSlot=0,int amount=-1);
    bool transferBrewingItem(int x,int y,int z,int slot,bool take,int targetSlot=0,int amount=-1);
    bool breakBlock(int x,int y,int z);
    bool useBlock(int x,int y,int z);
    bool useCauldron(int x,int y,int z,int heldItemId);
    bool placeBlock(int x,int y,int z,Block block,int data,Vec3 feet,double yaw);
    int getData(int x,int y,int z)const;
    bool setData(int x,int y,int z,int data);
    // Call after an accepted edit. Returns fizz locations for the future effects system.
    // Raw set/setData deliberately remain no-update storage operations.
    std::vector<Vec3> updateLiquidNeighbors(int x,int y,int z);
    int skyLight(int x,int y,int z)const;
    int blockLight(int x,int y,int z)const;
    int renderLight(int x,int y,int z,bool liquid=false)const;
    float skyDarken()const;
    std::int64_t time()const;
    // Level::setOverrideTimeOfDay (-1 clears): the tutorial freezes the time
    // of day. dayTime() is what sky colour, darkness and the sun read.
    void setOverrideTimeOfDay(std::int64_t timeOfDay);
    std::int64_t dayTime()const;
    void setTime(std::int64_t value);
    float rainLevel()const;
    float thunderLevel()const;
    std::array<float,3> skyColour(int x,int z)const;
    std::array<int,9> neighboringBiomes(int x,int z)const;
    void tickTime();
    void setPlayerPosition(Vec3 position);
    bool spawnCreativeEgg(int entityId,Vec3 position);
    bool attackEntity(Vec3 eye,Vec3 direction,int heldItemId,double reach=6);
    // GameRenderer::pick: the living entity under the crosshair (nearer than
    // any block), by entity id.
    std::optional<std::wstring> pickEntity(Vec3 eye,Vec3 direction,double reach)const;
    const std::vector<SimulatedEntity>& entities()const;
    const std::vector<ExperienceOrbState>& experienceOrbs()const;
    const std::vector<HangingDecoration>& hangingDecorations()const;
    std::optional<SkullInfo> skullInfo(int x,int y,int z)const;
    void setName(const std::string& name);
    // ---- Survival (GameType SURVIVAL) ----
    bool survival()const;
    void setSurvival(bool enabled);
    // Tile::getDestroyProgress for the block and the carried slot (per tick).
    float destroyProgress(int x,int y,int z,int slot)const;
    // Player destroys a block: survival drops (only if the tool can harvest
    // it), tool wear and mining exhaustion; creative simply removes it.
    bool destroyBlock(int x,int y,int z,int slot);
    // Survival placement and eating use up the carried item.
    bool consumeCarried(int slot,int amount=1);
    // ItemInstance::hurt on a carried tool in survival (removed when it breaks).
    void wearCarried(int slot,int amount);
    // Item::useOn for the farming items (hoes, seeds, carrots, potatoes,
    // nether wart, bone meal, cocoa beans) at tile x,y,z clicked on `face`
    // (Facing: 0 down, 1 up, 2 north, 3 south, 4 west, 5 east). True when
    // the item was used; the stack and blocks are updated.
    bool useItemOn(int x,int y,int z,int face,int slot);
    // Inventory::add; returns how many items did not fit.
    int addCarriedItem(int id,int count,int damage=0);
    // Player::drop(item,false) from the carried slot (one item or the stack).
    bool dropCarried(int slot,bool wholeStack,Vec3 eye,double yaw,double pitch);
    const std::vector<DroppedItem>& droppedItems()const;
    // Player::causeFallDamage after landing, in blocks fallen.
    void playerLanded(double fallDistance);
    // Direct damage for effects and tests (Mob::hurt window, unscaled).
    bool hurtPlayer(int damage);
    // Player::checkMovementStatistics / jumpFromGround / attack exhaustion,
    // plus the held tool's hurtEnemy wear after a successful hit.
    void playerWalked(double meters,bool sprinting,bool swimming);
    void playerJumped(bool sprinting);
    void playerAttacked(int slot);
    bool playerInWater()const;
    // Entity::isUnderLiquid(Material::water): the eye is in water.
    bool playerUnderWater()const{return playerEyeInWater();}
    int playerAir()const;
    // Gui heart blink inputs: Mob::invulnerableTime and Mob::lastHealth.
    int playerInvulnerableTicks()const;
    int playerLastHealth()const;
    int playerFireTicks()const;
    bool playerDead()const;
    void respawnPlayer();
    // FoodItem: may this slot be eaten now, and eat it (after 32 use ticks).
    bool canEatCarried(int slot)const;
    bool eatCarried(int slot);
    bool canCraft(const CraftingRecipe& recipe)const;
    bool craft(const CraftingRecipe& recipe);
    // Feet position saved with the player, if the save had one.
    std::optional<Vec3> savedPlayerPosition()const;
    // UpdatePlayerRuleDefinition::postProcessPlayer: lastHealth/setHealth and
    // FoodData::setFoodLevel.
    void setPlayerHealth(int health);
    void setPlayerFood(int food);
    // Inventory::setItem for slots 0-35 (dataTag is the 4jdata marker).
    bool setCarriedItem(int slot,int id,int count,int damage,int dataTag=0);
    int surface(int x,int z) const;
    Vec3 spawn() const;
    bool collides(Vec3 feet) const;
    bool collides(Vec3 feet,double width,double entityHeight) const;
    Hit raycast(Vec3 origin, Vec3 direction, double reach=6) const;
    void save(const std::filesystem::path& path);
    bool load(const std::filesystem::path& path);
};
}
