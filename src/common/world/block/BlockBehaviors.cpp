// File: src/common/world/block/BlockBehaviors.cpp
//
// Per-block interaction callbacks — the block-side mirror of ItemBehaviors.cpp.
// BlockRegistry::Initialize calls BlockRegistry_RegisterBehaviors once the
// block table exists, and this file fills in the `useWithoutItem` /
// `useItemOn` function pointers for the handful of blocks that react to a
// right-click.
//
// Each entry corresponds to a BlockBehaviour subclass override in MC. Keeping
// them here rather than in BlockRegistry.cpp keeps the registry file about
// registration and this one about behaviour, and gives new interactive blocks
// (chest, furnace, doors) an obvious home.
#include "BlockRegistry.hpp"
#include "BlockInteraction.hpp"
#include "BlockPlacement.hpp"
#include "RedstoneWire.hpp"
#include "common/world/level/World.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/inventory/MenuType.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include "common/core/Log.hpp"

#include <array>
#include <string>

namespace Game {

    namespace {

        // MC CraftingTableBlock.useWithoutItem (CraftingTableBlock.java): open
        // the 3x3 menu for the player and consume the click.
        //
        // `player->OpenMenu` is a request, not the open itself: on the server
        // PlayerSession picks it up as soon as the use dispatch returns and
        // does the actual menu swap + packets; on the client — which runs this
        // same dispatch to predict — it is a no-op, and the screen appears when
        // the server's OpenScreenS2C lands. Either way returning Success here
        // is what stops the click falling through to block placement.
        UseResult CraftingTableUse(ILevelWrite* /*world*/, const glm::ivec3& pos,
                                   IUsePlayer* player, const BlockHitResult& /*hit*/) {
            if (!player) return UseResult::Pass;
            player->OpenMenu(MenuType::Crafting, pos);
            return UseResult::Success;
        }

        // MC RedStoneWireBlock.useWithoutItem: right-clicking a wire that is a
        // full CROSS collapses it to a dot, and one that is a bare DOT expands
        // it back to a cross. A wire with real connections is left alone — MC
        // returns PASS, which matters because it lets the click fall through
        // to placing whatever is in hand.
        //
        // Runs on both sides: the client predicts the toggle so the shape
        // changes on the same frame, and the server's authoritative state
        // follows. Both call the same RedstoneToggleShape against their own
        // block access, so they agree.
        UseResult RedstoneWireUse(ILevelWrite* world, const glm::ivec3& pos,
                                  IUsePlayer* player, const BlockHitResult& /*hit*/) {
            if (!world || !player) return UseResult::Pass;
            const uint8_t state = world->GetBlockState(pos.x, pos.y, pos.z);
            uint8_t next = 0;
            if (!RedstoneToggleShape(*world, pos, state, next)) return UseResult::Pass;
            // MC uses flag 3 (UPDATE_NEIGHBORS | UPDATE_CLIENTS) here, then
            // calls updatesOnShapeChange to poke the wires around it.
            world->SetBlock(pos.x, pos.y, pos.z, BlockID::RedstoneWire,
                            World::UpdateFlags::All, next);
            return UseResult::Success;
        }

        // MC FaceAttachedHorizontalDirectionalBlock.updateShape: a button or
        // lever whose supporting surface went away is destroyed.
        //
        //   return getConnectedDirection(state).getOpposite() == direction
        //          && !state.canSurvive(level, pos)
        //       ? Blocks.AIR.defaultBlockState() : state;
        //
        // i.e. it only reacts to a change on the side it is ATTACHED to —
        // mining the block behind a wall button drops it, mining the one beside
        // it does nothing.
        bool FaceAttachedNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
                                         BlockID id, uint8_t stateIndex,
                                         Direction toNeighbour, BlockID /*neighbourId*/,
                                         BlockID& outBlock, uint8_t& outState) {
            (void)outState;
            if (CanSurviveAt(level, pos, id, stateIndex)) return false;
            // Only the attachment side matters. CanSurviveAt already answered
            // false, so the support is gone whichever neighbour reported it;
            // checking the direction just avoids re-destroying on every one of
            // the six updates a single change fans out to.
            const auto& def = BlockRegistry::GetStateDefinition(id);
            const std::string_view face = def.ValueOf(stateIndex, "face");
            Direction connected = Direction::North;
            if (face == "floor")        connected = Direction::Up;
            else if (face == "ceiling") connected = Direction::Down;
            else {
                const std::string_view f = def.ValueOf(stateIndex, "facing");
                if      (f == "east")  connected = Direction::East;
                else if (f == "south") connected = Direction::South;
                else if (f == "west")  connected = Direction::West;
                else                   connected = Direction::North;
            }
            if (Opposite(connected) != toNeighbour) return false;

            outBlock = BlockID::Air;   // World turns this into a destroy-with-drops
            return true;
        }

        // MC RedStoneWireBlock.updateShape — a neighbour changed, so re-resolve
        // this wire's connections. Wired through the generic neighborChanged
        // hook, which is what makes two wires laid side by side join up.
        bool RedstoneWireNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
                                         BlockID /*id*/, uint8_t stateIndex,
                                         Direction toNeighbour, BlockID /*neighbourId*/,
                                         BlockID& outBlock, uint8_t& outState) {
            // DOWN is the support case: MC returns AIR when the block below can
            // no longer hold dust. The engine's support-collapse rule already
            // handles that (redstone has a modelled canSurvive), so leave it.
            if (toNeighbour == Direction::Down) return false;

            const uint8_t next = RedstoneUpdateShape(level, pos, stateIndex, toNeighbour);
            if (next == stateIndex) return false;
            outBlock = BlockID::RedstoneWire;
            outState = next;
            return true;
        }

        // Every container block does the same thing on a right-click: ask for
        // its menu. MC spreads this across ChestBlock.useWithoutItem,
        // BarrelBlock, DispenserBlock, HopperBlock, AbstractFurnaceBlock… each
        // calling player.openMenu(state.getMenuProvider(...)). The menu type is
        // the only thing that varies, so one template covers all of them.
        template <MenuType kType>
        UseResult OpenContainerUse(ILevelWrite* /*world*/, const glm::ivec3& pos,
                                   IUsePlayer* player, const BlockHitResult& /*hit*/) {
            if (!player) return UseResult::Pass;
            player->OpenMenu(kType, pos);
            return UseResult::Success;
        }

        // MC CampfireBlock.useItemOn (CampfireBlock.java:81-98): right-clicking
        // a campfire with something that has a campfire_cooking recipe lays one
        // of it on the fire.
        //
        // Deliberately NOT gated on `lit`. MC lets you load an unlit campfire —
        // the food just sits there until someone lights it, because the block
        // state is what picks the cooking ticker, not a flag on the item.
        //
        // The recipe test is the one branch that must run on BOTH sides: it
        // decides between "this click was food" and "this click was a block
        // placement", and only the client can answer that in time to predict.
        // Whether a slot is actually free needs the block entity, so that part
        // is deferred — and MC returns CONSUME for the full-campfire case
        // anyway, which is what we return uniformly. (MC's success path returns
        // SUCCESS_SERVER; the difference is the arm swing and a stat award,
        // neither of which exists here.)
        UseResult CampfireUseItemOn(ItemStack& stack, ILevelWrite* /*world*/,
                                    const glm::ivec3& pos, IUsePlayer* player,
                                    uint32_t hand, const BlockHitResult& /*hit*/) {
            if (!player) return UseResult::Pass;
            if (!RecipeManager::FindCooking(CookingKind::CampfireCooking, stack)) {
                // Not food — fall through to the empty-hand path exactly as MC
                // does, so a click holding a block still places it.
                return UseResult::TryEmptyHandInteraction;
            }
            player->PlaceCampfireFood(pos, hand);
            return UseResult::Consume;
        }

    } // namespace

    // Declared at file scope in BlockRegistry.cpp, same as
    // ItemRegistry_RegisterBehaviors is in Item.cpp.
    void BlockRegistry_RegisterBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        // Matched on registrySlug, not modelName: several blocks deliberately
        // SHARE a model name (Water→"water_still", every Infested* variant
        // borrows its host block's model), so a modelName scan can attach a
        // behaviour to the wrong block — it is the same trap that had
        // RecipeManager resolving "stone" to InfestedStone.
        auto forSlug = [&blocks](const char* slug) -> Block* {
            for (auto& block : blocks) {
                if (block.registrySlug == slug) return &block;
            }
            return nullptr;
        };

        // Attach one container menu to every block that opens it. A slug that
        // matches nothing is a silent "this block never opens", so say so —
        // that failure mode is invisible in play and looks identical to a
        // broken menu.
        int attached = 0, missing = 0;
        auto attachContainers = [&](std::initializer_list<const char*> slugs,
                                    BlockUseWithoutItemFn fn) {
            for (const char* slug : slugs) {
                if (Block* b = forSlug(slug)) { b->useWithoutItem = fn; ++attached; }
                else {
                    ++missing;
                    Log::Warning("[BlockBehaviors] no block with registrySlug '%s' — "
                                 "it will not open a menu", slug);
                }
            }
        };

        // ── Campfires ─────────────────────────────────────────────────────
        // useItemOn only: there is no empty-hand interaction on a campfire in
        // MC, so an empty hand correctly does nothing.
        for (const char* slug : {"campfire", "soul_campfire"}) {
            if (Block* b = forSlug(slug)) { b->useItemOn = &CampfireUseItemOn; ++attached; }
            else {
                ++missing;
                Log::Warning("[BlockBehaviors] no block with registrySlug '%s' — "
                             "food cannot be placed on it", slug);
            }
        }

        if (Block* craftingTable = forSlug("crafting_table")) {
            craftingTable->useWithoutItem = &CraftingTableUse;
            // MC's CraftingTableBlock has no useItemOn override, so a click
            // holding an item routes through TryEmptyHandInteraction — which
            // is exactly what the item dispatch already does when the held
            // item declines. Leaving useItemOn null means "sneak + item still
            // places the block", matching vanilla.
        }

        // ── Redstone dust ─────────────────────────────────────────────────
        // The right-click dot/cross toggle, plus the neighbour hook that makes
        // two wires laid next to each other join up. See RedstoneWire.cpp.
        if (Block* wire = forSlug("redstone_wire")) {
            wire->useWithoutItem  = &RedstoneWireUse;
            wire->neighborChanged = &RedstoneWireNeighborChanged;
        }

        // ── Buttons and levers ────────────────────────────────────────────
        // Break when the surface they are stuck to is mined. Matched by model
        // name rather than a slug list because there is one button per wood
        // type plus stone/polished blackstone — the same test CanSurviveAt and
        // the placement rule use.
        for (auto& b : blocks) {
            const std::string& n = b.modelName;
            if (n.find("_button") != std::string::npos || n == "lever") {
                b.neighborChanged = &FaceAttachedNeighborChanged;
            }
        }

        // ── Storage ───────────────────────────────────────────────────────
        // Chest, trapped chest, barrel and every shulker box are all 3-row
        // (MenuType.GENERIC_9x3 in MC). Ender chest shares the screen but in
        // vanilla is backed by the PLAYER's own ender inventory rather than the
        // block — until that exists it opens its own block container, which is
        // the same UI with per-block storage.
        attachContainers({"chest", "trapped_chest", "ender_chest", "barrel",
                          "shulker_box",
                          "white_shulker_box",      "orange_shulker_box",
                          "magenta_shulker_box",    "light_blue_shulker_box",
                          "yellow_shulker_box",     "lime_shulker_box",
                          "pink_shulker_box",       "gray_shulker_box",
                          "light_gray_shulker_box", "cyan_shulker_box",
                          "purple_shulker_box",     "blue_shulker_box",
                          "brown_shulker_box",      "green_shulker_box",
                          "red_shulker_box",        "black_shulker_box"},
                         &OpenContainerUse<MenuType::Generic9x3>);

        // Dispenser / dropper — 3x3 (MenuType.GENERIC_3x3).
        attachContainers({"dispenser", "dropper"}, &OpenContainerUse<MenuType::Generic3x3>);

        // Hopper — 5 slots in a row (MenuType.HOPPER).
        attachContainers({"hopper"}, &OpenContainerUse<MenuType::Hopper>);

        // Furnace family. Each gets its own menu type so the screen can pick
        // the right panel texture; the recipe kind comes off the block entity.
        attachContainers({"furnace"},       &OpenContainerUse<MenuType::Furnace>);
        attachContainers({"blast_furnace"}, &OpenContainerUse<MenuType::BlastFurnace>);
        attachContainers({"smoker"},        &OpenContainerUse<MenuType::Smoker>);

        // ── Utility blocks ────────────────────────────────────────────────
        // No block entity behind these: the menu owns its inputs and hands
        // them back when the screen closes (ItemCombinerMenu::Removed).
        attachContainers({"stonecutter"},       &OpenContainerUse<MenuType::Stonecutter>);
        attachContainers({"grindstone"},        &OpenContainerUse<MenuType::Grindstone>);
        attachContainers({"cartography_table"}, &OpenContainerUse<MenuType::CartographyTable>);
        attachContainers({"loom"},              &OpenContainerUse<MenuType::Loom>);
        attachContainers({"smithing_table"},    &OpenContainerUse<MenuType::Smithing>);
        // Every damage stage of an anvil opens the same menu (MC AnvilBlock
        // covers anvil / chipped_anvil / damaged_anvil).
        attachContainers({"anvil", "chipped_anvil", "damaged_anvil"},
                         &OpenContainerUse<MenuType::Anvil>);

        // ── Blocks with a gameplay system behind them ─────────────────────
        attachContainers({"enchanting_table"}, &OpenContainerUse<MenuType::Enchantment>);
        attachContainers({"brewing_stand"},    &OpenContainerUse<MenuType::BrewingStand>);
        attachContainers({"beacon"},           &OpenContainerUse<MenuType::Beacon>);
        attachContainers({"crafter"},          &OpenContainerUse<MenuType::Crafter3x3>);

        Log::Info("[BlockBehaviors] %d container blocks wired (%d slugs unmatched)",
                  attached, missing);
    }

} // namespace Game
