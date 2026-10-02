// File: src/common/data/components/GameplayDataComponents.cpp
#include "GameplayDataComponents.hpp"

#include "ComponentTooltips.hpp"
#include "ToolComponents.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/text/Language.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/crafting/GeneratedRecipeList.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace Game {

    namespace {

        std::string_view StripMinecraft(std::string_view id) {
            return id.rfind("minecraft:", 0) == 0 ? id.substr(10) : id;
        }

        std::string WithMinecraft(std::string_view id) {
            if (id.find(':') != std::string_view::npos) return std::string(id);
            return "minecraft:" + std::string(id);
        }

        // ContextIntProviders.bootstrap: compostable(blocks, chance).
        struct CompostKey { std::string_view path; int chance; };
        constexpr CompostKey kCompostKeys[] = {
            {"compostable/low", 30}, {"compostable/low_medium", 50}, {"compostable/medium", 65},
            {"compostable/medium_high", 85}, {"compostable/always_add_one", 100},
        };

        // ContextIntProviders.bootstrap: cooking(predicates, normal, fast,
        // time) = time / (fast furnace ? 2 : 1).
        struct CookingKey { std::string_view path; int ticks; };
        constexpr CookingKey kCookingKeys[] = {
            {"cooking/time_bamboo", 50}, {"cooking/time_wool_slabs", 50}, {"cooking/time_wool_carpets", 67},
            {"cooking/time_dry_plants", 100}, {"cooking/time_wood_items_extra_small", 100},
            {"cooking/time_wool", 100}, {"cooking/time_wood_slabs", 150},
            {"cooking/time_wood_items_large", 200}, {"cooking/time_roots", 300},
            {"cooking/time_wood_blocks", 300}, {"cooking/time_wood_items_small", 300},
            {"cooking/time_hanging_signs", 800}, {"cooking/time_boats", 1200}, {"cooking/time_coal", 1600},
            {"cooking/time_blaze_rod", 2400}, {"cooking/time_dried_kelp_block", 4001},
            {"cooking/time_coal_block", 16000}, {"cooking/time_lava_bucket", 20000},
        };

        // LootPredicates.FAST_FURNACE: block/fast_cooking = smoker, blast furnace.
        bool IsFastFurnace(BlockID block) {
            return block == BlockID::Smoker || block == BlockID::BlastFurnace;
        }

        bool EntityMatchesHolderSet(EntityTypeId type, const std::vector<std::string>& entries) {
            if (type >= EntityTypeId::Count) return false;
            const std::string_view slug = GetEntityTypeInfo(type).slug;
            for (const std::string& entry : entries) {
                if (entry.empty()) continue;
                if (entry[0] == '#') {
                    if (DataTags::HasTag(DataTags::Registry::EntityType, slug, entry)) return true;
                    continue;
                }
                if (StripMinecraft(entry) == slug) return true;
            }
            return false;
        }

    } // namespace

    // ── Providers ──────────────────────────────────────────────────────────

    ResolvableInt ResolvableInt::Reference(std::string key) {
        ResolvableInt r;
        r.reference = WithMinecraft(key);
        return r;
    }

    ResolvableFloat ResolvableFloat::Reference(std::string key) {
        ResolvableFloat r;
        r.reference = WithMinecraft(key);
        return r;
    }

    bool IsKnownIntProvider(std::string_view key) {
        if (key.rfind("minecraft:", 0) != 0 && key.find(':') != std::string_view::npos) return false;
        const std::string_view path = StripMinecraft(key);
        for (const CompostKey& k : kCompostKeys) if (k.path == path) return true;
        for (const CookingKey& k : kCookingKeys) if (k.path == path) return true;
        return path == "cooking/normal_burn_time_reduction_factor" ||
               path == "cooking/fast_burn_time_reduction_factor" || path == "brewing/uses_default";
    }

    bool IsKnownFloatProvider(std::string_view key) {
        if (key.rfind("minecraft:", 0) != 0 && key.find(':') != std::string_view::npos) return false;
        const std::string_view path = StripMinecraft(key);
        return path == "cooking/speed_default" || path == "cooking/normal_speed_multiplier" ||
               path == "cooking/fast_speed_multiplier" || path == "brewing/speed_default";
    }

    int ResolveInt(const ResolvableInt& value, const ProviderContext& context, int defaultValue) {
        if (value.constant) return *value.constant;
        const std::string_view path = StripMinecraft(value.reference);
        for (const CompostKey& k : kCompostKeys) {
            if (k.path != path) continue;
            if (k.chance >= 100) return 1;
            // NumberDispatcher: the empty composter (LEVEL 0) always takes
            // the layer; otherwise the weighted list {1: chance, 0: 100 -
            // chance} — WeightedList.getRandom's nextInt(total weight).
            if (context.block == BlockID::Composter && context.blockLevel == 0) return 1;
            if (!context.random) return 0;
            return context.random->NextInt(100) < k.chance ? 1 : 0;
        }
        for (const CookingKey& k : kCookingKeys) {
            if (k.path == path) return k.ticks / (IsFastFurnace(context.block) ? 2 : 1);
        }
        if (path == "cooking/normal_burn_time_reduction_factor") return 1;
        if (path == "cooking/fast_burn_time_reduction_factor") return 2;
        if (path == "brewing/uses_default") return 20;
        return defaultValue;
    }

    float ResolveFloat(const ResolvableFloat& value, const ProviderContext& context, float defaultValue) {
        if (value.constant) return *value.constant;
        const std::string_view path = StripMinecraft(value.reference);
        if (path == "cooking/speed_default") return IsFastFurnace(context.block) ? 2.0f : 1.0f;
        if (path == "cooking/normal_speed_multiplier") return 1.0f;
        if (path == "cooking/fast_speed_multiplier") return 2.0f;
        if (path == "brewing/speed_default") return 1.0f;
        return defaultValue;
    }

    // ── Block predicates ──────────────────────────────────────────────────

    namespace {

        // A property of `block` by its serialized name.
        bool FindProperty(BlockID block, std::string_view name, PropertyId& out) {
            const uint16_t n = BlockStates::PropertyCount(block);
            for (uint16_t i = 0; i < n; ++i) {
                const PropertyId prop = BlockStates::PropertyAt(block, i);
                if (BlockStates::PropertyName(prop) == name) { out = prop; return true; }
            }
            return false;
        }

        // Property.getValue(name): the value's index, -1 when the property
        // has no such value.
        int ValueIndex(PropertyId prop, std::string_view value) {
            const uint16_t n = BlockStates::PropertyValueCount(prop);
            for (uint16_t i = 0; i < n; ++i) {
                if (BlockStates::PropertyValueName(prop, i) == value) return static_cast<int>(i);
            }
            return -1;
        }

        // Comparable order of two values of one property (RangedMatcher):
        // integers numerically, booleans false < true, enums by declaration.
        int CompareValues(PropertyId prop, int a, int b) {
            const std::string_view x = BlockStates::PropertyValueName(prop, static_cast<uint16_t>(a));
            const std::string_view y = BlockStates::PropertyValueName(prop, static_cast<uint16_t>(b));
            auto asInt = [](std::string_view s, long& out) {
                if (s.empty()) return false;
                char* end = nullptr;
                const std::string tmp(s);
                out = std::strtol(tmp.c_str(), &end, 10);
                return end && *end == '\0';
            };
            long xi = 0, yi = 0;
            if (asInt(x, xi) && asInt(y, yi)) return xi < yi ? -1 : (xi > yi ? 1 : 0);
            if ((x == "true" || x == "false") && (y == "true" || y == "false")) {
                const int xb = x == "true" ? 1 : 0, yb = y == "true" ? 1 : 0;
                return xb - yb;
            }
            return a < b ? -1 : (a > b ? 1 : 0);
        }

        bool PropertyMatches(const BlockPredicateSpec::PropertyMatcher& m, BlockState state) {
            PropertyId prop{};
            if (!FindProperty(state.Block(), m.name, prop)) return false;
            const int current = state.GetIndex(prop);
            if (m.exact) {
                const int want = ValueIndex(prop, *m.exact);
                return want >= 0 && want == current;
            }
            if (m.min) {
                const int lo = ValueIndex(prop, *m.min);
                if (lo < 0 || CompareValues(prop, current, lo) < 0) return false;
            }
            if (m.max) {
                const int hi = ValueIndex(prop, *m.max);
                if (hi < 0 || CompareValues(prop, current, hi) > 0) return false;
            }
            return true;
        }

        bool SameNbtString(const std::string& a, const std::string& b) { return a == b; }

    } // namespace

    bool CompareNbt(const ::World::NBTTag* expected, const ::World::NBTTag* actual, bool partialListMatches) {
        using T = ::World::NBTTagType;
        if (expected == actual || !expected) return true;
        if (!actual) return false;
        if (expected->type != actual->type) return false;
        switch (expected->type) {
            case T::TAG_Compound: {
                const auto& e = static_cast<const ::World::NBTTagCompound&>(*expected);
                const auto& a = static_cast<const ::World::NBTTagCompound&>(*actual);
                if (a.value.size() < e.value.size()) return false;
                for (const auto& [key, tag] : e.value) {
                    const auto it = a.value.find(key);
                    if (!CompareNbt(tag.get(), it == a.value.end() ? nullptr : it->second.get(), partialListMatches)) return false;
                }
                return true;
            }
            case T::TAG_List: {
                const auto& e = static_cast<const ::World::NBTTagList&>(*expected);
                const auto& a = static_cast<const ::World::NBTTagList&>(*actual);
                if (!partialListMatches) {
                    if (e.value.size() != a.value.size()) return false;
                    for (size_t i = 0; i < e.value.size(); ++i) {
                        if (!CompareNbt(e.value[i].get(), a.value[i].get(), false)) return false;
                    }
                    return true;
                }
                if (e.value.empty()) return a.value.empty();
                for (const auto& want : e.value) {
                    const bool found = std::any_of(a.value.begin(), a.value.end(), [&](const ::World::NBTTagPtr& have) {
                        return CompareNbt(want.get(), have.get(), true);
                    });
                    if (!found) return false;
                }
                return true;
            }
            case T::TAG_Byte:   return static_cast<const ::World::NBTTagByte&>(*expected).value == static_cast<const ::World::NBTTagByte&>(*actual).value;
            case T::TAG_Short:  return static_cast<const ::World::NBTTagShort&>(*expected).value == static_cast<const ::World::NBTTagShort&>(*actual).value;
            case T::TAG_Int:    return static_cast<const ::World::NBTTagInt&>(*expected).value == static_cast<const ::World::NBTTagInt&>(*actual).value;
            case T::TAG_Long:   return static_cast<const ::World::NBTTagLong&>(*expected).value == static_cast<const ::World::NBTTagLong&>(*actual).value;
            case T::TAG_Float: {
                const float x = static_cast<const ::World::NBTTagFloat&>(*expected).value;
                const float y = static_cast<const ::World::NBTTagFloat&>(*actual).value;
                uint32_t bx = 0, by = 0;
                std::memcpy(&bx, &x, sizeof bx);
                std::memcpy(&by, &y, sizeof by);
                return bx == by || (std::isnan(x) && std::isnan(y));
            }
            case T::TAG_Double: {
                const double x = static_cast<const ::World::NBTTagDouble&>(*expected).value;
                const double y = static_cast<const ::World::NBTTagDouble&>(*actual).value;
                uint64_t bx = 0, by = 0;
                std::memcpy(&bx, &x, sizeof bx);
                std::memcpy(&by, &y, sizeof by);
                return bx == by || (std::isnan(x) && std::isnan(y));
            }
            case T::TAG_String:
                return SameNbtString(static_cast<const ::World::NBTTagString&>(*expected).value,
                                     static_cast<const ::World::NBTTagString&>(*actual).value);
            case T::TAG_Byte_Array: return static_cast<const ::World::NBTTagByteArray&>(*expected).value == static_cast<const ::World::NBTTagByteArray&>(*actual).value;
            case T::TAG_Int_Array:  return static_cast<const ::World::NBTTagIntArray&>(*expected).value == static_cast<const ::World::NBTTagIntArray&>(*actual).value;
            case T::TAG_Long_Array: return static_cast<const ::World::NBTTagLongArray&>(*expected).value == static_cast<const ::World::NBTTagLongArray&>(*actual).value;
            case T::TAG_End:        return true;
        }
        return false;
    }

    bool BlockPredicateMatches(const BlockPredicateSpec& predicate, BlockState state,
                               const ::World::NBTTagCompound* blockEntityNbt) {
        // matchesState: blocks, then the state properties.
        if (predicate.blocks && !BlockMatchesHolderSet(BlockRegistry::Get(state.Block()), *predicate.blocks)) {
            return false;
        }
        if (predicate.state) {
            for (const auto& m : *predicate.state) {
                if (!PropertyMatches(m, state)) return false;
            }
        }
        // matchesBlockEntityData: an nbt predicate needs the entity's save.
        if (predicate.nbt) {
            if (!blockEntityNbt) return false;
            return CompareNbt(predicate.nbt->tag.get(), blockEntityNbt, true);
        }
        return true;
    }

    bool AdventurePredicateMatches(const AdventureModePredicate& predicate, BlockState state,
                                   const ::World::NBTTagCompound* blockEntityNbt) {
        for (const BlockPredicateSpec& p : predicate.predicates) {
            if (BlockPredicateMatches(p, state, blockEntityNbt)) return true;
        }
        return false;
    }

    bool AdventurePredicateNeedsNbt(const AdventureModePredicate& predicate) {
        return std::any_of(predicate.predicates.begin(), predicate.predicates.end(),
                           [](const BlockPredicateSpec& p) { return p.RequiresNbt(); });
    }

    bool CanBreakBlockInAdventureMode(const ItemStack& stack, BlockState state,
                                      const ::World::NBTTagCompound* blockEntityNbt) {
        const auto p = stack.get(DataComponents::CAN_BREAK);
        return p && AdventurePredicateMatches(*p, state, blockEntityNbt);
    }

    bool CanPlaceOnBlockInAdventureMode(const ItemStack& stack, BlockState state,
                                        const ::World::NBTTagCompound* blockEntityNbt) {
        const auto p = stack.get(DataComponents::CAN_PLACE_ON);
        return p && AdventurePredicateMatches(*p, state, blockEntityNbt);
    }

    // ── Behaviour helpers ──────────────────────────────────────────────────

    int GetCookingFuelBurnTime(const ItemStack& fuel, BlockID furnace, JavaRandom* random) {
        if (fuel.IsEmpty()) return 0;
        const auto c = fuel.get(DataComponents::COOKING_FUEL);
        if (!c) return 0;
        ProviderContext ctx;
        ctx.block = furnace;
        ctx.random = random;
        return ResolveInt(c->burnTime, ctx, 0);
    }

    float GetCookingFuelSpeedMultiplier(const ItemStack& fuel, BlockID furnace, JavaRandom* random) {
        if (fuel.IsEmpty()) return 1.0f;
        const auto c = fuel.get(DataComponents::COOKING_FUEL);
        if (!c) return 1.0f;
        ProviderContext ctx;
        ctx.block = furnace;
        ctx.random = random;
        return ResolveFloat(c->speedMultiplier, ctx, 1.0f);
    }

    float DefaultCookingSpeedMultiplier(BlockID furnace) {
        return IsFastFurnace(furnace) ? 2.0f : 1.0f;
    }

    bool IsCookingFuel(const ItemStack& stack) {
        return !stack.IsEmpty() && stack.has(DataComponents::COOKING_FUEL);
    }

    int VillagerFoodNutrition(const ItemStack& stack) {
        if (stack.IsEmpty()) return 0;
        const auto food = stack.get(DataComponents::VILLAGER_FOOD);
        return food ? food->nutrition : 0;
    }

    double MobVisibilityFactor(const ItemStack& worn, EntityTypeId targeting) {
        if (worn.IsEmpty()) return 1.0;
        const auto vis = worn.get(DataComponents::MOB_VISIBILITY);
        if (!vis || !EntityMatchesHolderSet(targeting, vis->targetingEntityTypes)) return 1.0;
        return static_cast<double>(vis->visibility);
    }

    ItemUseOnFn StackUseOn(const ItemStack& stack) {
        const Item& item = ItemRegistry::Get(stack.itemId);
        const auto transformer = stack.get(DataComponents::BLOCK_TRANSFORMER);
        const ItemUseOnFn fromComponent = !transformer ? nullptr
            : transformer->IsDirect() ? &UseOnInlineBlockTransformer
                                      : BlockTransformerUseOn(transformer->key);
        const bool itemIsTransformer = item.useOn &&
            (item.useOn == BlockTransformerUseOn("minecraft:axe") ||
             item.useOn == BlockTransformerUseOn("minecraft:hoe") ||
             item.useOn == BlockTransformerUseOn("minecraft:shovel"));
        if (itemIsTransformer) return fromComponent;
        if (item.useOn) return item.useOn;
        return fromComponent;
    }

    bool IsKnownBlockTransformer(std::string_view key) {
        const std::string_view path = StripMinecraft(key);
        if (key.find(':') != std::string_view::npos && key.rfind("minecraft:", 0) != 0) return false;
        return path == "axe" || path == "hoe" || path == "shovel";
    }

    // ── Defaults ───────────────────────────────────────────────────────────

    namespace {
        struct GameplayRow {
            const char* item;
            const char* compostable;
            const char* cookingFuel;
            const char* brewingFuel;
            int         villagerFood;
            const char* mobVisibility;
        };
        constexpr GameplayRow kGameplayRows[] = {
#define GAMEPLAY_ITEM(item, compost, fuel, brewing, food, visibility) {item, compost, fuel, brewing, food, visibility},
#include "GeneratedGameplayDefaults.inc"
#undef GAMEPLAY_ITEM
        };

        // The engine's own fuels (the mod ports' woods, from the recipe
        // generator's table) carry the vanilla provider of their burn time.
        const char* CookingKeyForTicks(int ticks) {
            switch (ticks) {
                case 100: return "cooking/time_dry_plants";
                case 150: return "cooking/time_wood_slabs";
                case 200: return "cooking/time_wood_items_large";
                case 300: return "cooking/time_wood_blocks";
                case 800: return "cooking/time_hanging_signs";
                case 1200: return "cooking/time_boats";
                case 1600: return "cooking/time_coal";
                default: return nullptr;
            }
        }

        std::vector<std::string> SplitList(std::string_view s) {
            std::vector<std::string> out;
            size_t start = 0;
            while (start <= s.size()) {
                const size_t comma = s.find(',', start);
                const std::string_view part = s.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
                if (!part.empty()) out.emplace_back(part);
                if (comma == std::string_view::npos) break;
                start = comma + 1;
            }
            return out;
        }
    } // namespace

    void ItemRegistry_RegisterGameplayDefaults(std::vector<Item>& blockItems,
                                               std::unordered_map<ItemID, Item>& pureItems) {
        // slug -> the registry's Item (block items by their block's slug).
        std::unordered_map<std::string_view, Item*> bySlug;
        bySlug.reserve(blockItems.size() + pureItems.size());
        for (size_t i = 1; i < blockItems.size() && i < static_cast<size_t>(BlockID::Count); ++i) {
            const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
            if (!slug.empty()) bySlug.emplace(slug, &blockItems[i]);
        }
        for (auto& [id, item] : pureItems) {
            const std::string_view slug = ItemRegistry::Slug(id);
            if (!slug.empty()) bySlug.emplace(slug, &item);
        }
        auto find = [&](std::string_view slug) -> Item* {
            auto it = bySlug.find(slug);
            return it == bySlug.end() ? nullptr : it->second;
        };

        for (const GameplayRow& row : kGameplayRows) {
            Item* item = find(row.item);
            if (!item) continue;
            if (*row.compostable) {
                item->defaultComponents.set(DataComponents::COMPOSTABLE,
                                            Compostable{ResolvableInt::Reference(row.compostable)});
            }
            if (*row.cookingFuel) {
                CookingFuel fuel;
                fuel.burnTime = ResolvableInt::Reference(row.cookingFuel);
                item->defaultComponents.set(DataComponents::COOKING_FUEL, fuel);
            }
            if (*row.brewingFuel) {
                BrewingFuel fuel;
                fuel.uses = ResolvableInt::Reference(row.brewingFuel);
                item->defaultComponents.set(DataComponents::BREWING_FUEL, fuel);
            }
            if (row.villagerFood > 0) {
                item->defaultComponents.set(DataComponents::VILLAGER_FOOD, VillagerFood{row.villagerFood});
            }
            if (*row.mobVisibility) {
                MobVisibility vis;
                vis.targetingEntityTypes = SplitList(row.mobVisibility);
                vis.visibility = 0.5f;
                item->defaultComponents.set(DataComponents::MOB_VISIBILITY, vis);
            }
        }

        // Fuels vanilla does not list: the engine's own woods.
        for (size_t i = 0; i < kFuelTableSize; ++i) {
            Item* item = find(kFuelTable[i].slug);
            if (!item || item->defaultComponents.get(DataComponents::COOKING_FUEL)) continue;
            CookingFuel fuel;
            if (const char* key = CookingKeyForTicks(kFuelTable[i].burnTicks)) {
                fuel.burnTime = ResolvableInt::Reference(key);
            } else {
                fuel.burnTime = ResolvableInt::Constant(kFuelTable[i].burnTicks);
            }
            item->defaultComponents.set(DataComponents::COOKING_FUEL, fuel);
        }
    }

} // namespace Game

// ── Tooltips ────────────────────────────────────────────────────────────────

namespace Game {

    namespace {

        using ComponentTooltips::Line;
        using ComponentTooltips::Slot;

        // ItemStack.INTANGIBLE_TOOLTIP.
        void IntangibleTooltip(const ItemStack& stack, const ComponentTooltips::Context&, std::vector<Line>& out) {
            if (!stack.get(DataComponents::INTANGIBLE_PROJECTILE)) return;
            out.push_back({Language::Get("item.intangible"), ComponentTooltips::kGray, std::nullopt});
        }

        std::string BlockName(BlockID id) {
            const std::string& slug = BlockRegistry::Get(id).registrySlug;
            const std::string key = "block.minecraft." + slug;
            std::string text = Language::GetOrDefault(key, "");
            if (!text.empty()) return text;
            return GetItemStackHoverName(ItemStack(ItemRegistry::FromBlock(id), 1));
        }

        // AdventureModePredicate.computeTooltip: every predicate with a
        // block set — their blocks, distinct, dark grey; any predicate
        // without one makes it the single grey "Unknown".
        void AdventureTooltip(const AdventureModePredicate& p, const char* header, std::vector<Line>& out) {
            out.push_back({"", ComponentTooltips::kWhite, std::nullopt});
            out.push_back({Language::Get(header), ComponentTooltips::kGray, std::nullopt});
            for (const BlockPredicateSpec& spec : p.predicates) {
                if (!spec.blocks) {
                    out.push_back({Language::Get("item.canUse.unknown"), ComponentTooltips::kGray, std::nullopt});
                    return;
                }
            }
            std::vector<BlockID> seen;
            for (const BlockPredicateSpec& spec : p.predicates) {
                for (const std::string& entry : *spec.blocks) {
                    if (entry.empty()) continue;
                    if (entry[0] == '#') {
                        // HolderSet.Named: the tag's blocks in registry order.
                        for (size_t i = 1; i < static_cast<size_t>(BlockID::Count); ++i) {
                            const BlockID id = static_cast<BlockID>(i);
                            const Block& block = BlockRegistry::Get(id);
                            if (block.registrySlug.empty() || !BlockInTag(block, entry)) continue;
                            if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
                            seen.push_back(id);
                        }
                        continue;
                    }
                    const BlockID id = BlockStates::FromSlug(StripMinecraft(entry)).Block();
                    if (id != BlockID::Air && std::find(seen.begin(), seen.end(), id) == seen.end()) seen.push_back(id);
                }
            }
            for (BlockID id : seen) out.push_back({BlockName(id), ComponentTooltips::kDarkGray, std::nullopt});
        }

        void CanBreakTooltip(const ItemStack& stack, const ComponentTooltips::Context&, std::vector<Line>& out) {
            if (const auto p = stack.get(DataComponents::CAN_BREAK)) AdventureTooltip(*p, "item.canBreak", out);
        }

        void CanPlaceOnTooltip(const ItemStack& stack, const ComponentTooltips::Context&, std::vector<Line>& out) {
            if (const auto p = stack.get(DataComponents::CAN_PLACE_ON)) AdventureTooltip(*p, "item.canPlace", out);
        }

        const ComponentTooltips::Registrar kIntangibleTooltip{Slot::IntangibleProjectile,
                                                              &DataComponents::INTANGIBLE_PROJECTILE, &IntangibleTooltip};
        const ComponentTooltips::Registrar kCanBreakTooltip{Slot::CanBreak, &DataComponents::CAN_BREAK, &CanBreakTooltip};
        const ComponentTooltips::Registrar kCanPlaceOnTooltip{Slot::CanPlaceOn, &DataComponents::CAN_PLACE_ON, &CanPlaceOnTooltip};

    } // namespace

} // namespace Game

// ── Wire codecs + registrations ─────────────────────────────────────────────

namespace Game::DataComponents {

    namespace {

        // ResolvableInt.STREAM_CODEC: either(INT constant, key reference).
        void WriteInt(Network::PacketBuffer& b, const ResolvableInt& v) {
            if (v.constant) { b.WriteByte(0); b.WriteInt(static_cast<uint32_t>(*v.constant)); }
            else            { b.WriteByte(1); b.WriteString(v.reference); }
        }
        ResolvableInt ReadResolvableInt(Network::PacketReader& r) {
            ResolvableInt v;
            if (r.ReadByte() == 0) v.constant = static_cast<int32_t>(r.ReadInt());
            else                   v.reference = r.ReadString();
            return v;
        }
        void WriteFloat(Network::PacketBuffer& b, const ResolvableFloat& v) {
            if (v.constant) { b.WriteByte(0); b.WriteFloat(*v.constant); }
            else            { b.WriteByte(1); b.WriteString(v.reference); }
        }
        ResolvableFloat ReadResolvableFloat(Network::PacketReader& r) {
            ResolvableFloat v;
            if (r.ReadByte() == 0) v.constant = r.ReadFloat();
            else                   v.reference = r.ReadString();
            return v;
        }

        void SerCompostable(Network::PacketBuffer& b, const Compostable& v) { WriteInt(b, v.layers); }
        Compostable DeCompostable(Network::PacketReader& r) { return Compostable{ReadResolvableInt(r)}; }

        void SerCookingFuel(Network::PacketBuffer& b, const CookingFuel& v) {
            WriteInt(b, v.burnTime);
            WriteFloat(b, v.speedMultiplier);
        }
        CookingFuel DeCookingFuel(Network::PacketReader& r) {
            CookingFuel v;
            v.burnTime = ReadResolvableInt(r);
            v.speedMultiplier = ReadResolvableFloat(r);
            return v;
        }

        void SerBrewingFuel(Network::PacketBuffer& b, const BrewingFuel& v) {
            WriteInt(b, v.uses);
            WriteFloat(b, v.speedMultiplier);
        }
        BrewingFuel DeBrewingFuel(Network::PacketReader& r) {
            BrewingFuel v;
            v.uses = ReadResolvableInt(r);
            v.speedMultiplier = ReadResolvableFloat(r);
            return v;
        }

        void SerVillagerFood(Network::PacketBuffer& b, const VillagerFood& v) { b.WriteVarInt(static_cast<uint32_t>(v.nutrition)); }
        VillagerFood DeVillagerFood(Network::PacketReader& r) { return VillagerFood{static_cast<int>(r.ReadVarInt())}; }

        void SerVarInt(Network::PacketBuffer& b, const int32_t& v) { b.WriteVarInt(static_cast<uint32_t>(v)); }
        int32_t DeVarInt(Network::PacketReader& r) { return static_cast<int32_t>(r.ReadVarInt()); }

        // Holder<BlockTransformer>: the key, or "" and the direct value.
        void SerTransformer(Network::PacketBuffer& b, const BlockTransformerHolder& v) {
            b.WriteString(v.key);
            if (v.key.empty()) SerNbtCompoundValue(b, v.direct);
        }
        BlockTransformerHolder DeTransformer(Network::PacketReader& r) {
            BlockTransformerHolder v;
            v.key = r.ReadString();
            if (v.key.empty()) v.direct = DeNbtCompoundValue(r);
            return v;
        }

        void SerStrings(Network::PacketBuffer& b, const std::vector<std::string>& v) {
            b.WriteVarInt(static_cast<uint32_t>(v.size()));
            for (const std::string& s : v) b.WriteString(s);
        }
        std::vector<std::string> DeStrings(Network::PacketReader& r) {
            const uint32_t n = r.ReadVarInt();
            if (n > 4096) throw std::runtime_error("holder set too large");
            std::vector<std::string> v;
            v.reserve(n);
            for (uint32_t i = 0; i < n; ++i) v.push_back(r.ReadString());
            return v;
        }

        void SerMobVisibility(Network::PacketBuffer& b, const MobVisibility& v) {
            SerStrings(b, v.targetingEntityTypes);
            b.WriteFloat(v.visibility);
        }
        MobVisibility DeMobVisibility(Network::PacketReader& r) {
            MobVisibility v;
            v.targetingEntityTypes = DeStrings(r);
            v.visibility = r.ReadFloat();
            return v;
        }

        void SerUnit(Network::PacketBuffer&, const bool&) {}
        bool DeUnit(Network::PacketReader&) { return true; }

        void WriteOptString(Network::PacketBuffer& b, const std::optional<std::string>& s) {
            b.WriteByte(s ? 1 : 0);
            if (s) b.WriteString(*s);
        }
        std::optional<std::string> ReadOptString(Network::PacketReader& r) {
            if (r.ReadByte() == 0) return std::nullopt;
            return r.ReadString();
        }

        // AdventureModePredicate.STREAM_CODEC: the list of BlockPredicates,
        // each its optional blocks, state and nbt.
        void SerAdventure(Network::PacketBuffer& b, const AdventureModePredicate& v) {
            b.WriteVarInt(static_cast<uint32_t>(v.predicates.size()));
            for (const BlockPredicateSpec& p : v.predicates) {
                b.WriteByte(static_cast<uint8_t>((p.blocks ? 1 : 0) | (p.state ? 2 : 0) | (p.nbt ? 4 : 0)));
                if (p.blocks) SerStrings(b, *p.blocks);
                if (p.state) {
                    b.WriteVarInt(static_cast<uint32_t>(p.state->size()));
                    for (const auto& m : *p.state) {
                        b.WriteString(m.name);
                        WriteOptString(b, m.exact);
                        WriteOptString(b, m.min);
                        WriteOptString(b, m.max);
                    }
                }
                if (p.nbt) SerNbtCompoundValue(b, *p.nbt);
            }
        }
        AdventureModePredicate DeAdventure(Network::PacketReader& r) {
            AdventureModePredicate v;
            const uint32_t n = r.ReadVarInt();
            if (n > 256) throw std::runtime_error("adventure predicate: too many predicates");
            for (uint32_t i = 0; i < n; ++i) {
                BlockPredicateSpec p;
                const uint8_t flags = r.ReadByte();
                if (flags & 1) p.blocks = DeStrings(r);
                if (flags & 2) {
                    const uint32_t count = r.ReadVarInt();
                    if (count > 256) throw std::runtime_error("adventure predicate: too many properties");
                    std::vector<BlockPredicateSpec::PropertyMatcher> state;
                    for (uint32_t k = 0; k < count; ++k) {
                        BlockPredicateSpec::PropertyMatcher m;
                        m.name = r.ReadString();
                        m.exact = ReadOptString(r);
                        m.min = ReadOptString(r);
                        m.max = ReadOptString(r);
                        state.push_back(std::move(m));
                    }
                    p.state = std::move(state);
                }
                if (flags & 4) p.nbt = DeNbtCompoundValue(r);
                v.predicates.push_back(std::move(p));
            }
            return v;
        }

    } // namespace

    const DataComponentType<Compostable>            COMPOSTABLE{"compostable", 400, &SerCompostable, &DeCompostable};
    const DataComponentType<CookingFuel>            COOKING_FUEL{"cooking_fuel", 401, &SerCookingFuel, &DeCookingFuel};
    const DataComponentType<BrewingFuel>            BREWING_FUEL{"brewing_fuel", 402, &SerBrewingFuel, &DeBrewingFuel};
    const DataComponentType<VillagerFood>           VILLAGER_FOOD{"villager_food", 403, &SerVillagerFood, &DeVillagerFood};
    const DataComponentType<int32_t>                ADDITIONAL_TRADE_COST{"additional_trade_cost", 404, &SerVarInt, &DeVarInt};
    const DataComponentType<BlockTransformerHolder> BLOCK_TRANSFORMER{"block_transformer", 405, &SerTransformer, &DeTransformer};
    const DataComponentType<MobVisibility>          MOB_VISIBILITY{"mob_visibility", 406, &SerMobVisibility, &DeMobVisibility};
    const DataComponentType<bool>                   INTANGIBLE_PROJECTILE{"intangible_projectile", 407, &SerUnit, &DeUnit};
    const DataComponentType<AdventureModePredicate> CAN_PLACE_ON{"can_place_on", 408, &SerAdventure, &DeAdventure};
    const DataComponentType<AdventureModePredicate> CAN_BREAK{"can_break", 409, &SerAdventure, &DeAdventure};

} // namespace Game::DataComponents
