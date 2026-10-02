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
#include <string>
#include <string_view>
#include <vector>

namespace Game {

    enum class Attribute : uint8_t {
        // MC 26.3 generic.air_drag_modifier — multiplies the "1 - drag" of
        // travelInAir's 0.91 / 0.98 (LivingEntity.computeModifiedFriction).
        // Only the sulfur cube's archetypes touch it.
        AirDragModifier = 0,
        Armor,
        ArmorToughness,
        AttackDamage,
        AttackKnockback,
        AttackSpeed,
        // MC 26.3 generic.bounciness — the restitution Entity.move applies
        // to a collision (restituteMovementAfterCollisions). 0 = none.
        Bounciness,
        ExplosionKnockbackResistance,
        FallDamageMultiplier,
        FlyingSpeed,
        FollowRange,
        // MC 26.3 generic.friction_modifier — same fold as air drag, over the
        // block's slipperiness while on the ground.
        FrictionModifier,
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
        // MC generic.luck — a PLAYER attribute (Player.createAttributes);
        // LUCK / UNLUCK move it. Appended so the table order stays put.
        Luck,
        // The attributes MC's enchantments move (EnchantmentAttributeEffect,
        // data/minecraft/enchantment/*.json `minecraft:attributes`), appended
        // for the same reason. The player-only ones (Player.createAttributes)
        // are read off the player; the living ones (createLivingAttributes)
        // by the LivingEntity paths that consult them.
        MiningEfficiency,        // Efficiency — Player.getDestroySpeed
        SubmergedMiningSpeed,    // Aqua Affinity — ditto, eye in water
        OxygenBonus,             // Respiration — decreaseAirSupply's skip roll
        WaterMovementEfficiency, // Depth Strider — travelInWater
        SneakingSpeed,           // Swift Sneak — LocalPlayer.modifyInput
        SweepingDamageRatio,     // Sweeping Edge — Player.doSweepAttack
        BurningTime,             // Fire Protection — LivingEntity.igniteForTicks
        MovementEfficiency,      // Soul Speed — getBlockSpeedFactor's lerp
        // The rest of MC 26.3's Attributes.java, appended for the same
        // reason — every attribute an ATTRIBUTE_MODIFIERS entry may name.
        BelowNameDistance,       // the below-name scoreboard line's range
        BlockBreakSpeed,         // Player.getDestroySpeed's final factor
        BlockInteractionRange,   // Player.blockInteractionRange (reach)
        CameraDistance,          // the third-person camera's distance
        EntityInteractionRange,  // Player.entityInteractionRange (attack reach)
        NameTagDistance,         // the name tag's render range
        WaypointTransmitRange,   // locator bar: how far this entity is seen
        WaypointReceiveRange,    // locator bar: how far this player sees
        Count
    };

    // MC Attribute.Sentiment — which way an increase reads in a tooltip
    // (getStyle: POSITIVE blue up / red down, NEGATIVE the reverse, NEUTRAL
    // grey both ways).
    enum class AttributeSentiment : uint8_t { Positive, Neutral, Negative };

    struct AttributeDef {
        std::string_view name;
        double defaultValue;
        double min;
        double max;
        AttributeSentiment sentiment = AttributeSentiment::Positive;
    };

    // Attributes.java — every one of 26.3's 40 attributes (what /attribute
    // and an ATTRIBUTE_MODIFIERS entry may name); a new one is appended here
    // and to the enum.
    inline constexpr AttributeDef kAttributeTable[] = {
        /* AirDragModifier      */ { "air_drag_modifier",      1.0,  0.0,  2048.0 },
        /* Armor                */ { "armor",                  0.0,  0.0,    30.0 },
        /* ArmorToughness       */ { "armor_toughness",        0.0,  0.0,    20.0 },
        /* AttackDamage         */ { "attack_damage",          2.0,  0.0,  2048.0 },
        /* AttackKnockback      */ { "attack_knockback",       0.0,  0.0,     5.0 },
        /* AttackSpeed          */ { "attack_speed",           4.0,  0.0,  1024.0 },
        /* Bounciness           */ { "bounciness",             0.0,  0.0,     1.0 },
        /* ExplosionKnockbackResistance */
                                   { "explosion_knockback_resistance", 0.0, 0.0, 1.0 },
        /* FallDamageMultiplier */ { "fall_damage_multiplier", 1.0,  0.0,   100.0, AttributeSentiment::Negative },
        /* FlyingSpeed          */ { "flying_speed",           0.4,  0.0,  1024.0 },
        /* FollowRange          */ { "follow_range",          32.0,  0.0,  2048.0 },
        /* FrictionModifier     */ { "friction_modifier",      1.0,  0.0,  2048.0 },
        /* Gravity              */ { "gravity",                0.08, -1.0,    1.0, AttributeSentiment::Neutral },
        /* JumpStrength         */ { "jump_strength",          0.41999998688697815, 0.0, 32.0 },
        /* KnockbackResistance  */ { "knockback_resistance",   0.0, -2.0,     1.0 },
        /* MaxAbsorption        */ { "max_absorption",         0.0,  0.0,  2048.0 },
        /* MaxHealth            */ { "max_health",            20.0,  1.0,  1024.0 },
        /* MovementSpeed        */ { "movement_speed",         0.7,  0.0,  1024.0 },
        /* SafeFallDistance     */ { "safe_fall_distance",     3.0, -1024.0, 1024.0 },
        /* Scale                */ { "scale",                  1.0,  0.0625, 16.0, AttributeSentiment::Neutral },
        /* SpawnReinforcements  */ { "spawn_reinforcements",   0.0,  0.0,     1.0 },
        /* StepHeight           */ { "step_height",            0.6,  0.0,    10.0 },
        /* TemptRange           */ { "tempt_range",           10.0,  0.0,  2048.0 },
        /* Luck                 */ { "luck",                   0.0, -1024.0, 1024.0 },
        /* MiningEfficiency     */ { "mining_efficiency",      0.0,  0.0,  1024.0 },
        /* SubmergedMiningSpeed */ { "submerged_mining_speed", 0.2,  0.0,    20.0 },
        /* OxygenBonus          */ { "oxygen_bonus",           0.0,  0.0,  1024.0 },
        /* WaterMovementEfficiency */
                                   { "water_movement_efficiency", 0.0, 0.0,   1.0 },
        /* SneakingSpeed        */ { "sneaking_speed",         0.3,  0.0,     1.0 },
        /* SweepingDamageRatio  */ { "sweeping_damage_ratio",  0.0,  0.0,     1.0 },
        /* BurningTime          */ { "burning_time",           1.0,  0.0,  1024.0, AttributeSentiment::Negative },
        /* MovementEfficiency   */ { "movement_efficiency",    0.0,  0.0,     1.0 },
        /* BelowNameDistance    */ { "below_name_distance",   10.0,  0.0,   512.0 },
        /* BlockBreakSpeed      */ { "block_break_speed",      1.0,  0.0,  1024.0 },
        /* BlockInteractionRange*/ { "block_interaction_range", 4.5, 0.0,    64.0 },
        /* CameraDistance       */ { "camera_distance",        4.0,  0.0,    32.0 },
        /* EntityInteractionRange */
                                   { "entity_interaction_range", 3.0, 0.0,   64.0 },
        /* NameTagDistance      */ { "name_tag_distance",     64.0,  0.0,   512.0 },
        /* WaypointTransmitRange*/ { "waypoint_transmit_range", 0.0, 0.0, 6.0e7, AttributeSentiment::Neutral },
        /* WaypointReceiveRange */ { "waypoint_receive_range", 0.0,  0.0, 6.0e7, AttributeSentiment::Neutral },
    };

    inline const AttributeDef& GetAttributeDef(Attribute attribute) {
        return kAttributeTable[static_cast<size_t>(attribute)];
    }

    // The registry id without namespace ("attack_damage"), and back. The
    // lookup accepts "minecraft:attack_damage", the bare path, and the
    // pre-1.21.2 "generic." / "player." prefixed names a legacy save or
    // command may still carry.
    inline std::string_view AttributeName(Attribute attribute) { return GetAttributeDef(attribute).name; }
    bool AttributeFromName(std::string_view name, Attribute& out);

    // MC Attribute.getDescriptionId: "attribute.name.<name>" (the lang key
    // the tooltip translates).
    std::string AttributeDescriptionId(Attribute attribute);

    // MC Player.createAttributes' base values (LivingEntity.
    // createLivingAttributes plus the player's own rows): what a player's
    // attribute reads before any modifier. ATTACK_DAMAGE 1.0 and
    // MOVEMENT_SPEED 0.1 override the registry defaults; the waypoint
    // ranges are 6e7; everything else is the registry default.
    double PlayerBaseAttributeValue(Attribute attribute);

    // ServerPlayer's CREATIVE_BLOCK_INTERACTION_RANGE_MODIFIER and
    // CREATIVE_ENTITY_INTERACTION_RANGE_MODIFIER (ADD_VALUE, creative only)
    // — Player.CREATIVE_*_INTERACTION_RANGE_MODIFIER_VALUE.
    inline constexpr double kCreativeBlockInteractionRangeBonus  = 0.5;
    inline constexpr double kCreativeEntityInteractionRangeBonus = 2.0;

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
        // MC AttributeInstance.addPermanentModifier vs addTransientModifier:
        // a permanent modifier is SAVED with its entity ("attributes" →
        // "modifiers") — what /attribute … modifier add makes. Transient
        // ones (effects, worn gear, the creative reach bonus) are rebuilt by
        // whatever owns them. Not on the wire: a client only folds them.
        bool               permanent = false;
    };
    // (The operations' serialized names — "add_value", … — are
    // AttributeOperationName / AttributeOperationFromName in
    // common/data/components/AttributeComponents.hpp.)

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
        // Status-effect templates (MC MobEffects.java "effect.<name>" ids).
        // One id per effect, every level — MC replaces rather than stacks.
        EffectSpeed         = 8,
        EffectSlowness      = 9,
        EffectHaste         = 10,
        EffectMiningFatigue = 11,
        EffectStrength      = 12,
        EffectWeakness      = 13,
        EffectJumpBoost     = 14,
        WitchDrinkingSlowdown = 15,  // Witch SPEED_MODIFIER_DRINKING (-0.25)
        PiglinAttackingSpeed  = 16,  // ZombifiedPiglin SPEED_MODIFIER_ATTACKING (+0.05)
        // Zombie "reinforcement_callee_charge" — DISTINCT from the caller's
        // ZombieSpawnReinf id (MC Zombie.java:294,502): a callee later
        // promoted to caller carries both, so one id must not clobber the
        // other.
        ZombieReinfCalleeCharge = 17,
        EndermanAttackingSpeed  = 18,  // EnderMan SPEED_MODIFIER_ATTACKING (+0.15)
        // SulfurCubeArchetype.AttributeEntry — one id per attribute an
        // archetype modifies (knockback resistance, explosion knockback
        // resistance, bounciness, friction, air drag), swapped as a set when
        // the cube swallows or ejects a block. MC names them
        // "<archetype>_add_<attribute>"; the id is the same for every
        // archetype here because a cube only ever carries one.
        SulfurCubeKnockbackResistance = 19,
        SulfurCubeExplosionKnockbackResistance = 20,
        SulfurCubeBounciness = 21,
        SulfurCubeFriction = 22,
        SulfurCubeAirDrag = 23,
        // The rest of MobEffects.java's "effect.<name>" templates (see the
        // Effect* block above for why one id serves every level).
        EffectHealthBoost = 24,
        EffectAbsorption  = 25,
        EffectLuck        = 26,
        EffectUnluck      = 27,
        // MC Rabbit EVIL_ATTACK_POWER_MODIFIER ("minecraft:evil", +5).
        RabbitEvilAttackPower = 28,
        // MC "minecraft:armor.body" (EquipmentSlotGroup.BODY) — the armour a
        // mob's BODY slot adds (the wolf's wolf armor). Well clear of the
        // sequential ids above, which parallel work appends to.
        BodyArmorEquipment = 0x8000,
        // A mob's humanoid equipment (Mob::SetEquipment): the worn item's
        // modifiers per slot — MC's "minecraft:armor.<piece>" and
        // "base_attack_damage" / "base_attack_speed". The id of modifier `k`
        // (0 armor / attack damage, 1 toughness / attack speed, 2 knockback
        // resistance) in slot `s` is MobEquipmentBase + s * 4 + k.
        MobEquipmentBase   = 0x8100,
        // The first id of the enchantment-modifier range (see
        // EnchantmentModifierId below). Everything at or above it belongs
        // to an item's enchantments, never to a named modifier above.
        EnchantmentBase   = 0x10000,
        // The item ATTRIBUTE_MODIFIERS range (ItemModifierId below): an
        // entry's own id ("minecraft:base_attack_damage", "minecraft:armor.
        // chestplate", a /give'n "foo:bar"), hashed. Clear of the
        // enchantment range, which ends below 0x08100000.
        ItemModifierBase  = 0x40000000,
    };

    // MC EnchantmentAttributeEffect.getModifier(level, slot): the effect's
    // own id ("minecraft:enchantment.efficiency") suffixed with the slot it
    // is worn in, so one enchantment on two pieces gives two modifiers that
    // come and go with their pieces. Hashed (FNV-1a, stable across runs)
    // into the EnchantmentBase range, the slot in the low three bits.
    inline ModifierId EnchantmentModifierId(std::string_view effectId, uint8_t slot) {
        uint32_t h = 2166136261u;
        for (const char c : effectId) {
            h ^= static_cast<uint8_t>(c);
            h *= 16777619u;
        }
        return static_cast<ModifierId>(static_cast<uint32_t>(ModifierId::EnchantmentBase) +
                                       ((h & 0x0FFFFFu) << 3) + (slot & 7u));
    }

    // MC AttributeModifier.id for an item's ATTRIBUTE_MODIFIERS entry: the
    // Identifier (namespace defaulted to minecraft), hashed into the
    // ItemModifierBase range. Two entries with the same id replace one
    // another on an entity, exactly as MC's id-keyed modifier map does (a
    // sword in the main hand and one in the off hand both being
    // "minecraft:base_attack_damage").
    inline ModifierId ItemModifierId(std::string_view id) {
        uint32_t h = 2166136261u;
        const std::string_view ns = "minecraft:";
        if (id.find(':') == std::string_view::npos) {
            for (const char c : ns) { h ^= static_cast<uint8_t>(c); h *= 16777619u; }
        }
        for (const char c : id) {
            h ^= static_cast<uint8_t>(c);
            h *= 16777619u;
        }
        return static_cast<ModifierId>(static_cast<uint32_t>(ModifierId::ItemModifierBase) | (h & 0x3FFFFFFFu));
    }

    // ── Named modifiers (MC AttributeModifier.id is an Identifier) ───────
    //
    // A modifier from a command (/attribute … modifier add <id>), a save
    // ("attributes" → "modifiers" → "id") or an item shares MC's one id
    // space: the same Identifier names the same modifier wherever it came
    // from, so `/attribute @s attack_damage modifier value get
    // minecraft:base_attack_damage` reads the held sword's. NamedModifierId
    // is ItemModifierId plus a process-wide record of the name, so the id
    // can be turned back into its Identifier — to save it, to list it, to
    // complete it. Thread-safe (the integrated client and server share it).

    // "foo" → "minecraft:foo"; a namespaced id is returned as is (lower-cased
    // namespace and path are not enforced here — see IsValidIdentifier).
    std::string NormalizeIdentifier(std::string_view id);
    // MC Identifier.isValidNamespace / isValidPath: [a-z0-9_.-] namespace,
    // [a-z0-9_.-/] path, at most one ':'.
    bool IsValidIdentifier(std::string_view id);
    // ItemModifierId(id), remembering `id` (normalized) for ModifierIdName.
    ModifierId NamedModifierId(std::string_view id);
    // The Identifier a modifier id stands for: a name recorded by
    // NamedModifierId, else MC's id for one of the engine's well-known
    // modifiers ("minecraft:effect.speed", "minecraft:baby" …), else "".
    std::string ModifierIdName(uint32_t id);
    // Record `name` for `id` without hashing — the client learning the
    // names UpdateAttributesS2C carries beside its modifiers.
    void RecordModifierIdName(uint32_t id, std::string_view name);

    // Well-known transient player modifiers (MC ServerPlayer's creative
    // reach modifiers and this engine's step-height rule), by their MC /
    // engine Identifier.
    inline constexpr std::string_view kCreativeBlockRangeModifierName  = "minecraft:creative_mode_block_range";
    inline constexpr std::string_view kCreativeEntityRangeModifierName = "minecraft:creative_mode_entity_range";
    // /gamerule player_step_height (an ADD_VALUE onto every player's
    // STEP_HEIGHT; see ServerPlayer::applyStepHeightRule).
    inline constexpr std::string_view kStepHeightRuleModifierName = "obeycraft:player_step_height";

    // One attribute on one entity: a base value plus its modifier stack.
    class AttributeInstance {
    public:
        AttributeInstance() = default;
        explicit AttributeInstance(Attribute attr, double base)
            : m_attribute(attr), m_base(base), m_default(base) {}

        double GetBaseValue() const { return m_base; }
        void   SetBaseValue(double v) { m_base = v; m_dirty = true; }
        // The base the entity's type supplies (MC AttributeSupplier — what
        // AttributeMap.resetBaseValue goes back to): the value Register gave.
        double GetDefaultValue() const { return m_default; }
        void   SetDefaultValue(double v) { m_default = v; }
        void   ResetBaseValue() { SetBaseValue(m_default); }

        void AddModifier(const AttributeModifier& mod);
        void RemoveModifier(ModifierId id);
        bool HasModifier(ModifierId id) const;
        const AttributeModifier* FindModifier(uint32_t id) const;

        // MC AttributeInstance.calculateValue, cached until something changes.
        double GetValue() const;

        const std::vector<AttributeModifier>& Modifiers() const { return m_modifiers; }

        Attribute GetAttribute() const { return m_attribute; }

    private:
        Attribute                      m_attribute = Attribute::MaxHealth;
        double                         m_base = 0.0;
        double                         m_default = 0.0;
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
    // Player.createAttributes: createLivingAttributes + the player's rows,
    // at PlayerBaseAttributeValue (ATTACK_DAMAGE 1, MOVEMENT_SPEED 0.1, …).
    void CreatePlayerAttributes(AttributeMap& out);

    // There is deliberately no CreateDefaultAttributes(EntityTypeId) table
    // here: MC keeps the per-type values in each mob's own createAttributes(),
    // and so does this port (see e.g. Zombie::CreateAttributes). A central
    // table would be a second place to update every time a mob is tuned.

} // namespace Game
