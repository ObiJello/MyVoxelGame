// File: src/common/entity/ai/goals/IllusionerGoals.cpp
#include "common/entity/ai/goals/IllusionerGoals.hpp"
#include "common/entity/EntityLevel.hpp"

namespace Game {

    // ── IllusionerMirrorSpellGoal ──────────────────────────────────────────

    IllusionerMirrorSpellGoal::IllusionerMirrorSpellGoal(Illusioner* illusioner)
        : SpellcasterUseSpellGoal(illusioner), m_illusioner(illusioner) {}

    bool IllusionerMirrorSpellGoal::CanUse() {
        if (!SpellcasterUseSpellGoal::CanUse()) return false;
        // MC: !hasEffect(INVISIBILITY).
        return !m_illusioner->HasEffect(MobEffectId::Invisibility);
    }

    void IllusionerMirrorSpellGoal::PerformSpellCasting() {
        // MC: addEffect(new MobEffectInstance(INVISIBILITY, 1200)). The
        // vanish + mirror-image RENDER is the skipped client half — see the
        // header.
        m_illusioner->AddEffect(MobEffectInstance(MobEffectId::Invisibility, 1200));
    }

    // ── IllusionerBlindnessSpellGoal ───────────────────────────────────────

    IllusionerBlindnessSpellGoal::IllusionerBlindnessSpellGoal(
            Illusioner* illusioner)
        : SpellcasterUseSpellGoal(illusioner), m_illusioner(illusioner) {}

    bool IllusionerBlindnessSpellGoal::CanUse() {
        if (!SpellcasterUseSpellGoal::CanUse()) return false;
        LivingEntity* target = m_illusioner->GetTarget();
        if (target == nullptr) return false;
        if (target->GetId() == m_lastTargetId) return false;
        // MC: getCurrentDifficultyAt(pos).isHarderThan(NORMAL.ordinal()) —
        // effective difficulty above 2.0, which without MC's inhabited-time /
        // moon-phase scaling (see GetSpecialMultiplier's note) is exactly
        // "the difficulty setting is HARD".
        return m_illusioner->Level() &&
               m_illusioner->Level()->GetDifficulty() == Difficulty::Hard;
    }

    void IllusionerBlindnessSpellGoal::Start() {
        SpellcasterUseSpellGoal::Start();
        if (LivingEntity* target = m_illusioner->GetTarget()) {
            m_lastTargetId = target->GetId();
        }
    }

    void IllusionerBlindnessSpellGoal::PerformSpellCasting() {
        // MC: getTarget().addEffect(new MobEffectInstance(BLINDNESS, 400),
        // this).
        if (LivingEntity* target = m_illusioner->GetTarget()) {
            target->AddEffect(MobEffectInstance(MobEffectId::Blindness, 400),
                              m_illusioner);
        }
    }

} // namespace Game
