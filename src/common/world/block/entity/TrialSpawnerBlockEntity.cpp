// File: src/common/world/block/entity/TrialSpawnerBlockEntity.cpp
//
// MC TrialSpawner / TrialSpawnerState.tickAndGetNext / TrialSpawnerStateData
// over the TrialSpawnerBlockEntity that owns them. See the header.
#include "TrialSpawnerBlockEntity.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/MobEquipment.hpp"
#include "common/entity/OminousItemSpawner.hpp"
#include "common/entity/SpawnReason.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/physics/Physics.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/DispenseItemBehavior.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/block/GeneratedBlockStates.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/VisualClip.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/loot/ChestLootTables.hpp"
#include "common/world/spawn/SpawnPlacements.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <string_view>

namespace Game {

    namespace {

        TrialSpawnerServerHooks g_trialHooks;

        constexpr const char* kStateNames[] = {
            "inactive", "waiting_for_players", "active",
            "waiting_for_reward_ejection", "ejecting_reward", "cooldown",
        };

        // MC TrialSpawner.FlameParticle: NORMAL (FLAME) = 0, OMINOUS
        // (SOUL_FIRE_FLAME) = 1 — the data of level events 3011 / 3012 / 3021.
        constexpr int kFlameNormal  = 0;
        constexpr int kFlameOminous = 1;

        // MC BlockPos.asLong — x 26 bits << 38, z 26 bits << 12, y 12 bits.
        int64_t BlockPosAsLong(const glm::ivec3& p) {
            const uint64_t x = static_cast<uint64_t>(static_cast<int64_t>(p.x)) & 0x3FFFFFFull;
            const uint64_t y = static_cast<uint64_t>(static_cast<int64_t>(p.y)) & 0xFFFull;
            const uint64_t z = static_cast<uint64_t>(static_cast<int64_t>(p.z)) & 0x3FFFFFFull;
            return static_cast<int64_t>((x << 38) | (z << 12) | y);
        }

        glm::dvec3 CenterOf(const glm::ivec3& p) {
            return glm::dvec3(p.x + 0.5, p.y + 0.5, p.z + 0.5);
        }

        glm::ivec3 Containing(const glm::dvec3& v) {
            return glm::ivec3(static_cast<int>(std::floor(v.x)), static_cast<int>(std::floor(v.y)),
                              static_cast<int>(std::floor(v.z)));
        }

        // MC Vec3i.distSqr — the integer difference, squared, as doubles.
        double DistSqr(const glm::ivec3& a, const glm::ivec3& b) {
            const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz;
        }

        // MC TrialSpawner.inLineOfSight / PlayerDetector.inLineOfSight: a
        // VISUAL clip from `dest` back to `origin` either misses or stops in
        // the origin's own cell (the spawner block itself).
        bool InLineOfSight(const IBlockAccess& blocks, const glm::dvec3& origin, const glm::dvec3& dest) {
            const VisualClipResult hit = ClipVisual(blocks, dest, origin);
            return !hit.hit || hit.blockPos == Containing(origin);
        }

        void AddUnique(std::vector<Uuid>& set, const Uuid& id) {
            if (std::find(set.begin(), set.end(), id) == set.end()) set.push_back(id);
        }

        // Vec3.offsetRandom(random, spread).
        glm::dvec3 OffsetRandom(const glm::dvec3& v, JavaRandom& random, float spread) {
            const double x = static_cast<double>((random.NextFloat() - 0.5f) * spread);
            const double y = static_cast<double>((random.NextFloat() - 0.5f) * spread);
            const double z = static_cast<double>((random.NextFloat() - 0.5f) * spread);
            return v + glm::dvec3(x, y, z);
        }

    } // namespace

    void SetTrialSpawnerServerHooks(const TrialSpawnerServerHooks& hooks) { g_trialHooks = hooks; }

    // ── TrialSpawnerState ───────────────────────────────────────────────────

    namespace TrialSpawnerStates {

        const char* Name(TrialSpawnerState state) {
            const size_t i = static_cast<size_t>(state);
            return i < std::size(kStateNames) ? kStateNames[i] : kStateNames[0];
        }

        TrialSpawnerState Of(BlockState state) {
            if (!state.HasProperty(PropertyId::TRIAL_SPAWNER_STATE)) return TrialSpawnerState::Inactive;
            const std::string_view name = state.GetName(PropertyId::TRIAL_SPAWNER_STATE);
            for (size_t i = 0; i < std::size(kStateNames); ++i) {
                if (name == kStateNames[i]) return static_cast<TrialSpawnerState>(i);
            }
            return TrialSpawnerState::Inactive;
        }

        bool OminousOf(BlockState state) {
            // getOptionalValue(OMINOUS).orElse(false).
            return state.HasProperty(PropertyId::OMINOUS) && state.GetName(PropertyId::OMINOUS) == "true";
        }

        int LightLevel(TrialSpawnerState state) {
            switch (state) {
                case TrialSpawnerState::WaitingForPlayers:        return 4;   // HALF_LIT
                case TrialSpawnerState::Active:
                case TrialSpawnerState::WaitingForRewardEjection:
                case TrialSpawnerState::EjectingReward:           return 8;   // LIT
                default:                                          return 0;   // UNLIT
            }
        }

        double SpinningMobSpeed(TrialSpawnerState state) {
            switch (state) {
                case TrialSpawnerState::WaitingForPlayers: return 200.0;    // SLOW
                case TrialSpawnerState::Active:            return 1000.0;   // FAST
                default:                                   return -1.0;     // NONE
            }
        }

        bool HasSpinningMob(TrialSpawnerState state) { return SpinningMobSpeed(state) >= 0.0; }

        bool IsCapableOfSpawning(TrialSpawnerState state) {
            return state == TrialSpawnerState::WaitingForPlayers || state == TrialSpawnerState::Active;
        }

    } // namespace TrialSpawnerStates

    // ── TrialSpawner ────────────────────────────────────────────────────────

    const TrialSpawnerConfig& TrialSpawnerBlockEntity::ActiveConfig() const {
        return m_isOminous ? m_config.ominous.Get() : m_config.normal.Get();
    }

    void TrialSpawnerBlockEntity::SetStateData(StateData data) {
        m_data = std::move(data);
        MarkDirty();
    }

    void TrialSpawnerBlockEntity::MarkUpdated() {
        MarkDirty();
        if (ILevelWrite* level = GetLevel(); level && !level->IsClientSide()) {
            level->BlockEntityChanged(GetWorldPos());
        }
    }

    void TrialSpawnerBlockEntity::SetState(World& world, TrialSpawnerState state) {
        // MC TrialSpawnerBlockEntity.setState: setChanged, then
        // setBlockAndUpdate with the property swapped on the CURRENT state
        // (applyOminous may have just rewritten it).
        MarkDirty();
        const glm::ivec3 pos = GetWorldPos();
        const BlockState current = world.GetBlockState(pos.x, pos.y, pos.z);
        if (current.Block() != BlockID::TrialSpawner) return;
        world.SetBlock(pos.x, pos.y, pos.z,
                       current.SetName(PropertyId::TRIAL_SPAWNER_STATE, TrialSpawnerStates::Name(state)),
                       World::UpdateFlags::All);
    }

    bool TrialSpawnerBlockEntity::CanSpawnInLevel(World& world, EntityLevel& level) const {
        // MC canSpawnInLevel: spawner_blocks_work, then (no test override)
        // not Peaceful and spawn_mobs.
        if (!Rules::GetBool(Rules::Id::SpawnerBlocksWork)) return false;
        if (world.GetDifficulty() == Difficulty::Peaceful) return false;
        return level.DoMobSpawning();
    }

    void TrialSpawnerBlockEntity::ApplyOminous(World& world, EntityLevel& level) {
        const glm::ivec3 pos = GetWorldPos();
        const BlockState current = world.GetBlockState(pos.x, pos.y, pos.z);
        if (current.Block() == BlockID::TrialSpawner) {
            world.SetBlock(pos.x, pos.y, pos.z, current.SetName(PropertyId::OMINOUS, "true"),
                           World::UpdateFlags::All);
        }
        world.PlayLevelEvent(nullptr, LevelEvent::PARTICLES_TRIAL_SPAWNER_BECOME_OMINOUS, pos, 1);
        m_isOminous = true;
        ResetAfterBecomingOminous(world, level);
    }

    void TrialSpawnerBlockEntity::RemoveOminous(World& world) {
        const glm::ivec3 pos = GetWorldPos();
        const BlockState current = world.GetBlockState(pos.x, pos.y, pos.z);
        if (current.Block() == BlockID::TrialSpawner) {
            world.SetBlock(pos.x, pos.y, pos.z, current.SetName(PropertyId::OMINOUS, "false"),
                           World::UpdateFlags::All);
        }
        m_isOminous = false;
    }

    std::optional<Uuid> TrialSpawnerBlockEntity::SpawnMob(World& world, EntityLevel& level) {
        const SpawnerServerHooks& hooks = GetSpawnerServerHooks();
        JavaRandom& random = level.Random();
        const glm::ivec3 spawnerPos = GetWorldPos();
        // A copy: nothing below may change it, but the spawn outlives the
        // reference's lifetime guarantees if the data resets mid-call.
        const SpawnData next = GetOrCreateNextSpawnData(random);

        // EntityType.by(input): no (known) id — nothing spawns.
        if (!next.hasType) return std::nullopt;
        const EntityTypeId type = next.type;
        const EntityTypeInfo& info = GetEntityTypeInfo(type);

        // input.read("Pos") or a scatter around the spawner, in Vec3's
        // argument order (x's two doubles, y's int, z's two doubles).
        glm::dvec3 spawnPos;
        if (next.hasPos) {
            spawnPos = next.pos;
        } else {
            const double range = static_cast<double>(ActiveConfig().spawnRange);
            const double x = spawnerPos.x + (random.NextDouble() - random.NextDouble()) * range + 0.5;
            const double y = static_cast<double>(spawnerPos.y + random.NextInt(3) - 1);
            const double z = spawnerPos.z + (random.NextDouble() - random.NextDouble()) * range + 0.5;
            spawnPos = glm::dvec3(x, y, z);
        }

        // level.noCollision(type.getSpawnAABB(x, y, z)).
        {
            const float scale = GetSpawnDimensionsScale(type);
            const float halfWidth = scale * info.width / 2.0f;
            const float height = scale * info.height;
            AABBd box;
            box.min = glm::dvec3(spawnPos.x - halfWidth, spawnPos.y, spawnPos.z - halfWidth);
            box.max = glm::dvec3(spawnPos.x + halfWidth, spawnPos.y + height, spawnPos.z + halfWidth);
            PhysicsContext phys;
            phys.blockAccess = &world;
            if (CollidesAt(box, phys)) return std::nullopt;
        }

        // inLineOfSight(level, Vec3.atCenterOf(spawnerPos), spawnPos).
        if (!InLineOfSight(world, CenterOf(spawnerPos), spawnPos)) return std::nullopt;

        // SpawnPlacements.checkSpawnRules(type, level, TRIAL_SPAWNER, pos, random)
        // — both it AND any custom rules must pass (BaseSpawner uses one or
        // the other; the trial spawner requires both).
        const glm::ivec3 spawnBlockPos = Containing(spawnPos);
        if (g_trialHooks.checkSpawnRules &&
            !g_trialHooks.checkSpawnRules(type, world, spawnBlockPos, level.Random())) {
            return std::nullopt;
        }
        if (next.hasCustomRules) {
            // CustomSpawnRules.isValidPosition: getBrightness(BLOCK) and
            // getEffectiveSkyBrightness.
            const int blockLight = level.GetBlockBrightness(spawnBlockPos.x, spawnBlockPos.y, spawnBlockPos.z);
            const int skyLight = std::max(0, level.GetSkyBrightness(spawnBlockPos.x, spawnBlockPos.y,
                                                                    spawnBlockPos.z) - level.GetSkyDarken());
            if (blockLight < next.blockLightMin || blockLight > next.blockLightMax ||
                skyLight < next.skyLightMin || skyLight > next.skyLightMax) {
                return std::nullopt;
            }
        }

        // EntityType.loadEntityRecursive(input, level, TRIAL_SPAWNER,
        // snapTo(spawnPos, nextFloat() * 360, 0)).
        std::unique_ptr<Mob> mob = hooks.loadEntity ? hooks.loadEntity(next, level) : nullptr;
        if (!mob) return std::nullopt;
        mob->position = spawnPos;
        mob->oldPosition = spawnPos;
        mob->yRot = random.NextFloat() * 360.0f;
        mob->xRot = 0.0f;
        mob->yRotO = mob->yRot;
        mob->xRotO = mob->xRot;
        mob->yHeadRot = mob->yHeadRotO = mob->yRot;
        mob->yBodyRot = mob->yBodyRotO = mob->yRot;

        if (!mob->CheckSpawnObstruction(level)) return std::nullopt;
        // Only a bare {id} is finalized: a configured compound already says
        // what the mob is.
        if (next.bareId) mob->FinalizeSpawn(SpawnReason::TrialSpawner, nullptr);
        mob->SetPersistenceRequired(true);
        // nextSpawnData.getEquipment().ifPresent(mob::equip).
        MobEquipment::EquipFromSpawnData(*mob, next);

        // tryAddFreshEntityWithPassengers. The UUID is the spawner's handle
        // on the mob (currentMobs), so it exists before the level takes it.
        mob->MintUuidIfUnset();
        Mob* placed = mob.get();
        if (hooks.addFreshEntity) {
            if (!hooks.addFreshEntity(mob, world)) return std::nullopt;
        } else {
            level.AddFreshEntity(std::move(mob));
        }

        const int flame = m_isOminous ? kFlameOminous : kFlameNormal;
        world.PlayLevelEvent(nullptr, LevelEvent::PARTICLES_TRIAL_SPAWNER_SPAWN, spawnerPos, flame);
        world.PlayLevelEvent(nullptr, LevelEvent::PARTICLES_TRIAL_SPAWNER_SPAWN_MOB_AT, spawnBlockPos, flame);
        world.GameEvent(placed, GameEventId::EntityPlace, spawnBlockPos);
        return placed->GetUuid();
    }

    void TrialSpawnerBlockEntity::EjectReward(World& world, const std::string& lootTable) {
        // MC ejectReward: the table rolled with the EMPTY parameter set (the
        // level random), each stack spawned like a dispenser shooting UP
        // from 1.2 above the bottom centre, then level event 3014 when
        // anything came out.
        JavaRandom* random = world.Random();
        if (!random) return;
        std::vector<ItemStack> drops;
        if (!ChestLoot::GetRandomItems(lootTable, *random, 0.0f, drops)) return;
        if (drops.empty()) return;
        const glm::ivec3 pos = GetWorldPos();
        const glm::dvec3 from(pos.x + 0.5, pos.y + 1.2, pos.z + 0.5);
        for (const ItemStack& item : drops) {
            DispenseSpawnItem(world, item, 2, static_cast<int>(Direction::Up), from);
        }
        world.PlayLevelEvent(nullptr, LevelEvent::ANIMATION_TRIAL_SPAWNER_EJECT_ITEM, pos, 0);
    }

    void TrialSpawnerBlockEntity::SetEntityId(EntityTypeId type, ILevelWrite& level) {
        // MC overrideEntityToSpawn(type, level): data.reset(), the configs
        // replaced, the state set to INACTIVE; then setChanged.
        Reset();
        m_config = m_config.OverrideEntity(type);
        const glm::ivec3 pos = GetWorldPos();
        const BlockState current = level.GetBlockState(pos.x, pos.y, pos.z);
        if (current.Block() == BlockID::TrialSpawner) {
            level.SetBlock(pos.x, pos.y, pos.z,
                           current.SetName(PropertyId::TRIAL_SPAWNER_STATE,
                                           TrialSpawnerStates::Name(TrialSpawnerState::Inactive)),
                           World::UpdateFlags::All);
        }
        MarkDirty();
    }

    // ── TrialSpawner.tickServer ─────────────────────────────────────────────

    void TrialSpawnerBlockEntity::Tick(World* world, float /*deltaTime*/) {
        if (!world) return;
        EntityLevel* level = world->Entities();
        if (!level) return;
        const glm::ivec3 pos = GetWorldPos();
        const BlockState blockState = world->GetBlockState(pos.x, pos.y, pos.z);
        if (blockState.Block() != BlockID::TrialSpawner) return;

        m_isOminous = TrialSpawnerStates::OminousOf(blockState);
        const TrialSpawnerState current = TrialSpawnerStates::Of(blockState);

        // currentMobs.removeIf(shouldMobBeUntracked): gone, dead, in another
        // level, or more than 47 blocks off. Any removal re-arms the spawn
        // clock.
        const size_t before = m_data.currentMobs.size();
        m_data.currentMobs.erase(
            std::remove_if(m_data.currentMobs.begin(), m_data.currentMobs.end(),
                [&](const Uuid& id) {
                    Entity* entity = EntityByUuid(*level, id);
                    return !entity || !entity->IsAlive() ||
                           DistSqr(entity->BlockPosition(), pos) >
                               static_cast<double>(kMaxMobTrackingDistance * kMaxMobTrackingDistance);
                }),
            m_data.currentMobs.end());
        if (m_data.currentMobs.size() != before) {
            m_data.nextMobSpawnsAt = world->GetGameTime() + ActiveConfig().ticksBetweenSpawn;
            MarkDirty();
        }

        const TrialSpawnerState next = TickAndGetNext(current, *world, *level);
        if (next != current) SetState(*world, next);
    }

    // ── TrialSpawnerState.tickAndGetNext ────────────────────────────────────

    TrialSpawnerState TrialSpawnerBlockEntity::TickAndGetNext(TrialSpawnerState current, World& world,
                                                              EntityLevel& level) {
        JavaRandom& random = level.Random();
        const int64_t now = world.GetGameTime();
        const glm::ivec3 pos = GetWorldPos();
        // MC reads activeConfig() once, at the top: a spawner turned ominous
        // by this tick's detection runs the rest of the tick on the config it
        // started with.
        const TrialSpawnerConfig& config = ActiveConfig();

        switch (current) {
            case TrialSpawnerState::Inactive:
                // getOrCreateDisplayEntity(this, level, WAITING_FOR_PLAYERS):
                // a display entity exists once the next SpawnData names a
                // type this build can build.
                return GetOrCreateNextSpawnData(random).hasType ? TrialSpawnerState::WaitingForPlayers
                                                                : TrialSpawnerState::Inactive;

            case TrialSpawnerState::WaitingForPlayers:
                if (!CanSpawnInLevel(world, level)) {
                    ResetStatistics();
                    return current;
                }
                if (!HasMobToSpawn(random)) return TrialSpawnerState::Inactive;
                TryDetectPlayers(world, level);
                return m_data.detectedPlayers.empty() ? current : TrialSpawnerState::Active;

            case TrialSpawnerState::Active: {
                if (!CanSpawnInLevel(world, level)) {
                    ResetStatistics();
                    return TrialSpawnerState::WaitingForPlayers;
                }
                if (!HasMobToSpawn(random)) return TrialSpawnerState::Inactive;
                const int additionalPlayers = CountAdditionalPlayers();
                TryDetectPlayers(world, level);
                if (m_isOminous) SpawnOminousItemSpawner(world, level);

                if (m_data.totalMobsSpawned >= config.CalculateTargetTotalMobs(additionalPlayers)) {
                    if (m_data.currentMobs.empty()) {
                        m_data.cooldownEndsAt = now + m_config.targetCooldownLength;
                        m_data.totalMobsSpawned = 0;
                        m_data.nextMobSpawnsAt = 0;
                        MarkDirty();
                        return TrialSpawnerState::WaitingForRewardEjection;
                    }
                } else if (now >= m_data.nextMobSpawnsAt &&
                           static_cast<int>(m_data.currentMobs.size()) <
                               config.CalculateTargetSimultaneousMobs(additionalPlayers)) {
                    if (const std::optional<Uuid> spawned = SpawnMob(world, level)) {
                        AddUnique(m_data.currentMobs, *spawned);
                        ++m_data.totalMobsSpawned;
                        m_data.nextMobSpawnsAt = now + config.ticksBetweenSpawn;
                        if (const int picked = config.PickSpawnPotential(random); picked >= 0) {
                            m_data.nextSpawnData = config.spawnPotentials[static_cast<size_t>(picked)].data;
                            MarkUpdated();
                        }
                        MarkDirty();
                    }
                }
                return current;
            }

            case TrialSpawnerState::WaitingForRewardEjection: {
                // isReadyToOpenShutter(level, 40, targetCooldownLength).
                const int64_t cooldownStartedAt = m_data.cooldownEndsAt - m_config.targetCooldownLength;
                if (static_cast<float>(now) >= static_cast<float>(cooldownStartedAt) +
                                                   kDelayBeforeEjectAfterKillingLastMob) {
                    world.PlaySound(nullptr, pos, SoundEvents::TRIAL_SPAWNER_OPEN_SHUTTER, SoundSource::Blocks);
                    return TrialSpawnerState::EjectingReward;
                }
                return current;
            }

            case TrialSpawnerState::EjectingReward: {
                // isReadyToEjectItems(level, 30, targetCooldownLength): every
                // 30 ticks since the cooldown started (a float remainder).
                const int64_t cooldownStartedAt = m_data.cooldownEndsAt - m_config.targetCooldownLength;
                const float elapsed = static_cast<float>(now - cooldownStartedAt);
                if (std::fmod(elapsed, kTimeBetweenEachEjection) != 0.0f) return current;
                if (m_data.detectedPlayers.empty()) {
                    world.PlaySound(nullptr, pos, SoundEvents::TRIAL_SPAWNER_CLOSE_SHUTTER, SoundSource::Blocks);
                    m_data.ejectingLootTable.reset();
                    MarkDirty();
                    return TrialSpawnerState::Cooldown;
                }
                if (!m_data.ejectingLootTable) {
                    const std::string picked = config.PickLootTableToEject(random);
                    if (!picked.empty()) m_data.ejectingLootTable = picked;
                }
                if (m_data.ejectingLootTable) EjectReward(world, *m_data.ejectingLootTable);
                // detectedPlayers.remove(detectedPlayers.iterator().next()).
                m_data.detectedPlayers.erase(m_data.detectedPlayers.begin());
                MarkDirty();
                return current;
            }

            case TrialSpawnerState::Cooldown:
                TryDetectPlayers(world, level);
                if (!m_data.detectedPlayers.empty()) {
                    m_data.totalMobsSpawned = 0;
                    m_data.nextMobSpawnsAt = 0;
                    MarkDirty();
                    return TrialSpawnerState::Active;
                }
                if (now >= m_data.cooldownEndsAt) {
                    RemoveOminous(world);
                    Reset();
                    return TrialSpawnerState::WaitingForPlayers;
                }
                return current;
        }
        return current;
    }

    void TrialSpawnerBlockEntity::SpawnOminousItemSpawner(World& world, EntityLevel& level) {
        // MC spawnOminousOminousItemSpawner: the dispensing list's weighted
        // pick is drawn EVERY tick, whether or not an item spawner is due.
        JavaRandom& random = level.Random();
        const auto& dispensing = GetDispensingItems(world);
        ItemStack item;
        {
            int64_t total = 0;
            for (const auto& [stack, weight] : dispensing) total += weight;
            if (total > 0 && total <= std::numeric_limits<int32_t>::max()) {
                int roll = random.NextInt(static_cast<int32_t>(total));
                for (const auto& [stack, weight] : dispensing) {
                    roll -= weight;
                    if (roll < 0) {
                        item = stack;
                        break;
                    }
                }
            }
        }
        if (item.IsEmpty()) return;
        if (world.GetGameTime() < m_data.cooldownEndsAt) return;   // timeToSpawnItemSpawner

        const std::optional<glm::dvec3> at = CalculatePositionToSpawnSpawner(world, level);
        if (!at) return;
        std::unique_ptr<OminousItemSpawner> spawner = OminousItemSpawner::Create(level, item);
        spawner->position = *at;
        spawner->oldPosition = *at;
        level.AddFreshEntity(std::move(spawner));
        const float pitch = (random.NextFloat() - random.NextFloat()) * 0.2f + 1.0f;
        world.PlaySound(nullptr, Containing(*at), SoundEvents::TRIAL_SPAWNER_SPAWN_ITEM_BEGIN,
                        SoundSource::Blocks, 1.0f, pitch);
        m_data.cooldownEndsAt = world.GetGameTime() + TrialSpawnerConfig::kTicksBetweenItemSpawners;
        MarkDirty();
    }

    std::optional<glm::dvec3> TrialSpawnerBlockEntity::CalculatePositionToSpawnSpawner(World& world,
                                                                                         EntityLevel& level) {
        const glm::dvec3 center = CenterOf(GetWorldPos());
        const double rangeSq = static_cast<double>(m_config.requiredPlayerRange) * m_config.requiredPlayerRange;

        // The detected players still here: not creative, not spectating,
        // alive, and within the detection range of the spawner's centre.
        std::vector<Entity*> nearbyPlayers;
        for (const Uuid& id : m_data.detectedPlayers) {
            LivingEntity* player = PlayerByUuid(level, id);
            if (!player || player->IsCreative() || player->IsSpectator() || !player->IsAlive()) continue;
            if (player->DistanceToSqr(center.x, center.y, center.z) <= rangeSq) nearbyPlayers.push_back(player);
        }
        if (nearbyPlayers.empty()) return std::nullopt;

        // selectEntityToSpawnItemAbove: a coin flip between the tracked mobs
        // near the spawner and those players.
        JavaRandom& random = level.Random();
        std::vector<Entity*> eligible;
        if (random.NextBool()) {
            for (const Uuid& id : m_data.currentMobs) {
                Entity* mob = EntityByUuid(level, id);
                if (mob && mob->IsAlive() && mob->DistanceToSqr(center.x, center.y, center.z) <= rangeSq) {
                    eligible.push_back(mob);
                }
            }
        } else {
            eligible = nearbyPlayers;
        }
        if (eligible.empty()) return std::nullopt;
        Entity* target = eligible.size() == 1
            ? eligible.front()
            : eligible[static_cast<size_t>(random.NextInt(static_cast<int32_t>(eligible.size())))];

        // calculatePositionAbove: 2..5 blocks over the target's head, clipped
        // against any ceiling, then one block down from the cell reached; a
        // cell with a collision shape refuses.
        const glm::dvec3 entityPos = target->position;
        const double up = static_cast<double>(target->GetBbHeight() + 2.0f +
                                              static_cast<float>(random.NextInt(4)));
        const glm::dvec3 trySpawnPos = entityPos + glm::dvec3(0.0, up, 0.0);
        const VisualClipResult hit = ClipVisual(world, entityPos, trySpawnPos);
        const glm::dvec3 down = CenterOf(hit.blockPos) - glm::dvec3(0.0, 1.0, 0.0);
        const glm::ivec3 downPos = Containing(down);
        const BlockState below = world.GetBlockState(downPos.x, downPos.y, downPos.z);
        if (BlockRegistry::HasCollision(below.Block()) &&
            BlockRegistry::GetBlockCollisionShapeSet(below).count > 0) {
            return std::nullopt;
        }
        return down;
    }

    // ── TrialSpawnerStateData ───────────────────────────────────────────────

    void TrialSpawnerBlockEntity::Reset() {
        m_data.currentMobs.clear();
        m_data.nextSpawnData.reset();
        ResetStatistics();
    }

    void TrialSpawnerBlockEntity::ResetStatistics() {
        // No setChanged here, as in MC: a Peaceful spawner resets every tick
        // and must not dirty its chunk every tick; the state change that
        // follows a real reset saves it.
        m_data.detectedPlayers.clear();
        m_data.totalMobsSpawned = 0;
        m_data.nextMobSpawnsAt = 0;
        m_data.cooldownEndsAt = 0;
    }

    const SpawnData& TrialSpawnerBlockEntity::GetOrCreateNextSpawnData(JavaRandom& random) {
        if (m_data.nextSpawnData) return *m_data.nextSpawnData;
        // An empty potentials list draws nothing (and no random); the next
        // SpawnData becomes an empty one.
        const TrialSpawnerConfig& config = ActiveConfig();
        const int picked = config.PickSpawnPotential(random);
        m_data.nextSpawnData = picked >= 0 ? config.spawnPotentials[static_cast<size_t>(picked)].data : SpawnData{};
        MarkUpdated();
        return *m_data.nextSpawnData;
    }

    bool TrialSpawnerBlockEntity::HasMobToSpawn(JavaRandom& random) {
        const bool hasNextMobToSpawn = GetOrCreateNextSpawnData(random).hasType;
        return hasNextMobToSpawn || !ActiveConfig().spawnPotentials.empty();
    }

    int TrialSpawnerBlockEntity::CountAdditionalPlayers() const {
        return std::max(0, static_cast<int>(m_data.detectedPlayers.size()) - 1);
    }

    std::vector<Uuid> TrialSpawnerBlockEntity::DetectPlayers(World& world, EntityLevel& level,
                                                             bool requireLineOfSight) const {
        // PlayerDetector.NO_CREATIVE_PLAYERS: block position closer than the
        // range (Vec3i.closerThan — strictly inside), not creative, not a
        // spectator; with line of sight from the spawner's centre to the eyes
        // when required.
        const glm::ivec3 pos = GetWorldPos();
        const double range = static_cast<double>(m_config.requiredPlayerRange);
        std::vector<LivingEntity*> players;
        level.GetPlayers(players);
        std::vector<Uuid> out;
        for (LivingEntity* p : players) {
            if (!p) continue;
            if (!(DistSqr(p->BlockPosition(), pos) < range * range)) continue;
            if (p->IsCreative() || p->IsSpectator()) continue;
            if (requireLineOfSight && !InLineOfSight(world, CenterOf(pos), p->GetEyePosition())) continue;
            out.push_back(p->GetUuid());
        }
        return out;
    }

    LivingEntity* TrialSpawnerBlockEntity::PlayerByUuid(EntityLevel& level, const Uuid& uuid) const {
        // MC ServerLevel.getPlayerByUUID — this level's players only.
        std::vector<LivingEntity*> players;
        level.GetPlayers(players);
        for (LivingEntity* p : players) {
            if (p && p->GetUuid() == uuid) return p;
        }
        return nullptr;
    }

    Entity* TrialSpawnerBlockEntity::EntityByUuid(EntityLevel& level, const Uuid& uuid) const {
        // MC ServerLevel.getEntity(uuid) — this level's entities only; the
        // resolver searches every level, so one found elsewhere is "not here"
        // (shouldMobBeUntracked's dimension test).
        Entity* entity = level.ResolveEntity(uuid);
        if (!entity || entity->Level() != &level) return nullptr;
        return entity;
    }

    void TrialSpawnerBlockEntity::TryDetectPlayers(World& world, EntityLevel& level) {
        const glm::ivec3 pos = GetWorldPos();
        // Throttled to one scan in 20 ticks, staggered by position.
        const bool throttled = (BlockPosAsLong(pos) + world.GetGameTime()) % 20 != 0;
        if (throttled) return;

        const TrialSpawnerState state =
            TrialSpawnerStates::Of(world.GetBlockState(pos.x, pos.y, pos.z));
        if (state == TrialSpawnerState::Cooldown && m_isOminous) return;

        const std::vector<Uuid> inLineOfSightPlayers = DetectPlayers(world, level, true);
        bool becameOminous = false;
        if (!m_isOminous && !inLineOfSightPlayers.empty()) {
            // findPlayerWithOminousEffect: the first with TRIAL_OMEN wins;
            // otherwise the last one found with BAD_OMEN.
            LivingEntity* withTrialOmen = nullptr;
            LivingEntity* withBadOmen = nullptr;
            for (const Uuid& id : inLineOfSightPlayers) {
                LivingEntity* player = PlayerByUuid(level, id);
                if (!player) continue;
                if (player->HasEffect(MobEffectId::TrialOmen)) {
                    withTrialOmen = player;
                    break;
                }
                if (player->HasEffect(MobEffectId::BadOmen)) withBadOmen = player;
            }
            LivingEntity* omenPlayer = withTrialOmen ? withTrialOmen : withBadOmen;
            if (omenPlayer) {
                if (!withTrialOmen) {
                    // transformBadOmenIntoTrialOmen: 15 minutes of Trial Omen
                    // per Bad Omen level, the Bad Omen spent.
                    if (const MobEffectInstance* badOmen = omenPlayer->GetEffect(MobEffectId::BadOmen)) {
                        const int amplifier = badOmen->amplifier + 1;
                        const int duration = kTrialOmenPerBadOmenLevel * amplifier;
                        omenPlayer->RemoveEffect(MobEffectId::BadOmen);
                        omenPlayer->AddEffect(MobEffectInstance(MobEffectId::TrialOmen, duration, 0));
                    }
                }
                world.PlayLevelEvent(nullptr, LevelEvent::PARTICLES_TRIAL_SPAWNER_BECOME_OMINOUS,
                                     Containing(omenPlayer->GetEyePosition()), 0);
                ApplyOminous(world, level);
                becameOminous = true;
            }
        }

        if (state == TrialSpawnerState::Cooldown && !becameOminous) return;

        // The first player needs the line of sight; once one is in, anyone in
        // range joins.
        const bool isSearchingForFirstPlayer = m_data.detectedPlayers.empty();
        const std::vector<Uuid> foundPlayers =
            isSearchingForFirstPlayer ? inLineOfSightPlayers : DetectPlayers(world, level, false);
        bool added = false;
        for (const Uuid& id : foundPlayers) {
            if (std::find(m_data.detectedPlayers.begin(), m_data.detectedPlayers.end(), id) ==
                m_data.detectedPlayers.end()) {
                m_data.detectedPlayers.push_back(id);
                added = true;
            }
        }
        if (!added) return;
        m_data.nextMobSpawnsAt = std::max(world.GetGameTime() + kDetectPlayerSpawnBuffer, m_data.nextMobSpawnsAt);
        MarkDirty();
        if (!becameOminous) {
            const int event = m_isOminous ? LevelEvent::PARTICLES_TRIAL_SPAWNER_DETECT_PLAYER_OMINOUS
                                          : LevelEvent::PARTICLES_TRIAL_SPAWNER_DETECT_PLAYER;
            world.PlayLevelEvent(nullptr, event, pos, static_cast<int>(m_data.detectedPlayers.size()));
        }
    }

    void TrialSpawnerBlockEntity::ResetAfterBecomingOminous(World& world, EntityLevel& level) {
        // Every tracked mob goes: its spawn burst, its preserved equipment
        // dropped, then discarded.
        for (const Uuid& id : m_data.currentMobs) {
            Entity* entity = EntityByUuid(level, id);
            if (!entity) continue;
            world.PlayLevelEvent(nullptr, LevelEvent::PARTICLES_TRIAL_SPAWNER_SPAWN_MOB_AT,
                                 entity->BlockPosition(), kFlameNormal);
            if (auto* mob = dynamic_cast<Mob*>(entity)) MobEquipment::DropPreservedEquipment(*mob);
            entity->Remove(RemovalReason::Discarded);
        }
        const TrialSpawnerConfig& ominous = OminousConfig();
        if (!ominous.spawnPotentials.empty()) m_data.nextSpawnData.reset();
        m_data.totalMobsSpawned = 0;
        m_data.currentMobs.clear();
        m_data.nextMobSpawnsAt = world.GetGameTime() + ominous.ticksBetweenSpawn;
        MarkUpdated();
        m_data.cooldownEndsAt = world.GetGameTime() + TrialSpawnerConfig::kTicksBetweenItemSpawners;
    }

    const std::vector<std::pair<ItemStack, int>>& TrialSpawnerBlockEntity::GetDispensingItems(World& world) {
        if (m_hasDispensing) return m_dispensing;
        // The ominous item table rolled with a seed fixed per 30x20x30
        // region of the level (lowResolutionPosition): every spawner in one
        // part of a chamber hands out the same kinds of items. Each drop is
        // an entry weighted by its count.
        const glm::ivec3 pos = GetWorldPos();
        const glm::ivec3 lowRes(static_cast<int>(std::floor(static_cast<float>(pos.x) / 30.0f)),
                                static_cast<int>(std::floor(static_cast<float>(pos.y) / 20.0f)),
                                static_cast<int>(std::floor(static_cast<float>(pos.z) / 30.0f)));
        const int64_t seed = world.GetGenerationSeed() + BlockPosAsLong(lowRes);
        JavaRandom seeded(seed);
        std::vector<ItemStack> drops;
        ChestLoot::GetRandomItems(ActiveConfig().itemsToDropWhenOminous, seeded, 0.0f, drops);
        m_dispensing.clear();
        if (drops.empty()) return m_dispensing;   // WeightedList.of(), not cached
        for (const ItemStack& drop : drops) {
            ItemStack one = drop;
            one.count = 1;
            m_dispensing.emplace_back(std::move(one), drop.count);
        }
        m_hasDispensing = true;
        return m_dispensing;
    }

    // ── TrialSpawner.tickClient ─────────────────────────────────────────────

    void TrialSpawnerBlockEntity::ClientTick(ILevelWrite& level) {
        const glm::ivec3 pos = GetWorldPos();
        const BlockState blockState = level.GetBlockState(pos.x, pos.y, pos.z);
        if (blockState.Block() != BlockID::TrialSpawner) return;
        const TrialSpawnerState state = TrialSpawnerStates::Of(blockState);
        const bool ominous = TrialSpawnerStates::OminousOf(blockState);
        m_clientState = state;

        JavaRandom* random = level.Random();
        if (random) {
            // TrialSpawnerState.emitParticles — the state's ParticleEmission.
            const glm::dvec3 center = CenterOf(pos);
            switch (state) {
                case TrialSpawnerState::WaitingForPlayers:
                case TrialSpawnerState::WaitingForRewardEjection:
                case TrialSpawnerState::EjectingReward:
                    // SMALL_FLAMES.
                    if (random->NextInt(2) == 0) {
                        const glm::dvec3 v = OffsetRandom(center, *random, 0.9f);
                        level.AddParticle(ominous ? ParticleKind::SoulFireFlame : ParticleKind::SmallFlame,
                                          v.x, v.y, v.z, 0.0, 0.0, 0.0);
                    }
                    break;
                case TrialSpawnerState::Active: {
                    // FLAMES_AND_SMOKE.
                    const glm::dvec3 v = OffsetRandom(center, *random, 1.0f);
                    level.AddParticle(ParticleKind::Smoke, v.x, v.y, v.z, 0.0, 0.0, 0.0);
                    level.AddParticle(ominous ? ParticleKind::SoulFireFlame : ParticleKind::Flame,
                                      v.x, v.y, v.z, 0.0, 0.0, 0.0);
                    break;
                }
                case TrialSpawnerState::Cooldown: {
                    // SMOKE_INSIDE_AND_TOP_FACE.
                    const glm::dvec3 v = OffsetRandom(center, *random, 0.9f);
                    if (random->NextInt(3) == 0) {
                        level.AddParticle(ParticleKind::Smoke, v.x, v.y, v.z, 0.0, 0.0, 0.0);
                    }
                    if (level.GameTime() % 20 == 0) {
                        const glm::dvec3 top = center + glm::dvec3(0.0, 0.5, 0.0);
                        const int smokeCount = random->NextInt(4) + 20;
                        for (int i = 0; i < smokeCount; ++i) {
                            level.AddParticle(ParticleKind::Smoke, top.x, top.y, top.z, 0.0, 0.0, 0.0);
                        }
                    }
                    break;
                }
                case TrialSpawnerState::Inactive:
                    break;   // NONE
            }
        }

        if (m_clientTicksUntilSpawn > 0) --m_clientTicksUntilSpawn;
        if (TrialSpawnerStates::HasSpinningMob(state)) {
            // spawnDelay = max(0, nextMobSpawnsAt - gameTime): the server's
            // count while ACTIVE, 0 otherwise (its update tag omits the
            // clock outside ACTIVE, which reads back as 0).
            const double spawnDelay = static_cast<double>(state == TrialSpawnerState::Active
                                                              ? m_clientTicksUntilSpawn : 0);
            m_oSpin = m_spin;
            m_spin = std::fmod(m_spin + TrialSpawnerStates::SpinningMobSpeed(state) / (spawnDelay + 200.0), 360.0);
        }

        if (TrialSpawnerStates::IsCapableOfSpawning(state) && random) {
            if (random->NextFloat() <= kSpawningAmbientSoundChance) {
                const float volume = random->NextFloat() * 0.25f + 0.75f;
                const float pitch = random->NextFloat() + 0.5f;
                level.PlayLocalSound(pos, ominous ? SoundEvents::TRIAL_SPAWNER_AMBIENT_OMINOUS
                                                  : SoundEvents::TRIAL_SPAWNER_AMBIENT,
                                     SoundSource::Blocks, volume, pitch, false);
            }
        }
    }

    void TrialSpawnerBlockEntity::CarryClientState(const BlockEntity& previous) {
        if (const auto* old = dynamic_cast<const TrialSpawnerBlockEntity*>(&previous)) {
            m_spin = old->m_spin;
            m_oSpin = old->m_oSpin;
            m_clientState = old->m_clientState;
        }
    }

    // ── Wire (MC getUpdateTag) ──────────────────────────────────────────────

    void TrialSpawnerBlockEntity::Save(Network::PacketBuffer& out) const {
        // next_mob_spawns_at while ACTIVE, as ticks from now.
        int ticksUntilSpawn = 0;
        if (ILevelWrite* level = GetLevel()) {
            const glm::ivec3 pos = GetWorldPos();
            if (TrialSpawnerStates::Of(level->GetBlockState(pos.x, pos.y, pos.z)) == TrialSpawnerState::Active) {
                ticksUntilSpawn = static_cast<int>(std::clamp<int64_t>(
                    m_data.nextMobSpawnsAt - level->GameTime(), 0, std::numeric_limits<int32_t>::max()));
            }
        }
        out.WriteVarInt(static_cast<uint32_t>(ticksUntilSpawn));
        // spawn_data: the display entity's type and baby flag.
        const bool display = m_data.nextSpawnData && m_data.nextSpawnData->hasType;
        out.WriteByte(display ? 1 : 0);
        if (display) {
            out.WriteShort(static_cast<uint16_t>(m_data.nextSpawnData->type));
            out.WriteByte(m_data.nextSpawnData->baby ? 1 : 0);
        }
    }

    void TrialSpawnerBlockEntity::Load(Network::PacketReader& in) {
        m_clientTicksUntilSpawn = static_cast<int>(in.ReadVarInt());
        m_hasDisplay = in.ReadByte() != 0;
        if (m_hasDisplay) {
            m_displayType = static_cast<EntityTypeId>(in.ReadShort());
            m_displayBaby = in.ReadByte() != 0;
            if (static_cast<uint16_t>(m_displayType) >= static_cast<uint16_t>(EntityTypeId::Count)) {
                m_hasDisplay = false;
            }
        } else {
            m_displayBaby = false;
        }
    }

} // namespace Game
