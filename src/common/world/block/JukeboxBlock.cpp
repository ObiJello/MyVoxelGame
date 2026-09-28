// File: src/common/world/block/JukeboxBlock.cpp
//
// MC JukeboxBlock and JukeboxPlayable.tryInsertIntoJukebox, method by method.
#include "common/world/block/JukeboxBlock.hpp"

#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/JukeboxSongs.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/block/entity/JukeboxBlockEntity.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

namespace Game {

    namespace {

        bool HasRecordOf(BlockState state) { return BoolOf(state, PropertyId::HAS_RECORD); }

        // The jukebox's block entity. On the server one is created for a
        // jukebox that has none — a chunk saved before jukeboxes had block
        // entities — as MC always has one; the client only reads the copy the
        // server sent.
        JukeboxBlockEntity* JukeboxAt(ILevelWrite& level, const glm::ivec3& pos) {
            BlockEntity* be = level.GetBlockEntity(pos);
            if (!be && !level.IsClientSide()) {
                if (const BlockEntityType* type = BlockEntityTypes::ForBlock(BlockID::Jukebox)) {
                    level.SetBlockEntity(pos, type->Create(pos, BlockID::Jukebox));
                    be = level.GetBlockEntity(pos);
                }
            }
            auto* jukebox = dynamic_cast<JukeboxBlockEntity*>(be);
            if (jukebox && !jukebox->GetLevel() && !level.IsClientSide()) jukebox->SetLevel(&level);
            return jukebox;
        }

        // The read-only lookup the signal hooks use: the level they are
        // handed is the writable one behind an IBlockAccess (the comparator
        // reaches its block entity the same way); nothing is written.
        const JukeboxBlockEntity* JukeboxAtReadOnly(const IBlockAccess& access, const glm::ivec3& pos) {
            auto* level = dynamic_cast<ILevelWrite*>(const_cast<IBlockAccess*>(&access));
            if (!level) return nullptr;
            return dynamic_cast<const JukeboxBlockEntity*>(level->GetBlockEntity(pos));
        }

        // MC JukeboxPlayable.tryInsertIntoJukebox.
        UseResult TryInsertIntoJukebox(ILevelWrite& level, const glm::ivec3& pos, ItemStack& toInsert,
                                       IUsePlayer* player) {
            if (!JukeboxSongs::IsJukeboxPlayable(toInsert)) return UseResult::TryEmptyHandInteraction;
            const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);
            if (state.Block() != BlockID::Jukebox || HasRecordOf(state)) {
                return UseResult::TryEmptyHandInteraction;
            }
            if (!level.IsClientSide()) {
                // toInsert.consumeAndReturn(1, player): one disc off the
                // stack — a copy, leaving the stack whole, for a player with
                // infinite materials (creative).
                ItemStack inserted = toInsert;
                inserted.count = 1;
                if (!player || !player->isCreative()) {
                    toInsert.count -= 1;
                    if (toInsert.count <= 0) toInsert.Clear();
                }
                if (JukeboxBlockEntity* jukebox = JukeboxAt(level, pos)) {
                    jukebox->SetTheItem(inserted);
                    // JukeboxPlayable.tryInsertIntoJukebox — gameEvent(BLOCK_CHANGE,
                    // pos, Context.of(player, state)) (the state before insertion).
                    level.GameEvent(GameEventId::BlockChange, pos,
                                    GameEventContext::Of(player ? player->GameEventSource() : nullptr, state));
                }
                // player.awardStat(Stats.PLAY_RECORD): no statistics yet.
            }
            return UseResult::Success;
        }

        // MC JukeboxBlock.useItemOn.
        UseResult JukeboxUseItemOn(ItemStack& stack, ILevelWrite* level, const glm::ivec3& pos,
                                   IUsePlayer* player, uint32_t /*hand*/, const BlockHitResult& /*hit*/) {
            if (!level) return UseResult::Pass;
            const BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);
            if (HasRecordOf(state)) return UseResult::TryEmptyHandInteraction;
            const UseResult result = TryInsertIntoJukebox(*level, pos, stack, player);
            return ConsumesAction(result) ? result : UseResult::TryEmptyHandInteraction;
        }

        // MC JukeboxBlock.useWithoutItem: a jukebox with a record pops it.
        UseResult JukeboxUseWithoutItem(ILevelWrite* level, const glm::ivec3& pos,
                                        IUsePlayer* /*player*/, const BlockHitResult& /*hit*/) {
            if (!level) return UseResult::Pass;
            const BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);
            if (!HasRecordOf(state)) return UseResult::Pass;
            if (level->IsClientSide()) {
                // The client's copy of the block entity carries no disc (MC
                // sends none either); the prediction only needs to know the
                // server will pop it — `instanceof JukeboxBlockEntity`.
                return level->GetBlockEntity(pos) ? UseResult::Success : UseResult::Pass;
            }
            JukeboxBlockEntity* jukebox = JukeboxAt(*level, pos);
            if (!jukebox) return UseResult::Pass;
            jukebox->PopOutTheItem();
            return UseResult::Success;
        }

        // MC ownSignal (getSignal's default): 15 while a song plays.
        int JukeboxGetSignal(const IBlockAccess& level, const glm::ivec3& pos, BlockState, Direction) {
            const JukeboxBlockEntity* jukebox = JukeboxAtReadOnly(level, pos);
            return (jukebox && jukebox->GetSongPlayer().IsPlaying()) ? 15 : 0;
        }

        // MC getAnalogOutputSignal: the disc's song's comparator_output.
        int JukeboxAnalogOutput(ILevelWrite& level, const glm::ivec3& pos, BlockState, Direction) {
            if (auto* jukebox = dynamic_cast<JukeboxBlockEntity*>(level.GetBlockEntity(pos))) {
                return jukebox->GetComparatorOutput();
            }
            return 0;
        }

        // MC affectNeighborsAfterRemoval → Containers.updateNeighboursAfterDestroy.
        void JukeboxAfterRemoval(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool) {
            level.UpdateNeighbourForOutputSignal(pos, state.Block());
        }

    } // namespace

    void RegisterJukeboxBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        Block& b = blocks[static_cast<size_t>(BlockID::Jukebox)];
        b.useItemOn                   = &JukeboxUseItemOn;
        b.useWithoutItem              = &JukeboxUseWithoutItem;
        b.isSignalSource              = true;
        b.getSignal                   = &JukeboxGetSignal;
        b.getDirectSignal             = nullptr;   // MC: no direct (strong) signal
        b.hasAnalogOutputSignal       = true;
        b.getAnalogOutputSignal       = &JukeboxAnalogOutput;
        b.affectNeighborsAfterRemoval = &JukeboxAfterRemoval;
    }

} // namespace Game
