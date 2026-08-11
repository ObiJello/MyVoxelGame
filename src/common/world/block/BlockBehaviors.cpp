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
#include "common/entity/IUsePlayer.hpp"
#include "common/inventory/MenuType.hpp"

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

    } // namespace

    // Declared at file scope in BlockRegistry.cpp, same as
    // ItemRegistry_RegisterBehaviors is in Item.cpp.
    void BlockRegistry_RegisterBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        auto forSlug = [&blocks](const char* slug) -> Block* {
            for (auto& block : blocks) {
                if (block.modelName == slug) return &block;
            }
            return nullptr;
        };

        if (Block* craftingTable = forSlug("crafting_table")) {
            craftingTable->useWithoutItem = &CraftingTableUse;
            // MC's CraftingTableBlock has no useItemOn override, so a click
            // holding an item routes through TryEmptyHandInteraction — which
            // is exactly what the item dispatch already does when the held
            // item declines. Leaving useItemOn null means "sneak + item still
            // places the block", matching vanilla.
        }
    }

} // namespace Game
