// File: src/server/world/storage/anvil/components/ConsumableNbt.cpp
//
// NBT codecs (MC DataComponentType.codec() over NbtOps) for the consumable
// components, registered with ComponentNbt: consumable, food, use_remainder
// (their types live in DataComponents.hpp), use_cooldown, use_effects,
// death_protection (ConsumableComponents.hpp).
#include "server/world/storage/anvil/ComponentNbt.hpp"
#include "server/world/storage/anvil/ItemStackNbt.hpp"
#include "common/data/components/ConsumableComponents.hpp"
#include "common/entity/effect/MobEffects.hpp"

#include <cmath>

namespace Game::Anvil::ComponentNbt {

    namespace {

        std::optional<bool> BoolKey(const ::World::NBTTagCompound& c, const char* key) {
            auto tag = c.GetTag(key);
            return tag ? BoolOf(*tag) : std::nullopt;
        }

        std::optional<double> NumberKey(const ::World::NBTTagCompound& c, const char* key) {
            auto tag = c.GetTag(key);
            return tag ? NumberOf(*tag) : std::nullopt;
        }

        // SoundEvent.CODEC: an id, or {sound_id, range?}.
        std::optional<std::string> SoundKey(const ::World::NBTTagCompound& c, const char* key) {
            auto tag = c.GetTag(key);
            if (!tag) return std::nullopt;
            if (auto s = StringOf(*tag)) return std::string(StripMinecraft(*s));
            if (const auto* d = AsCompound(tag.get())) {
                const std::string id = d->GetValue<std::string>("sound_id", "");
                if (!id.empty()) return std::string(StripMinecraft(id));
            }
            return std::nullopt;
        }

        // MobEffectInstance.CODEC: {id, amplifier = 0, duration = 0,
        // ambient = false, show_particles = true, show_icon = show_particles}.
        void WriteEffectBody(Nbt::Writer& w, const MobEffectInstance& e) {
            w.String("id", "minecraft:" + std::string(GetEffectName(e.effect)));
            if (e.amplifier != 0) w.Byte("amplifier", static_cast<int8_t>(e.amplifier));
            if (e.duration != 0) w.Int("duration", e.duration);
            if (e.ambient) w.Bool("ambient", true);
            if (!e.visible) w.Bool("show_particles", false);
            if (e.showIcon != e.visible) w.Bool("show_icon", e.showIcon);
        }

        bool ReadEffect(const ::World::NBTTagCompound& c, MobEffectInstance& out, const ReadContext& ctx) {
            const std::string id = c.GetValue<std::string>("id", "");
            MobEffectId effect;
            if (!ParseEffectId(id, effect)) {
                ctx.Fail("Unknown registry key in ResourceKey[minecraft:root / minecraft:mob_effect]: " + WithNamespace(id));
                return false;
            }
            const bool visible = BoolKey(c, "show_particles").value_or(true);
            const bool icon = BoolKey(c, "show_icon").value_or(visible);
            out = MobEffectInstance(effect, static_cast<int>(NumberKey(c, "duration").value_or(0.0)),
                                    static_cast<int>(NumberKey(c, "amplifier").value_or(0.0)) & 0xFF,
                                    BoolKey(c, "ambient").value_or(false), visible, icon);
            return true;
        }

        // ConsumeEffect.CODEC: dispatched on "type".
        void WriteConsumeEffectBody(Nbt::Writer& w, const ConsumeEffect& e) {
            w.String("type", "minecraft:" + std::string(ConsumeEffectTypeName(e.type)));
            switch (e.type) {
                case ConsumeEffect::Type::ApplyStatusEffects: {
                    auto list = w.BeginList("effects", Nbt::TagType::Compound);
                    for (const MobEffectInstance& inst : e.effects) {
                        w.ListCompoundBegin(list);
                        WriteEffectBody(w, inst);
                        w.ListCompoundEnd(list);
                    }
                    w.EndList(list);
                    if (e.probability != 1.0f) w.Float("probability", e.probability);
                    break;
                }
                case ConsumeEffect::Type::RemoveStatusEffects: {
                    std::vector<std::string> ids;
                    for (const std::string& id : e.removeEffects) ids.push_back(WithNamespace(id));
                    WriteHolderSet(w, "effects", ids);
                    break;
                }
                case ConsumeEffect::Type::ClearAllStatusEffects:
                    break;
                case ConsumeEffect::Type::TeleportRandomly:
                    if (e.diameter != 16.0f) w.Float("diameter", e.diameter);
                    if (!e.directionalParticles) w.Bool("directional_particles", false);
                    break;
                case ConsumeEffect::Type::PlaySound:
                    w.String("sound", WithNamespace(e.sound));
                    break;
            }
        }

        bool ReadConsumeEffect(const ::World::NBTTag& tag, ConsumeEffect& out, const ReadContext& ctx) {
            const auto* c = AsCompound(Unwrap(&tag));
            if (!c) { ctx.Fail("Not a map"); return false; }
            const std::string type = c->GetValue<std::string>("type", "");
            if (!ConsumeEffectTypeFromName(type, out.type)) {
                ctx.Fail("Unknown registry key in ResourceKey[minecraft:root / minecraft:consume_effect_type]: " +
                         WithNamespace(type));
                return false;
            }
            switch (out.type) {
                case ConsumeEffect::Type::ApplyStatusEffects: {
                    auto effectsTag = c->GetTag("effects");
                    if (!effectsTag) { ctx.Fail("No key effects in MapLike"); return false; }
                    if (const auto* list = AsList(effectsTag.get())) {
                        for (const auto& element : list->value) {
                            const auto* ec = AsCompound(Unwrap(element.get()));
                            if (!ec) { ctx.Fail("Not a map"); return false; }
                            MobEffectInstance inst;
                            if (!ReadEffect(*ec, inst, ctx)) return false;
                            out.effects.push_back(std::move(inst));
                        }
                    } else if (const auto* single = AsCompound(effectsTag.get())) {
                        // ExtraCodecs.compactListCodec: one effect on its own.
                        MobEffectInstance inst;
                        if (!ReadEffect(*single, inst, ctx)) return false;
                        out.effects.push_back(std::move(inst));
                    }
                    const double p = NumberKey(*c, "probability").value_or(1.0);
                    if (p < 0.0 || p > 1.0) { ctx.Fail("Value " + std::to_string(p) + " outside of range [0.0:1.0]"); return false; }
                    out.probability = static_cast<float>(p);
                    return true;
                }
                case ConsumeEffect::Type::RemoveStatusEffects:
                    out.removeEffects = ReadHolderSet(c->GetTag("effects").get());
                    for (std::string& id : out.removeEffects) id = WithNamespace(id);
                    return true;
                case ConsumeEffect::Type::ClearAllStatusEffects:
                    return true;
                case ConsumeEffect::Type::TeleportRandomly: {
                    const double d = NumberKey(*c, "diameter").value_or(16.0);
                    if (!(d > 0.0)) { ctx.Fail("Value must be positive: " + std::to_string(d)); return false; }
                    out.diameter = static_cast<float>(d);
                    out.directionalParticles = BoolKey(*c, "directional_particles").value_or(true);
                    return true;
                }
                case ConsumeEffect::Type::PlaySound: {
                    auto sound = SoundKey(*c, "sound");
                    if (!sound) { ctx.Fail("No key sound in MapLike"); return false; }
                    out.sound = *sound;
                    return true;
                }
            }
            return false;
        }

        void WriteConsumeEffectList(Nbt::Writer& w, std::string_view key, const std::vector<ConsumeEffect>& effects) {
            auto list = w.BeginList(key, Nbt::TagType::Compound);
            for (const ConsumeEffect& e : effects) {
                w.ListCompoundBegin(list);
                WriteConsumeEffectBody(w, e);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }

        bool ReadConsumeEffectList(const ::World::NBTTag* tag, std::vector<ConsumeEffect>& out, const ReadContext& ctx) {
            if (!tag) return true;
            if (const auto* list = AsList(tag)) {
                for (const auto& element : list->value) {
                    if (!element) continue;
                    ConsumeEffect e;
                    if (!ReadConsumeEffect(*element, e, ctx)) return false;
                    out.push_back(std::move(e));
                }
                return true;
            }
            ctx.Fail("Not a list");
            return false;
        }

        // ── consumable ────────────────────────────────────────────────────
        void WriteConsumable(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto c = stack.components.get(DataComponents::CONSUMABLE);
            if (!c) return;
            w.BeginCompound(key);
            if (c->consumeSeconds != 1.6f) w.Float("consume_seconds", c->consumeSeconds);
            if (c->animation != ItemUseAnimation::EAT) w.String("animation", std::string(ItemUseAnimationName(c->animation)));
            if (c->sound != "entity.generic.eat") w.String("sound", WithNamespace(c->sound));
            if (!c->hasConsumeParticles) w.Bool("has_consume_particles", false);
            if (!c->onConsumeEffects.empty()) WriteConsumeEffectList(w, "on_consume_effects", c->onConsumeEffects);
            w.EndCompound();
        }

        bool ReadConsumable(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            Consumable v;
            if (auto s = NumberKey(*c, "consume_seconds")) {
                if (*s < 0.0) { ctx.Fail("Value must be non-negative: " + std::to_string(*s)); return false; }
                v.consumeSeconds = static_cast<float>(*s);
            }
            if (auto a = c->GetTag("animation")) {
                const std::optional<std::string> name = StringOf(*a);
                if (!name || !ItemUseAnimationFromName(*name, v.animation)) {
                    ctx.Fail("Unknown element name:" + name.value_or("?"));
                    return false;
                }
            }
            if (auto sound = SoundKey(*c, "sound")) v.sound = *sound;
            if (auto p = BoolKey(*c, "has_consume_particles")) v.hasConsumeParticles = *p;
            if (!ReadConsumeEffectList(c->GetTag("on_consume_effects").get(), v.onConsumeEffects, ctx)) return false;
            stack.components.set(DataComponents::CONSUMABLE, std::move(v));
            return true;
        }

        // ── food ──────────────────────────────────────────────────────────
        void WriteFood(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto f = stack.components.get(DataComponents::FOOD);
            if (!f) return;
            w.BeginCompound(key);
            w.Int("nutrition", f->nutrition);
            w.Float("saturation", f->saturation);
            if (f->canAlwaysEat) w.Bool("can_always_eat", true);
            w.EndCompound();
        }

        bool ReadFood(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            const auto nutrition = NumberKey(*c, "nutrition");
            const auto saturation = NumberKey(*c, "saturation");
            if (!nutrition) { ctx.Fail("No key nutrition in MapLike"); return false; }
            if (!saturation) { ctx.Fail("No key saturation in MapLike"); return false; }
            if (*nutrition < 0.0) { ctx.Fail("Value must be non-negative: " + std::to_string(*nutrition)); return false; }
            FoodProperties f;
            f.nutrition = static_cast<int>(*nutrition);
            f.saturation = static_cast<float>(*saturation);
            f.canAlwaysEat = BoolKey(*c, "can_always_eat").value_or(false);
            stack.components.set(DataComponents::FOOD, f);
            return true;
        }

        // ── use_remainder ({convert_into: ItemStackTemplate}) ──────────────
        void WriteUseRemainder(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto r = stack.components.get(DataComponents::USE_REMAINDER);
            if (!r || r->convertInto.IsEmpty()) return;
            w.BeginCompound(key);
            w.BeginCompound("convert_into");
            WriteItemStackBody(w, r->convertInto);
            w.EndCompound();
            w.EndCompound();
        }

        bool ReadUseRemainder(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            ItemStack into;
            auto convert = c->GetTag("convert_into");
            if (const auto* item = AsCompound(convert.get())) into = ReadItemStack(*item);
            else if (convert) if (auto id = StringOf(*convert)) into = ItemStack(ItemFromName(*id), 1);
            if (into.IsEmpty()) { ctx.Fail("No key convert_into in MapLike"); return false; }
            stack.components.set(DataComponents::USE_REMAINDER, UseRemainder{std::move(into)});
            return true;
        }

        // ── use_cooldown ({seconds, cooldown_group?}) ──────────────────────
        void WriteUseCooldown(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto u = stack.components.get(DataComponents::USE_COOLDOWN);
            if (!u) return;
            w.BeginCompound(key);
            w.Float("seconds", u->seconds);
            if (!u->cooldownGroup.empty()) w.String("cooldown_group", WithNamespace(u->cooldownGroup));
            w.EndCompound();
        }

        bool ReadUseCooldown(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            const auto seconds = NumberKey(*c, "seconds");
            if (!seconds) { ctx.Fail("No key seconds in MapLike"); return false; }
            if (!(*seconds > 0.0)) { ctx.Fail("Value must be positive: " + std::to_string(*seconds)); return false; }
            UseCooldown u;
            u.seconds = static_cast<float>(*seconds);
            if (auto group = c->GetTag("cooldown_group")) {
                if (auto s = StringOf(*group)) u.cooldownGroup = WithNamespace(*s);
            }
            stack.components.set(DataComponents::USE_COOLDOWN, std::move(u));
            return true;
        }

        // ── use_effects ────────────────────────────────────────────────────
        void WriteUseEffects(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto u = stack.components.get(DataComponents::USE_EFFECTS);
            if (!u) return;
            const UseEffects defaults;
            w.BeginCompound(key);
            if (u->canSprint != defaults.canSprint) w.Bool("can_sprint", u->canSprint);
            if (u->interactVibrations != defaults.interactVibrations) w.Bool("interact_vibrations", u->interactVibrations);
            if (u->speedMultiplier != defaults.speedMultiplier) w.Float("speed_multiplier", u->speedMultiplier);
            w.EndCompound();
        }

        bool ReadUseEffects(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            UseEffects u;
            if (auto v = BoolKey(*c, "can_sprint")) u.canSprint = *v;
            if (auto v = BoolKey(*c, "interact_vibrations")) u.interactVibrations = *v;
            if (auto v = NumberKey(*c, "speed_multiplier")) {
                if (*v < 0.0 || *v > 1.0) { ctx.Fail("Value " + std::to_string(*v) + " outside of range [0.0:1.0]"); return false; }
                u.speedMultiplier = static_cast<float>(*v);
            }
            stack.components.set(DataComponents::USE_EFFECTS, u);
            return true;
        }

        // ── death_protection ({death_effects: [ConsumeEffect]}) ────────────
        void WriteDeathProtection(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto d = stack.components.get(DataComponents::DEATH_PROTECTION);
            if (!d) return;
            w.BeginCompound(key);
            if (!d->deathEffects.empty()) WriteConsumeEffectList(w, "death_effects", d->deathEffects);
            w.EndCompound();
        }

        bool ReadDeathProtection(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            DeathProtection d;
            if (!ReadConsumeEffectList(c->GetTag("death_effects").get(), d.deathEffects, ctx)) return false;
            stack.components.set(DataComponents::DEATH_PROTECTION, std::move(d));
            return true;
        }

        const Registrar kConsumable{DataComponents::CONSUMABLE, &WriteConsumable, &ReadConsumable};
        const Registrar kFood{DataComponents::FOOD, &WriteFood, &ReadFood};
        const Registrar kUseRemainder{DataComponents::USE_REMAINDER, &WriteUseRemainder, &ReadUseRemainder};
        const Registrar kUseCooldown{DataComponents::USE_COOLDOWN, &WriteUseCooldown, &ReadUseCooldown};
        const Registrar kUseEffects{DataComponents::USE_EFFECTS, &WriteUseEffects, &ReadUseEffects};
        const Registrar kDeathProtection{DataComponents::DEATH_PROTECTION, &WriteDeathProtection, &ReadDeathProtection};

    } // namespace

} // namespace Game::Anvil::ComponentNbt
