// File: src/common/physics/RayCast.cpp
#include "RayCast.hpp"
#include "../world/block/BlockRegistry.hpp"
#include "../core/Log.hpp"
#include "../core/Config.hpp"
#include "Physics.hpp"
#include <cmath>
#include "../world/chunk/IBlockAccess.hpp"

namespace Game {

    // Global block access pointer for raycast system
    static const IBlockAccess* g_raycastBlockAccess = nullptr;

    // **NEW**: Set the global block access for raycast system
    void SetGlobalBlockAccess(const IBlockAccess* blockAccess) {
        g_raycastBlockAccess = blockAccess;
    }

    // **NEW**: Get block using global access
    static BlockID GetBlock(int worldX, int worldY, int worldZ) {
        if (g_raycastBlockAccess) {
            return g_raycastBlockAccess->GetBlock(worldX, worldY, worldZ);
        }
        Log::Warning("No global block access available for raycast");
        return BlockID::Air;
    }

    std::optional<RaycastHit> Raycast::CastRay(
        const glm::vec3& origin,
        const glm::vec3& direction,
        float maxDistance)
    {
        // Normalize direction
        glm::vec3 dir = glm::normalize(direction);

        // DDA algorithm setup
        glm::vec3 currentPos = origin;
        glm::ivec3 currentBlock = glm::ivec3(
            static_cast<int>(std::floor(currentPos.x)),
            static_cast<int>(std::floor(currentPos.y)),
            static_cast<int>(std::floor(currentPos.z))
        );

        // Calculate step direction for each axis
        glm::ivec3 step;
        step.x = (dir.x > 0) ? 1 : -1;
        step.y = (dir.y > 0) ? 1 : -1;
        step.z = (dir.z > 0) ? 1 : -1;

        // Calculate the distance to the next voxel boundary for each axis
        glm::vec3 tMax;
        glm::vec3 tDelta;

        // Small epsilon to avoid division by zero
        const float epsilon = 1e-6f;

        // X axis
        if (std::abs(dir.x) > epsilon) {
            float voxelBoundary = (dir.x > 0) ?
                std::floor(currentPos.x) + 1.0f :
                std::floor(currentPos.x);
            tMax.x = (voxelBoundary - currentPos.x) / dir.x;
            tDelta.x = 1.0f / std::abs(dir.x);
        } else {
            tMax.x = 1e30f;
            tDelta.x = 1e30f;
        }

        // Y axis
        if (std::abs(dir.y) > epsilon) {
            float voxelBoundary = (dir.y > 0) ?
                std::floor(currentPos.y) + 1.0f :
                std::floor(currentPos.y);
            tMax.y = (voxelBoundary - currentPos.y) / dir.y;
            tDelta.y = 1.0f / std::abs(dir.y);
        } else {
            tMax.y = 1e30f;
            tDelta.y = 1e30f;
        }

        // Z axis
        if (std::abs(dir.z) > epsilon) {
            float voxelBoundary = (dir.z > 0) ?
                std::floor(currentPos.z) + 1.0f :
                std::floor(currentPos.z);
            tMax.z = (voxelBoundary - currentPos.z) / dir.z;
            tDelta.z = 1.0f / std::abs(dir.z);
        } else {
            tMax.z = 1e30f;
            tDelta.z = 1e30f;
        }

        // Track the previous block position for adjacent placement
        glm::ivec3 previousBlock = currentBlock;
        int lastStepAxis = -1; // 0=X, 1=Y, 2=Z

        float totalDistance = 0.0f;
        
        // Check if we start inside a block
        bool startedInsideBlock = false;
        BlockID startBlockId = GetBlockAtWorldPos(origin);
        if (IsBlockSolid(startBlockId)) {
            startedInsideBlock = true;
        }

        // Ray marching loop
        while (totalDistance < maxDistance) {
            // Check current block
            BlockID blockId = GetBlockAtWorldPos(glm::vec3(currentBlock));

            if (IsBlockSolid(blockId)) {
                // We hit a solid block!
                RaycastHit hit;
                hit.blockPos = currentBlock;
                hit.adjacentPos = previousBlock;
                hit.blockId = blockId;
                hit.distance = totalDistance;

                // Calculate exact hit point
                float t = totalDistance;
                hit.hitPoint = origin + dir * t;
                
                // Calculate cursor position (fractional part within block)
                glm::vec3 blockOrigin = glm::vec3(currentBlock);
                hit.cursorPos = hit.hitPoint - blockOrigin;
                // Clamp to [0, 1) range for safety
                hit.cursorPos = glm::clamp(hit.cursorPos, glm::vec3(0.0f), glm::vec3(0.999f));
                
                // Set inside block flag (true if this is the first block and we started inside)
                hit.insideBlock = (totalDistance == 0.0f && startedInsideBlock);

                // Determine which face was hit based on the last step
                if (lastStepAxis == 0) { // X axis
                    hit.hitFace = (step.x > 0) ? 1 : 0; // -X or +X face
                    hit.normal = glm::vec3(-step.x, 0, 0);
                } else if (lastStepAxis == 1) { // Y axis
                    hit.hitFace = (step.y > 0) ? 3 : 2; // -Y or +Y face
                    hit.normal = glm::vec3(0, -step.y, 0);
                } else if (lastStepAxis == 2) { // Z axis
                    hit.hitFace = (step.z > 0) ? 5 : 4; // -Z or +Z face
                    hit.normal = glm::vec3(0, 0, -step.z);
                } else {
                    // First block check (at origin)
                    hit.hitFace = 0;
                    hit.normal = glm::vec3(0, 1, 0);
                }

                return hit;
            }

            // Store current block as previous before stepping
            previousBlock = currentBlock;

            // Find the next voxel boundary crossing
            if (tMax.x < tMax.y && tMax.x < tMax.z) {
                // Step in X direction
                currentBlock.x += step.x;
                totalDistance = tMax.x;
                tMax.x += tDelta.x;
                lastStepAxis = 0;
            } else if (tMax.y < tMax.z) {
                // Step in Y direction
                currentBlock.y += step.y;
                totalDistance = tMax.y;
                tMax.y += tDelta.y;
                lastStepAxis = 1;
            } else {
                // Step in Z direction
                currentBlock.z += step.z;
                totalDistance = tMax.z;
                tMax.z += tDelta.z;
                lastStepAxis = 2;
            }

            // Check world boundaries - use Config namespace
            if (currentBlock.y < Config::MinY || currentBlock.y > Config::MaxY) {
                break; // Out of world bounds
            }
        }

        // No block hit within range
        return std::nullopt;
    }

    BlockID Raycast::GetBlockAtWorldPos(const glm::vec3& pos) {
        return GetBlock(
            static_cast<int>(std::floor(pos.x)),
            static_cast<int>(std::floor(pos.y)),
            static_cast<int>(std::floor(pos.z))
        );
    }

    bool Raycast::IsBlockSolid(BlockID id) {
        if (id == BlockID::Air) {
            return false;
        }
        // Raycast should hit all solid blocks, not just opaque ones.
        // Leaves (Cutout) are solid for interaction but not opaque for rendering.
        // Water and Lava are non-solid (can't target them).
        return id != BlockID::Water && id != BlockID::Lava;
    }

} // namespace Game