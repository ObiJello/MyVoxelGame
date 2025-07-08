

// File: src/engine/world/MinecraftChunkLoader.cpp
#include "MinecraftChunkLoader.hpp"
#include "ChunkProvider.hpp"
#include "../../core/Log.hpp"
#include "../../core/Config.hpp"
#include <filesystem>

// Include the external FastNoiseLite for terrain generation fallback
#include "../../../ext/FastNoiseLite.h"
#include "RegionDumper.hpp"

namespace Game {

    std::string MinecraftChunkLoader::s_worldPath = "";

    std::shared_ptr<Chunk> MinecraftChunkLoader::LoadOrGenerateChunk(Math::ChunkPos pos,
                                                                    const std::string& worldPath) {
        std::string effectiveWorldPath = worldPath.empty() ? s_worldPath : worldPath;

        // Try loading from Minecraft region files first
        if (!effectiveWorldPath.empty()) {
            auto minecraftChunk = LoadMinecraftChunk(pos, effectiveWorldPath);
            if (minecraftChunk) {
                Log::Info("Loaded chunk (%d, %d) from Minecraft region files", pos.x, pos.z);
                return minecraftChunk;
            }
        }

        // Fall back to procedural generation
        Log::Debug("Generating chunk (%d, %d) procedurally (no region data found)", pos.x, pos.z);
        return GenerateChunk(pos);
    }

    std::shared_ptr<Chunk> MinecraftChunkLoader::LoadMinecraftChunk(Math::ChunkPos pos,
                                                                   const std::string& worldPath) {
        if (worldPath.empty()) {
            Log::Warning("No world path provided for Minecraft chunk loading");
            return nullptr;
        }

        // Load chunk NBT from region file
        auto chunkNBT = LoadChunkNBT(pos, worldPath);
        if (!chunkNBT) {
            Log::Debug("No NBT data found for chunk (%d, %d) in world %s",
                      pos.x, pos.z, worldPath.c_str());
            return nullptr;
        }

        // Create new chunk
        auto chunk = std::make_shared<Chunk>();
        chunk->pos = pos;

        // Unpack section data from NBT
        if (!SectionDataUnpacker::UnpackChunkSections(chunkNBT, *chunk)) {
            Log::Warning("Failed to unpack section data for chunk (%d, %d)", pos.x, pos.z);
            return nullptr;
        }

        Log::Info("Successfully loaded Minecraft chunk (%d, %d) with real block data", pos.x, pos.z);
        return chunk;
    }

    std::shared_ptr<Chunk> MinecraftChunkLoader::GenerateChunk(Math::ChunkPos pos) {
        // Use the existing procedural generation from ChunkProvider
        // This is the same code that was in ChunkProvider::RequestChunk()

        static FastNoiseLite s_noise;
        static bool s_initialized = false;
        if (!s_initialized) {
            s_noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
            s_noise.SetSeed(1337);
            s_initialized = true;
        }

        auto chunk = std::make_shared<Chunk>();
        chunk->pos = pos;

        // Generate terrain data using noise
        int baseWorldX = pos.x * Math::CHUNK_SIZE_X;
        int baseWorldZ = pos.z * Math::CHUNK_SIZE_Z;

        for (int localX = 0; localX < Math::CHUNK_SIZE_X; ++localX) {
            for (int localZ = 0; localZ < Math::CHUNK_SIZE_Z; ++localZ) {
                int worldX = baseWorldX + localX;
                int worldZ = baseWorldZ + localZ;

                float n = s_noise.GetNoise(static_cast<float>(worldX), static_cast<float>(worldZ));
                float f = (n + 1.0f) * 0.5f;

                // Generate terrain that properly accounts for negative Y
                int height = static_cast<int>(f * 32.0f + 64.0f);
                height = std::clamp(height, Config::MinY, Config::MaxY);

                // Generate bedrock layer at the bottom
                for (int worldY = Config::MinY; worldY < Config::MinY + 5; ++worldY) {
                    chunk->SetBlock(localX, worldY, localZ, BlockID::Bedrock);
                }

                // Generate stone and surface blocks
                for (int worldY = Config::MinY + 5; worldY < height; ++worldY) {
                    BlockID id = (worldY == height - 1) ? BlockID::Grass : BlockID::Stone;
                    chunk->SetBlock(localX, worldY, localZ, id);
                }
            }
        }

        return chunk;
    }

    bool MinecraftChunkLoader::ChunkExistsInRegion(Math::ChunkPos pos, const std::string& worldPath) {
        if (worldPath.empty()) {
            return false;
        }

        // Convert to region coordinates
        int regionX, regionZ, localX, localZ;
        ChunkToRegion(pos, regionX, regionZ, localX, localZ);

        // Try to get region file
        auto regionFile = World::RegionFileCache::Instance().GetRegionFile(regionX, regionZ, worldPath);
        if (!regionFile || !regionFile->IsValid()) {
            return false;
        }

        // Check if chunk location is non-empty
        auto location = regionFile->GetLocation(localX, localZ);
        return !location.isEmpty();
    }

    void MinecraftChunkLoader::ChunkToRegion(Math::ChunkPos chunkPos, int& regionX, int& regionZ,
                                           int& localX, int& localZ) {
        // Region coordinates are chunk coordinates divided by 32
        regionX = chunkPos.x >> 5;  // Divide by 32 (arithmetic right shift)
        regionZ = chunkPos.z >> 5;

        // Handle negative coordinates properly
        if (chunkPos.x < 0 && (chunkPos.x & 31) != 0) regionX--;
        if (chunkPos.z < 0 && (chunkPos.z & 31) != 0) regionZ--;

        // Local coordinates within region (0-31)
        localX = chunkPos.x - (regionX << 5);  // chunkX - regionX * 32
        localZ = chunkPos.z - (regionZ << 5);  // chunkZ - regionZ * 32
    }

    World::NBTTagPtr MinecraftChunkLoader::LoadChunkNBT(Math::ChunkPos pos, const std::string& worldPath) {
        try {
            // Convert to region coordinates
            int regionX, regionZ, localX, localZ;
            ChunkToRegion(pos, regionX, regionZ, localX, localZ);

            Log::Debug("Loading chunk (%d, %d) from region (%d, %d) local (%d, %d)",
                      pos.x, pos.z, regionX, regionZ, localX, localZ);

            // Get region file from cache
            auto regionFile = World::RegionFileCache::Instance().GetRegionFile(regionX, regionZ, worldPath);
            if (!regionFile || !regionFile->IsValid()) {
                Log::Debug("No valid region file found for region (%d, %d)", regionX, regionZ);
                return nullptr;
            }

            // Check if chunk exists
            auto location = regionFile->GetLocation(localX, localZ);
            if (location.isEmpty()) {
                Log::Debug("Chunk (%d, %d) is empty in region file", localX, localZ);
                return nullptr;
            }

            // Read chunk data using RegionDumper
            auto chunkData = World::RegionDumper::ReadChunkData(*regionFile, localX, localZ);
            if (!chunkData.isValid || !chunkData.rootTag) {
                Log::Warning("Failed to read or parse chunk data for (%d, %d)", localX, localZ);
                return nullptr;
            }

            Log::Debug("Successfully loaded NBT for chunk (%d, %d): %zu bytes uncompressed",
                      pos.x, pos.z, chunkData.uncompressedData.size());

            return chunkData.rootTag;

        } catch (const std::exception& e) {
            Log::Error("Exception loading chunk NBT for (%d, %d): %s", pos.x, pos.z, e.what());
            return nullptr;
        }
    }

} // namespace Game