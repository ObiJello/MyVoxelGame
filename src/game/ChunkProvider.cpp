// File: src/game/ChunkProvider.cpp
#include "ChunkProvider.hpp"
#include "Log.hpp"
#include <memory>
#include <algorithm>
#include "JobSystem.hpp"

namespace Game {

    // Single global noise generator. We'll initialize it once.
    static FastNoiseLite s_noise;

    // Global storage for chunks to prevent premature deallocation
    static std::unordered_map<uint64_t, std::shared_ptr<Chunk>> s_chunkCache;
    static std::mutex s_chunkCacheMutex;

    // Helper function to create a unique key from chunk coordinates
    static uint64_t MakeChunkKey(int32_t x, int32_t z) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
               static_cast<uint32_t>(z);
    }

    void ChunkProvider::RequestChunk(Math::ChunkPos pos) {
        static bool s_initialized = false;
        if (!s_initialized) {
            s_noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
            s_noise.SetSeed(1337);
            s_initialized = true;
        }

        // Check if chunk already exists
        uint64_t key = MakeChunkKey(pos.x, pos.z);
        {
            std::lock_guard<std::mutex> lock(s_chunkCacheMutex);
            if (s_chunkCache.find(key) != s_chunkCache.end()) {
                Log::Debug("Chunk (%d, %d) already exists, skipping", pos.x, pos.z);
                return;
            }
        }

        // Create and store chunk in cache
        auto chunk = std::make_shared<Chunk>();
        chunk->pos = pos;

        {
            std::lock_guard<std::mutex> lock(s_chunkCacheMutex);
            s_chunkCache[key] = chunk;
        }

        // First job: fill block data asynchronously
        JobSystem::g_ThreadPool.Enqueue([chunk, pos]() {
            Log::Debug("Starting generation for chunk (%d, %d)", pos.x, pos.z);

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

            Log::Debug("Block generation complete for chunk (%d, %d)", pos.x, pos.z);

            // Now that blocks exist, enqueue MesherJob for each non-empty section
            int meshJobsEnqueued = 0;
            for (int s = 0; s < Math::SECTIONS_PER_CHUNK; ++s) {
                if (chunk->sections[s]) {
                    // Allocate MeshData and fill its metadata
                    auto* meshData = new MeshData();
                    meshData->chunkXZ = { chunk->pos.x, chunk->pos.z };
                    meshData->sectionIndex = s;

                    // Get the section pointer while keeping chunk alive
                    ChunkSection* sectionPtr = chunk->sections[s].get();

                    Log::Debug("Enqueueing mesher job for chunk (%d, %d) section %d",
                              pos.x, pos.z, s);

                    // CRITICAL: Capture the chunk shared_ptr to prevent deallocation
                    JobSystem::g_ThreadPool.Enqueue([chunk, sectionPtr, meshData, pos, s]() {
                        try {
                            Log::Debug("Starting mesher job for chunk (%d, %d) section %d",
                                      pos.x, pos.z, s);
                            MesherJob(sectionPtr, meshData);
                            Log::Debug("Mesher job completed for chunk (%d, %d) section %d with %zu vertices",
                                      pos.x, pos.z, s, meshData->vertices.size());
                        } catch (const std::exception& e) {
                            Log::Error("MesherJob failed for chunk (%d, %d) section %d: %s",
                                      pos.x, pos.z, s, e.what());
                            delete meshData; // Clean up on failure
                        } catch (...) {
                            Log::Error("MesherJob failed for chunk (%d, %d) section %d with unknown exception",
                                      pos.x, pos.z, s);
                            delete meshData; // Clean up on failure
                        }
                    });
                    meshJobsEnqueued++;
                }
            }

            Log::Info("Chunk (%d, %d) generated → %d meshing jobs enqueued",
                     chunk->pos.x, chunk->pos.z, meshJobsEnqueued);
        });
    }

    void ChunkProvider::UnloadChunk(Math::ChunkPos pos) {
        uint64_t key = MakeChunkKey(pos.x, pos.z);
        std::lock_guard<std::mutex> lk(s_chunkCacheMutex);
        s_chunkCache.erase(key);
    }

} // namespace Game