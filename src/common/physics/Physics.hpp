// File: src/common/physics/Physics.hpp
#pragma once

#include <glm/glm.hpp>
#include <functional>

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
        static constexpr float SNEAK_SPEED = 1.832f;       // Sneaking speed
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