// File: src/common/world/enchantment/EnchantmentEffects.cpp
#include "EnchantmentEffects.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/LightningBolt.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/damagesource/DamageSourceInfo.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/level/Explosion.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/tags/DataTags.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string_view>

namespace Game {

    namespace {

        std::string_view StripDefaultNamespace(std::string_view id) {
            constexpr std::string_view kPrefix = "minecraft:";
            return id.substr(0, kPrefix.size()) == kPrefix ? id.substr(kPrefix.size()) : id;
        }

        std::string WithNamespace(std::string_view id) {
            std::string out(id);
            if (out.find(':') == std::string::npos) out = "minecraft:" + out;
            return out;
        }

        // A HolderSet field: one string or an array of strings.
        std::vector<std::string> ReadHolderSet(const nlohmann::json& j) {
            std::vector<std::string> out;
            if (j.is_string()) {
                out.push_back(j.get<std::string>());
            } else if (j.is_array()) {
                for (const auto& e : j) if (e.is_string()) out.push_back(e.get<std::string>());
            }
            return out;
        }

        std::optional<bool> ReadOptionalBool(const nlohmann::json& j, const char* key) {
            if (!j.contains(key) || !j[key].is_boolean()) return std::nullopt;
            return j[key].get<bool>();
        }

        bool ReadLevelBased(const nlohmann::json& j, const char* key, LevelBasedValue& out) {
            return j.contains(key) && LevelBasedValue::Parse(j[key], out);
        }

        glm::ivec3 ReadVec3i(const nlohmann::json& j, const char* key) {
            glm::ivec3 v(0);
            if (j.contains(key) && j[key].is_array() && j[key].size() == 3) {
                for (int i = 0; i < 3; ++i) if (j[key][i].is_number()) v[i] = j[key][i].get<int>();
            }
            return v;
        }

        // MC HolderSet.contains over an id with a registry's tags ("#tag"
        // entries answered by DataTags, plain ids by equality).
        bool HolderSetContains(const std::vector<std::string>& entries, DataTags::Registry registry,
                               const std::string& id) {
            for (const std::string& e : entries) {
                if (e.empty()) continue;
                if (e[0] == '#') {
                    if (DataTags::HasTag(registry, id, e)) return true;
                } else if (WithNamespace(e) == id) {
                    return true;
                }
            }
            return false;
        }

        std::string BlockIdOf(BlockID block) {
            const Block& b = BlockRegistry::Get(block);
            return WithNamespace(b.registrySlug.empty() ? b.name : b.registrySlug);
        }

        // MC FluidPredicate over the fluid registry: water / flowing_water /
        // lava / flowing_lava, and the #water / #lava tags.
        bool FluidHolderSetContains(const std::vector<std::string>& entries, const FluidState& fluid) {
            if (fluid.IsEmpty()) return false;
            const bool water = fluid.Is(FluidType::Water);
            const std::string id = water ? (fluid.IsSource() ? "minecraft:water" : "minecraft:flowing_water")
                                         : (fluid.IsSource() ? "minecraft:lava" : "minecraft:flowing_lava");
            return HolderSetContains(entries, DataTags::Registry::Fluid, id);
        }

        // MC Mth.randomBetween(random, min, max).
        float RandomBetween(JavaRandom& random, float min, float max) {
            return random.NextFloat() * (max - min) + min;
        }

        // Math.round(float).
        int RoundFloat(float v) { return static_cast<int>(std::floor(v + 0.5f)); }

        const char* SlotName(EquipmentSlot slot) {
            switch (slot) {
                case EquipmentSlot::MAINHAND: return "mainhand";
                case EquipmentSlot::OFFHAND:  return "offhand";
                case EquipmentSlot::FEET:     return "feet";
                case EquipmentSlot::LEGS:     return "legs";
                case EquipmentSlot::CHEST:    return "chest";
                case EquipmentSlot::HEAD:     return "head";
                case EquipmentSlot::BODY:     return "body";
                case EquipmentSlot::SADDLE:   return "saddle";
            }
            return "mainhand";
        }

        // The entity a condition's `entity` field names, in the context.
        Entity* ResolveEntity(const EnchantmentContext& ctx, EnchantmentCondition::EntityTarget ref) {
            switch (ref) {
                case EnchantmentCondition::EntityTarget::This:
                    return ctx.thisEntity;
                case EnchantmentCondition::EntityTarget::Attacker:
                    return ctx.damage ? ctx.damage->causing : nullptr;
                case EnchantmentCondition::EntityTarget::DirectAttacker:
                    return ctx.damage ? ctx.damage->direct : nullptr;
                case EnchantmentCondition::EntityTarget::AttackingPlayer:
                    // LAST_DAMAGE_PLAYER is never part of an enchantment's
                    // context (Enchantment.damageContext does not set it).
                    return nullptr;
            }
            return nullptr;
        }

        ExplosionInteraction InteractionByName(std::string_view name) {
            name = StripDefaultNamespace(name);
            if (name == "block")   return ExplosionInteraction::Block;
            if (name == "mob")     return ExplosionInteraction::Mob;
            if (name == "tnt")     return ExplosionInteraction::Tnt;
            if (name == "trigger") return ExplosionInteraction::Trigger;
            return ExplosionInteraction::None;
        }

        // SimpleStateProvider's `state` — {Name, Properties} as NbtUtils
        // writes it; unknown properties are skipped as readBlockState does.
        bool ReadBlockStateJson(const nlohmann::json& j, BlockState& out) {
            if (!j.is_object() || !j.contains("Name") || !j["Name"].is_string()) return false;
            BlockState state = BlockStates::FromSlug(StripDefaultNamespace(j["Name"].get<std::string>()));
            if (state.Block() == BlockID::Air && StripDefaultNamespace(j["Name"].get<std::string>()) != "air") {
                return false;
            }
            if (j.contains("Properties") && j["Properties"].is_object()) {
                const BlockID block = state.Block();
                for (const auto& [name, value] : j["Properties"].items()) {
                    if (!value.is_string()) continue;
                    for (uint16_t i = 0; i < BlockStates::PropertyCount(block); ++i) {
                        const PropertyId prop = BlockStates::PropertyAt(block, i);
                        if (BlockStates::PropertyName(prop) == name) {
                            state = state.SetName(prop, value.get<std::string>());
                            break;
                        }
                    }
                }
            }
            out = state;
            return true;
        }

        // A FloatProvider (PlaySoundEffect's volume / pitch): a constant or
        // uniform(min_inclusive, max_exclusive).
        bool ReadFloatProvider(const nlohmann::json& j, float& min, float& max, bool& uniform) {
            uniform = false;
            if (j.is_number()) { min = max = j.get<float>(); return true; }
            if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) return false;
            const std::string_view type = StripDefaultNamespace(j["type"].get_ref<const std::string&>());
            if (type == "constant" && j.contains("value") && j["value"].is_number()) {
                min = max = j["value"].get<float>();
                return true;
            }
            if (type == "uniform") {
                min = j.value("min_inclusive", 0.0f);
                max = j.value("max_exclusive", 1.0f);
                uniform = true;
                return true;
            }
            return false;
        }

        // UniformFloat.sample: Mth.randomBetween(random, min, max).
        float SampleFloat(JavaRandom& random, float min, float max, bool uniform) {
            return uniform ? RandomBetween(random, min, max) : min;
        }

        bool TestBlockPredicate(const EnchantmentEntityEffect::BlockPredicate& p, EntityLevel& level,
                                const glm::ivec3& origin) {
            using Kind = EnchantmentEntityEffect::BlockPredicate::Kind;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return false;
            const glm::ivec3 pos = origin + p.offset;
            switch (p.kind) {
                case Kind::AllOf:
                    for (const auto& child : p.predicates) {
                        if (!TestBlockPredicate(child, level, origin)) return false;
                    }
                    return true;
                case Kind::MatchingBlockTag:
                    return !p.values.empty() &&
                           DataTags::HasTag(DataTags::Registry::Block,
                                            BlockIdOf(blocks->GetBlockState(pos.x, pos.y, pos.z).Block()),
                                            p.values[0]);
                case Kind::MatchingBlocks:
                    return HolderSetContains(p.values, DataTags::Registry::Block,
                                             BlockIdOf(blocks->GetBlockState(pos.x, pos.y, pos.z).Block()));
                case Kind::MatchingFluids:
                    return FluidHolderSetContains(p.values, GetFluidState(*blocks, pos.x, pos.y, pos.z));
                case Kind::Unobstructed: {
                    // MC UnobstructedPredicate: isUnobstructed(STONE, pos) —
                    // no live entity that blocks building overlaps the cell.
                    std::vector<Entity*> inside;
                    const AABB cell = AABB::FromMinMax(glm::vec3(pos), glm::vec3(pos) + glm::vec3(1.0f));
                    level.GetEntitiesInBox(cell, nullptr, inside);
                    for (const Entity* e : inside) {
                        if (e && !e->IsRemoved() && e->BlocksBuilding()) return false;
                    }
                    return true;
                }
                case Kind::Unsupported:
                    return false;
            }
            return false;
        }

        EnchantmentEntityEffect::BlockPredicate ParseBlockPredicate(const nlohmann::json& j) {
            using BP = EnchantmentEntityEffect::BlockPredicate;
            BP out;
            if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) return out;
            const std::string_view type = StripDefaultNamespace(j["type"].get_ref<const std::string&>());
            out.offset = ReadVec3i(j, "offset");
            if (type == "all_of") {
                out.kind = BP::Kind::AllOf;
                if (j.contains("predicates") && j["predicates"].is_array()) {
                    for (const auto& child : j["predicates"]) out.predicates.push_back(ParseBlockPredicate(child));
                }
            } else if (type == "matching_block_tag" && j.contains("tag") && j["tag"].is_string()) {
                out.kind = BP::Kind::MatchingBlockTag;
                out.values.push_back(j["tag"].get<std::string>());
            } else if (type == "matching_blocks" && j.contains("blocks")) {
                out.kind = BP::Kind::MatchingBlocks;
                out.values = ReadHolderSet(j["blocks"]);
            } else if (type == "matching_fluids" && j.contains("fluids")) {
                out.kind = BP::Kind::MatchingFluids;
                out.values = ReadHolderSet(j["fluids"]);
            } else if (type == "unobstructed") {
                out.kind = BP::Kind::Unobstructed;
            }
            return out;
        }

        MobDamageSource MobSourceForDamageType(std::string_view type) {
            type = StripDefaultNamespace(type);
            if (type == "thorns")        return MobDamageSource::Thorns;
            if (type == "magic" || type == "indirect_magic") return MobDamageSource::Magic;
            if (type == "on_fire" || type == "in_fire") return MobDamageSource::Fire;
            if (type == "explosion" || type == "player_explosion") return MobDamageSource::Explosion;
            if (type == "player_attack") return MobDamageSource::PlayerAttack;
            if (type == "mob_attack")    return MobDamageSource::MobAttack;
            return MobDamageSource::Generic;
        }

    } // namespace

    // ── Entity facts ────────────────────────────────────────────────────────

    EnchantmentEntityFacts EnchantmentEntityFacts::Of(const Entity& e) {
        EnchantmentEntityFacts f;
        f.typeId        = e.IsPlayer() ? std::string("minecraft:player") : WithNamespace(e.TypeInfo().slug);
        f.entity        = &e;
        f.position      = e.position;
        f.knownMovement = e.GetKnownMovement();
        f.fallDistance  = e.fallDistance;
        f.tickCount     = e.tickCount;
        f.onGround      = e.onGround;
        f.onFire        = e.IsOnFire();
        f.sprinting     = e.IsSprinting();
        f.flying        = e.IsAbilityFlying();
        f.inWater       = e.IsInWater();
        f.baby          = e.IsBaby();
        f.hasVehicle    = e.GetVehicle() != nullptr;
        if (const LivingEntity* living = e.AsLiving()) {
            f.crouching = living->IsDiscrete();
            f.swimming  = living->IsSwimming();
        }
        return f;
    }

    // ── Predicates ──────────────────────────────────────────────────────────

    bool DoubleBounds::Matches(double v) const {
        if (min && v < *min) return false;
        if (max && v > *max) return false;
        return true;
    }

    bool DoubleBounds::MatchesSqr(double sq) const {
        if (min && sq < *min * *min) return false;
        if (max && sq > *max * *max) return false;
        return true;
    }

    DoubleBounds DoubleBounds::Parse(const nlohmann::json& j) {
        DoubleBounds b;
        if (j.is_number()) {
            b.min = b.max = j.get<double>();
        } else if (j.is_object()) {
            if (j.contains("min") && j["min"].is_number()) b.min = j["min"].get<double>();
            if (j.contains("max") && j["max"].is_number()) b.max = j["max"].get<double>();
        }
        return b;
    }

    bool EnchantmentLocationPredicate::Matches(const EnchantmentContext& ctx, const glm::dvec3& pos) const {
        if (unsupported) return false;
        const glm::ivec3 bp(static_cast<int>(std::floor(pos.x)), static_cast<int>(std::floor(pos.y)),
                            static_cast<int>(std::floor(pos.z)));
        if (hasBlocks) {
            const IBlockAccess* access = ctx.blocks;
            if (!access && ctx.level) access = ctx.level->Blocks();
            if (!access) return false;
            if (!HolderSetContains(blocks, DataTags::Registry::Block,
                                   BlockIdOf(access->GetBlockState(bp.x, bp.y, bp.z).Block()))) {
                return false;
            }
        }
        if (canSeeSky) {
            const bool sees = ctx.level && ctx.level->CanSeeSky(bp.x, bp.y, bp.z);
            if (sees != *canSeeSky) return false;
        }
        return true;
    }

    EnchantmentLocationPredicate EnchantmentLocationPredicate::Parse(const nlohmann::json& j) {
        EnchantmentLocationPredicate p;
        if (!j.is_object()) { p.unsupported = true; return p; }
        for (const auto& [key, value] : j.items()) {
            if (key == "block") {
                if (!value.is_object()) { p.unsupported = true; continue; }
                for (const auto& [bkey, bvalue] : value.items()) {
                    if (bkey == "blocks") {
                        p.blocks = ReadHolderSet(bvalue);
                        p.hasBlocks = true;
                    } else {
                        p.unsupported = true;   // state / nbt / components
                    }
                }
            } else if (key == "can_see_sky" && value.is_boolean()) {
                p.canSeeSky = value.get<bool>();
            } else {
                p.unsupported = true;   // position, biomes, structures, light, fluid ...
            }
        }
        return p;
    }

    bool EnchantmentEntityPredicate::Matches(const EnchantmentContext& ctx,
                                             const EnchantmentEntityFacts& f) const {
        if (unsupported) return false;
        if (hasType && !HolderSetContains(types, DataTags::Registry::EntityType, f.typeId)) return false;

        if (onGround   && f.onGround   != *onGround)   return false;
        if (onFire     && f.onFire     != *onFire)     return false;
        if (sneaking   && f.crouching  != *sneaking)   return false;
        if (sprinting  && f.sprinting  != *sprinting)  return false;
        if (swimming   && f.swimming   != *swimming)   return false;
        if (flying     && f.flying     != *flying)     return false;
        if (inWater    && f.inWater    != *inWater)    return false;
        if (fallFlying && f.fallFlying != *fallFlying) return false;
        if (baby       && f.baby       != *baby)       return false;

        if (hasMovement) {
            // MovementPredicate.matches(entity): getKnownMovement * 20.
            const glm::dvec3 v = f.knownMovement * 20.0;
            if (!moveX.Matches(v.x) || !moveY.Matches(v.y) || !moveZ.Matches(v.z)) return false;
            if (!speed.MatchesSqr(v.x * v.x + v.y * v.y + v.z * v.z)) return false;
            if (!horizontalSpeed.MatchesSqr(v.x * v.x + v.z * v.z)) return false;
            if (!verticalSpeed.Matches(std::abs(v.y))) return false;
            if (!fallDistance.Matches(f.fallDistance)) return false;
        }
        if (!location.empty() && !location[0].Matches(ctx, f.position)) return false;
        if (!movementAffectedBy.empty()) {
            // MovementAffectedByPredicate: the centre of
            // getBlockPosBelowThatAffectsMyMovement (getOnPos(0.500001)).
            const glm::dvec3 on(std::floor(f.position.x) + 0.5,
                                std::floor(f.position.y - 0.500001) + 0.5,
                                std::floor(f.position.z) + 0.5);
            if (!movementAffectedBy[0].Matches(ctx, on)) return false;
        }
        if (periodicTick > 0 && f.tickCount % periodicTick != 0) return false;
        if (!vehicle.empty()) {
            // EntityPredicate.matches(level, pos, null) is false: no vehicle,
            // no match — `"vehicle": {}` asks "is riding anything".
            if (!f.hasVehicle) return false;
            const Entity* ride = f.entity ? f.entity->GetVehicle() : nullptr;
            if (ride) {
                if (!vehicle[0].Matches(ctx, EnchantmentEntityFacts::Of(*ride))) return false;
            } else if (!vehicle[0].IsEmpty()) {
                return false;   // a vehicle known only as a flag cannot answer more
            }
        }
        return true;
    }

    EnchantmentEntityPredicate EnchantmentEntityPredicate::Parse(const nlohmann::json& j) {
        EnchantmentEntityPredicate p;
        if (!j.is_object()) { p.unsupported = true; return p; }
        for (const auto& [key, value] : j.items()) {
            if (key == "type") {
                p.types = ReadHolderSet(value);
                p.hasType = true;
            } else if (key == "flags") {
                if (!value.is_object()) { p.unsupported = true; continue; }
                p.onGround   = ReadOptionalBool(value, "is_on_ground");
                p.onFire     = ReadOptionalBool(value, "is_on_fire");
                p.sneaking   = ReadOptionalBool(value, "is_sneaking");
                p.sprinting  = ReadOptionalBool(value, "is_sprinting");
                p.swimming   = ReadOptionalBool(value, "is_swimming");
                p.flying     = ReadOptionalBool(value, "is_flying");
                p.baby       = ReadOptionalBool(value, "is_baby");
                p.inWater    = ReadOptionalBool(value, "is_in_water");
                p.fallFlying = ReadOptionalBool(value, "is_fall_flying");
            } else if (key == "location") {
                p.location.push_back(EnchantmentLocationPredicate::Parse(value));
            } else if (key == "movement_affected_by") {
                p.movementAffectedBy.push_back(EnchantmentLocationPredicate::Parse(value));
            } else if (key == "movement") {
                if (!value.is_object()) { p.unsupported = true; continue; }
                p.hasMovement = true;
                if (value.contains("x"))                p.moveX           = DoubleBounds::Parse(value["x"]);
                if (value.contains("y"))                p.moveY           = DoubleBounds::Parse(value["y"]);
                if (value.contains("z"))                p.moveZ           = DoubleBounds::Parse(value["z"]);
                if (value.contains("speed"))            p.speed           = DoubleBounds::Parse(value["speed"]);
                if (value.contains("horizontal_speed")) p.horizontalSpeed = DoubleBounds::Parse(value["horizontal_speed"]);
                if (value.contains("vertical_speed"))   p.verticalSpeed   = DoubleBounds::Parse(value["vertical_speed"]);
                if (value.contains("fall_distance"))    p.fallDistance    = DoubleBounds::Parse(value["fall_distance"]);
            } else if (key == "periodic_tick" && value.is_number_integer()) {
                p.periodicTick = value.get<int>();
            } else if (key == "vehicle") {
                p.vehicle.push_back(Parse(value));
            } else {
                // type_specific, equipment, effects, nbt, stepping_on,
                // distance, passenger, team, slots, components ...
                p.unsupported = true;
            }
        }
        return p;
    }

    float EnchantmentNumberProvider::GetFloat(const EnchantmentContext& ctx) const {
        switch (type) {
            case Type::Constant:
                return value;
            case Type::Uniform:
                // UniformGenerator.getFloat: Mth.nextFloat(random, min, max).
                if (!ctx.random || min >= max) return min;
                return ctx.random->NextFloat() * (max - min) + min;
            case Type::EnchantmentLevel:
                return amount.Calculate(ctx.enchantmentLevel);
            case Type::Unsupported:
                return 0.0f;
        }
        return 0.0f;
    }

    EnchantmentNumberProvider EnchantmentNumberProvider::Parse(const nlohmann::json& j) {
        EnchantmentNumberProvider n;
        if (j.is_number()) { n.value = j.get<float>(); return n; }
        if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) {
            n.type = Type::Unsupported;
            return n;
        }
        const std::string_view type = StripDefaultNamespace(j["type"].get_ref<const std::string&>());
        if (type == "constant" && j.contains("value") && j["value"].is_number()) {
            n.value = j["value"].get<float>();
        } else if (type == "uniform" && j.contains("min") && j["min"].is_number() &&
                   j.contains("max") && j["max"].is_number()) {
            n.type = Type::Uniform;
            n.min = j["min"].get<float>();
            n.max = j["max"].get<float>();
        } else if (type == "enchantment_level" && ReadLevelBased(j, "amount", n.amount)) {
            n.type = Type::EnchantmentLevel;
        } else {
            n.type = Type::Unsupported;
        }
        return n;
    }

    // ── Conditions ──────────────────────────────────────────────────────────

    bool EnchantmentCondition::Test(const EnchantmentContext& ctx) const {
        switch (type) {
            case Type::MatchTool:
                // MatchTool: the predicate's `items` HolderSet (an absent
                // tool never matches).
                return ctx.tool && !ctx.tool->IsEmpty() && ItemHolderSetContains(items, ctx.tool->itemId);
            case Type::Inverted:
                return !terms.empty() && !terms[0].Test(ctx);
            case Type::AllOf:
                for (const auto& t : terms) if (!t.Test(ctx)) return false;
                return true;
            case Type::AnyOf:
                for (const auto& t : terms) if (t.Test(ctx)) return true;
                return false;
            case Type::EntityProperties: {
                // LootItemEntityPropertyCondition: the named entity must be
                // present (EntityPredicate.matches(..., null) is false).
                if (entity == EntityTarget::This && ctx.thisFacts) {
                    return entityPredicate.Matches(ctx, *ctx.thisFacts);
                }
                const Entity* e = ResolveEntity(ctx, entity);
                if (!e) return false;
                return entityPredicate.Matches(ctx, EnchantmentEntityFacts::Of(*e));
            }
            case Type::DamageSourceProperties: {
                // DamageSourceCondition: needs the source (and ORIGIN, which
                // a damage context always carries).
                if (!ctx.damage) return false;
                for (const auto& [tag, expected] : damageTags) {
                    if (ctx.damage->Is(tag) != expected) return false;
                }
                if (!directEntity.empty()) {
                    if (!ctx.damage->direct ||
                        !directEntity[0].Matches(ctx, EnchantmentEntityFacts::Of(*ctx.damage->direct))) {
                        return false;
                    }
                }
                if (!sourceEntity.empty()) {
                    if (!ctx.damage->causing ||
                        !sourceEntity[0].Matches(ctx, EnchantmentEntityFacts::Of(*ctx.damage->causing))) {
                        return false;
                    }
                }
                if (isDirect && ctx.damage->IsDirect() != *isDirect) return false;
                return true;
            }
            case Type::RandomChance:
                // LootItemRandomChanceCondition: nextFloat() < chance.
                // The probability is read first (a uniform one draws too).
                if (chance.type == EnchantmentNumberProvider::Type::Unsupported || !ctx.random) return false;
                {
                    const float probability = chance.GetFloat(ctx);
                    return ctx.random->NextFloat() < probability;
                }
            case Type::EnchantmentActiveCheck:
                return ctx.enchantmentActive >= 0 && (ctx.enchantmentActive != 0) == active;
            case Type::WeatherCheck:
                // Only `thundering` is answerable: no level here exposes rain
                // on its own.
                if (raining) return false;
                if (thundering) return ctx.level && ctx.level->IsThundering() == *thundering;
                return true;
            case Type::LocationCheck:
                return location.Matches(ctx, ctx.origin + glm::dvec3(offset));
            case Type::Unsupported:
                return false;
        }
        return false;
    }

    EnchantmentCondition EnchantmentCondition::Parse(const nlohmann::json& j) {
        EnchantmentCondition out;
        if (j.is_array()) {                        // the list form: all of them
            out.type = Type::AllOf;
            for (const auto& t : j) out.terms.push_back(Parse(t));
            return out;
        }
        if (!j.is_object() || !j.contains("condition") || !j["condition"].is_string()) return out;
        const std::string_view cond = StripDefaultNamespace(j["condition"].get_ref<const std::string&>());
        if (cond == "match_tool") {
            // ItemPredicate: only an `items` set is modelled; a predicate that
            // also asks for counts, components or sub-predicates stays
            // Unsupported.
            if (!j.contains("predicate") || !j["predicate"].is_object()) return out;
            const auto& predicate = j["predicate"];
            if (predicate.size() != 1 || !predicate.contains("items")) return out;
            out.type = Type::MatchTool;
            out.items = ReadHolderSet(predicate["items"]);
            return out;
        }
        if (cond == "inverted") {
            if (!j.contains("term")) return out;
            out.type = Type::Inverted;
            out.terms.push_back(Parse(j["term"]));
            return out;
        }
        if (cond == "all_of" || cond == "any_of") {
            if (!j.contains("terms") || !j["terms"].is_array()) return out;
            out.type = cond == "all_of" ? Type::AllOf : Type::AnyOf;
            for (const auto& t : j["terms"]) out.terms.push_back(Parse(t));
            return out;
        }
        if (cond == "entity_properties") {
            const std::string target = j.value("entity", std::string("this"));
            if (target == "this")                 out.entity = EntityTarget::This;
            else if (target == "attacker")        out.entity = EntityTarget::Attacker;
            else if (target == "direct_attacker") out.entity = EntityTarget::DirectAttacker;
            else if (target == "attacking_player") out.entity = EntityTarget::AttackingPlayer;
            else return out;
            out.type = Type::EntityProperties;
            out.entityPredicate = j.contains("predicate") ? EnchantmentEntityPredicate::Parse(j["predicate"])
                                                          : EnchantmentEntityPredicate{};
            return out;
        }
        if (cond == "damage_source_properties") {
            out.type = Type::DamageSourceProperties;
            if (!j.contains("predicate")) return out;
            const auto& predicate = j["predicate"];
            if (!predicate.is_object()) { out.type = Type::Unsupported; return out; }
            for (const auto& [key, value] : predicate.items()) {
                if (key == "tags" && value.is_array()) {
                    for (const auto& t : value) {
                        if (!t.is_object() || !t.contains("id") || !t["id"].is_string()) continue;
                        out.damageTags.emplace_back(t["id"].get<std::string>(), t.value("expected", true));
                    }
                } else if (key == "is_direct" && value.is_boolean()) {
                    out.isDirect = value.get<bool>();
                } else if (key == "direct_entity") {
                    out.directEntity.push_back(EnchantmentEntityPredicate::Parse(value));
                } else if (key == "source_entity") {
                    out.sourceEntity.push_back(EnchantmentEntityPredicate::Parse(value));
                } else {
                    out.type = Type::Unsupported;
                }
            }
            return out;
        }
        if (cond == "random_chance") {
            if (!j.contains("chance")) return out;
            out.type = Type::RandomChance;
            out.chance = EnchantmentNumberProvider::Parse(j["chance"]);
            return out;
        }
        if (cond == "enchantment_active_check") {
            out.type = Type::EnchantmentActiveCheck;
            out.active = j.value("active", false);
            return out;
        }
        if (cond == "weather_check") {
            out.type = Type::WeatherCheck;
            out.raining = ReadOptionalBool(j, "raining");
            out.thundering = ReadOptionalBool(j, "thundering");
            return out;
        }
        if (cond == "location_check") {
            out.type = Type::LocationCheck;
            out.offset = ReadVec3i(j, "offset");   // BlockPos.CODEC: [x, y, z]
            out.location = j.contains("predicate") ? EnchantmentLocationPredicate::Parse(j["predicate"])
                                                   : EnchantmentLocationPredicate{};
            return out;
        }
        return out;
    }

    // ── Attribute effects ───────────────────────────────────────────────────

    AttributeModifier EnchantmentAttributeEffect::GetModifier(int level, EquipmentSlot slot) const {
        AttributeModifier m;
        m.id = static_cast<uint32_t>(EnchantmentModifierId(id + "/" + SlotName(slot),
                                                           static_cast<uint8_t>(slot)));
        m.amount = static_cast<double>(amount.Calculate(level));
        m.operation = operation;
        return m;
    }

    bool EnchantmentAttributeEffect::Parse(const nlohmann::json& j, EnchantmentAttributeEffect& out) {
        out = EnchantmentAttributeEffect{};
        if (!j.is_object() || !j.contains("attribute") || !j["attribute"].is_string()) return false;
        const std::string_view name = StripDefaultNamespace(j["attribute"].get_ref<const std::string&>());
        for (size_t i = 0; i < static_cast<size_t>(Attribute::Count); ++i) {
            if (kAttributeTable[i].name == name) { out.attribute = static_cast<Attribute>(i); break; }
        }
        out.id = j.value("id", std::string());
        const std::string op = j.value("operation", std::string("add_value"));
        if (op == "add_multiplied_base")       out.operation = AttributeOperation::AddMultipliedBase;
        else if (op == "add_multiplied_total") out.operation = AttributeOperation::AddMultipliedTotal;
        else                                   out.operation = AttributeOperation::AddValue;
        if (!ReadLevelBased(j, "amount", out.amount)) return false;
        return out.Valid();
    }

    // ── Entity effects ──────────────────────────────────────────────────────

    void EnchantmentEntityEffect::Apply(EntityLevel& level, int enchantmentLevel,
                                        const EnchantedItemInUse& item, Entity& entity,
                                        const glm::dvec3& position) const {
        if (level.IsClientSide()) return;
        JavaRandom& random = level.Random();
        switch (type) {
            case Type::AllOf:
                for (const EnchantmentEntityEffect& e : effects) e.Apply(level, enchantmentLevel, item, entity, position);
                return;

            case Type::ApplyMobEffect: {
                LivingEntity* living = entity.AsLiving();
                if (!living || mobEffects.empty()) return;
                // HolderSet.getRandomElement: one draw over the set.
                const std::string& pick = mobEffects[static_cast<size_t>(
                    random.NextInt(static_cast<int>(mobEffects.size())))];
                MobEffectId id;
                if (!ParseEffectId(pick, id)) return;
                const int ticks = RoundFloat(RandomBetween(random, minDuration.Calculate(enchantmentLevel),
                                                           maxDuration.Calculate(enchantmentLevel)) * 20.0f);
                const int amplifier = std::max(0, RoundFloat(RandomBetween(
                    random, minAmplifier.Calculate(enchantmentLevel), maxAmplifier.Calculate(enchantmentLevel))));
                // addEffect(instance, item.owner()): the owner is the source.
                living->AddEffect(MobEffectInstance(id, ticks, amplifier), item.owner);
                return;
            }

            case Type::Ignite:
                entity.IgniteForSeconds(a.Calculate(enchantmentLevel));
                return;

            case Type::DamageEntity: {
                LivingEntity* living = entity.AsLiving();
                const float damage = RandomBetween(random, a.Calculate(enchantmentLevel),
                                                   b.Calculate(enchantmentLevel));
                // new DamageSource(damageType, item.owner()): the owner is
                // both the causing and the direct entity.
                if (living) living->Hurt(MobSourceForDamageType(damageType), damage, item.owner);
                return;
            }

            case Type::ChangeItemDamage: {
                ItemStack* stack = item.stack;
                if (!stack || stack->IsEmpty() || !IsDamageableItem(*stack)) return;
                const int change = static_cast<int>(a.Calculate(enchantmentLevel));
                // hurtAndBreak(change, serverLevel, player, onBreak): only a
                // player owner can have infinite materials.
                const bool infinite = item.owner && item.owner->IsPlayer() && item.owner->IsCreative();
                if (item.onBreak) {
                    HurtAndBreak(*stack, change, random, infinite, item.onBreak);
                } else {
                    LivingEntity* owner = item.owner;
                    const EquipmentSlot slot = item.slot.value_or(EquipmentSlot::MAINHAND);
                    HurtAndBreak(*stack, change, random, infinite, [owner, slot](const ItemStack& broken) {
                        if (owner) owner->OnEquippedItemBroken(broken, slot);
                    });
                }
                return;
            }

            case Type::SummonEntity: {
                if (entityTypes.empty()) return;
                const std::string& pick = entityTypes[static_cast<size_t>(
                    random.NextInt(static_cast<int>(entityTypes.size())))];
                // EntityType.spawn(level, blockPos, TRIGGERED). The vanilla
                // data summons one type, the lightning bolt; the engine's
                // entity factory is server-side, so that is the one built.
                if (StripDefaultNamespace(pick) != "lightning_bolt") return;
                // spawn(level, BlockPos.containing(position)) then
                // snapTo(position): the bolt ends at the exact position.
                const glm::dvec3 at = position;
                if (at.y < level.GetMinY() || at.y > level.GetMaxY()) return;   // isInSpawnableBounds
                auto bolt = std::make_unique<LightningBolt>(&level);
                bolt->position = at;
                bolt->oldPosition = at;
                level.AddFreshEntity(std::move(bolt));
                return;
            }

            case Type::PlaySound: {
                if (entity.IsSilent() || sounds.empty()) return;
                const int index = std::clamp(enchantmentLevel - 1, 0, static_cast<int>(sounds.size()) - 1);
                const float volume = SampleFloat(random, volumeMin, volumeMax, volumeUniform);
                const float pitch  = SampleFloat(random, pitchMin, pitchMax, pitchUniform);
                level.PlaySound(nullptr, position, StripDefaultNamespace(sounds[static_cast<size_t>(index)]),
                                entity.GetSoundSource(), volume, pitch);
                return;
            }

            case Type::Explode: {
                ExplosionParams p;
                p.center = position + explodeOffset;
                p.radius = std::max(a.Calculate(enchantmentLevel), 0.0f);
                p.source = attributeToUser ? &entity : nullptr;
                p.attributedTo = p.source;
                p.interaction = InteractionByName(blockInteraction);
                p.fire = createFire;
                // SimpleExplosionDamageCalculator(explodesBlocks, damagesEntities
                // = damage_type present, knockbackMultiplier, immuneBlocks).
                p.damageEntities = !damageType.empty();
                p.knockbackMultiplier = hasKnockbackMultiplier
                    ? knockbackMultiplier.Calculate(enchantmentLevel) : 1.0f;
                // The engine's blast visual is the vanilla explosion pair; an
                // effect naming other particles (a wind burst's gusts, which
                // have no particle system here) goes without it.
                p.spawnVisual = vanillaExplosionParticles;
                if (!explosionSound.empty()) p.explosionSound = explosionSound.c_str();
                Explode(level, p);
                return;
            }

            case Type::ApplyExhaustion:
                // ApplyExhaustion: players only (causeFoodExhaustion is a
                // no-op on everything else).
                if (LivingEntity* living = entity.AsLiving()) {
                    living->CauseFoodExhaustion(a.Calculate(enchantmentLevel));
                }
                return;

            case Type::ReplaceDisk: {
                ILevelWrite* blocks = level.MutableBlocks();
                if (!blocks || !hasDiskState) return;
                const glm::ivec3 center = glm::ivec3(static_cast<int>(std::floor(position.x)),
                                                     static_cast<int>(std::floor(position.y)),
                                                     static_cast<int>(std::floor(position.z))) + diskOffset;
                const int dist   = static_cast<int>(a.Calculate(enchantmentLevel));
                const int height = static_cast<int>(b.Calculate(enchantmentLevel));
                const int yTop   = std::min(height - 1, 0);
                const double distSq = static_cast<double>(dist) * static_cast<double>(dist);
                // BlockPos.betweenClosed(center + (-d, 0, -d), center + (d, min(h - 1, 0), d)).
                for (int y = center.y + std::min(0, yTop); y <= center.y + std::max(0, yTop); ++y) {
                    for (int z = center.z - dist; z <= center.z + dist; ++z) {
                        for (int x = center.x - dist; x <= center.x + dist; ++x) {
                            // pos.distToCenterSqr(position.x, pos.y + 0.5, position.z).
                            const double dx = x + 0.5 - position.x;
                            const double dz = z + 0.5 - position.z;
                            if (dx * dx + dz * dz >= distSq) continue;
                            const glm::ivec3 pos(x, y, z);
                            if (!diskPredicate.empty() && !TestBlockPredicate(diskPredicate[0], level, pos)) continue;
                            // setBlockAndUpdate (UPDATE_ALL). The block_place
                            // game event has no vibration system to reach.
                            blocks->SetBlock(x, y, z, diskState, World::UpdateFlags::All);
                        }
                    }
                }
                return;
            }

            case Type::SpawnParticles:
                // SpawnParticlesEffect is a server → client particle burst
                // (the soul speed wisps); this engine has no particle packet
                // for it, so the server has nothing to send.
                return;

            case Type::Attribute:
            case Type::Unsupported:
                return;
        }
    }

    void EnchantmentEntityEffect::OnChangedBlock(EntityLevel* level, int enchantmentLevel,
                                                 const EnchantedItemInUse& item, Entity* entity,
                                                 AttributeMap* attributes, const glm::dvec3& position,
                                                 bool becameActive) const {
        switch (type) {
            case Type::AllOf:
                // AllOf.LocationBasedEffects: every child in turn.
                for (const EnchantmentEntityEffect& e : effects) {
                    e.OnChangedBlock(level, enchantmentLevel, item, entity, attributes, position, becameActive);
                }
                return;
            case Type::Attribute:
                // EnchantmentAttributeEffect.onChangedBlock: the modifier
                // goes on when the effect becomes active.
                if (becameActive && attributes && attribute.Valid() && item.slot) {
                    const AttributeModifier m = attribute.GetModifier(enchantmentLevel, *item.slot);
                    attributes->RemoveModifier(attribute.attribute, static_cast<ModifierId>(m.id));
                    attributes->AddModifier(attribute.attribute, m);
                }
                return;
            default:
                // EnchantmentEntityEffect.onChangedBlock: apply, every time.
                if (level && entity) Apply(*level, enchantmentLevel, item, *entity, position);
                return;
        }
    }

    void EnchantmentEntityEffect::OnDeactivated(const EnchantedItemInUse& item, AttributeMap* attributes,
                                                int enchantmentLevel) const {
        switch (type) {
            case Type::AllOf:
                for (const EnchantmentEntityEffect& e : effects) e.OnDeactivated(item, attributes, enchantmentLevel);
                return;
            case Type::Attribute:
                if (attributes && attribute.Valid() && item.slot) {
                    const AttributeModifier m = attribute.GetModifier(enchantmentLevel, *item.slot);
                    attributes->RemoveModifier(attribute.attribute, static_cast<ModifierId>(m.id));
                }
                return;
            default:
                return;   // an entity effect has nothing to undo
        }
    }

    EnchantmentEntityEffect EnchantmentEntityEffect::Parse(const nlohmann::json& j, bool locationBased) {
        EnchantmentEntityEffect e;
        if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) return e;
        e.typeName = j["type"].get<std::string>();
        const std::string_view type = StripDefaultNamespace(e.typeName);
        if (type == "all_of") {
            e.type = Type::AllOf;
            if (j.contains("effects") && j["effects"].is_array()) {
                for (const auto& child : j["effects"]) e.effects.push_back(Parse(child, locationBased));
            }
        } else if (type == "apply_mob_effect") {
            e.type = Type::ApplyMobEffect;
            if (j.contains("to_apply")) e.mobEffects = ReadHolderSet(j["to_apply"]);
            const bool ok = ReadLevelBased(j, "min_duration", e.minDuration) &&
                            ReadLevelBased(j, "max_duration", e.maxDuration) &&
                            ReadLevelBased(j, "min_amplifier", e.minAmplifier) &&
                            ReadLevelBased(j, "max_amplifier", e.maxAmplifier);
            if (!ok) e.type = Type::Unsupported;
        } else if (type == "ignite") {
            e.type = ReadLevelBased(j, "duration", e.a) ? Type::Ignite : Type::Unsupported;
        } else if (type == "damage_entity") {
            e.type = (ReadLevelBased(j, "min_damage", e.a) && ReadLevelBased(j, "max_damage", e.b))
                   ? Type::DamageEntity : Type::Unsupported;
            e.damageType = j.value("damage_type", std::string("minecraft:generic"));
        } else if (type == "change_item_damage") {
            e.type = ReadLevelBased(j, "amount", e.a) ? Type::ChangeItemDamage : Type::Unsupported;
        } else if (type == "summon_entity") {
            e.type = Type::SummonEntity;
            if (j.contains("entity")) e.entityTypes = ReadHolderSet(j["entity"]);
        } else if (type == "play_sound") {
            e.type = Type::PlaySound;
            if (j.contains("sound")) e.sounds = ReadHolderSet(j["sound"]);
            if (j.contains("volume")) ReadFloatProvider(j["volume"], e.volumeMin, e.volumeMax, e.volumeUniform);
            if (j.contains("pitch"))  ReadFloatProvider(j["pitch"], e.pitchMin, e.pitchMax, e.pitchUniform);
        } else if (type == "explode") {
            e.type = ReadLevelBased(j, "radius", e.a) ? Type::Explode : Type::Unsupported;
            e.attributeToUser = j.value("attribute_to_user", false);
            e.damageType = j.value("damage_type", std::string());
            e.hasKnockbackMultiplier = ReadLevelBased(j, "knockback_multiplier", e.knockbackMultiplier);
            e.createFire = j.value("create_fire", false);
            e.blockInteraction = j.value("block_interaction", std::string("none"));
            if (j.contains("sound") && j["sound"].is_string()) {
                e.explosionSound = std::string(StripDefaultNamespace(j["sound"].get_ref<const std::string&>()));
            }
            const auto particleType = [&j](const char* key) {
                if (!j.contains(key) || !j[key].is_object()) return std::string();
                return j[key].value("type", std::string());
            };
            e.vanillaExplosionParticles =
                StripDefaultNamespace(particleType("small_particle")) == "explosion" &&
                StripDefaultNamespace(particleType("large_particle")) == "explosion_emitter";
            if (j.contains("offset") && j["offset"].is_array() && j["offset"].size() == 3) {
                for (int i = 0; i < 3; ++i) {
                    if (j["offset"][i].is_number()) e.explodeOffset[i] = j["offset"][i].get<double>();
                }
            }
        } else if (type == "apply_exhaustion") {
            e.type = ReadLevelBased(j, "amount", e.a) ? Type::ApplyExhaustion : Type::Unsupported;
        } else if (type == "replace_disk") {
            e.type = (ReadLevelBased(j, "radius", e.a) && ReadLevelBased(j, "height", e.b))
                   ? Type::ReplaceDisk : Type::Unsupported;
            e.diskOffset = ReadVec3i(j, "offset");
            if (j.contains("predicate")) e.diskPredicate.push_back(ParseBlockPredicate(j["predicate"]));
            // BlockStateProvider: only simple_state_provider is modelled.
            if (j.contains("block_state") && j["block_state"].is_object()) {
                const auto& provider = j["block_state"];
                const std::string providerType = provider.value("type", std::string());
                if (StripDefaultNamespace(providerType) == "simple_state_provider" && provider.contains("state")) {
                    e.hasDiskState = ReadBlockStateJson(provider["state"], e.diskState);
                }
            }
            if (!e.hasDiskState) e.type = Type::Unsupported;
        } else if (type == "spawn_particles") {
            e.type = Type::SpawnParticles;
        } else if (type == "attribute" && locationBased) {
            e.type = EnchantmentAttributeEffect::Parse(j, e.attribute) ? Type::Attribute : Type::Unsupported;
        }
        // apply_impulse (the spear's lunge), replace_block,
        // set_block_properties and run_function stay Unsupported: nothing in
        // this engine can reach them (no spear, no functions).
        return e;
    }

    // ── Components ──────────────────────────────────────────────────────────

    namespace {

        std::vector<ConditionalValueEffect> ParseValueList(const nlohmann::json& list, std::string_view owner) {
            std::vector<ConditionalValueEffect> out;
            if (!list.is_array()) return out;
            for (const auto& entry : list) {
                if (!entry.is_object() || !entry.contains("effect")) continue;
                ConditionalValueEffect c;
                if (!EnchantmentValueEffect::Parse(entry["effect"], c.effect)) {
                    Log::Warning("[Enchantments] %.*s: unreadable value effect %s",
                                 static_cast<int>(owner.size()), owner.data(), entry["effect"].dump().c_str());
                    continue;
                }
                if (entry.contains("requirements")) {
                    c.hasRequirements = true;
                    c.requirements = EnchantmentCondition::Parse(entry["requirements"]);
                }
                out.push_back(std::move(c));
            }
            return out;
        }

        EnchantmentTarget TargetByName(const std::string& name) {
            if (name == "victim")          return EnchantmentTarget::Victim;
            if (name == "damaging_entity") return EnchantmentTarget::DamagingEntity;
            return EnchantmentTarget::Attacker;
        }

        std::vector<ConditionalEntityEffect> ParseEntityList(const nlohmann::json& list, bool locationBased,
                                                             std::string_view owner) {
            std::vector<ConditionalEntityEffect> out;
            if (!list.is_array()) return out;
            for (const auto& entry : list) {
                if (!entry.is_object() || !entry.contains("effect")) continue;
                ConditionalEntityEffect c;
                c.effect = EnchantmentEntityEffect::Parse(entry["effect"], locationBased);
                if (c.effect.type == EnchantmentEntityEffect::Type::Unsupported) {
                    Log::Info("[Enchantments] %.*s: effect %s has no engine counterpart",
                              static_cast<int>(owner.size()), owner.data(), c.effect.typeName.c_str());
                }
                if (entry.contains("requirements")) {
                    c.hasRequirements = true;
                    c.requirements = EnchantmentCondition::Parse(entry["requirements"]);
                }
                out.push_back(std::move(c));
            }
            return out;
        }

        std::optional<EnchantmentValueEffect> ParseUnfiltered(const nlohmann::json& j) {
            EnchantmentValueEffect e;
            if (!EnchantmentValueEffect::Parse(j, e)) return std::nullopt;
            return e;
        }

    } // namespace

    EnchantmentEffectComponents EnchantmentEffectComponents::Parse(const nlohmann::json& effects,
                                                                   std::string_view owner) {
        EnchantmentEffectComponents c;
        if (!effects.is_object()) return c;
        for (const auto& [rawKey, value] : effects.items()) {
            const std::string_view key = StripDefaultNamespace(rawKey);
            if      (key == "item_damage")                   c.itemDamage = ParseValueList(value, owner);
            else if (key == "damage")                        c.damage = ParseValueList(value, owner);
            else if (key == "damage_protection")             c.damageProtection = ParseValueList(value, owner);
            else if (key == "knockback")                     c.knockback = ParseValueList(value, owner);
            else if (key == "armor_effectiveness")           c.armorEffectiveness = ParseValueList(value, owner);
            else if (key == "smash_damage_per_fallen_block") c.smashDamagePerFallenBlock = ParseValueList(value, owner);
            else if (key == "block_experience")              c.blockExperience = ParseValueList(value, owner);
            else if (key == "mob_experience")                c.mobExperience = ParseValueList(value, owner);
            else if (key == "repair_with_xp")                c.repairWithXp = ParseValueList(value, owner);
            else if (key == "ammo_use")                      c.ammoUse = ParseValueList(value, owner);
            else if (key == "projectile_piercing")           c.projectilePiercing = ParseValueList(value, owner);
            else if (key == "projectile_count")              c.projectileCount = ParseValueList(value, owner);
            else if (key == "projectile_spread")             c.projectileSpread = ParseValueList(value, owner);
            else if (key == "trident_return_acceleration")   c.tridentReturnAcceleration = ParseValueList(value, owner);
            else if (key == "fishing_time_reduction")        c.fishingTimeReduction = ParseValueList(value, owner);
            else if (key == "fishing_luck_bonus")            c.fishingLuckBonus = ParseValueList(value, owner);
            else if (key == "equipment_drops") {
                // TargetedConditionalEffect.equipmentDropsCodec: `enchanted`
                // only; the affected side is always the victim.
                if (!value.is_array()) continue;
                for (const auto& entry : value) {
                    if (!entry.is_object() || !entry.contains("effect")) continue;
                    TargetedConditionalValueEffect t;
                    if (!EnchantmentValueEffect::Parse(entry["effect"], t.effect)) continue;
                    t.enchanted = TargetByName(entry.value("enchanted", std::string("attacker")));
                    t.affected  = EnchantmentTarget::Victim;
                    if (entry.contains("requirements")) {
                        t.hasRequirements = true;
                        t.requirements = EnchantmentCondition::Parse(entry["requirements"]);
                    }
                    c.equipmentDrops.push_back(std::move(t));
                }
            }
            else if (key == "crossbow_charge_time")          c.crossbowChargeTime = ParseUnfiltered(value);
            else if (key == "trident_spin_attack_strength")  c.tridentSpinAttackStrength = ParseUnfiltered(value);
            else if (key == "post_attack") {
                if (!value.is_array()) continue;
                for (const auto& entry : value) {
                    if (!entry.is_object() || !entry.contains("effect")) continue;
                    TargetedConditionalEntityEffect t;
                    t.effect = EnchantmentEntityEffect::Parse(entry["effect"], false);
                    t.enchanted = TargetByName(entry.value("enchanted", std::string("attacker")));
                    t.affected  = TargetByName(entry.value("affected", std::string("victim")));
                    if (entry.contains("requirements")) {
                        t.hasRequirements = true;
                        t.requirements = EnchantmentCondition::Parse(entry["requirements"]);
                    }
                    c.postAttack.push_back(std::move(t));
                }
            }
            else if (key == "post_piercing_attack")          c.postPiercingAttack = ParseEntityList(value, false, owner);
            else if (key == "hit_block")                     c.hitBlock = ParseEntityList(value, false, owner);
            else if (key == "tick")                          c.tick = ParseEntityList(value, false, owner);
            else if (key == "projectile_spawned")            c.projectileSpawned = ParseEntityList(value, false, owner);
            else if (key == "location_changed")              c.locationChanged = ParseEntityList(value, true, owner);
            else if (key == "damage_immunity") {
                c.hasDamageImmunity = true;
                if (!value.is_array()) continue;
                for (const auto& entry : value) {
                    // An entry without requirements is unconditional immunity.
                    EnchantmentCondition cond;
                    if (entry.is_object() && entry.contains("requirements")) {
                        cond = EnchantmentCondition::Parse(entry["requirements"]);
                    } else {
                        cond.type = EnchantmentCondition::Type::AllOf;   // no terms: true
                    }
                    c.damageImmunity.push_back(std::move(cond));
                }
            }
            else if (key == "attributes") {
                if (!value.is_array()) continue;
                for (const auto& entry : value) {
                    EnchantmentAttributeEffect a;
                    if (EnchantmentAttributeEffect::Parse(entry, a)) {
                        c.attributes.push_back(std::move(a));
                    } else {
                        Log::Warning("[Enchantments] %.*s: attribute effect on an attribute the engine "
                                     "does not model: %s", static_cast<int>(owner.size()), owner.data(),
                                     entry.dump().c_str());
                    }
                }
            }
            else if (key == "prevent_equipment_drop")        c.preventEquipmentDrop = true;
            else if (key == "prevent_armor_change")          c.preventArmorChange = true;
            else if (key == "trident_sound")                 c.tridentSound = ReadHolderSet(value);
            else if (key == "crossbow_charging_sounds") {
                if (!value.is_array()) continue;
                for (const auto& entry : value) {
                    if (entry.is_object() && entry.contains("start") && entry["start"].is_string()) {
                        c.crossbowChargingSounds.push_back(entry["start"].get<std::string>());
                    }
                }
            }
            else {
                Log::Warning("[Enchantments] %.*s: unknown effect component %s",
                             static_cast<int>(owner.size()), owner.data(), rawKey.c_str());
            }
        }
        return c;
    }

} // namespace Game
