// File: src/server/world/storage/MinecraftChunkLoader.hpp
#pragma once

#include "common/world/math/WorldMath.hpp"
#include "RegionFileCache.hpp"
#include "SectionDataUnpacker.hpp"
#include "NBTParser.hpp"
#include <string>
#include <memory>

namespace Game {

    // Enhanced chunk loader that can load from both generated terrain and Minecraft region files
    class MinecraftChunkLoader {
    public:
        // Load chunk from Minecraft region file if available, otherwise generate
        static std::shared_ptr<Chunk> LoadOrGenerateChunk(Math::ChunkPos pos,
                                                          const std::string& worldPath = "");

        // Load chunk specifically from Minecraft region files
        static std::shared_ptr<Chunk> LoadMinecraftChunk(Math::ChunkPos pos,
                                                        const std::string& worldPath);

        // Generate chunk using built-in terrain generator (fallback)
        static std::shared_ptr<Chunk> GenerateChunk(Math::ChunkPos pos);

        // Check if a chunk exists in Minecraft region files
        static bool ChunkExistsInRegion(Math::ChunkPos pos, const std::string& worldPath);

        // Set world path for automatic chunk loading
        static void SetWorldPath(const std::string& path);
        static const std::string& GetWorldPath() { return s_worldPath; }

    private:
        static std::string s_worldPath;

        // Convert chunk coordinates to region coordinates
        static void ChunkToRegion(Math::ChunkPos chunkPos, int& regionX, int& regionZ,
                                 int& localX, int& localZ);

        // Load chunk NBT from region file
        static World::NBTTagPtr LoadChunkNBT(Math::ChunkPos pos, const std::string& worldPath);
    };

} // namespace Game