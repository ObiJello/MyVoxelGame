// File: src/server/entity/ItemEntityManager.cpp
#include "ItemEntityManager.hpp"

#include "common/world/level/World.hpp"
#include "common/core/Log.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Server {

    namespace {
        constexpr double kPi = 3.14159265358979323846;

        // MC Mth.nextDouble(random, min, max).
        double NextInRange(Game::JavaRandom& rng, double min, double max) {
            return min >= max ? min : rng.NextDouble() * (max - min) + min;
        }

        Game::Math::ChunkPos ChunkOf(const glm::dvec3& pos) {
            return Game::Math::ChunkPos{
                static_cast<int32_t>(std::floor(pos.x / 16.0)),
                static_cast<int32_t>(std::floor(pos.z / 16.0))
            };
        }
    } // namespace

    // ── Spawning ───────────────────────────────────────────────────────────

    int32_t ItemEntityManager::Spawn(const glm::dvec3& pos, const glm::dvec3& vel,
                                     const Game::ItemStack& stack, int pickupDelay) {
        if (stack.IsEmpty()) return 0;

        // Seed the RNG lazily off the clock. Item scatter is cosmetic — it is
        // deliberately NOT part of the deterministic loot roll, which has its
        // own seeded stream in PlayerSession.
        if (m_nextId == kItemEntityIdBase) {
            m_random.SetSeed(static_cast<int64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        }

        Game::ItemEntity e;
        e.id          = m_nextId++;
        e.stack       = stack;
        e.pos         = pos;
        e.vel         = vel;
        e.pickupDelay = pickupDelay;
        e.bobOffs     = m_random.NextFloat() * 2.0f * static_cast<float>(kPi);
        e.needsSync   = true;

        const int32_t id = e.id;
        m_entities.emplace(id, std::move(e));
        return id;
    }

    void ItemEntityManager::PopResource(const glm::ivec3& blockPos,
                                        const Game::ItemStack& stack) {
        if (stack.IsEmpty()) return;

        // MC Block.popResource: scatter within ±0.25 of the block centre, then
        // sink by half the entity height so the 0.25-tall box straddles the
        // sampled point rather than sitting on top of it.
        const double halfHeight = Game::ItemEntity::kHeight / 2.0;
        const glm::dvec3 pos{
            blockPos.x + 0.5 + NextInRange(m_random, -0.25, 0.25),
            blockPos.y + 0.5 + NextInRange(m_random, -0.25, 0.25) - halfHeight,
            blockPos.z + 0.5 + NextInRange(m_random, -0.25, 0.25)
        };

        // Velocity from MC's ItemEntity 5-arg constructor: a small horizontal
        // drift plus a fixed upward hop, which is what makes broken blocks pop
        // rather than dribble.
        const glm::dvec3 vel{
            m_random.NextDouble() * 0.2 - 0.1,
            0.2,
            m_random.NextDouble() * 0.2 - 0.1
        };

        Spawn(pos, vel, stack, Game::ItemEntity::kDefaultPickupDelay);
    }

    void ItemEntityManager::PopResourceFromFace(const glm::ivec3& blockPos,
                                                Game::Direction face,
                                                const Game::ItemStack& stack) {
        if (stack.IsEmpty()) return;

        // MC Block.popResourceFromFace. On the axis the face points along, the
        // item is placed just OUTSIDE the block and launched away from it; the
        // other two axes keep the ordinary ±0.25 scatter.
        const int stepX = Game::StepX(face);
        const int stepY = Game::StepY(face);
        const int stepZ = Game::StepZ(face);

        const double halfWidth  = Game::ItemEntity::kWidth  / 2.0;
        const double halfHeight = Game::ItemEntity::kHeight / 2.0;

        const glm::dvec3 pos{
            blockPos.x + 0.5 + (stepX == 0 ? NextInRange(m_random, -0.25, 0.25)
                                           : stepX * (0.5 + halfWidth)),
            blockPos.y + 0.5 + (stepY == 0 ? NextInRange(m_random, -0.25, 0.25)
                                           : stepY * (0.5 + halfHeight)) - halfHeight,
            blockPos.z + 0.5 + (stepZ == 0 ? NextInRange(m_random, -0.25, 0.25)
                                           : stepZ * (0.5 + halfWidth))
        };
        const glm::dvec3 vel{
            stepX == 0 ? NextInRange(m_random, -0.1, 0.1) : stepX * 0.1,
            stepY == 0 ? NextInRange(m_random,  0.0, 0.1) : stepY * 0.1 + 0.1,
            stepZ == 0 ? NextInRange(m_random, -0.1, 0.1) : stepZ * 0.1
        };

        Spawn(pos, vel, stack, Game::ItemEntity::kDefaultPickupDelay);
    }

    int32_t ItemEntityManager::DropFromPlayer(const glm::dvec3& eyePos,
                                              const glm::dvec3& forward,
                                              const Game::ItemStack& stack) {
        if (stack.IsEmpty()) return 0;

        // MC LivingEntity.createItemStackToDrop, randomly=false. Spawns from
        // just below eye level so the item appears to leave the hand.
        const glm::dvec3 pos{ eyePos.x, eyePos.y - 0.3, eyePos.z };

        // MC's expression (-sinY*cosX, -sinX, cosY*cosX) * 0.3 IS its forward
        // vector scaled by 0.3 — written out in MC's own angle convention.
        // Expressed as a vector it ports cleanly to any convention.
        const double len = glm::length(forward);
        const glm::dvec3 fwd = (len > 1e-9) ? forward / len : glm::dvec3(0.0, 0.0, 1.0);

        // A random direction+magnitude added on top of the 0.3 forward throw,
        // so repeatedly dropping a stack doesn't pile every item on one point.
        const float spreadDir = m_random.NextFloat() * 2.0f * static_cast<float>(kPi);
        const float spreadMag = 0.02f * m_random.NextFloat();

        const glm::dvec3 vel{
            fwd.x * 0.3 + std::cos(spreadDir) * spreadMag,
            // The +0.1 lift plus a symmetric ±0.1 jitter is what gives a
            // thrown stack its little arc instead of a flat line.
            fwd.y * 0.3 + 0.1 + (m_random.NextFloat() - m_random.NextFloat()) * 0.1,
            fwd.z * 0.3 + std::sin(spreadDir) * spreadMag
        };

        return Spawn(pos, vel, stack, Game::ItemEntity::kThrowPickupDelay);
    }

    int32_t ItemEntityManager::DropScattered(const glm::dvec3& pos,
                                             const Game::ItemStack& stack) {
        if (stack.IsEmpty()) return 0;

        // MC LivingEntity.createItemStackToDrop, randomly=true.
        const float power = m_random.NextFloat() * 0.5f;
        const float dir   = m_random.NextFloat() * 2.0f * static_cast<float>(kPi);
        const glm::dvec3 vel{ -std::sin(dir) * power, 0.2, std::cos(dir) * power };

        return Spawn(pos, vel, stack, Game::ItemEntity::kThrowPickupDelay);
    }

    // ── Merging ────────────────────────────────────────────────────────────

    int32_t ItemEntityManager::TryMergeWithNeighbours(Game::ItemEntity& entity) {
        if (!entity.IsMergable()) return 0;

        // MC inflates the search box by 0.5 horizontally and 0.0 vertically:
        // items on the same floor merge across a small gap, but one resting on
        // a slab never absorbs one on the floor below.
        const Game::AABB box = entity.GetAABB();
        const double minX = box.min.x - Game::ItemEntity::kMergeInflateXZ;
        const double maxX = box.max.x + Game::ItemEntity::kMergeInflateXZ;
        const double minY = box.min.y;
        const double maxY = box.max.y;
        const double minZ = box.min.z - Game::ItemEntity::kMergeInflateXZ;
        const double maxZ = box.max.z + Game::ItemEntity::kMergeInflateXZ;

        for (auto& [otherId, other] : m_entities) {
            if (otherId == entity.id) continue;
            if (!other.IsMergable()) continue;

            const Game::AABB ob = other.GetAABB();
            if (ob.max.x <= minX || ob.min.x >= maxX) continue;
            if (ob.max.y <= minY || ob.min.y >= maxY) continue;
            if (ob.max.z <= minZ || ob.min.z >= maxZ) continue;

            if (!Game::CanMergeItemEntities(entity.stack, other.stack)) continue;

            // MC pours the SMALLER stack into the larger so the surviving
            // entity is the one that was already more substantial.
            Game::ItemEntity* dst = &entity;
            Game::ItemEntity* src = &other;
            if (entity.stack.count < other.stack.count) std::swap(dst, src);

            dst->stack.count += src->stack.count;
            // The survivor inherits the stricter pickup delay and the younger
            // age, so merging can neither grant an early pickup nor reset the
            // despawn clock of an old item.
            dst->pickupDelay = std::max(dst->pickupDelay, src->pickupDelay);
            dst->age         = std::min(dst->age, src->age);
            dst->needsSync   = true;
            src->stack.Clear();

            return src->id;
        }
        return 0;
    }

    // ── Tick ───────────────────────────────────────────────────────────────

    void ItemEntityManager::Tick(Game::World* world, PlayerSessionManager* sessions,
                                 std::vector<int32_t>& outRemoved,
                                 std::vector<ItemPickupEvent>& outPickups) {
        if (!world || m_entities.empty()) return;

        Game::PhysicsContext ctx;
        ctx.blockAccess = world;

        // 1. Physics.
        for (auto& [id, e] : m_entities) {
            if (e.stack.IsEmpty()) continue;
            // Items in chunks that have unloaded under us have nothing to
            // collide against; freezing them beats letting them fall forever.
            const Game::Math::ChunkPos cp = ChunkOf(e.pos);
            if (!ctx.IsChunkLoaded(cp.x, cp.z)) continue;

            if (!e.Tick(ctx)) {
                e.stack.Clear();          // marks it for the sweep below
            }
        }

        // 2. Merge. Only entities due on their cadence scan, and a merged-away
        // entity is emptied rather than erased so we never mutate the map while
        // iterating it.
        for (auto& [id, e] : m_entities) {
            if (e.stack.IsEmpty()) continue;
            const int interval = (e.vel.x != 0.0 || e.vel.z != 0.0)
                               ? Game::ItemEntity::kMergeIntervalMoving
                               : Game::ItemEntity::kMergeIntervalIdle;
            if (e.tickCount % interval != 0) continue;
            TryMergeWithNeighbours(e);
        }

        // 3. Pickup.
        if (sessions) {
            for (auto& session : sessions->GetAllSessions()) {
                if (!session) continue;
                ServerPlayer* player = session->GetPlayer();
                if (!player || player->isDead()) continue;

                const glm::dvec3 ppos = player->getPosition();

                // MC Player.aiStep collects with getBoundingBox().inflate(1.0,
                // 0.5, 1.0) and calls playerTouch on everything that box
                // intersects. Note it is the INFLATED box that is tested, which
                // is why an item is collected from a short distance away rather
                // than only on contact. Built once per player, not per item.
                const float ph = Game::PlayerPhysics::HEIGHT_STANDING;
                const Game::AABB pickupBox(
                    glm::vec3(ppos.x, ppos.y + ph * 0.5f, ppos.z),
                    glm::vec3(Game::PlayerPhysics::WIDTH + 2.0f, ph + 1.0f,
                              Game::PlayerPhysics::WIDTH + 2.0f));

                for (auto& [id, e] : m_entities) {
                    if (e.stack.IsEmpty() || e.pickupDelay > 0) continue;
                    if (!e.GetAABB().Intersects(pickupBox)) continue;

                    // AddStack returns what did NOT fit. A partial pickup
                    // shrinks the entity and leaves it in the world — MC
                    // behaviour, and the reason a full inventory doesn't eat
                    // the whole stack.
                    const int leftover = player->getInventory().AddStack(e.stack);
                    if (leftover == e.stack.count) continue;   // nothing fit

                    // What actually went into the inventory. The client shrinks
                    // its own copy by this and flies exactly this many away.
                    const int taken = e.stack.count - leftover;
                    outPickups.push_back(ItemPickupEvent{
                        e.id, player->getPlayerId(), taken });

                    if (leftover > 0) {
                        e.stack.count = leftover;
                        e.needsSync   = true;
                    } else {
                        e.stack.Clear();
                        e.pickedUp = true;
                    }
                }
            }
        }

        // 4. Sweep everything that emptied out this tick. A pickup is retired
        //    on the client by its take packet, so it must not ALSO be
        //    broadcast as a removal — see ItemEntity::pickedUp.
        for (auto it = m_entities.begin(); it != m_entities.end(); ) {
            if (it->second.stack.IsEmpty()) {
                if (!it->second.pickedUp) {
                    outRemoved.push_back(it->first);
                }
                it = m_entities.erase(it);
            } else {
                ++it;
            }
        }
    }

    void ItemEntityManager::RemoveInChunk(Game::Math::ChunkPos chunk,
                                          std::vector<int32_t>& outRemoved) {
        for (auto it = m_entities.begin(); it != m_entities.end(); ) {
            if (ChunkOf(it->second.pos) == chunk) {
                outRemoved.push_back(it->first);
                it = m_entities.erase(it);
            } else {
                ++it;
            }
        }
    }

    void ItemEntityManager::Clear() {
        m_entities.clear();
        m_nextId = kItemEntityIdBase;
    }

    void ItemEntityManager::CollectSyncSets(int64_t serverTick,
                                            std::vector<int32_t>& outFullRefresh,
                                            std::vector<int32_t>& outMoveOnly) {
        for (auto& [id, e] : m_entities) {
            // Stagger the periodic refresh by id so a big pile doesn't put
            // every entity on the wire in the same tick.
            const bool periodic = ((serverTick + id) % kSyncIntervalTicks) == 0;

            if (e.pendingSpawn || periodic) {
                // Full packet: either the client has never seen this entity, or
                // it is this entity's turn on the self-healing rotation.
                outFullRefresh.push_back(id);
                e.pendingSpawn = false;
            } else if (e.needsSync) {
                outMoveOnly.push_back(id);
            }
            e.needsSync = false;
        }
    }

} // namespace Server
