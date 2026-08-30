// File: src/common/entity/ai/goals/IllusionerGoals.hpp
//
// MC Illusioner.java's two nested spell goals. MC nests them inside the mob
// class; they live here because every goal class in this port does (the
// evoker's spells set the precedent in EvokerGoals.hpp).
#pragma once

#include "common/entity/ai/goals/EvokerGoals.hpp"

namespace Game {

    class Illusioner;

    // MC Illusioner.IllusionerMirrorSpellGoal — the vanish: 1200 ticks of
    // INVISIBILITY on the illusioner itself, gated on not already having it.
    // The four mirror images the client draws while invisible
    // (clientSideIllusionOffsets, getIllusionOffsets) are pure render
    // trickery — SKIPPED: effects do not sync to the client and the renderer
    // has no multi-instance draw for one mob; the spell's server half (the
    // effect, the cast timing, the spell id on the wire) is exact.
    class IllusionerMirrorSpellGoal : public SpellcasterUseSpellGoal {
    public:
        explicit IllusionerMirrorSpellGoal(Illusioner* illusioner);

        bool CanUse() override;
        const char* Name() const override { return "IllusionerMirrorSpellGoal"; }

    protected:
        void PerformSpellCasting() override;
        int GetCastingTime() const override { return 20; }
        int GetCastingInterval() const override { return 340; }
        SpellcasterIllager::IllagerSpell GetSpell() const override {
            return SpellcasterIllager::IllagerSpell::Disappear;
        }

    private:
        Illusioner* m_illusioner;
    };

    // MC Illusioner.IllusionerBlindnessSpellGoal — 400 ticks of BLINDNESS on
    // the target, once per target (lastTargetId), HARD difficulty only.
    class IllusionerBlindnessSpellGoal : public SpellcasterUseSpellGoal {
    public:
        explicit IllusionerBlindnessSpellGoal(Illusioner* illusioner);

        bool CanUse() override;
        void Start() override;
        const char* Name() const override { return "IllusionerBlindnessSpellGoal"; }

    protected:
        void PerformSpellCasting() override;
        int GetCastingTime() const override { return 20; }
        int GetCastingInterval() const override { return 180; }
        SpellcasterIllager::IllagerSpell GetSpell() const override {
            return SpellcasterIllager::IllagerSpell::Blindness;
        }

    private:
        Illusioner* m_illusioner;
        // MC lastTargetId — one blinding per target acquisition.
        int32_t m_lastTargetId = 0;
    };

} // namespace Game
