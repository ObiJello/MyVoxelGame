// File: src/common/physics/Physics.cpp
#include "Physics.hpp"
#include "common/world//block/BlockRegistry.hpp"
#include "common/core/Log.hpp"
#include "common/core/Config.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Game {

    // **NEW**: PhysicsContext implementation
    BlockID PhysicsContext::GetBlock(int x, int y, int z) const {
        if (!blockAccess) {
            Log::Warning("No block access available in PhysicsContext");
            return BlockID::Air;
        }
        return blockAccess->GetBlock(x, y, z);
    }

    BlockState PhysicsContext::GetBlockState(int x, int y, int z) const {
        return blockAccess ? blockAccess->GetBlockState(x, y, z) : BlockState{};
    }

    namespace { PortalPassthroughFn g_portalPassthrough = nullptr; }
    void SetPortalPassthroughFn(PortalPassthroughFn fn) {
        g_portalPassthrough = fn;
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

    bool PhysicsContext::ContainsWater(int x, int y, int z) const {
        return blockAccess && blockAccess->ContainsWater(x, y, z);
    }

    bool IsCollisionShapeFullBlock(const PhysicsContext& context, int x, int y, int z) {
        const BlockID bid = context.GetBlock(x, y, z);
        if (!BlockRegistry::HasCollision(bid)) return false;
        return BlockRegistry::GetBlockCollisionShapeSet(
                   context.GetBlockState(x, y, z)).IsFullCube();
    }

    double FluidHeightAbove(const AABB& box, bool lava, const PhysicsContext& context) {
        // MC updateFluidHeightAndDoFluidPushing deflates by 0.001 so a box
        // exactly flush with a fluid cell's face doesn't count as inside it.
        const double minX = box.min.x + 0.001, maxX = box.max.x - 0.001;
        const double minY = box.min.y + 0.001, maxY = box.max.y - 0.001;
        const double minZ = box.min.z + 0.001, maxZ = box.max.z - 0.001;

        const auto isFluid = [&](int x, int y, int z) {
            return lava ? context.GetBlock(x, y, z) == BlockID::Lava
                        : context.ContainsWater(x, y, z);
        };

        // FlowingFluid.getOwnHeight for a source block — 8/9 of the cell.
        constexpr double kSourceHeight = 8.0 / 9.0;

        const int x0 = static_cast<int>(std::floor(minX));
        const int x1 = static_cast<int>(std::floor(maxX));
        const int y0 = static_cast<int>(std::floor(minY));
        const int y1 = static_cast<int>(std::floor(maxY));
        const int z0 = static_cast<int>(std::floor(minZ));
        const int z1 = static_cast<int>(std::floor(maxZ));

        double height = 0.0;
        for (int x = x0; x <= x1; ++x) {
            for (int y = y0; y <= y1; ++y) {
                for (int z = z0; z <= z1; ++z) {
                    if (!isFluid(x, y, z)) continue;
                    // A fluid cell with the same fluid overhead is a
                    // continuous column — MC FluidState.getHeight returns
                    // 1.0 for it.
                    const double surface = isFluid(x, y + 1, z)
                        ? static_cast<double>(y) + 1.0
                        : static_cast<double>(y) + kSourceHeight;
                    height = std::max(height, surface - box.min.y);
                }
            }
        }
        return height;
    }

    void EscapeTowardsClosestSpace(const glm::dvec3& stuckPoint, float escapeSpeed,
                                   glm::dvec3& velocity, const PhysicsContext& context) {
        const int bx = static_cast<int>(std::floor(stuckPoint.x));
        const int by = static_cast<int>(std::floor(stuckPoint.y));
        const int bz = static_cast<int>(std::floor(stuckPoint.z));
        const glm::dvec3 frac(stuckPoint.x - bx, stuckPoint.y - by, stuckPoint.z - bz);

        // MC's candidate order: NORTH, SOUTH, WEST, EAST, UP — never down.
        // {axis (0=X,1=Y,2=Z), step}
        struct Candidate { int axis; int step; };
        constexpr Candidate kDirections[] = {
            {2, -1},   // north (−Z)
            {2, +1},   // south (+Z)
            {0, -1},   // west  (−X)
            {0, +1},   // east  (+X)
            {1, +1},   // up
        };

        // Nearest open face wins. MC's fallback when every neighbour is a full
        // block is UP (closestDirection initialises to Direction.UP).
        int    bestAxis = 1;
        int    bestStep = +1;
        double closest  = std::numeric_limits<double>::max();

        for (const auto& dir : kDirections) {
            const int nx = bx + (dir.axis == 0 ? dir.step : 0);
            const int ny = by + (dir.axis == 1 ? dir.step : 0);
            const int nz = bz + (dir.axis == 2 ? dir.step : 0);
            if (IsCollisionShapeFullBlock(context, nx, ny, nz)) continue;

            const double d = frac[dir.axis];
            const double oriented = dir.step > 0 ? 1.0 - d : d;
            if (oriented < closest) {
                closest  = oriented;
                bestAxis = dir.axis;
                bestStep = dir.step;
            }
        }

        // The stuck axis gets the full escape speed; the others keep 0.75 of
        // whatever they had (Entity.java moveTowardsClosestSpace).
        glm::dvec3 scaled = velocity * 0.75;
        scaled[bestAxis] = static_cast<double>(bestStep) *
                           static_cast<double>(escapeSpeed);
        velocity = scaled;
    }

    // **NEW**: Main physics update function with PhysicsContext
    void UpdatePlayerPhysics(PlayerPhysics& physics,
                            const glm::vec3& movementInput,
                            bool jumpPressed,
                            bool sneakPressed,
                            float deltaTime,
                            const PhysicsContext& context) {

        physics.totalTime += deltaTime;

        // Decay the step-up visual offset back toward zero. MC's
        // Camera.setup interpolates `entity.yo → entity.getY()` over the
        // partial-tick interval (Camera.java:85) — a one-tick linear lerp.
        // We approximate the same look with exponential decay tuned so the
        // remaining offset is ~5% after one MC tick (50 ms) and ~0.25% after
        // two ticks. tau ≈ tick / ln(20) = 0.05 / 3.0 ≈ 0.0166s ⇒
        // decay = exp(-dt / tau). Capped at 0 so it never overshoots.
        if (physics.stepVisualOffset < 0.0f) {
            constexpr float kStepDecayTau = 0.0166f;
            const float decay = std::exp(-deltaTime / kStepDecayTau);
            physics.stepVisualOffset *= decay;
            if (physics.stepVisualOffset > -1.0e-4f) {
                physics.stepVisualOffset = 0.0f;
            }
        }

        // Update sneaking state. Not while flying — MC's isCrouching requires
        // !abilities.flying (shift descends instead of crouching mid-flight).
        physics.isSneaking = sneakPressed && !physics.isFlying;

        // Per-step jump flag (consumed by ClientPlayer for move-packet stats)
        physics.didJumpThisStep = false;

        // Fall tracking: remember the pre-step height (delta taken below).
        const float fallPrevY = physics.position.y;

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

        if (!physics.isInWater && !physics.isFlying) {
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

        // Landing cancels creative flight — MC LocalPlayer.aiStep:845
        // (onGround && abilities.flying && !isSpectator → flying = false).
        if (physics.isFlying && physics.isOnGround) {
            physics.isFlying = false;
        }

        // Fall tracking (see the field comment in Physics.hpp). Order
        // matters: the water/flight/noclip reset wins over a same-step
        // landing so falling into a pool never flushes damage.
        {
            // MC Entity.checkFallDamage:1481-1495 accumulates UNCONDITIONALLY
            // and only THEN tests onGround. The descent on the landing step is
            // real distance fallen — clipping it out of an `!isOnGround` guard
            // loses most of the final block, and CalculateFallDamage floors, so
            // the shortfall shows up as a whole missing damage point on every
            // integer-height drop. Mirrors Entity::CheckFallDamage.
            const float fallDy = physics.position.y - fallPrevY;
            if (physics.isInWater || physics.isFlying || physics.noclip) {
                physics.fallDistance = 0.0f;      // MC resetFallDistance
            } else if (fallDy < 0.0f) {
                physics.fallDistance += -fallDy;
            }
            if (physics.isOnGround) {
                if (physics.fallDistance > 0.0f) {
                    physics.landedFallDistance =
                        std::max(physics.landedFallDistance, physics.fallDistance);
                }
                physics.fallDistance = 0.0f;
            }
        }
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

        // Stuck-in-block escape: if the player's AABB currently overlaps
        // a solid block (clipped into a wall, server placed a block on
        // them, etc.) they normally can't move OR jump because
        // isOnGround stays false. Vanilla lets you jump-out anyway —
        // give a full jump impulse on rising edge so the player can
        // escape upward one block at a time.
        const bool stuckInBlock =
            jumpPressed && !physics.isOnGround &&
            CheckCollision(physics.position, physics, context);
        if (stuckInBlock) {
            physics.velocity.y = PlayerPhysics::JUMP_VELOCITY;
            physics.lastJumpTime = physics.totalTime;
            return;
        }

        // Normal ground jump
        if (jumpPressed && physics.isOnGround) {
            physics.velocity.y = PlayerPhysics::JUMP_VELOCITY;
            physics.isOnGround = false;
            physics.lastJumpTime = physics.totalTime;
            physics.didJumpThisStep = true;

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
        // Sneaking deliberately does NOT appear here. In MC the two are
        // different mechanisms: sprinting is a MOVEMENT_SPEED modifier
        // (SPEED_MODIFIER_SPRINTING, ×1.3 — LivingEntity.java:2162-2170),
        // while crouching scales the movement INPUT by Attributes.SNEAKING_SPEED
        // in LocalPlayer.modifyInput and leaves MOVEMENT_SPEED alone. Anything
        // reading currentSpeed as MC's MOVEMENT_SPEED analogue depends on that
        // split — notably the speed-driven FOV in PlatformMain, which used to
        // zoom IN while sneaking because this function folded the two together.
        // The sneak scale is applied to the movement vector in HandleMovement.
        physics.baseSpeed = physics.isSprinting ? PlayerPhysics::SPRINT_SPEED
                                                : PlayerPhysics::WALK_SPEED;

        // Reset current speed when changing movement modes
        if (!physics.isSprinting) {
            physics.currentSpeed = physics.baseSpeed;
        } else if (physics.currentSpeed < physics.baseSpeed) {
            // Sprint just started (or baseSpeed rose out from under a stale
            // currentSpeed). MC applies SPEED_MODIFIER_SPRINTING — a ×1.3
            // ADD_MULTIPLIED_TOTAL modifier on MOVEMENT_SPEED — the moment
            // setSprinting(true) runs (LivingEntity.java:2165-2167), so the
            // speed-up is immediate on the ground with no jump involved.
            //
            // Previously currentSpeed was only ever written by HandleJump, so
            // tapping sprint while walking changed baseSpeed but left
            // currentSpeed at WALK_SPEED — the movement code reads
            // currentSpeed, so nothing happened until you jumped.
            //
            // Raising it only when it's BELOW baseSpeed leaves the
            // consecutive-jump momentum bonus (which pushes currentSpeed
            // above baseSpeed) completely untouched.
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

        if (physics.isInWater && !physics.isFlying) {
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

            // Vertical collision — snap to collision boundary (mirrors
            // MC's Entity.collide()/Shapes.collide which return the
            // exact maximum-allowed movement). Without snapping, the
            // player would stop at the pre-frame Y, leaving them
            // floating slightly above the floor; the next ~5 frames of
            // gravity would drift them down — visible as a "land,
            // stall, drift" stutter.
            glm::vec3 newPosition = physics.position + glm::vec3(0.0f, movement.y, 0.0f);
            if (!CheckCollision(newPosition, physics, context)) {
                physics.position.y = newPosition.y;
                if (movement.y != 0.0f) {
                    physics.isOnGround = false;
                }
            } else {
                float lo = newPosition.y;        // colliding endpoint
                float hi = physics.position.y;   // last frame's resting Y (assumed safe)
                glm::vec3 testPos = physics.position;
                for (int i = 0; i < 10; ++i) {   // 10 iter ≈ 1024× precision
                    const float mid = (lo + hi) * 0.5f;
                    testPos.y = mid;
                    if (CheckCollision(testPos, physics, context)) {
                        lo = mid;
                    } else {
                        hi = mid;
                    }
                }
                physics.position.y = hi;
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

            if (physics.isFlying) {
                // Creative flight: MC speeds, full collision, no gravity
                // (ApplyGravity/HandleJump are skipped upstream).
                const float sprintMul = physics.isSprinting
                                      ? PlayerPhysics::FLY_SPRINT_MULTIPLIER : 1.0f;
                speed = PlayerPhysics::FLY_HORIZONTAL_SPEED * sprintMul;
                // Direct vertical control: Space up / Shift down. Use the
                // input's sign — CalculateMovementInput normalizes the whole
                // vector, so the raw y magnitude shrinks when combined with
                // WASD. Instant stop on release matches this codebase's
                // no-friction land model.
                //
                // DELIBERATE MC DEVIATION: vanilla only sprint-doubles the
                // horizontal (Player.getFlyingSpeed, Player.java:1877-1879);
                // the vertical impulse reads the undoubled ability value
                // (LocalPlayer.java:812 — `abilities.getFlyingSpeed() * 3`),
                // so sprint-ascending in MC is no faster than a walk. We
                // apply the same multiplier to the vertical so sprint scales
                // flight uniformly in every direction.
                const float vert = movementInput.y > 0.01f ? 1.0f
                                 : movementInput.y < -0.01f ? -1.0f : 0.0f;
                physics.velocity.y = vert * PlayerPhysics::FLY_VERTICAL_SPEED * sprintMul;
            }

            glm::vec3 horizontalMovement = glm::vec3(movementInput.x, 0.0f, movementInput.z);
            if (glm::length(horizontalMovement) > 0.0f) {
                horizontalMovement = glm::normalize(horizontalMovement) * speed;
            }

            // Sneak: MC LocalPlayer.modifyInput scales the movement input by
            // Attributes.SNEAKING_SPEED and never touches MOVEMENT_SPEED, so
            // crouching slows you without widening or narrowing the FOV.
            //
            // MC applies it to the raw input vector; this port normalises the
            // input and multiplies by a speed constant just above, which would
            // discard any scaling done up front — so the multiply lands here
            // instead. Same result, since the input direction is unit-length
            // by the time it gets here.
            //
            // isSneaking is already `sneakPressed && !isFlying` (set at the top
            // of UpdatePlayerPhysics), matching MC's isCrouching requiring
            // !abilities.flying, so shift-descending in creative is unaffected.
            if (physics.isSneaking) {
                horizontalMovement *= PlayerPhysics::SNEAKING_SPEED;
            }

            // Add residual horizontal velocity (set by portal teleports
            // when src=floor/ceiling and dst=wall — the player's vertical
            // fall velocity gets rotated into horizontal exit velocity).
            // Without this the velocity field is ignored and the player
            // just stands at the wall portal exit.
            horizontalMovement.x += physics.velocity.x;
            horizontalMovement.z += physics.velocity.z;

            glm::vec3 totalMovement = horizontalMovement + glm::vec3(0.0f, physics.velocity.y, 0.0f);
            glm::vec3 movement = totalMovement * deltaTime;

            // Vertical collision — snap to collision boundary (mirrors
            // MC's Entity.collide()/Shapes.collide). See the matching
            // comment in the water-physics branch above for the bug
            // this prevents.
            //
            // Stuck-in-block escape: if the player's current AABB is
            // already overlapping a solid (clipped into a wall, server
            // placed a block on them, …), the binary-search snap below
            // would oscillate between two colliding positions and end
            // up snapping back to where they started — killing the jump
            // impulse. When that's the case, just accept the upward
            // displacement so the player can climb out one jump at a
            // time. We only relax the rule for upward motion; downward
            // still snaps so they don't fall through the world.
            const bool currentlyStuck =
                CheckCollision(physics.position, physics, context);
            glm::vec3 newPosition = physics.position + glm::vec3(0.0f, movement.y, 0.0f);
            if (!CheckCollision(newPosition, physics, context)) {
                physics.position.y = newPosition.y;
                if (movement.y != 0.0f) {
                    physics.isOnGround = false;
                }
            } else if (currentlyStuck && movement.y > 0.0f) {
                physics.position.y = newPosition.y;
                physics.isOnGround = false;
            } else {
                float lo = newPosition.y;
                float hi = physics.position.y;
                glm::vec3 testPos = physics.position;
                for (int i = 0; i < 10; ++i) {
                    const float mid = (lo + hi) * 0.5f;
                    testPos.y = mid;
                    if (CheckCollision(testPos, physics, context)) {
                        lo = mid;
                    } else {
                        hi = mid;
                    }
                }
                physics.position.y = hi;
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

            // Auto-step (MC's Entity.collide() step-up branch, Entity.java:1089-1118).
            // When a horizontal move is blocked AND the player is on the ground,
            // try moving up by `maxUpStep` and re-attempting the move. If that
            // path is clear, snap the player Y back down to the highest
            // non-colliding height (so they end up STANDING on the obstacle
            // rather than levitating). maxUpStep = 0.6 in vanilla (Entity.java
            // line 3932), which is just enough to clear a 0.5-block slab but
            // not a full block.
            constexpr float kMaxUpStep = 0.6f;
            auto tryStepUp = [&](float dx, float dz) -> bool {
                if (!physics.isOnGround) return false;
                // 1. Vertical clearance above current position.
                glm::vec3 upPos = physics.position + glm::vec3(0.0f, kMaxUpStep, 0.0f);
                if (CheckCollision(upPos, physics, context)) return false;
                // 2. Horizontal move at elevated height.
                glm::vec3 stepPos = upPos + glm::vec3(dx, 0.0f, dz);
                if (CheckCollision(stepPos, physics, context)) return false;
                // 3. Snap Y back down to the top surface of whatever we stepped
                //    onto (binary search between elevated Y and original Y).
                float lo = physics.position.y;   // would collide if dropped this far
                float hi = stepPos.y;            // confirmed clear
                glm::vec3 testPos = stepPos;
                for (int i = 0; i < 10; ++i) {
                    const float mid = (lo + hi) * 0.5f;
                    testPos.y = mid;
                    if (CheckCollision(testPos, physics, context)) {
                        lo = mid;
                    } else {
                        hi = mid;
                    }
                }
                const float oldY = physics.position.y;
                physics.position = glm::vec3(stepPos.x, hi, stepPos.z);
                // Visual smoothing: shove the eye offset DOWN by the step
                // delta we just absorbed, then let it decay back to 0 over
                // the next ~tick. MC's Camera.setup interpolates between yo
                // (last tick's Y) and getY() (this tick's Y) across the
                // partial-tick interval (Camera.java:85), which produces a
                // visibly smooth rise instead of an instant snap. We track
                // the same delta here. Combines additively with any prior
                // unresolved offset so back-to-back steps stack instead of
                // cancelling out.
                const float dy = hi - oldY;
                if (dy > 0.0f) {
                    physics.stepVisualOffset -= dy;
                    // Don't let the camera drop below 0.6 m below the foot
                    // (≈ one max step) — if you stack steps faster than the
                    // offset can decay, just clamp rather than ending up
                    // looking at the floor.
                    if (physics.stepVisualOffset < -kMaxUpStep) {
                        physics.stepVisualOffset = -kMaxUpStep;
                    }
                }
                // Step counts as still on ground; preserve velocity (otherwise
                // walking up a long slab strip stutters every tick).
                physics.isOnGround = true;
                return true;
            };

            // Horizontal collision
            newPosition = physics.position + glm::vec3(movement.x, 0.0f, 0.0f);
            if (!CheckCollision(newPosition, physics, context)) {
                physics.position.x = newPosition.x;
            } else if (!tryStepUp(movement.x, 0.0f)) {
                // Wall collision — kill residual horizontal velocity in
                // the blocked axis so the player doesn't keep "pushing"
                // into the wall after hitting one.
                physics.velocity.x = 0.0f;
            }

            newPosition = physics.position + glm::vec3(0.0f, 0.0f, movement.z);
            if (!CheckCollision(newPosition, physics, context)) {
                physics.position.z = newPosition.z;
            } else if (!tryStepUp(0.0f, movement.z)) {
                physics.velocity.z = 0.0f;
            }

            // Decay residual horizontal velocity ONLY when on ground.
            // In-air motion keeps full momentum (Portal-style flight
            // through the air after a wall-portal exit). On ground,
            // friction ≈ 0.83 per 20-TPS tick (doubled from MC's 0.91:
            // 0.91² ≈ 0.83). Scaled to per-frame via the dt exponent.
            if (physics.isOnGround) {
                const float frictionFactor =
                    std::pow(0.83f, deltaTime * 20.0f);
                physics.velocity.x *= frictionFactor;
                physics.velocity.z *= frictionFactor;
            }
        }
    }

    bool CollidesAt(const AABB& box, const PhysicsContext& context) {

        // Check blocks that the box could be colliding with
        int minX = static_cast<int>(std::floor(box.min.x));
        int maxX = static_cast<int>(std::floor(box.max.x));
        // One cell lower than the box needs, because a collision shape may
        // reach out of its own cell upward — a fence is 24 pixels tall. Same
        // reason MC's BlockCollisions walks a cursor one cell wider than the
        // query box; see the note in MoveEntity.
        int minY = static_cast<int>(std::floor(box.min.y)) - 1;
        int maxY = static_cast<int>(std::floor(box.max.y));
        int minZ = static_cast<int>(std::floor(box.min.z));
        int maxZ = static_cast<int>(std::floor(box.max.z));

        // Same open-sky early-out as CollectBlockColliders: an all-air box
        // cannot collide, and the section flags say so without a cell read.
        if (context.blockAccess &&
            context.blockAccess->IsRegionAllAir(glm::ivec3(minX, minY, minZ),
                                                glm::ivec3(maxX, maxY, maxZ),
                                                /*absentIsAir=*/true)) {
            return false;
        }

        // Safety net behind the displacement clamp: never walk an absurd
        // region cell by cell. Treat it as free space instead.
        if (static_cast<size_t>(maxX - minX + 1) *
            static_cast<size_t>(maxY - minY + 1) *
            static_cast<size_t>(maxZ - minZ + 1) > 300000u) {
            return false;
        }

        // Bulk read: every state in the box in one call (one chunk and one
        // section lookup per tile) instead of one per cell.
        thread_local std::vector<BlockState> t_states;
        const int ny = maxY - minY + 1, nz = maxZ - minZ + 1;
        t_states.resize(static_cast<size_t>(maxX - minX + 1) * ny * nz);
        if (context.blockAccess) {
            context.blockAccess->GetBlockStatesInBox(glm::ivec3(minX, minY, minZ),
                                                     glm::ivec3(maxX, maxY, maxZ),
                                                     t_states.data());
        } else {
            for (int x = minX; x <= maxX; ++x)
                for (int y = minY; y <= maxY; ++y)
                    for (int z = minZ; z <= maxZ; ++z)
                        t_states[(static_cast<size_t>(x - minX) * ny + (y - minY)) * nz + (z - minZ)] =
                            context.GetBlockState(x, y, z);
        }
        const auto stateAt = [&](int x, int y, int z) {
            return t_states[(static_cast<size_t>(x - minX) * ny + (y - minY)) * nz + (z - minZ)];
        };

        for (int x = minX; x <= maxX; x++) {
            for (int y = minY; y <= maxY; y++) {
                for (int z = minZ; z <= maxZ; z++) {
                    // Per-block collision, and ONLY per-block collision.
                    // MC has no opacity/render-layer input to collision at
                    // all — BlockBehaviour.getCollisionShape is the single
                    // source of truth — so there is deliberately no
                    // IsBlockSolid pre-filter here. There used to be one,
                    // and it was the reason a networked client walked
                    // through leaves while the host didn't: the two sides
                    // run different IBlockAccess implementations and
                    // disagreed about "solid" (the client answered with the
                    // render-layer `opaque` flag, false for every Cutout /
                    // Translucent block). Reading the registry instead keeps
                    // host and joiner byte-identical.
                    //
                    // `.noCollision()` blocks (flowers, grasses, leaf litter,
                    // torches, vines, …) plus air and the fluids report
                    // hasCollision=false and get walked straight through.
                    // ONE state read, not a GetBlock followed by a
                    // GetBlockState for the same cell. Both walk the identical
                    // chain (World -> ChunkProvider -> memo -> Chunk -> Section
                    // -> PalettedContainer -> BitStorage), so asking twice
                    // doubled the cost of every collidable cell — and this loop
                    // runs over the swept AABB of every moving entity, every
                    // tick. BlockState::Block() is a table index off the state
                    // we already have.
                    const BlockState bstate = stateAt(x, y, z);
                    if (!BlockRegistry::HasCollision(bstate.Block())) continue;

                    // Build the block's actual collision AABB from its model
                    // shape. Full cubes (shape=0..1) produce the same 1×1×1
                    // box the old code used — no regression. Partial blocks
                    // (slabs, fences, trapdoors, leaf litter, …) get their
                    // real shape so the player can stand on a slab without
                    // floating at full-cube height, walk past a fence post
                    // through the gaps, etc.
                    // …and for a stair, all two or three boxes of its union,
                    // so the open half of the cell really is open.
                    // Single-box fast path: a reference into the shape cache
                    // instead of a ~148-byte BlockShapeSet built and copied per
                    // candidate cell. See GetSingleCollisionBox.
                    const BlockRegistry::BlockShape* one =
                        BlockRegistry::GetSingleCollisionBox(bstate);
                    BlockRegistry::BlockShapeSet multi;
                    if (!one) multi = BlockRegistry::GetBlockCollisionShapeSet(bstate);
                    const BlockRegistry::BlockShape* boxes = one ? one : multi.begin();
                    const size_t boxCount = one ? 1u : multi.count;
                    for (size_t bi = 0; bi < boxCount; ++bi) {
                        const BlockRegistry::BlockShape& shape = boxes[bi];
                        AABB blockAABB;
                        blockAABB.min = glm::vec3(x, y, z) + shape.min;
                        blockAABB.max = glm::vec3(x, y, z) + shape.max;
                        if (!box.Intersects(blockAABB)) continue;

                        // Portal-passthrough exception. The block is
                        // solid AND the moving AABB overlaps it, but
                        // the portal hook may say "this entity at
                        // this position fits inside the portal opening
                        // — let them through." If the AABB
                        // exceeds the opening laterally (e.g. they're
                        // approaching from the side), the hook returns
                        // false and the wall stays solid.
                        if (g_portalPassthrough &&
                            g_portalPassthrough(x, y, z, box)) {
                            continue;
                        }
                        return true; // Collision detected
                    }
                }
            }
        }

        return false; // No collision
    }

    bool CheckCollision(const glm::vec3& position, const PlayerPhysics& physics,
                       const PhysicsContext& context) {
        // Player-shaped wrapper over the generic test. Kept as its own
        // function (rather than inlined at every call site) so the player's
        // sneak-dependent height stays in one place.
        const float height = physics.GetCurrentHeight();
        return CollidesAt(
            AABB(glm::vec3(position.x, position.y + height * 0.5f, position.z),
                 glm::vec3(PlayerPhysics::WIDTH, height, PlayerPhysics::WIDTH)),
            context);
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
                int blockZ = static_cast<int>(std::floor(cornerPosition.z));

                // The cell the point is in, and the one below it. A fence's
                // collision shape is 24 pixels tall and so holds the player up
                // from a cell they are not standing in — without the second
                // probe, standing on a fence reads as standing on air.
                int blockY = static_cast<int>(std::floor(cornerPosition.y));
                bool inside = false;
                BlockID bid = BlockID::Air;
                for (int dy = 0; dy >= -1 && !inside; --dy) {
                    const int by = blockY + dy;
                    // Registry collision only — same rule as CheckCollision,
                    // and for the same host/joiner-parity reason. noCollision
                    // blocks don't provide support: you fall through a flower /
                    // leaf-litter pile the same way you walk through it, and
                    // air and the fluids hold nothing up.
                    bid = context.GetBlock(blockX, by, blockZ);
                    if (!BlockRegistry::HasCollision(bid)) continue;

                    // The check point is 0.1 below the foot — confirm it
                    // actually lies inside the block's collision shape (its top
                    // surface may be lower than the cube top for slabs / leaf
                    // litter / etc., and higher than it for a fence).
                    const auto shapes = BlockRegistry::GetBlockCollisionShapeSet(context.GetBlockState(blockX, by, blockZ));
                    const float lx = cornerPosition.x - blockX;
                    const float ly = cornerPosition.y - by;
                    const float lz = cornerPosition.z - blockZ;
                    for (const auto& shape : shapes) {
                        if (lx < shape.min.x || lx > shape.max.x) continue;
                        if (ly < shape.min.y || ly > shape.max.y) continue;
                        if (lz < shape.min.z || lz > shape.max.z) continue;
                        inside = true;
                        blockY = by;
                        break;
                    }
                }
                if (!inside) continue;

                // Portal-passthrough at this corner — degenerate AABB
                // collapsed to the corner point. If the corner falls
                // inside the portal opening (no surrounding wall
                // material at that position), the corner doesn't
                // count as support — needed so floor/ceiling portals
                // let the player fall through.
                if (g_portalPassthrough) {
                    AABB pointAABB;
                    pointAABB.min = cornerPosition;
                    pointAABB.max = cornerPosition;
                    if (g_portalPassthrough(blockX, blockY, blockZ, pointAABB)) {
                        continue;
                    }
                }
                return true;
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

    // ── Generic AABB mover (MC Entity.move(MoverType.SELF, …)) ──────────────
    //
    // Axis-separated sweep: move one axis at a time and back that axis out
    // entirely if the resulting box overlaps the world. Separating the axes is
    // what lets an entity slide along a wall instead of stopping dead, and it
    // is the same decomposition MC's `collide` makes.
    //
    // Y is stepped FIRST, before X and Z. MC does the same thing (its
    // `collideBoundingBox` resolves Y ahead of the horizontal axes), and the
    // order is load-bearing here: resolving Y last would let an item that is
    // falling into the corner of a block get pushed sideways out of the wall
    // before it ever registers as having landed, so `onGround` would flicker
    // and the -0.5 bounce would never fire.
    MoveResult MoveAABB(glm::dvec3& pos, glm::dvec3& velocity,
                        const glm::vec3& halfExtents,
                        const PhysicsContext& context) {
        MoveResult result;

        // Build the entity box for a candidate centre. `pos` is the entity's
        // FEET (MC convention), so the box is raised by half its height.
        const auto boxAt = [&](const glm::dvec3& p) {
            const glm::vec3 c(p.x, p.y + halfExtents.y, p.z);
            return AABB(c, halfExtents * 2.0f);
        };

        // Y first — see the ordering note above.
        if (velocity.y != 0.0) {
            glm::dvec3 next = pos;
            next.y += velocity.y;
            if (CollidesAt(boxAt(next), context)) {
                // Blocked. A downward block is a landing; either way this
                // axis' momentum is spent.
                if (velocity.y < 0.0) result.onGround = true;
                result.collidedY = true;
                velocity.y = 0.0;
            } else {
                pos.y = next.y;
            }
        }

        if (velocity.x != 0.0) {
            glm::dvec3 next = pos;
            next.x += velocity.x;
            if (CollidesAt(boxAt(next), context)) {
                result.collidedX = true;
                velocity.x = 0.0;
            } else {
                pos.x = next.x;
            }
        }

        if (velocity.z != 0.0) {
            glm::dvec3 next = pos;
            next.z += velocity.z;
            if (CollidesAt(boxAt(next), context)) {
                result.collidedZ = true;
                velocity.z = 0.0;
            } else {
                pos.z = next.z;
            }
        }

        // An entity that never moved down this step still needs to know it is
        // resting on something, or it would re-enter free-fall the moment the
        // sleep optimisation skips a tick. Probe a hair below the feet.
        if (!result.onGround) {
            glm::dvec3 probe = pos;
            probe.y -= 0.001;
            result.onGround = CollidesAt(boxAt(probe), context);
        }

        return result;
    }

    // ── MC-faithful entity mover ───────────────────────────────────────────

    void CollectBlockColliders(const AABBd& region, const PhysicsContext& context,
                               std::vector<AABBd>& out) {
        out.clear();

        const int minX = static_cast<int>(std::floor(region.min.x));
        const int maxX = static_cast<int>(std::floor(region.max.x));
        const int minY = static_cast<int>(std::floor(region.min.y));
        const int maxY = static_cast<int>(std::floor(region.max.y));
        const int minZ = static_cast<int>(std::floor(region.min.z));
        const int maxZ = static_cast<int>(std::floor(region.max.z));

        // Open-sky early-out: the swept region grows with the move, and an
        // entity thrown by a mass detonation sweeps tens of blocks a tick
        // through nothing. One section-flag test per (column, section) tile
        // answers "no colliders here" without visiting a cell. Exact — a
        // region that is all air has no collision shapes in it.
        if (context.blockAccess &&
            context.blockAccess->IsRegionAllAir(glm::ivec3(minX, minY, minZ),
                                                glm::ivec3(maxX, maxY, maxZ),
                                                /*absentIsAir=*/true)) {
            return;
        }

        // Safety net behind the displacement clamp: never walk an absurd
        // region cell by cell. Treat it as free space instead.
        if (static_cast<size_t>(maxX - minX + 1) *
            static_cast<size_t>(maxY - minY + 1) *
            static_cast<size_t>(maxZ - minZ + 1) > 300000u) {
            return;
        }

        // Bulk read, as in CollidesAt.
        thread_local std::vector<BlockState> t_states;
        const int ny = maxY - minY + 1, nz = maxZ - minZ + 1;
        t_states.resize(static_cast<size_t>(maxX - minX + 1) * ny * nz);
        if (context.blockAccess) {
            context.blockAccess->GetBlockStatesInBox(glm::ivec3(minX, minY, minZ),
                                                     glm::ivec3(maxX, maxY, maxZ),
                                                     t_states.data());
        } else {
            for (int x = minX; x <= maxX; ++x)
                for (int y = minY; y <= maxY; ++y)
                    for (int z = minZ; z <= maxZ; ++z)
                        t_states[(static_cast<size_t>(x - minX) * ny + (y - minY)) * nz + (z - minZ)] =
                            context.GetBlockState(x, y, z);
        }
        const auto stateAt = [&](int x, int y, int z) {
            return t_states[(static_cast<size_t>(x - minX) * ny + (y - minY)) * nz + (z - minZ)];
        };

        for (int x = minX; x <= maxX; ++x) {
            for (int y = minY; y <= maxY; ++y) {
                for (int z = minZ; z <= maxZ; ++z) {
                    // Same source of truth as CollidesAt — the registry, never
                    // a render-layer solidity flag. See the long note there for
                    // why: the two sides run different IBlockAccess impls and
                    // disagree about "solid", which desyncs host and joiner.
                    const BlockState cellState = stateAt(x, y, z);
                    const BlockID bid = cellState.Block();
                    if (!BlockRegistry::HasCollision(bid)) continue;

                    // The box UNION, not its bounds. MC's getCollisionShape is
                    // a VoxelShape and its collision walks every box in it;
                    // a stair contributes two or three, which is what lets the
                    // step-up search below find a half-block rise instead of a
                    // full-cube wall.
                    // Same single-box fast path as CollidesAt above — this is
                    // the hotter of the two: 30.4% of the server thread.
                    const BlockRegistry::BlockShape* one =
                        BlockRegistry::GetSingleCollisionBox(cellState);
                    BlockRegistry::BlockShapeSet multi;
                    if (!one) multi = BlockRegistry::GetBlockCollisionShapeSet(cellState);
                    const BlockRegistry::BlockShape* shapesBegin = one ? one : multi.begin();
                    const size_t shapesCount = one ? 1u : multi.count;

                    // Portal passthrough is deliberately NOT consulted here.
                    // The hook's answer depends on the moving entity's own box
                    // (it only opens for something that fits the 1x2 opening
                    // laterally), and this collider set is reused across the
                    // step-up candidates — so there is no single box to ask
                    // about. Mobs therefore treat portal frames as solid, which
                    // is the intended behaviour: nothing but the player travels
                    // through a portal.
                    for (size_t bi = 0; bi < shapesCount; ++bi) {
                        const BlockRegistry::BlockShape& shape = shapesBegin[bi];
                        // Built in DOUBLES from the integer block coordinate,
                        // so a collider face is exact at any distance from the
                        // origin. As floats, `1000000 + 0.5` is not even
                        // representable.
                        AABBd blockAABB;
                        blockAABB.min = glm::dvec3(x, y, z) + glm::dvec3(shape.min);
                        blockAABB.max = glm::dvec3(x, y, z) + glm::dvec3(shape.max);
                        out.push_back(blockAABB);
                    }
                }
            }
        }
    }

    double CollideAxis(int axis, const AABBd& box, double desired,
                       const std::vector<AABBd>& colliders) {
        if (desired == 0.0) return 0.0;

        // The two axes that are NOT being resolved. A collider only blocks
        // motion along `axis` if the box already overlaps it on both of them.
        const int a = (axis + 1) % 3;
        const int b = (axis + 2) % 3;

        const auto lo = [](const AABBd& v, int i) {
            return i == 0 ? v.min.x : (i == 1 ? v.min.y : v.min.z);
        };
        const auto hi = [](const AABBd& v, int i) {
            return i == 0 ? v.max.x : (i == 1 ? v.max.y : v.max.z);
        };

        double result = desired;

        for (const AABBd& c : colliders) {
            // Strict inequality on both cross axes, matching AABB::Intersects —
            // an entity sliding exactly along a face must not be caught by it.
            if (hi(box, a) <= lo(c, a) || lo(box, a) >= hi(c, a)) continue;
            if (hi(box, b) <= lo(c, b) || lo(box, b) >= hi(c, b)) continue;

            if (result > 0.0) {
                // Moving positive: the collider's near (min) face stops us.
                const double gap = lo(c, axis) - hi(box, axis);
                if (gap >= 0.0 && gap < result) result = gap;
            } else {
                const double gap = hi(c, axis) - lo(box, axis);
                if (gap <= 0.0 && gap > result) result = gap;
            }
        }

        return result;
    }

    bool MoveApproximate(glm::dvec3& position, glm::dvec3& velocity, const glm::vec3& half,
                         const glm::dvec3& delta, bool& onGround,
                         bool& horizontalCollision, bool& verticalCollision,
                         const PhysicsContext& ctx) {
        // Feet-anchored, exactly as MoveEntity's own boxAt: the box rises by
        // the FULL height from the position, which is the entity's feet.
        const auto boxAt = [&](const glm::dvec3& p) {
            AABB b;
            b.min = glm::vec3(static_cast<float>(p.x) - half.x,
                              static_cast<float>(p.y),
                              static_cast<float>(p.z) - half.z);
            b.max = glm::vec3(static_cast<float>(p.x) + half.x,
                              static_cast<float>(p.y) + 2.0f * half.y,
                              static_cast<float>(p.z) + half.z);
            return b;
        };

        const glm::dvec3 from = position;
        const bool wasOnGround = onGround;
        position += delta;

        horizontalCollision = false;
        verticalCollision   = false;

        if (CollidesAt(boxAt(position), ctx)) {
            // Y first, because gravity means Y is what almost always hit. This
            // is the one behaviour that has to survive the approximation: a
            // primed TNT reads onGround to damp its motion, and an entity that
            // never lands keeps accelerating downward forever if no position
            // packet arrives for it — which is exactly what happens when the
            // tracker is saturated.
            position.y        = from.y;
            velocity.y        = 0.0;
            onGround          = true;
            verticalCollision = true;

            if (CollidesAt(boxAt(position), ctx)) {
                // Still stuck with Y undone, so the horizontal half is what is
                // blocked. Undo the whole move rather than resolving X and Z
                // separately — one more overlap test, and a cosmetic entity
                // pressed into a wall for a tick is not worth two.
                position            = from;
                velocity.x          = 0.0;
                velocity.z          = 0.0;
                horizontalCollision = true;
            }
            return true;
        }

        // Nothing hit. An entity that was already airborne and still moving
        // down is still airborne — no probe needed, and that is the common case
        // for a cloud of falling TNT. Anything else has to ask, because
        // "resting on a block" is not visible from a non-overlapping box.
        if (!wasOnGround && delta.y < 0.0) {
            onGround = false;
            return false;
        }
        glm::dvec3 probe = position;
        probe.y -= 0.001;
        onGround = CollidesAt(boxAt(probe), ctx);
        return false;
    }

    EntityMoveResult MoveEntity(glm::dvec3& pos, glm::dvec3& velocity,
                                const glm::vec3& halfExtents,
                                float maxUpStep, bool wasOnGround,
                                const PhysicsContext& context) {
        EntityMoveResult result;

        // MC's equality epsilon (Mth.equal). Anything closer than this counts
        // as "went the whole way" and does NOT raise a collision flag.
        constexpr double kEpsilon = 1.0e-5;

        // `pos` is FEET; the box is raised by half the height. Built in
        // DOUBLES — see the note on AABBd for why this one cannot be float.
        const auto boxAt = [&](const glm::dvec3& p) {
            AABBd b;
            b.min = glm::dvec3(p.x - halfExtents.x, p.y, p.z - halfExtents.z);
            b.max = glm::dvec3(p.x + halfExtents.x,
                               p.y + 2.0 * static_cast<double>(halfExtents.y),
                               p.z + halfExtents.z);
            return b;
        };

        const glm::dvec3 desired = velocity;

        // ── The resting fast path ──────────────────────────────────────────
        //
        // An entity standing still on a block re-runs the FULL swept resolve
        // every tick — a collider gather over the swept region expanded a block
        // up and a block down, ~16-27 cells, then three ordered axis passes —
        // only to conclude it did not move. The exact-zero test below almost
        // never rescues it, because nothing at rest actually has zero velocity:
        // gravity re-applies -0.04 every tick and the landing damp zeroes it
        // again, so the steady state of a settled entity is a permanent -0.04.
        //
        // At a hundred thousand settled primed TNT that was ~2.5 million block
        // reads a tick and the single largest cost on the server — MobTick's
        // own body, 80 ms of a 205 ms tick.
        //
        // Conditions, all necessary:
        //   * it was on the ground LAST tick, so there is something under it;
        //   * the horizontal delta is under the same epsilon this function
        //     already treats as "went the whole way", so skipping it cannot be
        //     distinguished from the full resolve committing it;
        //   * the vertical delta is a small DOWNWARD one — a rise, or a fall
        //     large enough to matter, takes the real path.
        //
        // Then one overlap probe answers the whole question. If the probe hits,
        // the entity is still touching what it was standing on and the full
        // resolve would have moved it nowhere. If it misses — the block was
        // mined out from under it — this falls through and the real path runs,
        // so nothing hovers.
        //
        // THE DIVERGENCE: a resting entity accumulates no sub-epsilon
        // horizontal drift it would otherwise have crept along at, which at
        // <1e-5 blocks a tick is under a thousandth of a block a second.
        constexpr double kRestingMaxDrop = 0.1;   // well inside one block
        if (wasOnGround &&
            std::abs(desired.x) < kEpsilon && std::abs(desired.z) < kEpsilon &&
            desired.y <= 0.0 && desired.y > -kRestingMaxDrop) {
            glm::dvec3 probe = pos;
            probe.y -= 0.001;
            const AABBd pb = boxAt(probe);
            if (CollidesAt(AABB{ glm::vec3(pb.min), glm::vec3(pb.max) }, context)) {
                // Still standing on it. Zero the components the full resolve
                // would have zeroed, and report the same flags it would have.
                velocity.x = 0.0;
                velocity.z = 0.0;
                velocity.y = 0.0;
                result.onGround          = true;
                result.verticalCollision = true;
                return result;
            }
            // Nothing underneath any more — fall through to the real resolve.
        }

        if (desired.x == 0.0 && desired.y == 0.0 && desired.z == 0.0) {
            // Still owe the caller an accurate onGround: an entity that did not
            // move this tick is usually one that is standing still ON something,
            // and free-fall must not restart the moment it stops.
            glm::dvec3 probe = pos;
            probe.y -= 0.001;
            // A boolean overlap query, so the float AABB is fine here.
            const AABBd p = boxAt(probe);
            result.onGround = CollidesAt(AABB{ glm::vec3(p.min), glm::vec3(p.max) }, context);
            return result;
        }

        // ── Displacement clamp ──────────────────────────────────────────
        // Nothing natural moves anywhere near this fast: terminal velocity
        // under 0.98 drag is a few blocks a tick. Speeds beyond it are the
        // artifact of hundreds of same-tick explosion impulses stacking, and
        // an unclamped multi-hundred-block sweep makes the collider region
        // below cover hundreds of millions of cells — one such entity tick
        // was measured at twenty-one SECONDS. The excess displacement simply
        // is not taken this tick; the velocity is untouched, so the entity
        // keeps flying and covers the rest over the following ticks.
        constexpr double kMaxStep = 16.0;
        const glm::dvec3 desiredRef(
            std::clamp(desired.x, -kMaxStep, kMaxStep),
            std::clamp(desired.y, -kMaxStep, kMaxStep),
            std::clamp(desired.z, -kMaxStep, kMaxStep));

        const AABBd startBox = boxAt(pos);

        // One collider gather for the whole move, over the swept region. The
        // extra 1.0 of headroom on +Y covers the step-up candidates.
        AABBd region = startBox;
        region.min += glm::dvec3(std::min(0.0, desiredRef.x), std::min(0.0, desiredRef.y), std::min(0.0, desiredRef.z));
        region.max += glm::dvec3(std::max(0.0, desiredRef.x), std::max(0.0, desiredRef.y), std::max(0.0, desiredRef.z));
        region.max.y += std::max(1.0, static_cast<double>(maxUpStep));
        region.min -= glm::dvec3(1.0e-3);
        region.max += glm::dvec3(1.0e-3);
        // A collision shape may reach OUT of its own cell: a fence's is 24
        // pixels tall (CrossCollisionBlock's `collisionHeight`), which is what
        // makes it unjumpable. A cell-bounded gather would miss the fence the
        // player is standing on top of and drop them through it.
        //
        // MC solves this by iterating one cell further in every direction
        // (BlockCollisions' Cursor3D is built on the box expanded by 1). Only
        // -Y is needed here, because up is the only axis anything overhangs;
        // widening all six would triple the cells scanned on every move.
        region.min.y -= 1.0;

        // Reused across calls instead of built and destroyed per entity per
        // tick. A grounded 0.98-cube produces ~8 colliders, so the vector grew
        // 1->2->4->8: four malloc/free pairs every entity every tick, ~2,000
        // pairs a tick at 512 primed TNT. CollectBlockColliders already opens
        // with out.clear() — it was written for a caller-owned buffer and never
        // got one.
        //
        // thread_local, NOT a file-scope static: MoveEntity runs on the server
        // tick thread, the client main thread, and the item/orb managers.
        //
        // Not re-entrant. Audited: nothing in the SetBlock/NotifyNeighborBlocks
        // chain calls back into MoveEntity, and the resting-contact probe uses
        // CollidesAt, which has its own inline cell loop. If the portal
        // passthrough hook deliberately excluded from CollectBlockColliders
        // (see the note ~line 1032) is ever revisited, this buffer would be
        // live across that callback and would need a guard.
        thread_local std::vector<AABBd> colliders;
        CollectBlockColliders(region, context, colliders);

        // MC Direction.axisStepOrder: resolve the LARGEST component first, so
        // a mostly-horizontal move commits to its dominant axis before the
        // smaller ones can be clipped by geometry it has not reached yet.
        const auto resolve = [&](const glm::dvec3& move, const AABBd& from) {
            glm::dvec3 out(0.0);
            int order[3] = {0, 1, 2};
            const double mag[3] = {std::abs(move.x), std::abs(move.y), std::abs(move.z)};
            // Three elements — a hand-rolled descending sort beats pulling in
            // <algorithm> machinery for a function this hot.
            if (mag[order[0]] < mag[order[1]]) std::swap(order[0], order[1]);
            if (mag[order[1]] < mag[order[2]]) std::swap(order[1], order[2]);
            if (mag[order[0]] < mag[order[1]]) std::swap(order[0], order[1]);

            for (int i = 0; i < 3; ++i) {
                const int axis = order[i];
                const double want = axis == 0 ? move.x : (axis == 1 ? move.y : move.z);
                if (want == 0.0) continue;
                // Apply what has been resolved so far before testing the next
                // axis — that partial displacement is exactly what lets an
                // entity round a corner instead of catching on it.
                AABBd moved = from;
                moved.min += out;
                moved.max += out;
                const double got = CollideAxis(axis, moved, want, colliders);
                if (axis == 0) out.x = got; else if (axis == 1) out.y = got; else out.z = got;
            }
            return out;
        };

        glm::dvec3 moved = resolve(desiredRef, startBox);

        const bool blockedX = std::abs(moved.x - desired.x) > kEpsilon;
        const bool blockedZ = std::abs(moved.z - desired.z) > kEpsilon;
        const bool onGroundAfter = (std::abs(moved.y - desired.y) > kEpsilon) && desired.y < 0.0;

        // ── Step-up (MC Entity.collide, Entity.java:1089-1118) ─────────────
        //
        // Only when the entity is (or just became) grounded AND ran into
        // something horizontally. MC tries each collider top face inside
        // (0, maxUpStep] rather than a single fixed height, because on stairs
        // and slabs the useful height is not maxUpStep — it is whatever the
        // obstruction's top actually is.
        if (maxUpStep > 0.0f && (onGroundAfter || wasOnGround) && (blockedX || blockedZ)) {
            const AABBd groundedBox = onGroundAfter
                ? [&] { AABBd b = startBox; b.min.y += moved.y; b.max.y += moved.y; return b; }()
                : startBox;

            // Candidate heights: every collider top face strictly above the
            // box's floor and within reach, ascending, deduplicated.
            std::vector<double> candidates;
            candidates.reserve(8);
            for (const AABBd& c : colliders) {
                const double h = c.max.y - groundedBox.min.y;
                if (h > 1.0e-7 && h <= static_cast<double>(maxUpStep) &&
                    std::abs(h - moved.y) > 1.0e-7) {
                    candidates.push_back(h);
                }
            }
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end(),
                                         [](double a, double b) { return std::abs(a - b) < 1.0e-7; }),
                             candidates.end());

            const double bestHorizSq = moved.x * moved.x + moved.z * moved.z;
            for (double h : candidates) {
                const glm::dvec3 stepped =
                    resolve(glm::dvec3(desired.x, h, desired.z), groundedBox);
                if (stepped.x * stepped.x + stepped.z * stepped.z > bestHorizSq) {
                    // MC subtracts the grounded box's own Y offset back out, so
                    // the result stays expressed relative to the ORIGINAL box.
                    moved = stepped - glm::dvec3(0.0, startBox.min.y - groundedBox.min.y, 0.0);
                    result.steppedUp = true;
                    break;
                }
            }
        }

        pos += moved;

        result.collidedX = std::abs(moved.x - desired.x) > kEpsilon;
        result.collidedZ = std::abs(moved.z - desired.z) > kEpsilon;
        result.collidedY = std::abs(moved.y - desired.y) > kEpsilon;
        result.horizontalCollision = result.collidedX || result.collidedZ;
        result.verticalCollision = result.collidedY;
        result.onGround = result.collidedY && desired.y < 0.0;

        // Zero the momentum of any axis that hit something. A stepped-up move
        // is exempt on Y: the entity did not land on anything, it climbed, and
        // wiping Y here would eat the jump the mob may be part-way through.
        if (result.collidedX) velocity.x = 0.0;
        if (result.collidedZ) velocity.z = 0.0;
        if (result.collidedY && !result.steppedUp) velocity.y = 0.0;

        // Resting contact: an entity that did not move down this tick still
        // needs to know it is supported, or it re-enters free fall next tick.
        if (!result.onGround) {
            glm::dvec3 probe = pos;
            probe.y -= 0.001;
            // Reuses the collider set already gathered for this move, so the
            // probe is answered in doubles like everything else above.
            const AABBd probeBox = boxAt(probe);
            for (const AABBd& c : colliders) {
                if (probeBox.Intersects(c)) { result.onGround = true; break; }
            }
        }

        return result;
    }

} // namespace Game