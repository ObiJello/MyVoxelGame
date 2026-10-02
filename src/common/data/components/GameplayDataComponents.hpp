// File: src/common/data/components/GameplayDataComponents.hpp
//
// MC 26.3 item components (data-driven gameplay): compostable, cooking_fuel,
// brewing_fuel, villager_food, additional_trade_cost, block_transformer,
// mob_visibility, intangible_projectile, can_place_on, can_break.
// Wire ids 400-429 (DataComponents.hpp's id table). The NBT codecs live in
// server/world/storage/anvil/components/GameplayDataNbt.cpp, the tooltip
// providers register with ComponentTooltips.
#pragma once

#include "../DataComponents.hpp"
#include "../NbtCompoundValue.hpp"
#include "common/entity/Item.hpp"
#include "common/world/block/BlockState.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Game {

    struct Block;
    class World;
    class Entity;
    class ILevelWrite;
    class IContainer;
    class JavaRandom;

    // ── ResolvableInt / ResolvableFloat ─────────────────────────────────
    //
    // MC's number-provider slots of the data-driven components: either a
    // constant or a reference into the context_int_provider /
    // context_float_provider registry ("minecraft:compostable/low"). The
    // vanilla registry entries (ContextIntProviders / ContextFloatProviders
    // bootstrap) are what ResolveInt / ResolveFloat evaluate; the codecs
    // refuse any other key, as MC's registry-backed codec does.
    struct ResolvableInt {
        std::optional<int> constant;
        std::string        reference;   // namespaced key, when not a constant

        static ResolvableInt Constant(int v) { ResolvableInt r; r.constant = v; return r; }
        static ResolvableInt Reference(std::string key);
        bool operator==(const ResolvableInt& o) const { return constant == o.constant && reference == o.reference; }
    };

    struct ResolvableFloat {
        std::optional<float> constant;
        std::string          reference;

        static ResolvableFloat Constant(float v) { ResolvableFloat r; r.constant = v; return r; }
        static ResolvableFloat Reference(std::string key);
        bool operator==(const ResolvableFloat& o) const { return constant == o.constant && reference == o.reference; }
    };

    // The loot context the providers read: the block the value is resolved
    // against (LootContextParams.BLOCK_STATE — the composter, the furnace)
    // and the level random.
    struct ProviderContext {
        BlockID     block      = BlockID::Air;
        int         blockLevel = -1;   // the composter's LEVEL, -1 = n/a
        JavaRandom* random     = nullptr;
    };

    bool IsKnownIntProvider(std::string_view key);
    bool IsKnownFloatProvider(std::string_view key);
    int   ResolveInt(const ResolvableInt& value, const ProviderContext& context, int defaultValue);
    float ResolveFloat(const ResolvableFloat& value, const ProviderContext& context, float defaultValue);

    // ── The components ─────────────────────────────────────────────────

    // MC Compostable (world/item/component/Compostable.java).
    struct Compostable {
        ResolvableInt layers;
        bool operator==(const Compostable& o) const { return layers == o.layers; }
    };

    // MC CookingFuel: burn ticks and the cooking speed while it burns.
    struct CookingFuel {
        ResolvableInt   burnTime;
        ResolvableFloat speedMultiplier = ResolvableFloat::Reference("minecraft:cooking/speed_default");
        bool operator==(const CookingFuel& o) const {
            return burnTime == o.burnTime && speedMultiplier == o.speedMultiplier;
        }
    };

    // MC BrewingFuel: the brewing stand's fuel uses and its brew speed.
    struct BrewingFuel {
        ResolvableInt   uses;
        ResolvableFloat speedMultiplier = ResolvableFloat::Reference("minecraft:brewing/speed_default");
        bool operator==(const BrewingFuel& o) const { return uses == o.uses && speedMultiplier == o.speedMultiplier; }
    };

    // MC VillagerFood: what the item is worth to a villager's food level.
    struct VillagerFood {
        int nutrition = 0;
        bool operator==(const VillagerFood& o) const { return nutrition == o.nutrition; }
    };

    // MC MobVisibility: a worn item that lowers how visible its wearer is
    // to the listed mob types (LivingEntity.getVisibilityPercent).
    struct MobVisibility {
        std::vector<std::string> targetingEntityTypes;   // HolderSet<EntityType> entries
        float                    visibility = 0.5f;
        bool operator==(const MobVisibility& o) const {
            return targetingEntityTypes == o.targetingEntityTypes && visibility == o.visibility;
        }
    };

    // MC BlockPredicate (advancements/predicates), the parts an adventure
    // predicate uses: blocks (HolderSet<Block>), state (StatePropertiesPredicate:
    // name -> exact value or {min, max}) and nbt (NbtPredicate over the block
    // entity's full save).
    struct BlockPredicateSpec {
        struct PropertyMatcher {
            std::string                name;
            std::optional<std::string> exact;
            std::optional<std::string> min;
            std::optional<std::string> max;
            bool operator==(const PropertyMatcher& o) const {
                return name == o.name && exact == o.exact && min == o.min && max == o.max;
            }
        };
        std::optional<std::vector<std::string>>     blocks;
        std::optional<std::vector<PropertyMatcher>> state;
        std::optional<NbtCompoundValue>             nbt;

        bool operator==(const BlockPredicateSpec& o) const {
            return blocks == o.blocks && state == o.state && nbt == o.nbt;
        }
        bool RequiresNbt() const { return nbt.has_value(); }
    };

    // MC Holder<BlockTransformer>: a registry key ("minecraft:axe" / hoe /
    // shovel — the engine's own behaviours), or a direct BlockTransformer —
    // its BlockTransformData list kept as NBT ({transforms: [...]}) and
    // interpreted at use (UseOnInlineBlockTransformer).
    struct BlockTransformerHolder {
        std::string      key;
        NbtCompoundValue direct;
        bool IsDirect() const { return key.empty() && !direct.IsEmpty(); }
        bool operator==(const BlockTransformerHolder& o) const { return key == o.key && direct == o.direct; }
    };

    // MC AdventureModePredicate: can_break / can_place_on.
    struct AdventureModePredicate {
        std::vector<BlockPredicateSpec> predicates;
        bool operator==(const AdventureModePredicate& o) const { return predicates == o.predicates; }
    };

    // MC BlockPredicate.matchesState / matches(BlockInWorld). `blockEntityNbt`
    // is the block entity's full save (saveWithFullMetadata) or null when
    // there is none (or the caller cannot read it — the client's prediction):
    // an nbt predicate then fails, as MC's does for a missing entity.
    bool BlockPredicateMatches(const BlockPredicateSpec& predicate, BlockState state,
                               const ::World::NBTTagCompound* blockEntityNbt);
    bool AdventurePredicateMatches(const AdventureModePredicate& predicate, BlockState state,
                                   const ::World::NBTTagCompound* blockEntityNbt);
    // Whether any predicate needs the block entity's NBT (the caller then
    // saves it before testing).
    bool AdventurePredicateNeedsNbt(const AdventureModePredicate& predicate);

    // MC NbtUtils.compareNbt(expected, actual, partialListMatches = true).
    bool CompareNbt(const ::World::NBTTag* expected, const ::World::NBTTag* actual, bool partialListMatches = true);

    // ItemStack.canBreakBlockInAdventureMode / canPlaceOnBlockInAdventureMode.
    bool CanBreakBlockInAdventureMode(const ItemStack& stack, BlockState state,
                                      const ::World::NBTTagCompound* blockEntityNbt);
    bool CanPlaceOnBlockInAdventureMode(const ItemStack& stack, BlockState state,
                                        const ::World::NBTTagCompound* blockEntityNbt);

    // The same against a live world position: the block entity's full save
    // is read only when a predicate asks for nbt. SERVER only
    // (GameplayDataNbt.cpp — it runs the Anvil block-entity codec). `state`
    // is the block as the player saw it (a break's packet state).
    bool CanBreakInAdventureAt(const ItemStack& mainHand, World& world, const glm::ivec3& pos, BlockState state);
    // MC Player.mayUseItemAt / ItemStack.useOn's gate: `clicked` is the
    // block the item is used on.
    bool CanPlaceOnInAdventureAt(const ItemStack& stack, World& world, const glm::ivec3& clicked);

    // ── Behaviour helpers ──────────────────────────────────────────────

    // AbstractFurnaceBlockEntity.getBurnDuration / getSpeedMultiplier for a
    // furnace of the given block (smoker and blast furnace are MC's
    // #fast_cooking LootPredicate).
    int   GetCookingFuelBurnTime(const ItemStack& fuel, BlockID furnace, JavaRandom* random);
    float GetCookingFuelSpeedMultiplier(const ItemStack& fuel, BlockID furnace, JavaRandom* random);
    // The multiplier every vanilla fuel gives in this furnace (the recipe
    // data's cooking times already carry it — see FurnaceBlockEntity).
    float DefaultCookingSpeedMultiplier(BlockID furnace);
    bool  IsCookingFuel(const ItemStack& stack);

    // VILLAGER_FOOD's nutrition, 0 without it.
    int VillagerFoodNutrition(const ItemStack& stack);

    // MOB_VISIBILITY of a worn stack against a targeting entity type: the
    // factor (1 when the component is absent or does not list the type).
    double MobVisibilityFactor(const ItemStack& worn, EntityTypeId targeting);

    // Item.useOn with BLOCK_TRANSFORMER: the item's own useOn, unless it is
    // a block transformer's (then the stack's BLOCK_TRANSFORMER decides —
    // removing the component takes the behaviour away, giving it to any
    // item adds it).
    ItemUseOnFn StackUseOn(const ItemStack& stack);
    // The engine's block transformers ("minecraft:axe" / "hoe" / "shovel"),
    // bound in ItemBehaviors.cpp.
    ItemUseOnFn BlockTransformerUseOn(std::string_view key);
    bool IsKnownBlockTransformer(std::string_view key);
    // BlockTransformer.transformBlock for a direct (inline) transformer on
    // the stack: each BlockTransformData in turn — a face it allows and a
    // block_state_provider that answers (simple / a block state,
    // copy_properties, rule_based over the worldgen block predicates,
    // weighted, random_block, rotated) transforms the block, with its sound,
    // particle, loot and the item's consumption or wear.
    UseResult UseOnInlineBlockTransformer(const UseOnContext& ctx, ItemStack& stack);
    // Validates a direct transformer's NBT (the codec's job): "" when it is
    // well formed, else the reason.
    std::string ValidateInlineBlockTransformer(const ::World::NBTTagCompound& transformer);

    // ── Composter (MC ComposterBlock) ──────────────────────────────────

    namespace Composter {
        // WorldlyContainerHolder.getContainer — the hopper's view.
        std::unique_ptr<IContainer> GetContainer(ILevelWrite& level, const glm::ivec3& pos, BlockState state);
        // ComposterBlock.insertItem (the villager farmer's deposit).
        BlockState InsertItem(Entity* source, BlockState state, ILevelWrite& level, ItemStack& stack,
                              const glm::ivec3& pos);
    }

    // Items.java's defaults (GeneratedGameplayDefaults.inc), the engine's
    // own fuels (the mod ports' woods) and the axes' / hoes' / shovels'
    // BLOCK_TRANSFORMER.
    void ItemRegistry_RegisterGameplayDefaults(std::vector<Item>& blockItems,
                                               std::unordered_map<ItemID, Item>& pureItems);

} // namespace Game

namespace Game::DataComponents {

    extern const DataComponentType<Compostable>            COMPOSTABLE;
    extern const DataComponentType<CookingFuel>            COOKING_FUEL;
    extern const DataComponentType<BrewingFuel>            BREWING_FUEL;
    extern const DataComponentType<VillagerFood>           VILLAGER_FOOD;
    extern const DataComponentType<int32_t>                ADDITIONAL_TRADE_COST;
    extern const DataComponentType<BlockTransformerHolder> BLOCK_TRANSFORMER;
    extern const DataComponentType<MobVisibility>          MOB_VISIBILITY;
    extern const DataComponentType<bool>                   INTANGIBLE_PROJECTILE;
    extern const DataComponentType<AdventureModePredicate> CAN_PLACE_ON;
    extern const DataComponentType<AdventureModePredicate> CAN_BREAK;

} // namespace Game::DataComponents
