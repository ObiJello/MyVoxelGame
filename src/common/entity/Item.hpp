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
#include "../world/block/BlockInteraction.hpp"
#include "../data/DataComponentMap.hpp"
#include "../core/Features.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Game {
    class World;
}
namespace Server {
    class ServerPlayer;
}

namespace Game {

    using ItemID = uint32_t;

    // Pure items live at and above this base so they never collide with block items.
    static constexpr ItemID PURE_ITEM_BASE = 0x10000;

    // Built-in pure item IDs. The full vanilla MC item list lives in the
    // auto-generated GeneratedItemList.hpp (mirrors MC's Items.java declaration
    // order). The Air sentinel is defined here because it's special — ID 0 is
    // reserved for "empty slot" and isn't a registered item.
    namespace Items {
        static constexpr ItemID Air = 0;

#if ENABLE_PORTAL_GUN
        // Custom (non-MC) item — registered post-loop in
        // ItemRegistry::Initialize at PURE_ITEM_BASE + kPureItemTableSize.
        // Mutable (not constexpr) because the actual ID depends on
        // kPureItemTableSize which isn't a constant expression. Safe to
        // read after ItemRegistry::Initialize() runs.
        extern ItemID PortalGun;
#endif
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

    struct ItemStack;  // forward — defined below

    // Right-click ON a block. Mirrors MC's `Item.useOn(UseOnContext)`
    // (Item.java:187). Default behaviour (nullptr → Pass) lets the dispatch
    // fall through to BlockItem placement. Override per-item for things like
    // FlintAndSteel (spawn fire), BoneMeal (grow), Hoe (till), Bucket (fill),
    // Shovel (path), Shears (shear leaves).
    using ItemUseOnFn = UseResult (*)(const UseOnContext& ctx, ItemStack& stack);

    // Right-click in AIR. Mirrors MC's `Item.use(Level, Player, Hand)`
    // (Item.java:196-219). Default → Pass. Override for ranged items (Bow,
    // Snowball, EnderPearl, Egg) and self-targeting items.
    //
    // MC's BASE `Item.use` is itself non-trivial — it reads several
    // DataComponents and dispatches on them BEFORE returning Pass:
    //   1. CONSUMABLE      → consumable.startConsuming(player, stack, hand)
    //                        (food, potions; starts the eating/drinking timer)
    //   2. EQUIPPABLE     → if swappable, equippable.swapWithEquipmentSlot
    //                        (clicking a chestplate auto-equips it)
    //   3. BLOCKS_ATTACKS  → player.startUsingItem(hand) → CONSUME
    //                        (shield: start blocking)
    //   4. KINETIC_WEAPON  → player.startUsingItem(hand) + makeSound → CONSUME
    //                        (mace: start the wind-up swing)
    //   5. else            → PASS
    //
    // We don't yet have:
    //   • The UseItemC2SPacket that triggers this server-side (only
    //     UseItemOnC2SPacket exists today, which targets a block).
    //   • The CONSUMABLE/EQUIPPABLE/BLOCKS_ATTACKS/KINETIC_WEAPON DataComponent
    //     types registered (we only have STORED_ENCHANTMENTS and
    //     ENCHANTMENT_GLINT_OVERRIDE so far).
    //   • Player.startUsingItem (use-duration tick + finishUsingItem callback
    //     after N ticks, e.g. 32 for normal food).
    //
    // When those land, the default `Item.use` should be implemented in
    // Item.cpp (NOT as a per-item callback) and dispatch on the components
    // above. Per-item `use` callbacks override the default for items with
    // unique behaviour (BowItem.releaseUsing, EnderpearlItem.use to throw a
    // pearl entity, etc.).
    using ItemUseFn = UseResult (*)(World* world, Server::ServerPlayer* player,
                                    uint32_t hand, ItemStack& stack);

    struct Item {
        std::string                   name;            // human-readable display name
        ItemRenderType                renderType = ItemRenderType::Sprite;
        BlockID                       blockId    = BlockID::Air;  // iff renderType == Block
        std::string                   spriteName;                  // single static sprite (layer0 or non-layered)
        std::vector<std::string>      spriteLayers;                // all layerN textures (multi-layer items: leather armor, spawn egg, potion, …)
        std::vector<uint32_t>         layerTints;                  // ARGB tint per layer index (0 = untinted/white). From the items/{slug}.json `tints` array.
        std::vector<std::string>      spriteFrames;                // animated sprite frames
        ItemFrameSelector             selectFrame = nullptr;        // picks index into spriteFrames
        int                           maxStackSize = 64;
        // Predicate name from the JSON model's overrides[] (if any) — surfaced
        // by ItemModelLoader. Used by ItemRegistry::Initialize to wire up the
        // right ItemFrameSelector. "" means no animation.
        std::string                   predicateName;
        // Block-model override (set from `assets/items/{slug}.json`). When
        // non-empty AND renderType == Block, RenderBlockItemImpl uses this
        // model name instead of the block's own modelName. MC's data
        // generators register block items this way — e.g. trapdoors point at
        // `oak_trapdoor_bottom`, not the block's stateful root. Items.java
        // line 467 (`registerSimpleItemModel(trapdoor, bottom)`).
        std::string                   blockModelOverride;
        // BEWLR (BlockEntityWithoutLevelRenderer) hooks — set when the
        // items/{slug}.json uses {"type":"minecraft:special", "model":{"type":"chest","texture":"trapped"}}.
        // specialKind picks the C++ renderer (chest, shulker_box, …);
        // specialTexture is the variant (normal, trapped, ender, christmas, …).
        // Each renderer reads these from the Item to pick its assets.
        std::string                   specialKind;
        std::string                   specialTexture;
        // Default DataComponents for this item — mirrors MC's
        // `Item.Properties.component(...)` accumulation. ItemStack lookups
        // (ItemStack::get<T>) fall back to these when the stack itself doesn't
        // override the component. e.g. `enchanted_book` sets defaults
        // STORED_ENCHANTMENTS=EMPTY and ENCHANTMENT_GLINT_OVERRIDE=true here
        // (Items.java:2854), so every enchanted_book stack glints by default
        // even before a specific enchantment is stored on it.
        DataComponentMap              defaultComponents;
        // Per-item interaction callbacks. nullptr → defaults to UseResult::Pass
        // and the server's dispatch falls through to the next step (block
        // placement / empty-hand block use). Mirrors MC's per-Item-subclass
        // `useOn` and `use` overrides — see Item.java:187 (useOn) and
        // Item.java:196 (use). Wiring happens in ItemRegistry::Initialize for
        // items that need behaviour (FlintAndSteel, BoneMeal, Hoe, Bucket, etc).
        ItemUseOnFn                   useOn = nullptr;
        ItemUseFn                     use   = nullptr;
    };

    // Generated registration table — one entry per pure item, in MC's
    // Items.java declaration order. Lives in GeneratedItemList.cpp; consumed
    // by ItemRegistry::Initialize.
    struct PureItemTableEntry {
        const char* slug;           // matches assets/models/item/{slug}.json + textures/item/{slug}.png
        const char* predicateHint;  // "angle" | "time" | "pull" | ... | "none"
    };
    extern const PureItemTableEntry kPureItemTable[];
    extern const size_t             kPureItemTableSize;

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

        // Iterate every registered PURE item (id, item) — both vanilla MC
        // entries from kPureItemTable AND any custom items registered past
        // the table (e.g. Portal Gun). Use this from anywhere that needs
        // to enumerate the full item set rather than just the MC slice
        // (the inventory search tab is the canonical caller). Callback
        // signature: `void(ItemID, const Item&)`.
        //
        // Implemented as: get a const-ref to the backing map, iterate it.
        // Splitting accessor + inline template lets the header expose a
        // generic callback shape without leaking the anonymous-namespace
        // map into other TUs.
        static const std::unordered_map<ItemID, Item>& AllPureItems();

        template<typename F>
        static void ForEachPureItem(F&& callback) {
            for (const auto& [id, item] : AllPureItems()) {
                callback(id, item);
            }
        }

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
        ItemID            itemId = Items::Air;
        int               count  = 0;
        // Per-stack DataComponent overrides. Default-empty (vanilla stacks pay
        // zero allocations because the entry vector starts empty). Mirrors MC's
        // ItemStack `components` field; reads via `get<T>()` fall back to the
        // owning item's `defaultComponents`, matching MC's PatchedDataComponentMap.
        DataComponentMap  components;

        ItemStack() = default;
        ItemStack(ItemID id, int c) : itemId(id), count(c) {}

        // Convenience constructor: build a stack from a BlockID (creates a BlockItem stack).
        // Lets old code like `InventorySlot{BlockID::Dirt, 64}` continue to compile.
        ItemStack(BlockID b, int c)
            : itemId(ItemRegistry::FromBlock(b)), count(c) {}

        bool IsEmpty() const { return count <= 0 || itemId == Items::Air; }
        void Clear() { itemId = Items::Air; count = 0; components = {}; }

        // BlockID accessor for legacy callers that only deal with blocks. Returns Air for
        // pure items so existing block-only flows (place-block, etc.) treat them as nothing.
        BlockID AsBlockID() const {
            return ItemRegistry::IsBlockItem(itemId)
                ? ItemRegistry::ToBlock(itemId)
                : BlockID::Air;
        }

        // Read a DataComponent value, falling back to the owning item's defaults
        // when the stack itself doesn't override it. Mirrors MC's
        // `ItemStack.get(DataComponentType)` semantics — see PatchedDataComponentMap.
        template<typename T>
        std::optional<T> get(const DataComponentType<T>& key) const {
            if (auto v = components.get(key)) return v;
            return ItemRegistry::Get(itemId).defaultComponents.get(key);
        }

        // Mirrors MC `ItemStack.hasFoil()` (ItemStack.java:909–911): explicit
        // ENCHANTMENT_GLINT_OVERRIDE wins; otherwise the stack glints iff it
        // has any stored enchantments. Out-of-line so we don't pull
        // DataComponents.hpp (and the enchantment headers) into Item.hpp.
        bool HasFoil() const;
    };

} // namespace Game
