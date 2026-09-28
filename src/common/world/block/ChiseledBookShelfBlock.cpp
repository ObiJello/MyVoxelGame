// File: src/common/world/block/ChiseledBookShelfBlock.cpp
#include "common/world/block/ChiseledBookShelfBlock.hpp"

#include "common/core/Mth.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/entity/ChiseledBookShelfBlockEntity.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

namespace Game {

    namespace {

        int FacingFace(BlockState state) {
            const std::string_view f = state.GetValueByName("facing");
            if (f == "south") return 3;
            if (f == "west")  return 4;
            if (f == "east")  return 5;
            return 2;   // north
        }

        // SelectableSlotContainer.getSection.
        int Section(float relative, int maxSections) {
            const float targetedPixel = relative * 16.0f;
            const float sectionSize = 16.0f / static_cast<float>(maxSections);
            return std::clamp(static_cast<int>(std::floor(targetedPixel / sectionSize)), 0, maxSections - 1);
        }

        // SelectableSlotContainer.getHitSlot (2 rows, 3 columns): only a hit
        // on the facing side; its coordinates measured from the NEIGHBOUR
        // cell's corner (hitResult.getBlockPos().relative(direction)), as MC
        // does.
        std::optional<int> GetHitSlot(const BlockHitResult& hit, int blockFace) {
            if (hit.face != blockFace) return std::nullopt;
            glm::ivec3 neighbour = hit.blockPos;
            switch (hit.face) {
                case 2: neighbour.z -= 1; break;
                case 3: neighbour.z += 1; break;
                case 4: neighbour.x -= 1; break;
                case 5: neighbour.x += 1; break;
                default: return std::nullopt;
            }
            const glm::dvec3 rel = hit.hitPoint - glm::dvec3(neighbour);
            float x = 0.0f;
            switch (hit.face) {
                case 2: x = static_cast<float>(1.0 - rel.x); break;   // NORTH
                case 3: x = static_cast<float>(rel.x); break;         // SOUTH
                case 4: x = static_cast<float>(rel.z); break;         // WEST
                case 5: x = static_cast<float>(1.0 - rel.z); break;   // EAST
                default: break;
            }
            const float y = static_cast<float>(rel.y);
            const int row = Section(1.0f - y, 2);
            const int column = Section(x, 3);
            return column + row * 3;
        }

        bool SlotOccupied(BlockState state, int slot) {
            return state.GetIndex(static_cast<PropertyId>(static_cast<int>(PropertyId::SLOT_0_OCCUPIED) + slot)) >= 0 &&
                   state.GetName(static_cast<PropertyId>(static_cast<int>(PropertyId::SLOT_0_OCCUPIED) + slot)) == "true";
        }

        ChiseledBookShelfBlockEntity* ShelfAt(ILevelWrite& level, const glm::ivec3& pos) {
            auto* shelf = dynamic_cast<ChiseledBookShelfBlockEntity*>(level.GetBlockEntity(pos));
            if (shelf && !shelf->GetLevel() && !level.IsClientSide()) shelf->SetLevel(&level);
            return shelf;
        }

        // ChiseledBookShelfBlock.useItemOn.
        UseResult ShelfUseItemOn(ItemStack& stack, ILevelWrite* level, const glm::ivec3& pos,
                                 IUsePlayer* player, uint32_t /*hand*/, const BlockHitResult& hit) {
            if (!level) return UseResult::Pass;
            ChiseledBookShelfBlockEntity* shelf = ShelfAt(*level, pos);
            if (!shelf) return UseResult::Pass;
            if (!ChiseledBookShelfBlockEntity::AcceptsItemType(stack)) return UseResult::TryEmptyHandInteraction;
            const BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);
            const std::optional<int> slot = GetHitSlot(hit, FacingFace(state));
            if (!slot) return UseResult::Pass;
            if (SlotOccupied(state, *slot)) return UseResult::TryEmptyHandInteraction;
            // addBook: server only.
            if (!level->IsClientSide()) {
                const char* sound = stack.itemId == Items::EnchantedBook ? SoundEvents::CHISELED_BOOKSHELF_INSERT_ENCHANTED
                                                                          : SoundEvents::CHISELED_BOOKSHELF_INSERT;
                // itemStack.consumeAndReturn(1, player).
                ItemStack book = stack;
                book.count = 1;
                if (!player || !player->isCreative()) {
                    stack.count -= 1;
                    if (stack.count <= 0) stack.Clear();
                }
                shelf->SetItem(*slot, book);
                level->PlaySound(nullptr, pos, sound, SoundSource::Blocks, 1.0f, 1.0f);
            }
            return UseResult::Success;
        }

        // ChiseledBookShelfBlock.useWithoutItem.
        UseResult ShelfUseWithoutItem(ILevelWrite* level, const glm::ivec3& pos, IUsePlayer* player,
                                      const BlockHitResult& hit) {
            if (!level) return UseResult::Pass;
            ChiseledBookShelfBlockEntity* shelf = ShelfAt(*level, pos);
            if (!shelf) return UseResult::Pass;
            const BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);
            const std::optional<int> slot = GetHitSlot(hit, FacingFace(state));
            if (!slot) return UseResult::Pass;
            if (!SlotOccupied(state, *slot)) return UseResult::Consume;
            // removeBook: server only.
            if (!level->IsClientSide()) {
                const ItemStack book = shelf->RemoveItem(*slot, 1);
                const char* sound = book.itemId == Items::EnchantedBook ? SoundEvents::CHISELED_BOOKSHELF_PICKUP_ENCHANTED
                                                                         : SoundEvents::CHISELED_BOOKSHELF_PICKUP;
                level->PlaySound(nullptr, pos, sound, SoundSource::Blocks, 1.0f, 1.0f);
                if (player) player->AddItemOrDrop(book);
                level->GameEvent(player ? player->GameEventSource() : nullptr, GameEventId::BlockChange, pos);
            }
            return UseResult::Success;
        }

        // getAnalogOutputSignal: last interacted slot + 1 (0 on a client).
        int ShelfAnalogOutput(ILevelWrite& level, const glm::ivec3& pos, BlockState, Direction) {
            if (level.IsClientSide()) return 0;
            if (auto* shelf = dynamic_cast<ChiseledBookShelfBlockEntity*>(level.GetBlockEntity(pos))) {
                return shelf->GetLastInteractedSlot() + 1;
            }
            return 0;
        }

        void ShelfAfterRemoval(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool) {
            level.UpdateNeighbourForOutputSignal(pos, state.Block());
        }

    } // namespace

    void RegisterChiseledBookShelfBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        Block& b = blocks[static_cast<size_t>(BlockID::ChiseledBookshelf)];
        b.useItemOn                   = &ShelfUseItemOn;
        b.useWithoutItem              = &ShelfUseWithoutItem;
        b.hasAnalogOutputSignal       = true;
        b.getAnalogOutputSignal       = &ShelfAnalogOutput;
        b.affectNeighborsAfterRemoval = &ShelfAfterRemoval;
    }

} // namespace Game
