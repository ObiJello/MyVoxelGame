// File: src/common/data/components/ConsumableComponents.cpp
#include "ConsumableComponents.hpp"

#include "common/core/Log.hpp"
#include "common/entity/GeneratedItemList.hpp"

#include <stdexcept>
#include <string>

namespace Game {

    namespace {
        constexpr std::string_view kEffectTypeNames[] = {
            "apply_effects", "remove_effects", "clear_all_effects", "teleport_randomly", "play_sound" };
        constexpr std::string_view kAnimationNames[] = {
            "none", "eat", "drink", "block", "bow", "trident", "crossbow", "spyglass", "toot_horn",
            "brush", "bundle", "spear" };

        std::string_view StripMinecraft(std::string_view s) {
            return s.rfind("minecraft:", 0) == 0 ? s.substr(10) : s;
        }
    }

    DeathProtection DeathProtection::TotemOfUndying() {
        DeathProtection p;
        p.deathEffects.push_back(ConsumeEffect::ClearAllEffects());
        p.deathEffects.push_back(ConsumeEffect::ApplyEffects({
            MobEffectInstance(MobEffectId::Regeneration, 900, 1),
            MobEffectInstance(MobEffectId::Absorption, 100, 1),
            MobEffectInstance(MobEffectId::FireResistance, 800, 0) }));
        return p;
    }

    UseEffects GetUseEffects(const ItemStack& stack) {
        if (auto effects = stack.get(DataComponents::USE_EFFECTS)) return *effects;
        return UseEffects{};
    }

    std::string_view ConsumeEffectTypeName(ConsumeEffect::Type type) {
        const auto i = static_cast<size_t>(type);
        return i < std::size(kEffectTypeNames) ? kEffectTypeNames[i] : kEffectTypeNames[2];
    }

    bool ConsumeEffectTypeFromName(std::string_view name, ConsumeEffect::Type& out) {
        name = StripMinecraft(name);
        for (size_t i = 0; i < std::size(kEffectTypeNames); ++i) {
            if (kEffectTypeNames[i] == name) { out = static_cast<ConsumeEffect::Type>(i); return true; }
        }
        return false;
    }

    std::string_view ItemUseAnimationName(ItemUseAnimation animation) {
        const auto i = static_cast<size_t>(animation);
        return i < std::size(kAnimationNames) ? kAnimationNames[i] : kAnimationNames[0];
    }

    bool ItemUseAnimationFromName(std::string_view name, ItemUseAnimation& out) {
        name = StripMinecraft(name);
        for (size_t i = 0; i < std::size(kAnimationNames); ++i) {
            if (kAnimationNames[i] == name) { out = static_cast<ItemUseAnimation>(i); return true; }
        }
        return false;
    }

    void WriteEffectInstance(Network::PacketBuffer& b, const MobEffectInstance& e) {
        b.WriteVarInt(static_cast<uint32_t>(e.effect));
        b.WriteVarInt(static_cast<uint32_t>(e.amplifier));
        b.WriteVarInt(static_cast<uint32_t>(e.duration));   // -1 = infinite
        b.WriteByte(e.ambient ? 1 : 0);
        b.WriteByte(e.visible ? 1 : 0);
        b.WriteByte(e.showIcon ? 1 : 0);
    }

    MobEffectInstance ReadEffectInstance(Network::PacketReader& r) {
        const uint32_t raw = r.ReadVarInt();
        if (!IsValidEffectId(static_cast<int>(raw))) {
            throw std::runtime_error("consume effect: mob effect id out of range: " + std::to_string(raw));
        }
        const int amplifier = static_cast<int>(r.ReadVarInt());
        const int duration  = static_cast<int>(r.ReadVarInt());
        const bool ambient  = r.ReadByte() != 0;
        const bool visible  = r.ReadByte() != 0;
        const bool icon     = r.ReadByte() != 0;
        return MobEffectInstance(static_cast<MobEffectId>(raw), duration, amplifier, ambient, visible, icon);
    }

    void WriteConsumeEffect(Network::PacketBuffer& b, const ConsumeEffect& e) {
        b.WriteVarInt(static_cast<uint32_t>(e.type));
        switch (e.type) {
            case ConsumeEffect::Type::ApplyStatusEffects:
                // ApplyStatusEffectsConsumeEffect.STREAM_CODEC: list, FLOAT.
                b.WriteVarInt(static_cast<uint32_t>(e.effects.size()));
                for (const MobEffectInstance& inst : e.effects) WriteEffectInstance(b, inst);
                b.WriteFloat(e.probability);
                break;
            case ConsumeEffect::Type::RemoveStatusEffects:
                // ByteBufCodecs.holderSet — the raw entries here.
                b.WriteVarInt(static_cast<uint32_t>(e.removeEffects.size()));
                for (const std::string& id : e.removeEffects) b.WriteString(id);
                break;
            case ConsumeEffect::Type::ClearAllStatusEffects:
                break;   // StreamCodec.unit
            case ConsumeEffect::Type::TeleportRandomly:
                b.WriteFloat(e.diameter);
                b.WriteByte(e.directionalParticles ? 1 : 0);
                break;
            case ConsumeEffect::Type::PlaySound:
                b.WriteString(e.sound);
                break;
        }
    }

    ConsumeEffect ReadConsumeEffect(Network::PacketReader& r) {
        ConsumeEffect e;
        const uint32_t type = r.ReadVarInt();
        if (type > static_cast<uint32_t>(ConsumeEffect::Type::PlaySound)) {
            throw std::runtime_error("consume effect: type id out of range: " + std::to_string(type));
        }
        e.type = static_cast<ConsumeEffect::Type>(type);
        switch (e.type) {
            case ConsumeEffect::Type::ApplyStatusEffects: {
                const uint32_t count = r.ReadVarInt();
                if (count > 1024) throw std::runtime_error("consume effect: too many effects");
                e.effects.reserve(count);
                for (uint32_t i = 0; i < count; ++i) e.effects.push_back(ReadEffectInstance(r));
                e.probability = r.ReadFloat();
                break;
            }
            case ConsumeEffect::Type::RemoveStatusEffects: {
                const uint32_t count = r.ReadVarInt();
                if (count > 1024) throw std::runtime_error("consume effect: too many effect ids");
                e.removeEffects.reserve(count);
                for (uint32_t i = 0; i < count; ++i) e.removeEffects.push_back(r.ReadString());
                break;
            }
            case ConsumeEffect::Type::ClearAllStatusEffects:
                break;
            case ConsumeEffect::Type::TeleportRandomly:
                e.diameter = r.ReadFloat();
                e.directionalParticles = r.ReadByte() != 0;
                break;
            case ConsumeEffect::Type::PlaySound:
                e.sound = r.ReadString();
                break;
        }
        return e;
    }

    void ItemRegistry_RegisterConsumableComponents(std::unordered_map<ItemID, Item>& pureItems) {
        const auto set = [&](ItemID id, auto&& apply) {
            auto it = pureItems.find(id);
            if (it != pureItems.end()) apply(it->second);
        };
        // Items.java: ENDER_PEARL .useCooldown(1.0F), WIND_CHARGE
        // .useCooldown(0.5F), CHORUS_FRUIT .useCooldown(1.0F).
        set(Items::EnderPearl,  [](Item& i) { i.defaultComponents.set(DataComponents::USE_COOLDOWN, UseCooldown{1.0f, {}}); });
        set(Items::WindCharge,  [](Item& i) { i.defaultComponents.set(DataComponents::USE_COOLDOWN, UseCooldown{0.5f, {}}); });
        set(Items::ChorusFruit, [](Item& i) { i.defaultComponents.set(DataComponents::USE_COOLDOWN, UseCooldown{1.0f, {}}); });
        // TOTEM_OF_UNDYING .component(DEATH_PROTECTION, TOTEM_OF_UNDYING).
        set(Items::TotemOfUndying, [](Item& i) {
            i.defaultComponents.set(DataComponents::DEATH_PROTECTION, DeathProtection::TotemOfUndying());
        });
        // Item.Properties.spear: .component(USE_EFFECTS, new UseEffects(true,
        // false, 1.0F)) — every <material>_spear.
        size_t spears = 0;
        for (auto& [id, item] : pureItems) {
            const std::string_view slug = ItemRegistry::Slug(id);
            constexpr std::string_view kSuffix = "_spear";
            if (slug.size() > kSuffix.size() && slug.substr(slug.size() - kSuffix.size()) == kSuffix) {
                item.defaultComponents.set(DataComponents::USE_EFFECTS, UseEffects{true, false, 1.0f});
                ++spears;
            }
        }
        Log::Info("[ItemRegistry] use_cooldown / death_protection defaults set; use_effects on %zu spears", spears);
    }

} // namespace Game

namespace Game::DataComponents {

    namespace {
        // UseCooldown.STREAM_CODEC: FLOAT seconds, optional Identifier.
        void SerUseCooldown(Network::PacketBuffer& b, const UseCooldown& v) {
            b.WriteFloat(v.seconds);
            b.WriteByte(v.cooldownGroup.empty() ? 0 : 1);
            if (!v.cooldownGroup.empty()) b.WriteString(v.cooldownGroup);
        }
        UseCooldown DeUseCooldown(Network::PacketReader& r) {
            UseCooldown v;
            v.seconds = r.ReadFloat();
            if (r.ReadByte() != 0) v.cooldownGroup = r.ReadString();
            return v;
        }

        // UseEffects.STREAM_CODEC: BOOL, BOOL, FLOAT.
        void SerUseEffects(Network::PacketBuffer& b, const UseEffects& v) {
            b.WriteByte(v.canSprint ? 1 : 0);
            b.WriteByte(v.interactVibrations ? 1 : 0);
            b.WriteFloat(v.speedMultiplier);
        }
        UseEffects DeUseEffects(Network::PacketReader& r) {
            UseEffects v;
            v.canSprint = r.ReadByte() != 0;
            v.interactVibrations = r.ReadByte() != 0;
            v.speedMultiplier = r.ReadFloat();
            return v;
        }

        // DeathProtection.STREAM_CODEC: a ConsumeEffect list.
        void SerDeathProtection(Network::PacketBuffer& b, const DeathProtection& v) {
            b.WriteVarInt(static_cast<uint32_t>(v.deathEffects.size()));
            for (const ConsumeEffect& e : v.deathEffects) WriteConsumeEffect(b, e);
        }
        DeathProtection DeDeathProtection(Network::PacketReader& r) {
            DeathProtection v;
            const uint32_t count = r.ReadVarInt();
            if (count > 256) throw std::runtime_error("death_protection: too many effects");
            v.deathEffects.reserve(count);
            for (uint32_t i = 0; i < count; ++i) v.deathEffects.push_back(ReadConsumeEffect(r));
            return v;
        }
    } // namespace

    const DataComponentType<UseCooldown>     USE_COOLDOWN    {"use_cooldown",     230, &SerUseCooldown,     &DeUseCooldown};
    const DataComponentType<UseEffects>      USE_EFFECTS     {"use_effects",      231, &SerUseEffects,      &DeUseEffects};
    const DataComponentType<DeathProtection> DEATH_PROTECTION{"death_protection", 232, &SerDeathProtection, &DeDeathProtection};

} // namespace Game::DataComponents
