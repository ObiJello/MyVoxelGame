// File: src/game/ChunkProvider.cpp
#include "ChunkProvider.hpp"
#include "Log.hpp"
#include <memory>

#include "JobSystem.hpp"

namespace Game {

    // Single global noise generator. We’ll initialize it once.
    static FastNoiseLite s_noise;

    void ChunkProvider::RequestChunk(Math::ChunkPos pos) {
        static bool s_initialized = false;
        if (!s_initialized) {
            s_noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
            s_noise.SetSeed(1337);
            s_initialized = true;
        }

        // Use shared_ptr so the Chunk stays alive until all jobs finish
        auto chunk = std::make_shared<Chunk>();
        chunk->pos = pos;

        // First job: fill block data asynchronously
        JobSystem::g_ThreadPool.Enqueue([chunk]() {
            int baseWorldX = chunk->pos.x * Math::CHUNK_SIZE_X;
            int baseWorldZ = chunk->pos.z * Math::CHUNK_SIZE_Z;

            // Generate heights and fill blocks
            for (int localX = 0; localX < Math::CHUNK_SIZE_X; ++localX) {
                for (int localZ = 0; localZ < Math::CHUNK_SIZE_Z; ++localZ) {
                    int worldX = baseWorldX + localX;
                    int worldZ = baseWorldZ + localZ;

                    float n = s_noise.GetNoise((float)worldX, (float)worldZ);
                    float f = (n + 1.0f) * 0.5f;
                    int height = static_cast<int>(f * 32.0f + 64.0f);
                    height = std::clamp(height, 0, 255);

                    for (int y = 0; y < height; ++y) {
                        BlockID id = (y == height - 1) ? BlockID::Grass : BlockID::Stone;
                        chunk->SetBlock(localX, y, localZ, id);
                    }
                }
            }

            // Now that blocks exist, enqueue MesherJob for each non‐empty section
            for (int s = 0; s < Math::SECTIONS_PER_CHUNK; ++s) {
                if (chunk->sections[s]) {
                    int sectionIndex = s;
                    ChunkSection* sectionPtr = chunk->sections[s].get();

                    // Allocate MeshData and fill its metadata
                    auto* meshData = new MeshData();
                    meshData->chunkXZ     = { chunk->pos.x, chunk->pos.z };
                    meshData->sectionIndex = sectionIndex;

                    // Enqueue the actual meshing work
                    JobSystem::g_ThreadPool.Enqueue([sectionPtr, meshData]() {
                        MesherJob(sectionPtr, meshData);
                    });
                }
            }

            Log::Info("Chunk (%d, %d) generated → meshing enqueued", chunk->pos.x, chunk->pos.z);
            // When all lambdas capturing `chunk` finish, `chunk` will be freed automatically
        });
    }

} // namespace Game
