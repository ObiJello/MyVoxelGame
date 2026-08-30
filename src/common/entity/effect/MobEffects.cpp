// File: src/common/entity/effect/MobEffects.cpp
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/EntityType.hpp"

#include <algorithm>

namespace Game {

    // ── Registry ───────────────────────────────────────────────────────────

    MobEffectCategory GetEffectCategory(MobEffectId id) {
        // MC MobEffects.java registration order; GLOWING is the one NEUTRAL.
        switch (id) {
            case MobEffectId::Slowness:
            case MobEffectId::MiningFatigue:
            case MobEffectId::InstantDamage:
            case MobEffectId::Hunger:
            case MobEffectId::Weakness:
            case MobEffectId::Poison:
            case MobEffectId::Wither:
            case MobEffectId::Levitation:
            case MobEffectId::Blindness:
                return MobEffectCategory::Harmful;
            case MobEffectId::Glowing:
                return MobEffectCategory::Neutral;
            default:
                return MobEffectCategory::Beneficial;
        }
    }

    bool IsInstantenousEffect(MobEffectId id) {
        // MC InstantenousMobEffect subclasses: HealOrHarm and Saturation.
        return id == MobEffectId::InstantHealth ||
               id == MobEffectId::InstantDamage ||
               id == MobEffectId::Saturation;
    }

    const char* GetEffectName(MobEffectId id) {
        switch (id) {
            case MobEffectId::Speed:          return "speed";
            case MobEffectId::Slowness:       return "slowness";
            case MobEffectId::Haste:          return "haste";
            case MobEffectId::MiningFatigue:  return "mining_fatigue";
            case MobEffectId::Strength:       return "strength";
            case MobEffectId::InstantHealth:  return "instant_health";
            case MobEffectId::InstantDamage:  return "instant_damage";
            case MobEffectId::JumpBoost:      return "jump_boost";
            case MobEffectId::Regeneration:   return "regeneration";
            case MobEffectId::Resistance:     return "resistance";
            case MobEffectId::FireResistance: return "fire_resistance";
            case MobEffectId::WaterBreathing: return "water_breathing";
            case MobEffectId::Invisibility:   return "invisibility";
            case MobEffectId::Hunger:         return "hunger";
            case MobEffectId::Weakness:       return "weakness";
            case MobEffectId::Poison:         return "poison";
            case MobEffectId::Wither:         return "wither";
            case MobEffectId::Glowing:        return "glowing";
            case MobEffectId::Levitation:     return "levitation";
            case MobEffectId::SlowFalling:    return "slow_falling";
            case MobEffectId::Saturation:     return "saturation";
            case MobEffectId::Blindness:      return "blindness";
            default:                          return "unknown";
        }
    }

    // ── Tick cadence (MC MobEffect.shouldApplyEffectTickThisTick) ──────────

    bool ShouldApplyEffectTickThisTick(MobEffectId id, int tickCount, int amplifier) {
        switch (id) {
            case MobEffectId::Regeneration: {
                // RegenerationMobEffect: every 50 >> amp ticks; every tick once
                // the shift reaches zero.
                const int interval = 50 >> amplifier;
                return interval > 0 ? tickCount % interval == 0 : true;
            }
            case MobEffectId::Poison: {
                const int interval = 25 >> amplifier;   // PoisonMobEffect
                return interval > 0 ? tickCount % interval == 0 : true;
            }
            case MobEffectId::Wither: {
                const int interval = 40 >> amplifier;   // WitherMobEffect
                return interval > 0 ? tickCount % interval == 0 : true;
            }
            case MobEffectId::Hunger:
                return true;                             // HungerMobEffect
            case MobEffectId::InstantHealth:
            case MobEffectId::InstantDamage:
            case MobEffectId::Saturation:
                // InstantenousMobEffect: fire exactly once, on the tick the
                // 1-tick duration is still standing.
                return tickCount >= 1;
            default:
                return false;
        }
    }

    // ── Per-tick application (MC *MobEffect.applyEffectTick) ───────────────

    bool ApplyEffectTick(LivingEntity& mob, MobEffectId id, int amplifier) {
        switch (id) {
            case MobEffectId::Regeneration:
                if (mob.GetHealth() < mob.GetMaxHealth()) mob.Heal(1.0f);
                return true;
            case MobEffectId::Poison:
                // PoisonMobEffect: cannot kill — the hurt is skipped at 1 HP.
                if (mob.GetHealth() > 1.0f) {
                    mob.Hurt(MobDamageSource::Magic, 1.0f, nullptr);
                }
                return true;
            case MobEffectId::Wither:
                // WitherMobEffect: CAN kill.
                mob.Hurt(MobDamageSource::Wither, 1.0f, nullptr);
                return true;
            case MobEffectId::InstantHealth:
            case MobEffectId::InstantDamage: {
                // HealOrHarmMobEffect.applyEffectTick — the drink/direct path
                // (no distance scale).
                const bool isHarm = id == MobEffectId::InstantDamage;
                if (isHarm == mob.IsInvertedHealAndHarm()) {
                    mob.Heal(static_cast<float>(std::max(4 << amplifier, 0)));
                } else {
                    mob.Hurt(MobDamageSource::Magic,
                             static_cast<float>(6 << amplifier), nullptr);
                }
                return true;
            }
            case MobEffectId::Hunger:
                // HungerMobEffect — player-only; the hook is a no-op on mobs.
                mob.CauseFoodExhaustion(0.005f * static_cast<float>(amplifier + 1));
                return true;
            case MobEffectId::Saturation:
                // SaturationMobEffect — player-only, same hook pattern.
                mob.EatFood(amplifier + 1, 1.0f);
                return true;
            default:
                return true;   // MobEffect base: nothing to do, keep the effect
        }
    }

    void ApplyInstantenousEffect(Entity* source, Entity* owner, LivingEntity& mob,
                                 MobEffectId id, int amplifier, double scale) {
        switch (id) {
            case MobEffectId::InstantHealth:
            case MobEffectId::InstantDamage: {
                // HealOrHarmMobEffect.applyInstantenousEffect — the splash
                // falloff rounds through (int)(scale * base + 0.5).
                const bool isHarm = id == MobEffectId::InstantDamage;
                if (isHarm == mob.IsInvertedHealAndHarm()) {
                    const int amount = static_cast<int>(
                        scale * static_cast<double>(4 << amplifier) + 0.5);
                    mob.Heal(static_cast<float>(amount));
                } else {
                    const int amount = static_cast<int>(
                        scale * static_cast<double>(6 << amplifier) + 0.5);
                    // MC damageSources().indirectMagic(source, owner) — the
                    // attribution goes to the OWNER so retaliation targets the
                    // thrower, with the projectile as fallback.
                    mob.Hurt(MobDamageSource::Magic, static_cast<float>(amount),
                             owner ? owner : source);
                }
                break;
            }
            default:
                // InstantenousMobEffect base: applyInstantenousEffect falls
                // through to applyEffectTick (Saturation takes this path).
                ApplyEffectTick(mob, id, amplifier);
                break;
        }
    }

    // ── Attribute templates (MC MobEffects.java registrations) ─────────────

    namespace {
        struct EffectAttributeTemplate {
            Attribute          attribute;
            ModifierId         id;
            double             amount;      // per level; scaled by (amp + 1)
            AttributeOperation operation;
        };

        // MC MobEffect.AttributeTemplate.create(amplifier):
        //   amount * (amplifier + 1), same id every level.
        bool FindTemplate(MobEffectId id, EffectAttributeTemplate& out) {
            switch (id) {
                case MobEffectId::Speed:
                    out = { Attribute::MovementSpeed, ModifierId::EffectSpeed,
                            0.2, AttributeOperation::AddMultipliedTotal };
                    return true;
                case MobEffectId::Slowness:
                    out = { Attribute::MovementSpeed, ModifierId::EffectSlowness,
                            -0.15, AttributeOperation::AddMultipliedTotal };
                    return true;
                case MobEffectId::Haste:
                    out = { Attribute::AttackSpeed, ModifierId::EffectHaste,
                            0.1, AttributeOperation::AddMultipliedTotal };
                    return true;
                case MobEffectId::MiningFatigue:
                    out = { Attribute::AttackSpeed, ModifierId::EffectMiningFatigue,
                            -0.1, AttributeOperation::AddMultipliedTotal };
                    return true;
                case MobEffectId::Strength:
                    out = { Attribute::AttackDamage, ModifierId::EffectStrength,
                            3.0, AttributeOperation::AddValue };
                    return true;
                case MobEffectId::Weakness:
                    out = { Attribute::AttackDamage, ModifierId::EffectWeakness,
                            -4.0, AttributeOperation::AddValue };
                    return true;
                case MobEffectId::JumpBoost:
                    // Modern MC: the fall-distance allowance is the attribute
                    // half; the jump power itself is getJumpBoostPower.
                    out = { Attribute::SafeFallDistance, ModifierId::EffectJumpBoost,
                            1.0, AttributeOperation::AddValue };
                    return true;
                default:
                    return false;
            }
        }
    } // namespace

    void AddEffectAttributeModifiers(AttributeMap& attributes, MobEffectId id,
                                     int amplifier) {
        EffectAttributeTemplate t;
        if (!FindTemplate(id, t)) return;
        // MC removes then re-adds, so an upgrade replaces rather than stacks.
        attributes.RemoveModifier(t.attribute, t.id);
        AttributeModifier mod;
        mod.id = static_cast<uint32_t>(t.id);
        mod.amount = t.amount * static_cast<double>(amplifier + 1);
        mod.operation = t.operation;
        attributes.AddModifier(t.attribute, mod);
    }

    void RemoveEffectAttributeModifiers(AttributeMap& attributes, MobEffectId id) {
        EffectAttributeTemplate t;
        if (!FindTemplate(id, t)) return;
        attributes.RemoveModifier(t.attribute, t.id);
    }

    // ── Undead membership ──────────────────────────────────────────────────

    bool IsUndeadEntityType(EntityTypeId type) {
        // Vanilla data: #minecraft:undead = #zombies + #skeletons + phantom +
        // wither. Transcribed from the 1.21 data pack (the tag JSONs are not
        // part of the decompiled source tree).
        switch (type) {
            // #minecraft:zombies
            case EntityTypeId::Zombie:
            case EntityTypeId::ZombieVillager:
            case EntityTypeId::ZombifiedPiglin:
            case EntityTypeId::ZombieHorse:
            case EntityTypeId::Zoglin:
            case EntityTypeId::Husk:
            case EntityTypeId::Drowned:
            case EntityTypeId::ZombieNautilus:
            case EntityTypeId::CamelHusk:
            // #minecraft:skeletons
            case EntityTypeId::Skeleton:
            case EntityTypeId::Stray:
            case EntityTypeId::WitherSkeleton:
            case EntityTypeId::SkeletonHorse:
            case EntityTypeId::Bogged:
            case EntityTypeId::Parched:
            // direct members
            case EntityTypeId::Phantom:
            case EntityTypeId::Wither:
                return true;
            default:
                return false;
        }
    }

    bool CanBreatheUnderWaterEntityType(EntityTypeId type) {
        // Vanilla data: #minecraft:can_breathe_under_water = #undead + the
        // list below, transcribed from the 1.21 data pack
        // (data/minecraft/tags/entity_type/can_breathe_under_water.json,
        // vendored under data/). armor_stand is in the tag but is not an
        // entity type here. Note the tag is also the AXOLOTL's water half —
        // its 6000-tick land dry-out is Axolotl.handleAirSupply, which waits
        // on the axolotl's own class (still a generic Animal). The dolphin is
        // deliberately ABSENT: dolphins drown underwater (4800 ticks of air).
        // A baby happy ghast also breathes underwater in MC
        // (HappyGhast.canBreatheUnderwater); the adult does not — that
        // isBaby() split rides the happy ghast's own class if it ever
        // matters (ghastlings avoid water entirely).
        if (IsUndeadEntityType(type)) return true;
        switch (type) {
            case EntityTypeId::Axolotl:
            case EntityTypeId::Frog:
            case EntityTypeId::Guardian:
            case EntityTypeId::ElderGuardian:
            case EntityTypeId::Turtle:
            case EntityTypeId::GlowSquid:
            case EntityTypeId::Cod:
            case EntityTypeId::Pufferfish:
            case EntityTypeId::Salmon:
            case EntityTypeId::Squid:
            case EntityTypeId::TropicalFish:
            case EntityTypeId::Tadpole:
            case EntityTypeId::CopperGolem:
            case EntityTypeId::Nautilus:
                return true;
            default:
                return false;
        }
    }

    // ── MobEffectInstance ──────────────────────────────────────────────────

    bool MobEffectInstance::Update(const MobEffectInstance& takeOver) {
        // MC MobEffectInstance.update, transcribed.
        bool changed = false;
        if (takeOver.amplifier > amplifier) {
            if (takeOver.IsShorterDurationThan(*this)) {
                // Stash the current (weaker, longer) effect underneath.
                std::unique_ptr<MobEffectInstance> prevHidden = std::move(hiddenEffect);
                hiddenEffect = std::make_unique<MobEffectInstance>(*this);
                hiddenEffect->hiddenEffect = std::move(prevHidden);
            }
            amplifier = takeOver.amplifier;
            duration = takeOver.duration;
            changed = true;
        } else if (IsShorterDurationThan(takeOver)) {
            if (takeOver.amplifier == amplifier) {
                duration = takeOver.duration;
                changed = true;
            } else if (!hiddenEffect) {
                hiddenEffect = std::make_unique<MobEffectInstance>(takeOver);
            } else {
                hiddenEffect->Update(takeOver);
            }
        }

        if ((!takeOver.ambient && ambient) || changed) {
            ambient = takeOver.ambient;
            changed = true;
        }
        if (takeOver.visible != visible) {
            visible = takeOver.visible;
            changed = true;
        }
        return changed;
    }

    void MobEffectInstance::TickDownDuration() {
        // MC tickDownDuration: the whole hidden chain counts down in step, so
        // a surfaced hidden effect has already paid for the time it spent
        // buried.
        if (hiddenEffect) hiddenEffect->TickDownDuration();
        if (!IsInfiniteDuration() && duration != 0) --duration;
    }

    bool MobEffectInstance::DowngradeToHiddenEffect() {
        if (duration != 0 || !hiddenEffect) return false;
        SetDetailsFrom(*hiddenEffect);
        std::unique_ptr<MobEffectInstance> next = std::move(hiddenEffect->hiddenEffect);
        hiddenEffect = std::move(next);
        return true;
    }

    bool MobEffectInstance::TickServer(LivingEntity& target, bool& downgraded) {
        downgraded = false;
        if (!HasRemainingDuration()) return false;

        // MC: a finite effect keys its cadence on the remaining duration, an
        // infinite one on the entity's own tick count.
        const int tickCount = IsInfiniteDuration() ? target.tickCount : duration;
        if (ShouldApplyEffectTickThisTick(effect, tickCount, amplifier) &&
            !ApplyEffectTick(target, effect, amplifier)) {
            return false;
        }

        TickDownDuration();
        if (DowngradeToHiddenEffect()) downgraded = true;
        return HasRemainingDuration();
    }

} // namespace Game
