// File: src/server/world/storage/anvil/ItemStackNbt.cpp
#include "common/core/Features.hpp"
#include <optional>
#include "server/world/storage/anvil/ItemStackNbt.hpp"

#include "common/core/Log.hpp"
#include "common/data/DataComponents.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/enchantment/Enchantment.hpp"
#include "common/world/enchantment/ItemEnchantments.hpp"

#include <string_view>
#include <unordered_map>

namespace Game::Anvil {

    namespace {

        constexpr std::string_view kNamespace = "minecraft:";

        // This engine's own items live under their own namespace so a
        // vanilla reader sees them as unknown rather than as a wrong item.
        constexpr std::string_view kOwnNamespace = "obeycraft:";

        std::string_view StripNamespace(std::string_view name) {
            if (name.rfind(kNamespace, 0) == 0) return name.substr(kNamespace.size());
            if (name.rfind(kOwnNamespace, 0) == 0) return name.substr(kOwnNamespace.size());
            return name;
        }

        // slug -> ItemID, built once. The engine keeps item slugs in two places
        // — block items borrow the block's registrySlug, pure items carry their
        // own in kPureItemTable — and neither is indexed by name today.
        const std::unordered_map<std::string, ItemID>& NameIndex() {
            static const std::unordered_map<std::string, ItemID> index = [] {
                std::unordered_map<std::string, ItemID> map;
                for (size_t i = 0; i < static_cast<size_t>(BlockID::Count); ++i) {
                    const auto id = static_cast<BlockID>(i);
                    const std::string_view slug = BlockRegistry::Get(id).registrySlug;
                    if (!slug.empty()) map.emplace(std::string(slug), ItemRegistry::FromBlock(id));
                }
                for (size_t i = 0; i < kPureItemTableSize; ++i) {
                    if (const char* slug = kPureItemTable[i].slug) {
                        map[slug] = static_cast<ItemID>(PURE_ITEM_BASE + i);
                    }
                }
#if ENABLE_PORTAL_GUN
                // Custom items past the pure-item table have no table slug;
                // without an entry here the gun was dropped from every
                // inventory on load.
                if (Items::PortalGun != Items::Air) map["portal_gun"] = Items::PortalGun;
#endif
#if ENABLE_IMMERSIVE_PORTALS
                if (Items::PortalWand != Items::Air) map["portal_wand"] = Items::PortalWand;
#endif
                if (Items::AoWand != Items::Air) map["ao_wand"] = Items::AoWand;
                return map;
            }();
            return index;
        }

    } // namespace

    std::string ItemName(ItemID id) {
#if ENABLE_PORTAL_GUN
        if (id == Items::PortalGun && id != Items::Air) return std::string(kOwnNamespace) + "portal_gun";
#endif
#if ENABLE_IMMERSIVE_PORTALS
        if (id == Items::PortalWand && id != Items::Air) return std::string(kOwnNamespace) + "portal_wand";
#endif
        if (id == Items::AoWand && id != Items::Air) return std::string(kOwnNamespace) + "ao_wand";
        if (id >= PURE_ITEM_BASE) {
            const size_t index = static_cast<size_t>(id - PURE_ITEM_BASE);
            if (index < kPureItemTableSize) {
                if (const char* slug = kPureItemTable[index].slug) {
                    return std::string(kNamespace) + slug;
                }
            }
            return {};
        }
        const std::string_view slug = BlockRegistry::Get(ItemRegistry::ToBlock(id)).registrySlug;
        if (slug.empty()) return {};
        return std::string(kNamespace) + std::string(slug);
    }

    ItemID ItemFromName(std::string_view name) {
        const auto& index = NameIndex();
        auto it = index.find(std::string(StripNamespace(name)));
        return it != index.end() ? it->second : Items::Air;
    }

    void WriteItemStackBody(Nbt::Writer& w, const ItemStack& stack, int slot) {
        if (slot >= 0) w.Byte("Slot", static_cast<int8_t>(slot));

        w.String("id", ItemName(stack.itemId));
        // TAG_Int, and vanilla constrains it to 1..99
        // (ExtraCodecs.intRange(1, 99) in ItemStack's codec). A creative or
        // duped stack past that would otherwise silently become 1 on load.
        w.Int("count", std::clamp(stack.count, 1, 99));

        const auto customName = stack.components.get(DataComponents::CUSTOM_NAME);
        const auto stored     = stack.components.get(DataComponents::STORED_ENCHANTMENTS);
        const bool hasEnchants = stored.has_value() && !stored->entries.empty();
#if ENABLE_PORTAL_GUN
        // The gun's pair is keyed by this id (PortalRegistry); losing it on
        // save orphaned the saved pair from the reloaded gun.
        const auto gunInstance = stack.components.get(DataComponents::PORTAL_GUN_INSTANCE_ID);
#else
        const std::optional<uint64_t> gunInstance;
#endif

        const auto sulfurBucket = stack.components.get(DataComponents::SULFUR_CUBE_BUCKET);

        if (!customName.has_value() && !hasEnchants && !gunInstance.has_value() &&
            !sulfurBucket.has_value()) return;

        w.BeginCompound("components");
        if (sulfurBucket.has_value()) {
            // MC: minecraft:sulfur_cube_content (item template) beside
            // minecraft:bucket_entity_data {age, age_locked, NoAI}. One
            // compound here, under this engine's namespace.
            w.BeginCompound(std::string(kOwnNamespace) + "sulfur_cube_bucket");
            if (!sulfurBucket->bodyItem.empty()) w.String("content", sulfurBucket->bodyItem);
            w.Int("age", sulfurBucket->age);
            w.Bool("age_locked", sulfurBucket->ageLocked);
            w.Bool("NoAI", sulfurBucket->noAi);
            w.EndCompound();
        }
        if (gunInstance.has_value()) {
            w.Long(std::string(kOwnNamespace) + "portal_gun_instance_id",
                   static_cast<int64_t>(*gunInstance));
        }
        if (customName.has_value()) {
            // The custom_name component is a text Component. Its NBT codec
            // accepts a bare string and reads it as literal text, which is
            // exactly what we store.
            w.String("minecraft:custom_name", *customName);
        }
        if (hasEnchants) {
            // At DataVersion 4764 ItemEnchantments.CODEC is
            // Codec.unboundedMap(Enchantment.CODEC, LEVEL_CODEC) — a FLAT
            // compound of name -> level. The `{levels: {...}}` wrapper older
            // notes describe was 1.20.5..1.21.4 only.
            w.BeginCompound("minecraft:stored_enchantments");
            for (const EnchantmentInstance& e : stored->entries) {
                const Enchantment& def = EnchantmentRegistry::Get(e.id);
                if (def.slug.empty()) continue;
                w.Int(std::string(kNamespace) + def.slug, std::clamp(e.level, 1, 255));
            }
            w.EndCompound();
        }
        w.EndCompound();
    }

    ItemStack ReadItemStack(const ::World::NBTTagCompound& tag) {
        ItemStack stack;

        const std::string id = tag.GetValue<std::string>("id");
        if (id.empty()) return stack;
        stack.itemId = ItemFromName(id);
        if (stack.itemId == Items::Air) return stack;   // unknown item -> nothing, as vanilla does

        stack.count = tag.GetValue<int32_t>("count", 1);
        if (stack.count <= 0) return ItemStack{};
        stack.count = std::min(stack.count, ItemRegistry::Get(stack.itemId).maxStackSize);

        auto components = std::dynamic_pointer_cast<::World::NBTTagCompound>(tag.GetTag("components"));
        if (!components) return stack;

#if ENABLE_PORTAL_GUN
        if (auto gun = std::dynamic_pointer_cast<::World::NBTTagLong>(
                components->GetTag(std::string(kOwnNamespace) + "portal_gun_instance_id"))) {
            stack.components.set(DataComponents::PORTAL_GUN_INSTANCE_ID, static_cast<uint64_t>(gun->value));
        }
#endif

        if (auto name = std::dynamic_pointer_cast<::World::NBTTagString>(
                components->GetTag("minecraft:custom_name"))) {
            stack.components.set(DataComponents::CUSTOM_NAME, name->value);
        }

        if (auto sb = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                components->GetTag(std::string(kOwnNamespace) + "sulfur_cube_bucket"))) {
            SulfurCubeBucketData data;
            data.bodyItem  = sb->GetValue<std::string>("content", "");
            data.age       = sb->GetValue<int32_t>("age", 0);
            data.ageLocked = sb->GetValue<int8_t>("age_locked", 0) != 0;
            data.noAi      = sb->GetValue<int8_t>("NoAI", 0) != 0;
            stack.components.set(DataComponents::SULFUR_CUBE_BUCKET, data);
        }

        if (auto ench = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                components->GetTag("minecraft:stored_enchantments"))) {
            ItemEnchantments out;
            for (const auto& [key, value] : ench->value) {
                auto levelTag = std::dynamic_pointer_cast<::World::NBTTagInt>(value);
                if (!levelTag) continue;
                const auto found = EnchantmentRegistry::ByName(StripNamespace(key));
                if (!found) continue;              // an enchantment this build lacks
                out.entries.push_back({*found, levelTag->value});
            }
            if (!out.entries.empty()) {
                stack.components.set(DataComponents::STORED_ENCHANTMENTS, out);
            }
        }
        return stack;
    }

} // namespace Game::Anvil
