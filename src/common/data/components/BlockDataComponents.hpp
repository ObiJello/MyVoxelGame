// File: src/common/data/components/BlockDataComponents.hpp
//
// MC 26.3 item components (block data): block_entity_data, block_state, bees,
// lock, container_loot, sign_text_front/back, waxed, cushion/color,
// note_block_sound, profile, debug_stick_state, base_color.
// Wire ids 340-369 (DataComponents.hpp's id table). The NBT codecs live in
// server/world/storage/anvil/components/BlockDataNbt.cpp, the tooltip
// providers register with ComponentTooltips (BlockDataComponents.cpp).
//
// The block-side halves — what a placed block does with them and what a
// broken / picked block hands back — are each block entity's
// ApplyItemComponents / CollectComponents (MC applyImplicitComponents /
// collectImplicitComponents), plus the two BlockItem steps every block item
// runs: BlockItem.updateBlockStateFromTag (ApplyBlockItemStateProperties)
// and BlockItem.updateCustomBlockEntityTag (BlockData::ApplyBlockEntityData).
#pragma once

#include "../DataComponents.hpp"
#include "../NbtCompoundValue.hpp"
#include "common/text/TextComponent.hpp"
#include "common/world/block/BlockState.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace Game {

    class World;
    class BlockEntity;
    class ILevelWrite;
    class IUsePlayer;
    struct UseOnContext;
    enum class UseResult : int;

    // MC TypedEntityData<BlockEntityType<?>> — the block entity type
    // ("minecraft:chest") and its NBT (the `id` key lives in `type`, not in
    // the tag, exactly as TypedEntityData keeps it apart).
    struct TypedBlockEntityData {
        std::string      type;   // namespaced block-entity type id
        NbtCompoundValue tag;    // everything but `id`
    };

    // MC BlockItemStateProperties — property name → serialised value, kept
    // sorted by name (a map: two orders are one value).
    struct BlockItemStateProperties {
        std::vector<std::pair<std::string, std::string>> properties;

        bool IsEmpty() const { return properties.empty(); }
        // The value set for `name`, or nothing.
        std::optional<std::string_view> Get(std::string_view name) const;
        // BlockItemStateProperties.with: sets `name` (kept sorted).
        BlockItemStateProperties With(std::string_view name, std::string_view value) const;
        // BlockItemStateProperties.apply: every listed property the block
        // declares, set to its value when the value is valid for it.
        BlockState Apply(BlockState state) const;
    };

    // MC BeehiveBlockEntity.Occupant — one bee resting in a hive: its entity
    // data (TypedEntityData<EntityType>, a bee), how long it has been inside
    // and how long it must stay.
    struct BeehiveOccupant {
        std::string      entityType = "minecraft:bee";
        NbtCompoundValue entityData;   // without `id`
        int32_t          ticksInHive = 0;
        int32_t          minTicksInHive = 0;
    };

    // MC Bees — a hive / nest item's occupants (at most 3 in play).
    struct Bees {
        std::vector<BeehiveOccupant> bees;
    };

    // MC LockCode — an ItemPredicate the opener's main-hand item must match.
    // Carried as its raw NBT (ItemPredicate.CODEC's compound), tested by
    // BlockData::LockUnlocksWith.
    struct LockCode {
        NbtCompoundValue predicate;
    };

    // MC SeededContainerLoot — the loot table a placed container rolls on
    // first open, and its seed (0 = the level's random).
    struct SeededContainerLoot {
        std::string lootTable;   // "minecraft:chests/simple_dungeon"
        int64_t     seed = 0;
    };

    // MC SignText as an item component (sign_text_front / sign_text_back):
    // four messages (text components), their filtered forms when they
    // differ, the dye colour and the glow.
    struct SignTextComponent {
        std::array<Text::Component, 4>                messages;
        std::optional<std::array<Text::Component, 4>> filteredMessages;
        int32_t                                       color = 15;   // DyeColor id, BLACK
        bool                                          glowing = false;

        // SignText.EMPTY: blank lines, black, no glow.
        bool IsEmpty() const;
    };

    // MC ResolvableProfile: a player head's owner — a name and/or UUID and
    // the profile's properties (the signed skin), plus 26.x's skin patch
    // (texture / cape / elytra resource textures and the model type).
    struct ResolvableProfile {
        struct Property {
            std::string                name;
            std::string                value;
            std::optional<std::string> signature;
        };
        std::optional<std::string>             name;
        std::optional<std::array<int32_t, 4>>  id;          // UUIDUtil int-array form
        std::vector<Property>                  properties;
        std::string                            texture;     // PlayerSkin.Patch.body ("" = none)
        std::string                            cape;
        std::string                            elytra;
        std::string                            model;       // "wide" / "slim" / ""

        // MC ResolvableProfile.create: a Dynamic profile (resolved later by
        // name or by id) when it has no properties and exactly one of name /
        // id; otherwise Static.
        bool IsDynamic() const { return properties.empty() && name.has_value() != id.has_value(); }
        // The UUID the profile stands for: its id, else the offline-player
        // UUID of its name (UUIDUtil.createOfflinePlayerUUID), else nil.
        std::array<int32_t, 4> EffectiveId() const;
    };

    // MC DebugStickState — per block (namespaced id), the property the debug
    // stick last selected on it.
    struct DebugStickState {
        std::vector<std::pair<std::string, std::string>> properties;   // block id → property name, sorted
        std::optional<std::string_view> Get(std::string_view block) const;
        DebugStickState With(std::string_view block, std::string_view property) const;
    };

    // MC BlockItem.updateBlockStateFromTag for a stack about to be placed:
    // `state` with the stack's BLOCK_STATE applied (unchanged without one).
    BlockState ApplyBlockItemStateProperties(const ItemStack& stack, BlockState state);

    namespace BlockData {

        // MC BlockEntityType.onlyOpCanSetNbt: the types whose data only a
        // game master (a creative operator) may place — spawners, trial
        // spawners, command / structure / jigsaw blocks, lecterns, vaults.
        bool OnlyOpCanSetNbt(std::string_view blockEntityType);

        // MC BlockItem.updateCustomBlockEntityTag → TypedEntityData.loadInto:
        // the stack's BLOCK_ENTITY_DATA merged over the block entity just
        // placed at `pos` (its own saved NBT, then the component's keys on
        // top — CompoundTag.merge), when the types match and the placer may
        // set it. SERVER only (BlockDataNbt.cpp: it runs the Anvil block-
        // entity codec). Returns whether the block entity changed.
        // MC BlockEntity.saveWithFullMetadata: the block entity at `pos` as
        // its Anvil compound (id, x, y, z and its data), or null when there
        // is none. SERVER only (BlockDataNbt.cpp).
        std::shared_ptr<::World::NBTTagCompound> SaveBlockEntityNbt(World& world, const glm::ivec3& pos);

        bool ApplyBlockEntityData(World& world, const glm::ivec3& pos, const ItemStack& stack,
                                  bool canUseGameMasterBlocks);

        // MC LockCode.unlocksWith: `stack` matches the lock's ItemPredicate
        // (items, count, components, predicates). An empty predicate matches
        // anything — LockCode.NO_LOCK. SERVER only (BlockDataNbt.cpp).
        bool LockUnlocksWith(const LockCode& lock, const ItemStack& stack);

        // The text a sign component line shows (the plain string the sign
        // block entity keeps).
        std::array<std::string, 4> SignLines(const SignTextComponent& text);
        // MC DebugStickItem.handleInteraction(player, state, level, pos,
        // update, stick): `update` (right click) cycles the block's selected
        // property (the stick's DEBUG_STICK_STATE entry, else its first),
        // otherwise (left click) selects the next property — backwards while
        // sneaking — with the action-bar message either way. Creative only
        // (canUseGameMasterBlocks). Server side; false when nothing happened.
        bool DebugStickInteract(ILevelWrite& level, IUsePlayer& player, ItemStack& stick,
                                const glm::ivec3& pos, bool update);
        // The debug stick's useOn (DebugStickItem.useOn → the update half).
        UseResult UseOnDebugStick(const UseOnContext& ctx, ItemStack& stack);
        // The sign block entity's text as the component (literal lines).
        SignTextComponent SignComponentFromLines(const std::array<std::string, 4>& lines, int32_t color, bool glowing);

    } // namespace BlockData

} // namespace Game

namespace Game::DataComponents {

    // MC DataComponents.BLOCK_ENTITY_DATA (340).
    extern const DataComponentType<TypedBlockEntityData> BLOCK_ENTITY_DATA;
    // MC DataComponents.BLOCK_STATE (341).
    extern const DataComponentType<BlockItemStateProperties> BLOCK_STATE;
    // MC DataComponents.BEES (342).
    extern const DataComponentType<Bees> BEES;
    // MC DataComponents.LOCK (343).
    extern const DataComponentType<LockCode> LOCK;
    // MC DataComponents.CONTAINER_LOOT (344).
    extern const DataComponentType<SeededContainerLoot> CONTAINER_LOOT;
    // MC DataComponents.SIGN_TEXT_FRONT / SIGN_TEXT_BACK (345 / 346) —
    // Item.Properties.signText() puts SignText.EMPTY on every sign item.
    extern const DataComponentType<SignTextComponent> SIGN_TEXT_FRONT;
    extern const DataComponentType<SignTextComponent> SIGN_TEXT_BACK;
    // MC DataComponents.WAXED (347) — a Unit; the bool carried is true.
    extern const DataComponentType<bool> WAXED;
    // MC DataComponents.CUSHION_COLOR ("cushion/color", 348) — a DyeColor id.
    extern const DataComponentType<int32_t> CUSHION_COLOR;
    // MC DataComponents.NOTE_BLOCK_SOUND (349) — a sound event id a head
    // placed on a note block plays.
    extern const DataComponentType<std::string> NOTE_BLOCK_SOUND;
    // MC DataComponents.PROFILE (350).
    extern const DataComponentType<ResolvableProfile> PROFILE;
    // MC DataComponents.DEBUG_STICK_STATE (351).
    extern const DataComponentType<DebugStickState> DEBUG_STICK_STATE;
    // MC DataComponents.BASE_COLOR (352) — a shield's banner base colour
    // (DyeColor id).
    extern const DataComponentType<int32_t> BASE_COLOR;

} // namespace Game::DataComponents
