// File: src/common/entity/Attributes.hpp
//
// MC net.minecraft.world.entity.ai.attributes — the attribute registry,
// per-instance modifier stacks, and the per-entity-type default suppliers.
//
// Two things here are easy to get wrong and both change mob behaviour visibly:
//
//  1. The modifier fold order. ADD_VALUE all apply to the base first, THEN
//     ADD_MULTIPLIED_BASE each scale the *original* base (not the running
//     total), and only then ADD_MULTIPLIED_TOTAL compound on the running
//     total. Folding them in one pass gives different numbers the moment an
//     entity has two kinds at once — which every zombie does, because
//     finalizeSpawn always adds a FOLLOW_RANGE ADD_MULTIPLIED_BASE roll.
//
//  2. FOLLOW_RANGE's default is 32 in the REGISTRY but Mob.createMobAttributes
//     overrides it to 16. Every mob goes through the latter. Using 32 doubles
//     every target-acquisition radius in the game.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace Game {

    enum class Attribute : uint8_t {
        Armor = 0,
        ArmorToughness,
        AttackDamage,
        AttackKnockback,
        AttackSpeed,
        FallDamageMultiplier,
        FollowRange,
        Gravity,
        JumpStrength,
        KnockbackResistance,
        MaxAbsorption,
        MaxHealth,
        MovementSpeed,
        SafeFallDistance,
        Scale,
        SpawnReinforcements,
        StepHeight,
        TemptRange,
        Count
    };

    struct AttributeDef {
        std::string_view name;
        double defaultValue;
        double min;
        double max;
    };

    // Attributes.java. Only the rows the mob system reads are carried; adding
    // one is a matter of appending here and to the enum.
    inline constexpr AttributeDef kAttributeTable[] = {
        /* Armor                */ { "armor",                  0.0,  0.0,    30.0 },
        /* ArmorToughness       */ { "armor_toughness",        0.0,  0.0,    20.0 },
        /* AttackDamage         */ { "attack_damage",          2.0,  0.0,  2048.0 },
        /* AttackKnockback      */ { "attack_knockback",       0.0,  0.0,     5.0 },
        /* AttackSpeed          */ { "attack_speed",           4.0,  0.0,  1024.0 },
        /* FallDamageMultiplier */ { "fall_damage_multiplier", 1.0,  0.0,   100.0 },
        /* FollowRange          */ { "follow_range",          32.0,  0.0,  2048.0 },
        /* Gravity              */ { "gravity",                0.08, -1.0,    1.0 },
        /* JumpStrength         */ { "jump_strength",          0.42, 0.0,    32.0 },
        /* KnockbackResistance  */ { "knockback_resistance",   0.0,  0.0,     1.0 },
        /* MaxAbsorption        */ { "max_absorption",         0.0,  0.0,  2048.0 },
        /* MaxHealth            */ { "max_health",            20.0,  1.0,  1024.0 },
        /* MovementSpeed        */ { "movement_speed",         0.7,  0.0,  1024.0 },
        /* SafeFallDistance     */ { "safe_fall_distance",     3.0, -1024.0, 1024.0 },
        /* Scale                */ { "scale",                  1.0,  0.0625, 16.0 },
        /* SpawnReinforcements  */ { "spawn_reinforcements",   0.0,  0.0,     1.0 },
        /* StepHeight           */ { "step_height",            0.6,  0.0,    10.0 },
        /* TemptRange           */ { "tempt_range",           10.0,  0.0,  2048.0 },
    };

    static_assert(sizeof(kAttributeTable) / sizeof(kAttributeTable[0]) ==
                      static_cast<size_t>(Attribute::Count),
                  "kAttributeTable must stay in sync with Attribute");

    enum class AttributeOperation : uint8_t {
        AddValue = 0,           // base += amount
        AddMultipliedBase = 1,  // result += base * amount
        AddMultipliedTotal = 2, // result *= (1 + amount)
    };

    struct AttributeModifier {
        // Stable identity so a modifier can be replaced or removed later —
        // MC keys on a ResourceLocation; an interned id is enough here.
        uint32_t           id = 0;
        double             amount = 0.0;
        AttributeOperation operation = AttributeOperation::AddValue;
    };

    // Well-known modifier ids. Kept as an enum so the call sites that add and
    // later remove the same modifier cannot drift apart.
    enum class ModifierId : uint32_t {
        BabySpeedBoost      = 1,  // Zombie SPEED_MODIFIER_BABY
        RandomSpawnBonus    = 2,  // Mob.finalizeSpawn FOLLOW_RANGE roll
        ZombieLeaderHealth  = 3,
        ZombieLeaderReinf   = 4,
        ZombieSpawnReinf    = 5,
        ZombieRandomKnockback = 6,
        SpiderSpeedEffect   = 7,
    };

    // One attribute on one entity: a base value plus its modifier stack.
    class AttributeInstance {
    public:
        AttributeInstance() = default;
        explicit AttributeInstance(Attribute attr, double base)
            : m_attribute(attr), m_base(base) {}

        double GetBaseValue() const { return m_base; }
        void   SetBaseValue(double v) { m_base = v; m_dirty = true; }

        void AddModifier(const AttributeModifier& mod);
        void RemoveModifier(ModifierId id);
        bool HasModifier(ModifierId id) const;

        // MC AttributeInstance.calculateValue, cached until something changes.
        double GetValue() const;

        Attribute GetAttribute() const { return m_attribute; }

    private:
        Attribute                      m_attribute = Attribute::MaxHealth;
        double                         m_base = 0.0;
        std::vector<AttributeModifier> m_modifiers;
        mutable double                 m_cached = 0.0;
        mutable bool                   m_dirty = true;
    };

    // Every attribute an entity has. Sparse by design: a mob only registers the
    // handful its supplier declares, and reading an unregistered attribute
    // returns the registry default rather than asserting — MC does the same, and
    // it keeps a goal that asks for TEMPT_RANGE on a zombie from crashing.
    class AttributeMap {
    public:
        void   Register(Attribute attr, double base);
        bool   Has(Attribute attr) const;

        double GetValue(Attribute attr) const;
        double GetBaseValue(Attribute attr) const;
        void   SetBaseValue(Attribute attr, double v);

        void AddModifier(Attribute attr, const AttributeModifier& mod);
        void RemoveModifier(Attribute attr, ModifierId id);
        bool HasModifier(Attribute attr, ModifierId id) const;

        AttributeInstance*       Find(Attribute attr);
        const AttributeInstance* Find(Attribute attr) const;

        const std::vector<AttributeInstance>& All() const { return m_instances; }

    private:
        std::vector<AttributeInstance> m_instances;
    };

    // ── Suppliers ──────────────────────────────────────────────────────────
    //
    // MC's createXxxAttributes() chain. Each layer adds to the previous, and
    // the per-mob createAttributes() then overrides individual base values.
    void CreateLivingAttributes(AttributeMap& out);
    void CreateMobAttributes(AttributeMap& out);      // + FOLLOW_RANGE 16.0
    void CreateMonsterAttributes(AttributeMap& out);  // + ATTACK_DAMAGE
    void CreateAnimalAttributes(AttributeMap& out);   // + TEMPT_RANGE 10.0

    // There is deliberately no CreateDefaultAttributes(EntityTypeId) table
    // here: MC keeps the per-type values in each mob's own createAttributes(),
    // and so does this port (see e.g. Zombie::CreateAttributes). A central
    // table would be a second place to update every time a mob is tuned.

} // namespace Game
