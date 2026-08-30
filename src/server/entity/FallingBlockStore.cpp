// File: src/server/entity/FallingBlockStore.cpp
#include "server/entity/FallingBlockStore.hpp"

#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/core/TickParallel.hpp"
#include "common/entity/FallingBlockEntity.hpp"
#include "common/entity/FallingBlockLanding.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/entity/Item.hpp"
#include "common/network/packets/game/MobEntityPackets.hpp"
#include "common/network/packets/game/RemoveEntitiesS2CPacket.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/world/ticketing/ChunkTicketManager.hpp"

#include <algorithm>
#include <climits>
#include <cmath>

namespace Server {

    namespace {
        constexpr size_t kMaxSlots = 64;

        uint64_t ChunkKey(int cx, int cz) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) |
                    static_cast<uint32_t>(cz);
        }
        uint64_t ChunkKeyOf(const glm::dvec3& p) {
            return ChunkKey(static_cast<int>(std::floor(p.x)) >> 4,
                            static_cast<int>(std::floor(p.z)) >> 4);
        }
        Game::Math::ChunkPos ChunkOf(const glm::dvec3& p) {
            return Game::Math::ChunkPos(static_cast<int>(std::floor(p.x)) >> 4,
                                        static_cast<int>(std::floor(p.z)) >> 4);
        }

        const Game::EntityTypeInfo& Info() {
            return Game::GetEntityTypeInfo(Game::EntityTypeId::FallingBlock);
        }
        glm::vec3 Half() {
            const auto& t = Info();
            return glm::vec3(t.width * 0.5f, t.height * 0.5f, t.width * 0.5f);
        }

        void EmitTo(uint32_t connId, Network::PacketId id, std::vector<uint8_t> payload,
                    EntityPacketOut::Kind kind, std::vector<EntityPacketOut>& out) {
            EntityPacketOut p;
            p.kind = kind;
            p.connectionId = connId;
            p.packetId = id;
            p.payload = std::move(payload);
            out.push_back(std::move(p));
        }
    }

    FallingBlockStore::FallingBlockStore(ServerLevelBridge* level, MobManager* ids)
        : m_level(level), m_ids(ids) {
        m_slots.assign(kMaxSlots, 0);
    }

    bool FallingBlockStore::Takes(Game::BlockState state) {
        switch (state.Block()) {
            case Game::BlockID::Sand:
            case Game::BlockID::RedSand:
            case Game::BlockID::Gravel:
                return true;
            default:
                return false;
        }
    }

    int32_t FallingBlockStore::Spawn(const glm::ivec3& blockPos, Game::BlockState state) {
        const int32_t id = m_ids->AllocateId();
        // MC: (x + 0.5, y, z + 0.5) — horizontally centred, NOT vertically
        // offset. The position is the feet, so the block occupies exactly the
        // cell it left.
        const glm::dvec3 pos(static_cast<double>(blockPos.x) + 0.5,
                             static_cast<double>(blockPos.y),
                             static_cast<double>(blockPos.z) + 0.5);
        m_id.push_back(id);
        m_pos.push_back(pos);
        m_oldPos.push_back(pos);
        m_vel.push_back(glm::dvec3(0.0));
        m_state.push_back(state.RawId());
        m_time.push_back(0);
        m_onGround.push_back(0);
        m_dead.push_back(0);
        m_baseX.push_back(Network::EncodeEntityPos(pos.x));
        m_baseY.push_back(Network::EncodeEntityPos(pos.y));
        m_baseZ.push_back(Network::EncodeEntityPos(pos.z));
        m_watchers.push_back(0);
        m_trackTicks.push_back(0);
        m_teleportDelay.push_back(0);
        m_lastChunkKey.push_back(~0ull);
        m_wasOnGround.push_back(0);
        return id;
    }

    // ── Physics ────────────────────────────────────────────────────────────

    FallingBlockStore::Outcome FallingBlockStore::TickPhysics(size_t i) {
        // Exactly FallingBlockEntity::TickPhysics for a server-side plain
        // block: ++time, gravity, the swept mover, then the airborne fast
        // path (expiry test + drag) or a hand-off to the landing half.
        ++m_time[i];
        glm::dvec3& vel = m_vel[i];
        glm::dvec3& pos = m_pos[i];
        vel.y -= Game::FallingBlockEntity::kGravity;

        bool onGround = m_onGround[i] != 0;
        const Game::PhysicsContext ctx = m_level->Physics();
        const glm::vec3 half = Half();

        // Open-sky fast path: the same region MoveEntity gathers colliders
        // over (start box, end box, a block of headroom, a block below, the
        // epsilon skin), asked once as a section-flag test. All air means no
        // collider, and MoveEntity with no colliders moves the whole desired
        // delta and reports no contact — so that answer is written directly.
        // Every airborne tick of a fall takes this; only the landing tick
        // (and anything near terrain) pays for the mover.
        {
            constexpr double kMaxStep = 16.0;
            const glm::dvec3 d(std::clamp(vel.x, -kMaxStep, kMaxStep),
                               std::clamp(vel.y, -kMaxStep, kMaxStep),
                               std::clamp(vel.z, -kMaxStep, kMaxStep));
            const glm::dvec3 lo(pos.x - half.x + std::min(0.0, d.x) - 1.0e-3,
                                pos.y          + std::min(0.0, d.y) - 1.0 - 1.0e-3,
                                pos.z - half.z + std::min(0.0, d.z) - 1.0e-3);
            const glm::dvec3 hi(pos.x + half.x + std::max(0.0, d.x) + 1.0e-3,
                                pos.y + 2.0 * half.y + std::max(0.0, d.y) + 1.0 + 1.0e-3,
                                pos.z + half.z + std::max(0.0, d.z) + 1.0e-3);
            if (ctx.blockAccess && (d.x != 0.0 || d.y != 0.0 || d.z != 0.0) &&
                ctx.blockAccess->IsRegionAllAir(
                    glm::ivec3(static_cast<int>(std::floor(lo.x)), static_cast<int>(std::floor(lo.y)),
                               static_cast<int>(std::floor(lo.z))),
                    glm::ivec3(static_cast<int>(std::floor(hi.x)), static_cast<int>(std::floor(hi.y)),
                               static_cast<int>(std::floor(hi.z))),
                    /*absentIsAir=*/true)) {
                pos += d;
                m_onGround[i] = 0;
                // Airborne by construction; the expiry test below still runs.
                onGround = false;
            } else {
                const Game::EntityMoveResult r =
                    Game::MoveEntity(pos, vel, half, /*maxUpStep=*/0.0f, onGround, ctx);
                onGround = r.onGround;
                m_onGround[i] = onGround ? 1 : 0;
            }
        }

        // MC Entity.move's tail: the block speed factor (soul sand, honey)
        // for a grounded entity — see Entity::Move.
        if (onGround) {
            if (const Game::IBlockAccess* blocks = m_level->Blocks()) {
                const auto factorOf = [](Game::BlockID id) {
                    return (id == Game::BlockID::SoulSand || id == Game::BlockID::HoneyBlock)
                        ? 0.4f : 1.0f;
                };
                const int bx = static_cast<int>(std::floor(pos.x));
                const int by = static_cast<int>(std::floor(pos.y));
                const int bz = static_cast<int>(std::floor(pos.z));
                const Game::BlockID here = blocks->GetBlock(bx, by, bz);
                float f = factorOf(here);
                if (here != Game::BlockID::Water && f == 1.0f) {
                    f = factorOf(blocks->GetBlock(
                        bx, static_cast<int>(std::floor(pos.y - 0.500001)), bz));
                }
                if (f != 1.0f) { vel.x *= f; vel.z *= f; }
            }
        }

        if (!onGround) {
            const int cy = static_cast<int>(std::floor(pos.y));
            const bool outOfWorld = cy <= m_level->GetMinY() || cy > m_level->GetMaxY();
            const bool expiring =
                (m_time[i] > Game::FallingBlockEntity::kOutOfWorldGrace && outOfWorld) ||
                m_time[i] > Game::FallingBlockEntity::kMaxLifetime;
            if (!expiring) {
                vel *= Game::FallingBlockEntity::kAirDrag;
                return Outcome::Airborne;
            }
        }
        return Outcome::NeedsLanding;
    }

    void FallingBlockStore::Tick(const ChunkTicketManager* tickets) {
        const size_t n = m_id.size();
        if (n == 0) return;
        PROFILE_ZONE_N("FallingStore.Tick");
        PROFILE_PLOT("FallingStore/Count", static_cast<int64_t>(n));

        m_snap.resize(n);
        m_outcome.resize(n);

        // ── Phase 1: physics across the pool ───────────────────────────────
        // MC ticks only entities in entity-ticking chunks; the gate is
        // memoised per runner on the last chunk it saw (insertion order puts
        // long runs in one chunk).
        {
            PROFILE_ZONE_N("FallingStore.Physics");
            const auto body = [&](size_t i) {
                if (m_dead[i]) { m_outcome[i] = static_cast<uint8_t>(Outcome::Skipped); return; }
                if (tickets) {
                    thread_local uint64_t t_memoKey = ~0ull;
                    thread_local bool     t_memoTicking = false;
                    const uint64_t key = ChunkKeyOf(m_pos[i]);
                    if (key != t_memoKey) {
                        t_memoKey = key;
                        t_memoTicking = tickets->IsEntityTickingAfterUpdates(ChunkOf(m_pos[i]));
                    }
                    if (!t_memoTicking) {
                        m_outcome[i] = static_cast<uint8_t>(Outcome::Skipped);
                        return;
                    }
                }
                m_oldPos[i] = m_pos[i];
                m_snap[i] = Snapshot{m_pos[i], m_vel[i], m_onGround[i] != 0};
                m_outcome[i] = static_cast<uint8_t>(TickPhysics(i));
            };
            if (n >= 256 && Core::ParallelWidth() > 1) {
                Core::ParallelFor(n, 64, body);
            } else {
                for (size_t i = 0; i < n; ++i) body(i);
            }
        }

        // ── Phase 2: landing, serial, in insertion order ───────────────────
        // Same conflict-retry rule as MobManager's falling batch: a block
        // whose swept region holds a cell an EARLIER block filled this tick
        // rolls back to its snapshot and re-runs its move against the world
        // the serial loop would have seen. That is what keeps a column
        // collapsing in one tick.
        {
            PROFILE_ZONE_N("FallingStore.Landing");
            Game::ILevelWrite* level = m_level->MutableBlocks();
            m_writtenColumns.clear();
            const glm::vec3 half = Half();
            const auto conflicts = [&](const Snapshot& snap) {
                constexpr double kMaxStep = 16.0;
                glm::dvec3 d = snap.velocity;
                d.y -= Game::FallingBlockEntity::kGravity;
                d = glm::dvec3(std::clamp(d.x, -kMaxStep, kMaxStep),
                               std::clamp(d.y, -kMaxStep, kMaxStep),
                               std::clamp(d.z, -kMaxStep, kMaxStep));
                const glm::dvec3 a = snap.position;
                const glm::dvec3 b = snap.position + d;
                const int x0 = static_cast<int>(std::floor(std::min(a.x, b.x) - half.x)) - 1;
                const int x1 = static_cast<int>(std::floor(std::max(a.x, b.x) + half.x)) + 1;
                const int z0 = static_cast<int>(std::floor(std::min(a.z, b.z) - half.z)) - 1;
                const int z1 = static_cast<int>(std::floor(std::max(a.z, b.z) + half.z)) + 1;
                const int y0 = static_cast<int>(std::floor(std::min(a.y, b.y))) - 2;
                const int y1 = static_cast<int>(std::floor(std::max(a.y, b.y) + 2.0 * half.y)) + 2;
                for (int x = x0; x <= x1; ++x) {
                    for (int z = z0; z <= z1; ++z) {
                        const auto it = m_writtenColumns.find(ChunkKey(x, z));
                        if (it == m_writtenColumns.end()) continue;
                        if (it->second.first <= y1 && it->second.second >= y0) return true;
                    }
                }
                return false;
            };
            const bool doDrops = m_level->DoEntityDrops();
            size_t retries = 0;
            for (size_t i = 0; i < n; ++i) {
                if (m_outcome[i] == static_cast<uint8_t>(Outcome::Skipped)) continue;
                if (!m_writtenColumns.empty() && conflicts(m_snap[i])) {
                    m_pos[i] = m_snap[i].position;
                    m_vel[i] = m_snap[i].velocity;
                    m_onGround[i] = m_snap[i].onGround ? 1 : 0;
                    --m_time[i];
                    m_outcome[i] = static_cast<uint8_t>(TickPhysics(i));
                    ++retries;
                }
                if (m_outcome[i] != static_cast<uint8_t>(Outcome::NeedsLanding)) continue;

                glm::dvec3& vel = m_vel[i];
                const glm::dvec3& pos = m_pos[i];
                const Game::BlockState state = Game::BlockState::FromRawId(m_state[i]);
                if (!m_onGround[i]) {
                    // Expiry: out of the world for long enough, or simply too
                    // old. MC drops the item (gamerule permitting) and discards.
                    if (doDrops) Game::DropItemStackAt(pos, Game::ItemStack(state.Block(), 1));
                    m_dead[i] = 1;
                } else if (level) {
                    // MC damps BEFORE trying to place, so a block that fails to
                    // place and pops as an item has already lost its momentum.
                    vel = glm::dvec3(vel.x * Game::FallingBlockEntity::kLandHorizontal,
                                     vel.y * Game::FallingBlockEntity::kLandVertical,
                                     vel.z * Game::FallingBlockEntity::kLandHorizontal);
                    const glm::ivec3 cell(static_cast<int>(std::floor(pos.x)),
                                          static_cast<int>(std::floor(pos.y)),
                                          static_cast<int>(std::floor(pos.z)));
                    const auto outcome = Game::FallingBlockTryLand(
                        *level, cell, state, pos, /*dropItem=*/true, /*cancelDrop=*/false, doDrops);
                    if (outcome != Game::FallingBlockLandOutcome::Retry) m_dead[i] = 1;
                    // Conservative: the cell was targeted whether or not the
                    // write went through; a spurious retry recomputes the
                    // same answer.
                    const auto [it, fresh] = m_writtenColumns.try_emplace(
                        ChunkKey(cell.x, cell.z), cell.y, cell.y);
                    if (!fresh) {
                        it->second.first  = std::min(it->second.first,  cell.y);
                        it->second.second = std::max(it->second.second, cell.y);
                    }
                }
                // ALWAYS last, outside every branch — see FallingBlockEntity.
                vel *= Game::FallingBlockEntity::kAirDrag;
            }
            PROFILE_PLOT("FallingStore/Retries", static_cast<int64_t>(retries));
        }
    }

    // ── Tracking ───────────────────────────────────────────────────────────

    int FallingBlockStore::SlotOf(uint32_t connectionId) {
        for (size_t s = 0; s < m_slots.size(); ++s) {
            if (m_slots[s] == connectionId) return static_cast<int>(s);
        }
        for (size_t s = 0; s < m_slots.size(); ++s) {
            if (m_slots[s] == 0) { m_slots[s] = connectionId; return static_cast<int>(s); }
        }
        return -1;
    }

    void FallingBlockStore::RemovePlayer(uint32_t connectionId) {
        for (size_t s = 0; s < m_slots.size(); ++s) {
            if (m_slots[s] != connectionId) continue;
            const uint64_t bit = 1ull << s;
            for (uint64_t& w : m_watchers) w &= ~bit;
            m_slots[s] = 0;
        }
    }

    void FallingBlockStore::Track(const std::vector<ServerEntityTracker::TrackedPlayer>& players,
                                  const ChunkTicketManager* tickets,
                                  std::vector<EntityPacketOut>& out) {
        const size_t n = m_id.size();
        if (n == 0) return;
        PROFILE_ZONE_N("FallingStore.Track");

        // Players who left since last tick lose their bit everywhere.
        for (size_t s = 0; s < m_slots.size(); ++s) {
            if (m_slots[s] == 0) continue;
            const bool present = std::any_of(players.begin(), players.end(),
                [&](const ServerEntityTracker::TrackedPlayer& p) { return p.connectionId == m_slots[s]; });
            if (!present) RemovePlayer(m_slots[s]);
        }
        struct Slot { int slot; const ServerEntityTracker::TrackedPlayer* p; };
        std::vector<Slot> slots;
        for (const auto& p : players) {
            const int s = SlotOf(p.connectionId);
            if (s < 0) {
                static bool warned = false;
                if (!warned) { Log::Warning("[FallingBlockStore] more than %zu players; extras untracked", kMaxSlots); warned = true; }
                continue;
            }
            slots.push_back(Slot{s, &p});
        }

        // Per-slice outputs, merged in order below: the per-block evaluation
        // — range, chunk-sent, movement — is pure per row, so it runs across
        // the pool. Packets themselves are built serially from these.
        struct SliceOut {
            std::vector<std::pair<uint8_t, size_t>>  adds;      // (slot, row)
            std::vector<std::pair<uint8_t, int32_t>> removes;   // (slot, id)
            std::vector<std::pair<uint8_t, Network::MoveEntityS2CPacket::Entry>> moves;
            std::vector<std::pair<uint8_t, size_t>>  syncs;     // (slot, row)
        };
        constexpr size_t kSlice = 4096;
        const size_t sliceCount = (n + kSlice - 1) / kSlice;
        std::vector<SliceOut> parts(sliceCount);

        const int effectiveRange = Info().clientTrackingRange * 16;
        const int updateInterval = std::max(1, Info().updateInterval);

        const auto body = [&](size_t si) {
            SliceOut& o = parts[si];
            const size_t begin = si * kSlice, end = std::min(n, begin + kSlice);
            uint64_t memoKey = ~0ull; bool memoTicking = false;
            uint64_t memoSent = 0;   // bit per slot: chunk sent to that player
            for (size_t i = begin; i < end; ++i) {
                const int32_t id = m_id[i];
                if (m_dead[i]) {
                    // Retired this tick: tell everyone who saw it.
                    for (const Slot& s : slots) {
                        if (m_watchers[i] & (1ull << s.slot)) o.removes.emplace_back(s.slot, id);
                    }
                    m_watchers[i] = 0;
                    continue;
                }
                const glm::dvec3& pos = m_pos[i];
                const uint64_t key = ChunkKeyOf(pos);
                if (key != memoKey) {
                    memoKey = key;
                    const Game::Math::ChunkPos chunk = ChunkOf(pos);
                    memoTicking = (tickets == nullptr) || tickets->IsEntityTickingAfterUpdates(chunk);
                    memoSent = 0;
                    for (const Slot& s : slots) {
                        if (s.p->sentChunks == nullptr || s.p->sentChunks->count(chunk) != 0) {
                            memoSent |= 1ull << s.slot;
                        }
                    }
                }
                // Watch set — MC ChunkMap.TrackedEntity.updatePlayer.
                for (const Slot& s : slots) {
                    const uint64_t bit = 1ull << s.slot;
                    const double visibleRange = std::min(static_cast<double>(effectiveRange),
                                                         static_cast<double>(s.p->viewDistance) * 16.0);
                    const double dx = s.p->position.x - pos.x, dz = s.p->position.z - pos.z;
                    const bool inRange = (dx * dx + dz * dz) <= visibleRange * visibleRange &&
                                         (memoSent & bit) != 0;
                    const bool watching = (m_watchers[i] & bit) != 0;
                    if (inRange && !watching) {
                        m_watchers[i] |= bit;
                        o.adds.emplace_back(s.slot, i);
                    } else if (!inRange && watching) {
                        m_watchers[i] &= ~bit;
                        o.removes.emplace_back(s.slot, id);
                    }
                }
                ++m_trackTicks[i];
                if (m_watchers[i] == 0) continue;

                const bool sectionPosChanged = key != m_lastChunkKey[i];
                m_lastChunkKey[i] = key;
                if (!memoTicking && !sectionPosChanged) continue;
                ++m_teleportDelay[i];

                // Movement on the type's update interval only — this entity
                // has no synched data that changes.
                if ((m_trackTicks[i] % updateInterval) != 0) continue;

                const int64_t curX = Network::EncodeEntityPos(pos.x);
                const int64_t curY = Network::EncodeEntityPos(pos.y);
                const int64_t curZ = Network::EncodeEntityPos(pos.z);
                const int64_t dX = curX - m_baseX[i], dY = curY - m_baseY[i], dZ = curZ - m_baseZ[i];
                const double ddx = Network::DecodeEntityPos(dX);
                const double ddy = Network::DecodeEntityPos(dY);
                const double ddz = Network::DecodeEntityPos(dZ);
                const bool positionChanged =
                    (ddx * ddx + ddy * ddy + ddz * ddz) >= ServerEntityTracker::kPositionTolerance;
                const bool forcePos = ((m_trackTicks[i] + static_cast<uint32_t>(id)) %
                                       ServerEntityTracker::kForcedPosUpdatePeriod) == 0;
                const bool sendPos = positionChanged || forcePos;
                const bool deltaTooBig = std::abs(dX) > ServerEntityTracker::kMaxDelta ||
                                         std::abs(dY) > ServerEntityTracker::kMaxDelta ||
                                         std::abs(dZ) > ServerEntityTracker::kMaxDelta;
                const bool forceTeleport = m_teleportDelay[i] > ServerEntityTracker::kForcedTeleportPeriod;
                const bool groundChanged = (m_wasOnGround[i] != 0) != (m_onGround[i] != 0);

                if (deltaTooBig || forceTeleport || groundChanged) {
                    for (const Slot& s : slots) {
                        if (m_watchers[i] & (1ull << s.slot)) o.syncs.emplace_back(s.slot, i);
                    }
                    m_baseX[i] = curX; m_baseY[i] = curY; m_baseZ[i] = curZ;
                    m_wasOnGround[i] = m_onGround[i];
                    m_teleportDelay[i] = 0;
                } else if (sendPos) {
                    Network::MoveEntityS2CPacket::Entry e;
                    e.entityId = id;
                    e.onGround = m_onGround[i] != 0;
                    e.mask = 0x01;
                    e.dx = static_cast<int16_t>(dX);
                    e.dy = static_cast<int16_t>(dY);
                    e.dz = static_cast<int16_t>(dZ);
                    for (const Slot& s : slots) {
                        if (m_watchers[i] & (1ull << s.slot)) o.moves.emplace_back(s.slot, e);
                    }
                    m_baseX[i] = curX; m_baseY[i] = curY; m_baseZ[i] = curZ;
                }
            }
        };
        if (sliceCount >= 2 && Core::ParallelWidth() > 1) {
            Core::ParallelFor(sliceCount, 1, body);
        } else {
            for (size_t si = 0; si < sliceCount; ++si) body(si);
        }

        // ── Merge, in slice order, into packets ────────────────────────────
        // Adds and syncs are one packet each; moves and removals batch per
        // recipient, chunked under the reader's frame cap like the tracker.
        constexpr size_t kMoveEntriesPerPacket = 32768;
        std::vector<Network::MoveEntityS2CPacket>   moveBatch(m_slots.size());
        std::vector<Network::RemoveEntitiesS2CPacket> removeBatch(m_slots.size());
        std::vector<Network::EntityPositionSyncBatchS2CPacket> syncBatch(m_slots.size());
        for (const SliceOut& o : parts) {
            for (const auto& [slot, row] : o.adds) {
                Network::AddEntityS2CPacket p;
                p.entityId   = m_id[row];
                p.entityType = static_cast<uint16_t>(Game::EntityTypeId::FallingBlock);
                // The DELTA BASE, not the live position — see
                // ServerEntityTracker::BuildAddPacket.
                p.position   = glm::dvec3(Network::DecodeEntityPos(m_baseX[row]),
                                          Network::DecodeEntityPos(m_baseY[row]),
                                          Network::DecodeEntityPos(m_baseZ[row]));
                p.velocity   = glm::vec3(m_vel[row]);
                p.health     = 20.0f;
                p.vehicleId  = -1;
                p.blockStateRaw = m_state[row];
                EmitTo(m_slots[slot], Network::PacketId::AddEntityS2C,
                       Network::Serialization::Serialize(p), EntityPacketOut::Kind::Add, out);
            }
            for (const auto& [slot, row] : o.syncs) {
                Network::EntityPositionSyncBatchS2CPacket::Entry e;
                e.entityId = m_id[row];
                e.position = m_pos[row];
                e.velocity = glm::vec3(m_vel[row]);
                e.onGround = m_onGround[row] != 0;
                syncBatch[slot].entries.push_back(e);
            }
            for (const auto& [slot, e] : o.moves) moveBatch[slot].entries.push_back(e);
            for (const auto& [slot, id] : o.removes) removeBatch[slot].entityIds.push_back(id);
        }
        for (size_t s = 0; s < m_slots.size(); ++s) {
            if (m_slots[s] == 0) continue;
            auto& mb = moveBatch[s];
            for (size_t i = 0; i < mb.entries.size(); i += kMoveEntriesPerPacket) {
                Network::MoveEntityS2CPacket part;
                part.entries.assign(mb.entries.begin() + static_cast<std::ptrdiff_t>(i),
                                    mb.entries.begin() + static_cast<std::ptrdiff_t>(
                                        std::min(mb.entries.size(), i + kMoveEntriesPerPacket)));
                EmitTo(m_slots[s], Network::PacketId::MoveEntityS2C,
                       Network::Serialization::Serialize(part), EntityPacketOut::Kind::Move, out);
            }
            // Syncs: every falling block at terminal velocity takes this path
            // on its update interval (see EntityPositionSyncBatchS2CPacket),
            // so this is the bulk of the store's traffic. ~37 bytes an entry.
            constexpr size_t kSyncEntriesPerPacket = 16384;
            auto& sb = syncBatch[s];
            for (size_t i = 0; i < sb.entries.size(); i += kSyncEntriesPerPacket) {
                Network::EntityPositionSyncBatchS2CPacket part;
                part.entries.assign(sb.entries.begin() + static_cast<std::ptrdiff_t>(i),
                                    sb.entries.begin() + static_cast<std::ptrdiff_t>(
                                        std::min(sb.entries.size(), i + kSyncEntriesPerPacket)));
                EmitTo(m_slots[s], Network::PacketId::EntityPositionSyncBatchS2C,
                       Network::Serialization::Serialize(part), EntityPacketOut::Kind::PositionSync, out);
            }
            auto& rb = removeBatch[s];
            for (size_t i = 0; i < rb.entityIds.size(); i += kMoveEntriesPerPacket) {
                Network::RemoveEntitiesS2CPacket part;
                part.entityIds.assign(rb.entityIds.begin() + static_cast<std::ptrdiff_t>(i),
                                      rb.entityIds.begin() + static_cast<std::ptrdiff_t>(
                                          std::min(rb.entityIds.size(), i + kMoveEntriesPerPacket)));
                EmitTo(m_slots[s], Network::PacketId::EntityDestroy,
                       Network::Serialization::Serialize(part), EntityPacketOut::Kind::Remove, out);
            }
        }

        Compact();
    }

    void FallingBlockStore::Compact() {
        const size_t n = m_id.size();
        size_t w = 0;
        for (size_t i = 0; i < n; ++i) {
            if (m_dead[i]) continue;
            if (w != i) {
                m_id[w] = m_id[i]; m_pos[w] = m_pos[i]; m_oldPos[w] = m_oldPos[i];
                m_vel[w] = m_vel[i]; m_state[w] = m_state[i]; m_time[w] = m_time[i];
                m_onGround[w] = m_onGround[i];
                m_baseX[w] = m_baseX[i]; m_baseY[w] = m_baseY[i]; m_baseZ[w] = m_baseZ[i];
                m_watchers[w] = m_watchers[i]; m_trackTicks[w] = m_trackTicks[i];
                m_teleportDelay[w] = m_teleportDelay[i]; m_lastChunkKey[w] = m_lastChunkKey[i];
                m_wasOnGround[w] = m_wasOnGround[i];
            }
            m_dead[w] = 0;
            ++w;
        }
        if (w == n) return;
        m_id.resize(w); m_pos.resize(w); m_oldPos.resize(w); m_vel.resize(w);
        m_state.resize(w); m_time.resize(w); m_onGround.resize(w); m_dead.resize(w);
        m_baseX.resize(w); m_baseY.resize(w); m_baseZ.resize(w); m_watchers.resize(w);
        m_trackTicks.resize(w); m_teleportDelay.resize(w); m_lastChunkKey.resize(w);
        m_wasOnGround.resize(w);
    }

    void FallingBlockStore::RemoveInChunks(const std::vector<Game::Math::ChunkPos>& chunks,
                                           std::vector<EntityPacketOut>& out) {
        if (chunks.empty() || m_id.empty()) return;
        std::vector<uint64_t> keys;
        keys.reserve(chunks.size());
        for (const auto& c : chunks) keys.push_back(ChunkKey(c.x, c.z));
        std::sort(keys.begin(), keys.end());

        std::vector<Network::RemoveEntitiesS2CPacket> removeBatch(m_slots.size());
        bool any = false;
        for (size_t i = 0; i < m_id.size(); ++i) {
            if (m_dead[i]) continue;
            if (!std::binary_search(keys.begin(), keys.end(), ChunkKeyOf(m_pos[i]))) continue;
            m_dead[i] = 1;
            any = true;
            for (size_t s = 0; s < m_slots.size(); ++s) {
                if (m_watchers[i] & (1ull << s)) removeBatch[s].entityIds.push_back(m_id[i]);
            }
        }
        if (!any) return;
        for (size_t s = 0; s < m_slots.size(); ++s) {
            if (m_slots[s] == 0 || removeBatch[s].entityIds.empty()) continue;
            EmitTo(m_slots[s], Network::PacketId::EntityDestroy,
                   Network::Serialization::Serialize(removeBatch[s]),
                   EntityPacketOut::Kind::Remove, out);
        }
        Compact();
    }

    void FallingBlockStore::Clear() {
        m_id.clear(); m_pos.clear(); m_oldPos.clear(); m_vel.clear(); m_state.clear();
        m_time.clear(); m_onGround.clear(); m_dead.clear();
        m_baseX.clear(); m_baseY.clear(); m_baseZ.clear(); m_watchers.clear();
        m_trackTicks.clear(); m_teleportDelay.clear(); m_lastChunkKey.clear();
        m_wasOnGround.clear();
        std::fill(m_slots.begin(), m_slots.end(), 0u);
    }

} // namespace Server
