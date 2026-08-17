// File: src/common/entity/Attributes.cpp
#include "common/entity/Attributes.hpp"

#include <algorithm>

namespace Game {

    // ── AttributeInstance ──────────────────────────────────────────────────

    void AttributeInstance::AddModifier(const AttributeModifier& mod) {
        // Replace rather than stack when the id already exists. MC's map is
        // keyed by id, so adding the same modifier twice is a no-op there; a
        // plain push_back here would silently double-apply it.
        for (AttributeModifier& existing : m_modifiers) {
            if (existing.id == mod.id) {
                existing = mod;
                m_dirty = true;
                return;
            }
        }
        m_modifiers.push_back(mod);
        m_dirty = true;
    }

    void AttributeInstance::RemoveModifier(ModifierId id) {
        const uint32_t raw = static_cast<uint32_t>(id);
        const auto it = std::remove_if(m_modifiers.begin(), m_modifiers.end(),
                                       [raw](const AttributeModifier& m) { return m.id == raw; });
        if (it != m_modifiers.end()) {
            m_modifiers.erase(it, m_modifiers.end());
            m_dirty = true;
        }
    }

    bool AttributeInstance::HasModifier(ModifierId id) const {
        const uint32_t raw = static_cast<uint32_t>(id);
        for (const AttributeModifier& m : m_modifiers) {
            if (m.id == raw) return true;
        }
        return false;
    }

    double AttributeInstance::GetValue() const {
        if (!m_dirty) return m_cached;

        // MC AttributeInstance.calculateValue — three separate passes, in this
        // order. See the header for why collapsing them is wrong.
        double base = m_base;
        for (const AttributeModifier& m : m_modifiers) {
            if (m.operation == AttributeOperation::AddValue) base += m.amount;
        }

        double result = base;
        for (const AttributeModifier& m : m_modifiers) {
            // Note: scales `base`, not `result` — two of these do not compound.
            if (m.operation == AttributeOperation::AddMultipliedBase) result += base * m.amount;
        }
        for (const AttributeModifier& m : m_modifiers) {
            if (m.operation == AttributeOperation::AddMultipliedTotal) result *= (1.0 + m.amount);
        }

        const AttributeDef& def = kAttributeTable[static_cast<size_t>(m_attribute)];
        m_cached = std::clamp(result, def.min, def.max);
        m_dirty = false;
        return m_cached;
    }

    // ── AttributeMap ───────────────────────────────────────────────────────

    void AttributeMap::Register(Attribute attr, double base) {
        if (AttributeInstance* existing = Find(attr)) {
            existing->SetBaseValue(base);
            return;
        }
        m_instances.emplace_back(attr, base);
    }

    bool AttributeMap::Has(Attribute attr) const { return Find(attr) != nullptr; }

    AttributeInstance* AttributeMap::Find(Attribute attr) {
        for (AttributeInstance& inst : m_instances) {
            if (inst.GetAttribute() == attr) return &inst;
        }
        return nullptr;
    }

    const AttributeInstance* AttributeMap::Find(Attribute attr) const {
        for (const AttributeInstance& inst : m_instances) {
            if (inst.GetAttribute() == attr) return &inst;
        }
        return nullptr;
    }

    double AttributeMap::GetValue(Attribute attr) const {
        // Unregistered reads fall back to the registry default rather than
        // asserting. MC behaves the same way, and it is what lets a shared goal
        // ask for TEMPT_RANGE on a monster that never declared it.
        if (const AttributeInstance* inst = Find(attr)) return inst->GetValue();
        return kAttributeTable[static_cast<size_t>(attr)].defaultValue;
    }

    double AttributeMap::GetBaseValue(Attribute attr) const {
        if (const AttributeInstance* inst = Find(attr)) return inst->GetBaseValue();
        return kAttributeTable[static_cast<size_t>(attr)].defaultValue;
    }

    void AttributeMap::SetBaseValue(Attribute attr, double v) {
        if (AttributeInstance* inst = Find(attr)) { inst->SetBaseValue(v); return; }
        m_instances.emplace_back(attr, v);
    }

    void AttributeMap::AddModifier(Attribute attr, const AttributeModifier& mod) {
        if (AttributeInstance* inst = Find(attr)) { inst->AddModifier(mod); return; }
        // Modifying something the type never declared: materialise it at the
        // registry default so the modifier still has an effect.
        m_instances.emplace_back(attr, kAttributeTable[static_cast<size_t>(attr)].defaultValue);
        m_instances.back().AddModifier(mod);
    }

    void AttributeMap::RemoveModifier(Attribute attr, ModifierId id) {
        if (AttributeInstance* inst = Find(attr)) inst->RemoveModifier(id);
    }

    bool AttributeMap::HasModifier(Attribute attr, ModifierId id) const {
        const AttributeInstance* inst = Find(attr);
        return inst && inst->HasModifier(id);
    }

    // ── Suppliers ──────────────────────────────────────────────────────────

    void CreateLivingAttributes(AttributeMap& out) {
        // LivingEntity.createLivingAttributes(). Only the rows this port
        // carries; each takes the registry default unless MC says otherwise.
        out.Register(Attribute::MaxHealth,           20.0);
        out.Register(Attribute::KnockbackResistance,  0.0);
        out.Register(Attribute::MovementSpeed,        0.7);
        out.Register(Attribute::Armor,                0.0);
        out.Register(Attribute::ArmorToughness,       0.0);
        out.Register(Attribute::MaxAbsorption,        0.0);
        out.Register(Attribute::StepHeight,           0.6);
        out.Register(Attribute::Scale,                1.0);
        out.Register(Attribute::Gravity,              0.08);
        out.Register(Attribute::SafeFallDistance,     3.0);
        out.Register(Attribute::FallDamageMultiplier, 1.0);
        out.Register(Attribute::JumpStrength,         0.42);
        out.Register(Attribute::AttackKnockback,      0.0);
    }

    void CreateMobAttributes(AttributeMap& out) {
        CreateLivingAttributes(out);
        // Mob.createMobAttributes overrides the registry's 32.0. Using the
        // registry value here doubles every mob's aggro radius.
        out.Register(Attribute::FollowRange, 16.0);
    }

    void CreateMonsterAttributes(AttributeMap& out) {
        CreateMobAttributes(out);
        out.Register(Attribute::AttackDamage, 2.0);
    }

    void CreateAnimalAttributes(AttributeMap& out) {
        CreateMobAttributes(out);
        out.Register(Attribute::TemptRange, 10.0);
    }

} // namespace Game
