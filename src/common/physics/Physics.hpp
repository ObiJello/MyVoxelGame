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
        // (centre, size) — NOT (min, max). Brace-initialising an AABB from
        // two corners silently picks this constructor: `AABB{lo, hi}` is a
        // box centred on `lo` and `hi` blocks wide, which for an entity far
        // from the origin is a box spanning most of the way back to it.
        // Build from corners with FromMinMax.
        AABB(const glm::vec3& center, const glm::vec3& size)
            : min(center - size * 0.5f), max(center + size * 0.5f) {}
        static AABB FromMinMax(const glm::vec3& lo, const glm::vec3& hi) {
            AABB b; b.min = lo; b.max = hi; return b;
        }

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
        // MC LivingEntity.handleOnClimbable / travelInAir, per second: the
        // ladder slide and horizontal cap of 0.15 a tick, the climb of 0.2.
        static constexpr float CLIMB_SLIDE_SPEED = 3.0f;
        static constexpr float CLIMB_UP_SPEED    = 4.0f;

        // Fluid physics — MC LivingEntity.travelInWater / travelInLava run as
        // their own per-tick maps, v' = d·(v + a) − g, evaluated at
        // fractional ticks (exact at every tick boundary, smooth between;
        // see HandleMovement). Only the jump-out impulse is a constant here.
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
        // DOUBLE, like MC's Entity position: a float loses 3 cm at a
        // coordinate of 300,000, and the local player is where the camera
        // comes from (see renderer/core/RenderOrigin.hpp).
        glm::dvec3 position{0.0, 97.0, 0.0};
        glm::vec3 velocity{0.0f};
        bool isOnGround = false;
        bool isSneaking = false;
        bool isSprinting = false;
        // MC Entity.getFluidHeight(WATER/LAVA) and isEyeInFluid, refreshed
        // per step by UpdateWaterState from every cell the body overlaps
        // (a flowing cell's surface is at amount/9, a source's at 8/9, and
        // a column continues to the cell top where the same fluid is above).
        bool isInWater = false;
        float waterDepth = 0.0f;       // fluid surface above the feet, clamped to the body
        bool isEyeInWater = false;     // eye point under the water's camera surface
        bool isInLava = false;
        float lavaDepth = 0.0f;
        bool isEyeInLava = false;
        glm::vec3 waterVelocity{0.0f};     // fluid-model velocity in blocks/sec (water AND lava)
        // MC EntityFluidInteraction.CurrentAccumulator's impulse for this
        // step, in blocks per TICK (already scaled by 0.014 / the lava
        // scale, averaged over the cells as it is for a player).
        glm::vec3 fluidCurrent{0.0f};
        // MC Player.travel's swim steering reads the look vector's Y; the
        // client sets it from the camera before each step.
        float lookDirY = 0.0f;
        // MC `!level.getBlockState(containing(x, y + 0.9, z)).getFluidState()
        // .isEmpty()` — fluid at head height, the swim-steering gate.
        bool  fluidAboveHead = false;
        // MC LivingEntity.noJumpDelay: ten ticks between ground jumps while
        // the key is held (wading in shallow water re-jumps at that rate).
        float noJumpDelayTicks = 0.0f;
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

        // MC LivingEntity.onClimbable / horizontalCollision, for the ladder
        // rules in HandleMovement: the feet are in a climbable block, and the
        // last horizontal move was stopped by a block.
        bool onClimbable         = false;
        bool horizontalCollision = false;

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
        // The block landed on this step (MC Block.fallOn's fallDistanceReduction
        // — a bed halves the fall) and whether it threw the player back up
        // (Entity.restituteMovementAfterCollisions with the block's
        // bounceRestitution). Both set in HandleMovement's landing and
        // consumed by the fall-tracking block at the end of the step: the
        // reduction scales the flushed distance, the bounce then clears
        // isOnGround so the next step's jump check cannot overwrite the
        // rebound with a jump impulse.
        float landingFallReduction = 0.0f;
        bool  bouncedThisStep      = false;
        // The body overlaps an Aether aercloud cell this step (MC
        // Entity.checkInsideBlocks reaching AercloudBlock.entityInside):
        // `inAercloud` for any of the three, `inBlueAercloud` for the blue
        // one. Refreshed by UpdatePlayerPhysics; read by nothing else yet
        // (the blue cloud's bounce sound and splash are client cosmetics
        // still to come).
        bool  inAercloud           = false;
        bool  inBlueAercloud       = false;

        // ── The local player's status effects, as the physics reads them ──
        // Written by ClientPlayer from its synced effect list (UpdateMobEffect
        // S2C) before each step; defaults are "no effect".
        //   effectSpeedFactor   MOVEMENT_SPEED's SPEED (+20 %/lvl) × SLOWNESS
        //                       (-15 %/lvl) ADD_MULTIPLIED_TOTAL factor —
        //                       multiplies the walk/sprint speed like MC's
        //                       attribute (not flight, not swimming, which MC
        //                       drives from fixed speeds).
        //   effectJumpBoost     LivingEntity.getJumpBoostPower: +0.1/level on
        //                       the 0.42 jump power.
        //   effectLevitation    LEVITATION's amplifier, -1 when absent:
        //                       travelInAir's (0.05·(amp+1) − vy)·0.2 in place
        //                       of gravity.
        //   effectSlowFalling   getEffectiveGravity: 0.01 while falling.
        //   effectDolphinsGrace travelInWater's slow-down 0.96.
        // SLOW_FALLING and LEVITATION also reset the fall distance
        // (LivingEntity.aiStep's resetFallDistance).
        float effectSpeedFactor   = 1.0f;
        float effectJumpBoost     = 0.0f;
        int   effectLevitation    = -1;
        bool  effectSlowFalling   = false;
        bool  effectDolphinsGrace = false;
        // The movement attributes the local player's enchantments move,
        // written by ClientPlayer alongside the effect state:
        //   sneakingSpeed           Attributes.SNEAKING_SPEED (Swift Sneak):
        //                           the crouching input scale.
        //   waterMovementEfficiency Attributes.WATER_MOVEMENT_EFFICIENCY
        //                           (Depth Strider): travelInWater's blend
        //                           toward land drag and walking speed.
        //   movementEfficiency      Attributes.MOVEMENT_EFFICIENCY (Soul
        //                           Speed on soul blocks): how far the block
        //                           speed factor is lifted back to 1.
        // (Soul Speed's MOVEMENT_SPEED bonus rides effectSpeedFactor.)
        float sneakingSpeed           = SNEAKING_SPEED;
        float waterMovementEfficiency = 0.0f;
        float movementEfficiency      = 0.0f;
        
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
        // The player's size, 1 = vanilla. A scaled immersive portal
        // multiplies it on the way through (the mod does this through
        // Pehkui): the body, eye height, reach, speed and jump all follow,
        // so a world twice as large feels like the one you left.
        float scale = 1.0f;

        // /morph: the body is a mob's. Its box and eye replace the player's
        // (a mob has no sneak pose, so sneaking changes neither), and the
        // walk speed is the mob's — see UpdateBaseSpeed for the factor.
        bool  morphed         = false;
        float morphWidth      = WIDTH;
        float morphHeight     = HEIGHT_STANDING;
        float morphEyeHeight  = EYE_HEIGHT_STANDING;
        float morphWalkFactor = 1.0f;   // × WALK_SPEED
        // A spider morph: MC Spider.onClimbable is horizontalCollision, so a
        // wall walked into is climbed like a ladder.
        bool  morphClimbsWalls = false;
        void SetMorph(float width, float height, float eyeHeight, float movementSpeed) {
            morphed        = true;
            morphWidth     = width;
            morphHeight    = height;
            morphEyeHeight = eyeHeight;
            // MC: a mob's ground acceleration is speed × zza, and Mob.setSpeed
            // sets zza = speed too, so it is speed²; the player's is
            // MOVEMENT_SPEED (0.1) × a full input. Same friction after that,
            // so the mob walks at speed² / 0.1 of the player's walk: a
            // zombie (0.23) at 0.53×, a spider (0.3) at 0.9×.
            morphWalkFactor = movementSpeed > 0.0f ? (movementSpeed * movementSpeed) / 0.1f : 1.0f;
        }
        void ClearMorph() {
            morphed = false;
            morphWidth = WIDTH; morphHeight = HEIGHT_STANDING; morphEyeHeight = EYE_HEIGHT_STANDING;
            morphWalkFactor = 1.0f;
            morphClimbsWalls = false;
        }

        float GetWidth() const { return (morphed ? morphWidth : WIDTH) * scale; }
        float GetEyeHeight() const {
            if (morphed) return morphEyeHeight * scale;
            return (isSneaking ? EYE_HEIGHT_SNEAKING : EYE_HEIGHT_STANDING) * scale;
        }

        glm::dvec3 GetEyePosition() const {
            return position + glm::dvec3(0.0, static_cast<double>(GetEyeHeight() + stepVisualOffset), 0.0);
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
            if (morphed) return morphHeight * scale;
            return (isSneaking ? HEIGHT_SNEAKING : HEIGHT_STANDING) * scale;
        }

        // Get player's AABB
        AABB GetAABB() const {
            float height = GetCurrentHeight();
            return AABB(
                glm::vec3(static_cast<float>(position.x), static_cast<float>(position.y) + height * 0.5f,
                          static_cast<float>(position.z)),
                glm::vec3(GetWidth(), height, GetWidth())
            );
        }
    };

    // Cross-portal collision for ONE mover, carried on its PhysicsContext.
    //
    // The two global hooks below (SetPortalPassthroughFn / SetPortalExtra-
    // SolidFn) are the local player's: one process-wide pointer, refreshed
    // once a frame around the player's own box. A server level's mobs need
    // the same two answers for their OWN boxes, from their own level's
    // portals, on the server thread — so a context may carry a provider,
    // and when it does, that provider answers instead of the globals. (The
    // integrated server used to fall through to the client's hooks from
    // the server thread: a data race, and the wrong portal list.)
    class PortalCollisionProvider {
    public:
        virtual ~PortalCollisionProvider() = default;
        // May IsFarSideSolid ever answer true right now? The all-air region
        // early-out in the box test stands aside only while it may.
        virtual bool HasFarSideSolidity() const = 0;
        // A solid cell of this world behind a portal the box is going
        // through: not solid for this box.
        virtual bool IsBlockBehindPortal(int x, int y, int z, const AABB& box) const = 0;
        // An empty cell that is solid on the far side of a portal the box
        // is going through: a full cube for this box.
        virtual bool IsFarSideSolid(int x, int y, int z, const AABB& box) const = 0;
    };

    // **NEW**: Physics context that holds the world reference
    struct PhysicsContext {
        const IBlockAccess* blockAccess = nullptr;
        // Cross-portal collision for this mover; null = the global hooks.
        const PortalCollisionProvider* portalCollision = nullptr;

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
        // MC `getFluidState(pos).is(FluidTags.WATER)` — true for water AND for
        // waterlogged blocks. Delegates to IBlockAccess::ContainsWater.
        bool ContainsWater(int x, int y, int z) const;

        // MC EnvironmentAttributes.FAST_LAVA — true in the nether, where lava
        // currents push at 0.007 a tick instead of 0.0023. Set by the caller
        // that knows the dimension (the client's player).
        bool fastLava = false;

        // MC EntityCollisionContext — the entity the collision is being
        // resolved FOR. Left at `collisionEntity = false` it is
        // CollisionContext.empty() (spawn tests, placement, particles). Only
        // the Aether's aerclouds read it (AercloudBlock.hpp): their shape
        // depends on how far the mover has fallen. Set by the movers that
        // have an entity behind them — the local player (UpdatePlayerPhysics)
        // and Entity::Move / MoveApproximate.
        bool  collisionEntity       = false;
        float collisionFallDistance = 0.0f;
        bool  collisionFallFlying   = false;
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

    // The other half of cross-portal collision (immersive portals): a cell
    // that is NOT solid in this world may be solid on the far side of a
    // portal the entity is about to go through. When the hook says so the
    // cell collides as a full cube — the far floor holds you up before your
    // feet have crossed, and the far wall stops you. Same cost rule as the
    // passthrough hook: plain pointer, null-checked in the hot loop, and only
    // consulted for cells that would otherwise be walked through.
    using PortalExtraSolidFn = bool(*)(int x, int y, int z, const AABB& playerAABB);
    void SetPortalExtraSolidFn(PortalExtraSolidFn fn);
    // Is any portal close enough to the local player for the extra-solid
    // hook to have something to say? The box collision test has an
    // "all-air region" early-out that answers before any cell is visited —
    // and the cells the hook makes solid ARE all air on this side (the
    // void under the world's floor, the space beyond a wrap border), so
    // with a portal engaged the early-out must be skipped. Set once per
    // frame by whoever refreshes the hook's portal list.
    void SetPortalCollisionActive(bool active);
    bool PortalCollisionActive();

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

    // The cheap mover behind Entity::MoveApproximate, over plain values so the
    // compact falling-block representation can run exactly the same step:
    // apply `delta`, and if the box then overlaps anything undo Y first
    // (landing), then the whole move (a wall). Feet-anchored box of the given
    // half extents. Returns true when a vertical hit was resolved — the
    // caller's cue to reset its fall distance. `onGround` is both read (was
    // it resting last step) and written.
    bool MoveApproximate(glm::dvec3& pos, glm::dvec3& velocity, const glm::vec3& halfExtents,
                         const glm::dvec3& delta, bool& onGround,
                         bool& horizontalCollision, bool& verticalCollision,
                         const PhysicsContext& context);

    // MC BlockState.isCollisionShapeFullBlock — does the block at this cell
    // present a full 1×1×1 collision cube? Entity.moveTowardsClosestSpace uses
    // this to decide which neighbouring cells count as escape routes for an
    // entity stuck inside a block; a slab or stair neighbour is "open".
    bool IsCollisionShapeFullBlock(const PhysicsContext& context, int x, int y, int z);

    // How high fluid stands above `box.min.y` (the entity's feet), scanning
    // every cell the box overlaps — MC Entity.getFluidHeight(FluidTags.WATER /
    // LAVA). A cell's surface is FluidState.getHeight: amount/9 (8/9 for a
    // source), or the full cell when the same fluid continues above it.
    // Returns 0 when the box touches no fluid of the requested kind.
    //
    // `lava` selects lava instead of water (water blocks plus everything
    // waterlogged).
    double FluidHeightAbove(const AABB& box, bool lava, const PhysicsContext& context);

    // MC Entity.moveTowardsClosestSpace, verbatim: an entity stuck inside a
    // full collision block gets its velocity pointed at the nearest open
    // neighbour (N/S/W/E/UP — never down) of the cell containing
    // `stuckPoint`, at `escapeSpeed` blocks/tick, while the other two axes
    // keep 0.75 of their old velocity. `escapeSpeed` is MC's
    // `random.nextFloat() * 0.2 + 0.1`; the caller rolls it so this stays
    // RNG-free (the server rolls per entity, the client never runs this).
    void EscapeTowardsClosestSpace(const glm::dvec3& stuckPoint, float escapeSpeed,
                                   glm::dvec3& velocity, const PhysicsContext& context);

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
        static AABBd FromMinMax(const glm::dvec3& lo, const glm::dvec3& hi) { AABBd b; b.min = lo; b.max = hi; return b; }

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

    // The collision test in DOUBLE: the box is measured against the blocks
    // in a local integer frame, so a player at a coordinate of 300,000
    // collides to the same precision as one at spawn (a float box there is
    // quantised to 3 cm). The float overload forwards here.
    bool CollidesAt(const AABBd& box, const PhysicsContext& context);

    // Widen a float AABB. Lossless — every float is representable as a double.
    inline AABBd ToAABBd(const AABB& b) {
        return AABBd{ glm::dvec3(b.min), glm::dvec3(b.max) };
    }

    // Every block collision box overlapping `region`, in world coordinates.
    //
    // Split out from the collide math because the step-up branch needs to
    // reuse ONE collider set across several candidate heights; re-walking the
    // block grid per candidate is the same query up to four times over.
    //
    // `mover`, when given, is the box of the entity the colliders are for:
    // a cell behind a portal that box is going through is left out
    // (PortalCollisionProvider::IsBlockBehindPortal, the context's own
    // provider only — never the global player hook). The one box stands
    // for every step-up candidate: the provider's answer is about the body
    // fitting the opening and standing near the surface, which a
    // half-block lift does not change. Null = no portal passthrough (the
    // block-only callers: shape scans, leash checks, particles).
    void CollectBlockColliders(const AABBd& region, const PhysicsContext& context,
                               std::vector<AABBd>& out, const AABBd* mover = nullptr);

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
    bool CheckCollision(const glm::dvec3& position, const PlayerPhysics& physics,
                       const PhysicsContext& context);

    bool HasSupportBelow(const glm::dvec3& position, const PlayerPhysics& physics,
                        const PhysicsContext& context);

    void UpdateWaterState(PlayerPhysics& physics, const PhysicsContext& context);

    // **UPDATED**: Movement helper functions now take PhysicsContext
    void HandleJump(PlayerPhysics& physics, bool jumpPressed, float deltaTime,
                   const PhysicsContext& context);

    void UpdateBaseSpeed(PlayerPhysics& physics);

    void ApplyGravity(PlayerPhysics& physics, float deltaTime, const PhysicsContext& context);
    // MC LivingEntity.onClimbable for the player: the feet are in a
    // climbable block (ladder, vines, scaffolding) or an open trapdoor over
    // a ladder facing the same way.
    bool OnClimbable(const PlayerPhysics& physics, const PhysicsContext& context);

    void HandleMovement(PlayerPhysics& physics, const glm::vec3& movementInput,
                       bool jumpPressed, float deltaTime, const PhysicsContext& context);

} // namespace Game