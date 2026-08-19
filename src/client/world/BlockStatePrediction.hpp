// File: src/client/world/BlockStatePrediction.hpp
//
// Client-side block prediction bookkeeping — a direct port of MC's
// client/multiplayer/prediction/BlockStatePredictionHandler.
//
// WHY THIS EXISTS
// ---------------
// Without it, a client on a remote server sees nothing happen when it breaks or
// places a block until the server's BlockChangeS2C echo comes back — one full
// round trip of dead air. MC hides that latency by applying the change locally
// the instant you click and reconciling afterwards. The naive version of that
// ("just SetBlock locally and let the echo overwrite it") is wrong in two ways
// this class fixes:
//
//   1. While a prediction is outstanding, unrelated server updates for that
//      same position must NOT stomp the predicted block — they'd undo the
//      prediction for the rest of the round trip, which looks like the block
//      flickering back. Instead the incoming state is filed against the
//      prediction record (RecordServerState) and applied only when the
//      prediction retires.
//   2. When the prediction retires we must only touch the world if the server
//      disagrees with us. A correct prediction has to be a literal no-op, or
//      every placement would re-dirty its section and re-mesh for nothing.
//
// THE CONTRACT WITH THE SERVER
// ----------------------------
// Every predicted action carries a monotonically increasing sequence number.
// The server replies with BlockChangedAckS2C(sequence) *after* it has sent any
// block updates caused by that action. On ack we retire every record at or
// below that sequence: whatever state the server last told us for that position
// is the truth, and we snap to it if it differs from what we predicted.
//
// See ClientChunkManager, which owns one of these (MC hangs it off ClientLevel).
#pragma once

#include "common/world/block/Blocks.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>

namespace Client {

    class BlockStatePrediction {
    public:
        // Applies an authoritative block state to the live client world.
        // Supplied by the owner (ClientChunkManager) so this class stays free
        // of chunk-storage details.
        // `stateIndex` is the block-state index (MC BlockState.getId()); it must
        // travel with the BlockID everywhere in here, or rolling back a rejected
        // prediction would restore the right block facing the wrong way.
        using ApplyFn = std::function<void(const glm::ivec3& pos, Game::BlockID state, Game::BlockStateIndex stateIndex)>;
        // Reads the live client world — used to skip no-op reconciliations.
        using ReadFn  = std::function<std::pair<Game::BlockID, Game::BlockStateIndex>(const glm::ivec3& pos)>;

        void SetHooks(ApplyFn apply, ReadFn read) {
            m_apply = std::move(apply);
            m_read  = std::move(read);
        }

        // MC: BlockStatePredictionHandler.retainKnownServerState.
        // Call with the state the server last had for `pos` (i.e. the block
        // that was there *before* we predicted over it) BEFORE applying the
        // prediction. Re-predicting the same position just refreshes the
        // sequence — the originally retained state is kept, so a burst of
        // predictions on one block still rolls all the way back if the server
        // rejects the whole burst.
        void Retain(const glm::ivec3& pos, Game::BlockID knownServerState,
                    Game::BlockStateIndex knownServerStateIndex, uint32_t sequence);

        // MC: BlockStatePredictionHandler.updateKnownServerState.
        // Returns true when `pos` has an outstanding prediction — the caller
        // must then NOT write to the live world; the state is filed here and
        // applied (if still needed) when the prediction retires.
        bool RecordServerState(const glm::ivec3& pos, Game::BlockID state, Game::BlockStateIndex stateIndex);

        // MC: BlockStatePredictionHandler.endPredictionsUpTo.
        // Retires every record with sequence <= `sequence`, snapping the world
        // back to the server's state wherever our prediction was wrong.
        void EndPredictionsUpTo(uint32_t sequence);

        // Drop everything without touching the world — for world teardown /
        // reconnect, where the chunk store is about to be thrown away anyway.
        void Clear() { m_records.clear(); }

        bool Empty() const { return m_records.empty(); }
        size_t PendingCount() const { return m_records.size(); }

    private:
        struct Record {
            uint32_t      sequence = 0;
            Game::BlockID serverState = Game::BlockID::Air;
            Game::BlockStateIndex       serverStateIndex = 0;
        };

        // Block positions pack into a single key the same way MC uses
        // BlockPos.asLong(). Y is only 9 bits of range (-64..319) but we keep
        // the full 12 for headroom; X/Z get 26 bits each, matching MC's limits.
        static int64_t PackPos(const glm::ivec3& pos) {
            return ((static_cast<int64_t>(pos.x) & 0x3FFFFFF) << 38)
                 | ((static_cast<int64_t>(pos.z) & 0x3FFFFFF) << 12)
                 |  (static_cast<int64_t>(pos.y) & 0xFFF);
        }
        static glm::ivec3 UnpackPos(int64_t key) {
            auto sign = [](int64_t v, int bits) {
                const int64_t m = int64_t(1) << (bits - 1);
                return static_cast<int>((v ^ m) - m);
            };
            return glm::ivec3(sign((key >> 38) & 0x3FFFFFF, 26),
                              sign(key & 0xFFF, 12),
                              sign((key >> 12) & 0x3FFFFFF, 26));
        }

        std::unordered_map<int64_t, Record> m_records;
        ApplyFn m_apply;
        ReadFn  m_read;
    };

} // namespace Client
