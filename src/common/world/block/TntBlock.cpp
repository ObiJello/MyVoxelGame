// File: src/common/world/block/TntBlock.cpp
#include "common/world/block/TntBlock.hpp"

#include "common/core/SoundEvents.hpp"
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

        // MC: level.playSound(null, x, y, z, TNT_PRIMED, BLOCKS, 1.0F, 1.0F).
        PlaySound("entity.tnt.primed", pos);
        // MC: level.gameEvent(source, GameEvent.PRIME_FUSE, pos) — no game-event
        // system here; the site is named so a sculk sensor finds it later.
        return true;
    }

    void TntOnPlace(ILevelWrite& level, const glm::ivec3& pos,
                    BlockState /*newState*/, BlockState /*oldState*/) {
        // MC TntBlock.onPlace: place a TNT block into a powered cell and it
        // lights immediately. Dead until redstone exists — see
        // RedstoneSignal.hpp, which is the single seam.
        if (!HasNeighborSignal(level, pos)) return;
        if (TntPrime(level, pos, nullptr)) {
            // MC removeBlock(pos, false) — flag 3, no drops.
            level.SetBlock(pos.x, pos.y, pos.z, BlockID::Air, World::UpdateFlags::All);
        }
    }

    bool TntNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
                            BlockState /*state*/,
                            Direction /*toNeighbour*/, BlockID /*neighbourId*/,
                            BlockState& outState,
                            ScheduledTickAccess* /*ticks*/) {
        // Same check as onPlace, for a lever thrown next to TNT that is already
        // placed. Also dead until redstone exists.
        //
        // Returning a transform to AIR (rather than writing the block here) is
        // how a neighborChanged hook removes its own block — but priming has to
        // happen FIRST, and the const IBlockAccess this hook is handed cannot
        // spawn an entity. So the whole path waits on redstone anyway, and the
        // call below documents where it goes.
        if (!HasNeighborSignal(level, pos)) return false;
        // Unreachable today. When redstone lands, this needs the writable level
        // that MC's neighborChanged has and this hook does not — the likely
        // shape is a scheduled tick that primes, which is also how MC's
        // repeater and observer will want to work.
        outState = BlockStates::Default(BlockID::Air);
        return false;
    }

    UseResult TntUseItemOn(ItemStack& stack, ILevelWrite* level, const glm::ivec3& pos,
                           IUsePlayer* player, uint32_t /*hand*/,
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
            // MC hurtAndBreak(1). Durability is not modelled yet; the call site
            // is what matters (see ItemBehaviors' HurtAndBreak note).
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
