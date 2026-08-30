// File: src/common/entity/NeutralMob.hpp
//
// MC net.minecraft.world.entity.NeutralMob — persistent per-target anger.
//
// MC makes this an interface with default methods; C++ has no default
// interface methods, so it is a plain mixin base carrying the two fields and
// the shared logic, constructed with a back-pointer to the Mob half of the
// object (the same object — every implementer derives from both). The mob
// classes that mix it in mirror MC's implements list exactly: ZombifiedPiglin,
// EnderMan, Wolf, Bee, PolarBear, IronGolem. (Goat is NOT neutral in MC —
// its ramming is brain behaviour, not anger — and gets nothing here.)
//
// Time is MC's anger_end_time shape: an absolute game-time tick after which
// the mob stops being angry, -1 while calm (NeutralMob.NO_ANGER_END_TIME).
// The per-mob PERSISTENT_ANGER_TIME provider is TimeUtil.rangeOfSeconds(20,39)
// — a uniform 400..780 ticks — for every one of the six implementers; each
// class still owns its StartPersistentAngerTimer exactly as MC does.
//
// The persistent target is a raw LivingEntity* where MC holds an
// EntityReference<LivingEntity> (a UUID that survives save/load): this engine
// has no mob NBT persistence, so addPersistentAngerSaveData /
// readPersistentAngerSaveData have nothing to write into and are the skipped
// halves. ClearAngerReferenceTo is the pointer-safety half every implementer
// forwards its ClearReferenceTo into.
#pragma once

#include "common/core/EntityRef.hpp"

#include <cstdint>

namespace Game {

    class Mob;
    class LivingEntity;
    class Entity;

    class NeutralMob {
    public:
        // MC NeutralMob.NO_ANGER_END_TIME.
        static constexpr int64_t kNoAngerEndTime = -1;

        virtual ~NeutralMob() = default;

        int64_t GetPersistentAngerEndTime() const { return m_persistentAngerEndTime; }
        void    SetPersistentAngerEndTime(int64_t endTime) {
            m_persistentAngerEndTime = endTime;
        }

        // MC setTimeToRemainAngry: endTime = level gameTime + remaining.
        void SetTimeToRemainAngry(int64_t remainingTicks);

        // Persisted, so the target is an identity rather than a pointer: a
        // wolf must still be angry at a player who logged out mid-fight.
        LivingEntity* GetPersistentAngerTarget();      // non-const: resolves lazily
        void SetPersistentAngerTarget(LivingEntity* target);
        void SetPersistentAngerTargetUuid(const Uuid& uuid) { m_angryAtRef.SetUnresolved(uuid); }
        const EntityRef& AngryAtRef() const { return m_angryAtRef; }

        // Each implementer samples ITS OWN PERSISTENT_ANGER_TIME — all six use
        // rangeOfSeconds(20, 39), i.e. 400 + nextInt(381).
        virtual void StartPersistentAngerTimer() = 0;

        // MC NeutralMob.updatePersistentAnger — called every server tick from
        // wherever each mob calls it (aiStep for wolf/polar bear/iron golem/
        // enderman, customServerAiStep for zombified piglin and bee).
        void UpdatePersistentAnger(bool stayAngryIfTargetPresent);

        // MC isAngryAt(entity, level) — the selector the anger-gated
        // NearestAttackableTargetGoal runs per candidate.
        bool IsAngryAt(const LivingEntity& entity) const;

        // MC isAngryAtAllPlayers: gated on the UNIVERSAL_ANGER game rule,
        // which defaults OFF and has no game-rule system here — treated as
        // permanently false, matching MC's default world.
        bool IsAngryAtAllPlayers() const { return false; }

        // MC isAngry — the anger window is still open.
        bool IsAngry() const;

        // MC forgetCurrentTargetAndRefreshUniversalAnger — what
        // ResetUniversalAngerTargetGoal.start calls.
        void ForgetCurrentTargetAndRefreshUniversalAnger() {
            StopBeingAngry();
            StartPersistentAngerTimer();
        }

        // MC stopBeingAngry.
        void StopBeingAngry();

        // MC playerDied + FORGIVE_DEAD_PLAYERS (default ON) forgives the dead:
        // no player-death notification reaches mobs in this engine, but a dead
        // or disconnected player's entity is torn down, and the implementers'
        // ClearReferenceTo forwards here — which drops the grudge exactly
        // where MC's forgiveness would.
        void ClearAngerReferenceTo(const Entity* entity) {
            // Demote the pointer; keep the grudge unless the target actually
            // died. A player who merely logged out is still someone to be
            // angry at when they come back.
            m_angryAtRef.OnEntityRemoved(entity);
        }

    protected:
        explicit NeutralMob(Mob* self) : m_neutralSelf(self) {}

        // MC NeutralMob's private isValidPlayerTarget: a player who is neither
        // creative nor spectator.
        static bool IsValidPlayerTarget(const LivingEntity& target);

    private:
        Mob*          m_neutralSelf;
        int64_t       m_persistentAngerEndTime = kNoAngerEndTime;
        EntityRef     m_angryAtRef;
    };

} // namespace Game
