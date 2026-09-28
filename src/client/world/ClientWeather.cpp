// File: src/client/world/ClientWeather.cpp
//
// See ClientWeather.hpp.
#include "client/world/ClientWeather.hpp"

#include "client/world/ClientChunkManager.hpp"
#include "client/world/ClientLevel.hpp"
#include "common/network/packets/game/GameEventS2CPacket.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/chunk/Heightmap.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/lighting/ChunkLight.hpp"   // Lighting::LightLayer
#include "common/world/math/WorldCoordinates.hpp"

#include <algorithm>

namespace Client::ClientWeather {

    namespace {

        // MC Level.oRainLevel / rainLevel / oThunderLevel / thunderLevel.
        // setRainLevel / setThunderLevel write both ends, and nothing else
        // moves them on the client, so o == current always; they are kept
        // apart anyway so the accessors read exactly like Level's lerps.
        float s_oRainLevel    = 0.0f;
        float s_rainLevel     = 0.0f;
        float s_oThunderLevel = 0.0f;
        float s_thunderLevel  = 0.0f;

        float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

        void SetRainLevel(float level) {
            s_oRainLevel = s_rainLevel = Clamp01(level);
        }
        void SetThunderLevel(float level) {
            s_oThunderLevel = s_thunderLevel = Clamp01(level);
        }

    } // namespace

    void OnGameEvent(uint8_t event, float param) {
        using P = Network::GameEventS2CPacket;
        switch (event) {
            case P::kStartRaining:       SetRainLevel(0.0f);    break;
            case P::kStopRaining:        SetRainLevel(1.0f);    break;
            case P::kRainLevelChange:    SetRainLevel(param);   break;
            case P::kThunderLevelChange: SetThunderLevel(param); break;
            default: break;
        }
    }

    void Reset() {
        s_oRainLevel = s_rainLevel = 0.0f;
        s_oThunderLevel = s_thunderLevel = 0.0f;
    }

    bool CanHaveWeather() {
        return Game::DimensionCanHaveWeather(ClientLevels::BoundDimension());
    }

    float RainLevelIn(Game::DimensionId dimension, float partialTick) {
        if (!Game::DimensionCanHaveWeather(dimension)) return 0.0f;
        return s_oRainLevel + partialTick * (s_rainLevel - s_oRainLevel);
    }

    float ThunderLevelIn(Game::DimensionId dimension, float partialTick) {
        if (!Game::DimensionCanHaveWeather(dimension)) return 0.0f;
        return (s_oThunderLevel + partialTick * (s_thunderLevel - s_oThunderLevel)) *
               RainLevelIn(dimension, partialTick);
    }

    float RainLevel(float partialTick) {
        return RainLevelIn(ClientLevels::BoundDimension(), partialTick);
    }

    float ThunderLevel(float partialTick) {
        return ThunderLevelIn(ClientLevels::BoundDimension(), partialTick);
    }

    bool IsRaining()    { return static_cast<double>(RainLevel(1.0f)) > 0.2; }
    bool IsThundering() { return static_cast<double>(ThunderLevel(1.0f)) > 0.9; }

    int MotionBlockingHeight(int worldX, int worldZ) {
        const int minY = Game::DimensionMinY(ClientLevels::BoundDimension());
        if (!g_clientChunkManager) return minY;
        const auto cpos = Game::Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        const ClientChunk* chunk = g_clientChunkManager->GetChunk(cpos);
        if (!chunk || !chunk->chunkData || !chunk->chunkData->AreHeightmapsPrimed()) return minY;
        const int lx = worldX - cpos.x * Game::Math::CHUNK_SIZE_X;
        const int lz = worldZ - cpos.z * Game::Math::CHUNK_SIZE_Z;
        return chunk->chunkData->GetHeightmap(Game::HeightmapType::MotionBlocking).GetFirstAvailable(lx, lz);
    }

    int PrecipitationAt(const Game::IBlockAccess& blocks, const glm::ivec3& pos) {
        // MC Level.precipitationAt, in its order.
        if (!IsRaining()) return 0;
        if (blocks.GetBrightness(Game::Lighting::LightLayer::Sky, pos.x, pos.y, pos.z) < 15) return 0;
        if (MotionBlockingHeight(pos.x, pos.z) > pos.y) return 0;
        const Game::BiomeId biome = static_cast<Game::BiomeId>(blocks.GetBiome(pos.x, pos.y, pos.z));
        return static_cast<int>(Game::BiomeRegistry::PrecipitationAt(
            biome, pos.x, pos.y, pos.z, Game::DimensionSeaLevel(ClientLevels::BoundDimension())));
    }

    int BiomePrecipitationAt(const Game::IBlockAccess& blocks, const glm::ivec3& pos) {
        // MC ClientLevel.getPrecipitationAt: chunkSource.hasChunk, then the
        // biome's rule at that height.
        if (!g_clientChunkManager) return 0;
        const auto cpos = Game::Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
        const ClientChunk* chunk = g_clientChunkManager->GetChunk(cpos);
        if (!chunk || !chunk->chunkData) return 0;
        const Game::BiomeId biome = static_cast<Game::BiomeId>(blocks.GetBiome(pos.x, pos.y, pos.z));
        return static_cast<int>(Game::BiomeRegistry::PrecipitationAt(
            biome, pos.x, pos.y, pos.z, Game::DimensionSeaLevel(ClientLevels::BoundDimension())));
    }

} // namespace Client::ClientWeather
