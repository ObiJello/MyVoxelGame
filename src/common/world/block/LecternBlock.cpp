// File: src/common/world/block/LecternBlock.cpp
//
// MC LecternBlock, method by method. The book and page are the block
// entity's (LecternBlockEntity); what lives here is the interaction, the
// blockstate bookkeeping (HAS_BOOK, POWERED) and the redstone.
#include "common/world/block/LecternBlock.hpp"

#include "common/entity/BookItems.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/block/entity/LecternBlockEntity.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"

#include <string>

namespace Game {

    namespace {

        // MC LecternBlock.PAGE_CHANGE_IMPULSE_TICKS.
        constexpr int kPageChangeImpulseTicks = 2;

        bool HasBookOf(BlockState state) { return BoolOf(state, PropertyId::HAS_BOOK); }

        // MC LecternBlock.updateBelow: the block the lectern stands on is the
        // one its direct signal (UP) charges, so it is the one told.
        void UpdateBelow(ILevelWrite& level, const glm::ivec3& pos) {
            level.UpdateNeighborsAt(Below(pos), BlockID::Lectern);
        }

        // MC LecternBlock.changePowered.
        void ChangePowered(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool powered) {
            level.SetBlock(pos.x, pos.y, pos.z, WithPowered(state, powered), World::UpdateFlags::All);
            UpdateBelow(level, pos);
        }

        // The lectern's block entity, created if the cell has none — a lectern
        // from a chunk saved before lecterns had block entities, or a
        // generated one whose template carried no nbt. MC always has one.
        LecternBlockEntity* LecternAt(ILevelWrite& level, const glm::ivec3& pos) {
            BlockEntity* be = level.GetBlockEntity(pos);
            if (!be) {
                if (const BlockEntityType* type = BlockEntityTypes::ForBlock(BlockID::Lectern)) {
                    level.SetBlockEntity(pos, type->Create(pos, BlockID::Lectern));
                    be = level.GetBlockEntity(pos);
                }
            }
            auto* lectern = dynamic_cast<LecternBlockEntity*>(be);
            if (lectern && !lectern->GetLevel()) lectern->SetLevel(&level);
            return lectern;
        }

        // MC LecternBlock.placeBook: one book off the stack (none in creative
        // — ItemStack.consumeAndReturn), onto the lectern, HAS_BOOK set, and
        // the book-put sound.
        void PlaceBook(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                       IUsePlayer* player, ItemStack& book) {
            LecternBlockEntity* lectern = LecternAt(level, pos);
            if (!lectern) return;
            ItemStack one = book;
            one.count = 1;
            if (!player || !player->isCreative()) {
                book.count -= 1;
                if (book.count <= 0) book.Clear();
            }
            // MC setBook(book, player): the pages resolve with the placing
            // player as the command source ("@s" / "@p" name them); with no
            // name behind the player it is the lectern's own ("Lectern").
            const std::string readerName = player ? player->getPlainTextName() : std::string();
            lectern->SetBook(std::move(one), readerName.empty() ? nullptr : &readerName);
            LecternResetBookState(level, pos, state, true);
            level.PlaySound(SoundExcept(nullptr), pos, SoundEvents::BOOK_PUT, SoundSource::Blocks, 1.0f, 1.0f);
        }

        // MC LecternBlock.useItemOn.
        UseResult LecternUseItemOn(ItemStack& stack, ILevelWrite* level, const glm::ivec3& pos,
                                   IUsePlayer* player, uint32_t hand, const BlockHitResult& /*hit*/) {
            if (!level) return UseResult::Pass;
            const BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);
            if (HasBookOf(state)) return UseResult::TryEmptyHandInteraction;
            if (Books::IsLecternBook(stack)) {
                // tryPlaceBook: HAS_BOOK is already known false here, so it
                // succeeds; only the server moves the book (the client's
                // prediction takes the click and waits for the state).
                if (!level->IsClientSide()) PlaceBook(*level, pos, state, player, stack);
                return UseResult::Success;
            }
            return (stack.IsEmpty() && hand == 0) ? UseResult::Pass : UseResult::TryEmptyHandInteraction;
        }

        // MC LecternBlock.useWithoutItem → openScreen → player.openMenu.
        UseResult LecternUseWithoutItem(ILevelWrite* level, const glm::ivec3& pos,
                                        IUsePlayer* player, const BlockHitResult& /*hit*/) {
            if (!level) return UseResult::Pass;
            const BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);
            if (!HasBookOf(state)) return UseResult::Consume;
            if (!level->IsClientSide() && player) player->OpenMenu(MenuType::Lectern, pos);
            return UseResult::Success;
        }

        // MC ownSignal (getSignal's default) / getDirectSignal.
        int LecternGetSignal(const IBlockAccess&, const glm::ivec3&, BlockState state, Direction) {
            return PoweredOf(state) ? 15 : 0;
        }
        int LecternGetDirectSignal(const IBlockAccess&, const glm::ivec3&, BlockState state, Direction direction) {
            return (direction == Direction::Up && PoweredOf(state)) ? 15 : 0;
        }

        // MC getAnalogOutputSignal: the open page's progress, with a book.
        int LecternAnalogOutput(ILevelWrite& level, const glm::ivec3& pos, BlockState state, Direction) {
            if (!HasBookOf(state)) return 0;
            if (auto* lectern = dynamic_cast<LecternBlockEntity*>(level.GetBlockEntity(pos))) {
                return lectern->GetRedstoneSignal();
            }
            return 0;
        }

        // MC LecternBlock.tick — the end of the page-turn pulse.
        void LecternTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom&) {
            ChangePowered(level, pos, state, false);
        }

        // MC affectNeighborsAfterRemoval.
        void LecternAfterRemoval(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool) {
            if (PoweredOf(state)) UpdateBelow(level, pos);
        }

    } // namespace

    void LecternResetBookState(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool hasBook) {
        const BlockState newState = WithBool(WithPowered(state, false), PropertyId::HAS_BOOK, hasBook);
        level.SetBlock(pos.x, pos.y, pos.z, newState, World::UpdateFlags::All);
        UpdateBelow(level, pos);
    }

    void LecternSignalPageChange(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
        if (state.Block() != BlockID::Lectern) return;
        ChangePowered(level, pos, state, true);
        if (ScheduledTickAccess* ticks = level.Ticks()) {
            ticks->ScheduleTick(pos, BlockID::Lectern, kPageChangeImpulseTicks);
        }
        // level.levelEvent(1043, pos, 0): the page-turn sound.
        PlayLevelEventSound(level, SoundExcept(nullptr), LevelEvent::SOUND_PAGE_TURN, pos, 0, level.Random());
    }

    void RegisterLecternBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        Block& b = blocks[static_cast<size_t>(BlockID::Lectern)];
        b.useItemOn                   = &LecternUseItemOn;
        b.useWithoutItem              = &LecternUseWithoutItem;
        b.isSignalSource              = true;
        b.getSignal                   = &LecternGetSignal;
        b.getDirectSignal             = &LecternGetDirectSignal;
        b.hasAnalogOutputSignal       = true;
        b.getAnalogOutputSignal       = &LecternAnalogOutput;
        b.tick                        = &LecternTick;
        b.affectNeighborsAfterRemoval = &LecternAfterRemoval;
    }

} // namespace Game
