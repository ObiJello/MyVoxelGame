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
#include "common/world/math/WorldMath.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Game { class Mob; }

namespace Server {

    class ChunkTicketManager;

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
        // One recipient of entity updates, with everything MC's
        // ChunkMap.TrackedEntity.updatePlayer reads about them.
        struct TrackedPlayer {
            uint32_t   connectionId = 0;
            glm::dvec3 position{0.0};
            // MC ChunkMap.getPlayerViewDistance — the tracking range is
            // clamped to it, so a player on render distance 4 does not receive
            // updates for entities 128 blocks away.
            int        viewDistance = 8;
            // MC ChunkMap.isChunkTracked: the chunk must be in the player's
            // tracking view AND no longer pending in their chunk sender. Our
            // PlayerSession::m_sentChunks is exactly that set — a chunk lands
            // in it at the moment its ChunkDataS2C actually goes out.
            //
            // Null disables the gate (used by anything constructing a tracker
            // without sessions).
            const std::unordered_set<Game::Math::ChunkPos,
                                     Game::Math::ChunkPosHash>* sentChunks = nullptr;
        };

        // `tickets` answers MC's DistanceManager.inEntityTickingRange, read
        // LIVE per entity from its own chunk — not from a precomputed set. Null
        // disables the gate (tests), which fails open rather than closed.
        void Tick(const MobManager& mobs, ServerLevelBridge& level,
                  const std::vector<TrackedPlayer>& players,
                  const ChunkTicketManager* tickets,
                  std::vector<EntityPacketOut>& out);

        // Drain the entity events the entity system raised and emit them to
        // each event's watchers. Split out of Tick() so IntegratedServer can
        // flush BEFORE this tick's removals go out: an event raised in the
        // same tick its entity is discarded (creeper explosion, projectile
        // impact) must be emitted while the entity is still tracked —
        // RemoveEntity erases the watcher set this flush needs, and the
        // client drops events for entities it has already removed.
        void FlushEntityEvents(ServerLevelBridge& level,
                               std::vector<EntityPacketOut>& out);

        // A player disconnected — forget everything they were tracking, so a
        // reconnecting id does not inherit a stale watch set.
        void RemovePlayer(uint32_t connectionId);

        // An entity is gone. Emits removals to everyone tracking it.
        void RemoveEntity(int32_t entityId, std::vector<EntityPacketOut>& out);
        // Same, for a whole tick's removals: one packet per watcher.
        void RemoveEntities(const std::vector<int32_t>& entityIds,
                            std::vector<EntityPacketOut>& out);

        void Clear();

    private:
        struct Tracked {
            // Last position SENT, in 1/4096 units — the delta base.
            int64_t baseX = 0, baseY = 0, baseZ = 0;
            int8_t  lastYRot = 0, lastXRot = 0, lastYHeadRot = 0;
            bool    wasOnGround = false;

            int  tickCount = 0;
            // MC ChunkMap.tick's `sectionPosChanged` — an entity that crossed
            // a chunk boundary gets one update even when it sits outside the
            // entity-ticking range, so watchers see it arrive rather than
            // teleport in later.
            int  lastChunkX = INT32_MIN;
            int  lastChunkZ = INT32_MIN;
            int  teleportDelay = 0;

            // MC ServerEntity.wasRiding: while an entity is a passenger no
            // position packets are sent (the client derives its position from
            // the vehicle via PositionRider), but the delta base keeps
            // advancing silently; the FIRST update after dismounting is forced
            // to a full-precision EntityPositionSync so both sides re-agree.
            bool wasRiding = false;

            // Last synced data payload, so SetEntityData only goes out on change.
            float   lastHealth = -1.0f;
            uint8_t lastFlags = 0xFF;
            uint8_t lastVariant = 0xFF;
            uint8_t lastHurtTime = 0xFF;
            uint8_t lastDeathTime = 0xFF;
            uint8_t lastSwell = 0xFF;
            uint8_t lastPose = 0xFF;
            uint8_t lastAnimState = 0xFF;
            uint32_t lastCarriedBlock = 0xFFFFFFFFu;   // Mob::GetCarriedBlockRaw
            // Sentinel distinct from every real value (-1 means "no vehicle"),
            // so the first data send always carries the riding link.
            int32_t lastVehicleId = INT32_MIN;
            float   lastScale = -1.0f;

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
        // Connection ids seen last tick, so the per-mob "drop watchers who
        // left" walk runs only on the tick the player set actually changed.
        std::vector<uint32_t> m_lastPlayerIds;
    };

} // namespace Server
