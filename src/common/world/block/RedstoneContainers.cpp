// File: src/common/world/block/RedstoneContainers.cpp
//
// The redstone half of the container blocks: HopperBlock's enable/disable
// and item intake, DispenserBlock / DropperBlock's trigger and tick, and
// the comparator readings — AbstractContainerMenu.getRedstoneSignalFrom-
// Container for every container block entity, plus the handful of
// state-only readings (cake, cauldron, composter, end portal frame,
// respawn anchor, jukebox).
#include "common/world/block/RedstoneContainers.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/inventory/Container.hpp"
#include "common/world/block/DispenseItemBehavior.hpp"
#include "common/world/block/RedstoneSignal.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/block/entity/DispenserBlockEntity.hpp"
#include "common/world/block/entity/HopperBlockEntity.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"

#include <cmath>
#include <memory>

namespace Game {

    namespace {

        BlockState StateAt(const IBlockAccess& level, const glm::ivec3& p) {
            return level.GetBlockState(p.x, p.y, p.z);
        }

        // MC Mth.lerpDiscrete(alpha, 0, 15).
        int LerpDiscrete(float alpha, int p0, int p1) {
            const int delta = p1 - p0;
            return p0 + static_cast<int>(std::floor(alpha * static_cast<float>(delta - 1))) + (alpha > 0.0f ? 1 : 0);
        }

        // ── Hopper ───────────────────────────────────────────────────────

        void HopperCheckPoweredState(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            const bool shouldBeOn = !HasNeighborSignal(level, pos);
            if (shouldBeOn != BoolOf(state, PropertyId::ENABLED)) {
                level.SetBlock(pos.x, pos.y, pos.z, WithBool(state, PropertyId::ENABLED, shouldBeOn),
                               World::UpdateFlags::UpdateClients);
            }
        }
        void HopperOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockState oldState, bool) {
            if (oldState.Block() != state.Block()) HopperCheckPoweredState(level, pos, state);
        }
        void HopperNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockID, bool) {
            HopperCheckPoweredState(level, pos, state);
        }
        void HopperAnyInside(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            if (auto* hopper = dynamic_cast<HopperBlockEntity*>(level.GetBlockEntity(pos))) {
                hopper->ItemInside(level, pos, state);
            }
        }

        // ── Dispenser / dropper ──────────────────────────────────────────

        void DispenserNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockID, bool) {
            const bool shouldTrigger = HasNeighborSignal(level, pos) || HasNeighborSignal(level, Above(pos));
            const bool isTriggered = BoolOf(state, PropertyId::TRIGGERED);
            if (shouldTrigger && !isTriggered) {
                if (auto* ticks = level.Ticks()) ticks->ScheduleTick(pos, state.Block(), 4);
                level.SetBlock(pos.x, pos.y, pos.z, WithBool(state, PropertyId::TRIGGERED, true),
                               World::UpdateFlags::UpdateClients);
            } else if (!shouldTrigger && isTriggered) {
                level.SetBlock(pos.x, pos.y, pos.z, WithBool(state, PropertyId::TRIGGERED, false),
                               World::UpdateFlags::UpdateClients);
            }
        }

        // MC DispenserBlock.dispenseFrom / DropperBlock.dispenseFrom.
        void DispenserTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            auto* blockEntity = dynamic_cast<DispenserBlockEntity*>(level.GetBlockEntity(pos));
            if (!blockEntity) {
                Log::Warning("Ignoring dispensing attempt for dispenser without block entity at %d,%d,%d",
                             pos.x, pos.y, pos.z);
                return;
            }
            const DispenseSource source{level, pos, state, *blockEntity};
            const int slot = blockEntity->GetRandomSlot(random);
            if (slot < 0) {
                // MC: level.levelEvent(1001, pos, 0) — DISPENSER_FAIL at pitch 1.2.
                PlayLevelEventSound(level, nullptr, LevelEvent::SOUND_DISPENSER_FAIL, pos, 0, &random);
                return;
            }
            const ItemStack stack = blockEntity->GetItem(slot);
            if (state.Is(BlockID::Dropper)) {
                if (stack.IsEmpty()) return;
                const Direction direction = FacingOf(StateAt(level, pos));
                std::unique_ptr<IContainer> owned;
                IContainer* into = HopperBlockEntity::GetContainerAt(level, Relative(pos, direction), owned);
                ItemStack remaining;
                if (!into) {
                    remaining = DispenseDefault(source, stack);
                } else {
                    ItemStack one = stack;
                    one.count = 1;
                    remaining = HopperBlockEntity::AddItem(blockEntity, *into, one, Opposite(direction), true);
                    if (remaining.IsEmpty()) {
                        remaining = stack;
                        remaining.count -= 1;
                        if (remaining.count <= 0) remaining.Clear();
                    } else {
                        remaining = stack;
                    }
                }
                blockEntity->SetItem(slot, remaining);
                return;
            }
            blockEntity->SetItem(slot, DispenseItem(source, stack));
        }

        // ── Comparator readings ──────────────────────────────────────────

        // MC AbstractContainerMenu.getRedstoneSignalFromContainer.
        int SignalFromContainer(const IContainer* container) {
            if (!container) return 0;
            float totalPercent = 0.0f;
            const int n = container->GetContainerSize();
            for (int i = 0; i < n; ++i) {
                const ItemStack& stack = container->GetItem(i);
                if (!stack.IsEmpty()) {
                    totalPercent += static_cast<float>(stack.count) /
                                    static_cast<float>(container->GetMaxStackSize(stack));
                }
            }
            totalPercent /= static_cast<float>(n);
            return LerpDiscrete(totalPercent, 0, 15);
        }

        int ContainerAnalogOutput(ILevelWrite& level, const glm::ivec3& pos, BlockState, Direction) {
            // ChestBlock.getAnalogOutputSignal reads the JOINED container of a
            // double chest (getContainer with ignoreBlocked = false — a lid
            // held shut reads 0 in vanilla; blocking is not modelled here).
            std::unique_ptr<IContainer> owned;
            return SignalFromContainer(HopperBlockEntity::GetContainerAt(level, pos, owned));
        }

        // MC Containers.updateNeighboursAfterDestroy.
        void ContainerAfterRemoval(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool) {
            level.UpdateNeighbourForOutputSignal(pos, state.Block());
        }

        int CakeAnalogOutput(ILevelWrite&, const glm::ivec3&, BlockState state, Direction) {
            // CakeBlock: (7 - bites) * 2.
            return (7 - state.GetIndex(PropertyId::BITES)) * 2;
        }
        int CauldronAnalogOutput(ILevelWrite&, const glm::ivec3&, BlockState state, Direction) {
            // LayeredCauldronBlock: LEVEL (1..3); LavaCauldron: 3; empty: 0.
            if (state.Is(BlockID::LavaCauldron)) return 3;
            if (state.HasProperty(PropertyId::LEVEL_CAULDRON)) return state.GetIndex(PropertyId::LEVEL_CAULDRON) + 1;
            return 0;
        }
        int ComposterAnalogOutput(ILevelWrite&, const glm::ivec3&, BlockState state, Direction) {
            return state.GetIndex(PropertyId::LEVEL_COMPOSTER);
        }
        int EndPortalFrameAnalogOutput(ILevelWrite&, const glm::ivec3&, BlockState state, Direction) {
            return BoolOf(state, PropertyId::EYE) ? 15 : 0;
        }
        int RespawnAnchorAnalogOutput(ILevelWrite&, const glm::ivec3&, BlockState state, Direction) {
            // RespawnAnchorBlock.getScaledChargeLevel(state, 15).
            return static_cast<int>(std::floor(static_cast<float>(state.GetIndex(PropertyId::CHARGES)) / 4.0f * 15.0f));
        }
        int JukeboxAnalogOutput(ILevelWrite&, const glm::ivec3&, BlockState state, Direction) {
            return BoolOf(state, PropertyId::HAS_RECORD) ? 15 : 0;
        }

    } // namespace

    void RegisterContainerBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        auto at = [&blocks](BlockID id) -> Block& { return blocks[static_cast<size_t>(id)]; };

        {
            Block& hopper = at(BlockID::Hopper);
            hopper.onPlace         = &HopperOnPlace;
            hopper.neighborChanged = &HopperNeighborChanged;
            hopper.anyInside       = &HopperAnyInside;
        }
        for (BlockID id : {BlockID::Dispenser, BlockID::Dropper}) {
            Block& b = at(id);
            b.neighborChanged = &DispenserNeighborChanged;
            b.tick            = &DispenserTick;
        }

        // Every block whose block entity is a container reads on a
        // comparator and pokes the comparators when broken.
        int containers = 0;
        for (size_t i = 0; i < blocks.size(); ++i) {
            const BlockID id = static_cast<BlockID>(i);
            const BlockEntityType* type = BlockEntityTypes::ForBlock(id);
            if (!type) continue;
            const std::string& tid = type->StringId();
            const bool container =
                tid == "chest" || tid == "trapped_chest" || tid == "barrel" || tid == "shulker_box" ||
                tid == "dispenser" || tid == "dropper" || tid == "hopper" || tid == "furnace" ||
                tid == "blast_furnace" || tid == "smoker" || tid == "brewing_stand" || tid == "crafter";
            if (!container) continue;
            Block& b = blocks[i];
            b.hasAnalogOutputSignal       = true;
            b.getAnalogOutputSignal       = &ContainerAnalogOutput;
            if (!b.affectNeighborsAfterRemoval) b.affectNeighborsAfterRemoval = &ContainerAfterRemoval;
            ++containers;
        }

        auto analog = [&](BlockID id, BlockAnalogOutputSignalFn fn) {
            Block& b = at(id);
            b.hasAnalogOutputSignal = true;
            b.getAnalogOutputSignal = fn;
        };
        analog(BlockID::Cake, &CakeAnalogOutput);
        for (size_t i = 0; i < blocks.size(); ++i) {
            const std::string& slug = blocks[i].registrySlug;
            if (slug == "water_cauldron" || slug == "lava_cauldron" || slug == "powder_snow_cauldron" ||
                slug == "cauldron") {
                analog(static_cast<BlockID>(i), &CauldronAnalogOutput);
            }
        }
        analog(BlockID::Composter, &ComposterAnalogOutput);
        analog(BlockID::EndPortalFrame, &EndPortalFrameAnalogOutput);
        analog(BlockID::RespawnAnchor, &RespawnAnchorAnalogOutput);
        analog(BlockID::Jukebox, &JukeboxAnalogOutput);

        Log::Info("[Redstone] %d container blocks read on comparators", containers);
    }

} // namespace Game
