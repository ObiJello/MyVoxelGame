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

    // MC MoveControl. Drives yaw and forward speed toward a wanted position,
    // and requests a jump when the way is blocked.
    class MoveControl {
    public:
        explicit MoveControl(Mob* mob) : m_mob(mob) {}
        virtual ~MoveControl() = default;

        void SetWantedPosition(double x, double y, double z, double speedModifier);
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

        Mob*      m_mob;
        double    m_wantedX = 0.0, m_wantedY = 0.0, m_wantedZ = 0.0;
        double    m_speedModifier = 0.0;
        float     m_strafeForwards = 0.0f, m_strafeRight = 0.0f;
        Operation m_operation = Operation::Wait;
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

    // MC JumpControl. A one-shot latch: goals and MoveControl call Jump(), and
    // the flag survives exactly one tick so LivingEntity::AiStep can consume it.
    class JumpControl {
    public:
        explicit JumpControl(Mob* mob) : m_mob(mob) {}
        void Jump() { m_jump = true; }
        void Tick();

    private:
        Mob* m_mob;
        bool m_jump = false;
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
