// File: src/common/world/block/entity/SpawnerBlockEntity.hpp
//
// Mirrors net.minecraft.world.level.block.entity.SpawnerBlockEntity and the
// BaseSpawner it owns (net.minecraft.world.level.BaseSpawner) — the monster
// spawner: dungeons' zombies/skeletons/spiders, mineshaft cave spiders,
// stronghold silverfish, fortress blazes, and anything a spawn egg reprograms
// it to.
//
//   SpawnData        what to spawn (MC SpawnData: the entity compound, optional
//                    custom_spawn_rules light ranges; `equipment` is carried by
//                    MC but this port has no equipment tables — see below)
//   SpawnPotentials  weighted SpawnData, re-drawn after every spawn cycle
//   Delay            ticks until the next cycle (20 initially, then
//                    MinSpawnDelay + nextInt(MaxSpawnDelay - MinSpawnDelay))
//   SpawnCount       attempts per cycle (4)
//   SpawnRange       horizontal scatter and the nearby-count box (4)
//   MaxNearbyEntities  cap of same-type mobs in that box (6)
//   RequiredPlayerRange  the spawner sleeps unless a player is this close (16)
//
// The entity compound is kept as BINARY NBT (a named root compound) so any
// field a vanilla or template spawner carries survives a save and reaches
// the spawned mob untouched. Common code cannot read it — the NBT reader is
// server-side — so the reader that fills a SpawnData also fills the few
// fields the tick needs (type, "Pos", whether the compound is bare), and the
// server installs the one hook that turns a compound into a mob
// (SetSpawnerServerHooks, like MobEffects' SetEffectMobFactory).
//
// Wire: MC's getUpdateTag — everything but SpawnPotentials. The client needs
// the delay (the spin speed), MinSpawnDelay (event 1 resets the delay to it),
// RequiredPlayerRange (the spin only runs near a player) and the next
// SpawnData's type (the mini mob in the cage, SpawnerRenderer).
//
// Deviations, each an absent engine system rather than a choice:
//   * SpawnData.equipment (an EquipmentTable) is not applied — no mob
//     equipment tables exist here. It is kept on disk round-trip only as far
//     as the entity compound goes; the equipment field itself is dropped.
//   * Only mobs spawn: a SpawnData naming a non-mob entity (TNT, an item, a
//     minecart) has no loader here, so the cycle ends as for an unknown id
//     (delay and stop). Passengers in the compound are not loaded.
//   * MC broadcasts LevelEvent 2004 (smoke + flame burst) after each spawn;
//     this port has no level-event packet, so it rides this block's own
//     event channel as engine event kEventSpawnParticles (documented below).
//     GameEvent.ENTITY_PLACE is not raised (no vibration system).
#pragma once

#include "BlockEntity.hpp"
#include "common/entity/EntityType.hpp"

#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace Game {

    class Mob;
    class JavaRandom;
    struct EntityLevel;

    // MC SpawnData.
    struct SpawnData {
        // MC SpawnData.entityToSpawn — the entity compound as binary NBT (a
        // named root compound), empty for MC's `new SpawnData()` (an empty
        // compound: nothing to spawn).
        std::vector<uint8_t> entityNbt;

        // Parsed from entityNbt by whoever filled it (server NBT reader or
        // SetEntityId): the "id" (hasType false when absent or unknown to this
        // build), "Pos" when present (MC reads it as the fixed spawn position),
        // and MC's `entityToSpawn.size() == 1 && id present` — a bare {id}
        // compound, the only kind that gets finalizeSpawn(SPAWNER).
        bool         hasType = false;
        EntityTypeId type{};
        bool         hasPos = false;
        glm::dvec3   pos{0.0};
        bool         bareId = false;
        // The display entity's one visible NBT property: a baby
        // ("IsBaby":1b, or an ageable's negative "Age").
        bool         baby = false;

        // MC SpawnData.CustomSpawnRules — inclusive light ranges (0..15).
        bool hasCustomRules = false;
        int  blockLightMin = 0, blockLightMax = 15;
        int  skyLightMin = 0,   skyLightMax = 15;
    };

    // MC Weighted<SpawnData> — one SpawnPotentials entry.
    struct WeightedSpawnData {
        SpawnData data;
        int       weight = 1;
    };

    // Server-side operations the spawner's tick needs and common code cannot
    // perform. Installed once by the server (IntegratedServer); null hooks
    // make the spawner idle, never crash.
    struct SpawnerServerHooks {
        // MC EntityType.loadEntityRecursive(input, level, SPAWNER, snapTo):
        // build the mob the compound describes and apply the compound. The
        // caller positions it. Null when the id is unknown to this build.
        std::unique_ptr<Mob> (*loadEntity)(const SpawnData& data, EntityLevel& level) = nullptr;
        // MC `entityToSpawn.putString("id", type)` — rewrite the compound's
        // id, every other field kept, and refresh the parsed fields.
        void (*setEntityId)(SpawnData& data, EntityTypeId type) = nullptr;
        // MC SpawnPlacements.checkSpawnRules(type, level, SPAWNER, pos,
        // random) — the type's registered predicate, with the level context
        // (biome, surface, sea level, seed) only the server has.
        bool (*checkSpawnRules)(EntityTypeId type, World& world, const glm::ivec3& pos,
                                JavaRandom& random) = nullptr;
        // MC ServerLevel.tryAddFreshEntityWithPassengers: the mob joins the
        // level NOW — MC's addFreshEntity is immediate, so the next attempt of
        // the same cycle counts it in the nearby census and collides with it
        // in checkSpawnObstruction, and spawnAnim's entity event has a real
        // id. Refuses (the mob is dropped, false) when its UUID is already in
        // the level, as MC does for a compound carrying a fixed UUID.
        bool (*addFreshEntity)(std::unique_ptr<Mob>& mob, World& world) = nullptr;
    };
    void SetSpawnerServerHooks(const SpawnerServerHooks& hooks);

    class SpawnerBlockEntity : public BlockEntity {
    public:
        // MC BaseSpawner defaults.
        static constexpr int kDefaultSpawnDelay        = 20;
        static constexpr int kDefaultMinSpawnDelay     = 200;
        static constexpr int kDefaultMaxSpawnDelay     = 800;
        static constexpr int kDefaultSpawnCount        = 4;
        static constexpr int kDefaultMaxNearbyEntities = 6;
        static constexpr int kDefaultRequiredPlayerRange = 16;
        static constexpr int kDefaultSpawnRange        = 4;

        // MC BaseSpawner.EVENT_SPAWN: the block event that resets the
        // client's delay (onEventTriggered).
        static constexpr int kEventSpawn = 1;
        // Engine block event (deviation, see the header): the client plays
        // MC LevelEvent 2004 (PARTICLES_MOBBLOCK_SPAWN) — 20 SMOKE + FLAME
        // around the spawner. The event id rides a byte on the wire, so it
        // is 200, well clear of MC's own block-event ids.
        static constexpr int kEventSpawnParticles = 200;

        SpawnerBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        // MC SpawnerBlockEntity.serverTick / clientTick.
        bool NeedsTicking() const override { return true; }
        void Tick(World* world, float deltaTime) override;
        void ClientTick(ILevelWrite& level) override;

        // MC SpawnerBlockEntity.triggerEvent → BaseSpawner.onEventTriggered.
        bool TriggerEvent(int b0, int b1) override;

        // MC SpawnerBlockEntity.setEntityId (the spawn egg's first branch):
        // the next SpawnData's compound gets {id} (created from the
        // potentials, or empty, first — getOrCreateNextSpawnData). The
        // caller sends the block update (World::BlockEntityChanged).
        void SetEntityId(EntityTypeId type, JavaRandom& random);

        // ── State (MC BaseSpawner.load/save; the Anvil reader and writer) ──
        int  GetSpawnDelay() const { return m_spawnDelay; }
        void SetSpawnDelay(int v) { m_spawnDelay = v; }
        int  GetMinSpawnDelay() const { return m_minSpawnDelay; }
        int  GetMaxSpawnDelay() const { return m_maxSpawnDelay; }
        int  GetSpawnCount() const { return m_spawnCount; }
        int  GetMaxNearbyEntities() const { return m_maxNearbyEntities; }
        int  GetRequiredPlayerRange() const { return m_requiredPlayerRange; }
        int  GetSpawnRange() const { return m_spawnRange; }
        void SetConfig(int minDelay, int maxDelay, int spawnCount, int maxNearby,
                       int requiredPlayerRange, int spawnRange) {
            m_minSpawnDelay = minDelay;
            m_maxSpawnDelay = maxDelay;
            m_spawnCount = spawnCount;
            m_maxNearbyEntities = maxNearby;
            m_requiredPlayerRange = requiredPlayerRange;
            m_spawnRange = spawnRange;
        }

        bool HasNextSpawnData() const { return m_hasNextSpawnData; }
        const SpawnData& GetNextSpawnData() const { return m_nextSpawnData; }
        // MC load: "SpawnData" when present; potentials default to a single
        // entry of it (or of an empty SpawnData) when "SpawnPotentials" is
        // absent — call SetSpawnPotentials after, with `present` false.
        void SetNextSpawnData(const SpawnData& data) {
            m_nextSpawnData = data;
            m_hasNextSpawnData = true;
        }
        const std::vector<WeightedSpawnData>& GetSpawnPotentials() const { return m_spawnPotentials; }
        void SetSpawnPotentials(std::vector<WeightedSpawnData> potentials, bool present);

        // Client: MC getSpin / getOSpin (degrees / 10 — the renderer
        // multiplies by 10, as SpawnerRenderer does).
        double GetSpin() const { return m_spin; }
        double GetOSpin() const { return m_oSpin; }

        // Keep the cage spinning through a data update: the client rebuilds
        // this entity from each BlockEntityDataS2C, where MC loads into the
        // existing one and its spin survives (BaseSpawner.load leaves it).
        void CarryClientState(const BlockEntity& previous) override;

        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

    private:
        // MC BaseSpawner.delay — the next delay, the next SpawnData drawn from
        // the potentials, and block event 1.
        void Delay(ILevelWrite& level, JavaRandom& random);
        // MC getOrCreateNextSpawnData.
        const SpawnData& GetOrCreateNextSpawnData(JavaRandom& random);
        // MC setNextSpawnData, with SpawnerBlockEntity's override: the block
        // update that sends the new display entity to the clients.
        void SetNextSpawnDataAndSync(const SpawnData& data);
        // MC WeightedList.getRandom over the potentials: the index drawn, or
        // -1 (no draw) when the list weighs nothing.
        int PickPotential(JavaRandom& random) const;

        int  m_spawnDelay = kDefaultSpawnDelay;
        int  m_minSpawnDelay = kDefaultMinSpawnDelay;
        int  m_maxSpawnDelay = kDefaultMaxSpawnDelay;
        int  m_spawnCount = kDefaultSpawnCount;
        int  m_maxNearbyEntities = kDefaultMaxNearbyEntities;
        int  m_requiredPlayerRange = kDefaultRequiredPlayerRange;
        int  m_spawnRange = kDefaultSpawnRange;

        SpawnData m_nextSpawnData;
        bool      m_hasNextSpawnData = false;
        std::vector<WeightedSpawnData> m_spawnPotentials;
        // Java's SpawnData is a reference: nextSpawnData IS the potential it
        // was drawn from (and, for a spawner loaded without SpawnPotentials,
        // the one-entry default list holds the loaded SpawnData itself), so
        // the spawn egg's in-place putString("id") changes both. The index of
        // the potential nextSpawnData aliases, or -1.
        int       m_nextAliasesPotential = -1;

        // Client-side cage animation (MC spin / oSpin).
        double m_spin = 0.0;
        double m_oSpin = 0.0;
    };

} // namespace Game
