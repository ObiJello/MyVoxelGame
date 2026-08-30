// File: src/server/world/storage/anvil/ItemStackNbt.cpp
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

        std::string_view StripNamespace(std::string_view name) {
            if (name.rfind(kNamespace, 0) == 0) return name.substr(kNamespace.size());
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
                return map;
            }();
            return index;
        }

    } // namespace

    std::string ItemName(ItemID id) {
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

        if (!customName.has_value() && !hasEnchants) return;

        w.BeginCompound("components");
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

        if (auto name = std::dynamic_pointer_cast<::World::NBTTagString>(
                components->GetTag("minecraft:custom_name"))) {
            stack.components.set(DataComponents::CUSTOM_NAME, name->value);
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
