// File: src/common/entity/effect/MobEffects.hpp
//
// MC net.minecraft.world.effect — the whole status-effect package:
//
//   MobEffects.java            the registry (every vanilla effect, MC's exact
//                              registration order, colours, categories,
//                              attribute templates, blend durations)
//   MobEffect.java + subclasses the per-effect behaviour — applyEffectTick /
//                              shouldApplyEffectTickThisTick /
//                              applyInstantaneousEffect / onEffectStarted /
//                              onMobRemoved / onMobHurt
//   MobEffectInstance.java     one active effect, the hiddenEffect chain and
//                              the client-side BlendState
//   MobEffectUtil.java         the dig-speed / water-breathing helpers and the
//                              duration formatter
//
// MobEffectId's ordinals ARE the registry ids (BuiltInRegistries.MOB_EFFECT
// assigns them in registration order), and they travel on the wire in
// UpdateMobEffectS2C / RemoveMobEffectS2C and in the entity effect-particle
// lists, so the enum order is MC's and must not be reordered. NBT stores the
// name ("minecraft:speed"), never the ordinal.
//
// What has no consumer in this engine (the effect is stored, ticked, synced
// and displayed; only the downstream system is missing):
//   BAD_OMEN / RAID_OMEN   no villages (Level.isVillage) and no raids
//   HERO_OF_THE_VILLAGE    no villager trade discounts or gifts
//   TRIAL_OMEN             no ominous trial spawners (BAD_OMEN never
//                          converts to it)
//   LUCK / UNLUCK          read by container loot only (ChestLoot::Fill's
//                          luck, the opening player's) — no fishing exists
//   CONDUIT_POWER          no conduit block entity grants it (the effect's
//                          dig speed / water breathing / underwater vision
//                          all work when given by /effect)
//   BREATH_OF_THE_NAUTILUS players cannot ride a nautilus (its only source)
//   WEAVING                its cobweb half (WebBlock halves the web's slow
//                          for a weaver) waits on cobweb stuck-speed, which
//                          does not exist; the on-death webs work
//   INVISIBILITY's WAYPOINT_TRANSMIT_RANGE modifier — no locator bar
#pragma once

#include "common/entity/Attributes.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Game {

    class Entity;
    class LivingEntity;
    class Mob;
    struct EntityLevel;
    struct MobEffectInstance;
    enum class EntityTypeId : uint16_t;
    enum class RemovalReason : uint8_t;

    // MC MobEffects.java, registration order. The ordinal is the registry id.
    enum class MobEffectId : uint8_t {
        Speed = 0,          // speed
        Slowness,           // slowness
        Haste,              // haste
        MiningFatigue,      // mining_fatigue
        Strength,           // strength
        InstantHealth,      // instant_health
        InstantDamage,      // instant_damage
        JumpBoost,          // jump_boost
        Nausea,             // nausea
        Regeneration,       // regeneration
        Resistance,         // resistance
        FireResistance,     // fire_resistance
        WaterBreathing,     // water_breathing
        Invisibility,       // invisibility
        Blindness,          // blindness
        NightVision,        // night_vision
        Hunger,             // hunger
        Weakness,           // weakness
        Poison,             // poison
        Wither,             // wither
        HealthBoost,        // health_boost
        Absorption,         // absorption
        Saturation,         // saturation
        Glowing,            // glowing
        Levitation,         // levitation
        Luck,               // luck
        Unluck,             // unluck
        SlowFalling,        // slow_falling
        ConduitPower,       // conduit_power
        DolphinsGrace,      // dolphins_grace
        BadOmen,            // bad_omen
        HeroOfTheVillage,   // hero_of_the_village
        Darkness,           // darkness
        TrialOmen,          // trial_omen
        RaidOmen,           // raid_omen
        WindCharged,        // wind_charged
        Weaving,            // weaving
        Oozing,             // oozing
        Infested,           // infested
        BreathOfTheNautilus,// breath_of_the_nautilus
        Count
    };
    inline constexpr int kMobEffectCount = static_cast<int>(MobEffectId::Count);

    // MC MobEffectCategory. BENEFICIAL and NEUTRAL tooltips are blue, HARMFUL
    // red; only BENEFICIAL counts as MobEffect.isBeneficial (the HUD row).
    enum class MobEffectCategory : uint8_t { Beneficial, Harmful, Neutral };

    // MC MobEffect.particleFactory — ENTITY_EFFECT coloured by the effect
    // (alpha 38 for an ambient instance) for most; a fixed particle type for
    // the six effects registered with one.
    enum class MobEffectParticle : uint8_t {
        EntityEffect,   // ColorParticleOption(ENTITY_EFFECT, argb)
        TrialOmen,      // ParticleTypes.TRIAL_OMEN
        RaidOmen,       // ParticleTypes.RAID_OMEN
        SmallGust,      // ParticleTypes.SMALL_GUST       (wind_charged)
        ItemCobweb,     // ParticleTypes.ITEM_COBWEB      (weaving)
        ItemSlime,      // ParticleTypes.ITEM_SLIME       (oozing)
        Infested,       // ParticleTypes.INFESTED
    };

    // MC MobEffect.AttributeTemplate — one per (effect, attribute).
    struct EffectAttributeTemplate {
        Attribute          attribute;
        ModifierId         id;
        double             amount;      // per level: create(amp) = amount * (amp + 1)
        AttributeOperation operation;
    };

    // One registry row.
    struct MobEffectInfo {
        const char*       name;          // registry path ("speed")
        const char*       displayName;   // en_us "effect.minecraft.<name>"
        MobEffectCategory category;
        uint32_t          color;         // RGB, MobEffect.getColor
        MobEffectParticle particle;
        bool              instantaneous; // InstantaneousMobEffect
        // MobEffect.setBlendDuration(in, out, outAdvance): NAUSEA (150, 20,
        // 60) and DARKNESS (22) only. Read by the client BlendState.
        int               blendInTicks;
        int               blendOutTicks;
        int               blendOutAdvanceTicks;
        // 0 or 1 templates — no vanilla effect has two.
        int                     modifierCount;
        EffectAttributeTemplate modifier;
    };

    const MobEffectInfo& GetEffectInfo(MobEffectId id);

    // ── Registry queries (MC MobEffect) ────────────────────────────────────
    MobEffectCategory GetEffectCategory(MobEffectId id);
    bool IsBeneficialEffect(MobEffectId id);          // MobEffect.isBeneficial
    bool IsInstantenousEffect(MobEffectId id);        // MobEffect.isInstantaneous
    const char* GetEffectName(MobEffectId id);        // "speed"
    const char* GetEffectDisplayName(MobEffectId id); // "Speed"
    uint32_t GetEffectColor(MobEffectId id);          // 0xRRGGBB
    bool IsValidEffectId(int raw);
    // "speed" or "minecraft:speed"; false for an unknown id (MC
    // ResourceArgument.ERROR_UNKNOWN_RESOURCE).
    bool ParseEffectId(std::string_view name, MobEffectId& out);

    // MC MobEffect.shouldApplyEffectTickThisTick. `tickCount` is the remaining
    // duration for a finite effect and the entity's own tickCount for an
    // infinite one, exactly as MobEffectInstance.tickServer passes it.
    bool ShouldApplyEffectTickThisTick(MobEffectId id, int tickCount, int amplifier);

    // MC MobEffect.applyEffectTick. Returns false when the effect should be
    // removed (ABSORPTION once the hearts are gone, BAD_OMEN when it turns
    // into RAID_OMEN, RAID_OMEN once its raid starts).
    bool ApplyEffectTick(LivingEntity& mob, MobEffectId id, int amplifier);

    // MC MobEffect.applyInstantaneousEffect — the splash-potion path, where
    // `scale` is the distance falloff. `owner` gets the damage attribution.
    void ApplyInstantenousEffect(Entity* source, Entity* owner, LivingEntity& mob,
                                 MobEffectId id, int amplifier, double scale);

    // MC MobEffect.onEffectStarted — runs on EVERY addEffect call, landed or
    // not (AbsorptionMobEffect tops the absorption hearts up).
    void OnEffectStarted(LivingEntity& mob, MobEffectId id, int amplifier);

    // MC MobEffect.onMobRemoved — WIND_CHARGED's burst, WEAVING's cobwebs,
    // OOZING's slimes, all on RemovalReason.KILLED. Server side only.
    void OnEffectMobRemoved(LivingEntity& mob, MobEffectId id, int amplifier,
                            RemovalReason reason);

    // MC MobEffect.onMobHurt — INFESTED's silverfish. Server side only.
    void OnEffectMobHurt(LivingEntity& mob, MobEffectId id, int amplifier);

    // The server's mob factory (IntegratedServer's MakeMobForLoad), for the
    // two effects that spawn mobs (OOZING's slimes, INFESTED's silverfish).
    // Common code cannot see the server's 70-case switch; the server installs
    // it once at startup. Null = those two effects spawn nothing.
    using EffectMobFactory = std::unique_ptr<Mob> (*)(EntityTypeId type, EntityLevel* level);
    void SetEffectMobFactory(EffectMobFactory factory);

    // MC MobEffect.soundOnAdded (MobEffect.onEffectAdded plays it at the
    // mob's feet when the effect first lands): BAD_OMEN, TRIAL_OMEN and
    // RAID_OMEN chime; null for every other effect.
    const char* GetEffectSoundOnAdded(MobEffectId id);

    // MC MobEffectUtil.addEffectToPlayersAround — every survival/adventure
    // player strictly within `radius` of `position` that does not already
    // hold an equal-or-stronger instance lasting past `displayEffectLimit - 1`
    // ticks gets a copy of `effect`, attributed to `source`. Returns the
    // players it was given to. (The warden's darkness pulse, the elder
    // guardian's mining fatigue.)
    std::vector<LivingEntity*> AddEffectToPlayersAround(EntityLevel& level, Entity* source,
                                                        const glm::dvec3& position, double radius,
                                                        const MobEffectInstance& effect,
                                                        int displayEffectLimit);

    // MC MobEffect.addAttributeModifiers / removeAttributeModifiers — the
    // per-effect AttributeTemplate, amount scaled by (amplifier + 1). Like
    // MC, an attribute the map does not have is skipped (a cow has no
    // ATTACK_DAMAGE for STRENGTH to modify).
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
        static constexpr int kMinAmplifier = 0;         // MC MIN_AMPLIFIER
        static constexpr int kMaxAmplifier = 255;       // MC MAX_AMPLIFIER

        MobEffectId effect   = MobEffectId::Speed;
        int  duration        = 0;      // ticks; -1 = infinite
        int  amplifier       = 0;      // 0-based (amplifier 1 == level II)
        bool ambient         = false;  // beacon-style: faint particles, HUD border
        bool visible         = true;   // MC `visible` — the particles
        bool showIcon        = true;   // MC `showIcon` — the HUD / inventory icon

        // MC's hiddenEffect — a singly linked chain, strongest on top.
        std::unique_ptr<MobEffectInstance> hiddenEffect;

        // MC MobEffectInstance.BlendState — CLIENT only (the NAUSEA warp and
        // the DARKNESS fog/lightmap ease in and out over their blend ticks).
        float blendFactor          = 0.0f;
        float blendFactorPrevFrame = 0.0f;

        MobEffectInstance() = default;
        // MC (effect, duration, amplifier, ambient, visible) — showIcon
        // defaults to `visible`, as MC's five-argument constructor does.
        MobEffectInstance(MobEffectId e, int dur, int amp = 0,
                          bool amb = false, bool vis = true)
            : MobEffectInstance(e, dur, amp, amb, vis, vis) {}
        MobEffectInstance(MobEffectId e, int dur, int amp, bool amb, bool vis, bool icon)
            : effect(e), duration(dur), amplifier(ClampAmplifier(amp)),
              ambient(amb), visible(vis), showIcon(icon) {}

        // MC MobEffectInstance(copy) copies the DETAILS only — the hidden
        // chain deliberately does not travel with a copy, and the blend state
        // starts fresh.
        MobEffectInstance(const MobEffectInstance& o)
            : effect(o.effect), duration(o.duration), amplifier(o.amplifier),
              ambient(o.ambient), visible(o.visible), showIcon(o.showIcon) {}
        MobEffectInstance& operator=(const MobEffectInstance& o) {
            if (this == &o) return *this;
            effect = o.effect;
            SetDetailsFrom(o);
            hiddenEffect.reset();
            blendFactor = blendFactorPrevFrame = 0.0f;
            return *this;
        }
        MobEffectInstance(MobEffectInstance&&) = default;
        MobEffectInstance& operator=(MobEffectInstance&&) = default;

        static int ClampAmplifier(int a) {
            return a < kMinAmplifier ? kMinAmplifier : (a > kMaxAmplifier ? kMaxAmplifier : a);
        }

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

        // MC withScaledDuration — max(floor(d * scale), 1), infinite and 0 kept.
        MobEffectInstance WithScaledDuration(float scale) const;

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

        // MC tickServer split at applyEffectTick, for a caller that must
        // survive the effect list changing under it (a killing WITHER tick
        // spending a totem clears every effect mid-pass):
        //   HasRemainingDuration()          the up-front expiry test;
        //   CadenceTickCount(entityTick)    what shouldApplyEffectTickThisTick
        //                                   is asked with;
        //   FinishServerTick(downgraded)    tickDownDuration + the hidden
        //                                   effect surfacing; false = expired.
        bool HasRemainingDuration() const {
            return IsInfiniteDuration() || duration > 0;
        }
        int CadenceTickCount(int entityTickCount) const {
            return IsInfiniteDuration() ? entityTickCount : duration;
        }
        bool FinishServerTick(bool& downgraded) {
            TickDownDuration();
            downgraded = DowngradeToHiddenEffect();
            return HasRemainingDuration();
        }

        // MC MobEffectInstance.tickClient: count down (the client predicts the
        // expiry the server will confirm with a remove packet), surface a
        // hidden effect, advance the blend.
        void TickClient();

        // MC BlendState.getFactor / setImmediate / copyFrom.
        float GetBlendFactor(float partialTick) const {
            return blendFactorPrevFrame + (blendFactor - blendFactorPrevFrame) * partialTick;
        }
        void SkipBlending();
        void CopyBlendState(const MobEffectInstance& o) {
            blendFactor = o.blendFactor;
            blendFactorPrevFrame = o.blendFactorPrevFrame;
        }

        // MC MobEffectInstance.compareTo — the HUD and inventory sort order.
        // Negative when *this sorts before `o`.
        int CompareTo(const MobEffectInstance& o) const;

        // MC MobEffect.createParticleOptions(this): the particle kind and, for
        // ENTITY_EFFECT, the ARGB colour (alpha 38 when ambient, else 255).
        MobEffectParticle ParticleType() const { return GetEffectInfo(effect).particle; }
        uint32_t ParticleArgb() const;

    private:
        void SetDetailsFrom(const MobEffectInstance& o) {
            duration  = o.duration;
            amplifier = o.amplifier;
            ambient   = o.ambient;
            visible   = o.visible;
            showIcon  = o.showIcon;
        }
        void TickDownDuration();
        bool DowngradeToHiddenEffect();
        void TickBlend();
        // MC isShorterDurationThan.
        bool IsShorterDurationThan(const MobEffectInstance& other) const {
            return !IsInfiniteDuration() &&
                   (duration < other.duration || other.IsInfiniteDuration());
        }
    };

    // ── MobEffectUtil (and the handful of LivingEntity queries it wraps) ──
    //
    // Over a plain effect list so the client's local player — which is not a
    // LivingEntity — asks exactly the same questions the server does.
    const MobEffectInstance* FindEffectIn(const std::vector<MobEffectInstance>& effects,
                                          MobEffectId id);
    inline bool HasEffectIn(const std::vector<MobEffectInstance>& effects, MobEffectId id) {
        return FindEffectIn(effects, id) != nullptr;
    }
    // MobEffectUtil.hasDigSpeed / getDigSpeedAmplification.
    bool HasDigSpeed(const std::vector<MobEffectInstance>& effects);
    int  GetDigSpeedAmplification(const std::vector<MobEffectInstance>& effects);
    // MobEffectUtil.hasWaterBreathing / shouldEffectsRefillAirsupply.
    bool HasWaterBreathing(const std::vector<MobEffectInstance>& effects);
    bool ShouldEffectsRefillAirSupply(const std::vector<MobEffectInstance>& effects);
    // The effect half of Player.getDestroySpeed: HASTE / CONDUIT_POWER's
    // 1 + (amp + 1) * 0.2 and MINING_FATIGUE's 0.3^(amp + 1), as one factor
    // for GetDestroyProgressPerTick.
    float GetEffectDigSpeedMultiplier(const std::vector<MobEffectInstance>& effects);
    // MobEffectUtil.formatDuration(instance, 1.0, tickrate) —
    // StringUtil.formatTickDuration's "MM:SS" / "HH:MM:SS", or "∞".
    std::string FormatEffectDuration(const MobEffectInstance& instance, float tickrate = 20.0f);
    // EffectsInInventory.getEffectName: the display name plus " II".." X" for
    // amplifiers 1..9 (enchantment.level.N).
    std::string GetEffectInstanceDisplayName(const MobEffectInstance& instance);
    // The value an attribute takes under this effect list's templates — for
    // the two holders that keep an effect list without an AttributeMap (the
    // server's ServerPlayer and the client's local player). `base` is the
    // attribute's base plus whatever non-effect ADD_VALUE modifiers apply
    // (the held weapon's attack damage).
    double ComputeAttributeWithEffects(Attribute attribute, double base,
                                       const std::vector<MobEffectInstance>& effects);
    // The same templates added onto an instance that already carries other
    // modifiers (the worn enchantments' — EnchantmentHelper::
    // PlayerAttributeValue), so every modifier folds in MC's one pass.
    void AddEffectAttributeModifiers(AttributeInstance& instance,
                                     const std::vector<MobEffectInstance>& effects);
    // MC GameRenderer.nightVisionScale.
    float NightVisionScale(const MobEffectInstance& nightVision, float partialTick);
    // MC LivingEntity.areAllEffectsAmbient.
    bool AreAllEffectsAmbient(const std::vector<MobEffectInstance>& effects);

    // ── Synched effect visuals (MC DATA_EFFECT_PARTICLES, DATA_EFFECT_AMBIENCE_ID
    //    and the invisible / glowing shared flags) ──────────────────────────
    //
    // What every OTHER client needs to draw an entity's effects: one byte per
    // VISIBLE effect (registry id | 0x80 when ambient — the particle and its
    // alpha follow from those two, exactly as createParticleOptions does),
    // plus the INVISIBILITY and GLOWING flags. Carried by AddEntity /
    // SetEntityData for mobs and PlayerUpdate for players.
    struct EffectVisuals {
        static constexpr uint8_t kFlagInvisible = 0x01;
        static constexpr uint8_t kFlagGlowing   = 0x02;
        static constexpr uint8_t kAmbientBit    = 0x80;
        uint8_t              flags = 0;
        std::vector<uint8_t> particles;

        bool Invisible() const { return (flags & kFlagInvisible) != 0; }
        bool Glowing()   const { return (flags & kFlagGlowing) != 0; }
        bool operator==(const EffectVisuals& o) const {
            return flags == o.flags && particles == o.particles;
        }
        bool operator!=(const EffectVisuals& o) const { return !(*this == o); }
        // MC updateSynchronizedMobEffectParticles + updateInvisibilityStatus.
        static EffectVisuals FromEffects(const std::vector<MobEffectInstance>& effects,
                                         bool glowing);
        // MC areAllEffectsAmbient over the synched list.
        bool AllAmbient() const;
    };

    // MC LivingEntity.tickEffects' client branch: one particle roll a tick.
    // `invisible` picks the 1-in-15 rate over 1-in-4, ambient makes it five
    // times rarer. Spawns through the level's AddColorParticle at a random
    // point of the box (randomX(0.5), randomY(), randomZ(0.5)).
    void SpawnEffectParticles(EntityLevel& level, const EffectVisuals& visuals,
                              const glm::dvec3& pos, float bbWidth, float bbHeight);

} // namespace Game
