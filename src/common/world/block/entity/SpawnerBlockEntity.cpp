// File: src/common/world/block/entity/SpawnerBlockEntity.cpp
//
// MC BaseSpawner.serverTick / clientTick / delay / load-save, over the
// SpawnerBlockEntity that owns it. See the header for the representation and
// the deviations.
#include "SpawnerBlockEntity.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/MobCategory.hpp"
#include "common/entity/SpawnReason.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/spawn/SpawnPlacements.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Game {

    namespace {
        SpawnerServerHooks g_hooks;

        // A bare {id:"minecraft:<slug>"} compound — MC `new SpawnData()`
        // followed by putString("id", ...), for when the server has not
        // installed its NBT rewrite (a client-only build never reprograms).
        std::vector<uint8_t> BareIdCompound(EntityTypeId type) {
            Nbt::Writer w;
            w.BeginRootCompound();
            w.String("id", "minecraft:" + std::string(GetEntityTypeInfo(type).slug));
            w.EndRootCompound();
            return w.ok() ? w.TakeBytes() : std::vector<uint8_t>{};
        }
    } // namespace

    void SetSpawnerServerHooks(const SpawnerServerHooks& hooks) { g_hooks = hooks; }

    // ── MC BaseSpawner.serverTick ─────────────────────────────────────────

    void SpawnerBlockEntity::Tick(World* world, float /*deltaTime*/) {
        if (!world) return;
        EntityLevel* level = world->Entities();
        if (!level) return;
        const glm::ivec3 pos = GetWorldPos();

        // isNearPlayer (hasNearbyAlivePlayer: a living, non-spectator player
        // strictly inside RequiredPlayerRange of the block centre) and
        // level.isSpawnerBlockEnabled (the spawner_blocks_work rule).
        if (!level->GetNearestPlayer(pos.x + 0.5, pos.y + 0.5, pos.z + 0.5,
                                     static_cast<double>(m_requiredPlayerRange))) {
            return;
        }
        if (!Rules::GetBool(Rules::Id::SpawnerBlocksWork)) return;

        JavaRandom& random = level->Random();
        if (m_spawnDelay == -1) Delay(*world, random);
        if (m_spawnDelay > 0) {
            --m_spawnDelay;
            return;
        }

        bool delay = false;
        // A copy: delay() below may swap nextSpawnData for another potential
        // while the loop still reads this cycle's.
        const SpawnData next = GetOrCreateNextSpawnData(random);

        for (int c = 0; c < m_spawnCount; ++c) {
            // EntityType.by(input): no (known) id — delay and stop.
            if (!next.hasType) {
                Delay(*world, random);
                return;
            }
            const EntityTypeId type = next.type;
            const EntityTypeInfo& info = GetEntityTypeInfo(type);

            // input.read("Pos") or a scatter around the block: x and z by
            // (nextDouble - nextDouble) * SpawnRange + 0.5, y by -1..+1 —
            // evaluated in Vec3's argument order.
            glm::dvec3 spawnPos;
            if (next.hasPos) {
                spawnPos = next.pos;
            } else {
                const double x = pos.x + (random.NextDouble() - random.NextDouble())
                                             * static_cast<double>(m_spawnRange) + 0.5;
                const double y = static_cast<double>(pos.y + random.NextInt(3) - 1);
                const double z = pos.z + (random.NextDouble() - random.NextDouble())
                                             * static_cast<double>(m_spawnRange) + 0.5;
                spawnPos = glm::dvec3(x, y, z);
            }

            // level.noCollision(type.getSpawnAABB(x, y, z)) — the type's box
            // times its spawnDimensionsScale (slime / magma cube 4, sulfur
            // cube 2), halfWidth and height in float as MC computes them.
            {
                const float scale = GetSpawnDimensionsScale(type);
                const float halfWidth = scale * info.width / 2.0f;
                const float height = scale * info.height;
                AABBd box;
                box.min = glm::dvec3(spawnPos.x - halfWidth, spawnPos.y, spawnPos.z - halfWidth);
                box.max = glm::dvec3(spawnPos.x + halfWidth, spawnPos.y + height, spawnPos.z + halfWidth);
                PhysicsContext phys;
                phys.blockAccess = world;
                if (CollidesAt(box, phys)) continue;
            }

            const glm::ivec3 spawnBlockPos(static_cast<int>(std::floor(spawnPos.x)),
                                           static_cast<int>(std::floor(spawnPos.y)),
                                           static_cast<int>(std::floor(spawnPos.z)));
            if (next.hasCustomRules) {
                // Custom rules skip the type's placement predicate, but a
                // hostile still never spawns on Peaceful.
                if (!GetMobCategoryInfo(info.category).isFriendly &&
                    world->GetDifficulty() == Difficulty::Peaceful) {
                    continue;
                }
                // CustomSpawnRules.isValidPosition: getBrightness(BLOCK) and
                // getEffectiveSkyBrightness (raw sky minus the darkening,
                // floored at 0).
                const int blockLight = level->GetBlockBrightness(spawnBlockPos.x, spawnBlockPos.y,
                                                                 spawnBlockPos.z);
                const int skyLight = std::max(0, level->GetSkyBrightness(spawnBlockPos.x, spawnBlockPos.y,
                                                                         spawnBlockPos.z)
                                                     - level->GetSkyDarken());
                if (blockLight < next.blockLightMin || blockLight > next.blockLightMax ||
                    skyLight < next.skyLightMin || skyLight > next.skyLightMax) {
                    continue;
                }
            } else if (g_hooks.checkSpawnRules &&
                       !g_hooks.checkSpawnRules(type, *world, spawnBlockPos, level->Random())) {
                continue;
            }

            // EntityType.loadEntityRecursive(..., snapTo(spawnPos, yRot, xRot)).
            std::unique_ptr<Mob> mob = g_hooks.loadEntity ? g_hooks.loadEntity(next, *level) : nullptr;
            if (!mob) {
                Delay(*world, random);
                return;
            }
            mob->position = spawnPos;
            mob->oldPosition = spawnPos;

            // The same-class census: exact type, in the block's cell inflated
            // by SpawnRange, spectators excluded (players are never this type).
            {
                const glm::vec3 lo(pos.x - m_spawnRange, pos.y - m_spawnRange, pos.z - m_spawnRange);
                const glm::vec3 hi(pos.x + 1 + m_spawnRange, pos.y + 1 + m_spawnRange,
                                   pos.z + 1 + m_spawnRange);
                std::vector<Entity*> nearby;
                level->GetEntitiesInBox(AABB::FromMinMax(lo, hi), nullptr, nearby);
                int nearBy = 0;
                for (const Entity* e : nearby) {
                    if (e && !e->IsRemoved() && e->GetType() == type) ++nearBy;
                }
                if (nearBy >= m_maxNearbyEntities) {
                    Delay(*world, random);
                    return;
                }
            }

            // snapTo(x, y, z, nextFloat() * 360, 0).
            mob->yRot = random.NextFloat() * 360.0f;
            mob->xRot = 0.0f;
            mob->yRotO = mob->yRot;
            mob->xRotO = mob->xRot;
            mob->yHeadRot = mob->yHeadRotO = mob->yRot;
            mob->yBodyRot = mob->yBodyRotO = mob->yRot;

            if (!next.hasCustomRules && !mob->CheckSpawnRules(*level, SpawnReason::Spawner)) continue;
            if (!mob->CheckSpawnObstruction(*level)) continue;
            // Only a bare {id} is finalized: a configured compound already
            // says what the mob is. (SpawnData.equipment: see the header.)
            if (next.bareId) mob->FinalizeSpawn(SpawnReason::Spawner, nullptr);

            // tryAddFreshEntityWithPassengers (a refusal delays and stops),
            // then LevelEvent 2004 (engine block event, header) and
            // Mob.spawnAnim (entity event 20: the client's poof).
            Mob* placed = mob.get();
            if (g_hooks.addFreshEntity) {
                if (!g_hooks.addFreshEntity(mob, *world)) {
                    Delay(*world, random);
                    return;
                }
            } else {
                level->AddFreshEntity(std::move(mob));
            }
            world->BlockEvent(pos, GetBlockId(), kEventSpawnParticles, 0);
            level->BroadcastEntityEvent(*placed, 20);
            delay = true;
        }

        if (delay) Delay(*world, random);
    }

    // MC BaseSpawner.delay.
    void SpawnerBlockEntity::Delay(ILevelWrite& level, JavaRandom& random) {
        if (m_maxSpawnDelay <= m_minSpawnDelay) {
            m_spawnDelay = m_minSpawnDelay;
        } else {
            m_spawnDelay = m_minSpawnDelay + random.NextInt(m_maxSpawnDelay - m_minSpawnDelay);
        }
        // spawnPotentials.getRandom(random).ifPresent(setNextSpawnData).
        if (const int picked = PickPotential(random); picked >= 0) {
            SetNextSpawnDataAndSync(m_spawnPotentials[static_cast<size_t>(picked)].data);
            m_nextAliasesPotential = picked;
        }
        MarkDirty();
        // broadcastEvent(level, pos, 1): level.blockEvent(pos, SPAWNER, 1, 0).
        level.BlockEvent(GetWorldPos(), GetBlockId(), kEventSpawn, 0);
    }

    // MC WeightedList.getRandom: empty (no draw) when the total weight is 0,
    // else one nextInt(total) walked through the cumulative weights.
    int SpawnerBlockEntity::PickPotential(JavaRandom& random) const {
        int totalWeight = 0;
        for (const WeightedSpawnData& w : m_spawnPotentials) totalWeight += w.weight;
        if (totalWeight <= 0) return -1;
        int roll = random.NextInt(totalWeight);
        for (size_t i = 0; i < m_spawnPotentials.size(); ++i) {
            roll -= m_spawnPotentials[i].weight;
            if (roll < 0) return static_cast<int>(i);
        }
        return -1;
    }

    // MC getOrCreateNextSpawnData: a spawner with no next SpawnData draws one
    // from the potentials, or takes an empty one.
    const SpawnData& SpawnerBlockEntity::GetOrCreateNextSpawnData(JavaRandom& random) {
        if (m_hasNextSpawnData) return m_nextSpawnData;
        if (const int picked = PickPotential(random); picked >= 0) {
            SetNextSpawnDataAndSync(m_spawnPotentials[static_cast<size_t>(picked)].data);
            m_nextAliasesPotential = picked;
        } else {
            m_nextAliasesPotential = -1;
            SetNextSpawnDataAndSync(SpawnData{});
        }
        return m_nextSpawnData;
    }

    // MC SpawnerBlockEntity's setNextSpawnData override: the new display
    // entity goes to the clients (sendBlockUpdated -> the update packet).
    void SpawnerBlockEntity::SetNextSpawnDataAndSync(const SpawnData& data) {
        m_nextSpawnData = data;
        m_hasNextSpawnData = true;
        MarkDirty();
        if (ILevelWrite* level = GetLevel(); level && !level->IsClientSide()) {
            level->BlockEntityChanged(GetWorldPos());
        }
    }

    void SpawnerBlockEntity::SetSpawnPotentials(std::vector<WeightedSpawnData> potentials,
                                                bool present) {
        if (present) {
            m_spawnPotentials = std::move(potentials);
            m_nextAliasesPotential = -1;
            return;
        }
        // MC load: absent SpawnPotentials default to WeightedList.of(
        // nextSpawnData != null ? nextSpawnData : new SpawnData()) — the one
        // entry IS the next SpawnData (weight 1).
        m_spawnPotentials.clear();
        m_spawnPotentials.push_back({ m_hasNextSpawnData ? m_nextSpawnData : SpawnData{}, 1 });
        m_nextAliasesPotential = m_hasNextSpawnData ? 0 : -1;
    }

    // MC SpawnerBlockEntity.setEntityId -> BaseSpawner.setEntityId.
    void SpawnerBlockEntity::SetEntityId(EntityTypeId type, JavaRandom& random) {
        GetOrCreateNextSpawnData(random);
        if (g_hooks.setEntityId) {
            g_hooks.setEntityId(m_nextSpawnData, type);
        } else {
            m_nextSpawnData.entityNbt = BareIdCompound(type);
            m_nextSpawnData.hasType = true;
            m_nextSpawnData.type = type;
            m_nextSpawnData.bareId = true;
            m_nextSpawnData.hasPos = false;
            m_nextSpawnData.baby = false;
        }
        // The potential nextSpawnData is (in Java, by reference) changes too.
        if (m_nextAliasesPotential >= 0 &&
            m_nextAliasesPotential < static_cast<int>(m_spawnPotentials.size())) {
            m_spawnPotentials[static_cast<size_t>(m_nextAliasesPotential)].data = m_nextSpawnData;
        }
        MarkDirty();
    }

    // ── MC BaseSpawner.clientTick ─────────────────────────────────────────

    void SpawnerBlockEntity::ClientTick(ILevelWrite& level) {
        const glm::ivec3 pos = GetWorldPos();
        // isNearPlayer — on the client, the one player there is.
        bool nearPlayer = false;
        glm::dvec3 lo, hi;
        if (level.GetLocalPlayerBox(lo, hi)) {
            const double px = (lo.x + hi.x) * 0.5, py = lo.y, pz = (lo.z + hi.z) * 0.5;
            const double dx = px - (pos.x + 0.5), dy = py - (pos.y + 0.5), dz = pz - (pos.z + 0.5);
            const double range = static_cast<double>(m_requiredPlayerRange);
            nearPlayer = dx * dx + dy * dy + dz * dz < range * range;
        }
        if (!nearPlayer) {
            m_oSpin = m_spin;
            return;
        }
        // displayEntity != null: the next SpawnData names a mob.
        if (!m_hasNextSpawnData || !m_nextSpawnData.hasType) return;

        if (JavaRandom* random = level.Random()) {
            const double x = pos.x + random->NextDouble();
            const double y = pos.y + random->NextDouble();
            const double z = pos.z + random->NextDouble();
            level.AddParticle(ParticleKind::Smoke, x, y, z, 0.0, 0.0, 0.0);
            level.AddParticle(ParticleKind::Flame, x, y, z, 0.0, 0.0, 0.0);
        }
        if (m_spawnDelay > 0) --m_spawnDelay;
        m_oSpin = m_spin;
        m_spin = std::fmod(m_spin + static_cast<double>(1000.0f / (static_cast<float>(m_spawnDelay) + 200.0f)),
                           360.0);
    }

    // MC BaseSpawner.onEventTriggered: event 1 resets the client's delay.
    bool SpawnerBlockEntity::TriggerEvent(int b0, int /*b1*/) {
        if (b0 == kEventSpawn) {
            if (ILevelWrite* level = GetLevel(); !level || level->IsClientSide()) {
                m_spawnDelay = m_minSpawnDelay;
            }
            return true;
        }
        return false;
    }

    void SpawnerBlockEntity::CarryClientState(const BlockEntity& previous) {
        if (const auto* old = dynamic_cast<const SpawnerBlockEntity*>(&previous)) {
            m_spin = old->m_spin;
            m_oSpin = old->m_oSpin;
        }
    }

    // ── Wire (MC getUpdateTag: saveCustomOnly minus SpawnPotentials) ──────

    void SpawnerBlockEntity::Save(Network::PacketBuffer& out) const {
        out.WriteInt(static_cast<uint32_t>(m_spawnDelay));
        out.WriteInt(static_cast<uint32_t>(m_minSpawnDelay));
        out.WriteInt(static_cast<uint32_t>(m_maxSpawnDelay));
        out.WriteInt(static_cast<uint32_t>(m_spawnCount));
        out.WriteInt(static_cast<uint32_t>(m_maxNearbyEntities));
        out.WriteInt(static_cast<uint32_t>(m_requiredPlayerRange));
        out.WriteInt(static_cast<uint32_t>(m_spawnRange));
        const bool display = m_hasNextSpawnData && m_nextSpawnData.hasType;
        out.WriteByte(display ? 1 : 0);
        if (display) {
            out.WriteShort(static_cast<uint16_t>(m_nextSpawnData.type));
            out.WriteByte(m_nextSpawnData.baby ? 1 : 0);
        }
    }

    void SpawnerBlockEntity::Load(Network::PacketReader& in) {
        m_spawnDelay = static_cast<int32_t>(in.ReadInt());
        m_minSpawnDelay = static_cast<int32_t>(in.ReadInt());
        m_maxSpawnDelay = static_cast<int32_t>(in.ReadInt());
        m_spawnCount = static_cast<int32_t>(in.ReadInt());
        m_maxNearbyEntities = static_cast<int32_t>(in.ReadInt());
        m_requiredPlayerRange = static_cast<int32_t>(in.ReadInt());
        m_spawnRange = static_cast<int32_t>(in.ReadInt());
        m_nextSpawnData = SpawnData{};
        m_hasNextSpawnData = true;
        if (in.ReadByte() != 0) {
            m_nextSpawnData.hasType = true;
            m_nextSpawnData.type = static_cast<EntityTypeId>(in.ReadShort());
            m_nextSpawnData.baby = in.ReadByte() != 0;
        }
    }

} // namespace Game
