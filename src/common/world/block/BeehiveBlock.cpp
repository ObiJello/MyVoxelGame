// File: src/common/world/block/BeehiveBlock.cpp
//
// Mirrors net.minecraft.world.level.block.BeehiveBlock (26.3) for the beehive
// and the bee nest:
//
//   * useItemOn at honey level 5: shears harvest the harvest/beehive loot
//     (three honeycomb), a glass bottle fills to a honey bottle; then, unless
//     a lit campfire below smokes the hive, the bees inside burst out
//     (EMERGENCY) and the bees around turn on the players near it — either
//     way the honey level drops to 0;
//   * a comparator reads the honey level;
//   * a FireBlock appearing next to it empties it (MC updateShape — here the
//     writable neighbour notification, since updateShape is read-only).
//
// The block entity (bees, ticks, release) is BeehiveBlockEntity; the break
// halves (creative drop with BEES, survival release) live with the other
// playerWillDestroy / playerDestroy hooks in PlayerSession.
#include "common/core/JavaRandom.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/CandleBlocks.hpp"
#include "common/world/block/entity/BeehiveBlockEntity.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/loot/ChestLootTables.hpp"

#include <string>
#include <vector>

namespace Game {

    namespace {

        constexpr int kMaxHoneyLevel = 5;

        int HoneyLevel(BlockState state) {
            return state.HasProperty(PropertyId::HONEY_LEVEL) ? state.GetIndex(PropertyId::HONEY_LEVEL) : 0;
        }

        // BeehiveBlock.resetHoneyLevel: setBlockAndUpdate(HONEY_LEVEL 0).
        void ResetHoneyLevel(ILevelWrite& level, BlockState state, const glm::ivec3& pos) {
            if (!state.HasProperty(PropertyId::HONEY_LEVEL)) return;
            level.SetBlock(pos.x, pos.y, pos.z, state.SetIndex(PropertyId::HONEY_LEVEL, 0), World::UpdateFlags::All);
        }

        // BeehiveBlock.dropHoneycomb: the harvest/beehive block-interact
        // table, each stack popped at the hive.
        void DropHoneycomb(ILevelWrite& level, const glm::ivec3& pos) {
            std::vector<ItemStack> items;
            ChestLoot::LootLevelContext ctx;
            ctx.dimensionId = static_cast<int>(level.GetDimension());
            ctx.origin = glm::dvec3(pos) + glm::dvec3(0.5);
            JavaRandom* random = level.Random();
            JavaRandom fallback(static_cast<int64_t>(pos.x) * 3129871 ^ static_cast<int64_t>(pos.z) * 116129781 ^ pos.y);
            if (!ChestLoot::GetRandomItems("minecraft:harvest/beehive", random ? *random : fallback, 0.0f, items,
                                           &ctx)) {
                items.assign(1, ItemStack(Items::Honeycomb, 3));
            }
            for (const ItemStack& stack : items) {
                if (!stack.IsEmpty()) DropItemStackNear(level.GetDimension(), pos, stack);
            }
        }

        // BeehiveBlock.useItemOn.
        UseResult UseItemOn(ItemStack& stack, ILevelWrite* level, const glm::ivec3& pos, IUsePlayer* player,
                            uint32_t hand, const BlockHitResult& /*hit*/) {
            if (!level || !player) return UseResult::TryEmptyHandInteraction;
            const BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);
            if (HoneyLevel(state) < kMaxHoneyLevel) return UseResult::TryEmptyHandInteraction;
            // The harvest is the server's: the client's copy of the stack,
            // the honey level and the bees all come back from it.
            const bool shears = stack.itemId == Items::Shears && !stack.IsEmpty();
            const bool bottle = stack.itemId == Items::GlassBottle && !stack.IsEmpty();
            if (!shears && !bottle) return UseResult::TryEmptyHandInteraction;
            if (level->IsClientSide()) return UseResult::Success;

            Entity* source = player->GameEventSource();
            if (shears) {
                DropHoneycomb(*level, pos);
                level->PlaySound(nullptr, player->getPosition(), SoundEvents::BEEHIVE_SHEAR, SoundSource::Blocks,
                                 1.0f, 1.0f);
                HurtAndBreak(stack, 1, level, player, hand);
                level->GameEvent(source, GameEventId::Shear, pos);
            } else {
                // itemStack.shrink(1) — a plain shrink, creative included.
                --stack.count;
                level->PlaySound(player, player->getPosition(), SoundEvents::BOTTLE_FILL, SoundSource::Blocks,
                                 1.0f, 1.0f);
                if (stack.count <= 0) {
                    stack = ItemStack(Items::HoneyBottle, 1);
                } else {
                    player->AddItemOrDrop(ItemStack(Items::HoneyBottle, 1));
                }
                level->GameEvent(source, GameEventId::FluidPickup, pos);
            }
            player->markSlotDirty(player->handSlotIndex(hand));

            auto* hive = dynamic_cast<BeehiveBlockEntity*>(level->GetBlockEntity(pos));
            if (!IsSmokeyPos(*level, pos)) {
                if (hive && !hive->IsEmpty()) AngerNearbyBees(*level, pos);
                // releaseBeesAndResetHoneyLevel(EMERGENCY).
                ResetHoneyLevel(*level, state, pos);
                if (hive) {
                    hive->EmptyAllLivingFromHive(*level, source, level->GetBlockState(pos.x, pos.y, pos.z),
                                                 BeehiveBlockEntity::ReleaseStatus::Emergency);
                }
            } else {
                ResetHoneyLevel(*level, state, pos);
            }
            return UseResult::Success;
        }

        // BeehiveBlock.getAnalogOutputSignal: the honey level.
        int AnalogOutput(ILevelWrite& /*level*/, const glm::ivec3& /*pos*/, BlockState state, Direction /*dir*/) {
            return HoneyLevel(state);
        }

        // BeehiveBlock.updateShape's fire check: a FireBlock next to the
        // hive empties it.
        void NeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockID sourceBlock,
                             bool /*movedByPiston*/) {
            if (level.IsClientSide() || sourceBlock != BlockID::Fire) return;
            if (auto* hive = dynamic_cast<BeehiveBlockEntity*>(level.GetBlockEntity(pos))) {
                hive->EmptyAllLivingFromHive(level, nullptr, state, BeehiveBlockEntity::ReleaseStatus::Emergency);
            }
        }

    } // namespace

    bool TryShearBeehiveFromDispenser(ILevelWrite& level, const glm::ivec3& pos) {
        const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);
        // state.is(#beehives, s -> s.hasProperty(HONEY_LEVEL) && BeehiveBlock).
        if (state.Block() != BlockID::Beehive && state.Block() != BlockID::BeeNest) return false;
        if (HoneyLevel(state) < kMaxHoneyLevel) return false;
        level.PlaySound(nullptr, pos, SoundEvents::BEEHIVE_SHEAR, SoundSource::Blocks, 1.0f, 1.0f);
        DropHoneycomb(level, pos);
        // releaseBeesAndResetHoneyLevel(level, state, pos, null, BEE_RELEASED).
        ResetHoneyLevel(level, state, pos);
        if (auto* hive = dynamic_cast<BeehiveBlockEntity*>(level.GetBlockEntity(pos))) {
            hive->EmptyAllLivingFromHive(level, nullptr, state, BeehiveBlockEntity::ReleaseStatus::BeeReleased);
        }
        level.GameEvent(static_cast<Entity*>(nullptr), GameEventId::Shear, pos);
        return true;
    }

    void RegisterBeehiveBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        for (BlockID id : {BlockID::Beehive, BlockID::BeeNest}) {
            Block& b = blocks[static_cast<size_t>(id)];
            b.useItemOn             = &UseItemOn;
            b.hasAnalogOutputSignal = true;
            b.getAnalogOutputSignal = &AnalogOutput;
            if (!b.neighborChanged) b.neighborChanged = &NeighborChanged;
        }
    }

} // namespace Game
