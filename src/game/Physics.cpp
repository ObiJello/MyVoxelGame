// File: src/game/Physics.cpp (Enhanced with Sneak Edge Protection)
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

    // NEW: Check if there's solid ground beneath the given position (for sneak edge protection)
    bool Physics::HasSolidGroundBelow(const glm::vec3& position, float checkRadius) {
        // Check if ANY part of the player's 0.6x0.6 square hitbox would still be supported by solid ground
        // Use the actual player width for accurate edge detection

        const float halfWidth = PlayerPhysics::WIDTH * 0.5f; // 0.3 (half of 0.6)
        const int checksPerSide = 4; // Check 4 points per side for good coverage

        // Check the four corners of the 0.6x0.6 square first (most important for edge cases)
        glm::vec2 corners[4] = {
            {-halfWidth, -halfWidth}, // Bottom-left corner
            { halfWidth, -halfWidth}, // Bottom-right corner
            { halfWidth,  halfWidth}, // Top-right corner
            {-halfWidth,  halfWidth}  // Top-left corner
        };

        for (int i = 0; i < 4; ++i) {
            glm::vec3 checkPos = position + glm::vec3(corners[i].x, 0.0f, corners[i].y);

            int blockX = static_cast<int>(std::floor(checkPos.x));
            int blockY = static_cast<int>(std::floor(checkPos.y - 0.1f));
            int blockZ = static_cast<int>(std::floor(checkPos.z));

            BlockID blockBelow = WorldAccess::GetBlock(blockX, blockY, blockZ);
            if (IsBlockSolid(blockBelow)) {
                return true; // Found support at a corner
            }
        }

        // Check points along the edges of the 0.6x0.6 square (excluding corners to avoid double-checking)
        for (int side = 0; side < 4; ++side) {
            for (int point = 1; point < checksPerSide - 1; ++point) {
                float t = static_cast<float>(point) / static_cast<float>(checksPerSide - 1);
                glm::vec3 checkPos;

                switch (side) {
                    case 0: // Bottom edge (-Z side)
                        checkPos = position + glm::vec3(-halfWidth + t * 2.0f * halfWidth, 0.0f, -halfWidth);
                        break;
                    case 1: // Right edge (+X side)
                        checkPos = position + glm::vec3(halfWidth, 0.0f, -halfWidth + t * 2.0f * halfWidth);
                        break;
                    case 2: // Top edge (+Z side)
                        checkPos = position + glm::vec3(halfWidth - t * 2.0f * halfWidth, 0.0f, halfWidth);
                        break;
                    case 3: // Left edge (-X side)
                        checkPos = position + glm::vec3(-halfWidth, 0.0f, halfWidth - t * 2.0f * halfWidth);
                        break;
                }

                int blockX = static_cast<int>(std::floor(checkPos.x));
                int blockY = static_cast<int>(std::floor(checkPos.y - 0.1f));
                int blockZ = static_cast<int>(std::floor(checkPos.z));

                BlockID blockBelow = WorldAccess::GetBlock(blockX, blockY, blockZ);
                if (IsBlockSolid(blockBelow)) {
                    return true; // Found support along an edge
                }
            }
        }

        // Check the center position of the 0.6x0.6 square
        int centerX = static_cast<int>(std::floor(position.x));
        int centerY = static_cast<int>(std::floor(position.y - 0.1f));
        int centerZ = static_cast<int>(std::floor(position.z));

        BlockID centerBlock = WorldAccess::GetBlock(centerX, centerY, centerZ);
        if (IsBlockSolid(centerBlock)) {
            return true;
        }

        // No solid ground found under any part of the player's 0.6x0.6 hitbox
        return false;
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

    // ENHANCED: Resolve collision with sneak edge protection
    glm::vec3 Physics::ResolveCollisionWithSneakProtection(const AABB& aabb, const glm::vec3& movement, bool isSneaking) {
        glm::vec3 resolvedMovement = movement;

        // If not sneaking, use normal collision resolution
        if (!isSneaking) {
            return ResolveCollision(aabb, movement);
        }

        // IMPORTANT: Only apply sneak edge protection when the player is on the ground
        // This prevents the wonky behavior when jumping while sneaking
        glm::vec3 currentCenter = aabb.GetCenter();
        glm::vec3 currentFeetPos = glm::vec3(currentCenter.x, aabb.min.y, currentCenter.z);

        bool isOnGround = HasSolidGroundBelow(currentFeetPos, PlayerPhysics::WIDTH);

        if (!isOnGround) {
            // Player is in the air (jumping/falling), use normal collision resolution
            return ResolveCollision(aabb, movement);
        }

        // When sneaking AND on ground, handle horizontal movement with sliding
        glm::vec3 horizontalMovement = glm::vec3(resolvedMovement.x, 0.0f, resolvedMovement.z);

        // Check if the combined horizontal movement would cause collision or falling
        if (glm::length(horizontalMovement) > 0.001f) {
            // Test the combined horizontal movement first
            AABB testAABB = aabb;
            testAABB.min.x += horizontalMovement.x;
            testAABB.max.x += horizontalMovement.x;
            testAABB.min.z += horizontalMovement.z;
            testAABB.max.z += horizontalMovement.z;

            bool hasCollision = CheckCollision(testAABB).hasCollision;
            bool wouldFallOff = false;

            if (!hasCollision) {
                // Check if the FINAL combined position would cause player to walk off an edge
                glm::vec3 newFeetPos = aabb.GetCenter() + horizontalMovement;
                newFeetPos.y = aabb.min.y; // Set to feet level

                wouldFallOff = !HasSolidGroundBelow(newFeetPos, PlayerPhysics::WIDTH);
            }

            if (hasCollision) {
                // Normal collision - block all movement
                resolvedMovement.x = 0.0f;
                resolvedMovement.z = 0.0f;
            } else if (wouldFallOff) {
                // Would fall off - try to allow sliding along the edge

                // Try X movement only (sliding left/right)
                bool canMoveX = true;
                if (std::abs(horizontalMovement.x) > 0.001f) {
                    glm::vec3 xOnlyFeetPos = aabb.GetCenter() + glm::vec3(horizontalMovement.x, 0.0f, 0.0f);
                    xOnlyFeetPos.y = aabb.min.y;

                    AABB xTestAABB = aabb;
                    xTestAABB.min.x += horizontalMovement.x;
                    xTestAABB.max.x += horizontalMovement.x;

                    if (CheckCollision(xTestAABB).hasCollision ||
                        !HasSolidGroundBelow(xOnlyFeetPos, PlayerPhysics::WIDTH)) {
                        canMoveX = false;
                    }
                }

                // Try Z movement only (sliding forward/back)
                bool canMoveZ = true;
                if (std::abs(horizontalMovement.z) > 0.001f) {
                    glm::vec3 zOnlyFeetPos = aabb.GetCenter() + glm::vec3(0.0f, 0.0f, horizontalMovement.z);
                    zOnlyFeetPos.y = aabb.min.y;

                    AABB zTestAABB = aabb;
                    zTestAABB.min.z += horizontalMovement.z;
                    zTestAABB.max.z += horizontalMovement.z;

                    if (CheckCollision(zTestAABB).hasCollision ||
                        !HasSolidGroundBelow(zOnlyFeetPos, PlayerPhysics::WIDTH)) {
                        canMoveZ = false;
                    }
                }

                // Apply sliding: allow movement in directions that don't cause falling
                if (!canMoveX) {
                    resolvedMovement.x = 0.0f;
                    Log::Debug("Sneak edge protection: blocked X movement, allowing Z sliding");
                }
                if (!canMoveZ) {
                    resolvedMovement.z = 0.0f;
                    Log::Debug("Sneak edge protection: blocked Z movement, allowing X sliding");
                }

                // If neither direction is safe, block all movement
                if (!canMoveX && !canMoveZ) {
                    resolvedMovement.x = 0.0f;
                    resolvedMovement.z = 0.0f;
                    Log::Debug("Sneak edge protection: blocked all horizontal movement");
                }
            }
        }

        // Y axis (vertical movement) - no sneak protection needed
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

    // NEW: Check if the chunk containing the player is fully loaded
    bool Physics::IsPlayerChunkLoaded(const PlayerPhysics& physics) {
        int chunkX = static_cast<int>(std::floor(physics.position.x / 16.0f));
        int chunkZ = static_cast<int>(std::floor(physics.position.z / 16.0f));

        // Check if the chunk is loaded
        return WorldAccess::IsChunkLoadedAt(
            static_cast<int>(physics.position.x),
            static_cast<int>(physics.position.z)
        );
    }

    void Physics::UpdatePlayerPhysics(PlayerPhysics& physics, const glm::vec3& inputMovement,
                                     bool jumpPressed, bool sneakPressed, float deltaTime) {
        if (physics.noclip) {
            // FIXED: Noclip mode with full 3D movement including flight
            glm::vec3 movement = inputMovement * physics.GetMoveSpeed() * deltaTime;

            // Add vertical movement in noclip
            if (jumpPressed) {
                movement.y += physics.GetMoveSpeed() * deltaTime; // Space = fly up
            }
            if (sneakPressed) {
                movement.y -= physics.GetMoveSpeed() * deltaTime; // Shift = fly down
            }

            physics.position += movement;
            physics.velocity = glm::vec3(0.0f);
            physics.isOnGround = false;
            return;
        }

        // FIXED: Check if player's chunk is loaded before applying physics
        bool chunkLoaded = IsPlayerChunkLoaded(physics);

        if (!chunkLoaded) {
            // Chunk not loaded yet - only allow horizontal movement, no gravity
            glm::vec3 horizontalMovement = glm::vec3(inputMovement.x, 0.0f, inputMovement.z);
            if (glm::length(horizontalMovement) > 0.0f) {
                horizontalMovement = glm::normalize(horizontalMovement);
                horizontalMovement *= physics.GetMoveSpeed() * deltaTime;

                // Apply only horizontal movement (no collision checking since chunk isn't loaded)
                physics.position.x += horizontalMovement.x;
                physics.position.z += horizontalMovement.z;
            }

            // Keep velocity at zero and mark as not on ground
            physics.velocity = glm::vec3(0.0f);
            physics.isOnGround = false;
            physics.isInWater = false;

            // Log periodically that we're waiting for chunk to load
            static int logCounter = 0;
            if (++logCounter % 300 == 0) { // Every 5 seconds at 60 FPS
                Log::Info("Waiting for player chunk to load before enabling physics...");
            }

            return;
        }

        // Chunk is loaded - normal physics processing
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
                speed *= 0.429f; // Slower in water
            }

            horizontalMovement *= speed;
        }

        // Apply movement with collision (now includes sneak edge protection)
        glm::vec3 totalMovement = horizontalMovement * deltaTime +
                                 glm::vec3(0.0f, physics.velocity.y * deltaTime, 0.0f);

        AABB currentAABB = physics.GetAABB();

        // ENHANCED: Use sneak-aware collision resolution
        glm::vec3 resolvedMovement = ResolveCollisionWithSneakProtection(
            currentAABB, totalMovement, physics.isSneaking);

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