// File: src/common/entity/ai/goals/EvokerGoals.hpp
//
// MC SpellcasterIllager.java's nested goal bases, Evoker.java's three spells,
// and Vex.java's nested AI (move control included — Monsters.cpp constructs
// it). MC nests all of these inside their mob classes; they live here because
// every goal class in this port does.
#pragma once

#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/mobs/Monsters.hpp"

namespace Game {

    class Vex;

    // ── SpellcasterIllager's nested bases ──────────────────────────────────

    // MC SpellcasterIllager.SpellcasterCastingSpellGoal — while the cast
    // timer runs, stand still and stare at the target.
    class SpellcasterCastingSpellGoal : public Goal {
    public:
        explicit SpellcasterCastingSpellGoal(SpellcasterIllager* caster);

        bool CanUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "SpellcasterCastingSpellGoal"; }

    protected:
        SpellcasterIllager* m_caster;
    };

    // MC SpellcasterIllager.SpellcasterUseSpellGoal — the shared warmup /
    // casting-time / interval machine every spell derives from.
    class SpellcasterUseSpellGoal : public Goal {
    public:
        explicit SpellcasterUseSpellGoal(SpellcasterIllager* caster)
            : m_caster(caster) {}

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Tick() override;

    protected:
        virtual void PerformSpellCasting() = 0;
        virtual int GetCastWarmupTime() const { return 20; }
        virtual int GetCastingTime() const = 0;
        virtual int GetCastingInterval() const = 0;
        virtual SpellcasterIllager::IllagerSpell GetSpell() const = 0;

        SpellcasterIllager* m_caster;
        int m_attackWarmupDelay = 0;
        int m_nextAttackTickCount = 0;
    };

    // ── Evoker's spells ────────────────────────────────────────────────────

    // MC Evoker.EvokerCastingSpellGoal — the casting stare, extended to also
    // watch the wololo sheep when there is no combat target.
    class EvokerCastingSpellGoal : public SpellcasterCastingSpellGoal {
    public:
        explicit EvokerCastingSpellGoal(Evoker* evoker);

        void Tick() override;
        const char* Name() const override { return "EvokerCastingSpellGoal"; }

    private:
        Evoker* m_evoker;
    };

    // MC Evoker.EvokerAttackSpellGoal — the fang patterns: inside 3 blocks a
    // ring of 5 at 1.5 plus a ring of 8 at 2.5 (delay 3); beyond, a line of
    // 16 fangs marching toward the target, one tick of extra delay each.
    class EvokerAttackSpellGoal : public SpellcasterUseSpellGoal {
    public:
        explicit EvokerAttackSpellGoal(Evoker* evoker)
            : SpellcasterUseSpellGoal(evoker), m_evoker(evoker) {}

        const char* Name() const override { return "EvokerAttackSpellGoal"; }

    protected:
        void PerformSpellCasting() override;
        int GetCastingTime() const override { return 40; }
        int GetCastingInterval() const override { return 100; }
        SpellcasterIllager::IllagerSpell GetSpell() const override {
            return SpellcasterIllager::IllagerSpell::Fangs;
        }

    private:
        void CreateSpellEntity(double x, double z, double minY, double maxY,
                               float angle, int delayTicks);

        Evoker* m_evoker;
    };

    // MC Evoker.EvokerSummonSpellGoal — three vexes, each with the owner
    // link, a bound origin and a 30 + rand(90) second limited life; gated on
    // fewer than rand(8)+1 vexes already within 16 blocks.
    class EvokerSummonSpellGoal : public SpellcasterUseSpellGoal {
    public:
        explicit EvokerSummonSpellGoal(Evoker* evoker)
            : SpellcasterUseSpellGoal(evoker), m_evoker(evoker) {}

        bool CanUse() override;
        const char* Name() const override { return "EvokerSummonSpellGoal"; }

    protected:
        void PerformSpellCasting() override;
        int GetCastingTime() const override { return 100; }
        int GetCastingInterval() const override { return 340; }
        SpellcasterIllager::IllagerSpell GetSpell() const override {
            return SpellcasterIllager::IllagerSpell::SummonVex;
        }

    private:
        Evoker* m_evoker;
    };

    // MC Evoker.EvokerWololoSpellGoal — with no combat target, recolour a
    // nearby BLUE sheep RED. (MC gates it on mobGriefing; no game rules —
    // treated as ON, the vanilla default.)
    class EvokerWololoSpellGoal : public SpellcasterUseSpellGoal {
    public:
        explicit EvokerWololoSpellGoal(Evoker* evoker)
            : SpellcasterUseSpellGoal(evoker), m_evoker(evoker) {}

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Stop() override;
        const char* Name() const override { return "EvokerWololoSpellGoal"; }

    protected:
        void PerformSpellCasting() override;
        int GetCastWarmupTime() const override { return 40; }
        int GetCastingTime() const override { return 60; }
        int GetCastingInterval() const override { return 140; }
        SpellcasterIllager::IllagerSpell GetSpell() const override {
            return SpellcasterIllager::IllagerSpell::Wololo;
        }

    private:
        Evoker* m_evoker;
    };

    // MC NearestAttackableTargetGoal(...).setUnseenMemoryTicks(300) — the
    // evoker's player/villager targets remember an unseen target five times
    // longer than the default 60. TargetGoal keeps the field protected, so
    // the builder call becomes a constructor argument here.
    class NearestAttackableTargetGoalWithMemory : public NearestAttackableTargetGoal {
    public:
        NearestAttackableTargetGoalWithMemory(Mob* mob, bool mustSee,
                                              int unseenMemoryTicks)
            : NearestAttackableTargetGoal(mob, mustSee) {
            m_unseenMemoryTicks = unseenMemoryTicks;
        }
        NearestAttackableTargetGoalWithMemory(Mob* mob, const EntityTypeId* types,
                                              int typeCount, bool mustSee,
                                              int unseenMemoryTicks)
            : NearestAttackableTargetGoal(mob, types, typeCount, mustSee) {
            m_unseenMemoryTicks = unseenMemoryTicks;
        }
        const char* Name() const override {
            return "NearestAttackableTargetGoalWithMemory";
        }
    };

    // ── Vex's nested AI ────────────────────────────────────────────────────

    // MC Vex.VexMoveControl — impulse flight straight at the wanted point:
    // inside the box size it halves the velocity and goes idle; otherwise it
    // adds speed*0.05/dist of the delta per tick and faces the target (or
    // the travel direction).
    class VexMoveControl : public MoveControl {
    public:
        explicit VexMoveControl(Vex* vex);
        void Tick() override;

    private:
        Vex* m_vex;
    };

    // MC Vex.VexChargeAttackGoal — line up on the target's eyes, charge, and
    // bite on box contact.
    class VexChargeAttackGoal : public Goal {
    public:
        explicit VexChargeAttackGoal(Vex* vex);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "VexChargeAttackGoal"; }

    private:
        Vex* m_vex;
    };

    // MC Vex.VexRandomMoveGoal — drift to a random air block within 7 blocks
    // of the bound origin (or the vex itself).
    class VexRandomMoveGoal : public Goal {
    public:
        explicit VexRandomMoveGoal(Vex* vex);

        bool CanUse() override;
        bool CanContinueToUse() override { return false; }
        void Tick() override;
        const char* Name() const override { return "VexRandomMoveGoal"; }

    private:
        Vex* m_vex;
    };

    // MC Vex.VexCopyOwnerTargetGoal — fight whatever the summoning evoker is
    // fighting.
    class VexCopyOwnerTargetGoal : public TargetGoal {
    public:
        explicit VexCopyOwnerTargetGoal(Vex* vex);

        bool CanUse() override;
        void Start() override;
        const char* Name() const override { return "VexCopyOwnerTargetGoal"; }

    private:
        Vex* m_vex;
        TargetingConditions m_copyOwnerTargeting;
    };

} // namespace Game
