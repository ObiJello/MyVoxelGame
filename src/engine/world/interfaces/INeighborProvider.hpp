// File: src/engine/world/interfaces/INeighborProvider.hpp
#pragma once

#include "../../../game/WorldCoordinates.hpp"
#include "../../../game/WorldMath.hpp"
#include "../../block/Blocks.hpp"
#include <vector>

#include "glm/vec3.hpp"

namespace Game {

    // Information about a block for neighbor queries
    struct BlockInfo {
        BlockID id = BlockID::Air;
        bool isLoaded = false;
        bool isSolid = false;
        bool isFluid = false;
        bool isTransparent = false;

        BlockInfo() = default;
        BlockInfo(BlockID blockId, bool loaded = true)
            : id(blockId), isLoaded(loaded) {
            // Set properties based on block ID
            UpdateProperties();
        }

    private:
        void UpdateProperties();  // Implementation would set solid/fluid/transparent flags
    };

    // Interface for accessing neighboring blocks during mesh generation and physics
    // This decouples the meshing system from the world implementation
    class INeighborProvider {
    public:
        virtual ~INeighborProvider() = default;

        // === BASIC BLOCK ACCESS ===

        // Get block at world coordinates
        virtual BlockID GetBlock(int worldX, int worldY, int worldZ) const = 0;

        // Get detailed block information
        virtual BlockInfo GetBlockInfo(int worldX, int worldY, int worldZ) const {
            BlockID id = GetBlock(worldX, worldY, worldZ);
            bool loaded = IsPositionLoaded(worldX, worldY, worldZ);
            return BlockInfo(id, loaded);
        }

        // Check if position is within valid world bounds
        virtual bool IsValidPosition(int worldX, int worldY, int worldZ) const {
            return Math::WorldCoordinates::IsValidWorldY(worldY);
        }

        // === CHUNK LOADING STATE ===

        // Check if a chunk is loaded
        virtual bool IsChunkLoaded(int chunkX, int chunkZ) const = 0;

        // Check if a specific position is in a loaded chunk
        virtual bool IsPositionLoaded(int worldX, int worldY, int worldZ) const = 0;

        // Get chunk position for world coordinates
        Math::ChunkPos GetChunkPos(int worldX, int worldZ) const {
            return Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        }

        // === BLOCK PROPERTIES ===

        // Check if block is solid (blocks light and movement)
        virtual bool IsBlockSolid(int worldX, int worldY, int worldZ) const = 0;

        // Check if block is fluid (water, lava)
        virtual bool IsBlockFluid(int worldX, int worldY, int worldZ) const = 0;

        // Check if block is transparent (doesn't block light)
        virtual bool IsBlockTransparent(int worldX, int worldY, int worldZ) const {
            BlockID id = GetBlock(worldX, worldY, worldZ);
            return id == BlockID::Air || id == BlockID::Glass || id == BlockID::Water;
        }

        // Check if block is opaque (blocks light completely)
        virtual bool IsBlockOpaque(int worldX, int worldY, int worldZ) const {
            return !IsBlockTransparent(worldX, worldY, worldZ);
        }

        // === NEIGHBOR QUERIES ===

        // Face directions for neighbor access
        enum class Direction {
            Up = 0,     // +Y
            Down = 1,   // -Y
            North = 2,  // -Z
            South = 3,  // +Z
            East = 4,   // +X
            West = 5    // -X
        };

        // Get neighbor block in a specific direction
        virtual BlockID GetNeighbor(int worldX, int worldY, int worldZ, Direction direction) const {
            int neighborX = worldX;
            int neighborY = worldY;
            int neighborZ = worldZ;

            switch (direction) {
                case Direction::Up:    neighborY++; break;
                case Direction::Down:  neighborY--; break;
                case Direction::North: neighborZ--; break;
                case Direction::South: neighborZ++; break;
                case Direction::East:  neighborX++; break;
                case Direction::West:  neighborX--; break;
            }

            return GetBlock(neighborX, neighborY, neighborZ);
        }

        // Get all 6 neighbors at once
        virtual std::array<BlockID, 6> GetAllNeighbors(int worldX, int worldY, int worldZ) const {
            std::array<BlockID, 6> neighbors;

            neighbors[0] = GetNeighbor(worldX, worldY, worldZ, Direction::Up);
            neighbors[1] = GetNeighbor(worldX, worldY, worldZ, Direction::Down);
            neighbors[2] = GetNeighbor(worldX, worldY, worldZ, Direction::North);
            neighbors[3] = GetNeighbor(worldX, worldY, worldZ, Direction::South);
            neighbors[4] = GetNeighbor(worldX, worldY, worldZ, Direction::East);
            neighbors[5] = GetNeighbor(worldX, worldY, worldZ, Direction::West);

            return neighbors;
        }

        // Check if neighbor exists and is solid (for face culling)
        virtual bool HasSolidNeighbor(int worldX, int worldY, int worldZ, Direction direction) const {
            int neighborX = worldX;
            int neighborY = worldY;
            int neighborZ = worldZ;

            switch (direction) {
                case Direction::Up:    neighborY++; break;
                case Direction::Down:  neighborY--; break;
                case Direction::North: neighborZ--; break;
                case Direction::South: neighborZ++; break;
                case Direction::East:  neighborX++; break;
                case Direction::West:  neighborX--; break;
            }

            return IsBlockSolid(neighborX, neighborY, neighborZ);
        }

        // === AREA QUERIES ===

        // Get blocks in a 3x3x3 area around a position (for ambient occlusion)
        virtual std::vector<BlockID> GetBlockArea3x3x3(int centerX, int centerY, int centerZ) const {
            std::vector<BlockID> blocks;
            blocks.reserve(27);

            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        blocks.push_back(GetBlock(centerX + dx, centerY + dy, centerZ + dz));
                    }
                }
            }

            return blocks;
        }

        // Get height of terrain at X,Z coordinates (for surface detection)
        virtual int GetTerrainHeight(int worldX, int worldZ) const {
            // Scan from top to bottom to find first solid block
            for (int y = Math::WorldCoordinates::MAX_WORLD_Y; y >= Math::WorldCoordinates::MIN_WORLD_Y; --y) {
                if (IsBlockSolid(worldX, y, worldZ)) {
                    return y;
                }
            }
            return Math::WorldCoordinates::MIN_WORLD_Y; // Bedrock level
        }

        // === LIGHTING QUERIES ===

        // Check if there's a clear path to sky (for sunlight)
        virtual bool HasSkyAccess(int worldX, int worldY, int worldZ) const {
            for (int y = worldY + 1; y <= Math::WorldCoordinates::MAX_WORLD_Y; ++y) {
                if (IsBlockOpaque(worldX, y, worldZ)) {
                    return false;
                }
            }
            return true;
        }

        // Count solid neighbors (for ambient occlusion calculation)
        virtual int CountSolidNeighbors(int worldX, int worldY, int worldZ) const {
            int count = 0;

            for (int i = 0; i < 6; ++i) {
                Direction dir = static_cast<Direction>(i);
                if (HasSolidNeighbor(worldX, worldY, worldZ, dir)) {
                    count++;
                }
            }

            return count;
        }

        // === PERFORMANCE OPTIMIZATION ===

        // Batch neighbor queries (more efficient for some implementations)
        struct NeighborQuery {
            int worldX, worldY, worldZ;
            Direction direction;
        };

        struct NeighborResult {
            BlockID blockId;
            bool isLoaded;
            bool isSolid;
        };

        virtual std::vector<NeighborResult> QueryNeighborsBatch(const std::vector<NeighborQuery>& queries) const {
            std::vector<NeighborResult> results;
            results.reserve(queries.size());

            for (const auto& query : queries) {
                BlockID id = GetNeighbor(query.worldX, query.worldY, query.worldZ, query.direction);

                int neighborX = query.worldX;
                int neighborY = query.worldY;
                int neighborZ = query.worldZ;

                switch (query.direction) {
                    case Direction::Up:    neighborY++; break;
                    case Direction::Down:  neighborY--; break;
                    case Direction::North: neighborZ--; break;
                    case Direction::South: neighborZ++; break;
                    case Direction::East:  neighborX++; break;
                    case Direction::West:  neighborX--; break;
                }

                NeighborResult result;
                result.blockId = id;
                result.isLoaded = IsPositionLoaded(neighborX, neighborY, neighborZ);
                result.isSolid = IsBlockSolid(neighborX, neighborY, neighborZ);

                results.push_back(result);
            }

            return results;
        }

        // === CACHING HINTS ===

        // Hint that certain areas will be accessed frequently (for caching)
        virtual void HintFrequentAccess(int centerX, int centerY, int centerZ, int radius) {
            // Default implementation does nothing, but implementations can use this for optimization
        }

        // Clear any cached neighbor data
        virtual void ClearNeighborCache() {
            // Default implementation does nothing
        }

        // === STATISTICS ===

        struct NeighborStats {
            size_t totalQueries = 0;
            size_t cacheHits = 0;
            size_t cacheMisses = 0;
            size_t unloadedQueries = 0;

            float GetCacheHitRate() const {
                return totalQueries > 0 ? static_cast<float>(cacheHits) / static_cast<float>(totalQueries) : 0.0f;
            }

            void Reset() {
                totalQueries = cacheHits = cacheMisses = unloadedQueries = 0;
            }
        };

        virtual NeighborStats GetStats() const {
            return NeighborStats{}; // Default empty stats
        }

        virtual void ResetStats() {
            // Default implementation does nothing
        }

        // === UTILITY FUNCTIONS ===

        // Convert Direction enum to offset vector
        static glm::ivec3 DirectionToOffset(Direction direction) {
            switch (direction) {
                case Direction::Up:    return {0, 1, 0};
                case Direction::Down:  return {0, -1, 0};
                case Direction::North: return {0, 0, -1};
                case Direction::South: return {0, 0, 1};
                case Direction::East:  return {1, 0, 0};
                case Direction::West:  return {-1, 0, 0};
            }
            return {0, 0, 0};
        }

        // Get opposite direction
        static Direction GetOpposite(Direction direction) {
            switch (direction) {
                case Direction::Up:    return Direction::Down;
                case Direction::Down:  return Direction::Up;
                case Direction::North: return Direction::South;
                case Direction::South: return Direction::North;
                case Direction::East:  return Direction::West;
                case Direction::West:  return Direction::East;
            }
            return Direction::Up;
        }

        // Convert direction to string (for debugging)
        static std::string DirectionToString(Direction direction) {
            switch (direction) {
                case Direction::Up:    return "Up";
                case Direction::Down:  return "Down";
                case Direction::North: return "North";
                case Direction::South: return "South";
                case Direction::East:  return "East";
                case Direction::West:  return "West";
            }
            return "Unknown";
        }
    };

    // Factory function type for creating neighbor providers
    using NeighborProviderFactory = std::function<std::unique_ptr<INeighborProvider>()>;

} // namespace Game