// File: src/common/physics/RayCast.hpp
#pragma once

#include <glm/glm.hpp>
#include <optional>
#include "../world/block/Blocks.hpp"
#include "../world/math/WorldMath.hpp"

namespace Game {

    // Forward declarations
    struct IBlockAccess;

    // Represents the result of a raycast hit on a block
    struct RaycastHit {
        glm::ivec3 blockPos;        // World position of the hit block
        glm::ivec3 adjacentPos;     // Position where a new block could be placed
        glm::vec3 hitPoint;         // Exact world-space hit point
        glm::vec3 normal;           // Face normal at hit point
        BlockID blockId;            // ID of the hit block
        float distance;             // Distance from ray origin to hit point
        int hitFace;                // Which face was hit (0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z)
    };

    class Raycast {
    public:
        // Cast a ray and find the first solid block hit
        // Returns std::nullopt if no block was hit within maxDistance
        static std::optional<RaycastHit> CastRay(
            const glm::vec3& origin,
            const glm::vec3& direction,
            float maxDistance = 5.0f
        );

    private:
        // Helper to get block at world position (thread-safe)
        static BlockID GetBlockAtWorldPos(const glm::vec3& pos);

        // Check if a block is solid (can be hit by raycast)
        static bool IsBlockSolid(BlockID id);
    };

    // **NEW**: Global block access management for raycast system
    void SetGlobalBlockAccess(const IBlockAccess* blockAccess);

} // namespace Game