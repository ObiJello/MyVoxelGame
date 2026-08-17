// File: src/client/world/ClientBlockAccess.hpp
#pragma once

#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/ILevelWrite.hpp"

namespace Client {

    // IBlockAccess implementation backed by the client chunk cache.
    // Used for physics and raycasting when connected to a remote server
    // (no server-side World available in the same process).
    class ClientBlockAccess : public Game::ILevelWrite {
    public:
        ClientBlockAccess() = default;
        ~ClientBlockAccess() override = default;

        // ── Prediction window ───────────────────────────────────────────
        // Mirrors MC BlockStatePredictionHandler's isPredicting/currentSequence
        // pair. While open, SetBlock routes writes into ClientChunkManager's
        // prediction handler under `sequence` so the server's
        // BlockChangedAckS2C can confirm or roll them back. Outside the
        // window SetBlock is a no-op — a client must never mutate its world
        // except as an explicit, reconcilable prediction.
        void BeginPrediction(uint32_t sequence) { m_sequence = sequence; m_predicting = true; }
        void EndPrediction()                    { m_predicting = false; }
        bool IsPredicting() const               { return m_predicting; }

        // ILevelWrite
        Game::BlockID GetBlock(int worldX, int worldY, int worldZ) const override;
        uint8_t GetBlockState(int worldX, int worldY, int worldZ) const override;
        uint16_t GetBiome(int worldX, int worldY, int worldZ) const override;
        bool SetBlock(int worldX, int worldY, int worldZ,
                      Game::BlockID blockId, uint32_t updateFlags) override;
        // Prediction write carrying the block-state index, so a predicted
        // furnace shows the right facing immediately instead of snapping when
        // the server's block change lands.
        bool SetBlock(int worldX, int worldY, int worldZ,
                      Game::BlockID blockId, uint32_t updateFlags,
                      uint8_t stateIndex) override;
        bool IsChunkLoaded(int chunkX, int chunkZ) const override;
        bool IsPositionLoaded(int worldX, int worldY, int worldZ) const override;
        bool IsBlockSolid(int worldX, int worldY, int worldZ) const override;
        bool IsBlockFluid(int worldX, int worldY, int worldZ) const override;
        bool IsValidPosition(int worldX, int worldY, int worldZ) const override;
        bool IsClientSide() const override { return true; }

    private:
        uint32_t m_sequence   = 0;
        bool     m_predicting = false;
    };

    // Process-wide client level. Created in BOTH modes (see PlatformMain):
    // the integrated host needs it too, because prediction always targets the
    // CLIENT chunk cache — that is what the renderer meshes and what the
    // remote raycast reads — never the server World.
    extern ClientBlockAccess* g_clientBlockAccess;

} // namespace Client
