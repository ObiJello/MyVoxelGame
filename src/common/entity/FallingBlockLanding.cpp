// File: src/common/entity/FallingBlockLanding.cpp
#include "common/entity/FallingBlockLanding.hpp"

#include "common/entity/Item.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/FallingBlock.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"

namespace Game {

    namespace {
        // MC spawnAtLocation(block) — the BLOCK ITEM, not the loot table. A
        // falling block that cannot land drops ITSELF, even for a block whose
        // loot table would normally give something else (gravel never flints
        // here, and suspicious sand never yields its buried treasure).
        //
        // At the ENTITY's exact position: popping it from the block cell
        // instead snapped the item to the centre of whatever cell the block
        // happened to be overlapping, a visible sideways jump.
        void DropAsItem(const glm::dvec3& position, BlockState state) {
            const BlockID id = state.Block();
            if (id == BlockID::Air) return;
            DropItemStackAt(position, ItemStack(id, 1));
        }
    }

    FallingBlockLandOutcome FallingBlockTryLand(ILevelWrite& level, const glm::ivec3& pos,
                                                BlockState state, const glm::dvec3& position,
                                                bool dropItem, bool cancelDrop,
                                                bool doEntityDrops) {
        // MC gates every drop on the entity_drops gamerule; the PLACE path is
        // not a drop and is never gated.
        const bool mayDrop = dropItem && doEntityDrops;

        const BlockState current = level.GetBlockState(pos.x, pos.y, pos.z);

        if (cancelDrop) {
            // MC: a cancelled drop never places and never drops — it just
            // announces the break. This is the anvil that shattered on impact.
            FallingBlockOnBrokenAfterFall(level, pos, state);
            return FallingBlockLandOutcome::Broken;
        }

        // MC's three questions, in order.
        //
        // 1. May I overwrite what is here? MC asks the real canBeReplaced
        //    against a DirectionalPlaceContext(level, pos, DOWN, ItemStack
        //    .EMPTY, UP) — "as if placed from above, holding nothing", which
        //    is what lets sand overwrite tall grass.
        //
        //    Reading the blanket `replaceable` flag instead skipped every
        //    state-dependent rule MC has here. Snow was the visible one: the
        //    flag says yes at any depth, so a falling block deleted a
        //    2-to-8-layer pile that vanilla will not let it touch.
        PlacementClick click;
        click.clickedFace = Direction::Up;
        click.hitY        = 0.0f;
        click.replacingClickedOnBlock = false;
        const bool mayReplace =
            current.Block() == BlockID::Air ||
            CanBeReplacedByPlacement(current, /*held=*/BlockID::Air,
                                     /*secondaryUse=*/false, click);

        // 2. Would I just keep falling? A block that lands on something also
        //    free has not landed at all. Concrete powder stuck in water is the
        //    exception — it stops even though water is "free".
        const glm::ivec3 below = pos + glm::ivec3(0, -1, 0);
        const bool isConcrete = IsConcretePowder(state.Block());
        const bool stuckInWater = isConcrete && BlockRegistry::ContainsWater(current);
        const bool wouldContinueFalling =
            FallingBlockIsFree(level.GetBlockState(below.x, below.y, below.z)) &&
            (!isConcrete || !stuckInWater);

        // 3. Can this block legally exist here at all (a torch needs a wall)?
        const bool wouldSurvive = CanSurviveAt(level, pos, state) && !wouldContinueFalling;

        if (mayReplace && wouldSurvive) {
            BlockState toPlace = state;
            // MC re-logs the block if it landed in water — a waterlogged slab
            // that fell and landed in a pool comes back waterlogged. The test
            // is `getFluidState(pos).is(Fluids.WATER)`, the SOURCE fluid, so a
            // slab landing in a WATERFALL does not come back waterlogged.
            if (BlockRegistry::IsWaterloggable(toPlace.Block()) &&
                BlockRegistry::IsWaterSource(current)) {
                toPlace = BlockRegistry::WithWaterlogged(toPlace, true);
            }

            // MC flag 3 — neighbours AND clients. The neighbour half is what
            // makes the sand ABOVE this one schedule its own fall, so the
            // cascade continues.
            if (level.SetBlock(pos.x, pos.y, pos.z, toPlace, World::UpdateFlags::All)) {
                FallingBlockOnLand(level, pos, toPlace, current);
                return FallingBlockLandOutcome::Placed;
            }

            // MC's refused-write path is INSIDE the drop guard: with dropItem
            // false (or entity_drops off) nothing happens at all — no discard,
            // no break — and the entity simply tries again next tick. Only the
            // dropping case gives up here.
            if (mayDrop) {
                FallingBlockOnBrokenAfterFall(level, pos, state);
                DropAsItem(position, state);
                return FallingBlockLandOutcome::Dropped;
            }
            return FallingBlockLandOutcome::Retry;
        }

        // Cannot live here. MC discards unconditionally, but announces the
        // break and drops only when it is going to drop.
        if (mayDrop) {
            FallingBlockOnBrokenAfterFall(level, pos, state);
            DropAsItem(position, state);
            return FallingBlockLandOutcome::Dropped;
        }
        return FallingBlockLandOutcome::Broken;
    }

} // namespace Game
