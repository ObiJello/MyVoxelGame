// File: src/common/entity/Item.hpp
// MC-style Item / ItemStack / ItemRegistry.
//
// Items unify "things in inventory slots". Following Minecraft's design:
//   - Every Block has a corresponding BlockItem (an Item that, when used, places the block).
//   - Pure items (Compass, Stick, etc.) are not tied to any block.
//   - An ItemStack holds an ItemID + count and is the contents of an inventory slot.
//   - Rendering a stack dispatches on the item's render type (Block → 3D isometric,
//     Sprite → flat 2D), exactly like MC's ItemRenderer dispatches on the BakedModel.
//
// Wire/numeric layout:
//   - ItemID is a uint32_t.
//   - Block items use IDs 1..(BlockID::Count - 1), matching their BlockID numerically.
//   - Pure items use IDs starting at PURE_ITEM_BASE (0x10000) — well clear of blocks.
//   - ItemID 0 is Air (the empty slot sentinel).
#pragma once

#include "../world/block/Blocks.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace Game {

    using ItemID = uint32_t;

    // Pure items live at and above this base so they never collide with block items.
    static constexpr ItemID PURE_ITEM_BASE = 0x10000;

    // Built-in pure item IDs (mirrors MC's net.minecraft.world.item.Items class).
    namespace Items {
        static constexpr ItemID Air      = 0;
        static constexpr ItemID Compass  = PURE_ITEM_BASE + 0;
        // Future pure items go here. Block items are accessed via FromBlock(BlockID).
    }

    enum class ItemRenderType : uint8_t {
        Block,   // 3D isometric mini-cube — uses BlockModel (MC: BakedModel.isGui3d())
        Sprite,  // Flat 2D texture — uses spriteName (MC: layered item sprite)
    };

    // Render-time context that the item's frame selector can read. Mirrors MC's
    // `ClientItem.Properties` and the per-property functions like CompassAngleState
    // that resolve "which model variant should render this frame" based on world state.
    struct ItemRenderContext {
        // Player position in world space (entity holding the item, OR the local viewer).
        float playerX = 0.0f, playerY = 0.0f, playerZ = 0.0f;
        // Player yaw in degrees (0 = facing +Z south in MC convention).
        float playerYaw = 0.0f;
        // Compass target (lodestone OR world spawn). Default: world spawn at origin.
        float compassTargetX = 0.0f, compassTargetZ = 0.0f;
        // Wall-clock time in seconds since session start — used for animation that ticks
        // on its own (the compass needle has overshoot/wobble behavior in MC).
        float timeSeconds = 0.0f;
    };

    // Per-item frame selector. Returns the index into `spriteFrames` to draw THIS frame.
    // Used for animated items like the compass (which picks one of 32 frames based on the
    // angle from the player to the target). Static items leave this null and use spriteName.
    // MC: ItemModelResolver / RangeSelectItemModel resolve this from item state at render
    // time — exactly the same dispatch shape, just data-driven instead of subclass-driven.
    using ItemFrameSelector = int (*)(const ItemRenderContext& ctx);

    struct Item {
        std::string                   name;            // human-readable display name
        ItemRenderType                renderType = ItemRenderType::Sprite;
        BlockID                       blockId    = BlockID::Air;  // iff renderType == Block
        std::string                   spriteName;                  // single static sprite
        std::vector<std::string>      spriteFrames;                // animated sprite frames
        ItemFrameSelector             selectFrame = nullptr;        // picks index into spriteFrames
        int                           maxStackSize = 64;
    };

    // Process-global item registry. Build-once, read-many.
    class ItemRegistry {
    public:
        // Populate the registry. Call AFTER BlockRegistry::Initialize so block items can
        // copy display names from blocks. Idempotent.
        static void Initialize();

        // Lookup by ID. Returns a sentinel "Air" item if id is unknown.
        static const Item& Get(ItemID id);

        // Conversions between BlockID and the BlockItem's ItemID.
        // (Block items deliberately share numeric IDs with BlockID for zero-cost mapping.)
        static constexpr ItemID FromBlock(BlockID b) { return static_cast<ItemID>(b); }
        static constexpr BlockID ToBlock(ItemID i) {
            // Caller is responsible for IsBlockItem() check; non-block items return Air.
            return (i < static_cast<ItemID>(BlockID::Count)) ? static_cast<BlockID>(i)
                                                              : BlockID::Air;
        }
        static bool IsBlockItem(ItemID id);
        static bool IsAir(ItemID id) { return id == Items::Air; }

        // For diagnostics / iteration.
        static size_t Size();

        // Process-global render context. Renderers call ItemRegistry::SetRenderContext()
        // once per frame; per-item frame selectors read from it. Mirrors MC's pattern of
        // putting render-state on the client and the item asking for it at render time.
        static void SetRenderContext(const ItemRenderContext& ctx);
        static const ItemRenderContext& GetRenderContext();

        // Drive any per-frame simulation for animated items (e.g. compass needle wobble).
        // Call once per render frame from the client with the elapsed seconds. Internally
        // ticks the compass simulation at MC's fixed 20 TPS based on the accumulated time.
        // Matches MC's CompassAngleState.wobble() ticking.
        static void TickAnimated(float dtSeconds);

    private:
        static void RegisterPureItem(ItemID id, const std::string& name,
                                     const std::string& spriteName, int maxStack = 64);
        static void RegisterAnimatedItem(ItemID id, const std::string& name,
                                         std::vector<std::string> frames,
                                         ItemFrameSelector selector,
                                         int maxStack = 64);
    };

    // Inventory slot contents. Replaces the old InventorySlot { BlockID, count } struct.
    // (Renamed type alias `InventorySlot` is provided in Inventory.hpp for migration.)
    struct ItemStack {
        ItemID itemId = Items::Air;
        int    count  = 0;

        ItemStack() = default;
        ItemStack(ItemID id, int c) : itemId(id), count(c) {}

        // Convenience constructor: build a stack from a BlockID (creates a BlockItem stack).
        // Lets old code like `InventorySlot{BlockID::Dirt, 64}` continue to compile.
        ItemStack(BlockID b, int c)
            : itemId(ItemRegistry::FromBlock(b)), count(c) {}

        bool IsEmpty() const { return count <= 0 || itemId == Items::Air; }
        void Clear() { itemId = Items::Air; count = 0; }

        // BlockID accessor for legacy callers that only deal with blocks. Returns Air for
        // pure items so existing block-only flows (place-block, etc.) treat them as nothing.
        BlockID AsBlockID() const {
            return ItemRegistry::IsBlockItem(itemId)
                ? ItemRegistry::ToBlock(itemId)
                : BlockID::Air;
        }
    };

} // namespace Game
