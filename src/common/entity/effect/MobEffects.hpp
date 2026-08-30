// File: src/common/entity/effect/MobEffects.hpp
//
// MC net.minecraft.world.effect — the status-effect registry and
// MobEffectInstance, reduced to the effects the mob system can actually
// produce and consume.
//
// SERVER-SIDE ONLY this wave: no client sync and no particles exist yet, so
// an effect lives entirely on the server's copy of the entity. The places a
// client would notice — the swirl particles, the invisibility render, the HUD
// icons — are each commented at their site as the documented follow-up.
//
// The numbers here are MC's registration table (MobEffects.java) verbatim:
//
//   SPEED           MOVEMENT_SPEED  +20%/lvl  ADD_MULTIPLIED_TOTAL
//   SLOWNESS        MOVEMENT_SPEED  −15%/lvl  ADD_MULTIPLIED_TOTAL
//   HASTE           ATTACK_SPEED    +10%/lvl  ADD_MULTIPLIED_TOTAL
//   MINING_FATIGUE  ATTACK_SPEED    −10%/lvl  ADD_MULTIPLIED_TOTAL
//   STRENGTH        ATTACK_DAMAGE   +3/lvl    ADD_VALUE
//   WEAKNESS        ATTACK_DAMAGE   −4/lvl    ADD_VALUE
//   JUMP_BOOST      SAFE_FALL_DISTANCE +1/lvl ADD_VALUE (modern MC; the jump
//                   power itself is getJumpBoostPower, +0.1/lvl, on LivingEntity)
//   REGENERATION    heal 1 every 50 >> amp ticks (while below max health)
//   POISON          1 magic damage every 25 >> amp ticks, cannot drop below 1 HP
//   WITHER          1 wither damage every 40 >> amp ticks, CAN kill
//   INSTANT_HEALTH  heal 4 << amp (instantaneous; 6 << amp as damage on undead)
//   INSTANT_DAMAGE  6 << amp magic damage (heals 4 << amp on undead)
//   HUNGER          0.005 * (amp+1) exhaustion per tick (players only)
//   SATURATION      eat(amp+1, 1.0F) once (players only)
//   LEVITATION      motion y eased toward 0.05 * (amp+1) in travel
//   SLOW_FALLING    gravity capped at 0.01 while falling
//   FIRE_RESISTANCE immune to fire damage (hurt-path check)
//   WATER_BREATHING flag only — the air-supply system is a later wave
//   INVISIBILITY    flag only — rendering is client work
//   BLINDNESS       flag only — the fog/vision render is client work
//                   (the illusioner's blindness spell is the producer)
//   GLOWING         flag only — rendering is client work
//   RESISTANCE      −20%/lvl of post-armor damage (getDamageAfterMagicAbsorb)
#pragma once

#include "common/entity/Attributes.hpp"

#include <cstdint>
#include <memory>

namespace Game {

    class Entity;
    class LivingEntity;
    enum class EntityTypeId : uint16_t;

    enum class MobEffectId : uint8_t {
        Speed = 0,
        Slowness,
        Haste,          // MC DIG_SPEED / "haste"
        MiningFatigue,  // MC DIG_SLOWDOWN / "mining_fatigue"
        Strength,       // MC DAMAGE_BOOST
        InstantHealth,  // MC HEAL
        InstantDamage,  // MC HARM
        JumpBoost,      // MC JUMP
        Regeneration,
        Resistance,
        FireResistance,
        WaterBreathing,
        Invisibility,
        Hunger,
        Weakness,
        Poison,
        Wither,
        Glowing,
        Levitation,
        SlowFalling,
        Saturation,
        Blindness,      // appended (BLINDNESS) — enum order is not MC registry
                        // order and nothing wire-visible reads these ordinals
        Count
    };

    enum class MobEffectCategory : uint8_t { Beneficial, Harmful, Neutral };

    // ── Registry queries (MC MobEffect) ────────────────────────────────────
    MobEffectCategory GetEffectCategory(MobEffectId id);
    bool IsInstantenousEffect(MobEffectId id);       // MC's spelling, kept
    const char* GetEffectName(MobEffectId id);

    // MC MobEffect.shouldApplyEffectTickThisTick. `tickCount` is the remaining
    // duration for a finite effect and the entity's own tickCount for an
    // infinite one, exactly as MobEffectInstance.tickServer passes it.
    bool ShouldApplyEffectTickThisTick(MobEffectId id, int tickCount, int amplifier);

    // MC MobEffect.applyEffectTick. Returns false when the effect should be
    // removed (no effect in this set ever does — the hook is kept because
    // tickServer's contract depends on it).
    bool ApplyEffectTick(LivingEntity& mob, MobEffectId id, int amplifier);

    // MC MobEffect.applyInstantenousEffect — the splash-potion path, where
    // `scale` is the distance falloff. `owner` gets the damage attribution.
    void ApplyInstantenousEffect(Entity* source, Entity* owner, LivingEntity& mob,
                                 MobEffectId id, int amplifier, double scale);

    // MC MobEffect.addAttributeModifiers / removeAttributeModifiers — the
    // per-effect AttributeTemplate, amount scaled by (amplifier + 1).
    void AddEffectAttributeModifiers(AttributeMap& attributes, MobEffectId id,
                                     int amplifier);
    void RemoveEffectAttributeModifiers(AttributeMap& attributes, MobEffectId id);

    // MC EntityTypeTags.INVERTED_HEALING_AND_HARM and IGNORES_POISON_AND_REGEN.
    // Both vanilla data tags contain exactly #minecraft:undead (the zombie and
    // skeleton families, phantom, wither), so one membership test serves both.
    bool IsUndeadEntityType(EntityTypeId type);

    // MC EntityTypeTags.CAN_BREATHE_UNDER_WATER — #minecraft:undead plus the
    // true water breathers. Read by LivingEntity::CanBreatheUnderwater, which
    // is what keeps a squid alive at the bottom of the ocean while a cow
    // drowns there.
    bool CanBreatheUnderWaterEntityType(EntityTypeId type);

    // ── MobEffectInstance ──────────────────────────────────────────────────
    //
    // MC MobEffectInstance including the hiddenEffect chain: re-adding a
    // STRONGER but SHORTER effect stashes the current one underneath, and when
    // the strong one runs out the weaker, longer one surfaces again — the rule
    // that makes Strength II from a beacon not eat your 8-minute Strength I.
    struct MobEffectInstance {
        static constexpr int kInfiniteDuration = -1;   // MC INFINITE_DURATION

        MobEffectId effect   = MobEffectId::Speed;
        int  duration        = 0;      // ticks; -1 = infinite
        int  amplifier       = 0;      // 0-based (amplifier 1 == level II)
        bool ambient         = false;  // beacon-style; kept for parity
        bool visible         = true;   // particle flag; consumed when particles land

        // MC's hiddenEffect — a singly linked chain, strongest on top.
        std::unique_ptr<MobEffectInstance> hiddenEffect;

        MobEffectInstance() = default;
        MobEffectInstance(MobEffectId e, int dur, int amp = 0,
                          bool amb = false, bool vis = true)
            : effect(e), duration(dur), amplifier(amp), ambient(amb), visible(vis) {}

        // MC MobEffectInstance(copy) copies the DETAILS only — the hidden
        // chain deliberately does not travel with a copy.
        MobEffectInstance(const MobEffectInstance& o)
            : effect(o.effect), duration(o.duration), amplifier(o.amplifier),
              ambient(o.ambient), visible(o.visible) {}
        MobEffectInstance& operator=(const MobEffectInstance& o) {
            effect = o.effect;
            SetDetailsFrom(o);
            hiddenEffect.reset();
            return *this;
        }
        MobEffectInstance(MobEffectInstance&&) = default;
        MobEffectInstance& operator=(MobEffectInstance&&) = default;

        bool IsInfiniteDuration() const { return duration == kInfiniteDuration; }

        // MC endsWithin — false for infinite.
        bool EndsWithin(int ticks) const {
            return !IsInfiniteDuration() && duration <= ticks;
        }

        // MC mapDuration((d) -> (int)(scale * d + 0.5)) — the splash falloff.
        int MapScaledDuration(double scale) const {
            if (IsInfiniteDuration() || duration == 0) return duration;
            return static_cast<int>(scale * static_cast<double>(duration) + 0.5);
        }

        // MC MobEffectInstance.update — the upgrade rules, ported exactly:
        //   * higher amplifier always wins; if it is also SHORTER, the current
        //     effect is stashed as a hidden effect first;
        //   * same amplifier keeps the longer duration;
        //   * lower amplifier with a longer duration hides underneath.
        // Returns whether anything observable changed (MC's `changed`).
        bool Update(const MobEffectInstance& takeOver);

        // MC MobEffectInstance.tickServer. Returns false when the effect has
        // expired and should be removed; sets `downgraded` when the hidden
        // effect surfaced this tick (the caller refreshes attribute modifiers,
        // MC's onEffectUpdate runnable).
        bool TickServer(LivingEntity& target, bool& downgraded);

    private:
        void SetDetailsFrom(const MobEffectInstance& o) {
            duration = o.duration;
            amplifier = o.amplifier;
            ambient = o.ambient;
            visible = o.visible;
        }
        bool HasRemainingDuration() const {
            return IsInfiniteDuration() || duration > 0;
        }
        void TickDownDuration();
        bool DowngradeToHiddenEffect();
        // MC isShorterDurationThan.
        bool IsShorterDurationThan(const MobEffectInstance& other) const {
            return !IsInfiniteDuration() &&
                   (duration < other.duration || other.IsInfiniteDuration());
        }
    };

} // namespace Game
