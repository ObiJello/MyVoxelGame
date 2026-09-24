// File: src/common/physics/Physics.cpp
#include "Physics.hpp"
#include "common/world/block/AercloudBlock.hpp"
#include "common/world/block/BlockBounce.hpp"
#include "common/world/block/BlockFriction.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world//block/BlockRegistry.hpp"
#include "common/core/Log.hpp"
#include "common/core/Config.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include "common/world/fluid/FluidState.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

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
    namespace { bool g_portalCollisionActive = false; }
    void SetPortalCollisionActive(bool active) { g_portalCollisionActive = active; }
    bool PortalCollisionActive() { return g_portalCollisionActive; }
    namespace { PortalExtraSolidFn g_portalExtraSolid = nullptr; }
    void SetPortalExtraSolidFn(PortalExtraSolidFn fn) {
        g_portalExtraSolid = fn;
    }

    // The context's own provider when it has one, the global hooks
    // otherwise — see PortalCollisionProvider.
    namespace {
        bool PortalPassthroughAt(const PhysicsContext& ctx, int x, int y, int z, const AABB& box) {
            if (ctx.portalCollision) return ctx.portalCollision->IsBlockBehindPortal(x, y, z, box);
            return g_portalPassthrough && g_portalPassthrough(x, y, z, box);
        }
        bool PortalFarSideSolidAt(const PhysicsContext& ctx, int x, int y, int z, const AABB& box) {
            if (ctx.portalCollision) return ctx.portalCollision->IsFarSideSolid(x, y, z, box);
            return g_portalExtraSolid && g_portalExtraSolid(x, y, z, box);
        }
        bool PortalFarSideEngaged(const PhysicsContext& ctx) {
            if (ctx.portalCollision) return ctx.portalCollision->HasFarSideSolidity();
            return g_portalExtraSolid && g_portalCollisionActive;
        }

        // Aether AercloudBlock.getCollisionShape for the cloud at (x, y, z),
        // as this context's mover sees it (AercloudBlock.hpp: the shape
        // depends on the mover's fall distance and on the block above). The
        // block above is read here rather than taken from a caller's bulk
        // read — the rule looks one cell past any box it is asked about, and
        // one extra read per cloud cell is nothing. False = no collision.
        bool AercloudColliderAt(const PhysicsContext& ctx, BlockID id, int x, int y, int z,
                                glm::vec3& lo, glm::vec3& hi) {
            Aercloud::Asker asker;
            asker.entity       = ctx.collisionEntity;
            asker.fallDistance = ctx.collisionFallDistance;
            asker.fallFlying   = ctx.collisionFallFlying;
            const BlockID above = ctx.GetBlockState(x, y + 1, z).Block();
            return Aercloud::CollisionBox(id, above, asker, lo, hi);
        }
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
        if (Aercloud::IsAercloud(bid)) {
            // A cloud with another above it is a full block (AercloudBlock).
            glm::vec3 lo, hi;
            return AercloudColliderAt(context, bid, x, y, z, lo, hi) && hi.y >= 0.9999f;
        }
        return BlockRegistry::GetBlockCollisionShapeSet(
                   context.GetBlockState(x, y, z)).IsFullCube();
    }

    double FluidHeightAbove(const AABB& box, bool lava, const PhysicsContext& context) {
        // MC updateFluidHeightAndDoFluidPushing deflates by 0.001 so a box
        // exactly flush with a fluid cell's face doesn't count as inside it.
        const double minX = box.min.x + 0.001, maxX = box.max.x - 0.001;
        const double minY = box.min.y + 0.001, maxY = box.max.y - 0.001;
        const double minZ = box.min.z + 0.001, maxZ = box.max.z - 0.001;
        if (!context.blockAccess) return 0.0;
        const IBlockAccess& blocks = *context.blockAccess;
        const FluidType want = lava ? FluidType::Lava : FluidType::Water;

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
                    const FluidState fs = GetFluidState(blocks, x, y, z);
                    if (!fs.Is(want)) continue;
                    const double surface = static_cast<double>(y) +
                                           FluidHeight(blocks, glm::ivec3(x, y, z), fs);
                    if (surface < minY) continue;
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

    namespace {
        // MC Entity.checkInsideBlocks, for the Aether's aerclouds only: the
        // cloud cells the body's box overlaps. MC's cell range is the box
        // shrunk by 1e-7 on every side, so a body resting flush on a face
        // is not "inside" the cell beyond it.
        void ProbeAerclouds(PlayerPhysics& physics, const PhysicsContext& context) {
            physics.inAercloud = false;
            physics.inBlueAercloud = false;
            const double half   = physics.GetWidth() * 0.5;
            const double height = physics.GetCurrentHeight();
            constexpr double kShrink = 1.0e-7;
            const int minX = static_cast<int>(std::floor(physics.position.x - half + kShrink));
            const int maxX = static_cast<int>(std::floor(physics.position.x + half - kShrink));
            const int minY = static_cast<int>(std::floor(physics.position.y + kShrink));
            const int maxY = static_cast<int>(std::floor(physics.position.y + height - kShrink));
            const int minZ = static_cast<int>(std::floor(physics.position.z - half + kShrink));
            const int maxZ = static_cast<int>(std::floor(physics.position.z + half - kShrink));
            for (int x = minX; x <= maxX; ++x)
                for (int y = minY; y <= maxY; ++y)
                    for (int z = minZ; z <= maxZ; ++z) {
                        const BlockID id = context.GetBlockState(x, y, z).Block();
                        if (!Aercloud::IsAercloud(id)) continue;
                        physics.inAercloud = true;
                        if (id == BlockID::BlueAercloud) physics.inBlueAercloud = true;
                    }
        }

        // BlueAercloudBlock.entityInside launches anything that is not
        // sneaking; a sneaking player gets the plain AercloudBlock behaviour.
        // (The vehicle clause has nothing to apply to — the local player is
        // never a vehicle.)
        bool BlueAercloudLaunches(const PlayerPhysics& physics) {
            return physics.inBlueAercloud && !physics.isSneaking;
        }
    }

    // **NEW**: Main physics update function with PhysicsContext
    void UpdatePlayerPhysics(PlayerPhysics& physics,
                            const glm::vec3& movementInput,
                            bool jumpPressed,
                            bool sneakPressed,
                            float deltaTime,
                            const PhysicsContext& baseContext) {

        // The caller's context plus MC's EntityCollisionContext for the
        // player: the aerclouds' collision shape reads the fall distance
        // (AercloudBlock.hpp). The engine has no elytra, so never gliding.
        PhysicsContext context = baseContext;
        context.collisionEntity       = true;
        context.collisionFallDistance = physics.fallDistance;
        context.collisionFallFlying   = false;

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

        // Fluid state (AABB scan: water/lava heights, eye submersion, current)
        const bool wasInFluid = physics.isInWater || physics.isInLava;
        UpdateWaterState(physics, context);
        const bool inFluid = physics.isInWater || physics.isInLava;

        // Fluid↔land transitions: preserve momentum
        if (inFluid && !wasInFluid) {
            physics.waterVelocity = physics.velocity;
        } else if (!inFluid && wasInFluid) {
            physics.velocity.y = physics.waterVelocity.y;
            physics.waterVelocity = glm::vec3(0.0f);
        }

        physics.onClimbable = !physics.noclip && !physics.isFlying && !inFluid &&
                              (OnClimbable(physics, context) ||
                               // MC Spider.onClimbable: the wall it walked
                               // into last step (the flag is the previous
                               // move's, which is what MC reads too).
                               (physics.morphClimbsWalls && physics.horizontalCollision));

        if (!inFluid && !physics.isFlying) {
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

            // MC LivingEntity.handleOnClimbable: on a ladder the fall is capped
            // at 0.15 blocks a tick (3 blocks/s), and a sneaking player holds
            // still instead of sliding.
            if (physics.onClimbable) {
                const float slide = -PlayerPhysics::CLIMB_SLIDE_SPEED * physics.scale;
                if (physics.velocity.y < slide) physics.velocity.y = slide;
                if (physics.isSneaking && physics.velocity.y < 0.0f) physics.velocity.y = 0.0f;
            }

            // The Aether's aerclouds — AercloudBlock / BlueAercloudBlock
            // .entityInside, which MC runs at the end of the previous tick's
            // move; here it is the cells the body is in as this step begins
            // (the previous step's end position), applied before the move so
            // the step's displacement is already the cloud's:
            //   • blue, not sneaking: deltaMovement.y = 2.0 — the launch,
            //     carried over as the speed that reaches MC's apex
            //     (AercloudBlock.hpp), and off the ground;
            //   • any other cloud: a downward motion ×0.005 each tick, which
            //     with the tick's gravity settles at MC's sink rate — so the
            //     fall is cut to that rate, never sped up.
            // Gravity scales with the body (ApplyGravity), so both do too.
            if (!physics.noclip) {
                ProbeAerclouds(physics, context);
                if (BlueAercloudLaunches(physics)) {
                    physics.velocity.y = Aercloud::kBlueLaunchSpeedPerSecond * physics.scale;
                    physics.isOnGround = false;
                } else if (physics.inAercloud) {
                    const float sink = Aercloud::kSinkSpeedPerSecond * physics.scale;
                    if (physics.velocity.y < sink) physics.velocity.y = sink;
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

        // The Aether's aerclouds, the rest of entityInside for the cells the
        // body ended this step in: AercloudBlock sets onGround for a living
        // thing unless it is a flying player (so a player sinking through a
        // cloud can jump out of it), BlueAercloudBlock clears it for a
        // launch. The fall-distance reset both do is in the fall tracking
        // below — after this step's descent is counted, as MC's
        // checkInsideBlocks follows checkFallDamage.
        if (!physics.noclip) {
            ProbeAerclouds(physics, context);
            if (physics.inAercloud) {
                physics.isOnGround = BlueAercloudLaunches(physics) ? false : !physics.isFlying;
            }
        } else {
            physics.inAercloud = false;
            physics.inBlueAercloud = false;
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
            if (physics.isInWater || physics.isFlying || physics.noclip || physics.onClimbable ||
                physics.inAercloud || physics.effectSlowFalling || physics.effectLevitation >= 0) {
                // MC resetFallDistance (handleOnClimbable too, and both
                // aercloud entityInside hooks — the cloud breaks the fall).
                physics.fallDistance = 0.0f;
            } else if (physics.isInLava) {
                // MC Entity.baseTick: lava only HALVES the fall each tick,
                // so a drop into a lava pool still lands with damage.
                physics.fallDistance *= std::pow(0.5f, deltaTime * 20.0f);
            } else if (fallDy < 0.0f) {
                physics.fallDistance += -fallDy;
            }
            if (physics.isOnGround) {
                if (physics.fallDistance > 0.0f) {
                    // MC Block.fallOn: fallDistance * (1 - fallDistanceReduction)
                    // is what causeFallDamage sees — the bed's half fall.
                    physics.landedFallDistance = std::max(
                        physics.landedFallDistance,
                        physics.fallDistance * (1.0f - physics.landingFallReduction));
                }
                physics.fallDistance = 0.0f;
            }
            physics.landingFallReduction = 0.0f;
            // A bounce is a landing that immediately leaves the ground again
            // (MC: the next tick's move has movement.y > 0). Cleared here,
            // after the flush above has seen the contact, so the rebound is
            // reported as the landing it was and the next step's ground jump
            // and ground friction do not treat the airborne player as standing.
            if (physics.bouncedThisStep) {
                physics.bouncedThisStep = false;
                physics.isOnGround = false;
            }
        }
    }

    // MC LivingEntity.onClimbable: the block at the feet is in
    // #minecraft:climbable (ladder, vines, scaffolding, the nether and cave
    // vines), or an open trapdoor over a ladder facing the same way
    // (trapdoorUsableAsLadder).
    bool OnClimbable(const PlayerPhysics& physics, const PhysicsContext& context) {
        const int x = static_cast<int>(std::floor(physics.position.x));
        const int y = static_cast<int>(std::floor(physics.position.y));
        const int z = static_cast<int>(std::floor(physics.position.z));
        const BlockState state = context.GetBlockState(x, y, z);
        const BlockID id = state.Block();
        if (id == BlockID::Air) return false;
        static constexpr std::string_view kClimbable[] = {
            "ladder", "vine", "scaffolding", "weeping_vines", "weeping_vines_plant",
            "twisting_vines", "twisting_vines_plant", "cave_vines", "cave_vines_plant",
        };
        const std::string& slug = BlockRegistry::Get(id).registrySlug;
        for (std::string_view c : kClimbable) if (slug == c) return true;
        if (slug.size() > 9 && slug.compare(slug.size() - 9, 9, "_trapdoor") == 0 &&
            state.GetValueByName("open") == "true") {
            const BlockState below = context.GetBlockState(x, y - 1, z);
            return below.Block() == BlockID::Ladder &&
                   below.GetValueByName("facing") == state.GetValueByName("facing");
        }
        return false;
    }

    namespace {
        // MC's per-tick jump (y += v; v = (v − 0.08) · 0.98) reaches an apex
        // that this continuous model matches with v = √(2·g·h): 0.42 gives
        // the 9.04 b/s JUMP_VELOCITY above, and JUMP_BOOST's +0.1/level
        // gives MC's exact extra height (1.84 blocks at I, 2.52 at II).
        float JumpVelocityForPower(float power) {
            if (power <= 1.0e-5f) return 0.0f;   // MC jumpFromGround's gate
            double v = power, y = 0.0, apex = 0.0;
            for (int i = 0; i < 4096 && v > 0.0; ++i) {
                y += v;
                v = (v - 0.08) * 0.98;
                apex = std::max(apex, y);
            }
            return static_cast<float>(std::sqrt(2.0 * -static_cast<double>(PlayerPhysics::GRAVITY) * apex));
        }
    }

    void ApplyGravity(PlayerPhysics& physics, float deltaTime, const PhysicsContext& context) {
        // Water gravity is handled in HandleMovement's water branch.
        // Gravity scales with the body, like speed and the jump: a world
        // seen through a scaled portal is "just bigger", so every length in
        // the motion scales and every time stays the same — a jump lasts as
        // long as it always did and rises the same number of body heights.
        // With gravity left at vanilla a small player was slammed back down
        // in a fraction of the time.
        if (physics.effectLevitation >= 0) {
            // MC travelInAir under LEVITATION: movementY += (0.05·(amp+1) −
            // movementY)·0.2 instead of gravity, then the 0.98 drag — per
            // tick v' = 0.784·v + 0.196·L, evaluated at the fractional tick
            // (exact at every tick boundary).
            const float target = 0.05f * static_cast<float>(physics.effectLevitation + 1) *
                                 20.0f * physics.scale;   // blocks/s
            const float steady = 0.196f * target / 0.216f;
            const float k = std::pow(0.784f, deltaTime * 20.0f);
            physics.velocity.y = steady + (physics.velocity.y - steady) * k;
            return;
        }

        // MC getEffectiveGravity: SLOW_FALLING caps gravity at 0.01 (an
        // eighth of 0.08) while falling; the 0.98 drag then settles at
        // 0.49 blocks a tick.
        const bool slowFalling = physics.effectSlowFalling && physics.velocity.y <= 0.0f;
        const float gravityScale = slowFalling ? 0.125f : 1.0f;
        physics.velocity.y += PlayerPhysics::GRAVITY * physics.scale * gravityScale * deltaTime;

        const float terminal = (slowFalling ? -9.8f : PlayerPhysics::TERMINAL_VELOCITY) * physics.scale;
        if (physics.velocity.y < terminal) {
            physics.velocity.y = terminal;
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
        // A scaled player jumps their own height in the usual time: with
        // gravity scaled too (ApplyGravity), the velocity scales linearly.
        // JUMP_BOOST adds its +0.1/level to MC's 0.42 jump power.
        const float jumpVelocity = (physics.effectJumpBoost > 0.0f
            ? JumpVelocityForPower(0.42f + physics.effectJumpBoost)
            : PlayerPhysics::JUMP_VELOCITY) * physics.scale;
        if (stuckInBlock) {
            physics.velocity.y = jumpVelocity;
            physics.lastJumpTime = physics.totalTime;
            return;
        }

        // Normal ground jump
        if (jumpPressed && physics.isOnGround) {
            physics.velocity.y = jumpVelocity;
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
        // A morph walks at the mob's speed (PlayerPhysics::SetMorph); sprint
        // keeps the player's own ×1.3 on top of it.
        const float walk = physics.morphed ? PlayerPhysics::WALK_SPEED * physics.morphWalkFactor
                                           : PlayerPhysics::WALK_SPEED;
        // SPEED / SLOWNESS: MOVEMENT_SPEED's ADD_MULTIPLIED_TOTAL factors
        // multiply in with the sprint's ×1.3, exactly as the attribute folds.
        physics.baseSpeed = (physics.isSprinting
                                 ? walk * (PlayerPhysics::SPRINT_SPEED / PlayerPhysics::WALK_SPEED)
                                 : walk) * physics.scale * physics.effectSpeedFactor;

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

        if ((physics.isInWater || physics.isInLava) && !physics.isFlying) {
            // ============================================================
            // Fluid movement — MC LivingEntity.travelInFluid, exactly.
            //
            // MC advances a fluid at 20 Hz with, per tick and per axis,
            //     v' = d · (v + a) − g
            // (moveRelative adds `a`, move() displaces by v + a, the drag
            // `d` multiplies, the gravity term `g` subtracts). This runs
            // per frame, so each axis is advanced by the map's exact
            // solution at a fractional tick count f = dt · 20:
            //     v* = (d·a − g) / (1 − d),   v(f) = v* + (v − v*) · d^f
            // which lands on MC's value at every tick boundary and is the
            // same curve in between, and displaces by the pre-drag velocity
            // of the final tick, (v(f) + g) / d, times f. Velocities inside
            // this block are MC's blocks per TICK; waterVelocity stays b/s
            // for everything outside it.
            //
            // travelInFluid picks travelInWater whenever isInWater() holds
            // (water wins over lava), else travelInLava.
            // ============================================================
            const float f = deltaTime * 20.0f;
            const bool  lavaTravel  = !physics.isInWater;
            const bool  lavaShallow = lavaTravel && physics.lavaDepth <= 0.4f;
            const float depth       = lavaTravel ? physics.lavaDepth : physics.waterDepth;
            // MC LocalPlayer.aiStep: a sprint is cancelled in water unless
            // the player is under it — sprint-swimming is the only sprint
            // there is in water.
            const bool  sprint      = physics.isSprinting && (lavaTravel || physics.isEyeInWater);
            const bool  swimming    = !lavaTravel && sprint && physics.isEyeInWater;   // updateSwimming

            glm::vec3 vt = physics.waterVelocity / 20.0f;   // blocks per tick

            // One tick-map step at fractional f; returns the new velocity and
            // writes the displacement for the step.
            auto tickAxis = [&](float v, float a, float d, float g, float& outDisplacement) {
                const float fixed = (d * a - g) / (1.0f - d);
                const float df    = std::pow(d, f);
                const float vNew  = fixed + (v - fixed) * df;
                outDisplacement   = ((vNew + g) / d) * f;
                return vNew;
            };

            // ── Before travel (MC aiStep order) ────────────────────────
            // The current (Entity.updateFluidInteraction): an impulse per
            // tick, in blocks per tick.
            vt += physics.fluidCurrent * f;

            // Player.travel's swim steering: sprint-swimming follows the
            // look vector's pitch, faster when diving.
            if (swimming) {
                const float lookY = physics.lookDirY;
                const float d = lookY < -0.2f ? 0.085f : 0.06f;
                if (lookY <= 0.0f || jumpPressed || physics.fluidAboveHead) {
                    vt.y += (lookY - vt.y) * (1.0f - std::pow(1.0f - d, f));
                }
            }

            // aiStep's jump block: in water above the jump threshold (or off
            // the floor) the key SWIMS (jumpInLiquid, +0.04 a tick); on the
            // floor in shallow fluid it is a real jump, ten ticks apart while
            // held. Shift in water is goDownInWater (−0.04 a tick).
            if (physics.noJumpDelayTicks > 0.0f) physics.noJumpDelayTicks -= f;
            float verticalInput = 0.0f;
            const bool groundJump = jumpPressed && physics.isOnGround && depth <= 0.4f &&
                                    !(lavaTravel && !lavaShallow);
            if (jumpPressed) {
                if (groundJump) {
                    if (physics.noJumpDelayTicks <= 0.0f) {
                        // jumpFromGround → getJumpPower: JUMP_BOOST's +0.1 a
                        // level applies to a wading jump exactly as on land.
                        const float jumpVelocity = physics.effectJumpBoost > 0.0f
                            ? JumpVelocityForPower(0.42f + physics.effectJumpBoost)
                            : PlayerPhysics::JUMP_VELOCITY;
                        vt.y = (jumpVelocity * physics.scale) / 20.0f;
                        physics.isOnGround = false;
                        physics.lastJumpTime = physics.totalTime;
                        physics.didJumpThisStep = true;
                        physics.noJumpDelayTicks = 10.0f;
                    }
                } else {
                    verticalInput += 0.04f;
                }
            } else {
                physics.noJumpDelayTicks = 0.0f;
            }
            if (!lavaTravel && physics.isSneaking) verticalInput -= 0.04f;

            // ── travelInWater / travelInLava ───────────────────────────
            // moveRelative(0.02, input): the input vector, normalised when
            // longer than one, scaled by the fixed fluid speed; a crouching
            // player's input is 0.3 (isMovingSlowly).
            glm::vec3 inputDir(movementInput.x, 0.0f, movementInput.z);
            if (glm::dot(inputDir, inputDir) > 1.0f) inputDir = glm::normalize(inputDir);
            if (physics.isSneaking) inputDir *= 0.3f;
            const float speed = 0.02f;

            // MC travelInFluid's baseGravity is getEffectiveGravity(): with
            // SLOW_FALLING and the body not rising (isFalling, sampled after
            // the jump/swim impulses above) it is min(0.08, 0.01) — a slow
            // faller sinks through water and lava at an eighth of the rate.
            const bool  fluidFalling = vt.y + verticalInput <= 0.0f;
            const float baseGravity  = (physics.effectSlowFalling && fluidFalling) ? 0.01f : 0.08f;

            float hDrag, vDrag, gravityTerm;
            if (!lavaTravel) {
                hDrag = sprint ? 0.9f : 0.8f;                  // getWaterSlowDown
                if (physics.effectDolphinsGrace) hDrag = 0.96f;   // DOLPHINS_GRACE
                vDrag = 0.8f;
                // getFluidFallingAdjustedMovement: gravity/16 — 0.08/16 —
                // unless sprinting (a sprint-swimmer does not sink).
                gravityTerm = sprint ? 0.0f : baseGravity / 16.0f;
            } else if (lavaShallow) {
                hDrag = 0.5f; vDrag = 0.8f;
                gravityTerm = baseGravity / 16.0f + baseGravity / 4.0f;   // sixteenth, then the quarter
            } else {
                hDrag = 0.5f; vDrag = 0.5f;
                gravityTerm = baseGravity / 4.0f;             // scale(0.5) then −gravity/4
            }

            glm::vec3 disp(0.0f);
            vt.x = tickAxis(vt.x, inputDir.x * speed, hDrag, 0.0f, disp.x);
            vt.z = tickAxis(vt.z, inputDir.z * speed, hDrag, 0.0f, disp.z);
            vt.y = tickAxis(vt.y, verticalInput,       vDrag, gravityTerm, disp.y);
            // getFluidFallingAdjustedMovement's −0.003 snap is not carried:
            // its two conditions, |y − 0.005| >= 0.003 and |y − gravity/16|
            // < 0.003, are the same quantity at the player's gravity of 0.08
            // and can never both hold. Under SLOW_FALLING (gravity 0.01) they
            // can; the snap's −0.003 then sits within 0.0002 of the −0.003125
            // this map settles at anyway, so the continuous solution stands.

            // 4. Move with collision
            glm::vec3 movement = disp;

            // Vertical collision — snap to collision boundary (mirrors
            // MC's Entity.collide()/Shapes.collide which return the
            // exact maximum-allowed movement). Without snapping, the
            // player would stop at the pre-frame Y, leaving them
            // floating slightly above the floor; the next ~5 frames of
            // gravity would drift them down — visible as a "land,
            // stall, drift" stutter.
            glm::dvec3 newPosition = physics.position + glm::dvec3(0.0f, movement.y, 0.0f);
            if (!CheckCollision(newPosition, physics, context)) {
                physics.position.y = newPosition.y;
                if (movement.y != 0.0f) {
                    physics.isOnGround = false;
                }
            } else {
                double lo = newPosition.y;       // colliding endpoint
                double hi = physics.position.y;  // last frame's resting Y (assumed safe)
                // Double: a float copy put the test box on a 3 cm grid in x/z
                // far from the origin, so it touched block edges the real
                // body did not.
                glm::dvec3 testPos = physics.position;
                for (int i = 0; i < 10; ++i) {   // 10 iter ≈ 1024× precision
                    const double mid = (lo + hi) * 0.5;
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
                    vt.y = 0.0f;
                }
                if (movement.y > 0.0f) {
                    vt.y = 0.0f;
                }
            }

            // Ground check when not moving vertically
            if (movement.y == 0.0f) {
                glm::dvec3 testPos = physics.position + glm::dvec3(0.0f, -0.1f, 0.0f);
                physics.isOnGround = CheckCollision(testPos, physics, context);
            }

            if (!physics.wasOnGround && physics.isOnGround) {
                physics.lastLandingTime = physics.totalTime;
            }

            // Horizontal collision with jump-out-of-fluid
            bool hadHorizontalCollision = false;

            newPosition = physics.position + glm::dvec3(movement.x, 0.0f, 0.0f);
            if (!CheckCollision(newPosition, physics, context)) {
                physics.position.x = newPosition.x;
            } else {
                hadHorizontalCollision = true;
                vt.x = 0.0f;
            }

            newPosition = physics.position + glm::dvec3(0.0f, 0.0f, movement.z);
            if (!CheckCollision(newPosition, physics, context)) {
                physics.position.z = newPosition.z;
            } else {
                hadHorizontalCollision = true;
                vt.z = 0.0f;
            }

            physics.waterVelocity = vt * 20.0f;

            // 5. Jump-out-of-fluid (MC: jumpOutOfFluid, from both fluid travels)
            // Only trigger at the surface (partially submerged), not deep in the fluid.
            // MC checks if the player can move upward to exit the fluid.
            if (hadHorizontalCollision && depth < physics.GetCurrentHeight()) {
                glm::dvec3 abovePos = physics.position + glm::dvec3(0.0f, 0.6f, 0.0f);
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
                speed = PlayerPhysics::FLY_HORIZONTAL_SPEED * sprintMul * physics.scale;
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
                physics.velocity.y = vert * PlayerPhysics::FLY_VERTICAL_SPEED * sprintMul * physics.scale;
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
            // handleOnClimbable clamps the horizontal motion to 0.15 a tick.
            if (physics.onClimbable) {
                const float cap = PlayerPhysics::CLIMB_SLIDE_SPEED * physics.scale;
                const float len = glm::length(horizontalMovement);
                if (len > cap) horizontalMovement *= cap / len;
            }

            // Block friction — MC LivingEntity.travelInAir on the ground:
            //   moveRelative(speed · 0.21600002 / f³), move, then
            //   deltaMovement.xz *= f · 0.91,
            // f being the friction of the block that affects movement
            // (getBlockPosBelowThatAffectsMyMovement). This engine's walk is
            // instant — the steady state MC reaches on a 0.6 block — so the
            // tick map only runs where it gives something else: ice and blue
            // ice (0.98 / 0.989), slime (0.8) and the Aether's quicksoil
            // (1.1 — above 1, the slide that speeds up, until the
            // FrictionCapped cap drops it to 0.99 past one block a tick;
            // QuicksoilBlock.getFriction via GetBlockFriction's motion
            // overload). The walk input is carried over as the acceleration
            // that gives this engine's walk speed on a 0.6 block, scaled by
            // MC's 0.216 / f³, and the map is advanced by its exact solution
            // at a fractional tick count, as the fluid branch above does.
            // velocity.xz then IS the slide (MC's deltaMovement), so the
            // residual add and the ground decay below are skipped.
            bool slide = false;
            if (physics.isOnGround && !physics.isFlying) {
                const glm::ivec3 below = BlockPosBelowThatAffectsMovement(physics.position);
                const BlockID belowId = context.GetBlockState(below.x, below.y, below.z).Block();
                const glm::dvec3 motionPerTick(physics.velocity.x / 20.0, 0.0, physics.velocity.z / 20.0);
                const float friction = GetBlockFriction(belowId, motionPerTick);
                constexpr float kDefaultFriction = 0.6f;
                if (friction != kDefaultFriction) {
                    slide = true;
                    const float f        = deltaTime * 20.0f;
                    const float retain   = friction * 0.91f;
                    const float steady   = 1.0f - kDefaultFriction * 0.91f;   // 0.454
                    const float accelMul = steady * (0.21600002f / (friction * friction * friction));
                    auto slideAxis = [&](float velPerSecond, float inputPerSecond, float& displacement) {
                        const float u = velPerSecond / 20.0f;
                        const float a = (inputPerSecond / 20.0f) * accelMul;
                        float uNew;
                        if (std::abs(1.0f - retain) < 1.0e-6f) {
                            uNew = u + a * f;   // retain == 1: no decay, pure acceleration
                        } else {
                            const float fixed = retain * a / (1.0f - retain);
                            uNew = fixed + (u - fixed) * std::pow(retain, f);
                        }
                        displacement = (uNew / retain) * f;   // pre-friction motion of the last tick
                        return uNew * 20.0f;
                    };
                    float dispX = 0.0f, dispZ = 0.0f;
                    physics.velocity.x = slideAxis(physics.velocity.x, horizontalMovement.x, dispX);
                    physics.velocity.z = slideAxis(physics.velocity.z, horizontalMovement.z, dispZ);
                    horizontalMovement.x = deltaTime > 0.0f ? dispX / deltaTime : 0.0f;
                    horizontalMovement.z = deltaTime > 0.0f ? dispZ / deltaTime : 0.0f;
                }
            }
            if (!slide) {
                horizontalMovement.x += physics.velocity.x;
                horizontalMovement.z += physics.velocity.z;
            }
            physics.horizontalCollision = false;

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
            glm::dvec3 newPosition = physics.position + glm::dvec3(0.0f, movement.y, 0.0f);
            if (!CheckCollision(newPosition, physics, context)) {
                physics.position.y = newPosition.y;
                if (movement.y != 0.0f) {
                    physics.isOnGround = false;
                }
            } else if (currentlyStuck && movement.y > 0.0f) {
                physics.position.y = newPosition.y;
                physics.isOnGround = false;
            } else {
                double lo = newPosition.y;
                double hi = physics.position.y;
                glm::dvec3 testPos = physics.position;   // double, see above
                for (int i = 0; i < 10; ++i) {
                    const double mid = (lo + hi) * 0.5;
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
                    // The block landed on — MC Entity.getOnPos(0.2F), the
                    // `effectState` of Entity.move — decides two things
                    // (BlockBounce.hpp):
                    //   • Block.fallOn: its fallDistanceReduction scales the
                    //     distance the fall-tracking block flushes (a bed
                    //     halves it), sneaking or not.
                    //   • Entity.restituteMovementAfterCollisions: unless
                    //     sneaking (isSuppressingBounce) or the block forbids
                    //     it (honey), an impact faster than one tick of
                    //     gravity comes back up at bounceRestitution × the
                    //     impact speed — a bed's 0.75, a slime block's 1.0.
                    //     Slower than that and it is a plain landing, so a
                    //     standing player does not vibrate on the mattress.
                    // The player is a LivingEntity, so no ×0.8; gravity
                    // scales with the body here, so the threshold does too.
                    const BlockID landedOn = context.GetBlock(
                        static_cast<int>(std::floor(physics.position.x)),
                        static_cast<int>(std::floor(physics.position.y - 0.2)),
                        static_cast<int>(std::floor(physics.position.z)));
                    physics.landingFallReduction = FallDistanceReduction(landedOn);
                    const float impact = physics.velocity.y;   // blocks/s, negative
                    const float oneTickOfGravity = -PlayerPhysics::GRAVITY * physics.scale * 0.05f;
                    const float restitution =
                        (!physics.isSneaking && !SuppressesBounce(landedOn))
                            ? BounceRestitution(landedOn) : 0.0f;
                    if (restitution > 0.0f && -impact > oneTickOfGravity) {
                        physics.velocity.y = -impact * restitution;
                        physics.bouncedThisStep = true;
                    } else {
                        physics.velocity.y = 0.0f;
                    }
                }
                if (movement.y > 0.0f) {
                    physics.velocity.y = 0.0f;
                }
            }

            // Ground check when not moving vertically
            if (movement.y == 0.0f) {
                glm::dvec3 testPosition = physics.position + glm::dvec3(0.0f, -0.1f, 0.0f);
                physics.isOnGround = CheckCollision(testPosition, physics, context);
            }

            if (!physics.wasOnGround && physics.isOnGround) {
                physics.lastLandingTime = physics.totalTime;
            }

            // Sneaking ledge protection
            if (physics.isSneaking && physics.isOnGround) {
                glm::dvec3 testPosX = physics.position + glm::dvec3(movement.x, 0.0f, 0.0f);
                if (!HasSupportBelow(testPosX, physics, context)) {
                    movement.x = 0.0f;
                }
                glm::dvec3 testPosZ = physics.position + glm::dvec3(0.0f, 0.0f, movement.z);
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
                glm::dvec3 upPos = physics.position + glm::dvec3(0.0f, kMaxUpStep, 0.0f);
                if (CheckCollision(upPos, physics, context)) return false;
                // 2. Horizontal move at elevated height.
                glm::dvec3 stepPos = upPos + glm::dvec3(dx, 0.0, dz);
                if (CheckCollision(stepPos, physics, context)) return false;
                // 3. Snap Y back down to the top surface of whatever we stepped
                //    onto (binary search between elevated Y and original Y).
                double lo = physics.position.y;  // would collide if dropped this far
                double hi = stepPos.y;           // confirmed clear
                glm::dvec3 testPos = stepPos;
                for (int i = 0; i < 10; ++i) {
                    const double mid = (lo + hi) * 0.5;
                    testPos.y = mid;
                    if (CheckCollision(testPos, physics, context)) {
                        lo = mid;
                    } else {
                        hi = mid;
                    }
                }
                const double oldY = physics.position.y;
                physics.position = glm::dvec3(stepPos.x, hi, stepPos.z);
                // Visual smoothing: shove the eye offset DOWN by the step
                // delta we just absorbed, then let it decay back to 0 over
                // the next ~tick. MC's Camera.setup interpolates between yo
                // (last tick's Y) and getY() (this tick's Y) across the
                // partial-tick interval (Camera.java:85), which produces a
                // visibly smooth rise instead of an instant snap. We track
                // the same delta here. Combines additively with any prior
                // unresolved offset so back-to-back steps stack instead of
                // cancelling out.
                const float dy = static_cast<float>(hi - oldY);
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
            newPosition = physics.position + glm::dvec3(movement.x, 0.0f, 0.0f);
            if (!CheckCollision(newPosition, physics, context)) {
                physics.position.x = newPosition.x;
            } else if (!tryStepUp(movement.x, 0.0f)) {
                // Wall collision — kill residual horizontal velocity in
                // the blocked axis so the player doesn't keep "pushing"
                // into the wall after hitting one.
                physics.velocity.x = 0.0f;
                if (movement.x != 0.0f) physics.horizontalCollision = true;
            }

            newPosition = physics.position + glm::dvec3(0.0f, 0.0f, movement.z);
            if (!CheckCollision(newPosition, physics, context)) {
                physics.position.z = newPosition.z;
            } else if (!tryStepUp(0.0f, movement.z)) {
                physics.velocity.z = 0.0f;
                if (movement.z != 0.0f) physics.horizontalCollision = true;
            }

            // MC LivingEntity.travelInAir: pushing into the ladder (or holding
            // jump) while on it climbs at 0.2 blocks a tick (4 blocks/s).
            if (physics.onClimbable && (physics.horizontalCollision || jumpPressed)) {
                physics.velocity.y = PlayerPhysics::CLIMB_UP_SPEED * physics.scale;
                physics.isOnGround = false;
            }

            // Decay residual horizontal velocity ONLY when on ground.
            // In-air motion keeps full momentum (Portal-style flight
            // through the air after a wall-portal exit). On ground,
            // friction ≈ 0.83 per 20-TPS tick (doubled from MC's 0.91:
            // 0.91² ≈ 0.83). Scaled to per-frame via the dt exponent.
            if (physics.isOnGround && !slide) {
                const float frictionFactor =
                    std::pow(0.83f, deltaTime * 20.0f);
                physics.velocity.x *= frictionFactor;
                physics.velocity.z *= frictionFactor;
            }
        }
    }

    bool CollidesAt(const AABB& box, const PhysicsContext& context) {
        return CollidesAt(AABBd::FromMinMax(glm::dvec3(box.min), glm::dvec3(box.max)), context);
    }

    bool CollidesAt(const AABBd& box, const PhysicsContext& context) {
        PROFILE_ZONE_N("Physics.CollidesAt");

        // Check blocks that the box could be colliding with. The cell range
        // is taken from the DOUBLE box: a float box at x = 300,000 has a
        // 0.03-block grid, so its faces land in the wrong cell near a block
        // boundary and the player snags or sinks far from the origin.
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

        // The overlap tests run in a LOCAL frame anchored at the region's
        // min cell: box and block shapes are both expressed relative to
        // (minX, minY, minZ), so the floats involved are a few blocks in
        // size and exact to a micron wherever the region is. `worldF` is
        // the box in world floats for the portal hooks, which compare it
        // against their own world-space surfaces.
        const glm::dvec3 frameOrigin(minX, minY, minZ);
        const AABB local = AABB::FromMinMax(glm::vec3(box.min - frameOrigin),
                                            glm::vec3(box.max - frameOrigin));
        const AABB worldF = AABB::FromMinMax(glm::vec3(box.min), glm::vec3(box.max));

        // Same open-sky early-out as CollectBlockColliders: an all-air box
        // cannot collide, and the section flags say so without a cell read.
        // Not while a portal is engaged (see SetPortalCollisionActive): the
        // far side of a floor seam is solid exactly where this side is air
        // — the void below bedrock — and the early-out answered before the
        // extra-solid hook could say so, which is how a player fell out of
        // the Overworld into the Nether's roof instead of standing on it.
        if (context.blockAccess && !PortalFarSideEngaged(context) &&
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
                    if (!BlockRegistry::HasCollision(bstate.Block())) {
                        // Empty here — but maybe solid on the far side of a
                        // portal this box is entering (see PortalExtraSolidFn).
                        if (PortalFarSideSolidAt(context, x, y, z, worldF)) {
                            AABB cube;
                            cube.min = glm::vec3(x - minX, y - minY, z - minZ);
                            cube.max = cube.min + glm::vec3(1.0f);
                            if (local.Intersects(cube)) return true;
                        }
                        continue;
                    }

                    // The Aether's aerclouds: a shape that depends on the
                    // mover and the block above (AercloudBlock.hpp), never
                    // the model's cube.
                    if (Aercloud::IsAercloud(bstate.Block())) {
                        glm::vec3 lo, hi;
                        if (!AercloudColliderAt(context, bstate.Block(), x, y, z, lo, hi)) continue;
                        AABB cloudAABB;
                        cloudAABB.min = glm::vec3(x - minX, y - minY, z - minZ) + lo;
                        cloudAABB.max = glm::vec3(x - minX, y - minY, z - minZ) + hi;
                        if (!local.Intersects(cloudAABB)) continue;
                        if (PortalPassthroughAt(context, x, y, z, worldF)) continue;
                        return true;
                    }

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
                        blockAABB.min = glm::vec3(x - minX, y - minY, z - minZ) + shape.min;
                        blockAABB.max = glm::vec3(x - minX, y - minY, z - minZ) + shape.max;
                        if (!local.Intersects(blockAABB)) continue;

                        // Portal-passthrough exception. The block is
                        // solid AND the moving AABB overlaps it, but
                        // the portal hook may say "this entity at
                        // this position fits inside the portal opening
                        // — let them through." If the AABB
                        // exceeds the opening laterally (e.g. they're
                        // approaching from the side), the hook returns
                        // false and the wall stays solid.
                        if (PortalPassthroughAt(context, x, y, z, worldF)) {
                            continue;
                        }
                        return true; // Collision detected
                    }
                }
            }
        }

        return false; // No collision
    }

    bool CheckCollision(const glm::dvec3& position, const PlayerPhysics& physics,
                       const PhysicsContext& context) {
        // Player-shaped wrapper over the generic test. Kept as its own
        // function (rather than inlined at every call site) so the player's
        // sneak-dependent height stays in one place. Double in, double box:
        // the player's position is double so that a far-away player stands
        // on the ground exactly instead of on a 3 cm float grid.
        const double height = physics.GetCurrentHeight();
        const double half   = physics.GetWidth() * 0.5;
        return CollidesAt(
            AABBd::FromMinMax(position - glm::dvec3(half, 0.0, half),
                              position + glm::dvec3(half, height, half)),
            context);
    }

    bool HasSupportBelow(const glm::dvec3& position, const PlayerPhysics& physics,
                        const PhysicsContext& context) {

        float halfWidth = physics.GetWidth() / 2.0f;
        float offsets[] = { -halfWidth + PlayerPhysics::OVERHANG_MARGIN * physics.scale,
                           halfWidth - PlayerPhysics::OVERHANG_MARGIN * physics.scale };

        // The portal hooks below get the BODY's box at this position, not a
        // point collapsed onto the probed corner. They decide by the body:
        // whether it fits the opening, and whether it straddles the surface
        // — a body sinking into a floor portal has its centre below the
        // surface long before its eye crosses, and a point 0.1 under the
        // feet read as "approached from behind" and made the ground solid
        // again: the player stood in the surface, never entering.
        const float bodyHeight = physics.GetCurrentHeight();
        const AABB bodyBox(glm::vec3(position.x, position.y + bodyHeight * 0.5, position.z),
                           glm::vec3(physics.GetWidth(), bodyHeight, physics.GetWidth()));

        for (float xOffset : offsets) {
            for (float zOffset : offsets) {
                // Double: the corner's cell must be the one the double
                // position is in, not the one its float rounding lands in.
                glm::dvec3 cornerPosition(
                    position.x + xOffset,
                    position.y - 0.1, // Slightly below the player's feet
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
                // An Aether aercloud's floor is 0.000625 thick
                // (AercloudBlock.hpp): resting on it the feet are in the
                // cloud's OWN cell, and the probe point 0.1 lower has already
                // left it. The floor supports the corner when it spans the
                // probe's drop — what MC's backOffFromEdge finds by moving
                // the box down a step and hitting it.
                {
                    const int feetCell = static_cast<int>(std::floor(position.y));
                    const BlockID feetBlock = context.GetBlockState(blockX, feetCell, blockZ).Block();
                    glm::vec3 lo, hi;
                    if (feetCell != blockY && Aercloud::IsAercloud(feetBlock) &&
                        AercloudColliderAt(context, feetBlock, blockX, feetCell, blockZ, lo, hi)) {
                        const float lx = static_cast<float>(cornerPosition.x - blockX);
                        const float lz = static_cast<float>(cornerPosition.z - blockZ);
                        if (lx >= lo.x && lx <= hi.x && lz >= lo.z && lz <= hi.z &&
                            feetCell + static_cast<double>(lo.y) <= position.y &&
                            feetCell + static_cast<double>(hi.y) >= cornerPosition.y) {
                            inside = true;
                            blockY = feetCell;
                        }
                    }
                }
                for (int dy = 0; dy >= -1 && !inside; --dy) {
                    const int by = blockY + dy;
                    // Registry collision only — same rule as CheckCollision,
                    // and for the same host/joiner-parity reason. noCollision
                    // blocks don't provide support: you fall through a flower /
                    // leaf-litter pile the same way you walk through it, and
                    // air and the fluids hold nothing up.
                    bid = context.GetBlock(blockX, by, blockZ);
                    if (!BlockRegistry::HasCollision(bid)) {
                        // The far side of a portal underfoot can be the floor
                        // (see PortalExtraSolidFn) — a full cube, so the probe
                        // point is inside it whenever it is in the cell.
                        {
                            if (PortalFarSideSolidAt(context, blockX, by, blockZ, bodyBox)) {
                                inside = true;
                                blockY = by;
                                break;
                            }
                        }
                        continue;
                    }

                    // The check point is 0.1 below the foot — confirm it
                    // actually lies inside the block's collision shape (its top
                    // surface may be lower than the cube top for slabs / leaf
                    // litter / etc., and higher than it for a fence).
                    BlockRegistry::BlockShapeSet shapes;
                    if (Aercloud::IsAercloud(bid)) {
                        // The mover-dependent cloud shape (AercloudBlock.hpp).
                        glm::vec3 lo, hi;
                        if (AercloudColliderAt(context, bid, blockX, by, blockZ, lo, hi)) {
                            shapes.boxes[0].min = lo;
                            shapes.boxes[0].max = hi;
                            shapes.count = 1;
                        }
                    } else {
                        shapes = BlockRegistry::GetBlockCollisionShapeSet(context.GetBlockState(blockX, by, blockZ));
                    }
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

                // Portal-passthrough at this corner, judged by the body's
                // box (see bodyBox above). If the cell under the corner is
                // inside a portal opening the body is going through, the
                // corner doesn't count as support — needed so floor
                // portals let the player fall through.
                if (PortalPassthroughAt(context, blockX, blockY, blockZ, bodyBox)) {
                    continue;
                }
                return true;
            }
        }

        return false;
    }

    void UpdateWaterState(PlayerPhysics& physics, const PhysicsContext& context) {
        // MC EntityFluidInteraction.update for the player: every cell the
        // body box (deflated 0.001, like getFluidInteractionBox) overlaps,
        // per fluid the highest surface above the feet, whether the eye
        // point is under the fluid's camera surface, and the summed flow of
        // the overlapped cells for the current.
        const float height = physics.GetCurrentHeight();
        const float halfWidth = physics.GetWidth() * 0.5f - 0.001f;
        const double feetY = physics.position.y + 0.001;
        const double topY  = physics.position.y + height - 0.001;

        const int minX = static_cast<int>(std::floor(physics.position.x - halfWidth));
        const int maxX = static_cast<int>(std::floor(physics.position.x + halfWidth));
        const int minY = static_cast<int>(std::floor(feetY));
        const int maxY = static_cast<int>(std::floor(topY));
        const int minZ = static_cast<int>(std::floor(physics.position.z - halfWidth));
        const int maxZ = static_cast<int>(std::floor(physics.position.z + halfWidth));

        double     fluidHeight[3]   = {0.0, 0.0, 0.0};
        bool       eyesInside[3]    = {false, false, false};
        glm::dvec3 current[3]       = {glm::dvec3(0.0), glm::dvec3(0.0), glm::dvec3(0.0)};
        int        currentCount[3]  = {0, 0, 0};
        double     currentHeight[3] = {0.0, 0.0, 0.0};

        const double eyeY = physics.GetEyePosition().y;
        const int eyeBlockX = static_cast<int>(std::floor(physics.position.x));
        const int eyeBlockZ = static_cast<int>(std::floor(physics.position.z));
        // MC Player.isPushedByFluid: `!abilities.flying`.
        const bool wantCurrent = !physics.isFlying && !physics.noclip;

        if (const IBlockAccess* blocks = context.blockAccess) {
            for (int x = minX; x <= maxX; x++) {
                for (int z = minZ; z <= maxZ; z++) {
                    for (int y = minY; y <= maxY; y++) {
                        const FluidState fs = GetFluidState(*blocks, x, y, z);
                        if (fs.IsEmpty()) continue;
                        const glm::ivec3 cell(x, y, z);
                        const double fluidBottom = static_cast<double>(y);
                        const double fluidTop = fluidBottom + FluidHeight(*blocks, cell, fs);
                        if (fluidTop < feetY) continue;
                        const int t = static_cast<int>(fs.type);

                        if (x == eyeBlockX && z == eyeBlockZ && eyeY >= fluidBottom) {
                            const double topForCamera =
                                fluidBottom + FluidHeightForCamera(*blocks, cell, fs);
                            if (eyeY <= topForCamera) eyesInside[t] = true;
                        }
                        fluidHeight[t] = std::max(fluidTop - physics.position.y, fluidHeight[t]);

                        if (wantCurrent) {
                            glm::dvec3 flow = FluidFlow(*blocks, cell, fs);
                            currentHeight[t] = std::max(fluidHeight[t], currentHeight[t]);
                            if (currentHeight[t] < 0.4) flow *= currentHeight[t];
                            current[t] += flow;
                            ++currentCount[t];
                        }
                    }
                }
            }
        }

        constexpr int kWater = static_cast<int>(FluidType::Water);
        constexpr int kLava  = static_cast<int>(FluidType::Lava);
        physics.fluidAboveHead = false;
        if (const IBlockAccess* blocks = context.blockAccess) {
            physics.fluidAboveHead = !GetFluidState(*blocks,
                static_cast<int>(std::floor(physics.position.x)),
                static_cast<int>(std::floor(physics.position.y + 1.0 - 0.1)),
                static_cast<int>(std::floor(physics.position.z))).IsEmpty();
        }
        physics.isInWater    = fluidHeight[kWater] > 0.0;
        physics.waterDepth   = std::min(static_cast<float>(fluidHeight[kWater]), height);
        physics.isEyeInWater = eyesInside[kWater];
        physics.isInLava     = fluidHeight[kLava] > 0.0;
        physics.lavaDepth    = std::min(static_cast<float>(fluidHeight[kLava]), height);
        physics.isEyeInLava  = eyesInside[kLava];

        // MC CurrentAccumulator.applyTo, for a player: the AVERAGE of the
        // cells' flows (a mob takes the unit direction), scaled 0.014 for
        // water and 0.0023 (0.007 with FAST_LAVA) for lava, with the
        // minimum-nudge rule that keeps a still player from being pinned in
        // a slow current. Both fluids contribute when the body is in both.
        physics.fluidCurrent = glm::vec3(0.0f);
        if (wantCurrent) {
            for (int t : {kWater, kLava}) {
                if (currentCount[t] == 0) continue;
                const glm::dvec3 acc = current[t];
                if (glm::dot(acc, acc) < 9.999999747378752E-6) continue;
                const double scale = t == kWater ? 0.014 : (context.fastLava ? 0.007 : 0.0023333333333333335);
                glm::dvec3 impulse = acc * (scale / static_cast<double>(currentCount[t]));
                const double vx = physics.waterVelocity.x / 20.0, vz = physics.waterVelocity.z / 20.0;
                if (std::abs(vx) < 0.003 && std::abs(vz) < 0.003 &&
                    glm::length(impulse) < 0.0045000000000000005) {
                    impulse = glm::normalize(impulse) * 0.0045000000000000005;
                }
                physics.fluidCurrent += glm::vec3(impulse);
            }
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
                               std::vector<AABBd>& out, const AABBd* mover) {
        out.clear();

        // The mover's box as the provider takes it. Built once, outside the
        // cell loop; only asked about at all when this context has a
        // provider (a server level's mobs) and a mover was given.
        const PortalCollisionProvider* portals = mover ? context.portalCollision : nullptr;
        AABB moverF;
        if (portals) {
            moverF.min = glm::vec3(mover->min);
            moverF.max = glm::vec3(mover->max);
        }

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

                    // The Aether's aerclouds: the mover-dependent shape of
                    // AercloudBlock.getCollisionShape (AercloudBlock.hpp) —
                    // Entity::Move puts the mob's fall distance on the
                    // context, so a mob sinks into a cloud exactly as the
                    // player does and BlockBehaviors' entityInside hooks do
                    // the rest.
                    if (Aercloud::IsAercloud(bid)) {
                        if (portals && portals->IsBlockBehindPortal(x, y, z, moverF)) continue;
                        glm::vec3 lo, hi;
                        if (!AercloudColliderAt(context, bid, x, y, z, lo, hi)) continue;
                        AABBd cloudAABB;
                        cloudAABB.min = glm::dvec3(x, y, z) + glm::dvec3(lo);
                        cloudAABB.max = glm::dvec3(x, y, z) + glm::dvec3(hi);
                        out.push_back(cloudAABB);
                        continue;
                    }

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

                    // Portal passthrough, by the mover's box: the wall a gun
                    // portal is painted on is not solid for a body that fits
                    // the opening and is walking into it. The global player
                    // hook is deliberately NOT consulted here — its portal
                    // list is the player's, refreshed on the client thread;
                    // only a context-owned provider (the server level's
                    // MobPortalCollision) answers, and it calls nothing back
                    // into physics (see the re-entrancy note in MoveEntity).
                    if (portals && portals->IsBlockBehindPortal(x, y, z, moverF)) continue;
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
        PROFILE_ZONE_N("Physics.MoveEntity");

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
            // FromMinMax, not AABB{min, max}: the braces picked the (centre,
            // size) constructor, so this probe tested a box centred on the
            // entity's min corner and as WIDE AS ITS COORDINATES — at
            // z = 323,000 that was 12.6 million chunk columns per idle mob
            // per tick on both server and client (untitled1.tracy), and a
            // few hundred columns even at spawn. The resting probe below
            // had the same bug.
            if (CollidesAt(pb, context)) {
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
            const AABBd p = boxAt(probe);
            result.onGround = CollidesAt(p, context);
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
        // CollidesAt, which has its own inline cell loop. The portal
        // passthrough CollectBlockColliders now consults is the context's
        // PortalCollisionProvider, which is pure geometry against a copied
        // portal list and calls nothing back into physics — this buffer is
        // never live across a callback. Keep it that way for any provider.
        //
        // The mover for the passthrough is the box where the move STARTS:
        // a body entering a gun portal is judged by where it stands, and
        // the same answer serves every step-up candidate.
        thread_local std::vector<AABBd> colliders;
        CollectBlockColliders(region, context, colliders, &startBox);

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