// File: src/client/renderer/mesh/SnapshotBlockAccess.hpp
#pragma once

#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include "common/core/Config.hpp"
#include "MeshJobData.hpp"

namespace Client {
namespace Render {

    // Read-only block access adapter for mesh snapshot data
    // Implements Minecraft-style ChunkRenderRegion pattern
    class SnapshotBlockAccess : public Game::IBlockAccess {
    public:
        // Constructor takes snapshot data from MeshJobData
        SnapshotBlockAccess(const SectionSnapshot& sectionData,
                           Game::Math::ChunkPos chunkPos,
                           int sectionY)
            : m_sectionData(sectionData)
            , m_chunkPos(chunkPos)
            , m_sectionY(sectionY) {
            
            // Calculate world bounds for this section
            m_worldMinX = chunkPos.x * 16;
            m_worldMaxX = m_worldMinX + 15;
            m_worldMinZ = chunkPos.z * 16;
            m_worldMaxZ = m_worldMinZ + 15;
            m_worldMinY = Config::MinY + sectionY * 16;
            m_worldMaxY = m_worldMinY + 15;
        }

        // IBlockAccess implementation
        Game::BlockID GetBlock(int worldX, int worldY, int worldZ) const override {
            // Convert world coordinates to local
            int localX = worldX - m_worldMinX;
            int localY = worldY - m_worldMinY;
            int localZ = worldZ - m_worldMinZ;

            // Check if within this section
            if (localX >= 0 && localX < 16 && 
                localY >= 0 && localY < 16 && 
                localZ >= 0 && localZ < 16) {
                return m_sectionData.GetBlock(localX, localY, localZ);
            }

            // Check neighbor sections for face culling
            // This is for the 1-block halo needed for proper face culling
            return GetNeighborBlock(worldX, worldY, worldZ);
        }

        bool IsChunkLoaded(int chunkX, int chunkZ) const override {
            // Only this chunk and immediate neighbors are "loaded" in snapshot
            int dx = std::abs(chunkX - m_chunkPos.x);
            int dz = std::abs(chunkZ - m_chunkPos.z);
            return dx <= 1 && dz <= 1;
        }

        bool IsPositionLoaded(int worldX, int worldY, int worldZ) const override {
            // Check if position is within snapshot bounds (section + 1-block halo)
            return worldX >= (m_worldMinX - 1) && worldX <= (m_worldMaxX + 1) &&
                   worldY >= (m_worldMinY - 1) && worldY <= (m_worldMaxY + 1) &&
                   worldZ >= (m_worldMinZ - 1) && worldZ <= (m_worldMaxZ + 1);
        }

        bool IsBlockSolid(int worldX, int worldY, int worldZ) const override {
            Game::BlockID block = GetBlock(worldX, worldY, worldZ);
            // TODO: Use BlockRegistry to check if solid
            return block != Game::BlockID::Air && 
                   block != Game::BlockID::Water && 
                   block != Game::BlockID::Lava;
        }

        bool IsBlockFluid(int worldX, int worldY, int worldZ) const override {
            Game::BlockID block = GetBlock(worldX, worldY, worldZ);
            return block == Game::BlockID::Water || block == Game::BlockID::Lava;
        }

        bool IsValidPosition(int worldX, int worldY, int worldZ) const override {
            return worldY >= Config::MinY && worldY <= Config::MaxY;
        }

    private:
        // Get block from neighbor sections (for face culling)
        Game::BlockID GetNeighborBlock(int worldX, int worldY, int worldZ) const {
            // Determine which neighbor face to check
            int localX = worldX - m_worldMinX;
            int localY = worldY - m_worldMinY;
            int localZ = worldZ - m_worldMinZ;

            // Check each neighbor direction
            if (localX < 0 && localX >= -1) {
                // West neighbor (negative X)
                int nx = 15; // Get east edge of west neighbor
                int ny = std::min(std::max(localY, 0), 15); // Clamp Y to valid range
                int nz = std::min(std::max(localZ, 0), 15); // Clamp Z to valid range
                return m_sectionData.GetNeighborBlock(3, nx, ny, nz); // West = index 3
            }
            else if (localX > 15 && localX <= 16) {
                // East neighbor (positive X)
                int nx = 0; // Get west edge of east neighbor
                int ny = std::min(std::max(localY, 0), 15); // Clamp Y to valid range
                int nz = std::min(std::max(localZ, 0), 15); // Clamp Z to valid range
                return m_sectionData.GetNeighborBlock(2, nx, ny, nz); // East = index 2
            }
            else if (localZ < 0 && localZ >= -1) {
                // North neighbor (negative Z)
                int nx = std::min(std::max(localX, 0), 15); // Clamp X to valid range
                int ny = std::min(std::max(localY, 0), 15); // Clamp Y to valid range
                int nz = 15; // Get south edge of north neighbor
                return m_sectionData.GetNeighborBlock(0, nx, ny, nz); // North = index 0
            }
            else if (localZ > 15 && localZ <= 16) {
                // South neighbor (positive Z)
                int nx = std::min(std::max(localX, 0), 15); // Clamp X to valid range
                int ny = std::min(std::max(localY, 0), 15); // Clamp Y to valid range
                int nz = 0; // Get north edge of south neighbor
                return m_sectionData.GetNeighborBlock(1, nx, ny, nz); // South = index 1
            }
            else if (localY < 0 && localY >= -1) {
                // Down neighbor (negative Y)
                int nx = std::min(std::max(localX, 0), 15); // Clamp X to valid range
                int ny = 15; // Get top edge of neighbor below
                int nz = std::min(std::max(localZ, 0), 15); // Clamp Z to valid range
                return m_sectionData.GetNeighborBlock(5, nx, ny, nz); // Down = index 5
            }
            else if (localY > 15 && localY <= 16) {
                // Up neighbor (positive Y)
                int nx = std::min(std::max(localX, 0), 15); // Clamp X to valid range
                int ny = 0; // Get bottom edge of neighbor above
                int nz = std::min(std::max(localZ, 0), 15); // Clamp Z to valid range
                return m_sectionData.GetNeighborBlock(4, nx, ny, nz); // Up = index 4
            }

            return Game::BlockID::Air;
        }

    private:
        const SectionSnapshot& m_sectionData;
        Game::Math::ChunkPos m_chunkPos;
        int m_sectionY;
        
        // World bounds for this section
        int m_worldMinX, m_worldMaxX;
        int m_worldMinY, m_worldMaxY;
        int m_worldMinZ, m_worldMaxZ;
    };

} // namespace Render
} // namespace Client