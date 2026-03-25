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

        // Check if player is in water (AABB scan, sets waterDepth + isEyeInWater)
        bool wasInWater = physics.isInWater;
        UpdateWaterState(physics, context);

        // Water↔land transitions: preserve momentum
        if (physics.isInWater && !wasInWater) {
            physics.waterVelocity = physics.velocity;
        } else if (!physics.isInWater && wasInWater) {
            physics.velocity.y = physics.waterVelocity.y;
            physics.waterVelocity = glm::vec3(0.0f);
        }

        if (!physics.isInWater) {
            // Land/air physics
            HandleJump(physics, jumpPressed, deltaTime, context);

            if (!physics.noclip) {
                int chunkX = static_cast<int>(std::floor(physics.position.x / Math::CHUNK_SIZE_X));
                int chunkZ = static_cast<int>(std::floor(physics.position.z / Math::CHUNK_SIZE_Z));

                if (context.IsChunkLoaded(chunkX, chunkZ)) {
                    ApplyGravity(physics, deltaTime, context);
                } else {
                    physics.velocity.y = 0.0f;
                    physics.isOnGround = false;
                }
            }
        }

        // Handle movement (water uses per-frame friction model, land unchanged)
        HandleMovement(physics, movementInput, jumpPressed, deltaTime, context);
    }

    void ApplyGravity(PlayerPhysics& physics, float deltaTime, const PhysicsContext& context) {
        // Water gravity is handled in HandleMovement's water branch
        physics.velocity.y += PlayerPhysics::GRAVITY * deltaTime;

        if (physics.velocity.y < PlayerPhysics::TERMINAL_VELOCITY) {
            physics.velocity.y = PlayerPhysics::TERMINAL_VELOCITY;
        }
    }

    void HandleJump(PlayerPhysics& physics, bool jumpPressed, float deltaTime, const PhysicsContext& context) {
        if (physics.noclip) return;
        // Water jump bob is handled in the fixed-tick water loop (HandleMovement)

        // Normal ground jump
        if (jumpPressed && physics.isOnGround) {
            physics.velocity.y = PlayerPhysics::JUMP_VELOCITY;
            physics.isOnGround = false;
            physics.lastJumpTime = physics.totalTime;

            // Handle momentum system for sprinting
            if (physics.isSprinting) {
                if (physics.lastLandingTime > 0.0f) {
                    float timeSinceLanding = physics.lastJumpTime - physics.lastLandingTime;
                    if (timeSinceLanding <= PlayerPhysics::CORRECT_JUMP_TIME_WINDOW) {
                        physics.consecutiveJumps++;
                        float potentialSpeed = physics.baseSpeed + physics.consecutiveJumps * PlayerPhysics::SPEED_INCREMENT;
                        float maxSpeed = physics.baseSpeed * PlayerPhysics::MAX_SPEED_MULTIPLIER;
                        physics.currentSpeed = std::min(potentialSpeed, maxSpeed);
                    } else {
                        physics.consecutiveJumps = 0;
                        physics.currentSpeed = physics.baseSpeed;
                    }
                } else {
                    physics.consecutiveJumps = 0;
                    physics.currentSpeed = physics.baseSpeed;
                }
            } else {
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
                       bool jumpPressed, float deltaTime, const PhysicsContext& context) {

        // Store previous onGround state
        physics.wasOnGround = physics.isOnGround;

        // In noclip mode, allow free movement in all directions including vertical
        if (physics.noclip) {
            glm::vec3 horizontalMovement = glm::vec3(movementInput.x, 0.0f, movementInput.z);
            glm::vec3 verticalMovement = glm::vec3(0.0f, movementInput.y, 0.0f);

            // Ctrl (sprint) boosts noclip speed to 50 blocks/sec
            float hSpeed = physics.isSprinting ? PlayerPhysics::NOCLIP_SPRINT_HORIZONTAL_SPEED : physics.noclipHorizontalSpeed;
            float vSpeed = physics.isSprinting ? PlayerPhysics::NOCLIP_SPRINT_VERTICAL_SPEED : physics.noclipVerticalSpeed;

            if (glm::length(horizontalMovement) > 0.0f) {
                horizontalMovement = glm::normalize(horizontalMovement) * hSpeed;
            }
            verticalMovement *= vSpeed;

            physics.position += (horizontalMovement + verticalMovement) * deltaTime;
            physics.velocity = glm::vec3(0.0f);
            physics.waterVelocity = glm::vec3(0.0f);
            physics.isOnGround = false;
            return;
        }

        if (physics.isInWater) {
            // ============================================================
            // Water movement — per-frame continuous model
            // Uses exponential decay: dv/dt = accel - decay * v
            // Steady state: v_ss = accel / decay
            // Matched to MC steady states: walk=2.0, sink=-0.5, bob=+3.5 b/s
            // ============================================================

            float decay = physics.isSprinting ?
                PlayerPhysics::WATER_SPRINT_DECAY : PlayerPhysics::WATER_DECAY;

            // 1. Horizontal input acceleration
            glm::vec3 inputDir(movementInput.x, 0.0f, movementInput.z);
            if (glm::length(inputDir) > 0.0f) {
                inputDir = glm::normalize(inputDir);
                float accel = physics.isSprinting ?
                    PlayerPhysics::WATER_SPRINT_ACCEL : PlayerPhysics::WATER_WALK_ACCEL;
                physics.waterVelocity.x += inputDir.x * accel * deltaTime;
                physics.waterVelocity.z += inputDir.z * accel * deltaTime;
            }

            // 2. Vertical: gravity pulls down, jump bob pushes up
            physics.waterVelocity.y -= PlayerPhysics::WATER_GRAVITY_ACCEL * deltaTime;
            if (jumpPressed) {
                physics.waterVelocity.y += PlayerPhysics::WATER_BOB_ACCEL * deltaTime;
            }

            // 3. Apply exponential friction decay (all axes)
            float frictionMul = std::exp(-decay * deltaTime);
            physics.waterVelocity.x *= frictionMul;
            physics.waterVelocity.z *= frictionMul;
            physics.waterVelocity.y *= std::exp(-PlayerPhysics::WATER_DECAY * deltaTime);

            // 4. Move with collision
            glm::vec3 movement = physics.waterVelocity * deltaTime;

            // Vertical collision
            glm::vec3 newPosition = physics.position + glm::vec3(0.0f, movement.y, 0.0f);
            if (!CheckCollision(newPosition, physics, context)) {
                physics.position.y = newPosition.y;
                if (movement.y != 0.0f) {
                    physics.isOnGround = false;
                }
            } else {
                if (movement.y < 0.0f) {
                    physics.isOnGround = true;
                    physics.waterVelocity.y = 0.0f;
                }
                if (movement.y > 0.0f) {
                    physics.waterVelocity.y = 0.0f;
                }
            }

            // Ground check when not moving vertically
            if (movement.y == 0.0f) {
                glm::vec3 testPos = physics.position + glm::vec3(0.0f, -0.1f, 0.0f);
                physics.isOnGround = CheckCollision(testPos, physics, context);
            }

            if (!physics.wasOnGround && physics.isOnGround) {
                physics.lastLandingTime = physics.totalTime;
            }

            // Horizontal collision with jump-out-of-fluid
            bool hadHorizontalCollision = false;

            newPosition = physics.position + glm::vec3(movement.x, 0.0f, 0.0f);
            if (!CheckCollision(newPosition, physics, context)) {
                physics.position.x = newPosition.x;
            } else {
                hadHorizontalCollision = true;
                physics.waterVelocity.x = 0.0f;
            }

            newPosition = physics.position + glm::vec3(0.0f, 0.0f, movement.z);
            if (!CheckCollision(newPosition, physics, context)) {
                physics.position.z = newPosition.z;
            } else {
                hadHorizontalCollision = true;
                physics.waterVelocity.z = 0.0f;
            }

            // 5. Jump-out-of-fluid (MC: jumpOutOfFluid)
            // Only trigger at the water surface (partially submerged), not deep underwater.
            // MC checks if the player can move upward to exit the fluid.
            if (hadHorizontalCollision && physics.waterDepth < physics.GetCurrentHeight()) {
                glm::vec3 abovePos = physics.position + glm::vec3(0.0f, 0.6f, 0.0f);
                if (!CheckCollision(abovePos, physics, context)) {
                    physics.waterVelocity.y = PlayerPhysics::WATER_JUMP_OUT;
                }
            }

            // Sync velocity for external use (debug display)
            physics.velocity = physics.waterVelocity;

        } else {
            // ============================================================
            // Normal (land/air) movement — instant speed, no friction
            // ============================================================

            float speed = physics.currentSpeed;

            glm::vec3 horizontalMovement = glm::vec3(movementInput.x, 0.0f, movementInput.z);
            if (glm::length(horizontalMovement) > 0.0f) {
                horizontalMovement = glm::normalize(horizontalMovement) * speed;
            }

            glm::vec3 totalMovement = horizontalMovement + glm::vec3(0.0f, physics.velocity.y, 0.0f);
            glm::vec3 movement = totalMovement * deltaTime;

            // Vertical collision
            glm::vec3 newPosition = physics.position + glm::vec3(0.0f, movement.y, 0.0f);
            if (!CheckCollision(newPosition, physics, context)) {
                physics.position.y = newPosition.y;
                if (movement.y != 0.0f) {
                    physics.isOnGround = false;
                }
            } else {
                if (movement.y < 0.0f) {
                    physics.isOnGround = true;
                    physics.velocity.y = 0.0f;
                }
                if (movement.y > 0.0f) {
                    physics.velocity.y = 0.0f;
                }
            }

            // Ground check when not moving vertically
            if (movement.y == 0.0f) {
                glm::vec3 testPosition = physics.position + glm::vec3(0.0f, -0.1f, 0.0f);
                physics.isOnGround = CheckCollision(testPosition, physics, context);
            }

            if (!physics.wasOnGround && physics.isOnGround) {
                physics.lastLandingTime = physics.totalTime;
            }

            // Sneaking ledge protection
            if (physics.isSneaking && physics.isOnGround) {
                glm::vec3 testPosX = physics.position + glm::vec3(movement.x, 0.0f, 0.0f);
                if (!HasSupportBelow(testPosX, physics, context)) {
                    movement.x = 0.0f;
                }
                glm::vec3 testPosZ = physics.position + glm::vec3(0.0f, 0.0f, movement.z);
                if (!HasSupportBelow(testPosZ, physics, context)) {
                    movement.z = 0.0f;
                }
            }

            // Horizontal collision
            newPosition = physics.position + glm::vec3(movement.x, 0.0f, 0.0f);
            if (!CheckCollision(newPosition, physics, context)) {
                physics.position.x = newPosition.x;
            }

            newPosition = physics.position + glm::vec3(0.0f, 0.0f, movement.z);
            if (!CheckCollision(newPosition, physics, context)) {
                physics.position.z = newPosition.z;
            }
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

    void UpdateWaterState(PlayerPhysics& physics, const PhysicsContext& context) {
        // Scan the player's AABB (deflated by 0.001 like Minecraft) for water blocks.
        // Track the highest water surface touching the player to compute waterDepth.
        float height = physics.GetCurrentHeight();
        float halfWidth = PlayerPhysics::WIDTH * 0.5f - 0.001f;
        float feetY = physics.position.y + 0.001f;
        float topY = physics.position.y + height - 0.001f;

        int minX = static_cast<int>(std::floor(physics.position.x - halfWidth));
        int maxX = static_cast<int>(std::floor(physics.position.x + halfWidth));
        int minY = static_cast<int>(std::floor(feetY));
        int maxY = static_cast<int>(std::floor(topY));
        int minZ = static_cast<int>(std::floor(physics.position.z - halfWidth));
        int maxZ = static_cast<int>(std::floor(physics.position.z + halfWidth));

        float highestWaterSurface = 0.0f;
        bool foundWater = false;

        for (int x = minX; x <= maxX; x++) {
            for (int z = minZ; z <= maxZ; z++) {
                for (int y = minY; y <= maxY; y++) {
                    try {
                        if (context.GetBlock(x, y, z) == BlockID::Water) {
                            // Water surface is at the top of this block
                            // (source blocks fill to ~0.9, but treat as full block for physics)
                            float waterTop = static_cast<float>(y + 1);
                            if (waterTop > highestWaterSurface) {
                                highestWaterSurface = waterTop;
                            }
                            foundWater = true;
                        }
                    } catch (...) {}
                }
            }
        }

        physics.isInWater = foundWater;
        if (foundWater) {
            physics.waterDepth = std::max(0.0f, highestWaterSurface - physics.position.y);
            physics.waterDepth = std::min(physics.waterDepth, height); // Clamp to player height
        } else {
            physics.waterDepth = 0.0f;
        }

        // Check if eyes are submerged
        float eyeY = physics.GetEyePosition().y;
        int eyeBlockX = static_cast<int>(std::floor(physics.position.x));
        int eyeBlockY = static_cast<int>(std::floor(eyeY));
        int eyeBlockZ = static_cast<int>(std::floor(physics.position.z));
        try {
            physics.isEyeInWater = (context.GetBlock(eyeBlockX, eyeBlockY, eyeBlockZ) == BlockID::Water);
        } catch (...) {
            physics.isEyeInWater = false;
        }
    }

} // namespace Game