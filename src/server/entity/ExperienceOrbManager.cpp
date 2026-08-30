// File: src/server/entity/ExperienceOrbManager.cpp
#include "ExperienceOrbManager.hpp"

#include "common/world/level/World.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Server {

    namespace {
        Game::Math::ChunkPos ChunkOf(const glm::dvec3& pos) {
            return Game::Math::ChunkPos{
                static_cast<int32_t>(std::floor(pos.x / 16.0)),
                static_cast<int32_t>(std::floor(pos.z / 16.0))
            };
        }

        // What the orb tick needs to know about each live player. Snapshotted
        // once per tick rather than chasing sessions per orb.
        struct PlayerView {
            uint32_t      id;
            glm::dvec3    pos;
            ServerPlayer* player;
            bool          eligible;   // alive and not a spectator
        };
    } // namespace

    // ── Award (MC ExperienceOrb.award) ─────────────────────────────────────

    void ExperienceOrbManager::Award(const glm::dvec3& pos, int amount) {
        if (amount <= 0) return;

        if (!m_seeded) {
            m_random.SetSeed(static_cast<int64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()));
            m_seeded = true;
        }

        while (amount > 0) {
            const int value = Game::ExperienceOrb::GetExperienceValue(amount);
            amount -= value;
            if (!TryMergeToExisting(pos, value)) {
                SpawnOrb(pos, value);
            }
        }
    }
    int32_t ExperienceOrbManager::Adopt(Game::ExperienceOrb orb) {
        if (orb.value <= 0) return 0;

        orb.id = m_nextId++;
        if (Game::UuidIsNil(orb.uuid)) orb.uuid = Game::RandomUuid();

        const int32_t id = orb.id;
        m_entities.emplace(id, std::move(orb));
        return id;
    }


    int32_t ExperienceOrbManager::SpawnOrb(const glm::dvec3& pos, int value) {
        Game::ExperienceOrb orb;
        orb.id    = m_nextId++;
        // Persistent identity, minted where the session handle is assigned —
        // the same lifecycle rule the mobs use.
        orb.uuid = Game::RandomUuid();
        orb.value = value;
        orb.pos   = pos;
        // The death-burst scatter — MC's positional constructor doubles the
        // item drop's spread and always throws upward:
        //   ((rand*0.2 - 0.1) * 2, rand*0.2 * 2, (rand*0.2 - 0.1) * 2)
        orb.vel = glm::dvec3(
            (m_random.NextDouble() * 0.2 - 0.1) * 2.0,
            m_random.NextDouble() * 0.2 * 2.0,
            (m_random.NextDouble() * 0.2 - 0.1) * 2.0);
        orb.needsSync = true;

        const int32_t id = orb.id;
        m_entities.emplace(id, orb);
        return id;
    }

    bool ExperienceOrbManager::TryMergeToExisting(const glm::dvec3& pos, int value) {
        // MC tryMergeToExisting: a 1×1×1 box around the award point, a random
        // group roll, and the first orb of the same value in that group gets
        // count++ and a fresh despawn clock. The group keying caps a grinder's
        // orb population at ORB_GROUPS_PER_AREA entities per value.
        const int group = m_random.NextInt(Game::ExperienceOrb::kOrbGroups);

        const double minX = pos.x - 0.5, maxX = pos.x + 0.5;
        const double minY = pos.y - 0.5, maxY = pos.y + 0.5;
        const double minZ = pos.z - 0.5, maxZ = pos.z + 0.5;

        for (auto& [id, orb] : m_entities) {
            if (orb.count <= 0 || orb.pickedUp) continue;
            if (orb.value != value) continue;
            if ((orb.id - group) % Game::ExperienceOrb::kOrbGroups != 0) continue;

            const Game::AABB box = orb.GetAABB();
            if (box.max.x <= minX || box.min.x >= maxX) continue;
            if (box.max.y <= minY || box.min.y >= maxY) continue;
            if (box.max.z <= minZ || box.min.z >= maxZ) continue;

            ++orb.count;
            orb.age = 0;
            return true;
        }
        return false;
    }

    // ── Tick ───────────────────────────────────────────────────────────────

    void ExperienceOrbManager::Tick(Game::World* world, PlayerSessionManager* sessions,
                                    std::vector<int32_t>& outRemoved,
                                    std::vector<XpOrbPickupEvent>& outPickups) {
        if (!world) return;

        // Player snapshot + takeXpDelay countdown (MC Player.aiStep's
        // `if (takeXpDelay > 0) --takeXpDelay`).
        std::vector<PlayerView> players;
        if (sessions) {
            for (auto& session : sessions->GetAllSessions()) {
                if (!session) continue;
                ServerPlayer* player = session->GetPlayer();
                if (!player) continue;
                const bool eligible = !player->isDead()
                    && player->getGameMode() != GameMode::SPECTATOR;
                players.push_back(PlayerView{ player->getPlayerId(),
                                              player->getPosition(),
                                              player, eligible });
                int& delay = m_takeDelay[player->getPlayerId()];
                if (delay > 0) --delay;
            }
        }
        // Prune delay entries for players that left.
        for (auto it = m_takeDelay.begin(); it != m_takeDelay.end();) {
            const uint32_t pid = it->first;
            const bool present = std::any_of(players.begin(), players.end(),
                [pid](const PlayerView& v) { return v.id == pid; });
            if (present) ++it;
            else it = m_takeDelay.erase(it);
        }

        if (m_entities.empty()) return;

        if (!m_seeded) {
            m_random.SetSeed(static_cast<int64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()));
            m_seeded = true;
        }

        Game::PhysicsContext ctx;
        ctx.blockAccess = world;

        constexpr double kFollowDistSq = Game::ExperienceOrb::kMaxFollowDist
                                       * Game::ExperienceOrb::kMaxFollowDist;

        // 1. Physics + the pull + ageing.
        for (auto& [id, orb] : m_entities) {
            if (orb.count <= 0) continue;
            const Game::Math::ChunkPos cp = ChunkOf(orb.pos);
            if (!ctx.IsChunkLoaded(cp.x, cp.z)) continue;

            // followNearbyPlayer's selection: keep the current player until
            // they become invalid (gone, spectator, dead, out of range), THEN
            // ask for the nearest — MC does not retarget to a closer player
            // while the current one is still in range.
            const PlayerView* following = nullptr;
            if (orb.followingPlayerId >= 0) {
                for (const auto& v : players) {
                    if (v.id == static_cast<uint32_t>(orb.followingPlayerId)) {
                        following = &v;
                        break;
                    }
                }
                if (following) {
                    const glm::dvec3 d = following->pos - orb.pos;
                    if (!following->eligible || glm::dot(d, d) > kFollowDistSq) {
                        following = nullptr;
                    }
                }
            }
            if (!following) {
                double bestSq = kFollowDistSq;
                for (const auto& v : players) {
                    if (!v.eligible) continue;
                    const glm::dvec3 d = v.pos - orb.pos;
                    const double distSq = glm::dot(d, d);
                    if (distSq <= bestSq) {
                        bestSq = distSq;
                        following = &v;
                    }
                }
            }
            orb.followingPlayerId = following ? static_cast<int64_t>(following->id)
                                              : -1;

            glm::dvec3 target(0.0);
            if (following) {
                // Player mid-eye height — followNearbyPlayer's
                // `getY() + getEyeHeight() / 2`.
                target = following->pos
                       + glm::dvec3(0.0, Game::PlayerPhysics::EYE_HEIGHT_STANDING * 0.5, 0.0);
            }

            const glm::dvec3 oldVel = orb.vel;
            orb.TickMovement(ctx, following ? &target : nullptr, m_random,
                             /*isServer=*/true);

            // Same out-of-band resend rule as items: motion the client's own
            // simulation could not have predicted. The pull itself is
            // predicted client-side, so a followed orb doesn't spam packets.
            const glm::dvec3 dv = orb.vel - oldVel;
            if (glm::dot(dv, dv) > 0.01) orb.needsSync = true;

            if (orb.age != INT32_MIN) ++orb.age;
            if (orb.age >= Game::ExperienceOrb::kLifetimeTicks) {
                orb.count = 0;   // marks it for the sweep
            }
        }

        // 2. Merge scan (MC scanForMerges, every 20 ticks per orb).
        for (auto& [id, orb] : m_entities) {
            if (orb.count <= 0 || orb.pickedUp) continue;
            if (orb.tickCount % Game::ExperienceOrb::kMergeScanPeriod != 1) continue;

            const Game::AABB box = orb.GetAABB();
            const float inf = Game::ExperienceOrb::kMergeInflate;

            for (auto& [otherId, other] : m_entities) {
                if (otherId == id) continue;
                if (other.count <= 0 || other.pickedUp) continue;
                if (other.value != orb.value) continue;
                if ((other.id - orb.id) % Game::ExperienceOrb::kOrbGroups != 0) continue;

                const Game::AABB ob = other.GetAABB();
                if (ob.max.x <= box.min.x - inf || ob.min.x >= box.max.x + inf) continue;
                if (ob.max.y <= box.min.y - inf || ob.min.y >= box.max.y + inf) continue;
                if (ob.max.z <= box.min.z - inf || ob.min.z >= box.max.z + inf) continue;

                orb.count += other.count;
                orb.age = std::min(orb.age, other.age);
                other.count = 0;   // merged away — swept below as a removal
            }
        }

        // 3. Pickup (MC ExperienceOrb.playerTouch via Player.aiStep's
        //    inflate(1.0, 0.5, 1.0) touch box).
        for (const auto& v : players) {
            if (!v.eligible) continue;
            int& delay = m_takeDelay[v.id];

            const float ph = Game::PlayerPhysics::HEIGHT_STANDING;
            const Game::AABB touchBox(
                glm::vec3(v.pos.x, v.pos.y + ph * 0.5f, v.pos.z),
                glm::vec3(Game::PlayerPhysics::WIDTH + 2.0f, ph + 1.0f,
                          Game::PlayerPhysics::WIDTH + 2.0f));

            for (auto& [id, orb] : m_entities) {
                if (orb.count <= 0) continue;
                if (delay > 0) break;   // takeXpDelay — one orb per 2 ticks
                if (!orb.GetAABB().Intersects(touchBox)) continue;

                delay = Game::ExperienceOrb::kTakeDelayTicks;
                // No Mending here — the engine has no repair-with-XP
                // enchantment, so the whole value goes to the bar
                // (MC repairPlayerItems falls through to
                // giveExperiencePoints when nothing wants repair).
                v.player->getExperience().GivePoints(orb.value);

                outPickups.push_back(XpOrbPickupEvent{ orb.id, v.id });
                orb.pickedUp = true;
                --orb.count;
            }
        }

        // 4. Sweep. Orbs consumed by pickup were already retired client-side
        //    by their take packet; everything else broadcasts a removal.
        for (auto it = m_entities.begin(); it != m_entities.end();) {
            if (it->second.count <= 0) {
                if (!it->second.pickedUp) {
                    outRemoved.push_back(it->first);
                }
                it = m_entities.erase(it);
            } else {
                ++it;
            }
        }
    }

    void ExperienceOrbManager::RemoveInChunk(Game::Math::ChunkPos chunk,
                                             std::vector<int32_t>& outRemoved) {
        for (auto it = m_entities.begin(); it != m_entities.end();) {
            if (ChunkOf(it->second.pos) == chunk) {
                outRemoved.push_back(it->first);
                it = m_entities.erase(it);
            } else {
                ++it;
            }
        }
    }

    void ExperienceOrbManager::Clear() {
        m_entities.clear();
        m_takeDelay.clear();
        m_nextId = Game::kXpOrbEntityIdBase;
    }

    void ExperienceOrbManager::CollectSyncSets(int64_t serverTick,
                                               std::vector<int32_t>& outFullRefresh,
                                               std::vector<int32_t>& outMoveOnly) {
        for (auto& [id, orb] : m_entities) {
            // A draining orb no longer exists on any client (its take packet
            // removed it); re-introducing it would resurrect a ghost.
            if (orb.pickedUp) { orb.needsSync = false; continue; }

            const bool periodic = ((serverTick + id) % kSyncIntervalTicks) == 0;
            if (orb.pendingSpawn || periodic) {
                outFullRefresh.push_back(id);
                orb.pendingSpawn = false;
            } else if (orb.needsSync) {
                outMoveOnly.push_back(id);
            }
            orb.needsSync = false;
        }
    }

} // namespace Server
