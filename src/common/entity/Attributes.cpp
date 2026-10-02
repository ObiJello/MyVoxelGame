// File: src/common/entity/Attributes.cpp
#include "common/entity/Attributes.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>

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

    const AttributeModifier* AttributeInstance::FindModifier(uint32_t id) const {
        for (const AttributeModifier& m : m_modifiers) {
            if (m.id == id) return &m;
        }
        return nullptr;
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
        // A later layer of the supplier chain (createMobAttributes over
        // createLivingAttributes, a mob's own createAttributes) overrides
        // the row: base AND the default /attribute … base reset returns to.
        if (AttributeInstance* existing = Find(attr)) {
            existing->SetBaseValue(base);
            existing->SetDefaultValue(base);
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
        // Materialised by a base write: the type never supplied it, so its
        // reset default is the registry's.
        m_instances.emplace_back(attr, v);
        m_instances.back().SetDefaultValue(kAttributeTable[static_cast<size_t>(attr)].defaultValue);
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

    // ── Registry ───────────────────────────────────────────────────────────

    bool AttributeFromName(std::string_view name, Attribute& out) {
        if (name.rfind("minecraft:", 0) == 0) name.remove_prefix(10);
        // 1.20.5–1.21.1 ids ("generic.attack_damage", "player.block_
        // interaction_range"), renamed by MC's datafixer.
        if (name.rfind("generic.", 0) == 0) name.remove_prefix(8);
        else if (name.rfind("player.", 0) == 0) name.remove_prefix(7);
        else if (name.rfind("zombie.", 0) == 0) name.remove_prefix(7);
        if (name == "spawn_reinforcements_chance") name = "spawn_reinforcements";
        for (size_t i = 0; i < static_cast<size_t>(Attribute::Count); ++i) {
            if (kAttributeTable[i].name == name) {
                out = static_cast<Attribute>(i);
                return true;
            }
        }
        return false;
    }

    // ── Named modifiers ────────────────────────────────────────────────────

    namespace {
        struct ModifierNameTable {
            std::mutex                                mutex;
            std::unordered_map<uint32_t, std::string> names;
        };
        ModifierNameTable& ModifierNames() {
            static ModifierNameTable table;
            return table;
        }

        // MC's Identifier for the engine's fixed ModifierId values — the ids
        // the modifiers carry in MC (Zombie.SPEED_MODIFIER_BABY_ID,
        // MobEffects' "effect.<name>" templates …). Only what MC names.
        const char* WellKnownModifierName(uint32_t id) {
            switch (static_cast<ModifierId>(id)) {
                case ModifierId::BabySpeedBoost:          return "minecraft:baby";
                case ModifierId::RandomSpawnBonus:        return "minecraft:random_spawn_bonus";
                case ModifierId::ZombieLeaderHealth:      return "minecraft:leader_zombie_bonus";
                case ModifierId::ZombieLeaderReinf:       return "minecraft:leader_zombie_bonus";
                case ModifierId::ZombieSpawnReinf:        return "minecraft:reinforcement_caller_charge";
                case ModifierId::ZombieRandomKnockback:   return "minecraft:zombie_random_spawn_bonus";
                case ModifierId::ZombieReinfCalleeCharge: return "minecraft:reinforcement_callee_charge";
                case ModifierId::EffectSpeed:             return "minecraft:effect.speed";
                case ModifierId::EffectSlowness:          return "minecraft:effect.slowness";
                case ModifierId::EffectHaste:             return "minecraft:effect.haste";
                case ModifierId::EffectMiningFatigue:     return "minecraft:effect.mining_fatigue";
                case ModifierId::EffectStrength:          return "minecraft:effect.strength";
                case ModifierId::EffectWeakness:          return "minecraft:effect.weakness";
                case ModifierId::EffectJumpBoost:         return "minecraft:effect.jump_boost";
                case ModifierId::EffectHealthBoost:       return "minecraft:effect.health_boost";
                case ModifierId::EffectAbsorption:        return "minecraft:effect.absorption";
                case ModifierId::EffectLuck:              return "minecraft:effect.luck";
                case ModifierId::EffectUnluck:            return "minecraft:effect.unluck";
                case ModifierId::WitchDrinkingSlowdown:   return "minecraft:drinking";
                case ModifierId::PiglinAttackingSpeed:    return "minecraft:attacking";
                case ModifierId::EndermanAttackingSpeed:  return "minecraft:attacking";
                case ModifierId::RabbitEvilAttackPower:   return "minecraft:evil";
                case ModifierId::BodyArmorEquipment:      return "minecraft:armor.body";
                default:                                  return nullptr;
            }
        }

        bool IsValidNamespaceChar(char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        }
    } // namespace

    std::string NormalizeIdentifier(std::string_view id) {
        if (id.find(':') == std::string_view::npos) return "minecraft:" + std::string(id);
        // MC Identifier.parse: ":foo" is minecraft:foo.
        if (!id.empty() && id.front() == ':') return "minecraft" + std::string(id);
        return std::string(id);
    }

    bool IsValidIdentifier(std::string_view id) {
        const size_t colon = id.find(':');
        std::string_view ns = colon == std::string_view::npos ? std::string_view{} : id.substr(0, colon);
        std::string_view path = colon == std::string_view::npos ? id : id.substr(colon + 1);
        if (path.empty() || path.find(':') != std::string_view::npos) return false;
        for (const char c : ns) if (!IsValidNamespaceChar(c)) return false;
        for (const char c : path) if (!IsValidNamespaceChar(c) && c != '/') return false;
        return true;
    }

    ModifierId NamedModifierId(std::string_view id) {
        const std::string name = NormalizeIdentifier(id);
        const ModifierId out = ItemModifierId(name);
        RecordModifierIdName(static_cast<uint32_t>(out), name);
        return out;
    }

    void RecordModifierIdName(uint32_t id, std::string_view name) {
        if (name.empty()) return;
        ModifierNameTable& table = ModifierNames();
        std::lock_guard<std::mutex> lock(table.mutex);
        table.names[id] = std::string(name);
    }

    std::string ModifierIdName(uint32_t id) {
        {
            ModifierNameTable& table = ModifierNames();
            std::lock_guard<std::mutex> lock(table.mutex);
            if (const auto it = table.names.find(id); it != table.names.end()) return it->second;
        }
        if (const char* known = WellKnownModifierName(id)) return known;
        return {};
    }

    std::string AttributeDescriptionId(Attribute attribute) {
        return "attribute.name." + std::string(AttributeName(attribute));
    }

    double PlayerBaseAttributeValue(Attribute attribute) {
        switch (attribute) {
            case Attribute::AttackDamage:          return 1.0;
            case Attribute::MovementSpeed:         return 0.10000000149011612;
            case Attribute::WaypointTransmitRange: return 6.0e7;
            case Attribute::WaypointReceiveRange:  return 6.0e7;
            default:                               return GetAttributeDef(attribute).defaultValue;
        }
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
        out.Register(Attribute::JumpStrength,         0.41999998688697815);
        out.Register(Attribute::AttackKnockback,      0.0);
        // The rest of createLivingAttributes' rows (registry defaults).
        out.Register(Attribute::EntityInteractionRange,       3.0);
        out.Register(Attribute::OxygenBonus,                  0.0);
        out.Register(Attribute::BurningTime,                  1.0);
        out.Register(Attribute::ExplosionKnockbackResistance, 0.0);
        out.Register(Attribute::WaterMovementEfficiency,      0.0);
        out.Register(Attribute::MovementEfficiency,           0.0);
        out.Register(Attribute::CameraDistance,               4.0);
        out.Register(Attribute::WaypointTransmitRange,        0.0);
        out.Register(Attribute::Bounciness,                   0.0);
        out.Register(Attribute::AirDragModifier,              1.0);
        out.Register(Attribute::FrictionModifier,             1.0);
        out.Register(Attribute::NameTagDistance,             64.0);
        out.Register(Attribute::BelowNameDistance,           10.0);
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

    void CreatePlayerAttributes(AttributeMap& out) {
        // Player.createAttributes: createLivingAttributes().add(ATTACK_DAMAGE,
        // 1.0).add(MOVEMENT_SPEED, 0.1).add(ATTACK_SPEED).add(LUCK)
        // .add(BLOCK_INTERACTION_RANGE).add(BLOCK_BREAK_SPEED)
        // .add(SUBMERGED_MINING_SPEED).add(SNEAKING_SPEED)
        // .add(MINING_EFFICIENCY).add(SWEEPING_DAMAGE_RATIO)
        // .add(WAYPOINT_TRANSMIT_RANGE, 6e7).add(WAYPOINT_RECEIVE_RANGE, 6e7).
        CreateLivingAttributes(out);
        for (const Attribute a : { Attribute::AttackDamage, Attribute::MovementSpeed, Attribute::AttackSpeed,
                                   Attribute::Luck, Attribute::BlockInteractionRange, Attribute::BlockBreakSpeed,
                                   Attribute::SubmergedMiningSpeed, Attribute::SneakingSpeed,
                                   Attribute::MiningEfficiency, Attribute::SweepingDamageRatio,
                                   Attribute::WaypointTransmitRange, Attribute::WaypointReceiveRange }) {
            out.Register(a, PlayerBaseAttributeValue(a));
        }
    }

} // namespace Game
