// File: src/common/entity/npc/Villager.hpp
//
// MC net.minecraft.world.entity.npc.villager.{AbstractVillager, Villager}.
//
// ── What a villager is ───────────────────────────────────────────────────────
// A brain mob (VillagerAi.cpp ports VillagerGoalPackages) with an identity —
// VillagerData: the biome TYPE it was born to, its PROFESSION and its LEVEL —
// and a trader's books: a lazily generated MerchantOffers list, a merchant XP
// total that levels it up, a food level that decides whether it will breed,
// an 8-slot inventory, and a GossipContainer that turns what it has heard
// about players into prices.
//
// Its day runs off a SCHEDULE (the 26.3 `gameplay/villager_activity`
// timeline): IDLE, WORK at the claimed job site, MEET at the bell, IDLE,
// REST in the claimed bed. Job sites, beds and bells are points of interest
// claimed through the level's PoiManager — the villager holds the claim as a
// brain memory (JOB_SITE, HOME, MEETING_POINT) and releases it when it dies.
//
// ── What the client sees ─────────────────────────────────────────────────────
// MC syncs VillagerData and the unhappy counter as entity data. Here they ride
// the tracker's two per-type bytes (see PackVillagerVariant):
//   variant byte = type | level << 3
//   anim byte    = profession | unhappy << 4
// and the sleeping pose rides the pose byte like any other pose — the renderer
// reads the bed the villager lies in off the block at its feet.
//
// ── Trading ──────────────────────────────────────────────────────────────────
// MobInteract opens a MerchantMenu (common/inventory/MerchantMenu.hpp) through
// EntityLevel::OpenMerchantMenu; the server's session owns the menu and sends
// the offers (MerchantOffersS2C) whenever OffersRevision moves — which is how
// MC's resendOffersToTradingPlayer calls are carried out here.
#pragma once

#include "common/entity/ai/brain/GeneratedMemoryModules.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/entity/npc/GossipContainer.hpp"
#include "common/entity/npc/Merchant.hpp"
#include "common/entity/npc/VillagerData.hpp"
#include "common/inventory/SimpleContainer.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Game {

    class PoiManager;
    class Brain;

    // MC ReputationEventType.
    enum class ReputationEventType : uint8_t {
        ZombieVillagerCured, GolemKilled, VillagerHurt, VillagerKilled, Trade,
    };

    // ── AbstractVillager ─────────────────────────────────────────────────────

    class AbstractVillager : public GenericAgeableMob, public Merchant {
    public:
        static constexpr int kInventorySize = 8;   // MC SimpleContainer(8)

        AbstractVillager(EntityTypeId type, EntityLevel* level);

        // ── MC DATA_UNHAPPY_COUNTER — the head-shake ─────────────────────
        int  GetUnhappyCounter() const { return m_unhappyCounter; }
        void SetUnhappyCounter(int value) { m_unhappyCounter = value; }

        // ── Trading partner ──────────────────────────────────────────────
        // MC keeps the Player; this keeps the player's entity id in this
        // level and resolves it on use (a player who left the level simply
        // stops resolving, which reads as "no longer trading").
        LivingEntity* GetTradingPlayer() const;
        int32_t GetTradingPlayerId() const { return m_tradingPlayerId; }
        virtual void SetTradingPlayer(LivingEntity* player);
        bool IsTrading() const { return GetTradingPlayer() != nullptr; }
        // MC stopTrading → setTradingPlayer(null).
        void StopTrading() { SetTradingPlayer(nullptr); }
        // MC AbstractVillager.stillValid: still this villager's partner, alive,
        // and within the player's entity interaction range + 4.
        bool StillValid(const LivingEntity& player) const;

        // ── Merchant ─────────────────────────────────────────────────────
        MerchantOffers& GetOffers() override;
        // The saved list as-is (null before the first trade roll) — the
        // NBT writer must not generate offers just by saving.
        const std::optional<MerchantOffers>& PeekOffers() const { return m_offers; }
        void SetOffers(std::optional<MerchantOffers> offers) { m_offers = std::move(offers); BumpOffersRevision(); }
        void NotifyTrade(MerchantOffer& offer) override;
        void NotifyTradeUpdated(const ItemStack& result) override;
        int  GetVillagerXp() const override { return 0; }
        bool ShowProgressBar() const override { return true; }
        const char* GetNotifyTradeSound() const override { return "entity.villager.yes"; }
        bool IsClientSideMerchant() const override { return false; }
        void ClearTradingPlayer() override { StopTrading(); }
        // MC's merchant level for the screen title — 0 for a trader with no
        // levels (the wandering trader, whose title is its plain name).
        virtual int GetMerchantLevel() const { return 0; }
        // The trading screen's title (MC getDisplayName).
        virtual std::string GetMerchantTitle() const;

        // Bumped whenever the offers or the trader's XP/level change in a
        // way MC would push to the trading player (resendOffersToTradingPlayer
        // / updateSpecialPrices). The server's session compares it every tick.
        uint32_t OffersRevision() const { return m_offersRevision; }
        void     BumpOffersRevision() { ++m_offersRevision; }

        // ── Inventory (MC InventoryCarrier) ──────────────────────────────
        SimpleContainer&       GetInventory()       { return m_inventory; }
        const SimpleContainer& GetInventory() const { return m_inventory; }
        // MC SimpleContainer.addItem / canAddItem / countItem / removeItemType.
        ItemStack AddToInventory(const ItemStack& stack);
        bool CanAddToInventory(const ItemStack& stack) const;
        int  CountInventoryItem(ItemID item) const;
        void RemoveInventoryItemType(ItemID item, int count);

        // MC Mob.wantsToPickUp — per subclass.
        virtual bool WantsToPickUp(const ItemStack& stack) const { (void)stack; return false; }

        // MC AbstractVillager: never leashable, never despawns far away is
        // the Villager's (the trader despawns on its own timer).
        void Die(MobDamageSource source, Entity* attacker) override;

        // Particles in a small cloud around the upper body — MC
        // addParticlesAroundSelf (client).
        void AddParticlesAroundSelf(int kind);

    protected:
        // MC rewardTradeXp / updateTrades.
        virtual void RewardTradeXp(const MerchantOffer& offer) = 0;
        virtual void UpdateTrades() = 0;
        // MC addOffersFromTradeSet — `merchantType` for the villager/variant
        // predicate (the trader passes none).
        void AddOffersFromTradeSet(const std::string& key, MerchantOffers& offers,
                                   std::optional<VillagerType> merchantType);

        std::optional<MerchantOffers> m_offers;
        SimpleContainer               m_inventory{ kInventorySize };
        int32_t                       m_tradingPlayerId = -1;
        int                           m_unhappyCounter = 0;
        uint32_t                      m_offersRevision = 0;
    };

    // ── Villager ─────────────────────────────────────────────────────────────

    class Villager : public AbstractVillager {
    public:
        // MC constants.
        static constexpr int     kBreedingFoodThreshold = 12;     // BREEDING_FOOD_THRESHOLD
        static constexpr int     kMaxGossipTopics       = 10;     // MAX_GOSSIP_TOPICS
        static constexpr int     kGossipCooldown        = 1200;   // GOSSIP_COOLDOWN
        static constexpr int     kGossipDecayInterval   = 24000;  // GOSSIP_DECAY_INTERVAL
        static constexpr int     kGolemAgreeVillagers   = 5;      // HOW_MANY_VILLAGERS_NEED_TO_AGREE_TO_SPAWN_A_GOLEM
        static constexpr int64_t kTimeSinceSleepingForGolemSpawning = 24000;
        static constexpr float   kSpeedModifier         = 0.5f;   // SPEED_MODIFIER

        explicit Villager(EntityLevel* level);
        ~Villager() override;

        // ── VillagerData (MC DATA_VILLAGER_DATA / _FINALIZED) ────────────
        const VillagerData& GetVillagerData() const { return m_data; }
        // MC setVillagerData: a PROFESSION change drops the offers (they are
        // regenerated for the new job on the next read).
        void SetVillagerData(const VillagerData& data);
        bool GetVillagerDataFinalized() const { return m_dataFinalized; }
        void SetVillagerDataFinalized(bool v) { m_dataFinalized = v; }
        // MC VillagerDataHolder.finalizeVillagerType: a villager whose data was
        // never set takes its biome's type (a spawn egg in a snowy biome makes
        // a snow villager); one whose data came from NBT keeps it.
        void FinalizeVillagerType();

        // ── Merchant XP / level ──────────────────────────────────────────
        int  GetVillagerXp() const override { return m_villagerXp; }
        void SetVillagerXp(int xp) { m_villagerXp = xp; }
        int  GetMerchantLevel() const override { return m_data.level; }
        bool CanRestock() const override { return true; }
        std::string GetMerchantTitle() const override;

        // ── Trade reroll (the merchant screen's button) ──────────────────
        // MC ResetProfession's firing test: a profession that is not NONE or
        // NITWIT, getVillagerXp() == 0 and level <= 1. Once the villager has
        // traded (XP > 0) its profession — and so its trades — are locked.
        bool CanRerollTrades() const override;
        // What losing and re-taking the job site does to the offers: a fresh
        // list from this level's updateTrades. The profession, job site and
        // brain are left alone. Server-side; false when locked.
        bool RerollTrades() override;

        // ── Restocking (MC restock / shouldRestock / catchUpDemand) ──────
        void Restock();
        bool ShouldRestock();
        int64_t GetLastRestockGameTime() const { return m_lastRestockGameTime; }
        void    SetLastRestockGameTime(int64_t t) { m_lastRestockGameTime = t; }
        int     GetRestocksToday() const { return m_numberOfRestocksToday; }
        void    SetRestocksToday(int n) { m_numberOfRestocksToday = n; }

        // ── Reputation & gossip ──────────────────────────────────────────
        GossipContainer&       GetGossips()       { return m_gossips; }
        const GossipContainer& GetGossips() const { return m_gossips; }
        int64_t GetLastGossipDecayTime() const { return m_lastGossipDecayTime; }
        void    SetLastGossipDecayTime(int64_t t) { m_lastGossipDecayTime = t; }
        int  GetPlayerReputation(const LivingEntity& player) const;
        // MC ReputationEventHandler.onReputationEventFrom.
        void OnReputationEventFrom(ReputationEventType type, const Entity& source);
        // MC gossip(level, target, timestamp) — both villagers' 1200-tick
        // cooldown, a transfer, then maybe an iron golem.
        void Gossip(Villager& target, int64_t timestamp);
        // MC spawnGolemIfNeeded / wantsToSpawnGolem.
        void SpawnGolemIfNeeded(int64_t timestamp, int villagersNeededToAgree);
        bool WantsToSpawnGolem(int64_t timestamp) const;

        // ── Food & breeding ──────────────────────────────────────────────
        int  GetFoodLevel() const { return m_foodLevel; }
        void SetFoodLevel(int f) { m_foodLevel = f; }
        // MC canBreed: food in the belly plus food in the pockets, awake, adult.
        bool CanBreed() const;
        void EatAndDigestFood();
        bool HasExcessFood() const { return CountFoodPointsInInventory() >= 24; }
        bool WantsMoreFood() const { return CountFoodPointsInInventory() < kBreedingFoodThreshold; }
        bool HasFarmSeeds() const;
        int  CountFoodPointsInInventory() const;
        // MC getBreedOffspring: the child's type is the biome's (50%), this
        // parent's (25%) or the partner's (25%); it starts unemployed.
        std::unique_ptr<Villager> MakeBreedOffspring(const Villager& partner);

        bool WantsToPickUp(const ItemStack& stack) const override;

        // ── Sleeping (MC LivingEntity.startSleeping / stopSleeping) ──────
        bool IsSleeping() const;
        std::optional<glm::ivec3> GetSleepingPos() const { return m_sleepingPos; }
        bool StartSleeping(const glm::ivec3& bedPos);
        void StopSleeping();

        // ── POI claims ───────────────────────────────────────────────────
        // MC releasePoi(memory) — hand back the ticket behind a POI memory
        // (when the POI is still of the type the memory expects).
        void ReleasePoi(MemoryModule memory);
        void ReleaseAllPois();

        // ── Work ─────────────────────────────────────────────────────────
        void PlayWorkSound();
        // Farmers' neighbouring farmland (MC SECONDARY_JOB_SITE), written by
        // the SecondaryPoiSensor; the brain memory only marks its presence
        // (the engine's memory kinds carry no position list).
        const std::vector<glm::ivec3>& GetSecondaryJobSites() const { return m_secondaryJobSites; }
        void SetSecondaryJobSites(std::vector<glm::ivec3> sites) { m_secondaryJobSites = std::move(sites); }
        // MC DOORS_TO_CLOSE — the doors this villager opened and will shut.
        std::vector<glm::ivec3>& DoorsToClose() { return m_doorsToClose; }
        // MC NEAREST_VISIBLE_WANTED_ITEM — dropped items are not Entities in
        // this engine, so the sensor keeps the item entity's id here.
        std::optional<int32_t> GetWantedItemId() const { return m_wantedItemId; }
        void SetWantedItemId(std::optional<int32_t> id) { m_wantedItemId = id; }

        // ── Brain ────────────────────────────────────────────────────────
        // MC refreshBrain: stop everything, rebuild the activities for the
        // current profession / age, keep the memories. Deferred to the end
        // of the brain tick when asked for from inside a behaviour.
        void RefreshBrain();
        void RequestBrainRefresh() { m_brainRefreshPending = true; }
        // MC Brain.updateActivityFromSchedule (throttled to every 20 ticks).
        void UpdateActivityFromSchedule();
        void ResetScheduleThrottle() { m_lastScheduleUpdate = -9999; }

        // ── Mob hooks ────────────────────────────────────────────────────
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        void Die(MobDamageSource source, Entity* attacker) override;
        void Tick() override;
        void AiStep() override;
        void HandleEntityEvent(uint8_t id) override;
        bool RemoveWhenFarAway(double) const override { return false; }
        const char* GetAmbientSound() const override;
        const char* GetHurtSound(MobDamageSource) const override { return "entity.villager.hurt"; }
        const char* GetDeathSound() const override { return "entity.villager.death"; }

        float BaseBbWidth()   const override;
        float BaseBbHeight()  const override;
        float BaseEyeHeight() const override;

        uint8_t GetVariantByte() const override { return PackVillagerVariant(m_data); }
        void    SetVariantByte(uint8_t v) override;
        uint8_t GetAnimStateByte() const override;
        void    SetAnimStateByte(uint8_t v) override;
        // The head shake (MC DATA_UNHAPPY_COUNTER > 0): the server's counter,
        // or on a client the synched bit.
        bool    IsUnhappy() const { return m_clientUnhappy || GetUnhappyCounter() > 0; }

        // True while loading so a restored JOB_SITE/HOME/MEETING_POINT
        // memory re-takes its ticket on the next server tick.
        void MarkPoiTicketsForRestore() { m_poiTicketsRestored = false; }

    protected:
        void CustomServerAiStep() override;
        void UpdateBrainActivity() override;
        void RewardTradeXp(const MerchantOffer& offer) override;
        void UpdateTrades() override;

    private:
        void SetUnhappy();
        void StartTrading(LivingEntity& player);
        void UpdateSpecialPrices(LivingEntity& player);
        void ResetSpecialPrices();
        bool NeedsToRestock() const;
        bool AllowedToRestock() const;
        void CatchUpDemand();
        void UpdateDemand();
        void ResetNumberOfRestocks();
        bool ShouldIncreaseLevel() const;
        void IncreaseMerchantCareer();
        void MaybeDecayGossip();
        void EatUntilFull();
        bool GolemSpawnConditionsMet(int64_t gameTime) const;
        void TellWitnessesThatIWasMurdered(Entity& murderer);
        void RestorePoiTickets();
        void PickUpNearbyItems();
        PoiManager* Poi() const;

        VillagerData m_data;
        bool         m_dataFinalized = false;
        int          m_foodLevel = 0;
        GossipContainer m_gossips;
        int64_t      m_lastGossipTime = 0;
        int64_t      m_lastGossipDecayTime = 0;
        int          m_villagerXp = 0;
        int64_t      m_lastRestockGameTime = 0;
        int          m_numberOfRestocksToday = 0;
        int64_t      m_lastRestockCheckDay = 0;
        int32_t      m_lastTradedPlayerId = -1;

        std::optional<glm::ivec3> m_sleepingPos;
        std::vector<glm::ivec3>   m_secondaryJobSites;
        std::vector<glm::ivec3>   m_doorsToClose;
        std::optional<int32_t>    m_wantedItemId;

        int64_t m_lastScheduleUpdate = -9999;
        bool    m_brainRefreshPending = false;
        bool    m_poiTicketsRestored = true;
        bool    m_wasBaby = false;
        // Client mirror of the synched unhappy bit.
        bool    m_clientUnhappy = false;
    };

} // namespace Game
