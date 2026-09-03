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

#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/core/JavaRandom.hpp"

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

        // MC LivingEntity.isAttackable — creative and spectator players are not
        // valid targets, which is what makes a creative player invisible to
        // hostile mobs.
        bool IsAttackable() const override { return !IsCreative() && !IsSpectator(); }

        // The base box; Entity::scale (set from the player's size when the
        // view syncs) sits on top of it.
        float BaseBbWidth()   const override { return 0.6f; }
        float BaseBbHeight()  const override { return 1.8f; }
        float BaseEyeHeight() const override { return 1.62f; }

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

        // ── Status effects on a player ────────────────────────────────────
        //
        // The view's placeholder entity type is Zombie, so the type-tag
        // default would call a player undead — harming would heal them.
        bool IsInvertedHealAndHarm() const override { return false; }

        // The view's own health is a mirror; REGENERATION and INSTANT_HEALTH
        // must land on the real player or the next SyncFromPlayer would
        // silently erase them.
        void Heal(float amount) override;

        // The player-only halves of HUNGER and SATURATION, forwarded into
        // ServerPlayer's FoodData (MC Player.causeFoodExhaustion /
        // FoodData.eat).
        void CauseFoodExhaustion(float amount) override;
        void EatFood(int nutrition, float saturationModifier) override;

        // Called once per server tick, before mobs tick.
        void SyncFromPlayer();

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

        // MC ServerPlayer.indicateDamage — records the direction the hit came
        // from and pushes it to this player's own client, which is the only
        // thing that can drive the camera tilt (the hurt FLASH other players
        // see rides the position broadcast instead).
        void IndicateDamage(double xd, double zd);

        ServerPlayer* GetPlayer() const { return m_player; }

        // Knockback the mob system applied that the client has not been told
        // about yet. The player is client-authoritative for movement, so a push
        // has to be SENT rather than simply applied — the tick loop drains this.
        bool  ConsumePendingKnockback(glm::dvec3& out);

        // The world's difficulty, for MC's Player.hurtServer scaling pass.
        Game::Difficulty GetDifficultyOfLevel() const;

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

    private:
        ServerPlayer* m_player;
        bool          m_hasPendingKnockback = false;
        glm::dvec3    m_pendingKnockback{0.0};
        // Last value of ServerPlayer::getDamageCounter this view has reacted to
        // — see TickCombatState.
        uint32_t      m_lastSeenDamageCounter = 0;
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
        int64_t GetGameTime() const override;
        int64_t GetDayTime()  const override;
        // Out of line, and reading the WORLD rather than a member of its own:
        // m_difficulty sat here defaulted to Normal with nothing ever writing
        // it, so Peaceful was unreachable and MC's difficulty scaling on damage
        // could not be reproduced.
        Game::Difficulty GetDifficulty() const override;
        Game::JavaRandom& Random() override { return m_random; }

        int  GetSkyBrightness(int x, int y, int z) const override;
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

        void BroadcastEntityEvent(const Game::Entity& entity, uint8_t event) override;

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

        // MC ExperienceOrb.award, minus the orb. DEVIATION (documented in
        // EntityLevel.hpp): the points go straight into a ServerPlayer's
        // PlayerExperience — the credited player if still online, else the
        // nearest player within an orb's 8-block follow range — so none of
        // MC's orb mechanics exist: no split into orb-sized values
        // (ExperienceOrb.getExperienceValue), no merge, no 2-tick pickup
        // delay, no Mending repair, no orb lingering for whoever walks by.
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
        Game::World*          m_world;
        PlayerSessionManager* m_sessions;
        MobManager*           m_mobs = nullptr;
        ItemEntityManager*    m_items = nullptr;
        ExperienceOrbManager* m_orbs = nullptr;
        Game::IDragonFight*   m_dragonFight = nullptr;   // End only

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
