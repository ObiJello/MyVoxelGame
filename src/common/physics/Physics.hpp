// File: src/common/physics/Physics.hpp
#pragma once

#include <glm/glm.hpp>
#include <algorithm>
#include <cstdint>
#include "common/world/block/Blocks.hpp"
#include "common/world/block/BlockState.hpp"
#include <functional>
#include <vector>

namespace Game {

    // Forward declarations
    class Chunk;
    class ChunkProvider;
    struct IBlockAccess;  // Forward declare the interface
    enum class BlockID : uint16_t;  // Forward declare BlockID

    // AABB structure for collision detection
    struct AABB {
        glm::vec3 min;
        glm::vec3 max;

        AABB() = default;
        AABB(const glm::vec3& center, const glm::vec3& size)
            : min(center - size * 0.5f), max(center + size * 0.5f) {}

        bool Intersects(const AABB& other) const {
            return (min.x < other.max.x && max.x > other.min.x) &&
                   (min.y < other.max.y && max.y > other.min.y) &&
                   (min.z < other.max.z && max.z > other.min.z);
        }
    };

    // Player physics state and parameters
    struct PlayerPhysics {
        // Physical constants
        static constexpr float WALK_SPEED = 4.317f;        // Walking speed in blocks per second
        static constexpr float SPRINT_SPEED = 5.612f;      // Sprinting speed
        // MC Attributes.SNEAKING_SPEED (default 0.3). Sneaking is an INPUT
        // scale applied in LocalPlayer.modifyInput — it is NOT a MOVEMENT_SPEED
        // modifier the way sprinting is, which is why crouching doesn't change
        // vanilla's FOV. Effective crouch speed = WALK_SPEED * 0.3 ≈ 1.295 b/s.
        static constexpr float SNEAKING_SPEED = 0.3f;
        static constexpr float JUMP_VELOCITY = 9.04f;      // Velocity for a 1.25-block jump
        static constexpr float GRAVITY = -32.656f;         // Gravity acceleration
        static constexpr float TERMINAL_VELOCITY = -78.4f; // Terminal velocity

        // Water physics — derived from MC steady-state values, runs per-frame (no tick accumulator)
        // MC steady states: walk=2.0 b/s, sink=-0.5 b/s, bob=+3.5 b/s, sprint=4.0 b/s
        // Decay rate k = -20*ln(0.8) = 4.463 (continuous equivalent of MC's 0.8/tick friction)
        static constexpr float WATER_DECAY = 4.463f;                // Continuous friction decay rate
        static constexpr float WATER_WALK_ACCEL = 8.926f;           // 2.0 * 4.463 — gives 2.0 b/s steady state
        static constexpr float WATER_SPRINT_ACCEL = 8.926f;         // Same accel, different friction for sprint
        static constexpr float WATER_SPRINT_DECAY = 2.107f;         // -20*ln(0.9) — sprint friction (0.9/tick)
        static constexpr float WATER_GRAVITY_ACCEL = 2.232f;        // 0.5 * 4.463 — gives -0.5 b/s steady state
        static constexpr float WATER_BOB_ACCEL = 15.621f;           // 3.5 * 4.463 — gives +3.5 b/s steady state
        static constexpr float WATER_JUMP_OUT = 6.0f;               // 0.3 blocks/tick * 20 = 6.0 b/s

        static constexpr float OVERHANG_MARGIN = 0.125f; // Allowable overhang distance

        // Noclip mode flight speeds
        static constexpr float NOCLIP_HORIZONTAL_SPEED = 10.0f;       // Default horizontal flight speed
        static constexpr float NOCLIP_VERTICAL_SPEED = 10.0f;         // Default vertical flight speed
        static constexpr float NOCLIP_SPRINT_HORIZONTAL_SPEED = 50.0f; // Sprint (Ctrl) horizontal speed
        static constexpr float NOCLIP_SPRINT_VERTICAL_SPEED = 50.0f;   // Sprint (Ctrl) vertical speed

        // Creative flight (MC-style: collision on, gravity off).
        // MC's per-tick model: horizontal speed = abilities.flyingSpeed
        // (0.05) fed through the same friction-0.91 air chain as walking,
        // which steady-states at ~10.89 b/s; sprint doubles the input
        // (Player.getFlyingSpeed) → ~21.78 b/s. Vertical is a direct
        // velocity write of flyingSpeed*3 blocks/tick = 7.5 b/s
        // (LocalPlayer.aiStep's deltaMovement.add(0, input*speed*3, 0)).
        static constexpr float FLY_HORIZONTAL_SPEED = 10.89f;
        static constexpr float FLY_SPRINT_MULTIPLIER = 2.0f;
        static constexpr float FLY_VERTICAL_SPEED = 7.5f;

        // Player dimensions
        static constexpr float WIDTH = 0.6f;
        static constexpr float HEIGHT_STANDING = 1.8f;
        static constexpr float HEIGHT_SNEAKING = 1.49f;
        static constexpr float EYE_HEIGHT_STANDING = 1.62f;
        static constexpr float EYE_HEIGHT_SNEAKING = 1.42f;

        // Movement momentum system
        static constexpr float CORRECT_JUMP_TIME_WINDOW = 0.1f; // 100 milliseconds window
        static constexpr float SPEED_INCREMENT = 0.5f; // Speed increase per correct jump
        static constexpr float MAX_SPEED_MULTIPLIER = 2.0f; // Max speed multiplier

        // Current player state
        glm::vec3 position{0.0f, 97.0f, 0.0f};
        glm::vec3 velocity{0.0f};
        bool isOnGround = false;
        bool isSneaking = false;
        bool isSprinting = false;
        bool isInWater = false;
        float waterDepth = 0.0f;       // How deep the player is submerged (0 = not in water)
        bool isEyeInWater = false;     // True when eyes are submerged
        glm::vec3 waterVelocity{0.0f};     // Water velocity in blocks/sec
        bool noclip = false;

        // Creative flight state — MC Abilities.flying / Abilities.mayfly.
        // mayFly is server-granted (PlayerAbilitiesS2C, creative only);
        // isFlying toggles via double-tap space and cancels on landing.
        // Unlike noclip, flying keeps full collision resolution.
        bool isFlying = false;
        bool mayFly = false;

        // True when THIS physics step fired a ground-jump impulse. Cleared at
        // the top of UpdatePlayerPhysics; ClientPlayer accumulates it into the
        // per-tick move packet for the server's jump-exhaustion accounting.
        bool didJumpThisStep = false;

        // Fall tracking — done HERE (per physics step) because the client is
        // the only one that knows exact ground contact. The server's 20 Hz
        // position snapshots miss bunny-hop landings entirely (land + jump
        // inside one tick), which made server-side accumulation stack hop
        // descents into phantom fall damage. `fallDistance` accumulates
        // while airborne; on landing it's flushed into `landedFallDistance`
        // for ClientPlayer to forward in the next move packet. Water, noclip
        // and creative flight break falls (MC resetFallDistance sites).
        float fallDistance       = 0.0f;
        float landedFallDistance = 0.0f;   // consumed by ClientPlayer::UpdatePhysics
        
        // Mutable flight speeds for noclip mode
        float noclipHorizontalSpeed = NOCLIP_HORIZONTAL_SPEED;
        float noclipVerticalSpeed = NOCLIP_VERTICAL_SPEED;

        // Timing and momentum variables
        float totalTime = 0.0f;
        float lastLandingTime = 0.0f;
        float lastJumpTime = 0.0f;
        int consecutiveJumps = 0;
        float currentSpeed = WALK_SPEED;
        float baseSpeed = WALK_SPEED;
        bool wasOnGround = false;

        // Get current eye position. Includes `stepVisualOffset` (≤ 0) so the
        // camera lags behind a freshly-stepped position, producing MC's
        // smooth rise instead of an instant pop. Mirrors MC's
        // `Camera.setup()` lerp of `entity.yo → entity.getY()` across the
        // partial-tick interval (Camera.java:85): when the physics tick
        // snaps the player up onto a slab/stair, the rendered camera
        // interpolates over one tick rather than teleporting.
        glm::vec3 GetEyePosition() const {
            float eyeHeight = isSneaking ? EYE_HEIGHT_SNEAKING : EYE_HEIGHT_STANDING;
            return position + glm::vec3(0.0f, eyeHeight + stepVisualOffset, 0.0f);
        }

        // Visual-only Y offset applied on top of the physical position. When
        // an auto-step lifts the player by `Δy`, we shove this offset to
        // `-Δy` so the camera starts at the OLD height and the visible
        // model/eye smoothly rises as the offset decays to 0 each frame.
        // Physics, raycast, and collision all continue to use the actual
        // (post-step) position — only the camera Y reads this. Decayed in
        // `Physics::Update` per frame with a 50 ms time-constant (= one
        // server tick) to match MC's per-tick lerp.
        float stepVisualOffset = 0.0f;

        // Get current height
        float GetCurrentHeight() const {
            return isSneaking ? HEIGHT_SNEAKING : HEIGHT_STANDING;
        }

        // Get player's AABB
        AABB GetAABB() const {
            float height = GetCurrentHeight();
            return AABB(
                glm::vec3(position.x, position.y + height * 0.5f, position.z),
                glm::vec3(WIDTH, height, WIDTH)
            );
        }
    };

    // **NEW**: Physics context that holds the world reference
    struct PhysicsContext {
        const IBlockAccess* blockAccess = nullptr;

        // Helper methods that use the block access
        BlockID GetBlock(int x, int y, int z) const;
        // Index into the block's own state list. Needed alongside GetBlock
        // wherever a collision shape is built: rotation lives in the model, so
        // a stair or a segmented ground-cover block occupies a different part
        // of its cell per state. Block-only accessors report 0 (all default),
        // which is the correct answer for them.
        BlockState GetBlockState(int x, int y, int z) const;
        bool IsBlockSolid(int x, int y, int z) const;
        bool IsChunkLoaded(int chunkX, int chunkZ) const;
    };

    // Function to check if a block is solid for collision
    using BlockCollisionFunction = std::function<bool(int x, int y, int z)>;

    // Optional collision filter consulted by player-block collision
    // (CheckCollision + HasSupportBelow). When set and it returns true
    // for a given (block coords, player AABB), the block is treated as
    // non-solid for that specific player at that position. Used by
    // client physics so the player can walk THROUGH the 1×2 opening of
    // an active portal pair — but only when their AABB fits inside the
    // opening laterally. If they're standing off-center so part of their
    // body would intersect the wall material AROUND the opening, the
    // block stays solid.
    //
    // The AABB context is what makes the check directional: a player
    // approaching the front face along the portal normal slides through;
    // a player approaching from the side has AABB extent that exceeds
    // the opening rectangle in the tangent axes → blocked.
    //
    // Plain function pointer (not std::function) — collision is in a
    // hot loop, the null-check + call cost has to stay near zero.
    using PortalPassthroughFn = bool(*)(int x, int y, int z, const AABB& playerAABB);
    void SetPortalPassthroughFn(PortalPassthroughFn fn);

    // **UPDATED**: Main physics update function now takes PhysicsContext
    void UpdatePlayerPhysics(PlayerPhysics& physics,
                            const glm::vec3& movementInput,
                            bool jumpPressed,
                            bool sneakPressed,
                            float deltaTime,
                            const PhysicsContext& context);

    // ── Generic (non-player) collision ─────────────────────────────────────
    //
    // Does `box` overlap any block with collision? This is the shared core
    // that CheckCollision is built on — every rule that matters lives here:
    // `BlockRegistry::HasCollision` is the single source of truth (never a
    // render-layer solidity flag, which desyncs host and joiner), per-state
    // shapes come from GetBlockShape, and the portal-passthrough hook gets
    // its say. Anything that needs to collide a box against the world should
    // call THIS rather than reimplementing the walk.
    bool CollidesAt(const AABB& box, const PhysicsContext& context);

    // What MoveAABB ran into. `onGround` is true when the entity is resting on
    // something after the move, whether it landed this step or was already
    // supported.
    struct MoveResult {
        bool onGround  = false;
        bool collidedX = false;
        bool collidedY = false;
        bool collidedZ = false;
    };

    // Move an axis-aligned entity through the world, resolving one axis at a
    // time and zeroing the velocity component of any axis that hit something.
    // Mirrors MC Entity.move(MoverType.SELF, …).
    //
    // `pos` is the entity's FEET position (MC convention) and is updated in
    // place; `velocity` is in blocks per TICK, not per second, and is likewise
    // updated. `halfExtents` is (halfWidth, halfHeight, halfWidth).
    MoveResult MoveAABB(glm::dvec3& pos, glm::dvec3& velocity,
                        const glm::vec3& halfExtents,
                        const PhysicsContext& context);

    // ── MC-faithful entity mover ───────────────────────────────────────────
    //
    // MoveAABB above is all-or-nothing per axis: a blocked axis is cancelled
    // outright rather than resolved flush against the surface, and there is no
    // step height. That is fine for item entities (they settle in a cell and
    // stay there) but wrong for anything that walks: a mob falling at 0.4
    // blocks/tick would stop up to 0.4 blocks ABOVE the floor and hover there,
    // and it could never climb a single block.
    //
    // The functions below port MC's real mover. They are additive on purpose —
    // MoveAABB keeps its behaviour so item entities are untouched.

    // A DOUBLE-PRECISION box, used by the entity mover and nothing else.
    //
    // Game::AABB is float, which is fine for the queries it was built for
    // (overlap tests, culling, selectors) and NOT fine for resolving a
    // collision: the mover computes how far it may travel as
    // `colliderFace - boxFace` and then ADDS that to a double position. Round
    // the position to float first and the subtraction no longer cancels, so an
    // entity lands a few microns off the surface instead of exactly on it —
    // above at some coordinates, BELOW at others. Landing below puts the
    // entity's feet inside the block it is standing on, and MoveControl's
    // auto-jump ("my own cell has a collision top above my feet") then fires
    // every single tick. That was the mystery mob bouncing.
    //
    // MC's AABB is double throughout for exactly this reason.
    struct AABBd {
        glm::dvec3 min{0.0};
        glm::dvec3 max{0.0};

        // MC AABB.distanceToSqr: squared distance from `p` to the nearest point
        // ON the box, zero when inside. Used for interaction reach, which MC
        // measures eye-to-box rather than centre-to-centre — see
        // Player.isWithinEntityInteractionRange (Player.java:1905).
        double DistanceToSqr(const glm::dvec3& p) const {
            const double dx = std::max({min.x - p.x, 0.0, p.x - max.x});
            const double dy = std::max({min.y - p.y, 0.0, p.y - max.y});
            const double dz = std::max({min.z - p.z, 0.0, p.z - max.z});
            return dx * dx + dy * dy + dz * dz;
        }

        bool Intersects(const AABBd& o) const {
            return (min.x < o.max.x && max.x > o.min.x) &&
                   (min.y < o.max.y && max.y > o.min.y) &&
                   (min.z < o.max.z && max.z > o.min.z);
        }
    };

    // Widen a float AABB. Lossless — every float is representable as a double.
    inline AABBd ToAABBd(const AABB& b) {
        return AABBd{ glm::dvec3(b.min), glm::dvec3(b.max) };
    }

    // Every block collision box overlapping `region`, in world coordinates.
    //
    // Split out from the collide math because the step-up branch needs to
    // reuse ONE collider set across several candidate heights; re-walking the
    // block grid per candidate is the same query up to four times over.
    void CollectBlockColliders(const AABBd& region, const PhysicsContext& context,
                               std::vector<AABBd>& out);

    // MC Shapes.collide: how far can `box` travel along `axis` (0=X, 1=Y,
    // 2=Z) before it touches something? Returns a displacement with the same
    // sign as `desired` and magnitude <= |desired|.
    //
    // Only colliders that overlap the box on the OTHER two axes can block it,
    // which is what makes the per-axis sequence in MoveEntity correct.
    double CollideAxis(int axis, const AABBd& box, double desired,
                       const std::vector<AABBd>& colliders);

    // What MoveEntity ran into. Wider than MoveResult because MC's travel()
    // and the mob auto-jump both read `horizontalCollision`.
    struct EntityMoveResult {
        bool onGround              = false;
        bool horizontalCollision   = false;
        bool verticalCollision     = false;
        bool collidedX             = false;
        bool collidedY             = false;
        bool collidedZ             = false;
        // The step-up branch fired this move. Purely diagnostic.
        bool steppedUp             = false;
    };

    // Move an entity through the world — MC Entity.move(MoverType.SELF, …)
    // via Entity.collide (Entity.java:1089) and collideWithShapes (:1170).
    //
    // Differences from MoveAABB that matter:
    //   * axes resolve to the maximum PERMITTED distance, so the entity ends
    //     up flush against what it hit rather than short of it;
    //   * axes are resolved largest-component-first (MC Direction.axisStepOrder),
    //     not in a fixed Y/X/Z order;
    //   * `maxUpStep` enables MC's candidate-height step-up: every collider top
    //     face in (0, maxUpStep] is tried in ascending order and the one that
    //     travels furthest horizontally wins. This is NOT "try 0.6 and revert" —
    //     that older scheme picks the wrong height on stairs and slabs.
    //
    // `pos` is the entity's FEET and is updated in place; `velocity` is in
    // blocks per TICK. Blocked axes have their velocity component zeroed, and
    // collision flags use MC's 1.0E-5 comparison epsilon.
    //
    // `wasOnGround` is the entity's onGround state BEFORE this move. MC reads
    // `onGround()` inside collide() for the step-up test, and it matters: a mob
    // that walks into a step with its Y velocity already zeroed produces no
    // vertical collision this tick, so without the previous state the step-up
    // branch would never fire for it.
    EntityMoveResult MoveEntity(glm::dvec3& pos, glm::dvec3& velocity,
                                const glm::vec3& halfExtents,
                                float maxUpStep, bool wasOnGround,
                                const PhysicsContext& context);

    // **UPDATED**: Collision detection functions now take PhysicsContext
    bool CheckCollision(const glm::vec3& position, const PlayerPhysics& physics,
                       const PhysicsContext& context);

    bool HasSupportBelow(const glm::vec3& position, const PlayerPhysics& physics,
                        const PhysicsContext& context);

    void UpdateWaterState(PlayerPhysics& physics, const PhysicsContext& context);

    // **UPDATED**: Movement helper functions now take PhysicsContext
    void HandleJump(PlayerPhysics& physics, bool jumpPressed, float deltaTime,
                   const PhysicsContext& context);

    void UpdateBaseSpeed(PlayerPhysics& physics);

    void ApplyGravity(PlayerPhysics& physics, float deltaTime, const PhysicsContext& context);

    void HandleMovement(PlayerPhysics& physics, const glm::vec3& movementInput,
                       bool jumpPressed, float deltaTime, const PhysicsContext& context);

} // namespace Game