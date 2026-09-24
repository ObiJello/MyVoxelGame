// File: src/common/entity/alchemy/Potions.cpp
//
// See Potions.hpp. Citations are MC 26.3 (minecraft_code_26.3-pre-2): Potions.java,
// PotionIds.java, Potion.java, PotionContents.java, SuspiciousStewEffects.java,
// Blocks.java (the flower rows) and EyeblossomBlock.java.
#include "common/entity/alchemy/Potions.hpp"

#include "common/data/DataComponents.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/world/block/Blocks.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace Game {

    namespace {
        using E = MobEffectId;
        using MEI = MobEffectInstance;

        // ── Registry (MC Potions.java static block, verbatim) ──────────────
        const std::vector<PotionDef>& Registry() {
            static const std::vector<PotionDef> kPotions = {
                {"water",                 "water",           {}},
                {"mundane",               "mundane",         {}},
                {"thick",                 "thick",           {}},
                {"awkward",               "awkward",         {}},
                {"night_vision",          "night_vision",    {MEI(E::NightVision, 3600)}},
                {"long_night_vision",     "night_vision",    {MEI(E::NightVision, 9600)}},
                {"invisibility",          "invisibility",    {MEI(E::Invisibility, 3600)}},
                {"long_invisibility",     "invisibility",    {MEI(E::Invisibility, 9600)}},
                {"leaping",               "leaping",         {MEI(E::JumpBoost, 3600)}},
                {"long_leaping",          "leaping",         {MEI(E::JumpBoost, 9600)}},
                {"strong_leaping",        "leaping",         {MEI(E::JumpBoost, 1800, 1)}},
                {"fire_resistance",       "fire_resistance", {MEI(E::FireResistance, 3600)}},
                {"long_fire_resistance",  "fire_resistance", {MEI(E::FireResistance, 9600)}},
                {"swiftness",             "swiftness",       {MEI(E::Speed, 3600)}},
                {"long_swiftness",        "swiftness",       {MEI(E::Speed, 9600)}},
                {"strong_swiftness",      "swiftness",       {MEI(E::Speed, 1800, 1)}},
                {"slowness",              "slowness",        {MEI(E::Slowness, 1800)}},
                {"long_slowness",         "slowness",        {MEI(E::Slowness, 4800)}},
                {"strong_slowness",       "slowness",        {MEI(E::Slowness, 400, 3)}},
                {"turtle_master",         "turtle_master",   {MEI(E::Slowness, 400, 3), MEI(E::Resistance, 400, 2)}},
                {"long_turtle_master",    "turtle_master",   {MEI(E::Slowness, 800, 3), MEI(E::Resistance, 800, 2)}},
                {"strong_turtle_master",  "turtle_master",   {MEI(E::Slowness, 400, 5), MEI(E::Resistance, 400, 3)}},
                {"water_breathing",       "water_breathing", {MEI(E::WaterBreathing, 3600)}},
                {"long_water_breathing",  "water_breathing", {MEI(E::WaterBreathing, 9600)}},
                {"healing",               "healing",         {MEI(E::InstantHealth, 1)}},
                {"strong_healing",        "healing",         {MEI(E::InstantHealth, 1, 1)}},
                {"harming",               "harming",         {MEI(E::InstantDamage, 1)}},
                {"strong_harming",        "harming",         {MEI(E::InstantDamage, 1, 1)}},
                {"poison",                "poison",          {MEI(E::Poison, 900)}},
                {"long_poison",           "poison",          {MEI(E::Poison, 1800)}},
                {"strong_poison",         "poison",          {MEI(E::Poison, 432, 1)}},
                {"regeneration",          "regeneration",    {MEI(E::Regeneration, 900)}},
                {"long_regeneration",     "regeneration",    {MEI(E::Regeneration, 1800)}},
                {"strong_regeneration",   "regeneration",    {MEI(E::Regeneration, 450, 1)}},
                {"strength",              "strength",        {MEI(E::Strength, 3600)}},
                {"long_strength",         "strength",        {MEI(E::Strength, 9600)}},
                {"strong_strength",       "strength",        {MEI(E::Strength, 1800, 1)}},
                {"weakness",              "weakness",        {MEI(E::Weakness, 1800)}},
                {"long_weakness",         "weakness",        {MEI(E::Weakness, 4800)}},
                {"luck",                  "luck",            {MEI(E::Luck, 6000)}},
                {"slow_falling",          "slow_falling",    {MEI(E::SlowFalling, 1800)}},
                {"long_slow_falling",     "slow_falling",    {MEI(E::SlowFalling, 4800)}},
                {"wind_charged",          "wind_charged",    {MEI(E::WindCharged, 3600)}},
                {"weaving",               "weaving",         {MEI(E::Weaving, 3600)}},
                {"oozing",                "oozing",          {MEI(E::Oozing, 3600)}},
                {"infested",              "infested",        {MEI(E::Infested, 3600)}},
            };
            return kPotions;
        }

        // ── en_us subset (see LoadAlchemyLanguage) ─────────────────────────
        std::mutex g_langMutex;
        std::unordered_map<std::string, std::string> g_lang;

        bool WantsKey(const std::string& key) {
            static const char* kPrefixes[] = {
                "item.minecraft.potion.effect.",
                "item.minecraft.splash_potion.effect.",
                "item.minecraft.lingering_potion.effect.",
                "item.minecraft.tipped_arrow.effect.",
                "potion.",
                "attribute.name.",
                "attribute.modifier.",
                "effect.none",
            };
            for (const char* p : kPrefixes) {
                if (key.rfind(p, 0) == 0) return true;
            }
            return false;
        }

        // MC's %s substitution for the two-argument formats this file uses.
        std::string Format2(const std::string& fmt, const std::string& a, const std::string& b) {
            std::string out;
            out.reserve(fmt.size() + a.size() + b.size());
            int arg = 0;
            for (size_t i = 0; i < fmt.size(); ++i) {
                if (fmt[i] == '%' && i + 1 < fmt.size()) {
                    if (fmt[i + 1] == 's') {
                        out += (arg == 0) ? a : b;
                        ++arg;
                        ++i;
                        continue;
                    }
                    if (fmt[i + 1] == '%') { out += '%'; ++i; continue; }
                }
                out += fmt[i];
            }
            return out;
        }

        // MC ItemAttributeModifiers.ATTRIBUTE_MODIFIER_FORMAT = "#.##".
        std::string FormatModifierAmount(double v) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%.2f", v);
            std::string t = buf;
            while (!t.empty() && t.back() == '0') t.pop_back();
            if (!t.empty() && t.back() == '.') t.pop_back();
            if (t == "-0") t = "0";
            return t;
        }

        // ChatFormatting colours the tooltip uses.
        constexpr uint32_t kBlue       = 0xFF5555FFu;
        constexpr uint32_t kRed        = 0xFFFF5555u;
        constexpr uint32_t kGray       = 0xFFAAAAAAu;
        constexpr uint32_t kDarkPurple = 0xFFAA00AAu;
        constexpr uint32_t kWhite      = 0xFFFFFFFFu;

        // MC MobEffectCategory.getTooltipFormatting: BENEFICIAL/NEUTRAL blue,
        // HARMFUL red.
        uint32_t CategoryColor(MobEffectId id) {
            return GetEffectCategory(id) == MobEffectCategory::Harmful ? kRed : kBlue;
        }

        void WriteEffectInstance(const MobEffectInstance& e, std::vector<MobEffectInstance>& out) {
            out.push_back(MobEffectInstance(e));
        }
    } // namespace

    // ── Registry queries ────────────────────────────────────────────────────

    const PotionDef& GetPotion(PotionId id) {
        const auto& reg = Registry();
        const int i = static_cast<int>(id);
        return reg[(i >= 0 && i < kPotionCount) ? static_cast<size_t>(i) : 0];
    }

    const char* GetPotionKey(PotionId id) { return GetPotion(id).id; }

    bool IsValidPotionId(int raw) { return raw >= 0 && raw < kPotionCount; }

    bool ParsePotionId(std::string_view name, PotionId& out) {
        constexpr std::string_view kNs = "minecraft:";
        if (name.rfind(kNs, 0) == 0) name.remove_prefix(kNs.size());
        else if (name.find(':') != std::string_view::npos) return false;
        const auto& reg = Registry();
        for (size_t i = 0; i < reg.size(); ++i) {
            if (name == reg[i].id) { out = static_cast<PotionId>(i); return true; }
        }
        return false;
    }

    bool PotionHasInstantEffects(PotionId id) {
        for (const auto& e : GetPotion(id).effects) {
            if (IsInstantenousEffect(e.effect)) return true;
        }
        return false;
    }

    bool PotionDousesFire(PotionId id)          { return id == PotionId::Water; }
    bool PotionHurtsWaterSensitive(PotionId id) { return id == PotionId::Water; }
    bool PotionExtinguishesEntities(PotionId id){ return id == PotionId::Water; }

    // ── PotionContents ──────────────────────────────────────────────────────

    std::vector<MobEffectInstance> PotionContents::GetAllEffects() const {
        std::vector<MobEffectInstance> out;
        if (potion) {
            for (const auto& e : GetPotion(*potion).effects) WriteEffectInstance(e, out);
        }
        for (const auto& e : customEffects) WriteEffectInstance(e, out);
        return out;
    }

    void PotionContents::ForEachEffect(const std::function<void(MobEffectInstance)>& consumer,
                                       float durationScale) const {
        if (potion) {
            for (const auto& e : GetPotion(*potion).effects) {
                consumer(e.WithScaledDuration(durationScale));
            }
        }
        for (const auto& e : customEffects) {
            consumer(e.WithScaledDuration(durationScale));
        }
    }

    PotionContents PotionContents::WithPotion(PotionId p) const {
        PotionContents out = *this;
        out.potion = p;
        return out;
    }

    PotionContents PotionContents::WithEffectAdded(const MobEffectInstance& effect) const {
        PotionContents out = *this;
        out.customEffects.push_back(MobEffectInstance(effect));
        return out;
    }

    int32_t PotionContents::GetColorOr(int32_t defaultColor) const {
        if (customColor) return *customColor;
        if (auto mixed = GetPotionColorOptional(GetAllEffects())) return *mixed;
        return defaultColor;
    }

    bool PotionContents::HasEffects() const {
        if (!customEffects.empty()) return true;
        return potion.has_value() && !GetPotion(*potion).effects.empty();
    }

    void PotionContents::ApplyToLivingEntity(LivingEntity& entity, float durationScale) const {
        // MC: `if (entity.level() instanceof ServerLevel serverLevel)`.
        EntityLevel* level = entity.Level();
        if (!level || level->IsClientSide()) return;
        // MC: Player player = entity instanceof Player ? entity : null — the
        // drinker is both the source and the owner of an instant effect.
        Entity* player = entity.IsPlayer() ? &entity : nullptr;
        ForEachEffect([&](MobEffectInstance effect) {
            if (IsInstantenousEffect(effect.effect)) {
                ApplyInstantenousEffect(player, player, entity, effect.effect,
                                        effect.amplifier, 1.0);
            } else {
                entity.AddEffect(std::move(effect));
            }
        }, durationScale);
    }

    std::string PotionContents::GetName(std::string_view itemSlug) const {
        std::string suffix;
        if (customName) suffix = *customName;
        else if (potion) suffix = GetPotion(*potion).name;
        else suffix = "empty";
        return AlchemyTranslate("item.minecraft." + std::string(itemSlug) + ".effect." + suffix);
    }

    bool PotionContents::operator==(const PotionContents& o) const {
        if (potion != o.potion || customColor != o.customColor || customName != o.customName) {
            return false;
        }
        if (customEffects.size() != o.customEffects.size()) return false;
        for (size_t i = 0; i < customEffects.size(); ++i) {
            const auto& a = customEffects[i];
            const auto& b = o.customEffects[i];
            if (a.effect != b.effect || a.duration != b.duration || a.amplifier != b.amplifier ||
                a.ambient != b.ambient || a.visible != b.visible || a.showIcon != b.showIcon) {
                return false;
            }
        }
        return true;
    }

    std::optional<int32_t> GetPotionColorOptional(const std::vector<MobEffectInstance>& effects) {
        // MC PotionContents.getColorOptional, verbatim: amplifier+1 weights.
        int red = 0, green = 0, blue = 0, totalWeight = 0;
        for (const auto& effect : effects) {
            if (!effect.visible) continue;
            const uint32_t color = GetEffectColor(effect.effect);
            const int amplifier = effect.amplifier + 1;
            red   += amplifier * static_cast<int>((color >> 16) & 0xFF);
            green += amplifier * static_cast<int>((color >> 8) & 0xFF);
            blue  += amplifier * static_cast<int>(color & 0xFF);
            totalWeight += amplifier;
        }
        if (totalWeight == 0) return std::nullopt;
        // ARGB.color(r, g, b) — opaque.
        const uint32_t argb = 0xFF000000u |
                              (static_cast<uint32_t>(red / totalWeight) << 16) |
                              (static_cast<uint32_t>(green / totalWeight) << 8) |
                              static_cast<uint32_t>(blue / totalWeight);
        return static_cast<int32_t>(argb);
    }

    // ── Stack accessors ─────────────────────────────────────────────────────

    PotionContents GetPotionContents(const ItemStack& stack) {
        if (auto c = stack.get(DataComponents::POTION_CONTENTS)) return *c;
        return PotionContents{};
    }

    float GetPotionDurationScale(const ItemStack& stack) {
        return stack.get(DataComponents::POTION_DURATION_SCALE).value_or(1.0f);
    }

    ItemStack CreatePotionItemStack(ItemID item, PotionId potion) {
        ItemStack stack(item, 1);
        stack.components.set(DataComponents::POTION_CONTENTS, PotionContents(potion));
        return stack;
    }

    bool IsPotionNamedItem(ItemID item) {
        return item == Items::Potion || item == Items::SplashPotion ||
               item == Items::LingeringPotion || item == Items::TippedArrow;
    }

    // ── Tooltip ─────────────────────────────────────────────────────────────

    std::string GetPotionEffectDescription(MobEffectId effect, int amplifier) {
        std::string line = GetEffectDisplayName(effect);
        if (amplifier > 0) {
            // potion.withAmplifier "%s %s" + potion.potency.<amplifier>.
            line = Format2(AlchemyTranslate("potion.withAmplifier"), line,
                           AlchemyTranslate("potion.potency." + std::to_string(amplifier)));
        }
        return line;
    }

    void AddPotionTooltip(const std::vector<MobEffectInstance>& effects,
                          std::vector<PotionTooltipLine>& out,
                          float durationScale, float tickrate) {
        // MC PotionContents.addPotionTooltip, line for line.
        struct Mod { Attribute attribute; double amount; AttributeOperation op; };
        std::vector<Mod> modifiers;
        bool noEffects = true;

        for (const auto& effect : effects) {
            noEffects = false;
            const int amplifier = effect.amplifier;
            // MobEffect.createModifiers(amplifier, …): template amount * (amp+1).
            const MobEffectInfo& info = GetEffectInfo(effect.effect);
            for (int m = 0; m < info.modifierCount; ++m) {
                modifiers.push_back({info.modifier.attribute,
                                     info.modifier.amount * static_cast<double>(amplifier + 1),
                                     info.modifier.operation});
            }
            std::string line = GetPotionEffectDescription(effect.effect, amplifier);
            if (!effect.EndsWithin(20)) {
                // MobEffectUtil.formatDuration(effect, durationScale, tickrate):
                // floor(duration * scale) ticks, "∞" for an infinite one.
                MobEffectInstance scaled(effect);
                if (!scaled.IsInfiniteDuration()) {
                    scaled.duration = static_cast<int>(
                        std::floor(static_cast<float>(scaled.duration) * durationScale));
                }
                line = Format2(AlchemyTranslate("potion.withDuration"), line,
                               FormatEffectDuration(scaled, tickrate));
            }
            out.push_back({std::move(line), CategoryColor(effect.effect)});
        }

        if (noEffects) {
            out.push_back({AlchemyTranslate("effect.none"), kGray});   // NO_EFFECT
        }

        if (!modifiers.empty()) {
            out.push_back({"", kWhite});                                // CommonComponents.EMPTY
            out.push_back({AlchemyTranslate("potion.whenDrank"), kDarkPurple});
            for (const Mod& mod : modifiers) {
                const double amount = mod.amount;
                double display = (mod.op == AttributeOperation::AddMultipliedBase ||
                                  mod.op == AttributeOperation::AddMultipliedTotal)
                                     ? amount * 100.0 : amount;
                const std::string opId = std::to_string(static_cast<int>(mod.op));
                const std::string attrName = AlchemyTranslate(
                    "attribute.name." +
                    std::string(kAttributeTable[static_cast<size_t>(mod.attribute)].name));
                if (amount > 0.0) {
                    out.push_back({Format2(AlchemyTranslate("attribute.modifier.plus." + opId),
                                           FormatModifierAmount(display), attrName),
                                   kBlue});
                } else if (amount < 0.0) {
                    display *= -1.0;
                    out.push_back({Format2(AlchemyTranslate("attribute.modifier.take." + opId),
                                           FormatModifierAmount(display), attrName),
                                   kRed});
                }
            }
        }
    }

    // ── Language ────────────────────────────────────────────────────────────

    void LoadAlchemyLanguage(const std::unordered_map<std::string, std::string>& lang) {
        std::lock_guard<std::mutex> lock(g_langMutex);
        g_lang.clear();
        for (const auto& [key, value] : lang) {
            if (WantsKey(key)) g_lang.emplace(key, value);
        }
    }

    std::string AlchemyTranslate(const std::string& key) {
        std::lock_guard<std::mutex> lock(g_langMutex);
        auto it = g_lang.find(key);
        return it != g_lang.end() ? it->second : key;
    }

    // ── Suspicious stew ─────────────────────────────────────────────────────

    bool SuspiciousStewEffects::operator==(const SuspiciousStewEffects& o) const {
        if (effects.size() != o.effects.size()) return false;
        for (size_t i = 0; i < effects.size(); ++i) {
            if (effects[i].effect != o.effects[i].effect ||
                effects[i].duration != o.effects[i].duration) return false;
        }
        return true;
    }

    namespace {
        struct FlowerRow {
            BlockID     block;
            MobEffectId effect;
            float       seconds;
        };

        // Items.java registration order (SuspiciousEffectHolder.
        // getAllEffectHolders streams BuiltInRegistries.ITEM); effect and
        // seconds from each Blocks.java `new FlowerBlock(effect, seconds, …)`,
        // the eyeblossoms from EyeblossomBlock.Type.
        const std::vector<FlowerRow>& FlowerRows() {
            static const std::vector<FlowerRow> kRows = {
                {BlockID::Dandelion,        E::Saturation,     0.35f},
                {BlockID::GoldenDandelion,  E::Saturation,     0.35f},
                {BlockID::OpenEyeblossom,   E::Blindness,      11.0f},
                {BlockID::ClosedEyeblossom, E::Nausea,         7.0f},
                {BlockID::Poppy,            E::NightVision,    5.0f},
                {BlockID::BlueOrchid,       E::Saturation,     0.35f},
                {BlockID::Allium,           E::FireResistance, 3.0f},
                {BlockID::AzureBluet,       E::Blindness,      11.0f},
                {BlockID::RedTulip,         E::Weakness,       7.0f},
                {BlockID::OrangeTulip,      E::Weakness,       7.0f},
                {BlockID::WhiteTulip,       E::Weakness,       7.0f},
                {BlockID::PinkTulip,        E::Weakness,       7.0f},
                {BlockID::OxeyeDaisy,       E::Regeneration,   7.0f},
                {BlockID::Cornflower,       E::JumpBoost,      5.0f},
                {BlockID::LilyOfTheValley,  E::Poison,         11.0f},
                {BlockID::WitherRose,       E::Wither,         7.0f},
                {BlockID::Torchflower,      E::NightVision,    5.0f},
            };
            return kRows;
        }

        struct FlowerTable {
            std::unordered_map<uint16_t, SuspiciousStewEffects> byBlock;
            std::vector<BlockID> order;
        };

        const FlowerTable& Flowers() {
            static const FlowerTable table = [] {
                FlowerTable t;
                for (const FlowerRow& row : FlowerRows()) {
                    SuspiciousStewEffects effects;
                    // FlowerBlock.makeEffectList: Mth.floor(seconds * 20.0F).
                    effects.effects.push_back(
                        {row.effect, static_cast<int>(std::floor(row.seconds * 20.0f))});
                    t.byBlock.emplace(static_cast<uint16_t>(row.block), std::move(effects));
                    t.order.push_back(row.block);
                }
                return t;
            }();
            return table;
        }
    } // namespace

    const SuspiciousStewEffects* GetFlowerSuspiciousEffects(BlockID block) {
        const auto& table = Flowers();
        auto it = table.byBlock.find(static_cast<uint16_t>(block));
        return it != table.byBlock.end() ? &it->second : nullptr;
    }

    const std::vector<BlockID>& GetSuspiciousEffectFlowers() {
        return Flowers().order;
    }

} // namespace Game
