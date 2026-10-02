// File: src/common/world/block/ComposterBlock.cpp
//
// Mirrors net.minecraft.world.level.block.ComposterBlock (26.3): a stack
// carrying COMPOSTABLE adds a layer at its provider's chance (the empty
// composter always takes one), level 7 ripens to 8 twenty ticks later, and
// an empty-hand click on a ripe composter pops out bone meal. Hoppers feed it
// from above and drain the bone meal from below through its
// WorldlyContainerHolder containers (Composter::GetContainer, which
// HopperBlockEntity::GetContainerAt asks first).
#include "common/data/DataComponents.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/inventory/Container.hpp"
#include "common/inventory/SimpleContainer.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string>

namespace Game {

    namespace {

        constexpr int kReady = 8;

        int LevelOf(BlockState state) { return state.GetIndex(PropertyId::LEVEL_COMPOSTER); }

        BlockState WithLevel(BlockState state, int level) {
            return state.SetIndex(PropertyId::LEVEL_COMPOSTER, level);
        }

        void ScheduleRipen(ILevelWrite& level, const glm::ivec3& pos) {
            if (ScheduledTickAccess* ticks = level.Ticks()) ticks->ScheduleTick(pos, BlockID::Composter, 20);
        }

        // ComposterBlock.addLayer: the stack's layers, resolved against the
        // composter's state (the empty composter always takes one), clamped
        // to 7; reaching 7 schedules the ripening tick.
        BlockState AddLayer(Entity* source, BlockState state, ILevelWrite& level, const glm::ivec3& pos,
                            const Compostable& compostable) {
            const int fillLevel = LevelOf(state);
            ProviderContext ctx;
            ctx.block = BlockID::Composter;
            ctx.blockLevel = fillLevel;
            ctx.random = level.Random();
            const int layersToAdd = ResolveInt(compostable.layers, ctx, 0);
            if (layersToAdd <= 0) return state;
            const int newLevel = std::clamp(fillLevel + layersToAdd, 0, 7);
            const BlockState newState = WithLevel(state, newLevel);
            level.SetBlock(pos.x, pos.y, pos.z, newState, World::UpdateFlags::All);
            level.GameEvent(GameEventId::BlockChange, pos, GameEventContext::Of(source, newState));
            if (newLevel == 7) ScheduleRipen(level, pos);
            return newState;
        }

        // ComposterBlock.empty.
        BlockState Empty(Entity* source, BlockState state, ILevelWrite& level, const glm::ivec3& pos) {
            const BlockState newState = WithLevel(state, 0);
            level.SetBlock(pos.x, pos.y, pos.z, newState, World::UpdateFlags::All);
            level.GameEvent(GameEventId::BlockChange, pos, GameEventContext::Of(source, newState));
            return newState;
        }

        // ComposterBlock.extractProduce: one bone meal at the top centre,
        // offsetRandomXZ(0.7), with a fresh ItemEntity's toss and pickup
        // delay; then empty, and the empty sound for everyone.
        BlockState ExtractProduce(Entity* source, BlockState state, ILevelWrite& level, const glm::ivec3& pos) {
            if (!level.IsClientSide()) {
                JavaRandom* random = level.Random();
                double x = pos.x + 0.5, z = pos.z + 0.5;
                glm::dvec3 velocity(0.0, 0.2, 0.0);
                if (random) {
                    x += static_cast<double>((random->NextFloat() - 0.5f) * 0.7f);
                    z += static_cast<double>((random->NextFloat() - 0.5f) * 0.7f);
                    velocity.x = random->NextDouble() * 0.2 - 0.1;
                    velocity.z = random->NextDouble() * 0.2 - 0.1;
                }
                SpawnItemEntity(level.GetDimension(), glm::dvec3(x, pos.y + 1.01, z), velocity,
                                ItemStack(Items::BoneMeal, 1), 10);
            }
            const BlockState emptyState = Empty(source, state, level, pos);
            level.PlaySound(nullptr, pos, SoundEvents::COMPOSTER_EMPTY, SoundSource::Blocks, 1.0f, 1.0f);
            return emptyState;
        }

        // MC `itemStack.consume(1, player)`.
        void ConsumeOne(ItemStack& stack, const IUsePlayer* player) {
            if (player && player->isCreative()) return;
            if (--stack.count <= 0) stack.Clear();
        }

        // ComposterBlock.useItemOn.
        UseResult UseItemOn(ItemStack& stack, ILevelWrite* level, const glm::ivec3& pos,
                            IUsePlayer* player, uint32_t /*hand*/, const BlockHitResult& /*hit*/) {
            if (!level) return UseResult::Pass;
            const BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);
            const int fillLevel = LevelOf(state);
            const auto compostable = stack.IsEmpty() ? std::nullopt : stack.get(DataComponents::COMPOSTABLE);
            if (fillLevel < kReady && compostable) {
                if (fillLevel < 7 && !level->IsClientSide()) {
                    Entity* source = player ? player->GameEventSource() : nullptr;
                    const BlockState newState = AddLayer(source, state, *level, pos, *compostable);
                    level->PlayLevelEvent(nullptr, LevelEvent::COMPOSTER_FILL, pos, state != newState ? 1 : 0);
                    // player.awardStat(ITEM_USED) — no statistics here.
                    ConsumeOne(stack, player);
                }
                return UseResult::Success;
            }
            return UseResult::TryEmptyHandInteraction;
        }

        // ComposterBlock.useWithoutItem.
        UseResult UseWithoutItem(ILevelWrite* level, const glm::ivec3& pos, IUsePlayer* player,
                                 const BlockHitResult& /*hit*/) {
            if (!level) return UseResult::Pass;
            const BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);
            if (LevelOf(state) != kReady) return UseResult::Pass;
            ExtractProduce(player ? player->GameEventSource() : nullptr, state, *level, pos);
            return UseResult::Success;
        }

        // ComposterBlock.onPlace: placed (or set) at 7, it still ripens.
        void OnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState newState, BlockState /*oldState*/,
                     bool /*movedByPiston*/) {
            if (LevelOf(newState) == 7) ScheduleRipen(level, pos);
        }

        // ComposterBlock.tick: 7 -> 8, with the ready sound.
        void Tick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom& /*random*/) {
            if (LevelOf(state) != 7) return;
            level.SetBlock(pos.x, pos.y, pos.z, WithLevel(state, kReady), World::UpdateFlags::All);
            level.PlaySound(nullptr, pos, SoundEvents::COMPOSTER_READY, SoundSource::Blocks, 1.0f, 1.0f);
        }

        // ── The WorldlyContainerHolder containers ─────────────────────────

        // ComposterBlock.OutputContainer: the bone meal, taken from below
        // only; taking it empties the composter.
        class OutputContainer : public SimpleContainer, public IWorldlyContainer {
        public:
            OutputContainer(BlockState state, ILevelWrite& level, const glm::ivec3& pos)
                : SimpleContainer(1), m_state(state), m_level(level), m_pos(pos) {
                SimpleContainer::SetItem(0, ItemStack(Items::BoneMeal, 1));
            }
            int GetMaxStackSize() const override { return 1; }
            void GetSlotsForFace(Direction face, std::vector<int>& out) const override {
                out.clear();
                if (face == Direction::Down) out.push_back(0);
            }
            bool CanPlaceItemThroughFace(int, const ItemStack&, Direction, bool) const override { return false; }
            bool CanTakeItemThroughFace(int, const ItemStack& stack, Direction face) const override {
                return !m_changed && face == Direction::Down && stack.itemId == Items::BoneMeal;
            }
            void SetChanged() override {
                Empty(nullptr, m_state, m_level, m_pos);
                m_changed = true;
            }
        private:
            BlockState   m_state;
            ILevelWrite& m_level;
            glm::ivec3   m_pos;
            bool         m_changed = false;
        };

        // ComposterBlock.InputContainer: one compostable from above; the
        // moment it lands it is composted.
        class InputContainer : public SimpleContainer, public IWorldlyContainer {
        public:
            InputContainer(BlockState state, ILevelWrite& level, const glm::ivec3& pos)
                : SimpleContainer(1), m_state(state), m_level(level), m_pos(pos) {}
            int GetMaxStackSize() const override { return 1; }
            void GetSlotsForFace(Direction face, std::vector<int>& out) const override {
                out.clear();
                if (face == Direction::Up) out.push_back(0);
            }
            bool CanPlaceItemThroughFace(int, const ItemStack& stack, Direction face, bool hasDirection) const override {
                return !m_changed && hasDirection && face == Direction::Up && stack.has(DataComponents::COMPOSTABLE);
            }
            bool CanTakeItemThroughFace(int, const ItemStack&, Direction) const override { return false; }
            void SetChanged() override {
                const ItemStack contents = GetItem(0);
                const auto compostable = contents.IsEmpty() ? std::nullopt : contents.get(DataComponents::COMPOSTABLE);
                if (!compostable || m_level.IsClientSide()) return;
                m_changed = true;
                const BlockState newState = AddLayer(nullptr, m_state, m_level, m_pos, *compostable);
                m_level.PlayLevelEvent(nullptr, LevelEvent::COMPOSTER_FILL, m_pos, newState != m_state ? 1 : 0);
                SimpleContainer::SetItem(0, ItemStack{});
            }
        private:
            BlockState   m_state;
            ILevelWrite& m_level;
            glm::ivec3   m_pos;
            bool         m_changed = false;
        };

        // ComposterBlock.EmptyContainer: the ripening composter takes
        // nothing and gives nothing.
        class EmptyContainer : public SimpleContainer, public IWorldlyContainer {
        public:
            EmptyContainer() : SimpleContainer(0) {}
            void GetSlotsForFace(Direction, std::vector<int>& out) const override { out.clear(); }
            bool CanPlaceItemThroughFace(int, const ItemStack&, Direction, bool) const override { return false; }
            bool CanTakeItemThroughFace(int, const ItemStack&, Direction) const override { return false; }
        };

    } // namespace

    namespace Composter {

        std::unique_ptr<IContainer> GetContainer(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            const int contentLevel = LevelOf(state);
            if (contentLevel == kReady) return std::make_unique<OutputContainer>(state, level, pos);
            if (contentLevel < 7) return std::make_unique<InputContainer>(state, level, pos);
            return std::make_unique<EmptyContainer>();
        }

        BlockState InsertItem(Entity* source, BlockState state, ILevelWrite& level, ItemStack& stack,
                              const glm::ivec3& pos) {
            const int fillLevel = LevelOf(state);
            const auto compostable = stack.IsEmpty() ? std::nullopt : stack.get(DataComponents::COMPOSTABLE);
            if (fillLevel < 7 && compostable) {
                const BlockState newState = AddLayer(source, state, level, pos, *compostable);
                if (--stack.count <= 0) stack.Clear();
                return newState;
            }
            return state;
        }

    } // namespace Composter

    void RegisterComposterBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        Block& composter = blocks[static_cast<size_t>(BlockID::Composter)];
        composter.useItemOn      = &UseItemOn;
        composter.useWithoutItem = &UseWithoutItem;
        composter.onPlace        = &OnPlace;
        composter.tick           = &Tick;
    }

} // namespace Game
