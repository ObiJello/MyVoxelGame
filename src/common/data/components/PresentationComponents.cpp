// File: src/common/data/components/PresentationComponents.cpp
#include "PresentationComponents.hpp"

#include "ComponentTooltips.hpp"
#include "common/entity/BookItems.hpp"
#include "common/entity/ClientItemLoader.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace Game {

    // ── CustomModelData ────────────────────────────────────────────────────

    std::optional<float> CustomModelData::GetFloat(int index) const {
        if (index < 0 || static_cast<size_t>(index) >= floats.size()) return std::nullopt;
        return floats[static_cast<size_t>(index)];
    }
    std::optional<bool> CustomModelData::GetFlag(int index) const {
        if (index < 0 || static_cast<size_t>(index) >= flags.size()) return std::nullopt;
        return static_cast<bool>(flags[static_cast<size_t>(index)]);
    }
    std::optional<std::string> CustomModelData::GetString(int index) const {
        if (index < 0 || static_cast<size_t>(index) >= strings.size()) return std::nullopt;
        return strings[static_cast<size_t>(index)];
    }
    std::optional<int32_t> CustomModelData::GetColor(int index) const {
        if (index < 0 || static_cast<size_t>(index) >= colors.size()) return std::nullopt;
        return colors[static_cast<size_t>(index)];
    }

    // ── Names ──────────────────────────────────────────────────────────────

    namespace {
        // The styled form, when it was set for the plain text the stack
        // holds now.
        std::optional<Text::Component> CurrentStyle(const ItemStack& stack,
                                                    const DataComponentType<StyledName>& styleType,
                                                    const std::optional<std::string>& plain) {
            if (!plain) return std::nullopt;
            auto style = stack.get(styleType);
            if (!style || style->plain != *plain) return std::nullopt;
            return style->component;
        }

        void SetStyled(DataComponentMap& components, const DataComponentType<std::string>& plainType,
                       const DataComponentType<StyledName>& styleType, const Text::Component& name) {
            std::string collapsed;
            if (name.TryCollapseToString(collapsed)) {
                components.set(plainType, collapsed);
                components.remove(styleType);
                return;
            }
            const std::string plain = Text::GetString(name);
            components.set(plainType, plain);
            components.set(styleType, StyledName{name, plain});
        }

        // ChatFormatting's colour index of each Rarity (Rarity.java).
        int RarityFormatting(Rarity rarity) {
            switch (rarity) {
                case Rarity::UNCOMMON: return 14;   // YELLOW
                case Rarity::RARE:     return 11;   // AQUA
                case Rarity::EPIC:     return 13;   // LIGHT_PURPLE
                case Rarity::COMMON:
                default:               return 15;   // WHITE
            }
        }
    }

    std::optional<Text::Component> GetCustomNameComponent(const ItemStack& stack) {
        const auto plain = stack.get(DataComponents::CUSTOM_NAME);
        if (!plain) return std::nullopt;
        if (auto styled = CurrentStyle(stack, DataComponents::CUSTOM_NAME_STYLE, plain)) return styled;
        return Text::Component::Literal(*plain);
    }

    Text::Component GetItemNameComponent(const ItemStack& stack) {
        // Item.getName reads ITEM_NAME — except PotionItem / TippedArrowItem,
        // whose name comes from their contents (GetItemStackItemName).
        const std::string plainName = GetItemStackItemName(stack);
        const auto itemName = stack.get(DataComponents::ITEM_NAME);
        if (itemName && *itemName == plainName) {
            if (auto styled = CurrentStyle(stack, DataComponents::ITEM_NAME_STYLE, itemName)) return *styled;
        }
        return Text::Component::Literal(plainName);
    }

    Text::Component GetHoverNameComponent(const ItemStack& stack) {
        // ItemStack.getHoverName → getCustomName: CUSTOM_NAME, else a
        // written book's non-blank title, else the item name.
        if (auto custom = GetCustomNameComponent(stack)) return *custom;
        if (std::string title = Books::WrittenBookTitle(stack); !title.empty()) {
            return Text::Component::Literal(title);
        }
        return GetItemNameComponent(stack);
    }

    Text::Component GetStyledHoverName(const ItemStack& stack) {
        // Component.empty().append(getHoverName()).withStyle(getRarity().color()),
        // then ITALIC when the stack has CUSTOM_NAME.
        Text::Component out = Text::Component::Empty();
        out.style.color = Text::TextColor::FromFormatting(
            RarityFormatting(static_cast<Rarity>(GetStackRarity(stack))));
        if (stack.get(DataComponents::CUSTOM_NAME)) out.style.italic = true;
        out.Append(GetHoverNameComponent(stack));
        return out;
    }

    void SetCustomName(DataComponentMap& components, const Text::Component& name) {
        SetStyled(components, DataComponents::CUSTOM_NAME, DataComponents::CUSTOM_NAME_STYLE, name);
    }

    void SetCustomNameText(DataComponentMap& components, const std::string& text) {
        components.set(DataComponents::CUSTOM_NAME, text);
        components.remove(DataComponents::CUSTOM_NAME_STYLE);
    }

    void ClearCustomName(DataComponentMap& components) {
        components.remove(DataComponents::CUSTOM_NAME);
        components.remove(DataComponents::CUSTOM_NAME_STYLE);
    }

    void SetItemName(DataComponentMap& components, const Text::Component& name) {
        SetStyled(components, DataComponents::ITEM_NAME, DataComponents::ITEM_NAME_STYLE, name);
    }

    void ClearItemName(DataComponentMap& components) {
        components.remove(DataComponents::ITEM_NAME);
        components.remove(DataComponents::ITEM_NAME_STYLE);
    }

    Text::Style LoreStyle() {
        Text::Style style;
        style.color  = Text::TextColor::FromFormatting(5);   // DARK_PURPLE
        style.italic = true;
        return style;
    }

    // ── Item model ─────────────────────────────────────────────────────────

    namespace {
        // A render copy of `stack` drawn as `renderId`, its glint pinned to
        // the original's (the render item's defaults — an enchanted book's
        // glint override — must not decide it).
        const ItemStack& DrawnAs(const ItemStack& stack, ItemID renderId, ItemStack& scratch) {
            if (renderId == Items::Air || renderId == stack.itemId) return stack;
            const bool foil = stack.HasFoil();
            scratch = stack;
            scratch.itemId = renderId;
            if (scratch.HasFoil() != foil) {
                scratch.components.set(DataComponents::ENCHANTMENT_GLINT_OVERRIDE, foil);
            }
            return scratch;
        }

        // The registered item whose own definition `modelId` is
        // ("minecraft:<slug>"), Air for none.
        ItemID ItemOwningModel(const std::string& modelId) {
            if (modelId.rfind("minecraft:", 0) != 0) return Items::Air;
            const std::string_view path = std::string_view(modelId).substr(10);
            if (path.empty() || path.find('/') != std::string_view::npos) return Items::Air;
            static const std::unordered_map<std::string, ItemID> index = [] {
                std::unordered_map<std::string, ItemID> map;
                for (size_t i = 1; i < static_cast<size_t>(BlockID::Count); ++i) {
                    const ItemID id = static_cast<ItemID>(i);
                    const std::string_view slug = ItemRegistry::Slug(id);
                    if (!slug.empty()) map.emplace(std::string(slug), id);
                }
                ItemRegistry::ForEachPureItem([&](ItemID id, const Item&) {
                    const std::string_view slug = ItemRegistry::Slug(id);
                    if (!slug.empty()) map[std::string(slug)] = id;
                });
                return map;
            }();
            auto it = index.find(std::string(path));
            return it == index.end() ? Items::Air : it->second;
        }

        std::string ModelIdOf(std::string_view id) {
            return id.find(':') == std::string_view::npos ? "minecraft:" + std::string(id) : std::string(id);
        }
    }

    const ItemStack& GetRenderStack(const ItemStack& stack, ItemStack& scratch) {
        if (stack.IsEmpty() || ItemRegistry::IsRenderVariant(stack.itemId)) return stack;
        const Item& own = ItemRegistry::Get(stack.itemId);
        const std::optional<std::string> itemModel = stack.get(DataComponents::ITEM_MODEL);
        // The common case: the item's own definition, which ignores
        // CUSTOM_MODEL_DATA — the registered item already describes it.
        if (!itemModel && !own.modelReadsCustomModelData) return stack;

        const std::string ownSlug(ItemRegistry::Slug(stack.itemId));
        const std::string modelId = itemModel ? ModelIdOf(*itemModel) : "minecraft:" + ownSlug;
        const bool readsCmd = itemModel ? ClientItemLoader::ReadsCustomModelData(modelId)
                                        : own.modelReadsCustomModelData;
        const ItemID owner = ItemOwningModel(modelId);

        if (!readsCmd) {
            // Another registered item's own definition: draw as that item —
            // its sprite, block model, special renderer and animations.
            if (owner != Items::Air) return DrawnAs(stack, owner, scratch);
            if (itemModel && ModelIdOf(*itemModel) == "minecraft:" + ownSlug) return stack;
        }
        if (!ClientItemLoader::DefinitionExists(modelId)) {
            // MC draws the missing model; the stack keeps its own look here.
            return stack;
        }

        std::string leafKey;
        const ClientItemDesc desc = ClientItemLoader::Evaluate(modelId, stack, leafKey);
        if (desc.kind == ClientItemKind::Missing || leafKey.empty()) return stack;

        // The item the variant copies its non-render fields from: the
        // definition's own item when it has one, else the stack's.
        const ItemID base = owner != Items::Air ? owner : stack.itemId;
        const std::string slug = !desc.restSlug.empty() ? desc.restSlug : ownSlug;
        const ItemID variant = ItemRegistry::RegisterRenderVariant(leafKey, slug, [&]() {
            Item item = ItemRegistry::Get(base);
            // Every render field starts clean; the chosen model fills them.
            item.renderType = ItemRenderType::Sprite;
            item.spriteName.clear();
            item.spriteLayers.clear();
            item.layerTints.clear();
            item.layerTintKinds.clear();
            item.layerTintIndices.clear();
            item.spriteFrames.clear();
            item.selectFrame = nullptr;
            item.predicateName.clear();
            item.blockModelOverride.clear();
            item.compositeChildren.clear();
            item.specialKind.clear();
            item.specialTexture.clear();
            item.stackSprite = nullptr;
            ClientItemDesc applied = desc;
            // A composite is baked only for registered items; a variant
            // shows its first child's look.
            if (applied.kind == ClientItemKind::Composite) {
                applied.kind = applied.compositeChildren.empty() ? ClientItemKind::Missing
                                                                 : ClientItemKind::BlockModel;
                if (!applied.compositeChildren.empty()) applied.restSlug = applied.compositeChildren.front().modelSlug;
            }
            ApplyClientItemDesc(item, applied);
            if (item.renderType == ItemRenderType::Sprite && item.spriteName.empty() && item.spriteLayers.empty()) {
                item.spriteName = applied.restSlug;
            }
            item.selectFrame = ItemFrameSelectorFor(applied.property, slug, !item.spriteFrames.empty());
            return item;
        });
        return DrawnAs(stack, variant, scratch);
    }

} // namespace Game

namespace Game::DataComponents {

    namespace {

        void SerStyledName(Network::PacketBuffer& b, const StyledName& v) {
            b.WriteString(v.plain);
            Text::Write(b, v.component);
        }
        StyledName DeStyledName(Network::PacketReader& r) {
            StyledName v;
            v.plain = r.ReadString();
            v.component = Text::Read(r);
            return v;
        }

        void SerIdentifier(Network::PacketBuffer& b, const std::string& v) { b.WriteString(v); }
        std::string DeIdentifier(Network::PacketReader& r) { return r.ReadString(); }

        // CustomModelData.STREAM_CODEC: FLOAT, BOOL, STRING_UTF8 and INT
        // lists, in that order.
        void SerCustomModelData(Network::PacketBuffer& b, const CustomModelData& v) {
            b.WriteVarInt(static_cast<uint32_t>(v.floats.size()));
            for (float f : v.floats) b.WriteFloat(f);
            b.WriteVarInt(static_cast<uint32_t>(v.flags.size()));
            for (bool f : v.flags) b.WriteByte(f ? 1 : 0);
            b.WriteVarInt(static_cast<uint32_t>(v.strings.size()));
            for (const std::string& s : v.strings) b.WriteString(s);
            b.WriteVarInt(static_cast<uint32_t>(v.colors.size()));
            for (int32_t c : v.colors) b.WriteInt(static_cast<uint32_t>(c));
        }
        CustomModelData DeCustomModelData(Network::PacketReader& r) {
            constexpr uint32_t kMax = 65536;
            CustomModelData v;
            uint32_t n = r.ReadVarInt();
            if (n > kMax) throw std::runtime_error("custom_model_data: too many floats");
            v.floats.reserve(n);
            for (uint32_t i = 0; i < n; ++i) v.floats.push_back(r.ReadFloat());
            n = r.ReadVarInt();
            if (n > kMax) throw std::runtime_error("custom_model_data: too many flags");
            v.flags.reserve(n);
            for (uint32_t i = 0; i < n; ++i) v.flags.push_back(r.ReadByte() != 0);
            n = r.ReadVarInt();
            if (n > kMax) throw std::runtime_error("custom_model_data: too many strings");
            v.strings.reserve(n);
            for (uint32_t i = 0; i < n; ++i) v.strings.push_back(r.ReadString());
            n = r.ReadVarInt();
            if (n > kMax) throw std::runtime_error("custom_model_data: too many colors");
            v.colors.reserve(n);
            for (uint32_t i = 0; i < n; ++i) v.colors.push_back(static_cast<int32_t>(r.ReadInt()));
            return v;
        }

    } // namespace

    const DataComponentType<StyledName> CUSTOM_NAME_STYLE{"obeycraft:custom_name_style", 430,
                                                          &SerStyledName, &DeStyledName};
    const DataComponentType<StyledName> ITEM_NAME_STYLE{"obeycraft:item_name_style", 431,
                                                        &SerStyledName, &DeStyledName};
    const DataComponentType<std::string> ITEM_MODEL{"item_model", 432, &SerIdentifier, &DeIdentifier};
    const DataComponentType<CustomModelData> CUSTOM_MODEL_DATA{"custom_model_data", 433,
                                                               &SerCustomModelData, &DeCustomModelData};

} // namespace Game::DataComponents

namespace Game {

    namespace {
        // ItemLore.addToTooltip: every line, over LORE_STYLE.
        void LoreTooltip(const ItemStack& stack, const ComponentTooltips::Context&,
                         std::vector<ComponentTooltips::Line>& out) {
            const auto lore = stack.get(DataComponents::LORE);
            if (!lore) return;
            const Text::Style base = LoreStyle();
            for (const Text::Component& line : lore->lines) {
                Text::Component styled = Text::Component::Empty();
                styled.style = base;
                styled.Append(line);
                out.push_back({Text::GetString(line), ComponentTooltips::kDarkPurple, std::move(styled)});
            }
        }

        const ComponentTooltips::Registrar kLoreTooltip{ComponentTooltips::Slot::Lore, &DataComponents::LORE,
                                                        &LoreTooltip};
    }

} // namespace Game
