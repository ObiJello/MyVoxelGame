// File: src/common/data/DataComponents.cpp
//
// Component type definitions + the network-id registry + per-component wire
// codecs. Codec field order mirrors each MC component's STREAM_CODEC (cited
// per function) so the C++ side can be diffed against MC's source.
#include "DataComponents.hpp"
#include "../network/ItemStackSerialization.hpp"

#include <unordered_map>

namespace Game {

    namespace {
        // Meyers singleton so self-registration from the component globals'
        // constructors (this TU and any other) is safe regardless of
        // static-init order across TUs.
        std::unordered_map<uint32_t, const DataComponentTypeBase*>& NetworkIdRegistry() {
            static std::unordered_map<uint32_t, const DataComponentTypeBase*> registry;
            return registry;
        }
    }

    DataComponentTypeBase::DataComponentTypeBase(std::string n, uint32_t netId)
        : name(std::move(n)), networkId(netId) {
        if (netId != 0) {
            NetworkIdRegistry()[netId] = this;
        }
    }

    const DataComponentTypeBase* DataComponents::ById(uint32_t networkId) {
        const auto& registry = NetworkIdRegistry();
        auto it = registry.find(networkId);
        return it != registry.end() ? it->second : nullptr;
    }

} // namespace Game

namespace Game::DataComponents {

    // ── Wire codecs ─────────────────────────────────────────────────────────
    // Free functions passed into each DataComponentType below. Kept in an
    // anonymous namespace; only the typed key objects reference them.
    namespace {

        void SerBool(Network::PacketBuffer& b, const bool& v)   { b.WriteByte(v ? 1 : 0); }
        bool DeBool(Network::PacketReader& r)                   { return r.ReadByte() != 0; }

        // Mirrors ItemEnchantments.STREAM_CODEC (ItemEnchantments.java) —
        // a map codec: VarInt count + per entry (enchantment id, VarInt level).
        void SerEnchantments(Network::PacketBuffer& b, const ItemEnchantments& v) {
            b.WriteVarInt(static_cast<uint32_t>(v.entries.size()));
            for (const auto& e : v.entries) {
                b.WriteVarInt(e.id);
                b.WriteVarInt(static_cast<uint32_t>(e.level));
            }
        }
        ItemEnchantments DeEnchantments(Network::PacketReader& r) {
            ItemEnchantments v;
            const uint32_t count = r.ReadVarInt();
            v.entries.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                EnchantmentInstance inst;
                inst.id    = static_cast<EnchantmentId>(r.ReadVarInt());
                inst.level = static_cast<int>(r.ReadVarInt());
                v.entries.push_back(inst);
            }
            return v;
        }

        // Our Tool collapses MC Tool.java's rules list to (type, tier, speed) —
        // wire matches the struct, not MC's rules list.
        void SerTool(Network::PacketBuffer& b, const Tool& v) {
            b.WriteByte(static_cast<uint8_t>(v.type));
            b.WriteByte(static_cast<uint8_t>(v.tier));
            b.WriteFloat(v.miningSpeed);
        }
        Tool DeTool(Network::PacketReader& r) {
            Tool v;
            v.type        = static_cast<ToolType>(r.ReadByte());
            v.tier        = static_cast<MiningTier>(r.ReadByte());
            v.miningSpeed = r.ReadFloat();
            return v;
        }

        // Field order mirrors Consumable.STREAM_CODEC (Consumable.java:118-120):
        // consumeSeconds, animation, sound, hasConsumeParticles, effects list.
        void SerConsumable(Network::PacketBuffer& b, const Consumable& v) {
            b.WriteFloat(v.consumeSeconds);
            b.WriteByte(static_cast<uint8_t>(v.animation));
            b.WriteString(v.sound);
            b.WriteByte(v.hasConsumeParticles ? 1 : 0);
            b.WriteVarInt(static_cast<uint32_t>(v.onConsumeEffects.size()));
            for (const auto& e : v.onConsumeEffects) {
                b.WriteByte(static_cast<uint8_t>(e.type));
                b.WriteString(e.payload);
            }
        }
        Consumable DeConsumable(Network::PacketReader& r) {
            Consumable v;
            v.consumeSeconds      = r.ReadFloat();
            v.animation           = static_cast<ItemUseAnimation>(r.ReadByte());
            v.sound               = r.ReadString();
            v.hasConsumeParticles = r.ReadByte() != 0;
            const uint32_t count  = r.ReadVarInt();
            v.onConsumeEffects.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                ConsumeEffect e;
                e.type    = static_cast<ConsumeEffect::Type>(r.ReadByte());
                e.payload = r.ReadString();
                v.onConsumeEffects.push_back(std::move(e));
            }
            return v;
        }

        // Field order mirrors FoodProperties.DIRECT_STREAM_CODEC
        // (FoodProperties.java:36-38): VarInt nutrition, float saturation, bool.
        void SerFood(Network::PacketBuffer& b, const FoodProperties& v) {
            b.WriteVarInt(static_cast<uint32_t>(v.nutrition));
            b.WriteFloat(v.saturation);
            b.WriteByte(v.canAlwaysEat ? 1 : 0);
        }
        FoodProperties DeFood(Network::PacketReader& r) {
            FoodProperties v;
            v.nutrition    = static_cast<int>(r.ReadVarInt());
            v.saturation   = r.ReadFloat();
            v.canAlwaysEat = r.ReadByte() != 0;
            return v;
        }

        // Mirrors UseRemainder.STREAM_CODEC (UseRemainder.java:43-46) —
        // a nested full ItemStack.
        void SerUseRemainder(Network::PacketBuffer& b, const UseRemainder& v) {
            Network::Serialization::WriteItemStack(b, v.convertInto);
        }
        UseRemainder DeUseRemainder(Network::PacketReader& r) {
            UseRemainder v;
            v.convertInto = Network::Serialization::ReadItemStack(r);
            return v;
        }

        void SerString(Network::PacketBuffer& b, const std::string& v) { b.WriteString(v); }
        std::string DeString(Network::PacketReader& r)                 { return r.ReadString(); }

        // Mirrors ItemLore.STREAM_CODEC — a bounded list of Components
        // (plain strings here). MAX_LINES = 256 (ItemLore.java:22).
        void SerLore(Network::PacketBuffer& b, const ItemLore& v) {
            const uint32_t count = static_cast<uint32_t>(
                v.lines.size() > 256 ? 256 : v.lines.size());
            b.WriteVarInt(count);
            for (uint32_t i = 0; i < count; ++i) b.WriteString(v.lines[i]);
        }
        ItemLore DeLore(Network::PacketReader& r) {
            ItemLore v;
            uint32_t count = r.ReadVarInt();
            if (count > 256) count = 256;   // MAX_LINES clamp
            v.lines.reserve(count);
            for (uint32_t i = 0; i < count; ++i) v.lines.push_back(r.ReadString());
            return v;
        }

        void SerRarity(Network::PacketBuffer& b, const Rarity& v) {
            b.WriteByte(static_cast<uint8_t>(v));
        }
        Rarity DeRarity(Network::PacketReader& r) {
            return static_cast<Rarity>(r.ReadByte());
        }

        // Field order mirrors Equippable.STREAM_CODEC (Equippable.java:106) —
        // restricted to the fields we model: slot, equipSound, swappable.
        void SerEquippable(Network::PacketBuffer& b, const Equippable& v) {
            b.WriteByte(static_cast<uint8_t>(v.slot));
            b.WriteString(v.equipSound);
            b.WriteByte(v.swappable ? 1 : 0);
        }
        Equippable DeEquippable(Network::PacketReader& r) {
            Equippable v;
            v.slot       = static_cast<EquipmentSlot>(r.ReadByte());
            v.equipSound = r.ReadString();
            v.swappable  = r.ReadByte() != 0;
            return v;
        }

        // Field order mirrors BlocksAttacks.STREAM_CODEC (BlocksAttacks.java:86):
        // delay, disableScale, reductions list, itemDamage, sounds.
        void SerBlocksAttacks(Network::PacketBuffer& b, const BlocksAttacks& v) {
            b.WriteFloat(v.blockDelaySeconds);
            b.WriteFloat(v.disableCooldownScale);
            b.WriteVarInt(static_cast<uint32_t>(v.damageReductions.size()));
            for (const auto& dr : v.damageReductions) {
                b.WriteFloat(dr.horizontalBlockingAngle);
                b.WriteFloat(dr.base);
                b.WriteFloat(dr.factor);
            }
            b.WriteFloat(v.itemDamage.threshold);
            b.WriteFloat(v.itemDamage.base);
            b.WriteFloat(v.itemDamage.factor);
            b.WriteString(v.blockSound);
            b.WriteString(v.disableSound);
        }
        BlocksAttacks DeBlocksAttacks(Network::PacketReader& r) {
            BlocksAttacks v;
            v.blockDelaySeconds    = r.ReadFloat();
            v.disableCooldownScale = r.ReadFloat();
            const uint32_t count   = r.ReadVarInt();
            v.damageReductions.clear();
            v.damageReductions.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                BlocksAttacks::DamageReduction dr;
                dr.horizontalBlockingAngle = r.ReadFloat();
                dr.base   = r.ReadFloat();
                dr.factor = r.ReadFloat();
                v.damageReductions.push_back(dr);
            }
            v.itemDamage.threshold = r.ReadFloat();
            v.itemDamage.base      = r.ReadFloat();
            v.itemDamage.factor    = r.ReadFloat();
            v.blockSound   = r.ReadString();
            v.disableSound = r.ReadString();
            return v;
        }

        // Mirrors BundleContents.STREAM_CODEC (BundleContents.java:140) —
        // a bare list of ItemStacks; selectedItem is client-only, not sent.
        void SerBundleContents(Network::PacketBuffer& b, const BundleContents& v) {
            b.WriteVarInt(static_cast<uint32_t>(v.items.size()));
            for (const auto& s : v.items) {
                Network::Serialization::WriteItemStack(b, s);
            }
        }
        BundleContents DeBundleContents(Network::PacketReader& r) {
            BundleContents v;
            const uint32_t count = r.ReadVarInt();
            v.items.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                v.items.push_back(Network::Serialization::ReadItemStack(r));
            }
            return v;
        }

#if ENABLE_PORTAL_GUN
        void SerU8(Network::PacketBuffer& b, const uint8_t& v)  { b.WriteByte(v); }
        uint8_t DeU8(Network::PacketReader& r)                  { return r.ReadByte(); }
        void SerU64(Network::PacketBuffer& b, const uint64_t& v){ b.WriteLong(v); }
        uint64_t DeU64(Network::PacketReader& r)                { return r.ReadLong(); }
#endif

    } // namespace

    // ── Component type definitions ──────────────────────────────────────────
    // Network ids are OUR stable protocol ids (see the id table in
    // DataComponents.hpp) — deliberately not MC registry ordinals.

    const DataComponentType<bool>             ENCHANTMENT_GLINT_OVERRIDE{"enchantment_glint_override", 1, &SerBool,         &DeBool};
    const DataComponentType<ItemEnchantments> STORED_ENCHANTMENTS       {"stored_enchantments",        2, &SerEnchantments, &DeEnchantments};
    const DataComponentType<Tool>             TOOL                      {"tool",                       3, &SerTool,         &DeTool};
    const DataComponentType<Consumable>       CONSUMABLE                {"consumable",                 8, &SerConsumable,   &DeConsumable};
    const DataComponentType<FoodProperties>   FOOD                      {"food",                       9, &SerFood,         &DeFood};
    const DataComponentType<UseRemainder>     USE_REMAINDER             {"use_remainder",             10, &SerUseRemainder, &DeUseRemainder};
    const DataComponentType<std::string>      CUSTOM_NAME               {"custom_name",                4, &SerString,       &DeString};
    const DataComponentType<std::string>      ITEM_NAME                 {"item_name",                  5, &SerString,       &DeString};
    const DataComponentType<ItemLore>         LORE                      {"lore",                       6, &SerLore,         &DeLore};
    const DataComponentType<Rarity>           RARITY                    {"rarity",                     7, &SerRarity,       &DeRarity};
    const DataComponentType<Equippable>       EQUIPPABLE                {"equippable",                11, &SerEquippable,   &DeEquippable};
    const DataComponentType<BlocksAttacks>    BLOCKS_ATTACKS            {"blocks_attacks",            12, &SerBlocksAttacks,&DeBlocksAttacks};
    const DataComponentType<BundleContents>   BUNDLE_CONTENTS           {"bundle_contents",           13, &SerBundleContents,&DeBundleContents};

#if ENABLE_PORTAL_GUN
    const DataComponentType<uint8_t>  PORTAL_GUN_NEXT_COLOR  {"portal_gun_next_color",  100, &SerU8,  &DeU8};
    const DataComponentType<uint64_t> PORTAL_GUN_INSTANCE_ID {"portal_gun_instance_id", 101, &SerU64, &DeU64};
#endif

} // namespace Game::DataComponents
