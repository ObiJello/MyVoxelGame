// File: src/common/world/block/entity/TrialSpawnerBlockEntity.hpp
//
// MC TrialSpawnerBlockEntity with the TrialSpawner it owns
// (world/level/block/entity/trialspawner/*): the trial chambers' spawner.
//
//   TrialSpawnerState      the trial_spawner_state block property and its
//                          tickAndGetNext machine:
//                            inactive → waiting_for_players → active →
//                            waiting_for_reward_ejection → ejecting_reward →
//                            cooldown (30 min) → waiting_for_players
//   TrialSpawnerStateData  who was detected, which mobs are still out, the
//                          clocks, the next SpawnData and the loot table
//                          being ejected — saved with the block entity
//   TrialSpawnerConfig     the normal and ominous configs (a registry
//                          reference or a direct value — TrialSpawnerConfig.hpp)
//
// Players are detected every 20 ticks (staggered by position) within 14
// blocks, creative and spectator players excluded, the first one needing a
// clear VISUAL line from the spawner's centre to their eyes. Each detected
// player beyond the first raises the total and simultaneous mob targets by
// the config's per-player amounts. Every spawned mob is persistent and
// tracked by UUID; a mob that dies, leaves the dimension or wanders 47 blocks
// off stops counting. With every mob dead the shutter opens (40 ticks), one
// loot table (weighted pick from loot_tables_to_eject) is ejected per detected
// player every 30 ticks, and the spawner cools down for targetCooldownLength.
//
// Ominous trials: a detected player carrying Trial Omen — or Bad Omen, which
// is converted there and then (Trial Omen for 15 minutes per Bad Omen level)
// — turns the spawner ominous: every tracked mob is discarded (its preserved
// equipment dropped), the ominous config takes over, and while ACTIVE it
// drops an OminousItemSpawner above a random player or tracked mob every
// 160 ticks (the dispensing list is items_to_drop_when_ominous rolled once
// per 30x20x30 region from the level seed). The cooldown ends the omen.
//
// Server-only operations (loading a mob from its compound, the type's
// spawn-placement predicate, adding it to the level) go through the monster
// spawner's SpawnerServerHooks and TrialSpawnerServerHooks below.
//
// Wire (MC getUpdateTag): the ticks until the next spawn while ACTIVE (MC
// sends next_mob_spawns_at; a relative count needs no shared clock) and the
// next SpawnData's display type — what the client's cage spin and the mini
// mob (TrialSpawnerRenderer) read.
#pragma once

#include "BlockEntity.hpp"
#include "SpawnerBlockEntity.hpp"
#include "TrialSpawnerConfig.hpp"
#include "common/core/Uuid.hpp"
#include "common/entity/Item.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Game {

    class Entity;
    class JavaRandom;
    class LivingEntity;
    class Mob;
    struct EntityLevel;

    // MC TrialSpawnerState, in its declaration (and block-property) order.
    enum class TrialSpawnerState : uint8_t {
        Inactive = 0,
        WaitingForPlayers,
        Active,
        WaitingForRewardEjection,
        EjectingReward,
        Cooldown,
    };

    namespace TrialSpawnerStates {
        // The trial_spawner_state value names.
        const char* Name(TrialSpawnerState state);
        TrialSpawnerState Of(BlockState state);
        bool OminousOf(BlockState state);
        // MC TrialSpawnerState.lightLevel / spinningMobSpeed / hasSpinningMob /
        // isCapableOfSpawning.
        int    LightLevel(TrialSpawnerState state);
        double SpinningMobSpeed(TrialSpawnerState state);
        bool   HasSpinningMob(TrialSpawnerState state);
        bool   IsCapableOfSpawning(TrialSpawnerState state);
    }

    // What the trial spawner needs from the server beyond SpawnerServerHooks.
    struct TrialSpawnerServerHooks {
        // MC SpawnPlacements.checkSpawnRules(type, level, TRIAL_SPAWNER, pos,
        // random) — the type's registered predicate with the level context.
        bool (*checkSpawnRules)(EntityTypeId type, World& world, const glm::ivec3& pos,
                                JavaRandom& random) = nullptr;
    };
    void SetTrialSpawnerServerHooks(const TrialSpawnerServerHooks& hooks);

    class TrialSpawnerBlockEntity : public BlockEntity {
    public:
        // MC TrialSpawner constants.
        static constexpr int kDetectPlayerSpawnBuffer   = 40;
        static constexpr int kMaxMobTrackingDistance    = 47;
        static constexpr float kSpawningAmbientSoundChance = 0.02f;
        // TrialSpawnerStateData.
        static constexpr int kTrialOmenPerBadOmenLevel  = 18000;
        // TrialSpawnerState.
        static constexpr float kDelayBeforeEjectAfterKillingLastMob = 40.0f;
        static constexpr float kTimeBetweenEachEjection = 30.0f;

        // MC TrialSpawnerStateData (the persisted half, Packed).
        struct StateData {
            std::vector<Uuid> detectedPlayers;   // a set; insertion order kept
            std::vector<Uuid> currentMobs;       // a set; insertion order kept
            int64_t cooldownEndsAt = 0;
            int64_t nextMobSpawnsAt = 0;
            int     totalMobsSpawned = 0;
            std::optional<SpawnData>   nextSpawnData;
            std::optional<std::string> ejectingLootTable;
        };

        TrialSpawnerBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        bool NeedsTicking() const override { return true; }
        // MC TrialSpawner.tickServer / tickClient.
        void Tick(World* world, float deltaTime) override;
        void ClientTick(ILevelWrite& level) override;

        // MC TrialSpawnerBlockEntity.setEntityId (a spawn egg): both configs
        // spawn `type`, the state data resets and the spawner goes INACTIVE.
        void SetEntityId(EntityTypeId type, ILevelWrite& level);

        // ── Persistent state (TrialSpawner.load / store — the NBT layer) ──
        const TrialSpawnerFullConfig& GetFullConfig() const { return m_config; }
        void SetFullConfig(TrialSpawnerFullConfig config) { m_config = std::move(config); }
        const StateData& GetStateData() const { return m_data; }
        void SetStateData(StateData data);

        // ── Client (TrialSpawnerRenderer) ─────────────────────────────────
        double GetSpin() const { return m_spin; }
        double GetOSpin() const { return m_oSpin; }
        // The display mob: the next SpawnData's type, if it names one.
        bool HasDisplayEntity() const { return m_hasDisplay; }
        EntityTypeId GetDisplayType() const { return m_displayType; }
        bool IsDisplayBaby() const { return m_displayBaby; }
        // The client's block state at this entity (the spin speed and the
        // renderer's display gate read it).
        TrialSpawnerState GetClientState() const { return m_clientState; }

        void CarryClientState(const BlockEntity& previous) override;

        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

    private:
        // ── TrialSpawner ────────────────────────────────────────────────────
        const TrialSpawnerConfig& ActiveConfig() const;
        const TrialSpawnerConfig& OminousConfig() const { return m_config.ominous.Get(); }
        bool CanSpawnInLevel(World& world, EntityLevel& level) const;
        // MC spawnMob: the spawned mob's UUID, or nothing.
        std::optional<Uuid> SpawnMob(World& world, EntityLevel& level);
        void EjectReward(World& world, const std::string& lootTable);
        void ApplyOminous(World& world, EntityLevel& level);
        void RemoveOminous(World& world);
        void SetState(World& world, TrialSpawnerState state);
        // MC markUpdated: setChanged + the block update that re-sends the
        // update tag.
        void MarkUpdated();

        // ── TrialSpawnerState.tickAndGetNext ────────────────────────────────
        TrialSpawnerState TickAndGetNext(TrialSpawnerState current, World& world, EntityLevel& level);
        void SpawnOminousItemSpawner(World& world, EntityLevel& level);
        std::optional<glm::dvec3> CalculatePositionToSpawnSpawner(World& world, EntityLevel& level);

        // ── TrialSpawnerStateData ───────────────────────────────────────────
        const SpawnData& GetOrCreateNextSpawnData(JavaRandom& random);
        bool HasMobToSpawn(JavaRandom& random);
        int  CountAdditionalPlayers() const;
        void TryDetectPlayers(World& world, EntityLevel& level);
        void ResetAfterBecomingOminous(World& world, EntityLevel& level);
        void Reset();
        void ResetStatistics();
        // MC getDispensingItems: the ominous item list (item, weight), rolled
        // once per region and cached.
        const std::vector<std::pair<ItemStack, int>>& GetDispensingItems(World& world);

        // PlayerDetector.NO_CREATIVE_PLAYERS: the UUIDs of the players
        // within range, optionally only those in line of sight.
        std::vector<Uuid> DetectPlayers(World& world, EntityLevel& level, bool requireLineOfSight) const;
        LivingEntity* PlayerByUuid(EntityLevel& level, const Uuid& uuid) const;
        Entity* EntityByUuid(EntityLevel& level, const Uuid& uuid) const;

        TrialSpawnerFullConfig m_config;
        StateData m_data;
        bool m_isOminous = false;

        // Server runtime (not saved, as in MC): the dispensing list.
        bool m_hasDispensing = false;
        std::vector<std::pair<ItemStack, int>> m_dispensing;

        // Client mirror (wire) and animation.
        int  m_clientTicksUntilSpawn = 0;
        bool m_hasDisplay = false;
        EntityTypeId m_displayType{};
        bool m_displayBaby = false;
        TrialSpawnerState m_clientState = TrialSpawnerState::Inactive;
        double m_spin = 0.0;
        double m_oSpin = 0.0;
    };

} // namespace Game
