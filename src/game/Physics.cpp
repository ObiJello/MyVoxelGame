// File: src/game/Physics.cpp
#include "Physics.hpp"
#include "WorldAccess.hpp"
#include "BlockRegistry.hpp"
#include "../core/Log.hpp"
#include "../core/Config.hpp"
#include <algorithm>
#include <cmath>

namespace Game {

    bool Physics::IsBlockSolid(BlockID blockId) {
        if (blockId == BlockID::Air) {
            return false;
        }

        // Water and other liquids are not solid for collision
        if (blockId == BlockID::Water) {
            return false;
        }

        const Block& block = BlockRegistry::Get(blockId);
        return block.opaque; // Use opaque flag as solid indicator
    }

    CollisionResult Physics::CheckCollision(const AABB& aabb) {
        CollisionResult result;

        // Get all blocks that could potentially intersect
        auto blocks = GetIntersectingBlocks(aabb);

        for (const auto& blockPos : blocks) {
            BlockID blockId = WorldAccess::GetBlock(blockPos.x, blockPos.y, blockPos.z);

            if (!IsBlockSolid(blockId)) {
                continue;
            }

            // Create block AABB (each block is 1x1x1)
            AABB blockAABB(
                glm::vec3(blockPos) + glm::vec3(0.5f),
                glm::vec3(1.0f)
            );

            if (aabb.Intersects(blockAABB)) {
                result.hasCollision = true;
                result.blockId = blockId;
                result.blockPos = blockPos;

                // Calculate penetration (simple approach)
                glm::vec3 overlap;
                overlap.x = std::min(aabb.max.x - blockAABB.min.x, blockAABB.max.x - aabb.min.x);
                overlap.y = std::min(aabb.max.y - blockAABB.min.y, blockAABB.max.y - aabb.min.y);
                overlap.z = std::min(aabb.max.z - blockAABB.min.z, blockAABB.max.z - aabb.min.z);

                // Find the axis with minimum overlap (surface normal)
                if (overlap.x <= overlap.y && overlap.x <= overlap.z) {
                    result.normal = (aabb.GetCenter().x < blockAABB.GetCenter().x) ?
                        glm::vec3(-1, 0, 0) : glm::vec3(1, 0, 0);
                    result.penetration = glm::vec3(overlap.x * result.normal.x, 0, 0);
                } else if (overlap.y <= overlap.z) {
                    result.normal = (aabb.GetCenter().y < blockAABB.GetCenter().y) ?
                        glm::vec3(0, -1, 0) : glm::vec3(0, 1, 0);
                    result.penetration = glm::vec3(0, overlap.y * result.normal.y, 0);
                } else {
                    result.normal = (aabb.GetCenter().z < blockAABB.GetCenter().z) ?
                        glm::vec3(0, 0, -1) : glm::vec3(0, 0, 1);
                    result.penetration = glm::vec3(0, 0, overlap.z * result.normal.z);
                }

                break; // Return first collision found
            }
        }

        return result;
    }

    glm::vec3 Physics::ResolveCollision(const AABB& aabb, const glm::vec3& movement) {
        glm::vec3 resolvedMovement = movement;

        // Resolve each axis separately (order matters for step-up behavior)
        // X axis first
        if (std::abs(movement.x) > 0.001f) {
            AABB testAABB = aabb;
            testAABB.min.x += resolvedMovement.x;
            testAABB.max.x += resolvedMovement.x;

            if (CheckCollision(testAABB).hasCollision) {
                resolvedMovement.x = 0.0f;
            }
        }

        // Z axis second
        if (std::abs(movement.z) > 0.001f) {
            AABB testAABB = aabb;
            testAABB.min.x += resolvedMovement.x;
            testAABB.max.x += resolvedMovement.x;
            testAABB.min.z += resolvedMovement.z;
            testAABB.max.z += resolvedMovement.z;

            if (CheckCollision(testAABB).hasCollision) {
                resolvedMovement.z = 0.0f;
            }
        }

        // Y axis last (vertical movement)
        if (std::abs(movement.y) > 0.001f) {
            AABB testAABB = aabb;
            testAABB.min += resolvedMovement;
            testAABB.max += resolvedMovement;

            if (CheckCollision(testAABB).hasCollision) {
                resolvedMovement.y = 0.0f;
            }
        }

        return resolvedMovement;
    }

    void Physics::UpdatePlayerPhysics(PlayerPhysics& physics, const glm::vec3& inputMovement,
                                     bool jumpPressed, float deltaTime) {
        if (physics.noclip) {
            // Debug mode: no physics, just move freely
            physics.position += inputMovement * physics.GetMoveSpeed() * deltaTime;
            physics.velocity = glm::vec3(0.0f);
            physics.isOnGround = false;
            return;
        }

        // Check current state
        physics.isOnGround = CheckGroundCollision(physics);
        physics.isInWater = CheckWaterCollision(physics);

        // Apply gravity
        if (!physics.isOnGround && !physics.isInWater) {
            physics.velocity.y -= PlayerPhysics::GRAVITY * deltaTime;
            // Terminal velocity
            physics.velocity.y = std::max(physics.velocity.y, -PlayerPhysics::TERMINAL_VELOCITY);
        } else if (physics.isInWater) {
            // Water physics (simplified)
            physics.velocity.y *= 0.8f; // Water resistance
        }

        // Handle jumping
        if (jumpPressed && (physics.isOnGround || physics.isInWater)) {
            physics.velocity.y = PlayerPhysics::JUMP_VELOCITY;
            physics.isOnGround = false;
        }

        // Handle horizontal movement
        glm::vec3 horizontalMovement = glm::vec3(inputMovement.x, 0.0f, inputMovement.z);
        if (glm::length(horizontalMovement) > 0.0f) {
            horizontalMovement = glm::normalize(horizontalMovement);

            float speed = physics.GetMoveSpeed();
            if (physics.isInWater) {
                speed *= 0.5f; // Slower in water
            }

            horizontalMovement *= speed;
        }

        // Apply movement with collision
        glm::vec3 totalMovement = horizontalMovement * deltaTime +
                                 glm::vec3(0.0f, physics.velocity.y * deltaTime, 0.0f);

        AABB currentAABB = physics.GetAABB();
        glm::vec3 resolvedMovement = ResolveCollision(currentAABB, totalMovement);

        // Update position
        physics.position += resolvedMovement;

        // Update velocity based on collision
        if (std::abs(resolvedMovement.x) < std::abs(totalMovement.x) * 0.5f) {
            physics.velocity.x = 0.0f; // Hit wall
        }
        if (std::abs(resolvedMovement.z) < std::abs(totalMovement.z) * 0.5f) {
            physics.velocity.z = 0.0f; // Hit wall
        }
        if (std::abs(resolvedMovement.y) < std::abs(totalMovement.y) * 0.5f) {
            if (physics.velocity.y < 0.0f) {
                physics.isOnGround = true; // Landed
            }
            physics.velocity.y = 0.0f; // Hit ceiling or ground
        }

        // Apply friction when on ground
        if (physics.isOnGround && glm::length(inputMovement) < 0.1f) {
            physics.velocity.x *= PlayerPhysics::FRICTION;
            physics.velocity.z *= PlayerPhysics::FRICTION;
        }

        // Apply air resistance
        if (!physics.isOnGround) {
            physics.velocity.x *= PlayerPhysics::AIR_RESISTANCE;
            physics.velocity.z *= PlayerPhysics::AIR_RESISTANCE;
        }

        // Clamp position to world bounds
        physics.position.y = std::max(physics.position.y, static_cast<float>(Config::MinY));
        physics.position.y = std::min(physics.position.y, static_cast<float>(Config::MaxY - 2));
    }

    bool Physics::CheckGroundCollision(const PlayerPhysics& physics) {
        // Check slightly below the player's feet
        AABB groundCheck = physics.GetAABB();
        groundCheck.min.y -= 0.1f;
        groundCheck.max.y = physics.position.y + 0.1f;

        return CheckCollision(groundCheck).hasCollision;
    }

    bool Physics::CheckWaterCollision(const PlayerPhysics& physics) {
        // Check if player's feet are in water
        glm::ivec3 footPos = glm::ivec3(glm::floor(physics.position));
        BlockID blockId = WorldAccess::GetBlock(footPos.x, footPos.y, footPos.z);
        return blockId == BlockID::Water;
    }

    std::vector<glm::ivec3> Physics::GetIntersectingBlocks(const AABB& aabb) {
        std::vector<glm::ivec3> blocks;

        // Calculate block coordinate bounds
        int minX = static_cast<int>(std::floor(aabb.min.x));
        int maxX = static_cast<int>(std::floor(aabb.max.x));
        int minY = static_cast<int>(std::floor(aabb.min.y));
        int maxY = static_cast<int>(std::floor(aabb.max.y));
        int minZ = static_cast<int>(std::floor(aabb.min.z));
        int maxZ = static_cast<int>(std::floor(aabb.max.z));

        // Add all blocks in the range
        for (int x = minX; x <= maxX; ++x) {
            for (int y = minY; y <= maxY; ++y) {
                for (int z = minZ; z <= maxZ; ++z) {
                    // Check if block is within world bounds
                    if (WorldAccess::IsValidPosition(x, y, z)) {
                        blocks.emplace_back(x, y, z);
                    }
                }
            }
        }

        return blocks;
    }

    float Physics::ResolveAxisCollision(const AABB& aabb, const glm::vec3& movement, int axis) {
        // This is a more detailed per-axis collision resolution
        // For now, we use the simpler approach in ResolveCollision
        return movement[axis];
    }

    bool Physics::CheckAxisCollision(const AABB& aabb, float movement, int axis) {
        // Helper for per-axis collision checking
        AABB testAABB = aabb;
        testAABB.min[axis] += movement;
        testAABB.max[axis] += movement;

        return CheckCollision(testAABB).hasCollision;
    }

} // namespace Game