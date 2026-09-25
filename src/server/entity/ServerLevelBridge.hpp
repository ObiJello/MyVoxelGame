// File: src/server/entity/ServerLevelBridge.hpp
//
// The server's implementation of Game::EntityLevel, plus the player adapter.
//
// ── Why players need an adapter ────────────────────────────────────────────
//
// In MC, Player extends LivingEntity, so a mob can target a player, path to
// one, and damage one through exactly the same interface it uses for any other
// entity. Here, Server::ServerPlayer is a pre-existing class with its own
// position, health and packet-driven update path, and it does not derive from
// Game::LivingEntity.
//
// Rather than refactor ServerPlayer (a large, invasive change to code the whole
// server depends on), each session gets a PlayerEntityView: a real
// Game::LivingEntity whose position and health are MIRRORED from its
// ServerPlayer at the top of every tick, and whose Hurt() forwards back into
// ServerPlayer::damage. Mobs therefore see a genuine LivingEntity and every
// goal works unmodified.
//
// The mirroring direction matters and is one-way per field:
//   ServerPlayer -> view : position, rotation, health, alive, game mode
//   view -> ServerPlayer : damage only, via Hurt()
// Anything that writes position on the view would be silently discarded next
// tick, so nothing does.
#pragma once

#include <chrono>

#include "common/core/Features.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Item.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#if ENABLE_IMMERSIVE_PORTALS
#include "server/portal/MobPortalCollision.hpp"
#endif

#include <array>
#include <memory>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <vector>

namespace Game { class World; }

namespace Server {

    class ServerPlayer;
    class PlayerSessionManager;
    class MobManager;
    class ItemEntityManager;
    class ExperienceOrbManager;
    class IntegratedServer;

    // A mob-facing view of one player. See the header note.
    class PlayerEntityView : public Game::LivingEntity {
    public:
        PlayerEntityView(Game::EntityLevel* level, ServerPlayer* player, int32_t entityId);

        bool IsPlayer() const override { return true; }
        bool IsCreative() const override;
        bool IsSpectator() const override;
        bool IsAbilityFlying() const override;

        // A player's current push is applied by the CLIENT's own physics
        // (MC LocalPlayer runs updateFluidInteraction itself); pushing the
        // view too would queue a velocity packet every tick in a river and
        // fight the client's copy. The view still tracks the fluid state
        // (RefreshFluidState in TickCombatState) for the mobs that ask.
        bool IsPushedByFluid() const override { return false; }

        // MC LivingEntity.isAttackable — creative and spectator players are not
        // valid targets, which is what makes a creative player invisible to
        // hostile mobs.
        bool IsAttackable() const override { return !IsCreative() && !IsSpectator(); }

        // The base box; Entity::scale (set from the player's size when the
        // view syncs) sits on top of it. A morphed player (/morph) has the
        // mob's box and eye, so mobs target and reach them as that mob.
        float BaseBbWidth()   const override;
        float BaseBbHeight()  const override;
        float BaseEyeHeight() const override;

        // MC ServerPlayer.getKnownMovement: the movement the client actually
        // reported, not `velocity` (which on a view is only the knockback
        // accumulator). SyncFromPlayer keeps oldPosition exactly one tick
        // behind, so the difference IS last tick's displacement.
        glm::dvec3 GetKnownMovement() const override {
            return position - oldPosition;
        }

        // MC Player.getDimensionChangingDelay (Player.java:383) — TEN ticks,
        // against Entity's generic 300.
        //
        // This is the cooldown armed after travelling, and it is also what
        // SetAsInsidePortal re-arms while you stand in the portal you arrived
        // in. At 300 a player who came through and stepped back in found the
        // portal completely dead for fifteen seconds; vanilla's half-second
        // makes it feel immediate, which is what a creative player expects
        // when their transition delay is zero.
        int GetDimensionChangingDelay() const override {
            return Game::Portals::kPlayerPortalCooldown;
        }

        // Forwards into ServerPlayer::damage. Everything before the forward —
        // the invulnerability window, the hurt flash, knockback — still runs in
        // LivingEntity::Hurt, so a mob hitting a player behaves identically to
        // a mob hitting a mob.
        bool Hurt(Game::MobDamageSource source, float amount, Game::Entity* attacker) override;

        // A worn-out piece of the player's (HurtAndBreak through the view):
        // the break effects are the ServerPlayer's to send — the view is not
        // a tracked entity, so LivingEntity's entity event would go nowhere.
        void OnEquippedItemBroken(const Game::ItemStack& broken, Game::EquipmentSlot slot) override;

        // ── The player's equipment, for the enchantment runners ────────────
        // MC Player.getItemBySlot: the selected hotbar stack for MAINHAND,
        // the offhand and the four armour slots of the ServerPlayer's
        // inventory (live — an effect that wears a piece wears the real one).
        Game::ItemStack* EquipmentInSlot(Game::EquipmentSlot slot) override;
        bool HasEquipmentSlots() const override { return m_player != nullptr; }
        // MC LivingEntity.getWeaponItem: the main hand.
        Game::ItemStack* GetWeaponItem() override { return EquipmentInSlot(Game::EquipmentSlot::MAINHAND); }
        // The player's fire lives on its ServerPlayer (which applies the
        // BURNING_TIME scaling itself — Fire Protection).
        void IgniteForTicks(int ticks) override;

        // ── Status effects on a player ────────────────────────────────────
        //
        // The view's placeholder entity type is Zombie, so the type-tag
        // default would call a player undead — harming would heal them.
        bool IsInvertedHealAndHarm() const override { return false; }

        // MC: a player is not in #can_breathe_under_water (the placeholder
        // Zombie type would say it is — undead); LivingEntity.baseTick's
        // `isPlayer && abilities.invulnerable` exemption (creative and
        // spectator) is folded in here, which gives the same air outcome.
        bool CanBreatheUnderwater() const override { return IsCreative() || IsSpectator(); }

        // The view's own health is a mirror; REGENERATION and INSTANT_HEALTH
        // must land on the real player or the next SyncFromPlayer would
        // silently erase them.
        void Heal(float amount) override;

        // The player-only halves of HUNGER and SATURATION, forwarded into
        // ServerPlayer's FoodData (MC Player.causeFoodExhaustion /
        // FoodData.eat).
        void CauseFoodExhaustion(float amount) override;
        void EatFood(int nutrition, float saturationModifier) override;

        // MC getVisibilityPercent's three inputs for a player: sneaking
        // (isDiscrete), the four worn armour slots (getArmorCoverPercentage)
        // and a mob head on the HEAD slot (the MOB_VISIBILITY component a
        // skeleton skull / zombie head / creeper head / piglin head carries).
        bool   IsDiscrete() const override;
        float  GetArmorCoverPercentage() const override;
        double GetEquipmentVisibilityFactor(const Game::Entity* targetingEntity) const override;

        // MC Entity.isSwimming for the player: the sprint the client reports,
        // under water — the client's own swim rule (Physics: sprint with the
        // eye in water). Read by the dolphin's escort (DOLPHINS_GRACE).
        bool IsSwimming() const override;

        // The player's absorption hearts live on ServerPlayer (MC Player's
        // DATA_PLAYER_ABSORPTION_ID); ABSORPTION's onEffectStarted and the
        // hurt path reach them through these.
        float GetAbsorptionAmount() const override;
        void  SetAbsorptionAmount(float v) override;

        // MC LivingEntity.tickDeath on a player: at deathTime 20 the body is
        // removed (KILLED) — triggerOnDeathMobEffects fires WIND_CHARGED /
        // WEAVING / OOZING and empties the list. Called by TickCombatState.
        void TickPlayerDeathEffects();

        // Called once per server tick, before mobs tick.
        void SyncFromPlayer();

        // The enchantment half of MC LivingEntity.baseTick / tick for the
        // player: EnchantmentHelper.tickEffects, onChangedBlock's
        // runLocationChangedEffects on a new block position, and
        // collectEquipmentChanges — a changed slot's enchantment attribute
        // modifiers come off and the new item's go on (Efficiency, Aqua
        // Affinity, Respiration, Depth Strider, Swift Sneak, Sweeping Edge,
        // Fire and Blast Protection's attributes), its location effects stop
        // and restart. Called by TickCombatState.
        void TickEnchantments();

        // The part of MC LivingEntity.baseTick that a client-authoritative
        // player still needs: the combat timers.
        //
        // A view is never Tick()ed — running LivingEntity::Tick on it would
        // simulate movement the client owns — so nothing else counts these
        // down. Without this the FIRST hit a player takes leaves hurtTime
        // pinned at 10 (permanently red to everyone) and invulnerableTime
        // pinned at 20, which sends every later hit down the
        // "already invulnerable" branch: only strictly-larger damage lands, so
        // a player is effectively unhittable after one sword swing.
        void TickCombatState();

        // The player's arm swing, as MC's server-side Player keeps it
        // (ServerboundSwingPacket -> swing; LivingEntity.tick's
        // updateSwingTime). There is no swing packet here, so the server
        // swings the view where it learns of one: an attack, and every tick
        // of a dig (MC's client swings the whole time it mines —
        // continueAttack). TickCombatState runs the swing clock. Nothing is
        // broadcast: a view is not a tracked entity, so Swing's entity event
        // is dropped; mobs read the swing off the view (the echo mimic
        // replays it).
        void SetDigging(bool digging) { m_digging = digging; }

        // MC ServerPlayer.indicateDamage — records the direction the hit came
        // from and pushes it to this player's own client, which is the only
        // thing that can drive the camera tilt (the hurt FLASH other players
        // see rides the position broadcast instead).
        void IndicateDamage(double xd, double zd);

        ServerPlayer* GetPlayer() const { return m_player; }

        // Built for an earlier visit of the player to this level: they have
        // changed dimension since (ServerPlayer::getDimensionEpoch). A level
        // nobody stands in is not ticked, so its SyncPlayerViews never dropped
        // this view when they left; resuming it on their return would resume
        // the portal state, position and timers of that old visit instead of
        // the ones the player carried across (MC recreates the entity on
        // every dimension change — Entity.restoreFrom).
        bool IsFromEarlierVisit() const;

        // Knockback the mob system applied that the client has not been told
        // about yet. The player is client-authoritative for movement, so a push
        // has to be SENT rather than simply applied — the tick loop drains this.
        bool  ConsumePendingKnockback(glm::dvec3& out);

        // The world's difficulty, for MC's Player.hurtServer scaling pass.
        Game::Difficulty GetDifficultyOfLevel() const;

        // MC LivingEntity.knockback on a ServerPlayer: the push only matters
        // once sent (hurtMarked → a motion packet). The extra knockback of an
        // attack — a sprint hit, the Knockback enchantment, a sweep — lands
        // after Hurt has captured its own, so the view queues any push here.
        void Knockback(double power, double dx, double dz) override;

        // MC Entity.addDeltaMovement on a ServerPlayer sets hurtMarked so the
        // push reaches the client as a velocity packet; here that packet is
        // the pending-knockback drain above. Used by the wind-charge burst,
        // which pushes without dealing damage (so Hurt's capture never runs).
        void AddDeltaMovement(const glm::dvec3& v) override {
            velocity += v;
            m_pendingKnockback = velocity;
            m_hasPendingKnockback = true;
        }

    protected:
        void ActuallyHurt(Game::MobDamageSource source, float amount,
                          Game::Entity* attacker) override;

        // ── A player's effects (MC ServerPlayer's effect overrides) ────────
        // The list is the ServerPlayer's (it survives this per-level view and
        // is saved); the hooks add MC ServerPlayer's packet halves —
        // UpdateMobEffect (blend on add) / RemoveMobEffect to the player's own
        // client — and keep ServerPlayer's health / absorption inside the
        // maxima the remaining effects allow.
        std::vector<Game::MobEffectInstance>&       EffectStorage() override;
        const std::vector<Game::MobEffectInstance>& EffectStorage() const override;
        void OnEffectAdded(const Game::MobEffectInstance& effect, Game::Entity* source) override;
        void OnEffectUpdated(const Game::MobEffectInstance& effect, bool refreshAttributes,
                             Game::Entity* source) override;
        void OnEffectRemoved(const Game::MobEffectInstance& effect) override;

    private:
        ServerPlayer* m_player;
        // The player's dimension epoch when this view was built.
        uint32_t      m_dimensionEpoch = 0;
        bool          m_hasPendingKnockback = false;
        glm::dvec3    m_pendingKnockback{0.0};
        // Last value of ServerPlayer::getDamageCounter this view has reacted to
        // — see TickCombatState.
        uint32_t      m_lastSeenDamageCounter = 0;
        // The corpse's effects were already handed to triggerOnDeathMobEffects
        // (once per death).
        bool          m_deathEffectsTriggered = false;
        // Between a survival START_DESTROY and its STOP / ABORT (SetDigging).
        bool          m_digging = false;
        // TickEnchantments' state: MC lastEquipmentItems (per EquipmentSlot
        // ordinal, MAINHAND..SADDLE), lastPos and
        // activeLocationDependentEnchantments.
        std::array<Game::ItemStack, 8>       m_lastEquipment{};
        glm::ivec3                           m_lastBlockPos{0};
        bool                                 m_hasLastBlockPos = false;
        Game::ActiveLocationEnchantments     m_locationEnchantments;
    };

    // Game::EntityLevel over the server's world and session list.
    class ServerLevelBridge : public Game::EntityLevel {
    public:
        ServerLevelBridge(Game::World* world, PlayerSessionManager* sessions);

        // Out of line, and it has to be: m_pendingExplosions is a
        // vector<ExplosionParams>, and ExplosionParams is only FORWARD-declared
        // here (EntityLevel.hpp declares it; Explosion.hpp defines it). A
        // vector's destructor needs the element type complete, so an implicit
        // ~ServerLevelBridge would fail to compile in every translation unit
        // that destroys one without having included Explosion.hpp —
        // ServerLevel.cpp among them. Defining it in the .cpp, which does
        // include Explosion.hpp, keeps that heavy header out of this one.
        ~ServerLevelBridge();

        void SetMobManager(MobManager* mobs) { m_mobs = mobs; }
        // THIS level's item and orb managers. AwardExperience and
        // SpawnItemDrop used to route through IntegratedServer's Overworld-
        // pinned accessors, which is how a dragon killed in the End paid its
        // 12,000 XP into the Overworld at (0, 65, 0) and an End enderman's
        // pearl dropped a dimension away.
        void SetItemAndOrbManagers(ItemEntityManager* items,
                                   ExperienceOrbManager* orbs) {
            m_items = items;
            m_orbs  = orbs;
        }

        // ── EntityLevel ────────────────────────────────────────────────────
        const Game::IBlockAccess* Blocks() const override;
        uint64_t BlockWriteEpoch() const override;

        // See EntityLevel::TryBeginExplosion. Budget is reset once per server
        // tick by BeginExplosionBudget below.
        bool TryBeginExplosion() override;
        void BeginExplosionBudget();

        void QueueExplosion(const Game::ExplosionParams& p) override;

        // Run this tick's queued blasts: crater scans across the worker pool,
        // then the applies serially in queue order. Called from MobManager::Tick
        // AFTER the tick loop and BEFORE the sweep — the loop is what fills the
        // queue, and the sweep is what would free the sources the applies read.
        void ResolveQueuedExplosions();
        bool IsClientSide() const override { return false; }
#if ENABLE_IMMERSIVE_PORTALS
        // Cross-portal collision for this level's mobs: the gun surfaces
        // they may walk through (MobPortalCollision). Refreshed by
        // IntegratedServer::TickMobs before the mobs move.
        const Game::PortalCollisionProvider* PortalCollision() const override { return &m_portalCollision; }
        MobPortalCollision& PortalCollisionState() { return m_portalCollision; }
#endif
        int64_t GetGameTime() const override;
        int64_t GetDayTime()  const override;
        // Out of line, and reading the WORLD rather than a member of its own:
        // m_difficulty sat here defaulted to Normal with nothing ever writing
        // it, so Peaceful was unreachable and MC's difficulty scaling on damage
        // could not be reproduced.
        Game::Difficulty GetDifficulty() const override;
        Game::JavaRandom& Random() override { return m_random; }

        int  GetSkyBrightness(int x, int y, int z) const override;
        int  GetBlockBrightness(int x, int y, int z) const override;
        int  GetMaxLocalRawBrightness(int x, int y, int z) const override;
        int  GetMaxLocalRawBrightness(int x, int y, int z, int amount) const override;
        int  GetSkyDarken() const override;
        bool CanSeeSky(int x, int y, int z) const override;
        bool MonstersBurn() const override;
        bool IsDay() const override;
        float GetBiomeTemperature(int x, int y, int z) const override;

        void GetEntitiesInBox(const Game::AABB& box, const Game::Entity* except,
                              std::vector<Game::Entity*>& out) const override;
        Game::LivingEntity* GetNearestPlayer(double x, double y, double z,
                                             double maxDistance) const override;
        void GetPlayers(std::vector<Game::LivingEntity*>& out) const override;

        // ── Identity lookup for saved references (EntityRef) ────────────────
        //
        // Mobs first in THIS level, then every sibling dimension — a saved
        // reference can cross a portal, and MC resolves the same way
        // (ServerLevel.getEntityInAnyDimension). Players go through the
        // server-wide session list, not this level's players, because a pet's
        // owner may be standing in the Nether.
        Game::Entity*       ResolveEntity(const Game::Uuid& uuid) const override;
        Game::LivingEntity* ResolvePlayer(const Game::Uuid& uuid) const override;
        uint32_t GetHeldItemId(const Game::LivingEntity& player) const override;
        uint32_t GetChestItemId(const Game::LivingEntity& player) const override;
        void DisplayClientMessage(const Game::LivingEntity& player, const std::string& text,
                                  bool actionBar) const override;
        void GetItemEntitiesInBox(const Game::AABBd& box,
                                  std::vector<NearbyItemEntity>& out) const override;
        int  TakeFromItemEntity(int32_t id, int count) override;
        bool AddItemEntityDeltaMovement(int32_t id, const glm::dvec3& delta) override;
        const Game::ItemStack* GetItemEntityStack(int32_t id) const override;
        bool SetItemEntityStack(int32_t id, const Game::ItemStack& stack) override;
        void CreateFilledResult(Game::LivingEntity& player, Game::ItemStack& held,
                                const Game::ItemStack& filled) override;

        void BroadcastEntityEvent(const Game::Entity& entity, uint8_t event) override;

        // MC ServerLevel.playSeededSound (both forms): through the installed
        // ServerSoundSink, scoped to this level's dimension.
        void PlaySeededSound(const Game::SoundExcept& except, const glm::dvec3& pos,
                             std::string_view event, Game::SoundSource source,
                             float volume, float pitch, int64_t seed) override;
        void PlaySeededSoundFromEntity(const Game::SoundExcept& except, const Game::Entity& sourceEntity,
                                       std::string_view event, Game::SoundSource source,
                                       float volume, float pitch, int64_t seed) override;

        // MC ServerExplosion.hurtEntities over the two entity kinds that live
        // outside the Game::Entity hierarchy — dropped items and XP orbs.
        void ApplyExplosionToLooseEntities(const Game::ExplosionParams& params,
                                           const Game::CollisionGrid* occlusion) override;
        void ApplyExplosionsToLooseEntities(const Game::ExplosionParams* const* params,
                                            size_t count,
                                            const Game::CollisionGrid* occlusion) override;

        // MC ClientboundHurtAnimationPacket, sent to that player alone.
        void SendHurtAnimation(int32_t connectionId, float hurtDir);
        void SpawnItemDrop(const glm::dvec3& pos, uint32_t itemId, int count) override;
        void SpawnItemStackDrop(const glm::dvec3& pos, const Game::ItemStack& stack) override;
        // MC BehaviorUtils.throwItem's ItemEntity: an exact spawn point,
        // velocity and pickup delay.
        void SpawnThrownItem(const glm::dvec3& pos, const glm::dvec3& velocity,
                             const Game::ItemStack& stack, int pickupDelay) override;

        // MC ServerLevel.getPoiManager — ServerLevel owns it; set once.
        void SetPoiManager(Game::PoiManager* poi) { m_poi = poi; }
        Game::PoiManager* GetPoiManager() override { return m_poi; }
        // MC Merchant.openTradingScreen: recorded on the ServerPlayer and
        // performed by its session (PlayerSession::FlushPendingMenuOpen).
        void OpenMerchantMenu(Game::LivingEntity& player, Game::Mob& merchant) override;

        // MC ExperienceOrb.award: real orbs at `pos` through THIS level's
        // ExperienceOrbManager (split, merge, pickup delay and the Mending
        // repair on pickup are the manager's).
        void AwardExperience(const glm::dvec3& pos, int amount,
                             int32_t creditPlayerEntityId) override;

        // Block edits made by mobs (a sheep grazing). Both go through
        // Game::World so the change is broadcast and meshed like any other.
        void DestroyBlock(const glm::ivec3& pos, bool dropResources) override;
        void SetBlock(const glm::ivec3& pos, Game::BlockID block) override;
        void SetBlockState(const glm::ivec3& pos, Game::BlockState state) override;

        // The server IS the authority, so this hands back the real world. See
        // EntityLevel::MutableBlocks for why the client's bridge does not.
        //
        // Out of line: Game::World is only forward-declared here, so the
        // World* -> ILevelWrite* upcast cannot be done until the definition is
        // visible.
        Game::ILevelWrite* MutableBlocks() override;

        // MC TntBlock.wasExploded — chain detonation. See EntityLevel.
        void OnTntExploded(const glm::ivec3& pos, Game::Entity* igniter) override;

        // MC ServerLevel.explode's per-player ClientboundExplodePacket send.
        void BroadcastExplosion(const glm::dvec3& center, float radius,
                                int blockCount, bool small) override;
        // MC ServerLevel.getDragonFight — set by ServerLevel for the End,
        // null everywhere else. See common/entity/DragonFight.hpp.
        void SetDragonFight(Game::IDragonFight* fight) { m_dragonFight = fight; }
        Game::IDragonFight* DragonFight() override { return m_dragonFight; }
        // The Hush's stillness (Server::HushStillness raises it, once a tick
        // before the mobs tick). Atomic because mobs tick on the worker pool.
        void SetStilled(bool stilled) { m_stilled.store(stilled, std::memory_order_relaxed); }
        bool IsStilled() const override { return m_stilled.load(std::memory_order_relaxed); }

        Game::DimensionId Dimension() const override;
        bool MobGriefing() const override;
        bool DoMobSpawning() const override;
        bool TeleportPlayer(Game::LivingEntity& player,
                            const glm::dvec3& pos) override;
        int  GetMinY() const override;
        int  GetMaxY() const override;
        bool TntExplodes() const override;
        bool DoEntityDrops() const override;
        bool TntExplosionDropDecay() const override;
        bool BlockExplosionDropDecay() const override;
        bool MobExplosionDropDecay() const override;
        void AddFreshEntity(std::unique_ptr<Game::Entity> entity) override;

        // Container BEs for mob-facing chest work (the copper golem's
        // transport behaviour). Both read straight off the chunks.
        Game::BaseContainerBlockEntity*
        GetContainerBlockEntity(const glm::ivec3& pos) const override;
        void GetContainerBlockEntities(const glm::ivec3& center, int chunkRange,
                                       std::vector<Game::BaseContainerBlockEntity*>& out)
            const override;

        // ── Player views ───────────────────────────────────────────────────
        //
        // Rebuilt from the live session list each tick: sessions come and go,
        // and a stale view would be a dangling ServerPlayer pointer inside
        // every mob that had targeted it.
        void SyncPlayerViews();

        PlayerEntityView* GetPlayerView(uint32_t connectionId);
        const std::vector<PlayerEntityView*>& PlayerViews() const { return m_playerViewList; }

        // Entity events raised this tick, drained by IntegratedServer and sent
        // as EntityEventS2C. Buffered rather than sent inline because the
        // entity system must not depend on the network layer.
        struct PendingEvent { int32_t entityId; uint8_t event; };
        std::vector<PendingEvent>& DrainEvents() { return m_pendingEvents; }

        // Entities created during the tick (breeding, reinforcements). Drained
        // by MobManager after ticking, so the list being iterated is never
        // mutated mid-iteration.
        std::vector<std::unique_ptr<Game::Entity>>& DrainSpawned() { return m_spawned; }

    private:
        // Tell every mob, in every level, that `departing` is going away —
        // the half of dropping a player view that must precede the erase.
        void ClearReferencesToPlayerView(PlayerEntityView* departing);

        Game::World*          m_world;
        PlayerSessionManager* m_sessions;
        MobManager*           m_mobs = nullptr;
#if ENABLE_IMMERSIVE_PORTALS
        MobPortalCollision    m_portalCollision;
#endif
        ItemEntityManager*    m_items = nullptr;
        ExperienceOrbManager* m_orbs = nullptr;
        Game::IDragonFight*   m_dragonFight = nullptr;   // End only
        Game::PoiManager*     m_poi = nullptr;           // ServerLevel's
        std::atomic<bool>     m_stilled{false};          // Hush only (HushStillness)

        mutable Game::JavaRandom m_random{0};

        std::unordered_map<uint32_t, std::unique_ptr<PlayerEntityView>> m_playerViews;
        std::vector<PlayerEntityView*> m_playerViewList;

        // Explosion admission budget, reset per server tick.
        std::chrono::steady_clock::time_point m_explosionBudgetStart{};
        std::atomic<int> m_explosionsThisTick{0};

        // Guards m_pendingExplosions: primed TNT ticks in parallel now.
        std::mutex m_pendingExplosionsMutex;
        // This tick's blasts. Pushed in whatever order the workers finished;
        // sorted by source entity id before the applies run.
        std::vector<Game::ExplosionParams> m_pendingExplosions;
        // kExplosionRayCount pre-drawn jitter values per pending blast, packed
        // end to end. Reused across ticks so a detonation does not allocate.
        std::vector<float> m_explosionJitter;
        // Scan output, one crater per pending blast.
        std::vector<std::vector<glm::ivec3>> m_explosionCraters;

        // How many blasts the tick may START, derived from what the last
        // resolve actually cost. Replaces the old elapsed-time gate, which
        // cannot work once the cost is paid AFTER the gate rather than during
        // it. See TryBeginExplosion.
        int     m_blastsAllowedThisTick = 1;
        int64_t m_lastBlastCostNs = 0;
        // Per-tick (count-independent) part of the last resolve; see
        // BeginExplosionBudget.
        int64_t m_lastResolveFixedNs = 0;
        int64_t m_lastResolveTotalNs = 0;
        int64_t m_explosionFixedNs   = 0;
        int64_t m_explosionBlastNs   = 0;

        std::vector<PendingEvent> m_pendingEvents;
        std::vector<std::unique_ptr<Game::Entity>> m_spawned;
    };

} // namespace Server
