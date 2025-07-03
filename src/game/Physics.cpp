// File: src/game/Physics.cpp
#include "Physics.hpp"
#include "WorldAccess.hpp"
#include "BlockRegistry.hpp"
#include "../core/Log.hpp"
#include "../core/Config.hpp"
#include <algorithm>
#include <cmath>

namespace Game {

    void UpdatePlayerPhysics(PlayerPhysics& physics,
                            const glm::vec3& movementInput,
                            bool jumpPressed,
                            bool sneakPressed,
                            float deltaTime,
                            BlockCollisionFunction blockCollisionCheck) {

        // Use default collision checker if none provided
        if (!blockCollisionCheck) {
            blockCollisionCheck = DefaultBlockCollisionCheck;
        }

        physics.totalTime += deltaTime;

        // Update sneaking state
        physics.isSneaking = sneakPressed;

        // Update base speed based on current state
        UpdateBaseSpeed(physics);

        // Check if player is in water
        physics.isInWater = IsInWater(physics.position, blockCollisionCheck);

        // Handle jumping
        HandleJump(physics, jumpPressed, blockCollisionCheck);

        // Apply gravity (unless in noclip mode)
        if (!physics.noclip) {
            ApplyGravity(physics, deltaTime);
        }

        // Handle movement
        HandleMovement(physics, movementInput, deltaTime, blockCollisionCheck);
    }

    void ApplyGravity(PlayerPhysics& physics, float deltaTime) {
        // Use different gravity if in water
        float currentGravity = physics.isInWater ? PlayerPhysics::WATER_GRAVITY : PlayerPhysics::GRAVITY;
        physics.velocity.y += currentGravity * deltaTime;

        // Clamp vertical velocity to terminal velocity
        float terminalVelocity = physics.isInWater ? PlayerPhysics::WATER_GRAVITY : PlayerPhysics::TERMINAL_VELOCITY;
        if (physics.velocity.y < terminalVelocity) {
            physics.velocity.y = terminalVelocity;
        }
    }

    void HandleJump(PlayerPhysics& physics, bool jumpPressed, BlockCollisionFunction blockCollisionCheck) {
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
                       float deltaTime, BlockCollisionFunction blockCollisionCheck) {

        // Store previous onGround state
        physics.wasOnGround = physics.isOnGround;

        // Calculate movement speed
        float speed = physics.isInWater ? PlayerPhysics::WATER_WALK_SPEED : physics.currentSpeed;

        // Apply horizontal movement
        glm::vec3 horizontalMovement = glm::vec3(movementInput.x, 0.0f, movementInput.z);
        if (glm::length(horizontalMovement) > 0.0f) {
            horizontalMovement = glm::normalize(horizontalMovement) * speed;
        }

        // In noclip mode, allow free movement in all directions
        glm::vec3 totalMovement;
        if (physics.noclip) {
            totalMovement = movementInput * speed;
            physics.position += totalMovement * deltaTime;
            physics.velocity = glm::vec3(0.0f); // No velocity in noclip
            physics.isOnGround = false;
            return;
        }

        // Apply movement with collision detection
        totalMovement = horizontalMovement + glm::vec3(0.0f, physics.velocity.y, 0.0f);
        glm::vec3 movement = totalMovement * deltaTime;

        // Handle vertical movement first to update onGround
        glm::vec3 newPosition = physics.position + glm::vec3(0.0f, movement.y, 0.0f);

        if (!CheckCollision(newPosition, physics, blockCollisionCheck)) {
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
            if (CheckCollision(testPosition, physics, blockCollisionCheck)) {
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
            if (!HasSupportBelow(testPosX, physics, blockCollisionCheck)) {
                movement.x = 0.0f; // No support below, prevent movement along X
            }

            // Check Z axis movement
            glm::vec3 testPosZ = physics.position + glm::vec3(0.0f, 0.0f, movement.z);
            if (!HasSupportBelow(testPosZ, physics, blockCollisionCheck)) {
                movement.z = 0.0f; // No support below, prevent movement along Z
            }
        }

        // Handle horizontal movement with collision detection
        // X axis
        newPosition = physics.position + glm::vec3(movement.x, 0.0f, 0.0f);
        if (!CheckCollision(newPosition, physics, blockCollisionCheck)) {
            physics.position.x = newPosition.x;
        }

        // Z axis
        newPosition = physics.position + glm::vec3(0.0f, 0.0f, movement.z);
        if (!CheckCollision(newPosition, physics, blockCollisionCheck)) {
            physics.position.z = newPosition.z;
        }
    }

    bool CheckCollision(const glm::vec3& position, const PlayerPhysics& physics,
                       BlockCollisionFunction blockCollisionCheck) {

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
                    if (blockCollisionCheck(x, y, z)) {
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
                        BlockCollisionFunction blockCollisionCheck) {

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

                if (blockCollisionCheck(blockX, blockY, blockZ)) {
                    return true;
                }
            }
        }

        return false;
    }

    bool IsInWater(const glm::vec3& position, BlockCollisionFunction blockCollisionCheck) {
        // For simplicity, we'll check if the player's position contains water
        // In a more complete implementation, you'd check the specific block type
        int blockX = static_cast<int>(std::floor(position.x));
        int blockY = static_cast<int>(std::floor(position.y));
        int blockZ = static_cast<int>(std::floor(position.z));

        // Check if the block at player position is water
        try {
            BlockID blockId = WorldAccess::GetBlock(blockX, blockY, blockZ);
            return blockId == BlockID::Water;
        } catch (...) {
            return false;
        }
    }

    bool DefaultBlockCollisionCheck(int x, int y, int z) {
        try {
            BlockID blockId = WorldAccess::GetBlock(x, y, z);
            if (blockId == BlockID::Air) {
                return false;
            }

            const Block& block = BlockRegistry::Get(blockId);
            return block.opaque; // Use opaque flag as solid indicator
        } catch (...) {
            // If we can't access the block, assume it's solid for safety
            return true;
        }
    }

} // namespace Game