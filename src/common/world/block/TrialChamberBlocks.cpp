// File: src/common/world/block/TrialChamberBlocks.cpp
//
// MC VaultBlock.useItemOn. See the header.
#include "common/world/block/TrialChamberBlocks.hpp"

#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/block/entity/VaultBlockEntity.hpp"
#include "common/world/level/ILevelWrite.hpp"

namespace Game {

    namespace {

        UseResult VaultUseItemOn(ItemStack& stack, ILevelWrite* level, const glm::ivec3& pos,
                                 IUsePlayer* player, uint32_t /*hand*/, const BlockHitResult& /*hit*/) {
            if (!level) return UseResult::Pass;
            const BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);
            if (stack.IsEmpty() || VaultStates::Of(state) != VaultState::Active) {
                return UseResult::TryEmptyHandInteraction;
            }
            if (!level->IsClientSide()) {
                // serverLevel.getBlockEntity(pos) instanceof VaultBlockEntity,
                // else TRY_WITH_EMPTY_HAND.
                BlockEntity* be = level->GetBlockEntity(pos);
                if (!be && state.Block() == BlockID::Vault) {
                    // A vault whose entity never arrived (a hand-edited
                    // chunk): MC always has one, so make it.
                    if (const BlockEntityType* type = BlockEntityTypes::ForBlock(BlockID::Vault)) {
                        level->SetBlockEntity(pos, type->Create(pos, BlockID::Vault));
                        be = level->GetBlockEntity(pos);
                    }
                }
                auto* vault = dynamic_cast<VaultBlockEntity*>(be);
                if (!vault) return UseResult::TryEmptyHandInteraction;
                if (!vault->GetLevel()) vault->SetLevel(level);
                if (player) vault->TryInsertKey(*level, *player, stack);
            }
            return UseResult::SuccessServer;
        }

    } // namespace

    void RegisterTrialChamberBlockBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        blocks[static_cast<size_t>(BlockID::Vault)].useItemOn = &VaultUseItemOn;
    }

} // namespace Game
