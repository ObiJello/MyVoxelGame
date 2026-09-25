// File: src/common/data/DataComponents.cpp
//
// Component type definitions + the network-id registry + per-component wire
// codecs. Codec field order mirrors each MC component's STREAM_CODEC (cited
// per function) so the C++ side can be diffed against MC's source.
#include "DataComponents.hpp"
#include "../network/ItemStackSerialization.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
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
                if (inst.id >= EnchantmentRegistry::All().size()) {
                    throw std::runtime_error("enchantment id out of range: " + std::to_string(inst.id));
                }
                v.Set(inst.id, inst.level);
            }
            return v;
        }

        // Our Tool collapses MC Tool.java's rules list to (type, tier, speed) —
        // wire matches the struct, not MC's rules list.
        // damagePerBlock is MC Tool.STREAM_CODEC's VAR_INT.
        void SerTool(Network::PacketBuffer& b, const Tool& v) {
            b.WriteByte(static_cast<uint8_t>(v.type));
            b.WriteByte(static_cast<uint8_t>(v.tier));
            b.WriteFloat(v.miningSpeed);
            b.WriteVarInt(static_cast<uint32_t>(v.damagePerBlock));
        }
        Tool DeTool(Network::PacketReader& r) {
            Tool v;
            v.type           = static_cast<ToolType>(r.ReadByte());
            v.tier           = static_cast<MiningTier>(r.ReadByte());
            v.miningSpeed    = r.ReadFloat();
            v.damagePerBlock = static_cast<int>(r.ReadVarInt());
            return v;
        }

        // DAMAGE / MAX_DAMAGE / REPAIR_COST / ENCHANTABLE — MC
        // ByteBufCodecs.VAR_INT (Enchantable.STREAM_CODEC is its one field).
        void SerVarInt(Network::PacketBuffer& b, const int32_t& v) { b.WriteVarInt(static_cast<uint32_t>(v)); }
        int32_t DeVarInt(Network::PacketReader& r)                 { return static_cast<int32_t>(r.ReadVarInt()); }

        // UNBREAKABLE — Unit.STREAM_CODEC writes nothing; presence is the value.
        void SerUnit(Network::PacketBuffer&, const bool&) {}
        bool DeUnit(Network::PacketReader&)               { return true; }

        // Mirrors Weapon.STREAM_CODEC: VAR_INT itemDamagePerAttack, FLOAT
        // disableBlockingForSeconds.
        void SerWeapon(Network::PacketBuffer& b, const Weapon& v) {
            b.WriteVarInt(static_cast<uint32_t>(v.itemDamagePerAttack));
            b.WriteFloat(v.disableBlockingForSeconds);
        }
        Weapon DeWeapon(Network::PacketReader& r) {
            Weapon v;
            v.itemDamagePerAttack       = static_cast<int>(r.ReadVarInt());
            v.disableBlockingForSeconds = r.ReadFloat();
            return v;
        }

        // Repairable.STREAM_CODEC is ByteBufCodecs.holderSet(ITEM) — a tag
        // key or a list of registry ids. Ours carries the raw entries.
        void SerRepairable(Network::PacketBuffer& b, const Repairable& v) {
            b.WriteVarInt(static_cast<uint32_t>(v.items.size()));
            for (const std::string& e : v.items) b.WriteString(e);
        }
        Repairable DeRepairable(Network::PacketReader& r) {
            Repairable v;
            const uint32_t count = r.ReadVarInt();
            if (count > r.Remaining()) throw std::runtime_error("repairable entry count out of range");
            v.items.reserve(count);
            for (uint32_t i = 0; i < count; ++i) v.items.push_back(r.ReadString());
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
        void SerSulfurCubeBucket(Network::PacketBuffer& b, const SulfurCubeBucketData& v) {
            b.WriteString(v.bodyItem);
            b.WriteInt(static_cast<uint32_t>(v.age));
            b.WriteByte(v.ageLocked ? 1 : 0);
            b.WriteByte(v.noAi ? 1 : 0);
        }
        SulfurCubeBucketData DeSulfurCubeBucket(Network::PacketReader& r) {
            SulfurCubeBucketData v;
            v.bodyItem  = r.ReadString();
            v.age       = static_cast<int>(r.ReadInt());
            v.ageLocked = r.ReadByte() != 0;
            v.noAi      = r.ReadByte() != 0;
            return v;
        }
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
        // restricted to the fields we model: slot, equipSound, swappable,
        // damageOnHurt.
        void SerEquippable(Network::PacketBuffer& b, const Equippable& v) {
            b.WriteByte(static_cast<uint8_t>(v.slot));
            b.WriteString(v.equipSound);
            b.WriteByte(v.swappable ? 1 : 0);
            b.WriteByte(v.damageOnHurt ? 1 : 0);
        }
        Equippable DeEquippable(Network::PacketReader& r) {
            Equippable v;
            v.slot         = static_cast<EquipmentSlot>(r.ReadByte());
            v.equipSound   = r.ReadString();
            v.swappable    = r.ReadByte() != 0;
            v.damageOnHurt = r.ReadByte() != 0;
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

        // MC MobEffectInstance.STREAM_CODEC: the effect holder, then Details
        // (VarInt amplifier, VarInt duration, bool ambient, bool
        // showParticles, bool showIcon). The hidden chain is never part of a
        // potion's contents, so it is not carried.
        void SerEffectInstance(Network::PacketBuffer& b, const MobEffectInstance& e) {
            b.WriteVarInt(static_cast<uint32_t>(e.effect));
            b.WriteVarInt(static_cast<uint32_t>(e.amplifier));
            b.WriteVarInt(static_cast<uint32_t>(e.duration));   // -1 = infinite
            b.WriteByte(e.ambient ? 1 : 0);
            b.WriteByte(e.visible ? 1 : 0);
            b.WriteByte(e.showIcon ? 1 : 0);
        }
        MobEffectInstance DeEffectInstance(Network::PacketReader& r) {
            const uint32_t raw = r.ReadVarInt();
            if (!IsValidEffectId(static_cast<int>(raw))) {
                throw std::runtime_error("potion effect id out of range: " + std::to_string(raw));
            }
            const int amplifier = static_cast<int>(r.ReadVarInt());
            const int duration  = static_cast<int>(r.ReadVarInt());
            const bool ambient  = r.ReadByte() != 0;
            const bool visible  = r.ReadByte() != 0;
            const bool icon     = r.ReadByte() != 0;
            return MobEffectInstance(static_cast<MobEffectId>(raw), duration, amplifier,
                                     ambient, visible, icon);
        }

        // Field order mirrors PotionContents.STREAM_CODEC
        // (PotionContents.java): optional potion holder, optional INT colour,
        // the custom-effect list, optional UTF-8 custom name. Optionals are a
        // presence byte then the value (ByteBufCodecs.optional).
        void SerPotionContents(Network::PacketBuffer& b, const PotionContents& v) {
            b.WriteByte(v.potion ? 1 : 0);
            if (v.potion) b.WriteVarInt(static_cast<uint32_t>(*v.potion));
            b.WriteByte(v.customColor ? 1 : 0);
            if (v.customColor) b.WriteInt(static_cast<uint32_t>(*v.customColor));
            b.WriteVarInt(static_cast<uint32_t>(v.customEffects.size()));
            for (const auto& e : v.customEffects) SerEffectInstance(b, e);
            b.WriteByte(v.customName ? 1 : 0);
            if (v.customName) b.WriteString(*v.customName);
        }
        PotionContents DePotionContents(Network::PacketReader& r) {
            PotionContents v;
            if (r.ReadByte() != 0) {
                const uint32_t raw = r.ReadVarInt();
                if (!IsValidPotionId(static_cast<int>(raw))) {
                    throw std::runtime_error("potion id out of range: " + std::to_string(raw));
                }
                v.potion = static_cast<PotionId>(raw);
            }
            if (r.ReadByte() != 0) v.customColor = static_cast<int32_t>(r.ReadInt());
            const uint32_t count = r.ReadVarInt();
            v.customEffects.reserve(count);
            for (uint32_t i = 0; i < count; ++i) v.customEffects.push_back(DeEffectInstance(r));
            if (r.ReadByte() != 0) v.customName = r.ReadString();
            return v;
        }

        void SerFloat(Network::PacketBuffer& b, const float& v) { b.WriteFloat(v); }
        float DeFloat(Network::PacketReader& r)                 { return r.ReadFloat(); }

        // Mirrors SuspiciousStewEffects.STREAM_CODEC — a list of Entry
        // (effect holder, VarInt duration).
        void SerStewEffects(Network::PacketBuffer& b, const SuspiciousStewEffects& v) {
            b.WriteVarInt(static_cast<uint32_t>(v.effects.size()));
            for (const auto& e : v.effects) {
                b.WriteVarInt(static_cast<uint32_t>(e.effect));
                b.WriteVarInt(static_cast<uint32_t>(e.duration));
            }
        }
        SuspiciousStewEffects DeStewEffects(Network::PacketReader& r) {
            SuspiciousStewEffects v;
            const uint32_t count = r.ReadVarInt();
            v.effects.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t raw = r.ReadVarInt();
                if (!IsValidEffectId(static_cast<int>(raw))) {
                    throw std::runtime_error("stew effect id out of range: " + std::to_string(raw));
                }
                SuspiciousStewEffects::Entry e;
                e.effect   = static_cast<MobEffectId>(raw);
                e.duration = static_cast<int>(r.ReadVarInt());
                v.effects.push_back(e);
            }
            return v;
        }

        // Mirrors WrittenBookContent.STREAM_CODEC: Filterable<String(32)>
        // title, UTF-8 author, VarInt generation, list of
        // Filterable<Component> pages, bool resolved. A Filterable is the raw
        // value then an optional (presence byte) filtered value.
        void SerWrittenBook(Network::PacketBuffer& b, const WrittenBookContent& v) {
            b.WriteString(v.title.raw);
            b.WriteByte(v.title.filtered ? 1 : 0);
            if (v.title.filtered) b.WriteString(*v.title.filtered);
            b.WriteString(v.author);
            b.WriteVarInt(static_cast<uint32_t>(v.generation));
            b.WriteVarInt(static_cast<uint32_t>(v.pages.size()));
            for (const auto& page : v.pages) {
                Text::Write(b, page.raw);
                b.WriteByte(page.filtered ? 1 : 0);
                if (page.filtered) Text::Write(b, *page.filtered);
            }
            b.WriteByte(v.resolved ? 1 : 0);
        }
        WrittenBookContent DeWrittenBook(Network::PacketReader& r) {
            // stringUtf8(32) bounds CHARACTERS; four bytes each is the
            // widest a UTF-8 character can be.
            constexpr size_t kTitleBytes = WrittenBookContent::TITLE_MAX_LENGTH * 4;
            WrittenBookContent v;
            v.title.raw = r.ReadString(kTitleBytes);
            if (r.ReadByte() != 0) v.title.filtered = r.ReadString(kTitleBytes);
            v.author = r.ReadString(32767);
            const uint32_t generation = r.ReadVarInt();
            if (generation > static_cast<uint32_t>(WrittenBookContent::MAX_GENERATION)) {
                // The record constructor's IllegalArgumentException.
                throw std::runtime_error("written book generation " + std::to_string(generation) +
                                         " is not between 0 and 3");
            }
            v.generation = static_cast<int>(generation);
            const uint32_t count = r.ReadVarInt();
            if (count > r.Remaining()) throw std::runtime_error("written book page count out of range");
            v.pages.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                Filterable<Text::Component> page;
                page.raw = Text::Read(r);
                if (r.ReadByte() != 0) page.filtered = Text::Read(r);
                v.pages.push_back(std::move(page));
            }
            v.resolved = r.ReadByte() != 0;
            return v;
        }

        // Mirrors WritableBookContent.STREAM_CODEC: a list (at most 100) of
        // Filterable<String(1024)>.
        void SerWritableBook(Network::PacketBuffer& b, const WritableBookContent& v) {
            const size_t count = std::min<size_t>(v.pages.size(), WritableBookContent::MAX_PAGES);
            b.WriteVarInt(static_cast<uint32_t>(count));
            for (size_t i = 0; i < count; ++i) {
                const auto& page = v.pages[i];
                b.WriteString(page.raw);
                b.WriteByte(page.filtered ? 1 : 0);
                if (page.filtered) b.WriteString(*page.filtered);
            }
        }
        WritableBookContent DeWritableBook(Network::PacketReader& r) {
            constexpr size_t kPageBytes = WritableBookContent::PAGE_EDIT_LENGTH * 4;
            WritableBookContent v;
            const uint32_t count = r.ReadVarInt();
            if (count > static_cast<uint32_t>(WritableBookContent::MAX_PAGES)) {
                throw std::runtime_error("writable book has " + std::to_string(count) +
                                         " pages, but maximum is 100");
            }
            v.pages.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                Filterable<std::string> page;
                page.raw = r.ReadString(kPageBytes);
                if (r.ReadByte() != 0) page.filtered = r.ReadString(kPageBytes);
                v.pages.push_back(std::move(page));
            }
            return v;
        }

        // MC DyedItemColor.STREAM_CODEC — ByteBufCodecs.INT.
        void SerI32(Network::PacketBuffer& b, const int32_t& v) { b.WriteInt(static_cast<uint32_t>(v)); }
        int32_t DeI32(Network::PacketReader& r)                 { return static_cast<int32_t>(r.ReadInt()); }

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
    const DataComponentType<SulfurCubeBucketData> SULFUR_CUBE_BUCKET    {"sulfur_cube_bucket",        14, &SerSulfurCubeBucket, &DeSulfurCubeBucket};
    const DataComponentType<std::string>      ITEM_NAME                 {"item_name",                  5, &SerString,       &DeString};
    const DataComponentType<ItemLore>         LORE                      {"lore",                       6, &SerLore,         &DeLore};
    const DataComponentType<Rarity>           RARITY                    {"rarity",                     7, &SerRarity,       &DeRarity};
    const DataComponentType<Equippable>       EQUIPPABLE                {"equippable",                11, &SerEquippable,   &DeEquippable};
    const DataComponentType<BlocksAttacks>    BLOCKS_ATTACKS            {"blocks_attacks",            12, &SerBlocksAttacks,&DeBlocksAttacks};
    const DataComponentType<BundleContents>   BUNDLE_CONTENTS           {"bundle_contents",           13, &SerBundleContents,&DeBundleContents};
    const DataComponentType<PotionContents>   POTION_CONTENTS           {"potion_contents",           15, &SerPotionContents,&DePotionContents};
    const DataComponentType<float>            POTION_DURATION_SCALE     {"potion_duration_scale",     16, &SerFloat,        &DeFloat};
    const DataComponentType<SuspiciousStewEffects> SUSPICIOUS_STEW_EFFECTS {"suspicious_stew_effects", 17, &SerStewEffects, &DeStewEffects};
    const DataComponentType<WrittenBookContent> WRITTEN_BOOK_CONTENT  {"written_book_content",      18, &SerWrittenBook, &DeWrittenBook};
    const DataComponentType<WritableBookContent> WRITABLE_BOOK_CONTENT {"writable_book_content",     19, &SerWritableBook, &DeWritableBook};
    const DataComponentType<int32_t>          DYED_COLOR                {"dyed_color",                20, &SerI32,          &DeI32};
    const DataComponentType<int32_t>          DAMAGE                    {"damage",                    21, &SerVarInt,       &DeVarInt};
    const DataComponentType<int32_t>          MAX_DAMAGE                {"max_damage",                22, &SerVarInt,       &DeVarInt};
    const DataComponentType<bool>             UNBREAKABLE               {"unbreakable",               23, &SerUnit,         &DeUnit};
    const DataComponentType<int32_t>          REPAIR_COST               {"repair_cost",               24, &SerVarInt,       &DeVarInt};
    const DataComponentType<ItemEnchantments> ENCHANTMENTS              {"enchantments",              25, &SerEnchantments, &DeEnchantments};
    const DataComponentType<Repairable>       REPAIRABLE                {"repairable",                26, &SerRepairable,   &DeRepairable};
    const DataComponentType<int32_t>          ENCHANTABLE               {"enchantable",               27, &SerVarInt,       &DeVarInt};
    const DataComponentType<Weapon>           WEAPON                    {"weapon",                    28, &SerWeapon,       &DeWeapon};
    const DataComponentType<std::string>      BREAK_SOUND               {"break_sound",               29, &SerString,       &DeString};
    const DataComponentType<std::string>      DAMAGE_RESISTANT          {"damage_resistant",          30, &SerString,       &DeString};
    const DataComponentType<std::string>      PAINTING_VARIANT          {"painting/variant",          40, &SerString,       &DeString};

#if ENABLE_PORTAL_GUN
    const DataComponentType<uint8_t>  PORTAL_GUN_NEXT_COLOR  {"portal_gun_next_color",  100, &SerU8,  &DeU8};
    const DataComponentType<uint64_t> PORTAL_GUN_INSTANCE_ID {"portal_gun_instance_id", 101, &SerU64, &DeU64};
#endif

} // namespace Game::DataComponents
