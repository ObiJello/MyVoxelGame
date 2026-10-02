// File: src/server/world/storage/anvil/ItemStackNbt.cpp
#include "common/core/Features.hpp"
#include <algorithm>
#include <iterator>
#include <optional>
#include "server/world/storage/anvil/ItemStackNbt.hpp"
#include "server/world/storage/anvil/ComponentNbt.hpp"

#include "common/core/Log.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/mobs/TropicalFishVariant.hpp"
#include "common/entity/raid/OminousBanner.hpp"
#include "common/text/TextComponent.hpp"
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

        // ItemEnchantments.CODEC at DataVersion 4764+: Codec.unboundedMap(
        // Enchantment.CODEC, LEVEL_CODEC) — a FLAT compound of name -> level.
        // The `{levels: {...}}` wrapper older notes describe was
        // 1.20.5..1.21.4 only.
        void WriteEnchantments(Nbt::Writer& w, std::string_view name, const ItemEnchantments& e) {
            w.BeginCompound(name);
            for (const EnchantmentInstance& entry : e.entries) {
                const Enchantment& def = EnchantmentRegistry::Get(entry.id);
                if (def.slug.empty()) continue;
                w.Int(std::string(kNamespace) + def.slug, std::clamp(entry.level, 1, 255));
            }
            w.EndCompound();
        }

        // The reverse; an enchantment this build lacks is dropped, as is a
        // level outside LEVEL_CODEC's 1..255.
        ItemEnchantments ReadEnchantments(const ::World::NBTTagCompound& c) {
            ItemEnchantments out;
            for (const auto& [key, value] : c.value) {
                if (!value) continue;
                const std::optional<double> level = NumberOf(*value);
                if (!level) continue;
                const auto found = EnchantmentRegistry::ByName(StripNamespace(key));
                if (!found) continue;
                const int lvl = static_cast<int>(*level);
                if (lvl >= 1 && lvl <= 255) out.Set(*found, lvl);
            }
            return out;
        }

        // A HolderSet<T> as RegistryCodecs.holderSet writes it: a "#tag"
        // string, a single id string, or a list of ids.
        void WriteHolderSet(Nbt::Writer& w, std::string_view name, const std::vector<std::string>& entries) {
            if (entries.size() == 1) {
                w.String(name, entries.front());
                return;
            }
            auto list = w.BeginList(name, Nbt::TagType::String);
            for (const std::string& e : entries) w.ListString(list, e);
            w.EndList(list);
        }

        std::vector<std::string> ReadHolderSet(const ::World::NBTTag* tag) {
            std::vector<std::string> out;
            if (!tag) return out;
            if (auto str = dynamic_cast<const ::World::NBTTagString*>(tag)) {
                out.push_back(str->value);
            } else if (auto list = dynamic_cast<const ::World::NBTTagList*>(tag)) {
                for (const auto& e : list->value) {
                    if (!e) continue;
                    if (auto str = dynamic_cast<const ::World::NBTTagString*>(Unwrap(e.get()))) {
                        out.push_back(str->value);
                    }
                }
            }
            return out;
        }

        std::optional<int32_t> IntComponent(const ::World::NBTTagCompound& components, const char* key) {
            auto tag = components.GetTag(key);
            if (!tag) return std::nullopt;
            const std::optional<double> v = NumberOf(*tag);
            if (!v) return std::nullopt;
            return static_cast<int32_t>(*v);
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

    namespace {
        // FireworkExplosion.CODEC: shape (the serialized name), colors /
        // fade_colors (Codec.INT lists — an IntArray tag under NbtOps),
        // has_trail / has_twinkle; the optional fields omitted at their
        // defaults, as DFU does.
        void WriteFireworkExplosionBody(Nbt::Writer& w, const FireworkExplosion& e) {
            w.String("shape", std::string(FireworkExplosion::ShapeName(e.shape)));
            if (!e.colors.empty()) w.IntArray("colors", e.colors.data(), e.colors.size());
            if (!e.fadeColors.empty()) w.IntArray("fade_colors", e.fadeColors.data(), e.fadeColors.size());
            if (e.hasTrail) w.Bool("has_trail", true);
            if (e.hasTwinkle) w.Bool("has_twinkle", true);
        }

        // An int list in any of the forms an NBT reader may meet: the
        // IntArray NbtOps writes, or a list of numbers (SNBT, other tools).
        std::vector<int32_t> ReadIntList(const ::World::NBTTag* tag) {
            std::vector<int32_t> out;
            if (!tag) return out;
            if (auto arr = dynamic_cast<const ::World::NBTTagIntArray*>(tag)) return arr->value;
            if (auto list = dynamic_cast<const ::World::NBTTagList*>(tag)) {
                for (const auto& element : list->value) {
                    if (!element) continue;
                    if (auto n = NumberOf(*element)) out.push_back(static_cast<int32_t>(*n));
                }
            }
            return out;
        }

        std::optional<FireworkExplosion> ReadFireworkExplosion(const ::World::NBTTagCompound& c) {
            FireworkExplosion e;
            // `shape` is required (fieldOf); an unknown name fails the codec.
            if (!FireworkExplosion::ShapeFromName(c.GetValue<std::string>("shape", ""), e.shape)) {
                return std::nullopt;
            }
            e.colors     = ReadIntList(c.GetTag("colors").get());
            e.fadeColors = ReadIntList(c.GetTag("fade_colors").get());
            e.hasTrail   = c.GetValue<int8_t>("has_trail", 0) != 0;
            e.hasTwinkle = c.GetValue<int8_t>("has_twinkle", 0) != 0;
            return e;
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

    void WriteTextComponentBody(Nbt::Writer& w, const Text::Component& component) {
        WriteComponentBody(w, component);
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
        // Defaults this stack takes away (`[!max_damage]`).
        const std::vector<const DataComponentTypeBase*> removedDefaults = stack.components.removedTypes();
        const auto stored     = stack.components.get(DataComponents::STORED_ENCHANTMENTS);
        const bool hasEnchants = stored.has_value() && !stored->entries.empty();
        // Durability / enchanting (the stack's patch over its item: MC saves
        // only components that differ from the prototype, which is exactly
        // what stack.components holds).
        const auto enchantments   = stack.components.get(DataComponents::ENCHANTMENTS);
        const bool hasItemEnchants = enchantments.has_value() && !enchantments->IsEmpty();
        // (DAMAGE / MAX_DAMAGE / UNBREAKABLE / MAX_STACK_SIZE / CUSTOM_DATA
        // are registered codecs — components/StackNbt.cpp.)
        const auto repairCost     = stack.components.get(DataComponents::REPAIR_COST);
        const auto repairable     = stack.components.get(DataComponents::REPAIRABLE);
        const auto enchantable    = stack.components.get(DataComponents::ENCHANTABLE);
        const auto weapon         = stack.components.get(DataComponents::WEAPON);
        const auto breakSound     = stack.components.get(DataComponents::BREAK_SOUND);
        const auto resistant      = stack.components.get(DataComponents::DAMAGE_RESISTANT);
        const bool hasDurabilityData = hasItemEnchants ||
                                       repairCost || repairable || enchantable || weapon ||
                                       breakSound || resistant;
#if ENABLE_PORTAL_GUN
        // The gun's pair is keyed by this id (PortalRegistry); losing it on
        // save orphaned the saved pair from the reloaded gun.
        const auto gunInstance = stack.components.get(DataComponents::PORTAL_GUN_INSTANCE_ID);
#else
        const std::optional<uint64_t> gunInstance;
#endif

        // MC minecraft:axolotl/variant — the axolotl bucket. (bucket_entity_
        // data and sulfur_cube_content are registered codecs — EntityDataNbt.)
        const auto axolotlVariant = stack.components.get(DataComponents::AXOLOTL_VARIANT);
        // MC minecraft:salmon/size and minecraft:tropical_fish/{pattern,
        // base_color, pattern_color} — the salmon and tropical fish buckets.
        const auto salmonSize        = stack.components.get(DataComponents::SALMON_SIZE);
        const auto fishPattern       = stack.components.get(DataComponents::TROPICAL_FISH_PATTERN);
        const auto fishBaseColor     = stack.components.get(DataComponents::TROPICAL_FISH_BASE_COLOR);
        const auto fishPatternColor  = stack.components.get(DataComponents::TROPICAL_FISH_PATTERN_COLOR);
        const bool hasFishData       = salmonSize.has_value() || fishPattern.has_value() ||
                                       fishBaseColor.has_value() || fishPatternColor.has_value();
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
        // MC DataComponents.PAINTING_VARIANT — the variant holder's id.
        const auto paintingVariant = stack.components.get(DataComponents::PAINTING_VARIANT);
        // MC DataComponents.MAP_ID (MapId.CODEC: the bare int),
        // MAP_DECORATIONS (a compound of key -> {type, x, z, rotation}) and
        // MAP_COLOR (MapItemColor.CODEC: the bare RGB int).
        const auto mapId          = stack.components.get(DataComponents::MAP_ID);
        const auto mapDecorations = stack.components.get(DataComponents::MAP_DECORATIONS);
        const auto mapColor       = stack.components.get(DataComponents::MAP_COLOR);
        const bool hasMapData     = mapId.has_value() || mapColor.has_value() ||
                                    (mapDecorations.has_value() && !mapDecorations->decorations.empty());
        // MC DataComponents.FIREWORKS / FIREWORK_EXPLOSION /
        // CHARGED_PROJECTILES — a rocket's flight and stars, a star's
        // explosion, a loaded crossbow's projectiles.
        const auto fireworks         = stack.components.get(DataComponents::FIREWORKS);
        const auto fireworkExplosion = stack.components.get(DataComponents::FIREWORK_EXPLOSION);
        const auto chargedProjectiles = stack.components.get(DataComponents::CHARGED_PROJECTILES);
        const bool hasFireworkData   = fireworks.has_value() || fireworkExplosion.has_value() ||
                                       chargedProjectiles.has_value();
        // MC DataComponents.OMINOUS_BOTTLE_AMPLIFIER — OminousBottleAmplifier
        // .CODEC is the bare int (0..4).
        const auto ominousAmplifier  = stack.components.get(DataComponents::OMINOUS_BOTTLE_AMPLIFIER);
        // MC DataComponents.INSTRUMENT — InstrumentComponent.CODEC is an
        // EitherHolder: the registry id as a bare string.
        const auto instrument        = stack.components.get(DataComponents::INSTRUMENT);
        // MC DataComponents.POT_DECORATIONS (PotDecorations.CODEC: optional
        // back / left / right / front ItemStackTemplates) and CONTAINER
        // (ItemContainerContents.CODEC: [{slot, item}]).
        const auto potDecorations    = stack.components.get(DataComponents::POT_DECORATIONS);
        const auto containerContents = stack.components.get(DataComponents::CONTAINER);
        // MC DataComponents.BANNER_PATTERNS (BannerPatternLayers.CODEC: a
        // list of {pattern, color}), ITEM_NAME (a text Component) and RARITY
        // (the lower-case enum name) — the ominous banner's patch.
        const auto bannerPatterns    = stack.components.get(DataComponents::BANNER_PATTERNS);
        const auto itemName          = stack.components.get(DataComponents::ITEM_NAME);
        const auto rarity            = stack.components.get(DataComponents::RARITY);
        const bool hasBannerData     = (bannerPatterns.has_value() && !bannerPatterns->IsEmpty()) ||
                                       itemName.has_value() || rarity.has_value();
        // Every component written through the ComponentNbt registry (each
        // codec lives beside its component's own code).
        bool hasRegisteredComponent = false;
        for (const ComponentNbt::Codec& codec : ComponentNbt::All()) {
            if (codec.write && stack.components.has(*codec.type)) { hasRegisteredComponent = true; break; }
        }

        if (!customName.has_value() && !hasEnchants && !gunInstance.has_value() && !hasBannerData &&
            !axolotlVariant.has_value() &&
            !hasFishData &&
            !potion.has_value() && !durationScale.has_value() &&
            !stew.has_value() && !writtenBook.has_value() && !writableBook.has_value() &&
            !dyedColor.has_value() && !hasDurabilityData && !paintingVariant.has_value() &&
            !hasMapData && !hasFireworkData && !ominousAmplifier.has_value() &&
            !instrument.has_value() && !potDecorations.has_value() &&
            !containerContents.has_value() && removedDefaults.empty() && !hasRegisteredComponent) return;

        w.BeginCompound("components");
        // DataComponentPatch.CODEC: a removed default is `"!minecraft:<id>": {}`.
        for (const DataComponentTypeBase* type : removedDefaults) {
            w.BeginCompound("!" + std::string(kNamespace) + type->name);
            w.EndCompound();
        }
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
        if (ominousAmplifier.has_value()) {
            w.Int("minecraft:ominous_bottle_amplifier", *ominousAmplifier);
        }
        if (instrument.has_value()) {
            w.String("minecraft:instrument", *instrument);
        }
        if (potDecorations.has_value()) {
            static constexpr const char* kSides[4] = { "back", "left", "right", "front" };
            w.BeginCompound("minecraft:pot_decorations");
            for (int side = 0; side < 4; ++side) {
                const std::string name = ItemName(potDecorations->sides[static_cast<size_t>(side)]);
                if (name.empty()) continue;
                w.BeginCompound(kSides[side]);
                w.String("id", name);
                w.EndCompound();
            }
            w.EndCompound();
        }
        if (containerContents.has_value()) {
            auto list = w.BeginList("minecraft:container", Nbt::TagType::Compound);
            for (size_t slot = 0; slot < containerContents->items.size(); ++slot) {
                const ItemStack& item = containerContents->items[slot];
                if (item.IsEmpty()) continue;
                w.ListCompoundBegin(list);
                w.Int("slot", static_cast<int32_t>(slot));
                w.BeginCompound("item");
                WriteItemStackBody(w, item);
                w.EndCompound();
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }
        if (bannerPatterns.has_value() && !bannerPatterns->IsEmpty()) {
            // BannerPatternLayers.CODEC = Layer.CODEC.listOf(): {pattern:
            // the banner_pattern id, color: DyeColor's serialized name}.
            static constexpr const char* kDyeNames[16] = {
                "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black" };
            auto list = w.BeginList("minecraft:banner_patterns", Nbt::TagType::Compound);
            for (const BannerPatternLayer& layer : bannerPatterns->layers) {
                w.ListCompoundBegin(list);
                w.String("pattern", layer.pattern);
                w.String("color", kDyeNames[layer.color & 15]);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }
        // ITEM_NAME / CUSTOM_NAME / LORE: written as text components by
        // their ComponentNbt codecs (components/PresentationNbt.cpp).
        if (rarity.has_value()) {
            static constexpr const char* kRarityNames[] = { "common", "uncommon", "rare", "epic" };
            const auto index = static_cast<size_t>(*rarity);
            if (index < std::size(kRarityNames)) w.String("minecraft:rarity", kRarityNames[index]);
        }
        if (paintingVariant.has_value()) {
            w.String("minecraft:painting/variant", *paintingVariant);
        }
        if (fireworks.has_value()) {
            // Fireworks.CODEC: flight_duration (UNSIGNED_BYTE, default 0),
            // explosions (default empty).
            w.BeginCompound("minecraft:fireworks");
            if (fireworks->flightDuration != 0) {
                w.Byte("flight_duration", static_cast<int8_t>(std::clamp(fireworks->flightDuration, 0, 255)));
            }
            if (!fireworks->explosions.empty()) {
                auto list = w.BeginList("explosions", Nbt::TagType::Compound);
                for (const FireworkExplosion& e : fireworks->explosions) {
                    w.ListCompoundBegin(list);
                    WriteFireworkExplosionBody(w, e);
                    w.ListCompoundEnd(list);
                }
                w.EndList(list);
            }
            w.EndCompound();
        }
        if (fireworkExplosion.has_value()) {
            w.BeginCompound("minecraft:firework_explosion");
            WriteFireworkExplosionBody(w, *fireworkExplosion);
            w.EndCompound();
        }
        if (chargedProjectiles.has_value()) {
            // ChargedProjectiles.CODEC: a list of ItemStackTemplates.
            auto list = w.BeginList("minecraft:charged_projectiles", Nbt::TagType::Compound);
            for (const ItemStack& projectile : chargedProjectiles->items) {
                if (projectile.IsEmpty()) continue;
                w.ListCompoundBegin(list);
                WriteItemStackBody(w, projectile);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }
        if (mapId.has_value()) {
            w.Int("minecraft:map_id", *mapId);
        }
        if (mapColor.has_value()) {
            w.Int("minecraft:map_color", *mapColor);
        }
        if (mapDecorations.has_value() && !mapDecorations->decorations.empty()) {
            // MapDecorations.CODEC: Codec.unboundedMap(STRING, Entry.CODEC),
            // Entry = {type (registry id), x, z (double), rotation (float)}.
            w.BeginCompound("minecraft:map_decorations");
            for (const auto& [key, entry] : mapDecorations->decorations) {
                w.BeginCompound(key);
                w.String("type", std::string(kNamespace) + std::string(Maps::Info(entry.type).name));
                w.Double("x", entry.x);
                w.Double("z", entry.z);
                w.Float("rotation", entry.rotation);
                w.EndCompound();
            }
            w.EndCompound();
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
        if (axolotlVariant.has_value()) {
            // Axolotl.Variant.CODEC — the StringRepresentable name.
            static constexpr const char* kAxolotlVariants[] = {"lucy", "wild", "gold", "cyan", "blue"};
            const int32_t v = (*axolotlVariant >= 0 && *axolotlVariant < 5) ? *axolotlVariant : 0;
            w.String("minecraft:axolotl/variant", kAxolotlVariants[v]);
        }
        if (salmonSize.has_value()) {
            // Salmon.Variant.CODEC — the StringRepresentable name.
            static constexpr const char* kSalmonSizes[] = {"small", "medium", "large"};
            w.String("minecraft:salmon/size", kSalmonSizes[std::clamp<int32_t>(*salmonSize, 0, 2)]);
        }
        if (fishPattern.has_value()) {
            // TropicalFish.Pattern.CODEC — the StringRepresentable name.
            const int32_t p = (*fishPattern >= 0 && *fishPattern < TropicalFishVariants::kPatternCount)
                ? *fishPattern : 0;
            w.String("minecraft:tropical_fish/pattern", std::string(TropicalFishVariants::PatternName(
                static_cast<TropicalFishVariants::Pattern>(p))));
        }
        if (fishBaseColor.has_value()) {
            // DyeColor.CODEC — the colour's name.
            w.String("minecraft:tropical_fish/base_color",
                     std::string(TropicalFishVariants::DyeName(*fishBaseColor)));
        }
        if (fishPatternColor.has_value()) {
            w.String("minecraft:tropical_fish/pattern_color",
                     std::string(TropicalFishVariants::DyeName(*fishPatternColor)));
        }
        if (gunInstance.has_value()) {
            w.Long(std::string(kOwnNamespace) + "portal_gun_instance_id",
                   static_cast<int64_t>(*gunInstance));
        }
        if (hasEnchants) WriteEnchantments(w, "minecraft:stored_enchantments", *stored);
        if (hasItemEnchants) WriteEnchantments(w, "minecraft:enchantments", *enchantments);
        // REPAIR_COST: a bare int (NON_NEGATIVE_INT).
        if (repairCost) w.Int("minecraft:repair_cost", std::max(0, *repairCost));
        if (repairable && !repairable->items.empty()) {
            // Repairable.CODEC: {items: HolderSet<Item>}.
            w.BeginCompound("minecraft:repairable");
            WriteHolderSet(w, "items", repairable->items);
            w.EndCompound();
        }
        if (enchantable && *enchantable > 0) {
            // Enchantable.CODEC: {value: POSITIVE_INT}.
            w.BeginCompound("minecraft:enchantable");
            w.Int("value", *enchantable);
            w.EndCompound();
        }
        if (weapon) {
            // Weapon.CODEC: both fields optional with defaults 1 / 0.0.
            w.BeginCompound("minecraft:weapon");
            if (weapon->itemDamagePerAttack != 1) w.Int("item_damage_per_attack", weapon->itemDamagePerAttack);
            if (weapon->disableBlockingForSeconds != 0.0f) {
                w.Float("disable_blocking_for_seconds", weapon->disableBlockingForSeconds);
            }
            w.EndCompound();
        }
        if (breakSound && !breakSound->empty()) {
            // SoundEvent.CODEC — the registry id form.
            const std::string& sound = *breakSound;
            w.String("minecraft:break_sound",
                     sound.find(':') == std::string::npos ? std::string(kNamespace) + sound : sound);
        }
        if (resistant && !resistant->empty()) {
            // DamageResistant.CODEC: {types: TagKey<DamageType>} ("#ns:tag").
            w.BeginCompound("minecraft:damage_resistant");
            w.String("types", *resistant);
            w.EndCompound();
        }
        if (hasRegisteredComponent) {
            for (const ComponentNbt::Codec& codec : ComponentNbt::All()) {
                if (!codec.write || !stack.components.has(*codec.type)) continue;
                codec.write(w, std::string(kNamespace) + codec.type->name, stack);
            }
        }
        w.EndCompound();
    }

    ItemStack ReadItemStack(const ::World::NBTTagCompound& tag, std::string* error) {
        ItemStack stack;

        const std::string id = tag.GetValue<std::string>("id");
        if (id.empty()) return stack;
        stack.itemId = ItemFromName(id);
        if (stack.itemId == Items::Air) return stack;   // unknown item -> nothing, as vanilla does

        stack.count = tag.GetValue<int32_t>("count", 1);
        if (stack.count <= 0) return ItemStack{};
        // ItemStack.CODEC's count range (1..99); the stack's own limit is
        // applied once its components (a max_stack_size patch) are read.
        stack.count = std::min(stack.count, DataComponents::kMaxStackSizeLimit);

        auto components = std::dynamic_pointer_cast<::World::NBTTagCompound>(tag.GetTag("components"));
        if (!components) {
            stack.count = std::min(stack.count, GetMaxStackSize(stack));
            return stack;
        }

        // DataComponentPatch.CODEC's removals: `"!minecraft:<id>": {}` takes
        // the item's default away. Unknown ids are dropped, as vanilla does.
        for (const auto& [key, value] : components->value) {
            (void)value;
            if (key.empty() || key[0] != '!') continue;
            std::string name = key.substr(1);
            if (name.rfind(kNamespace, 0) == 0) name.erase(0, std::string_view(kNamespace).size());
            if (const DataComponentTypeBase* type = DataComponents::ByName(name)) stack.components.setRemoved(*type);
        }

#if ENABLE_PORTAL_GUN
        if (auto gun = std::dynamic_pointer_cast<::World::NBTTagLong>(
                components->GetTag(std::string(kOwnNamespace) + "portal_gun_instance_id"))) {
            stack.components.set(DataComponents::PORTAL_GUN_INSTANCE_ID, static_cast<uint64_t>(gun->value));
        }
#endif

        // custom_name / item_name / lore: the ComponentNbt codecs
        // (components/PresentationNbt.cpp) read them as text components.
        // RARITY: Rarity's serialized (lower-case) name.
        if (auto rarityTag = std::dynamic_pointer_cast<::World::NBTTagString>(
                components->GetTag("minecraft:rarity"))) {
            static constexpr const char* kRarityNames[] = { "common", "uncommon", "rare", "epic" };
            std::string_view value = rarityTag->value;
            if (value.rfind("minecraft:", 0) == 0) value.remove_prefix(10);
            for (size_t i = 0; i < std::size(kRarityNames); ++i) {
                if (value == kRarityNames[i]) {
                    stack.components.set(DataComponents::RARITY, static_cast<Rarity>(i));
                    break;
                }
            }
        }
        // BANNER_PATTERNS: [{pattern: id, color: dye name}, …]. A layer whose
        // colour is not a DyeColor fails the codec (MC drops the component).
        if (auto patterns = std::dynamic_pointer_cast<::World::NBTTagList>(
                components->GetTag("minecraft:banner_patterns"))) {
            static constexpr const char* kDyeNames[16] = {
                "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black" };
            BannerPatternLayers layers;
            bool valid = true;
            for (const auto& elem : patterns->value) {
                auto layerTag = std::dynamic_pointer_cast<::World::NBTTagCompound>(elem);
                if (!layerTag) { valid = false; break; }
                BannerPatternLayer layer;
                layer.pattern = layerTag->GetValue<std::string>("pattern", "");
                if (layer.pattern.empty()) { valid = false; break; }
                if (layer.pattern.find(':') == std::string::npos) layer.pattern = "minecraft:" + layer.pattern;
                const std::string color = layerTag->GetValue<std::string>("color", "");
                int found = -1;
                for (int i = 0; i < 16; ++i) if (color == kDyeNames[i]) { found = i; break; }
                if (found < 0) { valid = false; break; }
                layer.color = static_cast<uint8_t>(found);
                layers.layers.push_back(std::move(layer));
            }
            if (valid && !layers.IsEmpty()) {
                stack.components.set(DataComponents::BANNER_PATTERNS, std::move(layers));
            }
        }

        // The engine's pre-26.3 "obeycraft:sulfur_cube_bucket" compound
        // {content, age, age_locked, NoAI}: converted to MC's
        // sulfur_cube_content + bucket_entity_data {NoAI, age, age_locked}.
        // (A save that has minecraft:bucket_entity_data too is read by its
        // registered codec below, which then wins.)
        if (auto sb = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                components->GetTag(std::string(kOwnNamespace) + "sulfur_cube_bucket"))) {
            const ItemID content = ItemFromName(sb->GetValue<std::string>("content", ""));
            if (content != Items::Air) {
                stack.components.set(DataComponents::SULFUR_CUBE_CONTENT, SulfurCubeContent{ItemStack(content, 1)});
            }
            BucketEntityData data;
            data.noAi = sb->GetValue<int8_t>("NoAI", 0) != 0;
            auto extra = std::make_shared<::World::NBTTagCompound>();
            extra->value["age"] = std::make_shared<::World::NBTTagInt>(sb->GetValue<int32_t>("age", 0));
            extra->value["age_locked"] = std::make_shared<::World::NBTTagByte>(
                static_cast<int8_t>(sb->GetValue<int8_t>("age_locked", 0) != 0 ? 1 : 0));
            data.extra = NbtCompoundValue(std::move(extra));
            stack.components.set(DataComponents::BUCKET_ENTITY_DATA, std::move(data));
        }
        if (auto av = components->GetTag("minecraft:axolotl/variant")) {
            // The name (MC's codec); an int id is accepted too.
            static constexpr const char* kAxolotlVariants[] = {"lucy", "wild", "gold", "cyan", "blue"};
            int32_t id = -1;
            if (auto s = std::dynamic_pointer_cast<::World::NBTTagString>(av)) {
                std::string name = s->value;
                if (name.rfind("minecraft:", 0) == 0) name = name.substr(10);
                for (int32_t i = 0; i < 5; ++i) if (name == kAxolotlVariants[i]) id = i;
            } else if (auto n = std::dynamic_pointer_cast<::World::NBTTagInt>(av)) {
                id = n->value;
            }
            if (id >= 0 && id < 5) stack.components.set(DataComponents::AXOLOTL_VARIANT, id);
        }
        if (auto size = std::dynamic_pointer_cast<::World::NBTTagString>(
                components->GetTag("minecraft:salmon/size"))) {
            // Salmon.Variant.CODEC's names (small 0, medium 1, large 2).
            static constexpr std::string_view kSalmonSizes[] = {"small", "medium", "large"};
            std::string_view name = size->value;
            if (name.rfind("minecraft:", 0) == 0) name.remove_prefix(10);
            for (int32_t id = 0; id < 3; ++id) {
                if (name == kSalmonSizes[id]) stack.components.set(DataComponents::SALMON_SIZE, id);
            }
        }
        if (auto pattern = std::dynamic_pointer_cast<::World::NBTTagString>(
                components->GetTag("minecraft:tropical_fish/pattern"))) {
            if (auto p = TropicalFishVariants::PatternFromName(pattern->value)) {
                stack.components.set(DataComponents::TROPICAL_FISH_PATTERN, static_cast<int32_t>(*p));
            }
        }
        if (auto color = std::dynamic_pointer_cast<::World::NBTTagString>(
                components->GetTag("minecraft:tropical_fish/base_color"))) {
            if (auto c = TropicalFishVariants::DyeFromName(color->value)) {
                stack.components.set(DataComponents::TROPICAL_FISH_BASE_COLOR, static_cast<int32_t>(*c));
            }
        }
        if (auto color = std::dynamic_pointer_cast<::World::NBTTagString>(
                components->GetTag("minecraft:tropical_fish/pattern_color"))) {
            if (auto c = TropicalFishVariants::DyeFromName(color->value)) {
                stack.components.set(DataComponents::TROPICAL_FISH_PATTERN_COLOR, static_cast<int32_t>(*c));
            }
        }

        if (auto potionTag = components->GetTag("minecraft:potion_contents")) {
            stack.components.set(DataComponents::POTION_CONTENTS, ReadPotionContents(*potionTag));
        }
        if (auto scale = std::dynamic_pointer_cast<::World::NBTTagFloat>(
                components->GetTag("minecraft:potion_duration_scale"))) {
            stack.components.set(DataComponents::POTION_DURATION_SCALE, scale->value);
        }
        // OminousBottleAmplifier.CODEC: ExtraCodecs.intRange(0, 4) — a value
        // outside it fails the component (MC drops the stack's patch entry).
        // InstrumentComponent.CODEC: a holder id (a direct inline
        // instrument has no id to keep and is dropped).
        if (auto decorations = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                components->GetTag("minecraft:pot_decorations"))) {
            static constexpr const char* kSides[4] = { "back", "left", "right", "front" };
            PotDecorations value;
            for (int side = 0; side < 4; ++side) {
                auto entry = decorations->GetTag(kSides[side]);
                if (auto c = std::dynamic_pointer_cast<::World::NBTTagCompound>(entry)) {
                    value.sides[static_cast<size_t>(side)] = ItemFromName(c->GetValue<std::string>("id", ""));
                } else if (auto str = std::dynamic_pointer_cast<::World::NBTTagString>(entry)) {
                    value.sides[static_cast<size_t>(side)] = ItemFromName(str->value);
                }
            }
            stack.components.set(DataComponents::POT_DECORATIONS, value);
        } else if (auto sideList = std::dynamic_pointer_cast<::World::NBTTagList>(
                       components->GetTag("minecraft:pot_decorations"))) {
            // The pre-26 form: a list of up to four item ids, back, left,
            // right, front.
            PotDecorations value;
            for (size_t i = 0; i < sideList->value.size() && i < 4; ++i) {
                if (auto str = std::dynamic_pointer_cast<::World::NBTTagString>(sideList->value[i])) {
                    value.sides[i] = ItemFromName(str->value);
                }
            }
            stack.components.set(DataComponents::POT_DECORATIONS, value);
        }
        if (auto contents = std::dynamic_pointer_cast<::World::NBTTagList>(
                components->GetTag("minecraft:container"))) {
            ItemContainerContents value;
            for (const auto& element : contents->value) {
                auto entry = std::dynamic_pointer_cast<::World::NBTTagCompound>(element);
                if (!entry) continue;
                const int slot = entry->GetValue<int32_t>("slot", -1);
                auto itemTag = std::dynamic_pointer_cast<::World::NBTTagCompound>(entry->GetTag("item"));
                if (slot < 0 || slot >= 256 || !itemTag) continue;
                ItemStack item = ReadItemStack(*itemTag);
                if (item.IsEmpty()) continue;
                if (value.items.size() <= static_cast<size_t>(slot)) value.items.resize(static_cast<size_t>(slot) + 1);
                value.items[static_cast<size_t>(slot)] = std::move(item);
            }
            stack.components.set(DataComponents::CONTAINER, std::move(value));
        }
        if (auto instrument = std::dynamic_pointer_cast<::World::NBTTagString>(
                components->GetTag("minecraft:instrument"))) {
            std::string id = instrument->value;
            if (id.find(':') == std::string::npos) id = "minecraft:" + id;
            stack.components.set(DataComponents::INSTRUMENT, id);
        }
        if (auto amplifier = components->GetTag("minecraft:ominous_bottle_amplifier")) {
            const int32_t value = components->GetValue<int32_t>("minecraft:ominous_bottle_amplifier", -1);
            if (amplifier->type != ::World::NBTTagType::TAG_Compound && value >= 0 &&
                value <= DataComponents::kOminousBottleMaxAmplifier) {
                stack.components.set(DataComponents::OMINOUS_BOTTLE_AMPLIFIER, value);
            }
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
        if (auto fw = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                components->GetTag("minecraft:fireworks"))) {
            Fireworks value;
            value.flightDuration = fw->GetValue<int32_t>("flight_duration", 0) & 0xFF;
            if (auto list = std::dynamic_pointer_cast<::World::NBTTagList>(fw->GetTag("explosions"))) {
                for (const auto& element : list->value) {
                    auto c = std::dynamic_pointer_cast<::World::NBTTagCompound>(element);
                    if (!c) continue;
                    if (value.explosions.size() >= Fireworks::kMaxExplosions) break;
                    if (auto e = ReadFireworkExplosion(*c)) value.explosions.push_back(std::move(*e));
                }
            }
            // The item's own default (Fireworks(1, [])) is no patch.
            const auto prototype = ItemRegistry::Get(stack.itemId).defaultComponents.get(DataComponents::FIREWORKS);
            if (!prototype || !(*prototype == value)) stack.components.set(DataComponents::FIREWORKS, std::move(value));
        }
        if (auto fe = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                components->GetTag("minecraft:firework_explosion"))) {
            if (auto e = ReadFireworkExplosion(*fe)) stack.components.set(DataComponents::FIREWORK_EXPLOSION, std::move(*e));
        }
        if (auto cp = std::dynamic_pointer_cast<::World::NBTTagList>(
                components->GetTag("minecraft:charged_projectiles"))) {
            ChargedProjectiles value;
            for (const auto& element : cp->value) {
                auto c = std::dynamic_pointer_cast<::World::NBTTagCompound>(element);
                if (!c) continue;
                if (value.items.size() >= ChargedProjectiles::kMaxSize) break;
                ItemStack projectile = ReadItemStack(*c);
                if (!projectile.IsEmpty()) value.items.push_back(std::move(projectile));
            }
            // EMPTY is the crossbow's default — no patch.
            if (!value.items.empty()) stack.components.set(DataComponents::CHARGED_PROJECTILES, std::move(value));
        }
        if (auto variant = std::dynamic_pointer_cast<::World::NBTTagString>(
                components->GetTag("minecraft:painting/variant"))) {
            stack.components.set(DataComponents::PAINTING_VARIANT, variant->value);
        }
        if (auto mapIdTag = std::dynamic_pointer_cast<::World::NBTTagInt>(components->GetTag("minecraft:map_id"))) {
            stack.components.set(DataComponents::MAP_ID, static_cast<int32_t>(mapIdTag->value));
        }
        if (auto mapColorTag = std::dynamic_pointer_cast<::World::NBTTagInt>(components->GetTag("minecraft:map_color"))) {
            stack.components.set(DataComponents::MAP_COLOR, static_cast<int32_t>(mapColorTag->value));
        }
        if (auto decorationsTag = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                components->GetTag("minecraft:map_decorations"))) {
            Maps::MapDecorations decorations;
            for (const auto& [key, tag] : decorationsTag->value) {
                auto entryTag = std::dynamic_pointer_cast<::World::NBTTagCompound>(tag);
                if (!entryTag) continue;
                const auto type = Maps::DecorationTypeFromKey(entryTag->GetValue<std::string>("type", ""));
                if (!type) continue;
                Maps::MapDecorations::Entry entry;
                entry.type     = *type;
                entry.x        = entryTag->GetValue<double>("x", 0.0);
                entry.z        = entryTag->GetValue<double>("z", 0.0);
                entry.rotation = entryTag->GetValue<float>("rotation", 0.0f);
                decorations.decorations.emplace_back(key, entry);
            }
            if (!decorations.decorations.empty()) stack.components.set(DataComponents::MAP_DECORATIONS, decorations);
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
            ItemEnchantments out = ReadEnchantments(*ench);
            if (!out.IsEmpty()) stack.components.set(DataComponents::STORED_ENCHANTMENTS, std::move(out));
        }
        if (auto ench = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                components->GetTag("minecraft:enchantments"))) {
            ItemEnchantments out = ReadEnchantments(*ench);
            if (!out.IsEmpty()) stack.components.set(DataComponents::ENCHANTMENTS, std::move(out));
        }

        // (Durability — MAX_DAMAGE / DAMAGE / UNBREAKABLE — is read by the
        // registered codecs below: components/StackNbt.cpp.)
        if (auto repairCost = IntComponent(*components, "minecraft:repair_cost")) {
            stack.components.set(DataComponents::REPAIR_COST, std::max(0, *repairCost));
        }
        if (auto rep = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                components->GetTag("minecraft:repairable"))) {
            Repairable repairable;
            repairable.items = ReadHolderSet(rep->GetTag("items").get());
            if (!repairable.items.empty()) stack.components.set(DataComponents::REPAIRABLE, std::move(repairable));
        }
        if (auto ench = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                components->GetTag("minecraft:enchantable"))) {
            if (auto value = IntComponent(*ench, "value"); value && *value > 0) {
                stack.components.set(DataComponents::ENCHANTABLE, *value);
            }
        }
        if (auto wpn = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                components->GetTag("minecraft:weapon"))) {
            Weapon weapon;
            weapon.itemDamagePerAttack = std::max(0, IntComponent(*wpn, "item_damage_per_attack").value_or(1));
            if (auto t = wpn->GetTag("disable_blocking_for_seconds")) {
                weapon.disableBlockingForSeconds = static_cast<float>(std::max(0.0, NumberOf(*t).value_or(0.0)));
            }
            stack.components.set(DataComponents::WEAPON, weapon);
        }
        if (auto sound = std::dynamic_pointer_cast<::World::NBTTagString>(
                components->GetTag("minecraft:break_sound"))) {
            stack.components.set(DataComponents::BREAK_SOUND, std::string(StripNamespace(sound->value)));
        }
        if (auto res = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                components->GetTag("minecraft:damage_resistant"))) {
            if (auto types = std::dynamic_pointer_cast<::World::NBTTagString>(res->GetTag("types"))) {
                stack.components.set(DataComponents::DAMAGE_RESISTANT, types->value);
            }
        }
        // The registered codecs (ComponentNbt.hpp). A rejected value drops
        // that component only; its reason goes to the caller (the item
        // argument prints it).
        {
            ComponentNbt::ReadContext ctx;
            std::string reason;
            ctx.error = &reason;
            for (const ComponentNbt::Codec& codec : ComponentNbt::All()) {
                if (!codec.read) continue;
                auto value = components->GetTag(std::string(kNamespace) + codec.type->name);
                if (!value) continue;
                reason.clear();
                if (!codec.read(*value, stack, ctx) && error && error->empty()) {
                    *error = "Malformed 'minecraft:" + codec.type->name + "' component: '" +
                             (reason.empty() ? std::string("invalid value") : reason) + "'";
                }
            }
        }
        // ItemStack.validateStrict — reported to a caller that asks (the item
        // argument: arguments.item.malformed); a saved stack is kept, its
        // count brought within its limit, rather than lost.
        if (error && error->empty()) {
            const std::string invalid = ValidateItemStack(stack);
            if (!invalid.empty()) *error = "Malformed item: '" + invalid + "'";
        }
        stack.count = std::min(stack.count, std::max(1, GetMaxStackSize(stack)));
        return stack;
    }

} // namespace Game::Anvil
