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
        // Get block from neighbor sections for face culling and AO.
        // Handles diagonal lookups (2-axis offsets like corner AO samples) by
        // picking the dominant axis neighbor and clamping the other coordinates.
        // This avoids bright seams at chunk borders where diagonal data is unavailable.
        Game::BlockID GetNeighborBlock(int worldX, int worldY, int worldZ) const {
            int localX = worldX - m_worldMinX;
            int localY = worldY - m_worldMinY;
            int localZ = worldZ - m_worldMinZ;

            // Determine which axes are out of bounds
            bool outX = localX < 0 || localX > 15;
            bool outY = localY < 0 || localY > 15;
            bool outZ = localZ < 0 || localZ > 15;

            if (!outX && !outY && !outZ) {
                // Shouldn't reach here — within section bounds
                return m_sectionData.GetBlock(localX, localY, localZ);
            }

            // For diagonal lookups (multiple axes out of bounds), pick one neighbor
            // and clamp the other coordinates. This matches Minecraft's behavior of
            // using the closest available data rather than returning Air.
            // Priority: Y neighbors (up/down) > X neighbors (east/west) > Z neighbors (north/south)

            int face = -1;
            int nx = std::clamp(localX, 0, 15);
            int ny = std::clamp(localY, 0, 15);
            int nz = std::clamp(localZ, 0, 15);

            if (outY) {
                if (localY < 0) {
                    face = 5; // Down
                    ny = 15;
                } else {
                    face = 4; // Up
                    ny = 0;
                }
            } else if (outX) {
                if (localX < 0) {
                    face = 3; // West
                    nx = 15;
                } else {
                    face = 2; // East
                    nx = 0;
                }
            } else if (outZ) {
                if (localZ < 0) {
                    face = 0; // North
                    nz = 15;
                } else {
                    face = 1; // South
                    nz = 0;
                }
            }

            if (face >= 0) {
                return m_sectionData.GetNeighborBlock(face, nx, ny, nz);
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