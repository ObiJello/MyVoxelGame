// File: src/common/entity/Entity.hpp
//
// MC net.minecraft.world.entity.Entity — the base of every mob.
//
// UNITS: everything here is in MC's PER-TICK convention, like ItemEntity and
// unlike PlayerPhysics. `velocity` is blocks per tick, gravity is 0.08 per
// tick, and no dt appears anywhere. ItemEntity.hpp:5-11 explains why mixing the
// two is the single easiest way to get an entity that falls at a wildly wrong
// speed; the same warning applies with more force here, because mobs also
// derive their walk speed from an attribute measured in the same units.
//
// ROTATIONS: MC keeps four separate yaw values and they are not
// interchangeable —
//   yRot      the body's facing, what movement is applied relative to
//   yBodyRot  the rendered torso, which lags yRot
//   yHeadRot  where the head points, clamped to within 75 degrees of yBodyRot
//   xRot      pitch, positive DOWN — MC's convention, which is now the
//             engine's everywhere including the camera (Game::Mth::ViewVector)
// Each has an `*O` partner holding last tick's value for render interpolation.
#pragma once

#include "common/core/Uuid.hpp"
#include "common/entity/EntityType.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/portal/PortalState.hpp"

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Game {

    struct EntityLevel;
    class  ILevelWrite;
    // Tag must be `class` to match the definition in LivingEntity.hpp: MSVC
    // mangles class and struct differently, so a mismatched tag links fine on
    // Clang and gives LNK2019 on MSVC (see CLAUDE.md).
    class  LivingEntity;

    // ── Entity id space ────────────────────────────────────────────────────
    //
    // Mob ids live at or above this value. The split matters because
    // RemoveEntitiesS2CPacket is shared by every entity kind and the client
    // decides which of its maps an incoming id belongs to purely by range —
    // see the matching note on Game::kItemEntityIdBase in ItemEntity.hpp.
    //
    //   [0, kItemEntityIdBase)          players (= connection ids)
    //   [kItemEntityIdBase, kMobBase)   dropped items
    //   [kMobEntityIdBase, kXpOrbBase)  mobs and projectiles
    //   [kXpOrbEntityIdBase, ...)       experience orbs
    constexpr int32_t kMobEntityIdBase   = 0x0200'0000;
    constexpr int32_t kXpOrbEntityIdBase = 0x0300'0000;

    inline bool IsMobEntityId(int32_t id) {
        return id >= kMobEntityIdBase && id < kXpOrbEntityIdBase;
    }
    inline bool IsXpOrbEntityId(int32_t id) { return id >= kXpOrbEntityIdBase; }

    enum class RemovalReason : uint8_t {
        None = 0,
        Killed,      // died — loot has already been dropped
        Discarded,   // despawned, or removed for a reason the player never sees
        // The entity still EXISTS, it is just not resident here any more.
        //
        // The distinction is load-bearing for saved references (see EntityRef):
        // MC keeps a reference's UUID when its referent merely unloaded or
        // changed dimension, and drops it only when the referent actually
        // died. A wolf whose owner logged out must still know its owner.
        UnloadedToChunk,
        ChangedDimension,
    };

    // MC net.minecraft.world.entity.Pose, ordinals verbatim — they are the wire
    // encoding, so the order is not ours to change.
    //
    // MC uses the pose for two unrelated jobs and this port only needs the
    // second. It sizes the bounding box (a swimming player is 0.6 tall), which
    // nothing here does yet; and it is the SYNCHED state that drives episodic
    // animations. A frog croaks because the server put it in CROAKING and the
    // client saw DATA_POSE change — the croak timer itself never crosses the
    // wire.
    enum class Pose : uint8_t {
        Standing = 0,
        FallFlying = 1,
        Sleeping = 2,
        Swimming = 3,
        SpinAttack = 4,
        Crouching = 5,
        LongJumping = 6,
        Dying = 7,
        Croaking = 8,
        UsingTongue = 9,
        Sitting = 10,
        Roaring = 11,
        Sniffing = 12,
        Emerging = 13,
        Digging = 14,
        Sliding = 15,
        Shooting = 16,
        Inhaling = 17,
    };

    // MC Pose.BY_ID uses OutOfBoundsStrategy.ZERO — an unknown id decodes to
    // STANDING rather than throwing, so an older client meeting a newer pose
    // renders a plain mob instead of dropping the packet.
    inline Pose PoseById(uint8_t id) {
        return id <= static_cast<uint8_t>(Pose::Inhaling) ? static_cast<Pose>(id)
                                                          : Pose::Standing;
    }

    class Entity {
    public:
        explicit Entity(EntityTypeId type, EntityLevel* level);
        virtual ~Entity() = default;

        Entity(const Entity&) = delete;
        Entity& operator=(const Entity&) = delete;

        // ── Identity ───────────────────────────────────────────────────────
        int32_t      GetId()   const { return m_id; }

        // ── Persistent identity ─────────────────────────────────────────────
        //
        // GetId() above is a per-LEVEL, per-SESSION handle: reset on every
        // launch, reused freely, and identical between dimensions. It cannot
        // name an entity across a save. This can.
        //
        // Nil until a manager mints it, which is the same lifecycle SetId has —
        // construction happens all over common/entity/, id assignment happens
        // in exactly one place per manager.
        const Uuid& GetUuid() const { return m_uuid; }
        void        SetUuid(const Uuid& uuid) { m_uuid = uuid; }
        // Mint ONLY if unset, so the loader can stamp a saved UUID before
        // handing the entity to its manager and have that survive.
        void        MintUuidIfUnset() { if (UuidIsNil(m_uuid)) m_uuid = RandomUuid(); }

        // MC EntityType.canSerialize. False means "never written to
        // entities/*.mca" — projectiles opt out, because their one meaningful
        // field is an owner pointer that cannot round-trip and their lifetime
        // is seconds.
        virtual bool CanSerialize() const { return true; }
        void         SetId(int32_t id) { m_id = id; }
        EntityTypeId GetType() const { return m_type; }
        const EntityTypeInfo& TypeInfo() const { return GetEntityTypeInfo(m_type); }
        EntityLevel* Level() const { return m_level; }
        // Move the entity to another level's simulation. Only the portal
        // travel code calls this, after taking the entity out of its old
        // manager and before handing it to the new one; everything derived
        // from the old level (a navigation's block view, a target in the
        // old world) has to be reset by the override.
        virtual void SetLevel(EntityLevel* level) { m_level = level; }

        // ── Position and motion ────────────────────────────────────────────
        // `position` is the entity's FEET, matching MoveEntity and MC.
        glm::dvec3 position{0.0};
        glm::dvec3 velocity{0.0};   // blocks per TICK

        // MC Entity.getKnownMovement — how this entity actually moved this
        // tick. For a simulated entity that IS the velocity; a player's view
        // overrides it with the client-reported displacement, because its
        // `velocity` field is a knockback accumulator, never integrated
        // movement (MC ServerPlayer.getKnownMovement makes the same split).
        virtual glm::dvec3 GetKnownMovement() const { return velocity; }

        // Settled-physics parking (see PrimedTnt::Tick). True while the
        // entity's mover is provably a no-op: on the ground, velocity zeroed
        // by the resting path, and no block written anywhere since
        // physicsParkedEpoch. Cleared by any impulse (AddDeltaMovement), any
        // externally applied motion or position, or an epoch change.
        bool     physicsParked      = false;
        uint64_t physicsParkedEpoch = 0;
        // CLIENT-side only: when set, this remote entity's local physics
        // simulation is skipped this tick — its motion comes from server
        // packets plus interpolation. The client mob manager sets it per tick
        // for entities beyond the detail radius (or past the physics budget);
        // a chaotic million-entity cascade cannot afford a million local
        // movers for smoothness the packet interpolation already provides.
        bool clientPhysicsLod = false;

        // Last tick's position, captured by setOldPosAndRot before tick().
        // Read by the walk-animation distance and the body-rotation control.
        glm::dvec3 oldPosition{0.0};

        float yRot     = 0.0f;
        float xRot     = 0.0f;   // pitch, positive DOWN
        float yRotO    = 0.0f;
        float xRotO    = 0.0f;

        bool onGround            = false;
        bool horizontalCollision = false;
        bool verticalCollision   = false;

        float  fallDistance = 0.0f;
        int    tickCount    = 0;
        bool   firstTick    = true;

        // MC Entity.resetFallDistance.
        void ResetFallDistance() { fallDistance = 0.0f; }

        // The cells CheckInsideBlocks can touch this tick: the box swept from
        // oldPosition to position, shrunk by the same epsilon it uses. Exposed
        // so a caller can ask IsRegionAllAir over it BEFORE paying for the
        // call — that early-out is read-only, so it can be asked from a
        // worker thread, and it answers "nothing to do" for almost every
        // entity on almost every tick.
        void SweptInsideRegion(glm::ivec3& outLo, glm::ivec3& outHi) const;

        // MC Entity.getMaxFallDistance — how far the PATHFINDER will let this
        // entity drop on purpose. LivingEntity/Mob/Creeper override it.
        virtual int GetMaxFallDistance() const { return 3; }

        // MC Entity.fireImmune — the EntityType builder flag (blaze, ghast,
        // magma cube, zombified piglin...). Gates fire ticking damage and the
        // dangerous-block spawn test.
        virtual bool FireImmune() const { return false; }

        // MC Entity.causeFallDamage — the landing consequence. Base entities
        // take none; LivingEntity applies the fall-damage formula. Returns
        // whether damage was dealt.
        virtual bool CauseFallDamage(double fallDist, float damageMultiplier) {
            (void)fallDist; (void)damageMultiplier;
            return false;
        }

        // MC Entity.needsSync — "this entity's motion changed in a way the
        // periodic cadence would miss, send it now". Set by knockback, jumps
        // and explosions; consumed and cleared by the entity tracker.
        //
        // MUTABLE because clearing them is what CONSUMING them means, and the
        // consumer — ServerEntityTracker — holds mobs by const reference on
        // purpose: it is a read-only view of game state that happens to own
        // this one piece of sync bookkeeping. MC expresses the same split by
        // putting the flags on Entity and clearing them from ServerEntity
        // (sendChanges:219 and :223-226), which is not the entity either.
        //
        // They are one-tick flags. If they are ever left set, the entity sends
        // a velocity packet EVERY tick forever and takes the movement branch
        // with it — a few hundred blast-pushed TNT then overrun the client's
        // incoming queue and take the chunk packets down with them.
        mutable bool needsSync = false;
        // See HoldsEntityRefs(). Defaults TRUE — opt out, never opt in.
        bool m_holdsEntityRefs = true;
        // MC Entity.hurtMarked — a damage push that must reach the client as a
        // velocity packet even though the server is authoritative.
        mutable bool hurtMarked = false;

        // ── Lifecycle ──────────────────────────────────────────────────────
        bool IsRemoved() const { return m_removal != RemovalReason::None; }

        // MC Entity.Invulnerable — a base NBT key with no field here until now.
        bool IsInvulnerable() const { return m_invulnerable; }
        void SetInvulnerable(bool v) { m_invulnerable = v; }
        RemovalReason GetRemovalReason() const { return m_removal; }
        // MC Entity.remove(reason): a removed vehicle ejects its riders and a
        // removed rider leaves its seat BEFORE the flag is set, so no live
        // entity ever holds a riding pointer to a removed one. Out of line
        // because the eject walks the passenger list.
        void Remove(RemovalReason reason);
        // MC Entity.discard — removal with no death handling.
        void Discard() { Remove(RemovalReason::Discarded); }

        virtual bool IsAlive() const { return !IsRemoved(); }

        // MC Entity.handleEntityEvent — a one-byte broadcast from the server
        // that drives a purely CLIENT-SIDE effect. The sheep's grazing
        // animation is one: the server never sends the counter, it sends event
        // 10 once and the client runs the 40 ticks itself.
        virtual void HandleEntityEvent(uint8_t id) { (void)id; }

        // ── Pose (MC DATA_POSE) ────────────────────────────────────────────
        Pose GetPose() const { return m_pose; }

        // Setting the pose on the CLIENT is what MC's onSyncedDataUpdated does
        // when DATA_POSE arrives, so the notify hook fires from here. The
        // server calls the same setter; its own OnPoseUpdated is a no-op
        // because the animation timers it would start are client-only.
        void SetPose(Pose pose) {
            if (m_pose == pose) return;
            m_pose = pose;
            OnPoseUpdated();
        }

        // MC's `if (DATA_POSE.equals(accessor))` branch, as a virtual.
        virtual void OnPoseUpdated() {}

        // ── Dimensions ─────────────────────────────────────────────────────
        // The entity's size, 1 = its type's own. A scaled immersive portal
        // multiplies it on the way through, and /scale sets it; the box,
        // the eye and the renderer all follow it. Types override the BASE
        // box (a baby, a slime's size, a bear standing up); the size is
        // applied on top of whatever they answer.
        float scale = 1.0f;

        virtual float BaseBbWidth()   const { return TypeInfo().width; }
        virtual float BaseBbHeight()  const { return TypeInfo().height; }
        virtual float BaseEyeHeight() const { return TypeInfo().eyeHeight; }
        float GetBbWidth()   const { return BaseBbWidth()   * scale; }
        float GetBbHeight()  const { return BaseBbHeight()  * scale; }
        float GetEyeHeight() const { return BaseEyeHeight() * scale; }

        double GetEyeY() const { return position.y + GetEyeHeight(); }
        glm::dvec3 GetEyePosition() const {
            return glm::dvec3(position.x, GetEyeY(), position.z);
        }

        glm::vec3 HalfExtents() const {
            return glm::vec3(GetBbWidth() * 0.5f, GetBbHeight() * 0.5f, GetBbWidth() * 0.5f);
        }

        AABB GetAABB() const {
            const float w = GetBbWidth(), h = GetBbHeight();
            return AABB(glm::vec3(position.x, position.y + h * 0.5f, position.z),
                        glm::vec3(w, h, w));
        }

        // Same box in world doubles. GetAABB() above narrows the entity's
        // double position to float, which is fine for the collision work it
        // feeds but not for reach tests far from the origin — a few hundred
        // thousand blocks out, float spacing exceeds the reach distance itself.
        AABBd GetAABBd() const {
            const double w = GetBbWidth(), h = GetBbHeight();
            return AABBd{
                glm::dvec3(position.x - w * 0.5, position.y,     position.z - w * 0.5),
                glm::dvec3(position.x + w * 0.5, position.y + h, position.z + w * 0.5)
            };
        }

        glm::ivec3 BlockPosition() const {
            return glm::ivec3(static_cast<int>(std::floor(position.x)),
                              static_cast<int>(std::floor(position.y)),
                              static_cast<int>(std::floor(position.z)));
        }

        // ── Distances ──────────────────────────────────────────────────────
        double DistanceToSqr(const Entity& other) const {
            return DistanceToSqr(other.position.x, other.position.y, other.position.z);
        }
        double DistanceToSqr(double x, double y, double z) const {
            const double dx = position.x - x, dy = position.y - y, dz = position.z - z;
            return dx * dx + dy * dy + dz * dz;
        }
        double DistanceTo(const Entity& other) const {
            return std::sqrt(DistanceToSqr(other));
        }

        // ── Movement primitives ────────────────────────────────────────────

        // MC Entity.setOldPosAndRot — called by the level immediately BEFORE
        // tick(), never inside it. Ordering matters: the walk animation and
        // BodyRotationControl both measure this tick's displacement against
        // these values, so capturing them at the wrong point makes a standing
        // mob look like it is walking.
        void SetOldPosAndRot();

        // MC Entity.move(MoverType.SELF, delta): collide, slide, step up, then
        // update the collision flags and zero blocked velocity axes.
        void Move(const glm::dvec3& delta);

        // ── The cheap client-side mover ────────────────────────────────────
        //
        // Ballistic integration plus at most two box-overlap tests, for
        // entities whose client-side position is COSMETIC — primed TNT and
        // falling blocks, whose authoritative position is the server's and
        // arrives every few ticks through ServerEntityTracker. The client
        // simulates them only to fill the gap between those packets, and
        // Entity::Move's swept-AABB resolve is far more machinery than filling
        // two ticks of motion needs: it gathers colliders over the whole swept
        // region expanded a block up and a block down (~16-27 cells) and
        // resolves three axes in order, where this does one overlap test in the
        // common airborne case.
        //
        // The divergence is bounded and self-correcting: an entity can end a
        // tick up to one tick of travel (~0.1 block for TNT) inside geometry it
        // would have been stopped short of, and the next position packet
        // erases it. What it will NOT do is fall through the world — the
        // collide-then-revert below keeps it out of solid blocks.
        //
        // Server-side callers must keep using Move(): that position IS the
        // truth, and nothing corrects it.
        void MoveApproximate(const glm::dvec3& delta);

        // MC Entity.moveRelative — turn a local (strafe, up, forward) input
        // into world-space velocity, scaled by `speed` and rotated by yRot.
        void MoveRelative(float speed, const glm::dvec3& input);

        // MC Entity.getInputVector. Normalises only when the input exceeds
        // unit length, so a half-pressed input stays half speed.
        static glm::dvec3 GetInputVector(const glm::dvec3& input, float speed, float yRot);

        // MC Entity.applyGravity.
        void ApplyGravity();
        virtual double GetGravity() const { return 0.0; }

        // MC Entity.addDeltaMovement — a velocity push that must reach the
        // client (explosion knockback, the wind-charge burst). Virtual
        // because the server's player view is client-authoritative for
        // movement: it overrides this to queue the push as a velocity packet
        // instead of writing a field the next move packet would overwrite.
        virtual void AddDeltaMovement(const glm::dvec3& v) {
            velocity += v;
            needsSync = true;
            hurtMarked = true;
            physicsParked = false;   // an impulse ends a parked rest
        }

        // MC Entity.noGravity — set by FlyingMoveControl while steering (and
        // cleared when it stops, unless the mob hovers in place).
        bool IsNoGravity() const { return m_noGravity; }
        void SetNoGravity(bool v) { m_noGravity = v; }

        // MC Entity.maxUpStep. 0 on the base — only LivingEntity has a step
        // height, from the STEP_HEIGHT attribute.
        virtual float MaxUpStep() const { return 0.0f; }

        // ── Riding (MC Entity passengers/vehicle) ──────────────────────────
        //
        // The vehicle/passenger graph is raw pointers, like every other
        // entity-to-entity reference in this port (target, breeding partner).
        // The lifetime rule that makes that safe is enforced in three places:
        //   * Entity::Remove ejects passengers / leaves the seat first, so a
        //     Remove()'d entity is never referenced through a riding pointer;
        //   * MobManager::RemoveInChunk / ClientMobManager::Remove unlink
        //     before their direct erases (paths that never call Remove());
        //   * Mob::ClearReferenceTo carries a backup unlink for anything else.
        //
        // TO CREATE A JOCKEY (next wave): construct both mobs, then
        //   rider->StartRiding(*vehicle, /*force=*/true);
        // Order relative to AddFreshEntity does not matter — the link is pure
        // pointer state and the tracker/client resolve ids lazily — but both
        // entities must be positioned first, since PositionRider runs off the
        // vehicle's live position on the next tick.

        Entity*  GetVehicle() const { return m_vehicle; }
        bool     IsPassenger() const { return m_vehicle != nullptr; }
        bool     IsVehicle() const { return !m_passengers.empty(); }
        const std::vector<Entity*>& GetPassengers() const { return m_passengers; }
        Entity*  GetFirstPassenger() const {
            return m_passengers.empty() ? nullptr : m_passengers.front();
        }
        bool     HasPassenger(const Entity& e) const;

        // MC Entity.startRiding(entity, force). Returns false when refused —
        // already riding this vehicle, cycle, seat taken, boarding cooldown.
        // `force` skips CanRide/CanAddPassenger exactly as in MC (jockey
        // spawns use force, so a "seat rules" override can never break them).
        bool StartRiding(Entity& vehicle, bool force = false);
        // MC Entity.stopRiding / removeVehicle. Dismount KEEPS the rider's
        // current (seated) position — MC computes a DismountHelper landing
        // spot; this port lets the rider fall/collide from the seat instead.
        // Simplification, revisit if riders dismount into walls.
        virtual void StopRiding() { RemoveVehicle(); }
        void RemoveVehicle();
        // MC Entity.ejectPassengers — everyone off, last passenger first.
        void EjectPassengers();

        // MC Entity.rideTick: the VEHICLE's tick drives this from
        // MobManager/ClientMobManager (mirroring ServerLevel.tickPassenger) —
        // zero the rider's own motion, run its normal tick (AI, animations),
        // then let the vehicle place it. The rider's own Move() is a no-op
        // while riding — see the gate at the top of Entity::Move.
        void RideTick();

        // MC Entity.positionRider: seat position = vehicle position
        // + passenger attachment (vehicle's, rotated by vehicle yaw)
        // - vehicle attachment (rider's own mount point, MC ridingOffset).
        void PositionRider(Entity& passenger);
        glm::dvec3 GetPassengerRidingPosition(const Entity& passenger) const;

        // Attachment points. MC keeps these on EntityType (EntityAttachments,
        // built by .passengerAttachments()/.ridingOffset() in EntityType.java);
        // our generated EntityTypeInfo has no attachment columns, so the same
        // numbers live in a table in Entity.cpp keyed by EntityTypeId, with
        // MC's fallbacks (PASSENGER at bbHeight, VEHICLE at feet). Virtual so
        // a mob with a DYNAMIC seat (MC Strider bobs it with the walk
        // animation, Camel lowers it while sitting) can override later.
        virtual glm::dvec3 GetPassengerAttachmentPoint(const Entity& passenger) const;
        virtual glm::dvec3 GetVehicleAttachmentPoint() const;

        // MC Entity.getControllingPassenger — null on the base. Player mount
        // control (travelRidden, steering) is a later wave; until then no
        // passenger controls anything.
        virtual Entity* GetControllingPassenger() const { return nullptr; }

        // MC Entity.canRide: `!isShiftKeyDown() && boardingCooldown <= 0`.
        // The shift-key half is player-only, so mobs keep just the cooldown.
        virtual bool CanRide(const Entity& vehicle) const {
            (void)vehicle;
            return m_boardingCooldown <= 0;
        }
        // MC Entity.canAddPassenger — one seat by default. Multi-seat mobs
        // (camel) override.
        virtual bool CanAddPassenger(const Entity& passenger) const {
            (void)passenger;
            return m_passengers.empty();
        }
        // MC Entity.couldAcceptPassenger — boats refuse while shift-dismounting;
        // nothing here does yet.
        virtual bool CouldAcceptPassenger() const { return true; }

        // Backup unlink for direct-erase paths that bypass Remove(); see the
        // lifetime note above. Safe to call redundantly.
        void UnlinkRidingReferenceTo(const Entity* dead);

        // ── Portals (MC Entity.portalProcess / portalCooldown) ─────────────
        //
        // Public because the block callback writes it and the server reads it;
        // MC's fields are package-visible for the same reason.
        //
        // NOTE the two holders. `Server::ServerPlayer` is not an Entity in
        // this engine, but every connected player has a `PlayerEntityView`
        // (a LivingEntity) whose position is synced from it once per tick, so
        // players travel through THIS state like everything else and the
        // server maps the view back to the player when it fires. Dropped
        // items and XP orbs are plain structs rather than Entities and
        // therefore do not travel — a divergence from vanilla, called out
        // here because it is invisible from the call sites.
        PortalState portal;

        // MC Entity.setPortalCooldown() (Entity.java:607) — arms the
        // type-specific delay, which is what getDimensionChangingDelay names.
        void SetPortalCooldown() { portal.SetCooldown(GetDimensionChangingDelay()); }

        // MC Entity.getDimensionChangingDelay (Entity.java:2628). 300 ticks
        // for a generic entity; Player overrides to 10, Projectile to 2. A
        // vehicle carrying a player inherits the player's — not modelled here
        // because nothing rideable crosses a portal yet.
        virtual int GetDimensionChangingDelay() const {
            return Portals::kDefaultPortalCooldown;
        }

        // MC Entity.canUsePortal (Entity.java:3173). LivingEntity adds
        // `&& !isSleeping()`; this engine has no sleeping.
        //
        // The passenger clause is not a detail: MC teleports the VEHICLE and
        // brings its riders, so a rider that could use a portal on its own
        // would try to travel independently and leave its seat behind.
        virtual bool CanUsePortal(bool ignorePassenger) const {
            return (ignorePassenger || !IsPassenger()) && IsAlive();
        }

        // MC Entity.checkInsideBlocks (Entity.java:1240) — dispatch each
        // overlapped block's `entityInside`.
        //
        // MC drives this from `Entity.move`, once per movement step. Here it
        // is per TICK, from the caller that owns the entity, for two reasons:
        // a `PlayerEntityView` never moves under its own power (the client is
        // authoritative and the view is synced from the player each tick), and
        // MC's own `move` early-returns for passengers, so the per-move hook
        // would silently skip both. Per-tick covers every holder uniformly.
        //
        // The cost of the difference is that an entity moving faster than one
        // block per tick can tunnel through a one-block-thick portal. Vanilla
        // portals are at least three blocks tall and players cap out well
        // under that, so nothing reachable hits it.
        void CheckInsideBlocks(ILevelWrite& level);

    private:
        // MC's `visitedBlocks` LongSet — entityInside fires at most once per
        // block per tick, however many sweep steps pass through it. A member
        // rather than a local so the sweep allocates nothing per tick; it is
        // cleared at the top of every call and means nothing between them.
        std::vector<glm::ivec3> m_insideBlocksScratch;

    public:

        // ── Tick ───────────────────────────────────────────────────────────
        virtual void Tick();
        virtual void BaseTick();

        // ── State queries used across the port ─────────────────────────────
        virtual bool IsInWater() const;
        virtual bool IsInLava()  const { return false; }
        bool IsInLiquid() const { return IsInWater() || IsInLava(); }

        // MC Entity.isEyeInFluid(FluidTags.WATER) — the block at eye level is
        // water. Distinct from IsInWater on purpose: a mob wading chest-deep
        // breathes fine; drowning and zombie conversion key on the EYES.
        bool IsEyeInWater() const;

        // ── Air supply (MC Entity.TOTAL_AIR_SUPPLY / DATA_AIR_SUPPLY_ID) ───
        //
        // MC syncs this so the client can draw the bubble bar; no mob HUD
        // exists here, so it is plain server-side state. 300 ticks = 15 s;
        // the drowning consumer lives in LivingEntity::BaseTick.
        static constexpr int kTotalAirSupply = 300;
        virtual int GetMaxAirSupply() const { return kTotalAirSupply; }
        int  GetAirSupply() const { return m_airSupply; }
        void SetAirSupply(int supply) { m_airSupply = supply; }

        // MC Entity.isBaby — false on the base, overridden by AgeableMob.
        virtual bool IsBaby() const { return false; }

        virtual bool IsSpectator() const { return false; }
        virtual bool IsCreative()  const { return false; }
        // MC Player.getAbilities().flying. Only a player can be flying in the
        // sense that matters (creative/spectator flight); everything else
        // answers false. Read by the explosion's player-knockback gate.
        // MC has NO Entity.isFlying(). Player flight is
        // Player.getAbilities().flying, while Parrot.isFlying (Parrot.java:362)
        // and Bee.isFlying (Bee.java:612) are unrelated per-class "airborne"
        // predicates. Naming ours IsFlying() made Parrot::IsFlying silently
        // override it with the WRONG meaning — harmless only because the one
        // caller short-circuits on IsPlayer() first. Named for the ability so
        // the two concepts cannot collide again.
        virtual bool IsAbilityFlying() const { return false; }

        // MC Entity.updateInWaterStateAndDoFluidPushing — the water-state
        // refresh plus the current push, split out of baseTick because MC's
        // own PrimedTnt calls it directly and does NOT call baseTick.
        //
        // The state half is exact. The PUSH half is not implemented: it needs
        // FluidState.getFlow, i.e. per-cell fluid heights and a flow gradient,
        // and this engine models fluids as plain blocks with no level or
        // direction. The seam is here (and named) so the day a fluid model
        // lands, water currents start carrying entities from one place.
        void UpdateInWaterStateAndDoFluidPushing();

        // True only for the player adapters the level bridge hands out.
        // Goals that MC writes as `Player.class` filters test this instead —
        // the port has a closed entity set, so a type predicate is enough and
        // avoids a dynamic_cast in the targeting hot path.
        virtual bool IsPlayer() const { return false; }

        // Downcast to LivingEntity without RTTI. Exact, and cheaper than the
        // dynamic_cast it replaces by a factor of ~25 on the explosion victim
        // loop, which ran it ten million times in one profile.
        //
        // Why a virtual is equivalent to the dynamic_cast here: LivingEntity is
        // Entity's only direct subclass, every derivation in the tree is public
        // and non-virtual, and the multiply-inheriting mobs (Drowned, Skeleton,
        // Enderman, IronGolem, SnowGolem, Witch, Wither, Wolf, Parrot,
        // PolarBear, Cat) all mix in RangedAttackMob / NeutralMob /
        // TamableAnimal, none of which derive from anything. So there is
        // exactly one unambiguous LivingEntity subobject in every chain, Entity
        // is the primary base throughout, and this returns the identical
        // pointer value a dynamic_cast would.
        virtual LivingEntity* AsLiving() { return nullptr; }

        // Has this entity EVER acquired a raw pointer to another entity — an
        // attack target, a hurt-by, a brain memory, a goal's cached victim, a
        // vehicle or a passenger?
        //
        // MobManager's pre-sweep announces each dying mob to every survivor so
        // nothing is left pointing at freed memory. That pass is O(dying x
        // surviving), which its own comment justifies with "dying is normally
        // zero and rarely more than a handful" — a premise a mass detonation
        // destroys. This flag lets the pass skip entities for which
        // ClearReferenceTo is a provable no-op, which is every primed TNT and
        // falling block in the world.
        //
        // DEFAULTS TO TRUE, and that direction is the whole safety argument: a
        // false negative is a use-after-free, a false positive costs only the
        // call that would have happened anyway. So every entity pays the pass
        // unless a type OPTS OUT by calling ClearHoldsEntityRefs in its own
        // constructor, and opting out is only legitimate for a type that can be
        // shown to hold nothing ClearReferenceTo touches.
        //
        // Two types opt out today — PrimedTnt and FallingBlockEntity. Both are
        // Mob-shaped with the mob machinery inert: no brain, no registered
        // goals, no ClearReferenceTo override of their own. Everything else
        // Mob::ClearReferenceTo reads can only be acquired through a marked
        // chokepoint (SetTarget, SetLastHurtByMob, SetLastHurtMob,
        // Brain::SetMemory, StartRiding, or ticking AI), each of which re-arms
        // the flag — so even those two become safe the instant they acquire
        // anything.
        //
        // This deliberately does NOT try to enumerate every setter. Eight
        // subclasses override ClearReferenceTo to clear extra members, and an
        // enumeration that misses one is silent memory corruption; a
        // default-true flag with an explicit opt-out cannot fail that way.
        bool HoldsEntityRefs() const { return m_holdsEntityRefs; }
        void MarkHoldsEntityRefs()   { m_holdsEntityRefs = true; }
        // Only from a constructor, and only with the argument above.
        void ClearHoldsEntityRefs()  { m_holdsEntityRefs = false; }
        const LivingEntity* AsLiving() const {
            return const_cast<Entity*>(this)->AsLiving();
        }

        // MC Entity.ignoreExplosion — false for everything except a warden
        // mid-dig or mid-emerge, which is untouchable while the animation
        // plays. Neither damage nor knockback reaches an entity that says yes.
        virtual bool IgnoreExplosion() const { return false; }
        // True for an entity whose Hurt is a documented no-op against a blast,
        // so an explosion's only effect on it is the push. The batched apply
        // (Game::ExplodeApplyBatch) accumulates those in parallel; anything
        // else keeps the serial, in-order damage path.
        virtual bool ExplosionPushOnly() const { return false; }

        // MC Entity.isPushable — false by default; LivingEntity turns it on.
        // Read by LivingEntity::PushEntities (crowd shoving).
        virtual bool IsPushable() const { return false; }

        // MC Entity.isPickable — may the player's crosshair land on this?
        //
        // The engine had no equivalent: the client's PickEntity considered
        // EVERY mob, so anything standing between you and a block stole the
        // click. Defaulting to true keeps that behaviour for everything that
        // had it.
        //
        // DELIBERATE DIVERGENCE on the two block-shaped entities, requested:
        // vanilla's PrimedTnt and FallingBlockEntity both override isPickable
        // to `!isRemoved()`, i.e. TRUE, so in MC a lit TNT does block clicks
        // past it. Here they return false, so you can place a block through a
        // primed TNT or light the next one in a row behind it. See their
        // overrides.
        virtual bool IsPickable() const { return true; }

        // Client-visible flag bits (MC DATA_SHARED_FLAGS_ID).
        bool IsSprinting() const { return m_sprinting; }
        void SetSprinting(bool v) { m_sprinting = v; }
        // Virtual because MC's is: Blaze answers with its charged flag (the
        // wire's on-fire bit and the render flames key on this).
        virtual bool IsOnFire() const { return m_remainingFireTicks > 0; }
        void SetRemainingFireTicks(int t) { m_remainingFireTicks = t; }
        int  GetRemainingFireTicks() const { return m_remainingFireTicks; }
        void IgniteForSeconds(int seconds);

    protected:
        // MC Entity.checkFallDamage, called from Move with the ACTUAL vertical
        // displacement: falling accumulates, landing settles via
        // CauseFallDamage and resets.
        virtual void CheckFallDamage(double dy, bool onGroundNow);

        // MC Entity.propagateFallToPassengers. In MC this is called from
        // causeFallDamage (Entity's AND LivingEntity's override); here it is
        // called from CheckFallDamage right beside the CauseFallDamage call,
        // because LivingEntity::CauseFallDamage predates riding and does not
        // chain to the base. Same observable behaviour: the vehicle lands,
        // every rider takes the fall with the vehicle's fallDistance.
        void PropagateFallToPassengers(double fallDist, float damageMultiplier);

        EntityTypeId  m_type;
        EntityLevel*  m_level = nullptr;
        int32_t       m_id = 0;
        // See GetUuid(). Nil until a manager mints it.
        Uuid          m_uuid{};
        bool          m_invulnerable = false;
        RemovalReason m_removal = RemovalReason::None;
        Pose          m_pose    = Pose::Standing;

        bool m_sprinting = false;
        bool m_noGravity = false;
        int  m_remainingFireTicks = 0;
        // Starts full (MC defines DATA_AIR_SUPPLY_ID with getMaxAirSupply()).
        // A type with a larger maximum re-fills it in its own constructor,
        // exactly as MC's Dolphin constructor does.
        int  m_airSupply = kTotalAirSupply;

        // ── Riding state ───────────────────────────────────────────────────
        // MC Entity.passengers / Entity.vehicle. Invariant (MC's, enforced by
        // StartRiding/RemoveVehicle being the ONLY writers): p is in
        // v.m_passengers  <=>  p.m_vehicle == &v.
        std::vector<Entity*> m_passengers;
        Entity*              m_vehicle = nullptr;
        // MC Entity.boardingCooldown — set to 60 on dismount, decremented in
        // BaseTick, gates CanRide so a mob cannot re-mount the same tick.
        int m_boardingCooldown = 0;
    };


    // MC ClientPacketListener.handleExplosion + ClientExplosionTracker — the
    // client-side visual for a blast that already happened on the server: one
    // EXPLOSION / EXPLOSION_EMITTER particle at the centre, plus the debris
    // cloud (cbrt-distributed radius, air cells only, poof/smoke 50/50).
    //
    // A FREE function rather than an Entity member, which is where it used to
    // live. The blast now arrives as ExplodeS2C, and the packet handler has a
    // level but no entity to hang it off — the TNT that produced it was
    // discarded a tick before the packet went out.
    //
    // `blockCount` is now the REAL number of blocks destroyed rather than the
    // radius-cubed guess the old entity-event path had to make.
    void SpawnExplosionVisualEffects(EntityLevel& level,
                                     double cx, double cy, double cz,
                                     float radius, bool small, int blockCount);

} // namespace Game
