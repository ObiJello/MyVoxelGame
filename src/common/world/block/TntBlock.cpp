// File: src/common/world/block/TntBlock.cpp
#include "common/world/block/TntBlock.hpp"

#include "common/sound/SoundEvents.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/RedstoneSignal.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldPrimedTnt.hpp"

namespace Game {

    bool TntPrime(ILevelWrite& level, const glm::ivec3& pos, Entity* igniter) {
        // MC gates every prime path on the tnt_explodes gamerule. Note it
        // returns FALSE rather than throwing — TntUseItemOn reads that to tell
        // the player the block is disabled instead of silently eating a
        // flint-and-steel durability point.
        if (!SpawnPrimedTnt(level, pos, igniter)) return false;

        // MC TntBlock.prime: level.playSound(null, tnt.getX(), tnt.getY(),
        // tnt.getZ(), TNT_PRIMED, BLOCKS, 1.0F, 1.0F) — at the primed entity,
        // which spawns at the cell's bottom centre.
        level.PlaySound(nullptr, glm::dvec3(pos.x + 0.5, pos.y, pos.z + 0.5),
                        SoundEvents::TNT_PRIMED, SoundSource::Blocks, 1.0f, 1.0f);
        // MC: level.gameEvent(source, GameEvent.PRIME_FUSE, pos) — no game-event
        // system here; the site is named so a sculk sensor finds it later.
        return true;
    }

    void TntOnPlace(ILevelWrite& level, const glm::ivec3& pos,
                    BlockState newState, BlockState oldState, bool /*movedByPiston*/) {
        // MC TntBlock.onPlace: place a TNT block into a powered cell and it
        // lights immediately. Guarded on a BLOCK change exactly as vanilla
        // (`!oldState.is(state.getBlock())`) — onPlace also fires for a
        // state-only edit, and TNT has one (`unstable`).
        if (oldState.Block() == newState.Block()) return;
        if (!HasNeighborSignal(level, pos)) return;
        if (TntPrime(level, pos, nullptr)) {
            // MC removeBlock(pos, false) — flag 3, no drops.
            level.SetBlock(pos.x, pos.y, pos.z, BlockID::Air, World::UpdateFlags::All);
        }
    }

    UseResult TntUseItemOn(ItemStack& stack, ILevelWrite* level, const glm::ivec3& pos,
                           IUsePlayer* player, uint32_t hand,
                           const BlockHitResult& /*hit*/) {
        if (!level) return UseResult::Pass;

        const bool isFlintAndSteel = stack.itemId == Items::FlintAndSteel;
        const bool isFireCharge    = stack.itemId == Items::FireCharge;
        // MC falls through to super.useItemOn for anything else, which is Pass.
        if (!isFlintAndSteel && !isFireCharge) return UseResult::Pass;

        // Prediction runs this on the client too, and spawning has no
        // client-side equivalent — MC's own SpawnEggItem opens with the same
        // guard for the same reason. Report Success so the client's prediction
        // does not fall through and place a fire block on top.
        if (level->IsClientSide()) return UseResult::Success;

        Entity* igniter = nullptr;   // the player's entity view is not reachable here
        if (!TntPrime(*level, pos, igniter)) {
            // MC TntBlock.useItemOn:114-117 — with tnt_explodes off the player
            // is TOLD, on the action bar, and the click returns PASS so the
            // item's own useOn still runs and you get a fire block. Returning
            // Pass silently (which is what this did) left the player clicking
            // a TNT block with no feedback at all and no idea why.
            if (player) {
                player->DisplayClientMessage("TNT is disabled", /*actionBar=*/true);
            }
            return UseResult::Pass;
        }

        // MC setBlock(pos, AIR, 11) = UPDATE_NEIGHBORS | UPDATE_CLIENTS |
        // UPDATE_IMMEDIATE.
        level->SetBlock(pos.x, pos.y, pos.z, BlockID::Air, World::UpdateFlags::All);

        if (isFlintAndSteel) {
            // MC itemStack.hurtAndBreak(1, player, hand) — no wear in
            // creative (hasInfiniteMaterials), which HurtAndBreak checks.
            HurtAndBreak(stack, 1, level, player, hand);
        } else if (player && !player->isCreative()) {
            // MC itemStack.consume(1, player) — a fire charge is used up.
            if (stack.count > 0) --stack.count;
            if (stack.count <= 0) stack.Clear();
        }
        return UseResult::Success;
    }

    bool TntPlayerWillDestroy(ILevelWrite& level, const glm::ivec3& pos,
                              BlockState state, Entity* player, bool creative) {
        // MC TntBlock.playerWillDestroy: only an UNSTABLE TNT primes when
        // broken, and never in creative. `unstable` is a real blockstate here
        // (GeneratedBlockStates has tnt with one boolean property); nothing
        // sets it true yet, so this is inert but correct.
        if (creative) return false;
        if (state.GetValueByName("unstable") != "true") return false;
        return TntPrime(level, pos, player);
    }

    bool TntOnProjectileHit(ILevelWrite& level, const glm::ivec3& pos,
                            Entity* projectile, Entity* owner) {
        // MC TntBlock.onProjectileHit: only a BURNING projectile lights it.
        if (!projectile || !projectile->IsOnFire()) return false;
        return TntPrime(level, pos, owner ? owner : projectile);
    }

} // namespace Game
