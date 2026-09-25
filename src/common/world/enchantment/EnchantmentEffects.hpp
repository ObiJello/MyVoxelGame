// File: src/common/world/enchantment/EnchantmentEffects.hpp
//
// MC's enchantment EFFECT COMPONENTS (world/item/enchantment/
// EnchantmentEffectComponents.java), parsed from the `effects` object of
// data/minecraft/enchantment/<slug>.json with the Java codecs' rules:
//
//   ConditionalEffect<T>         an effect plus optional `requirements`
//   TargetedConditionalEffect<T> the same, naming which side of a hit is
//                                enchanted and which is affected
//   EnchantmentValueEffect       rewrites a number (EnchantmentValueEffect.hpp)
//   EnchantmentEntityEffect      does something to an entity (ignite, apply a
//                                mob effect, damage, wear the item, summon,
//                                play a sound, explode, replace a disk ...)
//   EnchantmentLocationBasedEffect  an entity effect or an attribute modifier
//                                that switches on and off as its holder moves
//   EnchantmentAttributeEffect   an attribute modifier while the item is worn
//
// The requirements are MC LootItemConditions evaluated against the
// LootContext Enchantment.damageContext / itemContext / entityContext /
// locationContext / blockHitContext builds. EnchantmentContext below is that
// context; EnchantmentCondition is the condition subset the vanilla data
// uses — match_tool, entity_properties, damage_source_properties,
// random_chance, enchantment_active_check, weather_check, location_check and
// the inverted / all_of / any_of combinators. A condition (or a predicate
// field inside one) this engine cannot evaluate parses as Unsupported and
// TESTS FALSE, so an effect gated on something unmodelled never fires rather
// than firing unconditionally. The runners that walk an entity's equipment
// and apply these are EnchantmentHelper's.
#pragma once

#include "Enchantment.hpp"
#include "EnchantmentValueEffect.hpp"
#include "common/entity/Attributes.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/world/block/BlockState.hpp"

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Game {

    class Entity;
    class LivingEntity;
    struct EntityLevel;
    struct IBlockAccess;
    class JavaRandom;
    struct ItemStack;
    struct DamageSourceInfo;

    // ── Context ──────────────────────────────────────────────────────────

    // What an entity_properties predicate reads off one entity (MC
    // EntityPredicate's inputs). Built from a Game::Entity on the server
    // (Of); the client builds one from its own player, which is no Entity.
    struct EnchantmentEntityFacts {
        std::string typeId;               // "minecraft:zombie", "minecraft:player"
        const Entity* entity = nullptr;   // for the vehicle sub-predicate; may be null
        glm::dvec3 position{0.0};
        glm::dvec3 knownMovement{0.0};    // MC getKnownMovement, blocks per tick
        double fallDistance = 0.0;
        int  tickCount  = 0;
        bool onGround   = false;
        bool onFire     = false;
        bool crouching  = false;
        bool sprinting  = false;
        bool swimming   = false;
        // EntityFlagsPredicate.isFlying: fall-flying, or a player's
        // abilities.flying.
        bool flying     = false;
        bool fallFlying = false;
        bool inWater    = false;
        bool baby       = false;
        bool hasVehicle = false;

        static EnchantmentEntityFacts Of(const Entity& e);
    };

    // MC LootContext as the enchantment code builds it. Unset parameters are
    // simply absent (MC's getOptionalParameter returning null), which fails
    // any condition that needs them.
    struct EnchantmentContext {
        EntityLevel*        level  = nullptr;   // server level; null on the client
        const IBlockAccess* blocks = nullptr;   // for location predicates
        JavaRandom*         random = nullptr;   // LootContext.getRandom (the level's)
        int                 enchantmentLevel = 0;          // ENCHANTMENT_LEVEL
        const ItemStack*    tool = nullptr;                // TOOL
        Entity*             thisEntity = nullptr;          // THIS_ENTITY
        // Facts for THIS_ENTITY when it is not a Game::Entity (the client's
        // player); otherwise derived from thisEntity.
        const EnchantmentEntityFacts* thisFacts = nullptr;
        const DamageSourceInfo* damage = nullptr;          // DAMAGE_SOURCE + the two attackers
        glm::dvec3          origin{0.0};                   // ORIGIN
        int                 enchantmentActive = -1;        // ENCHANTMENT_ACTIVE (-1 absent)
        bool                hasBlockState = false;         // BLOCK_STATE
        BlockState          blockState{};
    };

    // MC EnchantedItemInUse — the item an effect runs for, where it is worn
    // and by whom, and what to do if an effect wears it out.
    struct EnchantedItemInUse {
        ItemStack*                   stack = nullptr;
        std::optional<EquipmentSlot> slot;        // null for a projectile's launcher
        LivingEntity*                owner = nullptr;
        std::function<void(const ItemStack& broken)> onBreak;   // default: owner's equipment break
    };

    // MC EnchantmentTarget.
    enum class EnchantmentTarget : uint8_t { Attacker, DamagingEntity, Victim };

    // ── Predicates ───────────────────────────────────────────────────────

    // MC MinMaxBounds.Doubles (a bare number is an exact bound).
    struct DoubleBounds {
        std::optional<double> min, max;
        bool Matches(double v) const;
        // MC matchesSqr — compare a squared magnitude against squared bounds.
        bool MatchesSqr(double sq) const;
        static DoubleBounds Parse(const nlohmann::json& j);
    };

    // MC LocationPredicate, the fields the vanilla enchantments use: the
    // block (a HolderSet of blocks) and can_see_sky. Anything else marks the
    // predicate unsupported.
    struct EnchantmentLocationPredicate {
        std::vector<std::string> blocks;     // BlockPredicate.blocks
        bool hasBlocks = false;
        std::optional<bool> canSeeSky;
        bool unsupported = false;
        bool Matches(const EnchantmentContext& ctx, const glm::dvec3& pos) const;
        static EnchantmentLocationPredicate Parse(const nlohmann::json& j);
    };

    // MC EntityPredicate: type, flags, location, movement,
    // movement_affected_by, periodic_tick and vehicle. Any other field
    // (type_specific, equipment, effects, nbt ...) marks it unsupported.
    struct EnchantmentEntityPredicate {
        std::vector<std::string> types;      // EntityTypePredicate HolderSet
        bool hasType = false;
        std::optional<bool> onGround, onFire, sneaking, sprinting, swimming, flying,
                            baby, inWater, fallFlying;
        std::vector<EnchantmentLocationPredicate> location;           // 0 or 1
        std::vector<EnchantmentLocationPredicate> movementAffectedBy; // 0 or 1
        bool hasMovement = false;
        DoubleBounds moveX, moveY, moveZ, speed, horizontalSpeed, verticalSpeed, fallDistance;
        int  periodicTick = 0;
        std::vector<EnchantmentEntityPredicate> vehicle;              // 0 or 1
        bool unsupported = false;

        // `{}` — asks nothing but that the entity exists.
        bool IsEmpty() const {
            return !hasType && !onGround && !onFire && !sneaking && !sprinting && !swimming &&
                   !flying && !baby && !inWater && !fallFlying && location.empty() &&
                   movementAffectedBy.empty() && !hasMovement && periodicTick == 0 &&
                   vehicle.empty() && !unsupported;
        }

        bool Matches(const EnchantmentContext& ctx, const EnchantmentEntityFacts& facts) const;
        static EnchantmentEntityPredicate Parse(const nlohmann::json& j);
    };

    // MC NumberProvider, for random_chance: a constant, uniform, or
    // enchantment_level (a LevelBasedValue at the context's level).
    struct EnchantmentNumberProvider {
        enum class Type : uint8_t { Constant, Uniform, EnchantmentLevel, Unsupported };
        Type  type = Type::Constant;
        float value = 0.0f;                  // Constant
        float min = 0.0f, max = 0.0f;        // Uniform
        LevelBasedValue amount;              // EnchantmentLevel
        float GetFloat(const EnchantmentContext& ctx) const;
        static EnchantmentNumberProvider Parse(const nlohmann::json& j);
    };

    // MC LootItemCondition, the enchantment subset (see the header note).
    struct EnchantmentCondition {
        enum class Type : uint8_t {
            MatchTool, Inverted, AllOf, AnyOf, EntityProperties, DamageSourceProperties,
            RandomChance, EnchantmentActiveCheck, WeatherCheck, LocationCheck, Unsupported
        };
        enum class EntityTarget : uint8_t { This, Attacker, DirectAttacker, AttackingPlayer };

        Type type = Type::Unsupported;
        std::vector<EnchantmentCondition> terms;          // Inverted (one), AllOf, AnyOf
        std::vector<std::string> items;                   // MatchTool: the item HolderSet
        EntityTarget entity = EntityTarget::This;               // EntityProperties
        EnchantmentEntityPredicate entityPredicate;       // EntityProperties
        // DamageSourceProperties: the tag list, is_direct and the two
        // entity sub-predicates (0 or 1 each).
        std::vector<std::pair<std::string, bool>> damageTags;
        std::optional<bool> isDirect;
        std::vector<EnchantmentEntityPredicate> directEntity, sourceEntity;
        EnchantmentNumberProvider chance;                 // RandomChance
        bool active = false;                              // EnchantmentActiveCheck
        std::optional<bool> raining, thundering;          // WeatherCheck
        EnchantmentLocationPredicate location;            // LocationCheck
        glm::ivec3 offset{0};                             // LocationCheck

        bool Test(const EnchantmentContext& ctx) const;
        // A condition object, or an array (MC's list form = all_of).
        static EnchantmentCondition Parse(const nlohmann::json& j);
    };

    // ── Effects ──────────────────────────────────────────────────────────

    // ConditionalEffect<EnchantmentValueEffect>.
    struct ConditionalValueEffect {
        EnchantmentValueEffect effect;
        bool                   hasRequirements = false;
        EnchantmentCondition   requirements;
        bool Matches(const EnchantmentContext& ctx) const {
            return !hasRequirements || requirements.Test(ctx);
        }
    };

    // TargetedConditionalEffect<EnchantmentValueEffect> (equipment_drops).
    struct TargetedConditionalValueEffect : ConditionalValueEffect {
        EnchantmentTarget enchanted = EnchantmentTarget::Attacker;
        EnchantmentTarget affected  = EnchantmentTarget::Victim;
    };

    // MC EnchantmentAttributeEffect.
    struct EnchantmentAttributeEffect {
        std::string        id;               // "minecraft:enchantment.efficiency"
        Attribute          attribute = Attribute::Count;   // Count = unknown attribute
        LevelBasedValue    amount;
        AttributeOperation operation = AttributeOperation::AddValue;

        bool Valid() const { return attribute != Attribute::Count; }
        // getModifier(level, slot): the id suffixed with the slot.
        AttributeModifier GetModifier(int level, EquipmentSlot slot) const;
        static bool Parse(const nlohmann::json& j, EnchantmentAttributeEffect& out);
    };

    // MC EnchantmentEntityEffect / EnchantmentLocationBasedEffect. One struct
    // for every shape; the fields a type does not use stay default.
    struct EnchantmentEntityEffect {
        enum class Type : uint8_t {
            AllOf, ApplyMobEffect, Ignite, DamageEntity, ChangeItemDamage, SummonEntity,
            PlaySound, Explode, ApplyExhaustion, ReplaceDisk, SpawnParticles,
            Attribute,      // location_changed only (EnchantmentAttributeEffect)
            Unsupported
        };
        Type type = Type::Unsupported;
        std::string typeName;                              // for the log

        std::vector<EnchantmentEntityEffect> effects;      // AllOf
        // ApplyMobEffect: the HolderSet, durations (s) and amplifiers.
        std::vector<std::string> mobEffects;
        LevelBasedValue minDuration, maxDuration, minAmplifier, maxAmplifier;
        // Ignite duration (s) / DamageEntity min + max / ChangeItemDamage and
        // ApplyExhaustion amount / Explode radius / ReplaceDisk radius +
        // height — `a` and `b` in that order.
        LevelBasedValue a, b;
        std::string damageType;                            // DamageEntity
        std::vector<std::string> entityTypes;              // SummonEntity
        // PlaySound: the sounds (one per level, the last repeating) and the
        // two FloatProviders (constant, or uniform min/max).
        std::vector<std::string> sounds;
        float volumeMin = 1.0f, volumeMax = 1.0f, pitchMin = 1.0f, pitchMax = 1.0f;
        bool  volumeUniform = false, pitchUniform = false;
        // Explode.
        bool attributeToUser = false;
        bool hasKnockbackMultiplier = false;
        LevelBasedValue knockbackMultiplier;
        bool createFire = false;
        std::string blockInteraction;                      // "none" / "trigger" / ...
        std::string explosionSound;                        // namespace stripped
        bool vanillaExplosionParticles = false;            // explosion + explosion_emitter
        glm::dvec3 explodeOffset{0.0};
        // ReplaceDisk.
        glm::ivec3 diskOffset{0};
        BlockState diskState{};
        bool hasDiskState = false;
        struct BlockPredicate {
            enum class Kind : uint8_t { AllOf, MatchingBlockTag, MatchingBlocks, MatchingFluids,
                                        Unobstructed, Unsupported };
            Kind kind = Kind::Unsupported;
            glm::ivec3 offset{0};
            std::vector<std::string> values;               // tag / blocks / fluids
            std::vector<BlockPredicate> predicates;        // AllOf
        };
        std::vector<BlockPredicate> diskPredicate;         // 0 or 1
        // Attribute (location_changed).
        EnchantmentAttributeEffect attribute;

        // MC EnchantmentEntityEffect.apply(serverLevel, level, item, entity,
        // position). Server only; a no-op without a level.
        void Apply(EntityLevel& level, int enchantmentLevel, const EnchantedItemInUse& item,
                   Entity& entity, const glm::dvec3& position) const;

        // MC EnchantmentLocationBasedEffect.onChangedBlock / onDeactivated.
        // `attributes` is where an attribute effect lands (the entity's own
        // map on the server; the client's player keeps its own). `level` and
        // `entity` may be null on the client, which then only runs the
        // attribute half.
        void OnChangedBlock(EntityLevel* level, int enchantmentLevel, const EnchantedItemInUse& item,
                            Entity* entity, AttributeMap* attributes, const glm::dvec3& position,
                            bool becameActive) const;
        void OnDeactivated(const EnchantedItemInUse& item, AttributeMap* attributes,
                           int enchantmentLevel) const;

        static EnchantmentEntityEffect Parse(const nlohmann::json& j, bool locationBased);
    };

    struct ConditionalEntityEffect {
        EnchantmentEntityEffect effect;
        bool                    hasRequirements = false;
        EnchantmentCondition    requirements;
        bool Matches(const EnchantmentContext& ctx) const {
            return !hasRequirements || requirements.Test(ctx);
        }
    };

    struct TargetedConditionalEntityEffect : ConditionalEntityEffect {
        EnchantmentTarget enchanted = EnchantmentTarget::Attacker;
        EnchantmentTarget affected  = EnchantmentTarget::Victim;
    };

    // Every effect component of one enchantment. Lists are empty when the
    // JSON does not carry the component.
    struct EnchantmentEffectComponents {
        // Value components (List<ConditionalEffect<EnchantmentValueEffect>>).
        std::vector<ConditionalValueEffect> itemDamage;             // minecraft:item_damage
        std::vector<ConditionalValueEffect> damage;                 // minecraft:damage
        std::vector<ConditionalValueEffect> damageProtection;       // minecraft:damage_protection
        std::vector<ConditionalValueEffect> knockback;              // minecraft:knockback
        std::vector<ConditionalValueEffect> armorEffectiveness;     // minecraft:armor_effectiveness
        std::vector<ConditionalValueEffect> smashDamagePerFallenBlock;
        std::vector<ConditionalValueEffect> blockExperience;
        std::vector<ConditionalValueEffect> mobExperience;
        std::vector<ConditionalValueEffect> repairWithXp;
        std::vector<ConditionalValueEffect> ammoUse;
        std::vector<ConditionalValueEffect> projectilePiercing;
        std::vector<ConditionalValueEffect> projectileCount;
        std::vector<ConditionalValueEffect> projectileSpread;
        std::vector<ConditionalValueEffect> tridentReturnAcceleration;
        std::vector<ConditionalValueEffect> fishingTimeReduction;
        std::vector<ConditionalValueEffect> fishingLuckBonus;
        std::vector<TargetedConditionalValueEffect> equipmentDrops;
        // Unfiltered value components (a single EnchantmentValueEffect).
        std::optional<EnchantmentValueEffect> crossbowChargeTime;
        std::optional<EnchantmentValueEffect> tridentSpinAttackStrength;
        // Entity-effect components.
        std::vector<TargetedConditionalEntityEffect> postAttack;
        std::vector<ConditionalEntityEffect> postPiercingAttack;
        std::vector<ConditionalEntityEffect> hitBlock;
        std::vector<ConditionalEntityEffect> tick;
        std::vector<ConditionalEntityEffect> projectileSpawned;
        std::vector<ConditionalEntityEffect> locationChanged;
        // List<ConditionalEffect<DamageImmunity>> — the effect is a unit.
        std::vector<EnchantmentCondition> damageImmunity;
        bool hasDamageImmunity = false;
        std::vector<EnchantmentAttributeEffect> attributes;
        // Unit components.
        bool preventEquipmentDrop = false;
        bool preventArmorChange   = false;
        // Sound lists (one per level): trident_sound; crossbow_charging_sounds
        // keeps only the start sounds.
        std::vector<std::string> tridentSound;
        std::vector<std::string> crossbowChargingSounds;

        static EnchantmentEffectComponents Parse(const nlohmann::json& effects, std::string_view owner);
    };

} // namespace Game
