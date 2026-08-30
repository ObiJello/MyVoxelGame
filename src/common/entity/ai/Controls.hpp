// File: src/common/entity/ai/Controls.hpp
//
// MC net.minecraft.world.entity.ai.control — the four per-tick controllers a
// Mob runs after its goals have decided what it wants.
//
// The division of labour is worth stating because it is not obvious from the
// names: goals never touch velocity. A goal calls navigation.MoveTo(...), the
// navigation walks the path and calls MoveControl::SetWantedPosition once per
// tick, and only MoveControl converts that into a yaw and a `zza` forward
// input. LivingEntity::Travel then turns those into motion. Anything that
// writes velocity directly from a goal is a bug.
#pragma once

#include <glm/glm.hpp>

namespace Game {

    class Mob;
    class Guardian;
    class Rabbit;

    // MC MoveControl. Drives yaw and forward speed toward a wanted position,
    // and requests a jump when the way is blocked.
    class MoveControl {
    public:
        explicit MoveControl(Mob* mob) : m_mob(mob) {}
        virtual ~MoveControl() = default;

        // Virtual because MC's is (Java): RabbitMoveControl overrides it to
        // remember the speed for the NEXT hop, and the navigation calls it
        // through the base pointer every tick.
        virtual void SetWantedPosition(double x, double y, double z, double speedModifier);
        void Strafe(float forward, float right);
        void SetWait() { m_operation = Operation::Wait; }

        bool   HasWanted() const { return m_operation == Operation::MoveTo; }
        double GetSpeedModifier() const { return m_speedModifier; }
        double GetWantedX() const { return m_wantedX; }
        double GetWantedY() const { return m_wantedY; }
        double GetWantedZ() const { return m_wantedZ; }

        virtual void Tick();

        // MC MoveControl.MIN_SPEED_SQR — below this the mob is close enough to
        // its waypoint that steering toward it would just jitter.
        static constexpr double kMinSpeedSq = 2.5000003e-7;
        static constexpr float  kMaxTurn    = 90.0f;

    protected:
        enum class Operation { Wait, MoveTo, Strafe, Jumping };

        // MC MoveControl.rotlerp — step `from` toward `to` by at most `max`
        // degrees, taking the short way round.
        static float RotLerp(float from, float to, float max);

        // MC MoveControl.isWalkable — is the block one strafe-step away a
        // WALKABLE path node? The STRAFE branch queries this before applying
        // the input, and a failed answer converts the strafe into a plain
        // advance so a circling skeleton never sidesteps off a cliff.
        bool IsWalkable(float dx, float dz) const;

        Mob*      m_mob;
        double    m_wantedX = 0.0, m_wantedY = 0.0, m_wantedZ = 0.0;
        double    m_speedModifier = 0.0;
        float     m_strafeForwards = 0.0f, m_strafeRight = 0.0f;
        Operation m_operation = Operation::Wait;
    };

    // MC FlyingMoveControl — steers in three dimensions. While it has a
    // wanted position it turns gravity OFF and drives yya toward the target's
    // height; when it goes idle it restores gravity unless the mob hovers in
    // place (bees hover, parrots settle).
    class FlyingMoveControl : public MoveControl {
    public:
        FlyingMoveControl(Mob* mob, int maxTurn, bool hoversInPlace)
            : MoveControl(mob), m_maxTurn(maxTurn), m_hoversInPlace(hoversInPlace) {}

        void Tick() override;

    private:
        int  m_maxTurn;
        bool m_hoversInPlace;
    };

    // MC Guardian.GuardianMoveControl — the guardian's hover-swim. While a
    // path is live it turns the whole body to the waypoint, lerps its speed
    // up at 0.125/tick, and adds a per-tick sinusoidal push (phased by
    // tickCount + id so a shoal does not bob in unison) plus a 0.1-scaled
    // vertical pull toward the waypoint; the look target is dragged along at
    // an eighth per tick so the head sweeps rather than snaps. It also owns
    // the guardian's synched `moving` flag, which is what the client's tail
    // animation speed keys on. MC nests it inside Guardian; it lives here
    // with the other controls, per this port's layout.
    class GuardianMoveControl : public MoveControl {
    public:
        explicit GuardianMoveControl(Guardian* guardian);

        void Tick() override;

    private:
        Guardian* m_guardian;
    };

    // MC Ghast.GhastMoveControl (careful = false, the plain ghast's) — no
    // navigation at all: every 2-6 ticks it adds one velocity impulse of
    // FLYING_SPEED * 5/3 toward the wanted position, after checking the
    // flight corridor is clear, and goes idle when it is not. The drift
    // between impulses is what gives ghasts their floaty wander.
    class GhastMoveControl : public MoveControl {
    public:
        explicit GhastMoveControl(Mob* ghast) : MoveControl(ghast) {}

        void Tick() override;

    private:
        // MC canReach: would the ghast's box collide anywhere along `travel`?
        // MC walks the blocks the swept box intersects; this samples the box
        // at one-block steps along the segment — same collision table, same
        // answer for anything wider than a pixel gap.
        bool CanReach(const glm::dvec3& travel) const;

        int m_floatDuration = 0;
    };

    // MC AbstractFish.FishMoveControl — the fish's own steering: speed lerps
    // toward the target at 0.125/tick, vertical motion is a direct velocity
    // nudge proportional to how far above or below the waypoint sits, and a
    // +0.005 buoyancy tick keeps the fish off the sea floor.
    class FishMoveControl : public MoveControl {
    public:
        explicit FishMoveControl(Mob* mob) : MoveControl(mob) {}
        void Tick() override;
    };

    // MC SmoothSwimmingMoveControl — pitch-based swimming: the mob banks its
    // whole body toward the waypoint and moves along its view vector, with
    // zza/yya derived from the pitch. Out of water it flops along the ground
    // at outsideWaterSpeedModifier, throttled by how far it still has to turn.
    class SmoothSwimmingMoveControl : public MoveControl {
    public:
        static constexpr float kFullSpeedTurnThreshold = 10.0f;
        static constexpr float kStopTurnThreshold      = 60.0f;

        SmoothSwimmingMoveControl(Mob* mob, int maxTurnX, int maxTurnY,
                                  float inWaterSpeedModifier,
                                  float outsideWaterSpeedModifier, bool applyGravity)
            : MoveControl(mob), m_maxTurnX(maxTurnX), m_maxTurnY(maxTurnY),
              m_inWaterSpeedModifier(inWaterSpeedModifier),
              m_outsideWaterSpeedModifier(outsideWaterSpeedModifier),
              m_applyGravity(applyGravity) {}

        void Tick() override;

    private:
        static float GetTurningSpeedFactor(float leftToTurn);

        int   m_maxTurnX;
        int   m_maxTurnY;
        float m_inWaterSpeedModifier;
        float m_outsideWaterSpeedModifier;
        bool  m_applyGravity;
    };

    // MC LookControl. Turns the HEAD (yHeadRot) and pitch, never the body.
    class LookControl {
    public:
        explicit LookControl(Mob* mob) : m_mob(mob) {}
        virtual ~LookControl() = default;

        void SetLookAt(const glm::dvec3& target);
        void SetLookAt(double x, double y, double z);
        void SetLookAt(double x, double y, double z, float yMaxRotSpeed, float xMaxRotAngle);

        bool IsLookingAtTarget() const { return m_lookAtCooldown > 0; }

        // MC LookControl.getWantedX/Y/Z — read by GuardianMoveControl to drag
        // the look target toward the travel direction instead of snapping it.
        double GetWantedX() const { return m_wantedX; }
        double GetWantedY() const { return m_wantedY; }
        double GetWantedZ() const { return m_wantedZ; }

        virtual void Tick();

    protected:
        // MC resets pitch every tick unless the mob overrides it, which is why
        // an idle mob always ends up looking level.
        virtual bool ResetXRotOnTick() const { return true; }

        void ClampHeadRotationToBody();
        bool GetYRotD(float& out) const;
        bool GetXRotD(float& out) const;

        Mob*  m_mob;
        float m_yMaxRotSpeed = 0.0f;
        float m_xMaxRotAngle = 0.0f;
        int   m_lookAtCooldown = 0;
        double m_wantedX = 0.0, m_wantedY = 0.0, m_wantedZ = 0.0;
    };

    // MC SmoothSwimmingLookControl — the swimming head: while looking at a
    // target the head leads by a fixed 20-degree yaw / 10-degree pitch tilt;
    // idle, pitch levels out at 5 degrees per tick and the body is dragged
    // 4 degrees per tick once the head strays past maxYRotFromCenter.
    class SmoothSwimmingLookControl : public LookControl {
    public:
        SmoothSwimmingLookControl(Mob* mob, int maxYRotFromCenter)
            : LookControl(mob), m_maxYRotFromCenter(maxYRotFromCenter) {}

        void Tick() override;

    private:
        int m_maxYRotFromCenter;
    };

    // MC JumpControl. A one-shot latch: goals and MoveControl call Jump(), and
    // the flag survives exactly one tick so LivingEntity::AiStep can consume it.
    // Tick is virtual because MC's is: RabbitJumpControl replaces the latch
    // hand-off with a StartJumping call.
    class JumpControl {
    public:
        explicit JumpControl(Mob* mob) : m_mob(mob) {}
        virtual ~JumpControl() = default;
        void Jump() { m_jump = true; }
        virtual void Tick();

    protected:
        Mob* m_mob;
        bool m_jump = false;
    };

    // MC Rabbit.RabbitJumpControl — the rabbit's jump latch with an extra
    // `canJump` gate the rabbit's landing-delay machinery toggles. Its Tick
    // routes through Rabbit::StartJumping (which arms the client animation
    // clock) instead of writing `jumping` directly. MC nests it inside
    // Rabbit.java; it lives here with the other controls, per this port's
    // layout.
    class RabbitJumpControl : public JumpControl {
    public:
        explicit RabbitJumpControl(Rabbit* rabbit);

        // MC wantJump — is the one-shot latch armed?
        bool WantJump() const { return m_jump; }
        bool CanJump() const { return m_canJump; }
        void SetCanJump(bool canJump) { m_canJump = canJump; }

        void Tick() override;

    private:
        Rabbit* m_rabbit;
        bool    m_canJump = false;
    };

    // MC Rabbit.RabbitMoveControl — remembers the speed each SetWantedPosition
    // asked for (`nextJumpSpeed`) and re-applies it on the tick the rabbit is
    // airborne or mid-plan, while a grounded, idle rabbit is parked at speed 0.
    // This is what makes a rabbit move in discrete hops rather than glide.
    class RabbitMoveControl : public MoveControl {
    public:
        explicit RabbitMoveControl(Rabbit* rabbit);

        void Tick() override;
        void SetWantedPosition(double x, double y, double z, double speedModifier) override;

    private:
        Rabbit* m_rabbit;
        double  m_nextJumpSpeed = 0.0;
    };

    // MC BodyRotationControl. Keeps the torso from snapping to the head:
    // while moving the body follows the facing exactly, and while standing
    // still it only catches up after the head has held a new angle for a while.
    class BodyRotationControl {
    public:
        explicit BodyRotationControl(Mob* mob) : m_mob(mob) {}
        virtual ~BodyRotationControl() = default;
        virtual void ClientTick();

        static constexpr float kHeadStableAngle = 15.0f;
        static constexpr int   kDelayUntilFacingForward = 10;
        static constexpr int   kTicksToFaceForward = 10;

    protected:
        bool IsMoving() const;

        Mob*  m_mob;
        float m_lastStableYHeadRot = 0.0f;
        int   m_headStableTime = 0;
    };

} // namespace Game
