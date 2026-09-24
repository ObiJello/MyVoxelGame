// File: src/common/entity/npc/Villager.cpp
#include "common/entity/npc/Villager.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/text/Language.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/entity/ai/brain/VillagerAi.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/ai/village/PoiManager.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/npc/VillagerTrades.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/block/BedBlock.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/fluid/FluidState.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace {
        LivingEntity* ResolvePlayerById(EntityLevel* level, int32_t id) {
            if (!level || id < 0) return nullptr;
            std::vector<LivingEntity*> players;
            level->GetPlayers(players);
            for (LivingEntity* p : players) {
                if (p && p->GetId() == id && p->IsAlive()) return p;
            }
            return nullptr;
        }

        // The memories MC's Brain.pack() keeps across refreshBrain — the ones
        // registered with a codec (MemoryModuleType.register(name, codec)).
        constexpr MemoryModule kSerializableMemories[] = {
            MemoryModule::Home, MemoryModule::JobSite, MemoryModule::PotentialJobSite,
            MemoryModule::MeetingPoint, MemoryModule::GolemDetectedRecently,
            MemoryModule::DangerDetectedRecently, MemoryModule::LastSlept,
            MemoryModule::LastWoken, MemoryModule::LastWorkedAtPoi,
        };
    }

    // ═════════════════════════════════════════════════════════════════════
    // AbstractVillager
    // ═════════════════════════════════════════════════════════════════════

    AbstractVillager::AbstractVillager(EntityTypeId type, EntityLevel* level)
        : GenericAgeableMob(type, level) {
        // MC's villagers are all brain (Villager) or all custom goals (the
        // trader registers its own); the generic ageable set goes.
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        // MC AbstractVillager: FIRE_IN_NEIGHBOR 16, FIRE -1.
        SetPathfindingMalus(PathType::DangerFire, 16.0f);
        SetPathfindingMalus(PathType::DamageFire, -1.0f);
    }

    LivingEntity* AbstractVillager::GetTradingPlayer() const {
        return ResolvePlayerById(m_level, m_tradingPlayerId);
    }

    void AbstractVillager::SetTradingPlayer(LivingEntity* player) {
        m_tradingPlayerId = player ? player->GetId() : -1;
    }

    bool AbstractVillager::StillValid(const LivingEntity& player) const {
        // MC: getTradingPlayer() == player && isAlive() &&
        // player.isWithinEntityInteractionRange(this, 4.0) — the AABB
        // distance from the eye, inside entity_interaction_range (3, +2 in
        // creative) plus the 4 extra.
        if (m_tradingPlayerId != player.GetId() || !IsAlive()) return false;
        const double range = 3.0 + (player.IsCreative() ? 2.0 : 0.0) + 4.0;
        return GetAABBd().DistanceToSqr(player.GetEyePosition()) < range * range;
    }

    MerchantOffers& AbstractVillager::GetOffers() {
        // MC throws on the client ("Cannot load Villager offers on the
        // client"); the client never trades against the entity (its menu has
        // a ClientSideMerchant), so an empty list is the safe answer.
        if (!m_level || m_level->IsClientSide()) {
            static MerchantOffers s_none;
            s_none.clear();
            return s_none;
        }
        if (!m_offers) {
            m_offers.emplace();
            UpdateTrades();
            BumpOffersRevision();
        }
        return *m_offers;
    }

    std::string AbstractVillager::GetMerchantTitle() const {
        // MC getDisplayName → the type's name ("Wandering Trader").
        const std::string slug(TypeInfo().slug);
        return Language::GetOrDefault("entity.minecraft." + slug, slug);
    }

    void AbstractVillager::NotifyTrade(MerchantOffer& offer) {
        // MC AbstractVillager.notifyTrade (the TRADE criterion is skipped —
        // no advancements).
        offer.IncreaseUses();
        ResetAmbientSoundTime();
        RewardTradeXp(offer);
    }

    void AbstractVillager::NotifyTradeUpdated(const ItemStack& result) {
        // MC: at most one yes/no every 20 ticks of the ambient clock.
        if (m_level && !m_level->IsClientSide() &&
            m_ambientSoundTime > -GetAmbientSoundInterval() + 20) {
            ResetAmbientSoundTime();
            MakeSound(result.IsEmpty() ? "entity.villager.no" : "entity.villager.yes");
        }
    }

    void AbstractVillager::Die(MobDamageSource source, Entity* attacker) {
        GenericAgeableMob::Die(source, attacker);
        StopTrading();
    }

    void AbstractVillager::AddParticlesAroundSelf(int kind) {
        // MC addParticlesAroundSelf: five, gaussian*0.02 velocities, at
        // getRandomX(1.0), getRandomY() + 1.0, getRandomZ(1.0).
        if (!m_level || !m_level->IsClientSide()) return;
        JavaRandom& r = m_level->Random();
        const double w = GetBbWidth(), h = GetBbHeight();
        for (int i = 0; i < 5; ++i) {
            const double xa = r.NextGaussian() * 0.02;
            const double ya = r.NextGaussian() * 0.02;
            const double za = r.NextGaussian() * 0.02;
            const double x = position.x + w * (2.0 * r.NextDouble() - 1.0);
            const double y = position.y + h * r.NextDouble() + 1.0;
            const double z = position.z + w * (2.0 * r.NextDouble() - 1.0);
            m_level->AddParticle(static_cast<ParticleKind>(kind), x, y, z, xa, ya, za);
        }
    }

    void AbstractVillager::AddOffersFromTradeSet(const std::string& key, MerchantOffers& offers,
                                                 std::optional<VillagerType> merchantType) {
        if (!m_level) return;
        VillagerTrades::AddOffersFromTradeSet(key, offers, m_level->Random(), merchantType);
    }

    // ── Inventory (MC SimpleContainer) ───────────────────────────────────

    ItemStack AbstractVillager::AddToInventory(const ItemStack& stack) {
        // MC SimpleContainer.addItem: merge into matching stacks, then the
        // first empty slot; the remainder comes back.
        if (stack.IsEmpty()) return {};
        ItemStack rest = stack;
        const int maxStack = std::min(m_inventory.GetMaxStackSize(rest),
                                      ItemRegistry::Get(rest.itemId).maxStackSize);
        for (int i = 0; i < m_inventory.GetContainerSize() && !rest.IsEmpty(); ++i) {
            ItemStack& slot = m_inventory.GetItem(i);
            if (slot.IsEmpty() || !IsSameItemSameComponents(slot, rest)) continue;
            const int moved = std::min(rest.count, maxStack - slot.count);
            if (moved <= 0) continue;
            slot.count += moved;
            rest.count -= moved;
        }
        for (int i = 0; i < m_inventory.GetContainerSize() && !rest.IsEmpty(); ++i) {
            if (!m_inventory.GetItem(i).IsEmpty()) continue;
            m_inventory.SetItem(i, rest);
            rest.Clear();
        }
        if (rest.count <= 0) rest.Clear();
        return rest;
    }

    bool AbstractVillager::CanAddToInventory(const ItemStack& stack) const {
        // MC SimpleContainer.canAddItem: an empty slot, or a matching stack
        // with room.
        for (int i = 0; i < m_inventory.GetContainerSize(); ++i) {
            const ItemStack& slot = m_inventory.GetItem(i);
            if (slot.IsEmpty()) return true;
            if (IsSameItemSameComponents(slot, stack) &&
                slot.count < ItemRegistry::Get(slot.itemId).maxStackSize) {
                return true;
            }
        }
        return false;
    }

    int AbstractVillager::CountInventoryItem(ItemID item) const {
        int n = 0;
        for (int i = 0; i < m_inventory.GetContainerSize(); ++i) {
            const ItemStack& s = m_inventory.GetItem(i);
            if (s.itemId == item) n += s.count;
        }
        return n;
    }

    void AbstractVillager::RemoveInventoryItemType(ItemID item, int count) {
        // MC SimpleContainer.removeItemType — from the LAST slot backwards.
        for (int i = m_inventory.GetContainerSize() - 1; i >= 0 && count > 0; --i) {
            ItemStack& s = m_inventory.GetItem(i);
            if (s.itemId != item) continue;
            const int take = std::min(count, s.count);
            s.count -= take;
            count -= take;
            if (s.count <= 0) s.Clear();
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    // Villager
    // ═════════════════════════════════════════════════════════════════════

    Villager::Villager(EntityLevel* level)
        : AbstractVillager(EntityTypeId::Villager, level) {
        // MC Villager's constructor.
        GetNavigation().SetCanOpenDoors(true);
        GetNavigation().SetCanFloat(true);
        GetNavigation().SetRequiredPathLength(48.0f);
        SetCanPickUpLoot(true);

        // MC makeBrain → registerBrainGoals (schedule + first activity).
        m_brain = std::make_unique<Brain>();
        VillagerAi::InitBrain(*this, *m_brain);
        m_wasBaby = IsBaby();
        UpdateActivityFromSchedule();
    }

    Villager::~Villager() = default;

    PoiManager* Villager::Poi() const {
        return m_level ? m_level->GetPoiManager() : nullptr;
    }

    // ── VillagerData ─────────────────────────────────────────────────────

    void Villager::SetVillagerData(const VillagerData& data) {
        if (m_data.profession != data.profession) {
            m_offers.reset();
            BumpOffersRevision();
        }
        if (m_data != data) BumpOffersRevision();
        m_data = data;
    }

    void Villager::FinalizeVillagerType() {
        if (m_dataFinalized) return;
        VillagerType type = VillagerType::Plains;
        if (m_level) {
            if (const IBlockAccess* blocks = m_level->Blocks()) {
                const glm::ivec3 p = BlockPosition();
                type = VillagerTypeByBiome(BiomeRegistry::Get(blocks->GetBiome(p.x, p.y, p.z)).name);
            }
        }
        SetVillagerData(m_data.WithType(type));
        SetVillagerDataFinalized(true);
    }

    std::string Villager::GetMerchantTitle() const {
        // MC getTypeName → the profession's name ("Farmer"; "Villager" for
        // the unemployed).
        return VillagerProfessionDisplayName(m_data.profession);
    }

    void Villager::SetVariantByte(uint8_t v) {
        UnpackVillagerVariant(v, m_data);
    }

    uint8_t Villager::GetAnimStateByte() const {
        const bool unhappy = (m_level && m_level->IsClientSide()) ? m_clientUnhappy
                                                                  : m_unhappyCounter > 0;
        return static_cast<uint8_t>((static_cast<uint8_t>(m_data.profession) & 0x0F) |
                                    (unhappy ? 0x10 : 0x00));
    }

    void Villager::SetAnimStateByte(uint8_t v) {
        const int p = v & 0x0F;
        m_data.profession = p < kVillagerProfessionCount ? static_cast<VillagerProfession>(p)
                                                         : VillagerProfession::None;
        m_clientUnhappy = (v & 0x10) != 0;
        // The client's head-shake clock (Villager.tick counts it down on both
        // sides; the synched bit restarts it).
        m_unhappyCounter = m_clientUnhappy ? std::max(m_unhappyCounter, 1) : 0;
    }

    // ── Dimensions (MC SLEEPING_DIMENSIONS, BABY_DIMENSIONS) ─────────────

    float Villager::BaseBbWidth() const {
        if (GetPose() == Pose::Sleeping) return 0.2f;
        return AbstractVillager::BaseBbWidth();
    }
    float Villager::BaseBbHeight() const {
        if (GetPose() == Pose::Sleeping) return 0.2f;
        return AbstractVillager::BaseBbHeight();
    }
    float Villager::BaseEyeHeight() const {
        if (GetPose() == Pose::Sleeping) return 0.2f;
        return AbstractVillager::BaseEyeHeight();
    }

    // ── Brain ────────────────────────────────────────────────────────────

    void Villager::RefreshBrain() {
        m_brainRefreshPending = false;
        if (!m_level) return;
        auto fresh = std::make_unique<Brain>();
        VillagerAi::InitBrain(*this, *fresh);
        if (m_brain) {
            // MC refreshBrain: oldBrain.stopAll, then the new brain unpacks
            // the old one's SERIALISABLE memories — sensed and transient ones
            // (walk targets, nearby entities) start over.
            m_brain->StopAll(*m_level, *this);
            for (MemoryModule m : kSerializableMemories) {
                const MemoryValue* v = m_brain->GetMemory(m);
                if (!v || !fresh->IsRegistered(m)) continue;
                fresh->SetMemoryWithExpiry(m, *v, m_brain->GetTimeUntilExpiry(m));
            }
        }
        m_brain = std::move(fresh);
        m_lastScheduleUpdate = -9999;
        UpdateActivityFromSchedule();
    }

    void Villager::UpdateActivityFromSchedule() {
        // MC Brain.updateActivityFromSchedule.
        if (!m_level || !m_brain) return;
        const int64_t gameTime = m_level->GetGameTime();
        if (gameTime - m_lastScheduleUpdate <= 20) return;
        m_lastScheduleUpdate = gameTime;
        const Activity scheduled = VillagerAi::ScheduledActivity(IsBaby(), m_level->GetDayTime());
        if (!m_brain->IsActive(scheduled)) m_brain->SetActiveActivityIfPossible(scheduled);
    }

    void Villager::UpdateBrainActivity() {
        // Runs right after the brain tick, OUTSIDE it: a behaviour that asked
        // for a refresh (a new profession, a lost job) must not have its own
        // brain destroyed under it, so the rebuild lands here.
        if (m_level && !m_level->IsClientSide() && IsBaby() != m_wasBaby) {
            // MC AgeableMob.ageBoundaryReached → refreshBrain.
            m_wasBaby = IsBaby();
            m_brainRefreshPending = true;
        }
        if (m_brainRefreshPending) RefreshBrain();
    }

    void Villager::CustomServerAiStep() {
        // MC Villager.customServerAiStep, after getBrain().tick (the base ran
        // it). The TRADE reputation event for the last trade, and the happy
        // particles.
        if (m_lastTradedPlayerId >= 0) {
            if (LivingEntity* p = ResolvePlayerById(m_level, m_lastTradedPlayerId)) {
                OnReputationEventFrom(ReputationEventType::Trade, *p);
                if (m_level) m_level->BroadcastEntityEvent(*this, 14);
            }
            m_lastTradedPlayerId = -1;
        }
        // (The raid splash — entity event 42 — waits on raids.)
        if ((!m_offers || m_offers->empty()) && IsTrading()) StopTrading();
        if (!m_poiTicketsRestored) RestorePoiTickets();
        AbstractVillager::CustomServerAiStep();
    }

    void Villager::RestorePoiTickets() {
        PoiManager* poi = Poi();
        Brain* brain = GetBrain();
        if (!poi || !brain) return;
        bool pending = false;
        const auto restore = [&](MemoryModule memory) {
            const auto pos = brain->GetBlockPos(memory);
            if (!pos) return;
            if (!poi->IsChunkScanned(pos->x >> 4, pos->z >> 4)) { pending = true; return; }
            const auto type = poi->GetType(*pos);
            if (!type) return;   // gone — ValidateNearbyPoi will drop the memory
            bool fits = false;
            switch (memory) {
                case MemoryModule::Home:             fits = *type == PoiType::Home; break;
                case MemoryModule::JobSite:          fits = ProfessionHoldsJobSite(m_data.profession, *type); break;
                case MemoryModule::PotentialJobSite: fits = IsAcquirableJobSite(*type); break;
                case MemoryModule::MeetingPoint:     fits = *type == PoiType::Meeting; break;
                default: break;
            }
            if (fits) poi->Restore(*pos, *type, GetUuid());
        };
        restore(MemoryModule::Home);
        restore(MemoryModule::JobSite);
        restore(MemoryModule::PotentialJobSite);
        restore(MemoryModule::MeetingPoint);
        m_poiTicketsRestored = !pending;
    }

    // ── Spawning ─────────────────────────────────────────────────────────

    std::shared_ptr<SpawnGroupData>
    Villager::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC Villager.finalizeSpawn: a bred villager starts unemployed, then
        // finalizeVillagerType; AbstractVillager's no-baby group data needs
        // no port (the engine's AgeableMob rolls no babies at spawn).
        if (reason == SpawnReason::Breeding) {
            SetVillagerData(m_data.WithProfession(VillagerProfession::None));
        }
        FinalizeVillagerType();
        return AbstractVillager::FinalizeSpawn(reason, std::move(groupData));
    }

    std::unique_ptr<Villager> Villager::MakeBreedOffspring(const Villager& partner) {
        // MC getBreedOffspring.
        if (!m_level) return nullptr;
        const double biomeRoll = m_level->Random().NextDouble();
        VillagerType type;
        if (biomeRoll < 0.5) {
            type = VillagerType::Plains;
            if (const IBlockAccess* blocks = m_level->Blocks()) {
                const glm::ivec3 p = BlockPosition();
                type = VillagerTypeByBiome(BiomeRegistry::Get(blocks->GetBiome(p.x, p.y, p.z)).name);
            }
        } else if (biomeRoll < 0.75) {
            type = m_data.type;
        } else {
            type = partner.m_data.type;
        }
        auto baby = std::make_unique<Villager>(m_level);
        baby->SetVillagerData(baby->m_data.WithType(type).WithProfession(VillagerProfession::None));
        baby->SetVillagerDataFinalized(true);
        return baby;
    }

    // ── Interaction ──────────────────────────────────────────────────────

    void Villager::SetUnhappy() {
        // MC setUnhappy: the 40-tick head shake, and the "no" (server).
        SetUnhappyCounter(40);
        if (m_level && !m_level->IsClientSide()) MakeSound("entity.villager.no");
    }

    UseResult Villager::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC Villager.mobInteract (the spawn-egg branch ran before this — the
        // server's SpawnOffspringFromSpawnEgg).
        if (held.itemId != Items::VillagerSpawnEgg && IsAlive() && !IsTrading() && !IsSleeping()) {
            if (IsBaby()) {
                SetUnhappy();
                return UseResult::Success;
            }
            if (m_level && !m_level->IsClientSide()) {
                const bool noOffers = GetOffers().empty();
                // (Stats.TALKED_TO_VILLAGER — no statistics.)
                if (noOffers) {
                    SetUnhappy();
                    return UseResult::Consume;
                }
                StartTrading(player);
            }
            return UseResult::Success;
        }
        return AbstractVillager::MobInteract(player, held);
    }

    void Villager::StartTrading(LivingEntity& player) {
        // MC startTrading: prices for THIS player, the partner, the screen.
        UpdateSpecialPrices(player);
        SetTradingPlayer(&player);
        if (m_level) m_level->OpenMerchantMenu(player, *this);
    }

    void Villager::ResetSpecialPrices() {
        if (!m_level || m_level->IsClientSide()) return;
        for (MerchantOffer& o : GetOffers()) o.ResetSpecialPriceDiff();
    }

    void Villager::UpdateSpecialPrices(LivingEntity& player) {
        // MC updateSpecialPrices: reputation × priceMultiplier off each price,
        // and Hero of the Village's 0.3 + 0.0625 × amplifier of the base cost.
        ResetSpecialPrices();
        const int reputation = GetPlayerReputation(player);
        const MobEffectInstance* hero = player.GetEffect(MobEffectId::HeroOfTheVillage);
        const double heroModifier = hero ? static_cast<double>(0.3f + 0.0625f * static_cast<float>(hero->amplifier))
                                         : 0.0;
        bool changed = false;
        for (MerchantOffer& offer : GetOffers()) {
            if (reputation != 0) {
                offer.AddToSpecialPriceDiff(-static_cast<int>(std::floor(
                    static_cast<float>(reputation) * offer.GetPriceMultiplier())));
                changed = true;
            }
            if (heroModifier > 0.0) {
                const int costReduction = static_cast<int>(std::floor(
                    heroModifier * static_cast<double>(offer.GetBaseCostA().count)));
                offer.AddToSpecialPriceDiff(-std::max(costReduction, 1));
                changed = true;
            }
        }
        // MC re-runs the open menu's updateSellItem; here the session sees
        // the revision move and does it (and resends the prices).
        if (changed) BumpOffersRevision();
    }

    // ── Restocking ───────────────────────────────────────────────────────

    void Villager::Restock() {
        // MC restock: demand moves, every use is refunded, the partner is
        // told, and the clock / count advance.
        UpdateDemand();
        for (MerchantOffer& o : GetOffers()) o.ResetUses();
        BumpOffersRevision();   // resendOffersToTradingPlayer
        if (m_level) m_lastRestockGameTime = m_level->GetGameTime();
        ++m_numberOfRestocksToday;
    }

    bool Villager::NeedsToRestock() const {
        if (!m_offers) return false;
        for (const MerchantOffer& o : *m_offers) if (o.NeedsRestock()) return true;
        return false;
    }

    bool Villager::AllowedToRestock() const {
        // MC: the first restock of a day any time, the second only 2400
        // ticks after the first.
        const int64_t gameTime = m_level ? m_level->GetGameTime() : 0;
        return m_numberOfRestocksToday == 0 ||
               (m_numberOfRestocksToday < 2 && gameTime > m_lastRestockGameTime + 2400);
    }

    bool Villager::ShouldRestock() {
        // MC shouldRestock: a new "day" begins half a day after the last
        // restock, or when the overworld day counter (the day timeline's
        // period count) moved on since the last check.
        if (!m_level) return false;
        const int64_t halfDayPassedTime = m_lastRestockGameTime + 12000;
        const int64_t gameTime = m_level->GetGameTime();
        bool isNewDay = gameTime > halfDayPassedTime;
        const int64_t dayTime = m_level->GetDayTime();
        const int64_t currentDay = dayTime >= 0 ? dayTime / 24000 : -((-dayTime + 23999) / 24000);
        isNewDay |= m_lastRestockCheckDay > 0 && currentDay > m_lastRestockCheckDay;
        m_lastRestockCheckDay = currentDay;
        if (isNewDay) {
            m_lastRestockGameTime = gameTime;
            ResetNumberOfRestocks();
        }
        return AllowedToRestock() && NeedsToRestock();
    }

    void Villager::CatchUpDemand() {
        // MC catchUpDemand: the restocks a day skipped still move demand.
        const int missedUpdates = 2 - m_numberOfRestocksToday;
        if (missedUpdates > 0) {
            for (MerchantOffer& o : GetOffers()) o.ResetUses();
        }
        for (int i = 0; i < missedUpdates; ++i) UpdateDemand();
        BumpOffersRevision();
    }

    void Villager::UpdateDemand() {
        for (MerchantOffer& o : GetOffers()) o.UpdateDemand();
    }

    void Villager::ResetNumberOfRestocks() {
        CatchUpDemand();
        m_numberOfRestocksToday = 0;
    }

    // ── Trading XP / level ───────────────────────────────────────────────

    bool Villager::ShouldIncreaseLevel() const {
        const int level = m_data.level;
        return VillagerData::CanLevelUp(level) && m_villagerXp >= VillagerData::GetMaxXpPerLevel(level);
    }

    void Villager::IncreaseMerchantCareer() {
        SetVillagerData(m_data.WithLevel(m_data.level + 1));
        UpdateTrades();
    }

    void Villager::RewardTradeXp(const MerchantOffer& offer) {
        // MC rewardTradeXp: 3..6 XP to the player, the offer's XP to the
        // villager, a level-up (with 10 s of Regeneration and 5 more XP)
        // when it crosses the next threshold.
        if (!m_level) return;
        int popXp = 3 + m_level->Random().NextInt(4);
        m_villagerXp += offer.GetXp();
        m_lastTradedPlayerId = m_tradingPlayerId;
        if (ShouldIncreaseLevel()) {
            if (!m_level->IsClientSide()) {
                IncreaseMerchantCareer();
                AddEffect(MobEffectInstance(MobEffectId::Regeneration, 200));
            }
            popXp += 5;
        }
        BumpOffersRevision();   // the XP bar moved
        if (offer.ShouldRewardExp() && !m_level->IsClientSide()) {
            m_level->AwardExperience(position + glm::dvec3(0.0, 0.5, 0.0), popXp, m_tradingPlayerId);
        }
    }

    void Villager::UpdateTrades() {
        // MC updateTrades: this level's trade set, added to the list.
        const std::string key = ProfessionTradeSetKey(m_data.profession, m_data.level);
        if (key.empty() || !m_offers) return;
        AddOffersFromTradeSet(key, *m_offers, m_data.type);
        if (LivingEntity* player = GetTradingPlayer()) UpdateSpecialPrices(*player);
        BumpOffersRevision();   // resendOffersToTradingPlayer
    }

    bool Villager::CanRerollTrades() const {
        // MC ResetProfession: canBeFired && getVillagerXp() == 0 &&
        // level <= 1.
        const bool canBeFired = m_data.profession != VillagerProfession::None &&
                                m_data.profession != VillagerProfession::Nitwit;
        return canBeFired && m_villagerXp == 0 && m_data.level <= 1;
    }

    bool Villager::RerollTrades() {
        if (!m_level || m_level->IsClientSide() || !CanRerollTrades()) return false;
        // The job-site round trip, collapsed: ResetProfession's
        // setVillagerData(NONE) drops the offers (offers = null), taking the
        // job site back restores the profession, and the next getOffers()
        // builds a new MerchantOffers and runs updateTrades for level 1. The
        // trading player's special prices are applied inside UpdateTrades,
        // and the revision bump resends the list to the open screen.
        m_offers.emplace();
        UpdateTrades();
        BumpOffersRevision();
        return true;
    }

    // ── Reputation & gossip ──────────────────────────────────────────────

    int Villager::GetPlayerReputation(const LivingEntity& player) const {
        return m_gossips.GetReputation(player.GetUuid());
    }

    void Villager::OnReputationEventFrom(ReputationEventType type, const Entity& source) {
        // MC Villager.onReputationEventFrom.
        const Uuid& id = source.GetUuid();
        switch (type) {
            case ReputationEventType::ZombieVillagerCured:
                m_gossips.Add(id, GossipType::MajorPositive, 20);
                m_gossips.Add(id, GossipType::MinorPositive, 25);
                break;
            case ReputationEventType::Trade:
                m_gossips.Add(id, GossipType::Trading, 2);
                break;
            case ReputationEventType::VillagerHurt:
                m_gossips.Add(id, GossipType::MinorNegative, 25);
                break;
            case ReputationEventType::VillagerKilled:
                m_gossips.Add(id, GossipType::MajorNegative, 25);
                break;
            case ReputationEventType::GolemKilled:
                break;
        }
        // The trading partner's prices move with its reputation.
        LivingEntity* partner = GetTradingPlayer();
        if (partner && partner->GetUuid() == id) UpdateSpecialPrices(*partner);
    }

    void Villager::Gossip(Villager& target, int64_t timestamp) {
        // MC gossip: both sides off their 1200-tick cooldown.
        const auto ready = [timestamp](int64_t last) {
            return timestamp < last || timestamp >= last + kGossipCooldown;
        };
        if (!ready(m_lastGossipTime) || !ready(target.m_lastGossipTime) || !m_level) return;
        const int newGossip = m_gossips.TransferFrom(target.m_gossips, m_level->Random(), kMaxGossipTopics);
        m_lastGossipTime = timestamp;
        target.m_lastGossipTime = timestamp;
        SpawnGolemIfNeeded(timestamp, kGolemAgreeVillagers);
        if (newGossip > 0) {
            if (LivingEntity* partner = GetTradingPlayer()) UpdateSpecialPrices(*partner);
        }
    }

    void Villager::MaybeDecayGossip() {
        // MC maybeDecayGossip — once per 24000 ticks of game time.
        if (!m_level) return;
        const int64_t now = m_level->GetGameTime();
        if (m_lastGossipDecayTime == 0) {
            m_lastGossipDecayTime = now;
        } else if (now >= m_lastGossipDecayTime + kGossipDecayInterval) {
            m_gossips.Decay();
            m_lastGossipDecayTime = now;
        }
    }

    bool Villager::GolemSpawnConditionsMet(int64_t gameTime) const {
        // MC: slept within the last day.
        const Brain* brain = GetBrain();
        const auto slept = brain ? brain->GetLong(MemoryModule::LastSlept) : std::nullopt;
        return slept && gameTime - *slept < kTimeSinceSleepingForGolemSpawning;
    }

    bool Villager::WantsToSpawnGolem(int64_t timestamp) const {
        (void)timestamp;
        if (!m_level || !GolemSpawnConditionsMet(m_level->GetGameTime())) return false;
        const Brain* brain = GetBrain();
        return brain && !brain->HasMemoryValue(MemoryModule::GolemDetectedRecently);
    }

    namespace {
        // MC SpawnUtil.Strategy.LEGACY_IRON_GOLEM.
        bool LegacyIronGolemCanSpawnOn(BlockState state, BlockState above) {
            const BlockID id = state.Block();
            switch (id) {
                case BlockID::Cobweb: case BlockID::Cactus: case BlockID::GlassPane:
                case BlockID::Conduit: case BlockID::Ice: case BlockID::Tnt:
                case BlockID::Glowstone: case BlockID::Beacon: case BlockID::SeaLantern:
                case BlockID::FrostedIce: case BlockID::TintedGlass: case BlockID::Glass:
                    return false;
                default: break;
            }
            const std::string& slug = BlockRegistry::Get(id).registrySlug;
            const auto endsWith = [&](const char* suffix) {
                const size_t n = std::char_traits<char>::length(suffix);
                return slug.size() >= n && slug.compare(slug.size() - n, n, suffix) == 0;
            };
            // StainedGlassPaneBlock, StainedGlassBlock, LeavesBlock.
            if (endsWith("_stained_glass_pane") || endsWith("_stained_glass") || endsWith("_leaves")) {
                return false;
            }
            const BlockID aboveId = above.Block();
            const bool aboveOk = aboveId == BlockID::Air || aboveId == BlockID::Water ||
                                 aboveId == BlockID::Lava;
            // BlockState.isSolid ≈ blocks motion (the engine's #blocks_motion).
            return aboveOk && (BlockBlocksMotion(id) || id == BlockID::PowderSnow);
        }
    }

    void Villager::SpawnGolemIfNeeded(int64_t timestamp, int villagersNeededToAgree) {
        // MC spawnGolemIfNeeded.
        if (!m_level || m_level->IsClientSide() || !WantsToSpawnGolem(timestamp)) return;
        AABB box = GetAABB();
        box.min -= glm::vec3(10.0f);
        box.max += glm::vec3(10.0f);
        std::vector<Entity*> found;
        m_level->GetEntitiesInBox(box, nullptr, found);
        std::vector<Villager*> nearby;
        int wanting = 0;
        for (Entity* e : found) {
            if (e->GetType() != EntityTypeId::Villager) continue;
            auto* v = static_cast<Villager*>(e);
            nearby.push_back(v);
            if (wanting < 5 && v->WantsToSpawnGolem(timestamp)) ++wanting;
        }
        if (wanting < villagersNeededToAgree) return;

        // MC SpawnUtil.trySpawnMob(IRON_GOLEM, MOB_SUMMONED, pos, 10, 8, 6,
        // LEGACY_IRON_GOLEM, checkCollisions=false).
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return;
        JavaRandom& random = m_level->Random();
        const glm::ivec3 start = BlockPosition();
        bool spawned = false;
        for (int attempt = 0; attempt < 10 && !spawned; ++attempt) {
            const int dx = random.NextInt(17) - 8;   // Mth.randomBetweenInclusive(-8, 8)
            const int dz = random.NextInt(17) - 8;
            glm::ivec3 search(start.x + dx, start.y + 6, start.z + dz);
            BlockState aboveState = blocks->GetBlockState(search.x, search.y, search.z);
            bool found = false;
            for (int y = 6; y >= -6; --y) {
                search.y -= 1;
                const BlockState current = blocks->GetBlockState(search.x, search.y, search.z);
                if (LegacyIronGolemCanSpawnOn(current, aboveState)) {
                    search.y += 1;
                    found = true;
                    break;
                }
                aboveState = current;
            }
            if (!found) continue;
            auto golem = std::make_unique<IronGolem>(m_level);
            golem->position = glm::dvec3(search.x + 0.5, search.y, search.z + 0.5);
            golem->oldPosition = golem->position;
            golem->yRot = Mth::WrapDegrees(random.NextFloat() * 360.0f);
            golem->SetYHeadRot(golem->yRot);
            golem->yBodyRot = golem->yRot;
            golem->FinalizeSpawn(SpawnReason::MobSummoned, nullptr);
            if (!golem->CheckSpawnRules(*m_level, SpawnReason::MobSummoned) ||
                !golem->CheckSpawnObstruction(*m_level)) {
                continue;
            }
            golem->PlayAmbientSound();
            m_level->AddFreshEntity(std::move(golem));
            spawned = true;
        }
        if (!spawned) return;
        // GolemSensor.golemDetected for every villager in the box.
        for (Villager* v : nearby) {
            if (Brain* b = v->GetBrain()) b->SetMemoryWithExpiry(MemoryModule::GolemDetectedRecently, true, 599);
        }
    }

    void Villager::TellWitnessesThatIWasMurdered(Entity& murderer) {
        // MC: every ReputationEventHandler (villager) among the visible
        // living entities hears VILLAGER_KILLED.
        Brain* brain = GetBrain();
        const NearestVisibleLivingEntities* witnesses =
            brain ? brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities) : nullptr;
        if (!witnesses) return;
        for (LivingEntity* e : witnesses->entities) {
            if (!e || e->GetType() != EntityTypeId::Villager || !witnesses->IsVisible(e)) continue;
            static_cast<Villager*>(e)->OnReputationEventFrom(ReputationEventType::VillagerKilled, murderer);
        }
    }

    // ── Food ─────────────────────────────────────────────────────────────

    int Villager::CountFoodPointsInInventory() const {
        int points = 0;
        for (int i = 0; i < m_inventory.GetContainerSize(); ++i) {
            const ItemStack& s = m_inventory.GetItem(i);
            points += s.count * VillagerFoodNutrition(s.itemId);
        }
        return points;
    }

    bool Villager::CanBreed() const {
        return m_foodLevel + CountFoodPointsInInventory() >= kBreedingFoodThreshold &&
               !IsSleeping() && GetAge() == 0;
    }

    bool Villager::HasFarmSeeds() const {
        for (int i = 0; i < m_inventory.GetContainerSize(); ++i) {
            const ItemStack& s = m_inventory.GetItem(i);
            if (!s.IsEmpty() && IsVillagerPlantableSeed(s.itemId)) return true;
        }
        return false;
    }

    void Villager::EatUntilFull() {
        // MC eatUntilFull: whole slots of villager food, one item at a time,
        // until the level reaches 12.
        if (m_foodLevel >= kBreedingFoodThreshold) return;
        for (int slot = 0; slot < m_inventory.GetContainerSize(); ++slot) {
            ItemStack& s = m_inventory.GetItem(slot);
            const int nutrition = VillagerFoodNutrition(s.itemId);
            if (s.IsEmpty() || nutrition <= 0) continue;
            int toRemove = 0;
            for (int count = s.count; count > 0; --count) {
                m_foodLevel += nutrition;
                ++toRemove;
                if (m_foodLevel >= kBreedingFoodThreshold) {
                    m_inventory.RemoveItem(slot, toRemove);
                    return;
                }
            }
            m_inventory.RemoveItem(slot, toRemove);
        }
    }

    void Villager::EatAndDigestFood() {
        EatUntilFull();
        m_foodLevel -= kBreedingFoodThreshold;   // digestFood(12)
    }

    bool Villager::WantsToPickUp(const ItemStack& stack) const {
        // MC wantsToPickUp: #villager_picks_up, villager food, or what the
        // profession requests — and room for it.
        return (IsVillagerPicksUp(stack.itemId) || VillagerFoodNutrition(stack.itemId) > 0 ||
                ProfessionRequestsItem(m_data.profession, stack.itemId)) &&
               CanAddToInventory(stack);
    }

    void Villager::PickUpNearbyItems() {
        // MC Mob.aiStep's pickup (canPickUpLoot, alive, mobGriefing) with
        // Villager.pickUpItem → InventoryCarrier.pickUpItem: the part that
        // fits goes into the inventory; getPickupReach is (1, 0, 1).
        if (!m_level || m_level->IsClientSide() || !CanPickUpLoot() || !IsAlive() ||
            !m_level->MobGriefing()) {
            return;
        }
        AABBd box = GetAABBd();
        box.min -= glm::dvec3(1.0, 0.0, 1.0);
        box.max += glm::dvec3(1.0, 0.0, 1.0);
        std::vector<EntityLevel::NearbyItemEntity> items;
        m_level->GetItemEntitiesInBox(box, items);
        for (const auto& item : items) {
            if (!item.canPickUp) continue;
            const ItemStack* stack = m_level->GetItemEntityStack(item.id);
            if (!stack || stack->IsEmpty() || !WantsToPickUp(*stack)) continue;
            const ItemStack wanted = *stack;
            const ItemStack remainder = AddToInventory(wanted);
            const int taken = wanted.count - remainder.count;
            if (taken > 0) m_level->TakeFromItemEntity(item.id, taken);
        }
    }

    // ── Sleeping ─────────────────────────────────────────────────────────

    bool Villager::IsSleeping() const {
        if (m_sleepingPos) return true;
        // The client has no sleeping position — the synched pose says it.
        return m_level && m_level->IsClientSide() && GetPose() == Pose::Sleeping;
    }

    bool Villager::StartSleeping(const glm::ivec3& bedPos) {
        // MC LivingEntity.startSleeping.
        if (!m_level) return false;
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return false;
        const BlockState state = blocks->GetBlockState(bedPos.x, bedPos.y, bedPos.z);
        if (!IsBedBlock(state.Block())) return false;
        const double sleepHeight = BedSleepHeight(state.Block());
        StopRiding();
        // setPosToBed: + sleepHeight + 0.125, centred.
        position = glm::dvec3(bedPos.x + 0.5, bedPos.y + sleepHeight + 0.125, bedPos.z + 0.5);
        m_level->SetBlockState(bedPos, BedWithOccupied(state, true));
        SetPose(Pose::Sleeping);
        m_sleepingPos = bedPos;
        velocity = glm::dvec3(0.0);
        needsSync = true;
        return true;
    }

    void Villager::StopSleeping() {
        // MC LivingEntity.stopSleeping + Villager.stopSleeping (LAST_WOKEN).
        if (m_sleepingPos && m_level) {
            const glm::ivec3 bedPos = *m_sleepingPos;
            if (const IBlockAccess* blocks = m_level->Blocks()) {
                const BlockState state = blocks->GetBlockState(bedPos.x, bedPos.y, bedPos.z);
                if (IsBedBlock(state.Block())) {
                    const Direction facing = BedFacing(state);
                    m_level->SetBlockState(bedPos, BedWithOccupied(state, false));
                    const glm::dvec3 standUp =
                        FindBedStandUpPosition(*blocks, bedPos, facing, yRot)
                            .value_or(glm::dvec3(bedPos.x + 0.5, bedPos.y + 1 + 0.1, bedPos.z + 0.5));
                    const glm::dvec3 bottomCentre(bedPos.x + 0.5, bedPos.y, bedPos.z + 0.5);
                    glm::dvec3 look = bottomCentre - standUp;
                    const double len = glm::length(look);
                    if (len > 1.0e-7) look /= len;
                    const float yaw = Mth::WrapDegrees(static_cast<float>(
                        std::atan2(look.z, look.x) * 57.2957763671875 - 90.0));
                    position = standUp;
                    yRot = yaw;
                    xRot = 0.0f;
                }
            }
        }
        SetPose(Pose::Standing);
        m_sleepingPos.reset();
        needsSync = true;
        if (m_level && !m_level->IsClientSide()) {
            if (Brain* brain = GetBrain()) brain->SetMemory(MemoryModule::LastWoken, m_level->GetGameTime());
        }
    }

    // ── POI claims ───────────────────────────────────────────────────────

    void Villager::ReleasePoi(MemoryModule memory) {
        // MC releasePoi: only when the POI is still of the kind the memory
        // expects (POI_MEMORIES' predicates).
        if (!m_level || m_level->IsClientSide()) return;
        Brain* brain = GetBrain();
        PoiManager* poi = Poi();
        if (!brain || !poi) return;
        const auto pos = brain->GetBlockPos(memory);
        if (!pos) return;
        const auto type = poi->GetType(*pos);
        if (!type) return;
        bool matches = false;
        switch (memory) {
            case MemoryModule::Home:             matches = *type == PoiType::Home; break;
            case MemoryModule::JobSite:          matches = ProfessionHoldsJobSite(m_data.profession, *type); break;
            case MemoryModule::PotentialJobSite: matches = IsAcquirableJobSite(*type); break;
            case MemoryModule::MeetingPoint:     matches = *type == PoiType::Meeting; break;
            default: break;
        }
        if (matches) poi->Release(*pos, GetUuid());
    }

    void Villager::ReleaseAllPois() {
        ReleasePoi(MemoryModule::Home);
        ReleasePoi(MemoryModule::JobSite);
        ReleasePoi(MemoryModule::PotentialJobSite);
        ReleasePoi(MemoryModule::MeetingPoint);
    }

    void Villager::PlayWorkSound() {
        if (const char* sound = ProfessionWorkSound(m_data.profession)) MakeSound(sound);
    }

    // ── Life ─────────────────────────────────────────────────────────────

    bool Villager::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        const bool hurt = AbstractVillager::Hurt(source, amount, attacker);
        // MC Villager.setLastHurtByMob: VILLAGER_HURT gossip against the
        // attacker, and the angry particles when a player did it.
        if (hurt && attacker && m_level && !m_level->IsClientSide() && attacker->AsLiving()) {
            OnReputationEventFrom(ReputationEventType::VillagerHurt, *attacker);
            if (IsAlive() && attacker->IsPlayer()) m_level->BroadcastEntityEvent(*this, 13);
        }
        return hurt;
    }

    void Villager::Die(MobDamageSource source, Entity* attacker) {
        // MC Villager.die: the witnesses hear it, the claims go back, then
        // AbstractVillager.die (which stops trading).
        if (m_level && !m_level->IsClientSide()) {
            Log::Info("Villager %d (%s) died", GetId(), std::string(VillagerProfessionId(m_data.profession)).c_str());
            if (attacker) TellWitnessesThatIWasMurdered(*attacker);
            ReleaseAllPois();
        }
        AbstractVillager::Die(source, attacker);
    }

    void Villager::Tick() {
        // MC LivingEntity.tick's bed check (server): a sleeper whose bed is
        // gone gets up.
        if (m_level && !m_level->IsClientSide() && m_sleepingPos) {
            const IBlockAccess* blocks = m_level->Blocks();
            const BlockState bed = blocks ? blocks->GetBlockState(m_sleepingPos->x, m_sleepingPos->y,
                                                                  m_sleepingPos->z)
                                          : BlockState{};
            if (!IsBedBlock(bed.Block())) StopSleeping();
        }
        AbstractVillager::Tick();
        // MC Villager.tick.
        if (m_unhappyCounter > 0) {
            --m_unhappyCounter;
            if (m_level && m_level->IsClientSide() && m_unhappyCounter == 0 && m_clientUnhappy) {
                m_unhappyCounter = 1;   // held until the server clears the bit
            }
        }
        if (m_level && !m_level->IsClientSide()) MaybeDecayGossip();
    }

    void Villager::AiStep() {
        AbstractVillager::AiStep();
        PickUpNearbyItems();
    }

    const char* Villager::GetAmbientSound() const {
        // MC getAmbientSound: silent asleep, "trade" while trading.
        if (IsSleeping()) return "";
        return IsTrading() ? "entity.villager.trade" : "entity.villager.ambient";
    }

    void Villager::HandleEntityEvent(uint8_t id) {
        // MC Villager.handleEntityEvent.
        switch (id) {
            case 12: AddParticlesAroundSelf(static_cast<int>(ParticleKind::Heart)); return;
            case 13: AddParticlesAroundSelf(static_cast<int>(ParticleKind::AngryVillager)); return;
            case 14: AddParticlesAroundSelf(static_cast<int>(ParticleKind::HappyVillager)); return;
            case 42: return;   // SPLASH (raid) — no raids, no splash particle
            default: break;
        }
        AbstractVillager::HandleEntityEvent(id);
    }

} // namespace Game
