// File: src/client/world/BlockStatePrediction.cpp
#include "BlockStatePrediction.hpp"
#include "common/core/Log.hpp"

namespace Client {

    void BlockStatePrediction::Retain(const glm::ivec3& pos,
                                      Game::BlockID knownServerState,
                                      uint32_t sequence) {
        const int64_t key = PackPos(pos);
        auto it = m_records.find(key);
        if (it != m_records.end()) {
            // MC's compute() keeps the existing ServerVerifiedState and only
            // bumps its sequence — the FIRST retained state is the one the
            // server actually knows about, so overwriting it here would make a
            // rejected two-step prediction roll back to the intermediate
            // (also-predicted) block instead of the real one.
            it->second.sequence = sequence;
            return;
        }
        m_records.emplace(key, Record{sequence, knownServerState});
    }

    bool BlockStatePrediction::RecordServerState(const glm::ivec3& pos, Game::BlockID state) {
        auto it = m_records.find(PackPos(pos));
        if (it == m_records.end()) return false;
        it->second.serverState = state;
        return true;
    }

    void BlockStatePrediction::EndPredictionsUpTo(uint32_t sequence) {
        if (m_records.empty()) return;

        for (auto it = m_records.begin(); it != m_records.end(); ) {
            if (it->second.sequence > sequence) {
                ++it;
                continue;
            }

            const glm::ivec3 pos = UnpackPos(it->first);
            const Game::BlockID serverState = it->second.serverState;
            it = m_records.erase(it);

            // MC ClientLevel.syncBlockState: only touch the world when the
            // server disagrees. A correct prediction must cost nothing — see
            // the header for why re-applying would cause pointless remeshes.
            if (!m_read || !m_apply) continue;
            if (m_read(pos) == serverState) continue;

            Log::Debug("[Prediction] Rollback at (%d,%d,%d) -> block %u",
                       pos.x, pos.y, pos.z, static_cast<unsigned>(serverState));
            m_apply(pos, serverState);

            // TODO(parity): MC also un-clips the player here — syncBlockState
            // calls player.absSnapTo(playerPos) with the position recorded at
            // prediction time when the restored block would now intersect them
            // (you predicted a block away, walked into the space, and the
            // server said no). We have no player handle at this layer; the
            // physics stuck-in-block escape in Physics.cpp handles the fallout
            // less gracefully but does prevent a hard stick.
        }
    }

} // namespace Client
