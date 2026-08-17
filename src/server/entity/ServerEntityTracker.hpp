// File: src/server/entity/ServerEntityTracker.hpp
//
// MC net.minecraft.server.level.ServerEntity + ChunkMap.TrackedEntity.
//
// This is the piece Server::ItemEntityManager deliberately skipped (see its
// note about there being "no per-client tracked-entity set here"). Items could
// get away with a cheap periodic full re-send because they are static and
// cheap; mobs cannot — they move every tick, and a full re-send per mob per
// second would be both wasteful and visibly laggy.
//
// What a tracker owns per entity:
//
//   * WHO is watching it. A player who walks into range gets a full AddEntity
//     bundle; one who walks out gets a removal. Without this a joining player
//     would never learn about mobs already in the world.
//
//   * The DELTA BASE. Movement packets carry a 1/4096-block offset from the
//     last position the server SENT, not from the mob's current position, so
//     the two sides accumulate identically. The base only advances when a
//     position packet actually goes out.
//
// The three cadences, all MC's:
//   updateInterval (3)  how often a mob is even considered for a move packet
//   60 ticks            force a position packet even if nothing moved, so a
//                       client that dropped one self-heals
//   400 ticks           force a full-precision teleport, bounding accumulated
//                       rounding drift
#pragma once

#include "common/network/packets/game/MobEntityPackets.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Game { class Mob; }

namespace Server {

    class MobManager;
    class ServerLevelBridge;

    // What the tracker wants sent. Buffered rather than sent inline so the
    // tracker has no dependency on the connection layer — IntegratedServer
    // drains this and does the actual sending.
    struct EntityPacketOut {
        enum class Kind : uint8_t { Add, Move, PositionSync, Motion, Data, Event, Remove };

        Kind     kind = Kind::Move;
        uint32_t connectionId = 0;      // recipient
        std::vector<uint8_t> payload;   // already serialised
        Network::PacketId packetId = Network::PacketId::MoveEntityS2C;
    };

    class ServerEntityTracker {
    public:
        static constexpr int kForcedPosUpdatePeriod = 60;
        static constexpr int kForcedTeleportPeriod  = 400;
        // MC's VecDeltaCodec threshold: below this squared delta the position
        // is considered unchanged and no packet is sent.
        static constexpr double kPositionTolerance = 7.6293945e-6;
        // A short holds +-32767 in 1/4096 units. Anything larger must go as a
        // full-precision teleport instead.
        static constexpr int32_t kMaxDelta = 32767;

        // Compute everything that should go out this tick.
        void Tick(const MobManager& mobs, ServerLevelBridge& level,
                  const std::vector<std::pair<uint32_t, glm::dvec3>>& players,
                  std::vector<EntityPacketOut>& out);

        // A player disconnected — forget everything they were tracking, so a
        // reconnecting id does not inherit a stale watch set.
        void RemovePlayer(uint32_t connectionId);

        // An entity is gone. Emits removals to everyone tracking it.
        void RemoveEntity(int32_t entityId, std::vector<EntityPacketOut>& out);

        void Clear();

    private:
        struct Tracked {
            // Last position SENT, in 1/4096 units — the delta base.
            int64_t baseX = 0, baseY = 0, baseZ = 0;
            int8_t  lastYRot = 0, lastXRot = 0, lastYHeadRot = 0;
            bool    wasOnGround = false;

            int  tickCount = 0;
            int  teleportDelay = 0;

            // Last synced data payload, so SetEntityData only goes out on change.
            float   lastHealth = -1.0f;
            uint8_t lastFlags = 0xFF;
            uint8_t lastVariant = 0xFF;
            uint8_t lastHurtTime = 0xFF;
            uint8_t lastDeathTime = 0xFF;
            uint8_t lastSwell = 0xFF;
            uint8_t lastPose = 0xFF;
            uint8_t lastAnimState = 0xFF;

            std::unordered_set<uint32_t> watchers;
        };

        static uint8_t PackFlags(const Game::Mob& mob);
        static uint8_t VariantData(const Game::Mob& mob);
        static Network::AddEntityS2CPacket BuildAddPacket(const Game::Mob& mob,
                                                          const glm::dvec3& base);

        void EmitTo(uint32_t connectionId, Network::PacketId id,
                    std::vector<uint8_t> payload, EntityPacketOut::Kind kind,
                    std::vector<EntityPacketOut>& out);

        std::unordered_map<int32_t, Tracked> m_tracked;
    };

} // namespace Server
