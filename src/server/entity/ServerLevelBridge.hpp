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

#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/core/JavaRandom.hpp"

#include <memory>
#include <unordered_map>
#include <vector>

namespace Game { class World; }

namespace Server {

    class ServerPlayer;
    class PlayerSessionManager;
    class MobManager;
    class IntegratedServer;

    // A mob-facing view of one player. See the header note.
    class PlayerEntityView : public Game::LivingEntity {
    public:
        PlayerEntityView(Game::EntityLevel* level, ServerPlayer* player, int32_t entityId);

        bool IsPlayer() const override { return true; }
        bool IsCreative() const override;
        bool IsSpectator() const override;

        // MC LivingEntity.isAttackable — creative and spectator players are not
        // valid targets, which is what makes a creative player invisible to
        // hostile mobs.
        bool IsAttackable() const override { return !IsCreative() && !IsSpectator(); }

        float GetBbWidth()  const override { return 0.6f; }
        float GetBbHeight() const override { return 1.8f; }
        float GetEyeHeight() const override { return 1.62f; }

        // Forwards into ServerPlayer::damage. Everything before the forward —
        // the invulnerability window, the hurt flash, knockback — still runs in
        // LivingEntity::Hurt, so a mob hitting a player behaves identically to
        // a mob hitting a mob.
        bool Hurt(Game::MobDamageSource source, float amount, Game::Entity* attacker) override;

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

        void SetMobManager(MobManager* mobs) { m_mobs = mobs; }

        // ── EntityLevel ────────────────────────────────────────────────────
        const Game::IBlockAccess* Blocks() const override;
        bool IsClientSide() const override { return false; }
        int64_t GetGameTime() const override;
        int64_t GetDayTime()  const override;
        Game::Difficulty GetDifficulty() const override { return m_difficulty; }
        Game::JavaRandom& Random() override { return m_random; }

        int  GetSkyBrightness(int x, int y, int z) const override;
        int  GetMaxLocalRawBrightness(int x, int y, int z) const override;
        int  GetMaxLocalRawBrightness(int x, int y, int z, int amount) const override;
        int  GetSkyDarken() const override;
        bool CanSeeSky(int x, int y, int z) const override;
        bool MonstersBurn() const override;
        bool IsDay() const override;

        void GetEntitiesInBox(const Game::AABB& box, const Game::Entity* except,
                              std::vector<Game::Entity*>& out) const override;
        Game::LivingEntity* GetNearestPlayer(double x, double y, double z,
                                             double maxDistance) const override;
        void GetPlayers(std::vector<Game::LivingEntity*>& out) const override;
        uint32_t GetHeldItemId(const Game::LivingEntity& player) const override;

        void BroadcastEntityEvent(const Game::Entity& entity, uint8_t event) override;

        // MC ClientboundHurtAnimationPacket, sent to that player alone.
        void SendHurtAnimation(int32_t connectionId, float hurtDir);
        void SpawnItemDrop(const glm::dvec3& pos, uint32_t itemId, int count) override;

        // Block edits made by mobs (a sheep grazing). Both go through
        // Game::World so the change is broadcast and meshed like any other.
        void DestroyBlock(const glm::ivec3& pos, bool dropResources) override;
        void SetBlock(const glm::ivec3& pos, Game::BlockID block) override;
        bool MobGriefing() const override;
        void AddFreshEntity(std::unique_ptr<Game::Entity> entity) override;

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

        Game::Difficulty m_difficulty = Game::Difficulty::Normal;
        mutable Game::JavaRandom m_random{0};

        std::unordered_map<uint32_t, std::unique_ptr<PlayerEntityView>> m_playerViews;
        std::vector<PlayerEntityView*> m_playerViewList;

        std::vector<PendingEvent> m_pendingEvents;
        std::vector<std::unique_ptr<Game::Entity>> m_spawned;
    };

} // namespace Server
