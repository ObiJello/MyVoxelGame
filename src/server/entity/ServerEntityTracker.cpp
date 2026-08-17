// File: src/server/entity/ServerEntityTracker.cpp
#include "server/entity/ServerEntityTracker.hpp"
#include "common/network/packets/game/RemoveEntitiesS2CPacket.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/core/Mth.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <algorithm>
#include <cmath>

namespace Server {

    namespace {
        constexpr uint8_t kFlagBaby       = 0x01;
        constexpr uint8_t kFlagAggressive = 0x02;
        constexpr uint8_t kFlagOnFire     = 0x04;

        int64_t Encode(double v) { return Network::EncodeEntityPos(v); }
    }

    uint8_t ServerEntityTracker::PackFlags(const Game::Mob& mob) {
        uint8_t flags = 0;
        if (mob.IsBaby())       flags |= kFlagBaby;
        if (mob.IsAggressive()) flags |= kFlagAggressive;
        if (mob.IsOnFire())     flags |= kFlagOnFire;
        return flags;
    }

    uint8_t ServerEntityTracker::VariantData(const Game::Mob& mob) {
        // Only sheep carry a variant byte today. The field exists on the wire
        // for every type so adding one (cow/pig biome variants) needs no
        // protocol change.
        if (const auto* sheep = dynamic_cast<const Game::Sheep*>(&mob)) {
            return sheep->GetWoolData();
        }
        return 0;
    }

    Network::AddEntityS2CPacket ServerEntityTracker::BuildAddPacket(const Game::Mob& mob,
                                                                    const glm::dvec3& base) {
        Network::AddEntityS2CPacket p;
        p.entityId   = mob.GetId();
        p.entityType = static_cast<uint16_t>(mob.GetType());
        // The DELTA BASE, not the live position.
        //
        // Movement packets carry an offset from the last position the server
        // sent, and that base is shared by every watcher. A client seeded from
        // the live position instead would apply subsequent deltas against a
        // different origin than the server encoded them from — so a player who
        // started tracking mid-flight would see the mob drift further and
        // further from where it actually is. MC's sendPairingData sends
        // positionCodec.getBase() for exactly this reason.
        p.position   = base;
        p.velocity   = glm::vec3(mob.velocity);
        p.yRot       = Game::Mth::PackDegrees(mob.yRot);
        p.xRot       = Game::Mth::PackDegrees(mob.xRot);
        p.yHeadRot   = Game::Mth::PackDegrees(mob.GetYHeadRot());
        p.health     = mob.GetHealth();
        p.flags      = PackFlags(mob);
        p.variantData = VariantData(mob);
        p.pose       = static_cast<uint8_t>(mob.GetPose());
        p.animState  = mob.GetAnimStateByte();
        return p;
    }

    void ServerEntityTracker::EmitTo(uint32_t connectionId, Network::PacketId id,
                                     std::vector<uint8_t> payload,
                                     EntityPacketOut::Kind kind,
                                     std::vector<EntityPacketOut>& out) {
        EntityPacketOut packet;
        packet.kind = kind;
        packet.connectionId = connectionId;
        packet.packetId = id;
        packet.payload = std::move(payload);
        out.push_back(std::move(packet));
    }

    void ServerEntityTracker::Tick(const MobManager& mobs, ServerLevelBridge& level,
                                   const std::vector<std::pair<uint32_t, glm::dvec3>>& players,
                                   std::vector<EntityPacketOut>& out) {
        PROFILE_ZONE_N("EntityTracker");

        // Per-tick move batching: one MoveEntityS2C per recipient rather than
        // one per mob. A player watching 60 mobs gets one packet, not 60.
        std::unordered_map<uint32_t, Network::MoveEntityS2CPacket> moveBatches;

        for (const auto& [id, mobPtr] : mobs.All()) {
            const Game::Mob& mob = *mobPtr;
            const bool firstSight = m_tracked.find(id) == m_tracked.end();
            Tracked& tracked = m_tracked[id];

            if (firstSight) {
                // Seed the delta base from where the mob actually is, so the
                // first AddEntity and the first delta agree.
                tracked.baseX = Encode(mob.position.x);
                tracked.baseY = Encode(mob.position.y);
                tracked.baseZ = Encode(mob.position.z);
                tracked.lastYRot = Game::Mth::PackDegrees(mob.yRot);
                tracked.lastXRot = Game::Mth::PackDegrees(mob.xRot);
                tracked.lastYHeadRot = Game::Mth::PackDegrees(mob.GetYHeadRot());
                tracked.wasOnGround = mob.onGround;
            }

            const glm::dvec3 basePos(Network::DecodeEntityPos(tracked.baseX),
                                     Network::DecodeEntityPos(tracked.baseY),
                                     Network::DecodeEntityPos(tracked.baseZ));

            // ── Watch set ──────────────────────────────────────────────────
            const double range = static_cast<double>(mob.TypeInfo().clientTrackingRange) * 16.0;
            const double rangeSq = range * range;

            for (const auto& [connId, playerPos] : players) {
                const double dx = playerPos.x - mob.position.x;
                const double dz = playerPos.z - mob.position.z;
                // Horizontal only, like MC — a player directly above a mob at
                // build height still tracks it.
                const bool inRange = (dx * dx + dz * dz) <= rangeSq;
                const bool watching = tracked.watchers.count(connId) != 0;

                if (inRange && !watching) {
                    tracked.watchers.insert(connId);
                    EmitTo(connId, Network::PacketId::AddEntityS2C,
                           Network::Serialization::Serialize(BuildAddPacket(mob, basePos)),
                           EntityPacketOut::Kind::Add, out);
                } else if (!inRange && watching) {
                    tracked.watchers.erase(connId);
                    Network::RemoveEntitiesS2CPacket removal;
                    removal.entityIds.push_back(id);
                    EmitTo(connId, Network::PacketId::EntityDestroy,
                           Network::Serialization::Serialize(removal),
                           EntityPacketOut::Kind::Remove, out);
                }
            }

            // Drop watchers who are no longer connected at all.
            for (auto it = tracked.watchers.begin(); it != tracked.watchers.end();) {
                const bool stillHere = std::any_of(players.begin(), players.end(),
                    [&](const auto& p) { return p.first == *it; });
                it = stillHere ? std::next(it) : tracked.watchers.erase(it);
            }

            if (tracked.watchers.empty()) { ++tracked.tickCount; continue; }

            // ── Movement ───────────────────────────────────────────────────
            ++tracked.tickCount;
            ++tracked.teleportDelay;

            const int updateInterval = std::max(1, mob.TypeInfo().updateInterval);
            const bool dueThisTick = (tracked.tickCount % updateInterval) == 0;
            if (!dueThisTick && !mob.needsSync) {
                continue;
            }

            const int8_t yRotN     = Game::Mth::PackDegrees(mob.yRot);
            const int8_t xRotN     = Game::Mth::PackDegrees(mob.xRot);
            const int8_t yHeadRotN = Game::Mth::PackDegrees(mob.GetYHeadRot());

            const bool rotationChanged =
                std::abs(yRotN - tracked.lastYRot) >= 1 ||
                std::abs(xRotN - tracked.lastXRot) >= 1 ||
                std::abs(yHeadRotN - tracked.lastYHeadRot) >= 1;

            const int64_t curX = Encode(mob.position.x);
            const int64_t curY = Encode(mob.position.y);
            const int64_t curZ = Encode(mob.position.z);

            const int64_t dX = curX - tracked.baseX;
            const int64_t dY = curY - tracked.baseY;
            const int64_t dZ = curZ - tracked.baseZ;

            // MC compares the DECODED delta against a squared tolerance rather
            // than testing the integers for zero, so sub-quantum jitter does
            // not generate traffic.
            const double ddx = Network::DecodeEntityPos(dX);
            const double ddy = Network::DecodeEntityPos(dY);
            const double ddz = Network::DecodeEntityPos(dZ);
            const bool positionChanged =
                (ddx * ddx + ddy * ddy + ddz * ddz) >= kPositionTolerance;

            const bool forcePos = (tracked.tickCount % kForcedPosUpdatePeriod) == 0;
            const bool sendPos = positionChanged || forcePos;

            const bool deltaTooBig = std::abs(dX) > kMaxDelta ||
                                     std::abs(dY) > kMaxDelta ||
                                     std::abs(dZ) > kMaxDelta;
            const bool forceTeleport = tracked.teleportDelay > kForcedTeleportPeriod;
            const bool groundChanged = tracked.wasOnGround != mob.onGround;

            if (deltaTooBig || forceTeleport || groundChanged) {
                Network::EntityPositionSyncS2CPacket p;
                p.entityId = id;
                p.position = mob.position;
                p.velocity = glm::vec3(mob.velocity);
                p.yRot = yRotN;
                p.xRot = xRotN;
                p.yHeadRot = yHeadRotN;
                p.onGround = mob.onGround;

                const auto payload = Network::Serialization::Serialize(p);
                for (uint32_t connId : tracked.watchers) {
                    EmitTo(connId, Network::PacketId::EntityPositionSyncS2C, payload,
                           EntityPacketOut::Kind::PositionSync, out);
                }

                tracked.baseX = curX; tracked.baseY = curY; tracked.baseZ = curZ;
                tracked.lastYRot = yRotN; tracked.lastXRot = xRotN;
                tracked.lastYHeadRot = yHeadRotN;
                tracked.wasOnGround = mob.onGround;
                tracked.teleportDelay = 0;

            } else if (sendPos || rotationChanged) {
                Network::MoveEntityS2CPacket::Entry e;
                e.entityId = id;
                e.onGround = mob.onGround;

                if (sendPos) {
                    e.mask |= 0x01;
                    e.dx = static_cast<int16_t>(dX);
                    e.dy = static_cast<int16_t>(dY);
                    e.dz = static_cast<int16_t>(dZ);
                }
                if (rotationChanged) {
                    e.mask |= 0x02;
                    e.yRot = yRotN;
                    e.xRot = xRotN;
                    e.yHeadRot = yHeadRotN;
                }

                for (uint32_t connId : tracked.watchers) {
                    moveBatches[connId].entries.push_back(e);
                }

                // The base only advances when a POSITION actually went out —
                // advancing it on a rotation-only packet would silently drop
                // the movement since the last send.
                if (sendPos) {
                    tracked.baseX = curX; tracked.baseY = curY; tracked.baseZ = curZ;
                }
                if (rotationChanged) {
                    tracked.lastYRot = yRotN; tracked.lastXRot = xRotN;
                    tracked.lastYHeadRot = yHeadRotN;
                }
            }

            // ── Velocity ───────────────────────────────────────────────────
            // Only when something asked for it (knockback, jump, leap). Sending
            // velocity every tick would undo the client's own simulation.
            if (mob.needsSync || mob.hurtMarked) {
                Network::SetEntityMotionS2CPacket p;
                p.entityId = id;
                p.velocity = glm::vec3(mob.velocity);

                const auto payload = Network::Serialization::Serialize(p);
                for (uint32_t connId : tracked.watchers) {
                    EmitTo(connId, Network::PacketId::SetEntityMotionS2C, payload,
                           EntityPacketOut::Kind::Motion, out);
                }
            }

            // ── Synched data ───────────────────────────────────────────────
            const uint8_t flags = PackFlags(mob);
            const uint8_t variant = VariantData(mob);
            const auto hurtTime = static_cast<uint8_t>(std::clamp(mob.hurtTime, 0, 255));
            const auto deathTime = static_cast<uint8_t>(std::clamp(mob.deathTime, 0, 255));

            uint8_t swell = 0, swellDir = 0;
            if (const auto* creeper = dynamic_cast<const Game::Creeper*>(&mob)) {
                swellDir = creeper->GetSwellDir() > 0 ? 1 : 0;
                // Derived from the render fraction rather than read directly:
                // the fuse counter is private to Creeper, and the client only
                // needs enough resolution to drive the flash.
                swell = static_cast<uint8_t>(
                    std::clamp(creeper->GetSwelling(0.0f) * (Game::Creeper::kMaxSwell - 2),
                               0.0f, 255.0f));
            }

            const uint8_t pose = static_cast<uint8_t>(mob.GetPose());
            const uint8_t animState = mob.GetAnimStateByte();

            const bool dataChanged =
                flags != tracked.lastFlags || variant != tracked.lastVariant ||
                hurtTime != tracked.lastHurtTime || deathTime != tracked.lastDeathTime ||
                swell != tracked.lastSwell || pose != tracked.lastPose ||
                animState != tracked.lastAnimState ||
                std::abs(mob.GetHealth() - tracked.lastHealth) > 1.0e-4f;

            if (dataChanged) {
                Network::SetEntityDataS2CPacket p;
                p.entityId = id;
                p.health = mob.GetHealth();
                p.flags = flags;
                p.variantData = variant;
                p.hurtTime = hurtTime;
                p.deathTime = deathTime;
                p.swellDir = swellDir;
                p.swell = swell;
                p.pose = pose;
                p.animState = animState;

                const auto payload = Network::Serialization::Serialize(p);
                for (uint32_t connId : tracked.watchers) {
                    EmitTo(connId, Network::PacketId::SetEntityDataS2C, payload,
                           EntityPacketOut::Kind::Data, out);
                }

                tracked.lastHealth = mob.GetHealth();
                tracked.lastFlags = flags;
                tracked.lastVariant = variant;
                tracked.lastHurtTime = hurtTime;
                tracked.lastDeathTime = deathTime;
                tracked.lastSwell = swell;
                tracked.lastPose = pose;
                tracked.lastAnimState = animState;
            }
        }

        // Flush the batched move packets.
        for (auto& [connId, batch] : moveBatches) {
            if (batch.entries.empty()) continue;
            EmitTo(connId, Network::PacketId::MoveEntityS2C,
                   Network::Serialization::Serialize(batch),
                   EntityPacketOut::Kind::Move, out);
        }

        // ── Entity events raised by the entity system this tick ────────────
        auto& events = level.DrainEvents();
        for (const auto& ev : events) {
            const auto it = m_tracked.find(ev.entityId);
            if (it == m_tracked.end()) continue;

            Network::EntityEventS2CPacket p;
            p.entityId = ev.entityId;
            p.event = ev.event;

            const auto payload = Network::Serialization::Serialize(p);
            for (uint32_t connId : it->second.watchers) {
                EmitTo(connId, Network::PacketId::EntityEventS2C, payload,
                       EntityPacketOut::Kind::Event, out);
            }
        }
        events.clear();
    }

    void ServerEntityTracker::RemoveEntity(int32_t entityId, std::vector<EntityPacketOut>& out) {
        const auto it = m_tracked.find(entityId);
        if (it == m_tracked.end()) return;

        Network::RemoveEntitiesS2CPacket removal;
        removal.entityIds.push_back(entityId);
        const auto payload = Network::Serialization::Serialize(removal);

        for (uint32_t connId : it->second.watchers) {
            EmitTo(connId, Network::PacketId::EntityDestroy, payload,
                   EntityPacketOut::Kind::Remove, out);
        }
        m_tracked.erase(it);
    }

    void ServerEntityTracker::RemovePlayer(uint32_t connectionId) {
        for (auto& [id, tracked] : m_tracked) tracked.watchers.erase(connectionId);
    }

    void ServerEntityTracker::Clear() { m_tracked.clear(); }

} // namespace Server
