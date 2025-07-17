// File: src/game/WorldCoordinates.hpp
#pragma once

#include "../core/Config.hpp"
#include "WorldMath.hpp"
#include <cassert>

namespace Game::Math {

    // Centralized coordinate conversion utilities
    // This class eliminates duplication and ensures consistent Y-coordinate handling
    class WorldCoordinates {
    public:
        // === WORLD Y COORDINATE CONVERSIONS ===

        // Convert world Y coordinate to section index (0-23)
        static constexpr int WorldYToSectionIndex(int worldY) {
            return (worldY - Config::MinY) / SECTION_HEIGHT;
        }

        // Convert world Y coordinate to local Y within section (0-15)
        static constexpr int WorldYToSectionLocal(int worldY) {
            return (worldY - Config::MinY) % SECTION_HEIGHT;
        }

        // Validate world Y coordinate bounds
        static constexpr bool IsValidWorldY(int worldY) {
            return worldY >= Config::MinY && worldY <= Config::MaxY;
        }

        // Convert world Y to both section index and local Y in one call
        static void WorldYToSectionCoords(int worldY, int& sectionIndex, int& sectionY) {
            if (!IsValidWorldY(worldY)) {
                sectionIndex = -1;
                sectionY = -1;
                return;
            }

            int chunkLocalY = worldY - Config::MinY;
            sectionIndex = chunkLocalY / SECTION_HEIGHT;
            sectionY = chunkLocalY % SECTION_HEIGHT;

            // Ensure results are in valid range
            assert(sectionIndex >= 0 && sectionIndex < SECTIONS_PER_CHUNK);
            assert(sectionY >= 0 && sectionY < SECTION_HEIGHT);
        }

        // Convert section index and local Y back to world Y
        static constexpr int SectionCoordsToWorldY(int sectionIndex, int sectionY) {
            return Config::MinY + (sectionIndex * SECTION_HEIGHT) + sectionY;
        }

        // Convert world X coordinate to local chunk X coordinate
        static constexpr int WorldXToChunkX(int worldX) {
            return worldX - (WorldToChunkPos(worldX, 0).x * CHUNK_SIZE_X);
        }

        // Convert world Z coordinate to local chunk Z coordinate
        static constexpr int WorldZToChunkZ(int worldZ) {
            return worldZ - (WorldToChunkPos(0, worldZ).z * CHUNK_SIZE_Z);
        }

        // Clamp world Y coordinate to valid range
        static constexpr int ClampWorldY(int worldY) {
            return std::clamp(worldY, MIN_WORLD_Y, MAX_WORLD_Y);
        }

        // === CHUNK COORDINATE CONVERSIONS ===

        // Convert world X/Z coordinates to chunk coordinates
        static ChunkPos WorldToChunkPos(int worldX, int worldZ) {
            return ChunkPos{
                static_cast<int32_t>(worldX >= 0 ? worldX / CHUNK_SIZE_X : (worldX - CHUNK_SIZE_X + 1) / CHUNK_SIZE_X),
                static_cast<int32_t>(worldZ >= 0 ? worldZ / CHUNK_SIZE_Z : (worldZ - CHUNK_SIZE_Z + 1) / CHUNK_SIZE_Z)
            };
        }

        // Convert world coordinates to chunk position and local coordinates
        static void WorldToLocal(int worldX, int worldY, int worldZ,
                                ChunkPos& chunkPos, int& localX, int& localY, int& localZ) {
            // Calculate chunk coordinates
            chunkPos = WorldToChunkPos(worldX, worldZ);

            // Calculate local coordinates within chunk
            localX = worldX - (chunkPos.x * CHUNK_SIZE_X);
            localZ = worldZ - (chunkPos.z * CHUNK_SIZE_Z);
            localY = worldY; // World Y is used directly now

            // Ensure local coordinates are in valid range
            if (localX < 0) localX += CHUNK_SIZE_X;
            if (localZ < 0) localZ += CHUNK_SIZE_Z;

            assert(localX >= 0 && localX < CHUNK_SIZE_X);
            assert(localZ >= 0 && localZ < CHUNK_SIZE_Z);
        }

        // Convert local coordinates back to world coordinates
        static void LocalToWorld(const ChunkPos& chunkPos, int localX, int localY, int localZ,
                                int& worldX, int& worldY, int& worldZ) {
            worldX = chunkPos.x * CHUNK_SIZE_X + localX;
            worldY = localY; // Local Y is world Y
            worldZ = chunkPos.z * CHUNK_SIZE_Z + localZ;
        }

        // === VALIDATION FUNCTIONS ===

        // Check if local chunk coordinates are valid
        static constexpr bool IsValidLocalCoords(int localX, int localY, int localZ) {
            return localX >= 0 && localX < CHUNK_SIZE_X &&
                   IsValidWorldY(localY) &&
                   localZ >= 0 && localZ < CHUNK_SIZE_Z;
        }

        // Check if section coordinates are valid
        static constexpr bool IsValidSectionCoords(int sectionIndex, int sectionY) {
            return sectionIndex >= 0 && sectionIndex < SECTIONS_PER_CHUNK &&
                   sectionY >= 0 && sectionY < SECTION_HEIGHT;
        }

        // === NEIGHBOR CALCULATION ===

        // Calculate world coordinates of neighboring block
        static void GetNeighborWorldCoords(int worldX, int worldY, int worldZ,
                                           Render::BlockFace face, int& neighborX, int& neighborY, int& neighborZ) {
            neighborX = worldX;
            neighborY = worldY;
            neighborZ = worldZ;

            switch (face) {
                case BlockFace::PositiveX: neighborX++; break;
                case BlockFace::NegativeX: neighborX--; break;
                case BlockFace::PositiveY: neighborY++; break;
                case BlockFace::NegativeY: neighborY--; break;
                case BlockFace::PositiveZ: neighborZ++; break;
                case BlockFace::NegativeZ: neighborZ--; break;
            }
        }

        // === DISTANCE CALCULATIONS ===

        // Calculate Manhattan distance between two chunk positions
        static int ChunkManhattanDistance(const ChunkPos& a, const ChunkPos& b) {
            return std::abs(a.x - b.x) + std::abs(a.z - b.z);
        }

        // Calculate Chebyshev distance (square pattern) between chunk positions
        static int ChunkChebyshevDistance(const ChunkPos& a, const ChunkPos& b) {
            return std::max(std::abs(a.x - b.x), std::abs(a.z - b.z));
        }

        // Calculate world distance between two section positions
        static float SectionDistance(const ChunkPos& chunkPos1, int sectionY1,
                                   const ChunkPos& chunkPos2, int sectionY2) {
            float dx = static_cast<float>((chunkPos1.x - chunkPos2.x) * CHUNK_SIZE_X);
            float dy = static_cast<float>(SectionCoordsToWorldY(sectionY1, 0) - SectionCoordsToWorldY(sectionY2, 0));
            float dz = static_cast<float>((chunkPos1.z - chunkPos2.z) * CHUNK_SIZE_Z);

            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        // === CONSTANTS FOR REFERENCE ===

        static constexpr int MIN_WORLD_Y = Config::MinY;         // -64
        static constexpr int MAX_WORLD_Y = Config::MaxY;         // 319
        static constexpr int WORLD_HEIGHT = MAX_WORLD_Y - MIN_WORLD_Y + 1; // 384

    private:
        // Block face enum forward declaration (defined in render system)
        enum class BlockFace {
            PositiveX, NegativeX,
            PositiveY, NegativeY,
            PositiveZ, NegativeZ
        };

        WorldCoordinates() = delete; // Static utility class
    };

    // === CONVENIENCE ALIASES ===

    // Short aliases for commonly used functions
    using WC = WorldCoordinates;

    // Inline convenience functions for performance-critical code
    inline constexpr int WorldYToSection(int worldY) {
        return WorldCoordinates::WorldYToSectionIndex(worldY);
    }

    inline constexpr int WorldYToSectionLocal(int worldY) {
        return WorldCoordinates::WorldYToSectionLocal(worldY);
    }

    inline constexpr bool IsValidWorldY(int worldY) {
        return WorldCoordinates::IsValidWorldY(worldY);
    }

} // namespace Game::Math