// File: src/common/physics/Physics.cpp
#include "Physics.hpp"
#include "common/world//block/BlockRegistry.hpp"
#include "common/core/Log.hpp"
#include "common/core/Config.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include <algorithm>
#include <cmath>

namespace Game {

    // **NEW**: PhysicsContext implementation
    BlockID PhysicsContext::GetBlock(int x, int y, int z) const {
        if (!blockAccess) {
            Log::Warning("No block access available in PhysicsContext");
            return BlockID::Air;
        }
        return blockAccess->GetBlock(x, y, z);
    }

    bool PhysicsContext::IsBlockSolid(int x, int y, int z) const {
        if (!blockAccess) {
            return false;
        }
        return blockAccess->IsBlockSolid(x, y, z);
    }

    bool PhysicsContext::IsChunkLoaded(int chunkX, int chunkZ) const {
        if (!blockAccess) {
            return false;
        }
        return blockAccess->IsChunkLoaded(chunkX, chunkZ);
    }

    // **NEW**: Main physics update function with PhysicsContext
    void UpdatePlayerPhysics(PlayerPhysics& physics,
                            const glm::vec3& movementInput,
                            bool jumpPressed,
                            bool sneakPressed,
                            float deltaTime,
                            const PhysicsContext& context) {

        physics.totalTime += deltaTime;

        // Update sneaking state
        physics.isSneaking = sneakPressed;

        // Update base speed based on current state
        UpdateBaseSpeed(physics);

        // Check if player is in water
        physics.isInWater = IsInWater(physics.position, context);

        // Handle jumping
        HandleJump(physics, jumpPressed, context);

        // Only apply gravity if not in noclip mode AND the chunk below player is loaded
        if (!physics.noclip) {
            // Check if the chunk containing the player is loaded before applying gravity
            int chunkX = static_cast<int>(std::floor(physics.position.x / Math::CHUNK_SIZE_X));
            int chunkZ = static_cast<int>(std::floor(physics.position.z / Math::CHUNK_SIZE_Z));

            bool chunkLoaded = context.IsChunkLoaded(chunkX, chunkZ);

            if (chunkLoaded) {
                ApplyGravity(physics, deltaTime, context);
            } else {
                // Chunk not loaded yet - don't apply gravity to prevent falling through world
                physics.velocity.y = 0.0f;
                physics.isOnGround = false;

                // Log this occasionally for debugging
                static int gravityDelayCounter = 0;
                if (++gravityDelayCounter % 60 == 0) { // Every second at 60fps
                    Log::Debug("Delaying gravity - chunk not loaded at player position (%.1f, %.1f)",
                              physics.position.x, physics.position.z);
                }
            }
        }

        // Handle movement
        HandleMovement(physics, movementInput, deltaTime, context);
    }

    void ApplyGravity(PlayerPhysics& physics, float deltaTime, const PhysicsContext& context) {
        // Use different gravity if in water
        float currentGravity = physics.isInWater ? PlayerPhysics::WATER_GRAVITY : PlayerPhysics::GRAVITY;
        physics.velocity.y += currentGravity * deltaTime;

        // Clamp vertical velocity to terminal velocity
        float terminalVelocity = physics.isInWater ? PlayerPhysics::WATER_GRAVITY : PlayerPhysics::TERMINAL_VELOCITY;
        if (physics.velocity.y < terminalVelocity) {
            physics.velocity.y = terminalVelocity;
        }
    }

    void HandleJump(PlayerPhysics& physics, bool jumpPressed, const PhysicsContext& context) {
        if (jumpPressed && physics.isOnGround && !physics.noclip) {
            float jumpVelocity = physics.isInWater ? PlayerPhysics::WATER_JUMP_VELOCITY : PlayerPhysics::JUMP_VELOCITY;
            physics.velocity.y = jumpVelocity;
            physics.isOnGround = false;
            physics.lastJumpTime = physics.totalTime;

            // Handle momentum system for sprinting
            if (physics.isSprinting) {
                if (physics.lastLandingTime > 0.0f) {
                    float timeSinceLanding = physics.lastJumpTime - physics.lastLandingTime;
                    if (timeSinceLanding <= PlayerPhysics::CORRECT_JUMP_TIME_WINDOW) {
                        // Correctly timed jump while sprinting
                        physics.consecutiveJumps++;
                        float potentialSpeed = physics.baseSpeed + physics.consecutiveJumps * PlayerPhysics::SPEED_INCREMENT;
                        float maxSpeed = physics.baseSpeed * PlayerPhysics::MAX_SPEED_MULTIPLIER;
                        physics.currentSpeed = std::min(potentialSpeed, maxSpeed);
                        Log::Debug("Momentum increased! Current speed: %.2f", physics.currentSpeed);
                    } else {
                        // Incorrect timing or delay too long
                        physics.consecutiveJumps = 0;
                        physics.currentSpeed = physics.baseSpeed;
                        Log::Debug("Momentum reset. Incorrect jump timing while sprinting.");
                    }
                } else {
                    // First jump while sprinting
                    physics.consecutiveJumps = 0;
                    physics.currentSpeed = physics.baseSpeed;
                }
            } else {
                // Not sprinting, reset momentum
                physics.consecutiveJumps = 0;
                physics.currentSpeed = physics.baseSpeed;
            }
        }
    }

    void UpdateBaseSpeed(PlayerPhysics& physics) {
        if (physics.isSneaking) {
            physics.baseSpeed = PlayerPhysics::SNEAK_SPEED;
        } else if (physics.isSprinting) {
            physics.baseSpeed = PlayerPhysics::SPRINT_SPEED;
        } else {
            physics.baseSpeed = PlayerPhysics::WALK_SPEED;
        }

        // Reset current speed when changing movement modes
        if (!physics.isSprinting) {
            physics.currentSpeed = physics.baseSpeed;
        }
    }

    void HandleMovement(PlayerPhysics& physics, const glm::vec3& movementInput,
                       float deltaTime, const PhysicsContext& context) {

        // Store previous onGround state
        physics.wasOnGround = physics.isOnGround;

        // Calculate movement speed
        float speed = physics.isInWater ? PlayerPhysics::WATER_WALK_SPEED : physics.currentSpeed;

        // In noclip mode, allow free movement in all directions including vertical
        if (physics.noclip) {
            // Apply separate speeds for horizontal and vertical movement
            glm::vec3 horizontalMovement = glm::vec3(movementInput.x, 0.0f, movementInput.z);
            glm::vec3 verticalMovement = glm::vec3(0.0f, movementInput.y, 0.0f);
            
            // Apply horizontal speed
            if (glm::length(horizontalMovement) > 0.0f) {
                horizontalMovement = glm::normalize(horizontalMovement) * physics.noclipHorizontalSpeed;
            }
            
            // Apply vertical speed
            verticalMovement *= physics.noclipVerticalSpeed;
            
            // Combine movements
            glm::vec3 totalMovement = horizontalMovement + verticalMovement;
            physics.position += totalMovement * deltaTime;
            physics.velocity = glm::vec3(0.0f); // No velocity in noclip
            physics.isOnGround = false;
            return;
        }

        // Normal physics mode - separate horizontal and vertical movement
        glm::vec3 horizontalMovement = glm::vec3(movementInput.x, 0.0f, movementInput.z);
        if (glm::length(horizontalMovement) > 0.0f) {
            horizontalMovement = glm::normalize(horizontalMovement) * speed;
        }

        // Apply movement with collision detection
        glm::vec3 totalMovement = horizontalMovement + glm::vec3(0.0f, physics.velocity.y, 0.0f);
        glm::vec3 movement = totalMovement * deltaTime;

        // Handle vertical movement first to update onGround
        glm::vec3 newPosition = physics.position + glm::vec3(0.0f, movement.y, 0.0f);

        if (!CheckCollision(newPosition, physics, context)) {
            physics.position.y = newPosition.y;
            if (movement.y != 0.0f) {
                physics.isOnGround = false; // If we're moving vertically and no collision, we're in the air
            }
        } else {
            if (movement.y < 0.0f) {
                physics.isOnGround = true; // Landed
                physics.velocity.y = 0.0f;
            }
            if (movement.y > 0.0f) {
                physics.velocity.y = 0.0f; // Hit ceiling
            }
        }

        // Additional check: If movement.y == 0, we need to check if the player is on the ground
        if (movement.y == 0.0f) {
            glm::vec3 testPosition = physics.position + glm::vec3(0.0f, -0.1f, 0.0f);
            if (CheckCollision(testPosition, physics, context)) {
                physics.isOnGround = true;
            } else {
                physics.isOnGround = false;
            }
        }

        // Check if the player just landed
        if (!physics.wasOnGround && physics.isOnGround) {
            physics.lastLandingTime = physics.totalTime;
        }

        // Apply movement restrictions based on sneaking and onGround
        if (physics.isSneaking && physics.isOnGround) {
            // Check X axis movement
            glm::vec3 testPosX = physics.position + glm::vec3(movement.x, 0.0f, 0.0f);
            if (!HasSupportBelow(testPosX, physics, context)) {
                movement.x = 0.0f; // No support below, prevent movement along X
            }

            // Check Z axis movement
            glm::vec3 testPosZ = physics.position + glm::vec3(0.0f, 0.0f, movement.z);
            if (!HasSupportBelow(testPosZ, physics, context)) {
                movement.z = 0.0f; // No support below, prevent movement along Z
            }
        }

        // Handle horizontal movement with collision detection
        // X axis
        newPosition = physics.position + glm::vec3(movement.x, 0.0f, 0.0f);
        if (!CheckCollision(newPosition, physics, context)) {
            physics.position.x = newPosition.x;
        }

        // Z axis
        newPosition = physics.position + glm::vec3(0.0f, 0.0f, movement.z);
        if (!CheckCollision(newPosition, physics, context)) {
            physics.position.z = newPosition.z;
        }
    }

    bool CheckCollision(const glm::vec3& position, const PlayerPhysics& physics,
                       const PhysicsContext& context) {

        // Create AABB at the new position
        float height = physics.GetCurrentHeight();
        AABB playerAABB(
            glm::vec3(position.x, position.y + height * 0.5f, position.z),
            glm::vec3(PlayerPhysics::WIDTH, height, PlayerPhysics::WIDTH)
        );

        // Check blocks that the player could be colliding with
        int minX = static_cast<int>(std::floor(playerAABB.min.x));
        int maxX = static_cast<int>(std::floor(playerAABB.max.x));
        int minY = static_cast<int>(std::floor(playerAABB.min.y));
        int maxY = static_cast<int>(std::floor(playerAABB.max.y));
        int minZ = static_cast<int>(std::floor(playerAABB.min.z));
        int maxZ = static_cast<int>(std::floor(playerAABB.max.z));

        for (int x = minX; x <= maxX; x++) {
            for (int y = minY; y <= maxY; y++) {
                for (int z = minZ; z <= maxZ; z++) {
                    if (context.IsBlockSolid(x, y, z)) {
                        // Block is solid, check AABB intersection
                        AABB blockAABB(
                            glm::vec3(x + 0.5f, y + 0.5f, z + 0.5f),
                            glm::vec3(1.0f, 1.0f, 1.0f)
                        );

                        if (playerAABB.Intersects(blockAABB)) {
                            return true; // Collision detected
                        }
                    }
                }
            }
        }

        return false; // No collision
    }

    bool HasSupportBelow(const glm::vec3& position, const PlayerPhysics& physics,
                        const PhysicsContext& context) {

        float halfWidth = PlayerPhysics::WIDTH / 2.0f;
        float offsets[] = { -halfWidth + PlayerPhysics::OVERHANG_MARGIN,
                           halfWidth - PlayerPhysics::OVERHANG_MARGIN };

        for (float xOffset : offsets) {
            for (float zOffset : offsets) {
                glm::vec3 cornerPosition(
                    position.x + xOffset,
                    position.y - 0.1f, // Slightly below the player's feet
                    position.z + zOffset
                );

                int blockX = static_cast<int>(std::floor(cornerPosition.x));
                int blockY = static_cast<int>(std::floor(cornerPosition.y));
                int blockZ = static_cast<int>(std::floor(cornerPosition.z));

                if (context.IsBlockSolid(blockX, blockY, blockZ)) {
                    return true;
                }
            }
        }

        return false;
    }

    bool IsInWater(const glm::vec3& position, const PhysicsContext& context) {
        // Check if the player's position contains water
        int blockX = static_cast<int>(std::floor(position.x));
        int blockY = static_cast<int>(std::floor(position.y));
        int blockZ = static_cast<int>(std::floor(position.z));

        // Check if the block at player position is water
        try {
            BlockID blockId = context.GetBlock(blockX, blockY, blockZ);
            return blockId == BlockID::Water;
        } catch (...) {
            return false;
        }
    }

} // namespace Game