// File: src/client/world/ClientBlockAccess.hpp
#pragma once

#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/core/JavaRandom.hpp"

namespace Game { class BlockEntity; class ClientPlayer; }

namespace Client {

    class ClientChunkManager;

    // IBlockAccess implementation backed by the client chunk cache.
    // Used for physics and raycasting when connected to a remote server
    // (no server-side World available in the same process).
    class ClientBlockAccess : public Game::ILevelWrite {
    public:
        // Reads the given chunk manager — its own level's, never the
        // global, so a level renders and collides correctly whichever level
        // the globals are bound to (see ClientLevel.hpp).
        explicit ClientBlockAccess(ClientChunkManager* chunks) : m_chunks(chunks) {}
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
        Game::BlockState GetBlockState(int worldX, int worldY, int worldZ) const override;
        uint16_t GetBiome(int worldX, int worldY, int worldZ) const override;
        // The chunk's light as the server sent it.
        int GetBrightness(Game::Lighting::LightLayer layer, int worldX, int worldY, int worldZ) const override;
        bool SetBlock(int worldX, int worldY, int worldZ,
                      Game::BlockID blockId, uint32_t updateFlags) override;
        // Prediction write carrying the block-state index, so a predicted
        // furnace shows the right facing immediately instead of snapping when
        // the server's block change lands.
        bool SetBlock(int worldX, int worldY, int worldZ,
                      Game::BlockID blockId, uint32_t updateFlags,
                      Game::BlockStateIndex stateIndex) override;
        using Game::ILevelWrite::SetBlock;
        bool IsChunkLoaded(int chunkX, int chunkZ) const override;
        bool IsPositionLoaded(int worldX, int worldY, int worldZ) const override;
        bool IsBlockSolid(int worldX, int worldY, int worldZ) const override;
        bool IsBlockFluid(int worldX, int worldY, int worldZ) const override;
        bool IsValidPosition(int worldX, int worldY, int worldZ) const override;
        bool IsClientSide() const override { return true; }

        // MC ClientLevel.playSeededSound: play only what THIS client's player
        // caused (`except` is the local player — the item-use prediction's
        // player); everything else arrives from the server as a sound packet.
        // See common/sound/LevelSound.hpp.
        void PlaySeededSound(const Game::SoundExcept& except, const glm::dvec3& pos,
                             std::string_view event, Game::SoundSource source,
                             float volume, float pitch, int64_t seed) override;
        // MC ClientLevel.playLocalSound — animateTick sounds and the like.
        void PlayLocalSound(const glm::dvec3& pos, std::string_view event, Game::SoundSource source,
                            float volume, float pitch, bool distanceDelay) override;
        using Game::ILevelWrite::PlayLocalSound;

        // ── MC ClientLevel's block-event / block-entity half ──────────────
        //
        // Writes outside a prediction scope are refused (see SetBlock), which
        // is right for item behaviours. A block EVENT is different: vanilla's
        // client runs PistonBaseBlock.triggerEvent against its own level and
        // writes the moving cells itself. LocalWriteScope opens that door for
        // the duration of one event or one client block-entity tick.
        class LocalWriteScope {
        public:
            explicit LocalWriteScope(ClientBlockAccess& a) : m_a(a), m_prev(a.m_localWrites) { a.m_localWrites = true; }
            ~LocalWriteScope() { m_a.m_localWrites = m_prev; }
        private:
            ClientBlockAccess& m_a;
            bool m_prev;
        };

        Game::BlockEntity* GetBlockEntity(const glm::ivec3& pos) override;
        void SetBlockEntity(const glm::ivec3& pos, std::unique_ptr<Game::BlockEntity> entity) override;
        void RemoveBlockEntity(const glm::ivec3& pos) override;
        int64_t GameTime() const override;
        Game::JavaRandom* Random() override { return &m_random; }

        // The local player, for the piston tick (ILevelWrite hooks).
        void SetLocalPlayer(Game::ClientPlayer* player) { m_player = player; }
        bool GetLocalPlayerBox(glm::dvec3& outMin, glm::dvec3& outMax) const override;
        void MoveLocalPlayerByPiston(const glm::dvec3& delta) override;
        // ... and for the geyser's launch (PotentSulfurBlockEntity).
        bool GetLocalPlayerMovement(glm::dvec3& outDeltaMovement, bool& outFlying) const override;
        void AddLocalPlayerDeltaMovement(const glm::dvec3& delta) override;
        void CheckLocalPlayerFallDistanceAccumulation() override;

        // MC ClientLevel.blockEvent → BlockState.triggerEvent, run at once.
        void BlockEvent(const glm::ivec3& pos, Game::BlockID block, int b0, int b1) override;
        // MC ClientLevel.addParticle, for block code (a spawner's client tick
        // and its spawn burst): into the mob particle system's queue.
        void AddParticle(Game::ParticleKind kind, double x, double y, double z,
                         double vx, double vy, double vz) override;
        // Section-flag answers for the two hot physics queries, so the client
        // simulation of a hundred thousand primed TNT pays one chunk lookup per
        // gather rather than one per cell (see IBlockAccess).
        bool IsRegionAllAir(const glm::ivec3& min, const glm::ivec3& max,
                            bool absentIsAir = false) const override;
        void GetBlockStatesInBox(const glm::ivec3& min, const glm::ivec3& max,
                                 Game::BlockState* out) const override;
        uint64_t RegionWriteStamp(const glm::ivec3& min, const glm::ivec3& max) const override;

    private:
        ClientChunkManager* m_chunks = nullptr;
        uint32_t m_sequence   = 0;
        bool     m_predicting = false;
        bool     m_localWrites = false;
        Game::ClientPlayer* m_player = nullptr;
        Game::JavaRandom    m_random{0};
    };

    // Process-wide client level. Created in BOTH modes (see PlatformMain):
    // the integrated host needs it too, because prediction always targets the
    // CLIENT chunk cache — that is what the renderer meshes and what the
    // remote raycast reads — never the server World.
    extern ClientBlockAccess* g_clientBlockAccess;

} // namespace Client
