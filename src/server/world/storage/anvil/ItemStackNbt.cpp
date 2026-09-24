// File: src/server/world/storage/anvil/ItemStackNbt.cpp
#include "common/core/Features.hpp"
#include <algorithm>
#include <optional>
#include "server/world/storage/anvil/ItemStackNbt.hpp"

#include "common/core/Log.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/enchantment/Enchantment.hpp"
#include "common/world/enchantment/ItemEnchantments.hpp"

#include <nlohmann/json.hpp>

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

        // MobEffectInstance.CODEC — id plus the Details fields, every one
        // written explicitly (the same choice EntityNbt's active_effects
        // makes). A potion's custom effect never has a hidden chain.
        void WriteEffect(Nbt::Writer& w, const MobEffectInstance& e) {
            w.String("id", std::string(kNamespace) + GetEffectName(e.effect));
            w.Byte  ("amplifier", static_cast<int8_t>(e.amplifier));
            w.Int   ("duration",  e.duration);
            w.Bool  ("ambient",   e.ambient);
            w.Bool  ("show_particles", e.visible);
            w.Bool  ("show_icon", e.showIcon);
        }

        bool ReadEffect(const ::World::NBTTagCompound& tag, MobEffectInstance& out) {
            MobEffectId id;
            if (!ParseEffectId(tag.GetValue<std::string>("id", ""), id)) return false;
            const bool visible = tag.GetValue<int8_t>("show_particles", 1) != 0;
            // Details::create: show_icon.orElse(showParticles).
            const bool icon = tag.HasTag("show_icon")
                ? tag.GetValue<int8_t>("show_icon", 1) != 0 : visible;
            out = MobEffectInstance(id, tag.GetValue<int32_t>("duration", 0),
                                    tag.GetValue<int8_t>("amplifier", 0) & 0xFF,
                                    tag.GetValue<int8_t>("ambient", 0) != 0, visible, icon);
            return true;
        }

        // ── Text components (ComponentSerialization.CODEC over NbtOps) ───

        // Any numeric tag as MC's NbtOps.getNumberValue reads it.
        std::optional<double> NumberOf(const ::World::NBTTag& tag) {
            if (auto t = dynamic_cast<const ::World::NBTTagByte*>(&tag))   return t->value;
            if (auto t = dynamic_cast<const ::World::NBTTagShort*>(&tag))  return t->value;
            if (auto t = dynamic_cast<const ::World::NBTTagInt*>(&tag))    return t->value;
            if (auto t = dynamic_cast<const ::World::NBTTagLong*>(&tag))   return static_cast<double>(t->value);
            if (auto t = dynamic_cast<const ::World::NBTTagFloat*>(&tag))  return t->value;
            if (auto t = dynamic_cast<const ::World::NBTTagDouble*>(&tag)) return t->value;
            return std::nullopt;
        }

        // 26.x ListTag stores a heterogeneous list as compounds, wrapping each
        // non-compound element as {"": value} (ListTag.tryUnwrap on read).
        const ::World::NBTTag* Unwrap(const ::World::NBTTag* tag) {
            auto compound = dynamic_cast<const ::World::NBTTagCompound*>(tag);
            if (compound && compound->value.size() == 1) {
                auto it = compound->value.find("");
                if (it != compound->value.end() && it->second) return it->second.get();
            }
            return tag;
        }

        // The NBT tree as the JSON tree the component codec is written
        // against (Text::FromJson): numbers stay numbers — the codec reads a
        // byte 1b as a boolean exactly as NbtOps hands one to Codec.BOOL.
        nlohmann::json NbtToJson(const ::World::NBTTag& tag, int depth = 0) {
            if (depth > 512) return nullptr;
            if (auto t = dynamic_cast<const ::World::NBTTagString*>(&tag)) return t->value;
            if (auto t = dynamic_cast<const ::World::NBTTagByte*>(&tag))   return static_cast<int>(t->value);
            if (auto t = dynamic_cast<const ::World::NBTTagShort*>(&tag))  return static_cast<int>(t->value);
            if (auto t = dynamic_cast<const ::World::NBTTagInt*>(&tag))    return t->value;
            if (auto t = dynamic_cast<const ::World::NBTTagLong*>(&tag))   return t->value;
            if (auto t = dynamic_cast<const ::World::NBTTagFloat*>(&tag))  return t->value;
            if (auto t = dynamic_cast<const ::World::NBTTagDouble*>(&tag)) return t->value;
            if (auto t = dynamic_cast<const ::World::NBTTagList*>(&tag)) {
                nlohmann::json out = nlohmann::json::array();
                for (const auto& e : t->value) {
                    if (e) out.push_back(NbtToJson(*Unwrap(e.get()), depth + 1));
                }
                return out;
            }
            if (auto t = dynamic_cast<const ::World::NBTTagCompound*>(&tag)) {
                nlohmann::json out = nlohmann::json::object();
                for (const auto& [key, value] : t->value) {
                    if (value) out[key] = NbtToJson(*value, depth + 1);
                }
                return out;
            }
            if (auto t = dynamic_cast<const ::World::NBTTagByteArray*>(&tag)) {
                nlohmann::json out = nlohmann::json::array();
                for (int8_t v : t->value) out.push_back(static_cast<int>(v));
                return out;
            }
            if (auto t = dynamic_cast<const ::World::NBTTagIntArray*>(&tag)) {
                nlohmann::json out = nlohmann::json::array();
                for (int32_t v : t->value) out.push_back(v);
                return out;
            }
            if (auto t = dynamic_cast<const ::World::NBTTagLongArray*>(&tag)) {
                nlohmann::json out = nlohmann::json::array();
                for (int64_t v : t->value) out.push_back(v);
                return out;
            }
            return nullptr;
        }

        void WriteComponentBody(Nbt::Writer& w, const Text::Component& c);

        // A list of components. MC writes a string list when every element
        // collapses and a (wrapped) mixed list otherwise; writing the full
        // compound form for every element of a mixed list is the same list
        // to MC's reader.
        void WriteComponentList(Nbt::Writer& w, std::string_view name, const std::vector<Text::Component>& list) {
            bool allStrings = true;
            std::string scratch;
            for (const auto& e : list) {
                if (!e.TryCollapseToString(scratch)) { allStrings = false; break; }
            }
            if (allStrings) {
                auto l = w.BeginList(name, Nbt::TagType::String);
                for (const auto& e : list) w.ListString(l, e.text);
                w.EndList(l);
                return;
            }
            auto l = w.BeginList(name, Nbt::TagType::Compound);
            for (const auto& e : list) {
                w.ListCompoundBegin(l);
                WriteComponentBody(w, e);
                w.ListCompoundEnd(l);
            }
            w.EndList(l);
        }

        // The full (compound) form's fields, into an open compound — the
        // fuzzy contents encoder (no "type"), "extra", then the style.
        void WriteComponentBody(Nbt::Writer& w, const Text::Component& c) {
            using Kind = Text::Component::Kind;
            switch (c.kind) {
                case Kind::Text:
                    w.String("text", c.text);
                    break;
                case Kind::Translatable:
                    w.String("translate", c.text);
                    if (c.fallback) w.String("fallback", *c.fallback);
                    if (!c.args.empty()) WriteComponentList(w, "with", c.args);
                    break;
                case Kind::Keybind:
                    w.String("keybind", c.text);
                    break;
                case Kind::Score:
                    w.BeginCompound("score");
                    w.String("name", c.text);
                    w.String("objective", c.objective);
                    w.EndCompound();
                    break;
                case Kind::Selector:
                    w.String("selector", c.text);
                    if (!c.separator.empty()) WriteTextComponent(w, "separator", c.separator.front());
                    break;
                case Kind::Nbt:
                    w.String("nbt", c.text);
                    if (c.interpret) w.Bool("interpret", true);
                    if (c.plain) w.Bool("plain", true);
                    if (!c.separator.empty()) WriteTextComponent(w, "separator", c.separator.front());
                    if (!c.sourceKind.empty()) w.String(c.sourceKind, c.source);
                    break;
            }
            if (!c.extra.empty()) WriteComponentList(w, "extra", c.extra);

            const Text::Style& st = c.style;
            if (st.color) w.String("color", st.color->Serialize());
            if (st.shadowColor) w.Int("shadow_color", *st.shadowColor);
            if (st.bold) w.Bool("bold", *st.bold);
            if (st.italic) w.Bool("italic", *st.italic);
            if (st.underlined) w.Bool("underlined", *st.underlined);
            if (st.strikethrough) w.Bool("strikethrough", *st.strikethrough);
            if (st.obfuscated) w.Bool("obfuscated", *st.obfuscated);
            if (st.clickEvent) {
                using Action = Text::ClickEvent::Action;
                const Text::ClickEvent& e = *st.clickEvent;
                w.BeginCompound("click_event");
                w.String("action", Text::ClickEvent::ActionName(e.action));
                switch (e.action) {
                    case Action::OpenUrl:         w.String("url", e.value); break;
                    case Action::OpenFile:        w.String("path", e.value); break;
                    case Action::RunCommand:
                    case Action::SuggestCommand:  w.String("command", e.value); break;
                    case Action::CopyToClipboard: w.String("value", e.value); break;
                    case Action::ShowDialog:      w.String("dialog", e.value); break;
                    case Action::Custom:          w.String("id", e.value); break;
                    case Action::ChangePage:      w.Int("page", e.page); break;
                }
                w.EndCompound();
            }
            if (st.hoverText) {
                w.BeginCompound("hover_event");
                w.String("action", "show_text");
                WriteTextComponent(w, "value", *st.hoverText);
                w.EndCompound();
            }
            if (st.insertion) w.String("insertion", *st.insertion);
            if (st.font) w.String("font", *st.font);
        }

        // Filterable.codec's full form, which is what MC encodes with
        // (Codec.withAlternative encodes through the first codec).
        void WriteFilterableString(Nbt::Writer& w, std::string_view name, const Filterable<std::string>& f) {
            w.BeginCompound(name);
            w.String("raw", f.raw);
            if (f.filtered) w.String("filtered", *f.filtered);
            w.EndCompound();
        }

        // Filterable.codec's decode: the full {raw, filtered?} form first,
        // else the bare value.
        std::optional<Filterable<std::string>> ReadFilterableString(const ::World::NBTTag& tag) {
            if (auto str = dynamic_cast<const ::World::NBTTagString*>(&tag)) {
                return Filterable<std::string>::PassThrough(str->value);
            }
            auto c = dynamic_cast<const ::World::NBTTagCompound*>(&tag);
            if (!c) return std::nullopt;
            auto raw = std::dynamic_pointer_cast<::World::NBTTagString>(c->GetTag("raw"));
            if (!raw) return std::nullopt;
            Filterable<std::string> out = Filterable<std::string>::PassThrough(raw->value);
            if (auto filtered = std::dynamic_pointer_cast<::World::NBTTagString>(c->GetTag("filtered"))) {
                out.filtered = filtered->value;
            }
            return out;
        }

        std::optional<Filterable<Text::Component>> ReadFilterableComponent(const ::World::NBTTag& tag) {
            if (auto c = dynamic_cast<const ::World::NBTTagCompound*>(&tag)) {
                if (auto raw = c->GetTag("raw")) {
                    auto rawComponent = ReadTextComponent(*raw);
                    if (!rawComponent) return std::nullopt;
                    Filterable<Text::Component> out = Filterable<Text::Component>::PassThrough(std::move(*rawComponent));
                    if (auto filtered = c->GetTag("filtered")) {
                        out.filtered = ReadTextComponent(*filtered);
                    }
                    return out;
                }
            }
            auto component = ReadTextComponent(tag);
            if (!component) return std::nullopt;
            return Filterable<Text::Component>::PassThrough(std::move(*component));
        }

    } // namespace

    void WritePotionContentsBody(Nbt::Writer& w, const PotionContents& c) {
        if (c.potion) w.String("potion", std::string(kNamespace) + GetPotionKey(*c.potion));
        if (c.customColor) w.Int("custom_color", *c.customColor);
        // optionalFieldOf("custom_effects", List.of()) omits the empty list.
        if (!c.customEffects.empty()) {
            auto list = w.BeginList("custom_effects", Nbt::TagType::Compound);
            for (const auto& e : c.customEffects) {
                w.ListCompoundBegin(list);
                WriteEffect(w, e);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }
        if (c.customName) w.String("custom_name", *c.customName);
    }

    PotionContents ReadPotionContents(const ::World::NBTTag& tag) {
        PotionContents out;
        if (const auto* str = dynamic_cast<const ::World::NBTTagString*>(&tag)) {
            PotionId id;
            if (ParsePotionId(str->value, id)) out.potion = id;
            return out;
        }
        const auto* c = dynamic_cast<const ::World::NBTTagCompound*>(&tag);
        if (!c) return out;
        if (c->HasTag("potion")) {
            PotionId id;
            if (ParsePotionId(c->GetValue<std::string>("potion", ""), id)) out.potion = id;
        }
        if (c->HasTag("custom_color")) out.customColor = c->GetValue<int32_t>("custom_color", 0);
        if (auto list = std::dynamic_pointer_cast<::World::NBTTagList>(c->GetTag("custom_effects"))) {
            for (const auto& elem : list->value) {
                auto effect = std::dynamic_pointer_cast<::World::NBTTagCompound>(elem);
                if (!effect) continue;
                MobEffectInstance inst;
                if (ReadEffect(*effect, inst)) out.customEffects.push_back(std::move(inst));
            }
        }
        if (auto name = std::dynamic_pointer_cast<::World::NBTTagString>(c->GetTag("custom_name"))) {
            out.customName = name->value;
        }
        return out;
    }

    std::optional<Text::Component> ReadTextComponent(const ::World::NBTTag& tag) {
        return Text::FromJson(NbtToJson(*Unwrap(&tag)));
    }

    void WriteTextComponent(Nbt::Writer& w, std::string_view name, const Text::Component& component) {
        std::string collapsed;
        if (component.TryCollapseToString(collapsed)) {
            w.String(name, collapsed);
            return;
        }
        w.BeginCompound(name);
        WriteComponentBody(w, component);
        w.EndCompound();
    }

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
        const auto potion        = stack.components.get(DataComponents::POTION_CONTENTS);
        const auto durationScale = stack.components.get(DataComponents::POTION_DURATION_SCALE);
        const auto stew          = stack.components.get(DataComponents::SUSPICIOUS_STEW_EFFECTS);
        const auto writtenBook   = stack.components.get(DataComponents::WRITTEN_BOOK_CONTENT);
        auto       writableBook  = stack.components.get(DataComponents::WRITABLE_BOOK_CONTENT);
        // A book and quill's EMPTY content is its default (Items.java), and
        // MC's patch drops a value equal to the default — nothing to write.
        if (writableBook && writableBook->pages.empty() && stack.itemId == Items::WritableBook) {
            writableBook.reset();
        }

        // MC DataComponents.DYED_COLOR — DyedItemColor.CODEC is the bare
        // RGB int (ExtraCodecs.RGB_COLOR_CODEC).
        const auto dyedColor     = stack.components.get(DataComponents::DYED_COLOR);

        if (!customName.has_value() && !hasEnchants && !gunInstance.has_value() &&
            !sulfurBucket.has_value() && !potion.has_value() && !durationScale.has_value() &&
            !stew.has_value() && !writtenBook.has_value() && !writableBook.has_value() &&
            !dyedColor.has_value()) return;

        w.BeginCompound("components");
        if (potion.has_value()) {
            // PotionContents.CODEC's FULL form — the compound, even for a
            // plain potion (vanilla encodes with the primary codec).
            w.BeginCompound("minecraft:potion_contents");
            WritePotionContentsBody(w, *potion);
            w.EndCompound();
        }
        if (durationScale.has_value()) {
            w.Float("minecraft:potion_duration_scale", *durationScale);
        }
        if (dyedColor.has_value()) {
            w.Int("minecraft:dyed_color", *dyedColor);
        }
        if (stew.has_value()) {
            // SuspiciousStewEffects.CODEC: a list of {id, duration}.
            auto list = w.BeginList("minecraft:suspicious_stew_effects", Nbt::TagType::Compound);
            for (const auto& e : stew->effects) {
                w.ListCompoundBegin(list);
                w.String("id", std::string(kNamespace) + GetEffectName(e.effect));
                w.Int("duration", e.duration);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }
        if (writtenBook.has_value()) {
            // WrittenBookContent.CODEC: title (Filterable, full form),
            // author, then the three optionalFieldOf-with-default fields,
            // which DFU omits when they hold their default (generation 0, no
            // pages, resolved false).
            w.BeginCompound("minecraft:written_book_content");
            WriteFilterableString(w, "title", writtenBook->title);
            w.String("author", writtenBook->author);
            if (writtenBook->generation != 0) w.Int("generation", writtenBook->generation);
            if (!writtenBook->pages.empty()) {
                auto pages = w.BeginList("pages", Nbt::TagType::Compound);
                for (const auto& page : writtenBook->pages) {
                    w.ListCompoundBegin(pages);
                    WriteTextComponent(w, "raw", page.raw);
                    if (page.filtered) WriteTextComponent(w, "filtered", *page.filtered);
                    w.ListCompoundEnd(pages);
                }
                w.EndList(pages);
            }
            if (writtenBook->resolved) w.Bool("resolved", true);
            w.EndCompound();
        }
        if (writableBook.has_value()) {
            // WritableBookContent.CODEC: pages, omitted when empty.
            w.BeginCompound("minecraft:writable_book_content");
            if (!writableBook->pages.empty()) {
                auto pages = w.BeginList("pages", Nbt::TagType::Compound);
                for (const auto& page : writableBook->pages) {
                    w.ListCompoundBegin(pages);
                    w.String("raw", page.raw);
                    if (page.filtered) w.String("filtered", *page.filtered);
                    w.ListCompoundEnd(pages);
                }
                w.EndList(pages);
            }
            w.EndCompound();
        }
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

        if (auto potionTag = components->GetTag("minecraft:potion_contents")) {
            stack.components.set(DataComponents::POTION_CONTENTS, ReadPotionContents(*potionTag));
        }
        if (auto scale = std::dynamic_pointer_cast<::World::NBTTagFloat>(
                components->GetTag("minecraft:potion_duration_scale"))) {
            stack.components.set(DataComponents::POTION_DURATION_SCALE, scale->value);
        }
        // DyedItemColor.CODEC: an int — or, through RGB_COLOR_CODEC's
        // alternative, a list of three floats (0..1 per channel).
        if (auto dyed = components->GetTag("minecraft:dyed_color")) {
            if (auto i = std::dynamic_pointer_cast<::World::NBTTagInt>(dyed)) {
                stack.components.set(DataComponents::DYED_COLOR, static_cast<int32_t>(i->value));
            } else if (auto list = std::dynamic_pointer_cast<::World::NBTTagList>(dyed);
                       list && list->value.size() == 3) {
                int rgb[3] = { 0, 0, 0 };
                for (size_t c = 0; c < 3; ++c) {
                    if (auto f = std::dynamic_pointer_cast<::World::NBTTagFloat>(list->value[c])) {
                        rgb[c] = std::clamp(static_cast<int>(f->value * 255.0f), 0, 255);
                    }
                }
                stack.components.set(DataComponents::DYED_COLOR,
                                     static_cast<int32_t>((rgb[0] << 16) | (rgb[1] << 8) | rgb[2]));
            }
        }
        if (auto list = std::dynamic_pointer_cast<::World::NBTTagList>(
                components->GetTag("minecraft:suspicious_stew_effects"))) {
            SuspiciousStewEffects effects;
            for (const auto& elem : list->value) {
                auto entry = std::dynamic_pointer_cast<::World::NBTTagCompound>(elem);
                if (!entry) continue;
                MobEffectId id;
                if (!ParseEffectId(entry->GetValue<std::string>("id", ""), id)) continue;
                // Entry.CODEC: duration lenientOptionalFieldOf(160).
                effects.effects.push_back(
                    {id, entry->GetValue<int32_t>("duration", SuspiciousStewEffects::kDefaultDuration)});
            }
            stack.components.set(DataComponents::SUSPICIOUS_STEW_EFFECTS, effects);
        }

        if (auto book = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                components->GetTag("minecraft:written_book_content"))) {
            // WrittenBookContent.CODEC. Read leniently: a field MC's codec
            // would reject (an over-long title, a generation past 3) is kept
            // or clamped rather than costing the player the whole book.
            WrittenBookContent content;
            if (auto title = book->GetTag("title")) {
                if (auto f = ReadFilterableString(*title)) content.title = std::move(*f);
            }
            if (auto author = std::dynamic_pointer_cast<::World::NBTTagString>(book->GetTag("author"))) {
                content.author = author->value;
            }
            if (auto generation = book->GetTag("generation")) {
                const double g = NumberOf(*generation).value_or(0.0);
                content.generation = std::clamp(static_cast<int>(g), 0, WrittenBookContent::MAX_GENERATION);
            }
            if (auto pages = std::dynamic_pointer_cast<::World::NBTTagList>(book->GetTag("pages"))) {
                content.pages.reserve(pages->value.size());
                for (const auto& element : pages->value) {
                    if (!element) continue;
                    if (auto page = ReadFilterableComponent(*Unwrap(element.get()))) {
                        content.pages.push_back(std::move(*page));
                    }
                }
            }
            if (auto resolved = book->GetTag("resolved")) {
                content.resolved = NumberOf(*resolved).value_or(0.0) != 0.0;
            }
            stack.components.set(DataComponents::WRITTEN_BOOK_CONTENT, std::move(content));
        }
        if (auto book = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                components->GetTag("minecraft:writable_book_content"))) {
            // WritableBookContent.CODEC: Filterable<String(1024)> pages,
            // sizeLimitedListOf(100).
            WritableBookContent content;
            if (auto pages = std::dynamic_pointer_cast<::World::NBTTagList>(book->GetTag("pages"))) {
                for (const auto& element : pages->value) {
                    if (!element) continue;
                    if (content.pages.size() >= static_cast<size_t>(WritableBookContent::MAX_PAGES)) break;
                    if (auto page = ReadFilterableString(*Unwrap(element.get()))) {
                        content.pages.push_back(std::move(*page));
                    }
                }
            }
            stack.components.set(DataComponents::WRITABLE_BOOK_CONTENT, std::move(content));
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
