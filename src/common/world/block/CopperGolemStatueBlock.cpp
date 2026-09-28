// File: src/common/world/block/CopperGolemStatueBlock.cpp
#include "common/world/block/CopperGolemStatueBlock.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/CopperChestBlock.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/tags/DataTags.hpp"

#include <cstdlib>

namespace Game {

    namespace {

        constexpr BlockID kStatues[8] = {
            BlockID::CopperGolemStatue, BlockID::ExposedCopperGolemStatue,
            BlockID::WeatheredCopperGolemStatue, BlockID::OxidizedCopperGolemStatue,
            BlockID::WaxedCopperGolemStatue, BlockID::WaxedExposedCopperGolemStatue,
            BlockID::WaxedWeatheredCopperGolemStatue, BlockID::WaxedOxidizedCopperGolemStatue,
        };

        // Block.withPropertiesOf: `block`'s state with every shared property.
        BlockState WithPropertiesOf(BlockID block, BlockState from) {
            const auto& src = BlockRegistry::GetStateDefinition(from.Block());
            const auto& dst = BlockRegistry::GetStateDefinition(block);
            return BlockStates::FromIndex(block, dst.IndexOf(src.PropertiesOf(from.Index())));
        }

        // CopperGolemStatueBlock.useItemOn: an axe passes (to the axe's
        // scrape / wax-off); anything else turns the pose.
        UseResult StatueUseItemOn(ItemStack& stack, ILevelWrite* level, const glm::ivec3& pos,
                                  IUsePlayer* player, uint32_t /*hand*/, const BlockHitResult& /*hit*/) {
            if (!level) return UseResult::Pass;
            if (!stack.IsEmpty() &&
                DataTags::HasTag(DataTags::Registry::Item, ItemRegistry::Slug(stack.itemId), "minecraft:axes")) {
                return UseResult::Pass;
            }
            if (level->IsClientSide()) return UseResult::Success;
            // updatePose: the sound, the next pose, BLOCK_CHANGE.
            const BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);
            const int pose = state.GetIndex(PropertyId::COPPER_GOLEM_POSE);
            if (pose < 0) return UseResult::Pass;
            level->PlaySound(nullptr, pos, SoundEvents::COPPER_GOLEM_BECOME_STATUE, SoundSource::Blocks, 1.0f, 1.0f);
            level->SetBlock(pos.x, pos.y, pos.z, state.SetIndex(PropertyId::COPPER_GOLEM_POSE, (pose + 1) % 4),
                            World::UpdateFlags::All);
            level->GameEvent(player ? player->GameEventSource() : nullptr, GameEventId::BlockChange, pos);
            return UseResult::Success;
        }

        // An empty hand is useItemOn's EMPTY stack in MC (no axe): the pose
        // turns too.
        UseResult StatueUseWithoutItem(ILevelWrite* level, const glm::ivec3& pos, IUsePlayer* player,
                                       const BlockHitResult& hit) {
            ItemStack empty;
            return StatueUseItemOn(empty, level, pos, player, 0, hit);
        }

        int StatueAnalogOutput(ILevelWrite& level, const glm::ivec3& pos, BlockState state, Direction) {
            (void)level; (void)pos;
            return state.GetIndex(PropertyId::COPPER_GOLEM_POSE) + 1;
        }

        void StatueAfterRemoval(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool) {
            level.UpdateNeighbourForOutputSignal(pos, state.Block());
        }

        bool StatueIsRandomlyTicking(BlockState state) {
            return WeatheringCopperNext(state.Block()) != BlockID::Air;
        }

        // WeatheringCopperGolemStatueBlock.randomTick → changeOverTime
        // (ChangeOverTimeBlock: every weathering copper block within
        // Manhattan 4 votes).
        void StatueRandomTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (!(random.NextFloat() < 0.05688889f)) return;
            const int age = WeatheringCopperAge(state.Block());
            const BlockID next = WeatheringCopperNext(state.Block());
            if (age < 0 || next == BlockID::Air) return;
            int sameAgeCount = 0, olderCount = 0;
            for (int dx = -4; dx <= 4; ++dx) {
                for (int dy = -4; dy <= 4; ++dy) {
                    for (int dz = -4; dz <= 4; ++dz) {
                        if (std::abs(dx) + std::abs(dy) + std::abs(dz) > 4) continue;
                        if (dx == 0 && dy == 0 && dz == 0) continue;
                        const int foundAge = WeatheringCopperAge(level.GetBlock(pos.x + dx, pos.y + dy, pos.z + dz));
                        if (foundAge < 0) continue;
                        if (foundAge < age) return;
                        if (foundAge > age) ++olderCount;
                        else ++sameAgeCount;
                    }
                }
            }
            const float chance = static_cast<float>(olderCount + 1) / static_cast<float>(olderCount + sameAgeCount + 1);
            const float modifier = age == 0 ? 0.75f : 1.0f;
            if (!(random.NextFloat() < chance * chance * modifier)) return;
            level.SetBlock(pos.x, pos.y, pos.z, WithPropertiesOf(next, state), World::UpdateFlags::All);
        }

    } // namespace

    bool IsCopperGolemStatue(BlockID id) {
        for (BlockID s : kStatues) if (s == id) return true;
        return false;
    }

    void RegisterCopperGolemStatueBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        for (int i = 0; i < 8; ++i) {
            Block& b = blocks[static_cast<size_t>(kStatues[i])];
            b.useItemOn                   = &StatueUseItemOn;
            b.useWithoutItem              = &StatueUseWithoutItem;
            b.hasAnalogOutputSignal       = true;
            b.getAnalogOutputSignal       = &StatueAnalogOutput;
            b.affectNeighborsAfterRemoval = &StatueAfterRemoval;
            if (i < 3) {
                b.isRandomlyTicking = &StatueIsRandomlyTicking;
                b.randomTick        = &StatueRandomTick;
            }
        }
    }

} // namespace Game
