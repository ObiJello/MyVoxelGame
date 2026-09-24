// File: src/common/entity/alchemy/Potions.hpp
//
// MC net.minecraft.world.item.alchemy — the potion half of the status-effect
// system:
//
//   Potions.java / PotionIds.java   the registry (every vanilla potion, MC's
//                                   registration order, exact effect
//                                   instances and durations)
//   Potion.java                     one registry row (base name + effects)
//   PotionContents.java             the POTION_CONTENTS component record:
//                                   potion, custom colour, custom effects,
//                                   custom name — and everything that reads it
//                                   (colour mix, effect iteration, applying to
//                                   an entity, the tooltip, the item name)
//   SuspiciousStewEffects.java      the SUSPICIOUS_STEW_EFFECTS component, and
//                                   FlowerBlock's per-flower effect table
//                                   (SuspiciousEffectHolder)
//
// PotionId's ordinals ARE the registry ids (BuiltInRegistries.POTION assigns
// them in registration order). They travel on the wire inside the component
// codec, so the enum order is MC's and must not be reordered. NBT stores the
// name ("minecraft:long_swiftness"), never the ordinal.
#pragma once

#include "common/entity/effect/MobEffects.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Game {

    class LivingEntity;
    using ItemID = uint32_t;
    enum class BlockID : uint16_t;

    // MC Potions.java, registration order. The ordinal is the registry id.
    enum class PotionId : uint8_t {
        Water = 0,
        Mundane,
        Thick,
        Awkward,
        NightVision,
        LongNightVision,
        Invisibility,
        LongInvisibility,
        Leaping,
        LongLeaping,
        StrongLeaping,
        FireResistance,
        LongFireResistance,
        Swiftness,
        LongSwiftness,
        StrongSwiftness,
        Slowness,
        LongSlowness,
        StrongSlowness,
        TurtleMaster,
        LongTurtleMaster,
        StrongTurtleMaster,
        WaterBreathing,
        LongWaterBreathing,
        Healing,
        StrongHealing,
        Harming,
        StrongHarming,
        Poison,
        LongPoison,
        StrongPoison,
        Regeneration,
        LongRegeneration,
        StrongRegeneration,
        Strength,
        LongStrength,
        StrongStrength,
        Weakness,
        LongWeakness,
        Luck,
        SlowFalling,
        LongSlowFalling,
        WindCharged,
        Weaving,
        Oozing,
        Infested,
        Count
    };
    inline constexpr int kPotionCount = static_cast<int>(PotionId::Count);

    // One registry row — MC Potion(name, effects...). `id` is the registry
    // path (PotionIds: "long_swiftness"); `name` is Potion.name(), the suffix
    // every translation key uses ("swiftness" — the long and strong variants
    // share their base potion's name, which is why "Potion of Swiftness"
    // covers all three).
    struct PotionDef {
        const char*                    id;
        const char*                    name;
        std::vector<MobEffectInstance> effects;
    };

    const PotionDef& GetPotion(PotionId id);
    const char* GetPotionKey(PotionId id);      // "long_swiftness"
    bool IsValidPotionId(int raw);
    // "long_swiftness" or "minecraft:long_swiftness"; false for an unknown id.
    bool ParsePotionId(std::string_view name, PotionId& out);
    // MC Potion.hasInstantEffects.
    bool PotionHasInstantEffects(PotionId id);

    // MC PotionTags — the three data tags AbstractThrownPotion reads. Each
    // vanilla tag holds exactly minecraft:water.
    bool PotionDousesFire(PotionId id);                    // #douses_fire
    bool PotionHurtsWaterSensitive(PotionId id);           // #hurts_water_sensitive_entities
    bool PotionExtinguishesEntities(PotionId id);          // #extinguishes_entities

    // ── PotionContents (the POTION_CONTENTS component) ─────────────────────
    struct PotionContents {
        // MC PotionContents.BASE_POTION_COLOR — the empty/uncraftable blue.
        static constexpr int32_t kBasePotionColor = -13083194;   // 0xFF385DC6

        std::optional<PotionId>        potion;
        std::optional<int32_t>         customColor;   // Java int, ARGB or RGB
        std::vector<MobEffectInstance> customEffects;
        std::optional<std::string>     customName;

        PotionContents() = default;
        explicit PotionContents(PotionId p) : potion(p) {}

        // MC PotionContents.EMPTY equality — record equals over all four.
        bool IsEmpty() const {
            return !potion && !customColor && customEffects.empty() && !customName;
        }

        // MC is(Holder<Potion>): the potion, with no custom effects on top.
        bool Is(PotionId p) const { return IsPotionWithoutCustomEffects() && *potion == p; }
        bool IsPotionWithoutCustomEffects() const { return potion.has_value() && customEffects.empty(); }

        // MC getAllEffects — the potion's effects, then the custom ones.
        std::vector<MobEffectInstance> GetAllEffects() const;

        // MC forEachEffect(consumer, durationScale) — every effect with
        // withScaledDuration(scale) applied (instantaneous ones included; a
        // duration of 1 scales to max(floor(scale), 1) = 1).
        void ForEachEffect(const std::function<void(MobEffectInstance)>& consumer,
                           float durationScale) const;

        // MC withPotion / withEffectAdded.
        PotionContents WithPotion(PotionId p) const;
        PotionContents WithEffectAdded(const MobEffectInstance& effect) const;

        // MC getColor / getColorOr — the custom colour, else the amplifier-
        // weighted mix of the visible effects, else the default.
        int32_t GetColor() const { return GetColorOr(kBasePotionColor); }
        int32_t GetColorOr(int32_t defaultColor) const;

        // MC hasEffects.
        bool HasEffects() const;

        // MC applyToLivingEntity (server only — a no-op on a client level):
        // instantaneous effects through applyInstantaneousEffect (the player
        // drinking is both source and owner), the rest through addEffect.
        void ApplyToLivingEntity(LivingEntity& entity, float durationScale) const;

        // MC getName(prefix) — the translation of prefix + (customName, else
        // the potion's name, else "empty"). `itemSlug` picks the prefix
        // "item.minecraft.<slug>.effect.".
        std::string GetName(std::string_view itemSlug) const;

        // Value equality (the component map compares serialized bytes; this
        // is for the callers that hold two decoded values).
        bool operator==(const PotionContents& o) const;
        bool operator!=(const PotionContents& o) const { return !(*this == o); }
    };

    // MC PotionContents.getColorOptional — the amplifier-weighted RGB mean of
    // the VISIBLE effects' colours, opaque; nullopt when none is visible.
    std::optional<int32_t> GetPotionColorOptional(const std::vector<MobEffectInstance>& effects);

    // MC ItemStack.getOrDefault(POTION_CONTENTS, EMPTY) / POTION_DURATION_SCALE
    // (1.0 by default). Declared here, defined in Potions.cpp, so the callers
    // that only need the answer do not pull DataComponents in.
    struct ItemStack;
    PotionContents GetPotionContents(const ItemStack& stack);
    float GetPotionDurationScale(const ItemStack& stack);

    // MC PotionContents.createItemStack(item, potion).
    ItemStack CreatePotionItemStack(ItemID item, PotionId potion);

    // The four PotionItem / TippedArrowItem kinds whose getName reads the
    // POTION_CONTENTS component.
    bool IsPotionNamedItem(ItemID item);

    // ── Tooltip (MC PotionContents.addPotionTooltip) ────────────────────────
    struct PotionTooltipLine {
        std::string text;
        uint32_t    colorARGB;
    };
    void AddPotionTooltip(const std::vector<MobEffectInstance>& effects,
                          std::vector<PotionTooltipLine>& out,
                          float durationScale, float tickrate = 20.0f);

    // MC PotionContents.getPotionDescription — "Speed" / "Speed II".
    std::string GetPotionEffectDescription(MobEffectId effect, int amplifier);

    // The en_us strings the names and tooltips need, captured once by
    // ItemRegistry::Initialize from the file it already parses. Every lookup
    // falls back to the raw key, which is what MC's Component.translatable
    // prints for a key the language lacks.
    void LoadAlchemyLanguage(const std::unordered_map<std::string, std::string>& lang);
    std::string AlchemyTranslate(const std::string& key);

    // ── Suspicious stew (MC SuspiciousStewEffects) ──────────────────────────
    struct SuspiciousStewEffects {
        static constexpr int kDefaultDuration = 160;   // MC DEFAULT_DURATION
        struct Entry {
            MobEffectId effect   = MobEffectId::Speed;
            int         duration = kDefaultDuration;
            // MC Entry.createEffectInstance.
            MobEffectInstance CreateEffectInstance() const {
                return MobEffectInstance(effect, duration);
            }
        };
        std::vector<Entry> effects;

        bool operator==(const SuspiciousStewEffects& o) const;
    };

    // MC FlowerBlock.getSuspiciousEffects (SuspiciousEffectHolder) — the
    // flower's stew effect, makeEffectList(effect, seconds) =
    // floor(seconds * 20) ticks. Null for a block that is no flower.
    const SuspiciousStewEffects* GetFlowerSuspiciousEffects(BlockID block);
    // SuspiciousEffectHolder.getAllEffectHolders, in Blocks.java order — the
    // creative tab's stew list.
    const std::vector<BlockID>& GetSuspiciousEffectFlowers();

} // namespace Game
