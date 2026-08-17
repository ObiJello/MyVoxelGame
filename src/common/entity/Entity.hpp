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

#include "common/entity/EntityType.hpp"
#include "common/physics/Physics.hpp"

#include <glm/glm.hpp>
#include <cstdint>

namespace Game {

    struct EntityLevel;

    // ── Entity id space ────────────────────────────────────────────────────
    //
    // Mob ids live at or above this value. The split matters because
    // RemoveEntitiesS2CPacket is shared by every entity kind and the client
    // decides which of its maps an incoming id belongs to purely by range —
    // see the matching note on Game::kItemEntityIdBase in ItemEntity.hpp.
    //
    //   [0, kItemEntityIdBase)          players (= connection ids)
    //   [kItemEntityIdBase, kMobBase)   dropped items
    //   [kMobEntityIdBase, ...)         mobs and projectiles
    constexpr int32_t kMobEntityIdBase = 0x0200'0000;

    inline bool IsMobEntityId(int32_t id) { return id >= kMobEntityIdBase; }

    enum class RemovalReason : uint8_t {
        None = 0,
        Killed,      // died — loot has already been dropped
        Discarded,   // despawned, or removed for a reason the player never sees
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
        void         SetId(int32_t id) { m_id = id; }
        EntityTypeId GetType() const { return m_type; }
        const EntityTypeInfo& TypeInfo() const { return GetEntityTypeInfo(m_type); }
        EntityLevel* Level() const { return m_level; }

        // ── Position and motion ────────────────────────────────────────────
        // `position` is the entity's FEET, matching MoveEntity and MC.
        glm::dvec3 position{0.0};
        glm::dvec3 velocity{0.0};   // blocks per TICK

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

        // MC Entity.needsSync — "this entity's motion changed in a way the
        // periodic cadence would miss, send it now". Set by knockback, jumps
        // and explosions; consumed and cleared by the entity tracker.
        bool needsSync = false;
        // MC Entity.hurtMarked — a damage push that must reach the client as a
        // velocity packet even though the server is authoritative.
        bool hurtMarked = false;

        // ── Lifecycle ──────────────────────────────────────────────────────
        bool IsRemoved() const { return m_removal != RemovalReason::None; }
        RemovalReason GetRemovalReason() const { return m_removal; }
        void Remove(RemovalReason reason) { m_removal = reason; }
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
        virtual float GetBbWidth()  const { return TypeInfo().width; }
        virtual float GetBbHeight() const { return TypeInfo().height; }
        virtual float GetEyeHeight() const { return TypeInfo().eyeHeight; }

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

        // MC Entity.moveRelative — turn a local (strafe, up, forward) input
        // into world-space velocity, scaled by `speed` and rotated by yRot.
        void MoveRelative(float speed, const glm::dvec3& input);

        // MC Entity.getInputVector. Normalises only when the input exceeds
        // unit length, so a half-pressed input stays half speed.
        static glm::dvec3 GetInputVector(const glm::dvec3& input, float speed, float yRot);

        // MC Entity.applyGravity.
        void ApplyGravity();
        virtual double GetGravity() const { return 0.0; }

        // MC Entity.maxUpStep. 0 on the base — only LivingEntity has a step
        // height, from the STEP_HEIGHT attribute.
        virtual float MaxUpStep() const { return 0.0f; }

        // ── Tick ───────────────────────────────────────────────────────────
        virtual void Tick();
        virtual void BaseTick();

        // ── State queries used across the port ─────────────────────────────
        virtual bool IsInWater() const;
        virtual bool IsInLava()  const { return false; }
        bool IsInLiquid() const { return IsInWater() || IsInLava(); }

        // MC Entity.isBaby — false on the base, overridden by AgeableMob.
        virtual bool IsBaby() const { return false; }

        virtual bool IsSpectator() const { return false; }
        virtual bool IsCreative()  const { return false; }

        // True only for the player adapters the level bridge hands out.
        // Goals that MC writes as `Player.class` filters test this instead —
        // the port has a closed entity set, so a type predicate is enough and
        // avoids a dynamic_cast in the targeting hot path.
        virtual bool IsPlayer() const { return false; }

        // Client-visible flag bits (MC DATA_SHARED_FLAGS_ID).
        bool IsSprinting() const { return m_sprinting; }
        void SetSprinting(bool v) { m_sprinting = v; }
        bool IsOnFire() const { return m_remainingFireTicks > 0; }
        void SetRemainingFireTicks(int t) { m_remainingFireTicks = t; }
        int  GetRemainingFireTicks() const { return m_remainingFireTicks; }
        void IgniteForSeconds(int seconds);

    protected:
        EntityTypeId  m_type;
        EntityLevel*  m_level = nullptr;
        int32_t       m_id = 0;
        RemovalReason m_removal = RemovalReason::None;
        Pose          m_pose    = Pose::Standing;

        bool m_sprinting = false;
        int  m_remainingFireTicks = 0;
    };

} // namespace Game
