// File: src/common/world/block/entity/HopperBlockEntity.cpp
#include "common/world/block/entity/HopperBlockEntity.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/inventory/CompoundContainer.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/entity/DoubleChest.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"

#include <algorithm>
#include <memory>
#include <vector>

namespace Game {

    namespace {

        // MC ItemStack.isSameItemSameComponents(a, b) && a.count <= max.
        bool CanMergeItems(const ItemStack& a, const ItemStack& b) {
            return a.count <= ItemRegistry::Get(a.itemId).maxStackSize && IsSameItemSameComponents(a, b);
        }

        // MC HopperBlockEntity.getSlots(container, direction).
        void GetSlots(const IContainer& container, Direction direction, bool hasDirection,
                      std::vector<int>& out) {
            out.clear();
            if (hasDirection) {
                if (const auto* worldly = dynamic_cast<const IWorldlyContainer*>(&container)) {
                    worldly->GetSlotsForFace(direction, out);
                    return;
                }
            } else if (dynamic_cast<const IWorldlyContainer*>(&container)) {
                // MC: a WorldlyContainer asked with a null direction falls
                // through to the flat slot list.
            }
            const int n = container.GetContainerSize();
            for (int i = 0; i < n; ++i) out.push_back(i);
        }

        bool IsFullContainer(const IContainer& container, Direction direction) {
            std::vector<int> slots;
            GetSlots(container, direction, true, slots);
            for (int slot : slots) {
                const ItemStack& stack = container.GetItem(slot);
                // MC: `stack.getCount() < stack.getMaxStackSize()`, where the
                // EMPTY stack's max is 64 (Items.AIR). This engine's air item
                // has a max stack of 0, so an empty slot must be answered
                // explicitly or it reads as full and the hopper never ejects.
                if (stack.IsEmpty()) return false;
                if (stack.count < ItemRegistry::Get(stack.itemId).maxStackSize) return false;
            }
            return true;
        }

        bool CanPlaceItemInContainer(const IContainer& container, const ItemStack& stack, int slot,
                                     Direction direction, bool hasDirection) {
            if (!container.CanPlaceItem(slot, stack)) return false;
            if (const auto* worldly = dynamic_cast<const IWorldlyContainer*>(&container)) {
                if (!worldly->CanPlaceItemThroughFace(slot, stack, direction, hasDirection)) return false;
            }
            return true;
        }

        bool CanTakeItemFromContainer(const IContainer& into, const IContainer& from, const ItemStack& stack,
                                      int slot, Direction direction) {
            if (!from.CanTakeItem(into, slot, stack)) return false;
            if (const auto* worldly = dynamic_cast<const IWorldlyContainer*>(&from)) {
                if (!worldly->CanTakeItemThroughFace(slot, stack, direction)) return false;
            }
            return true;
        }

    } // namespace

    // Friend access to the hopper's cooldown from the shared insert.
    struct HopperTransfer {
        static ItemStack TryMoveInItem(IContainer* from, IContainer& container, ItemStack stack, int slot,
                                       Direction direction, bool hasDirection) {
            ItemStack& current = container.GetItem(slot);
            if (!CanPlaceItemInContainer(container, stack, slot, direction, hasDirection)) return stack;
            bool success = false;
            const bool wasEmpty = container.IsEmpty();
            if (current.IsEmpty()) {
                container.SetItem(slot, stack);
                stack = ItemStack{};
                success = true;
            } else if (CanMergeItems(current, stack)) {
                const int space = ItemRegistry::Get(stack.itemId).maxStackSize - current.count;
                const int count = std::min(stack.count, space);
                stack.count -= count;
                if (stack.count <= 0) stack.Clear();
                current.count += count;
                success = count > 0;
            }
            if (success) {
                if (wasEmpty) {
                    if (auto* hopper = dynamic_cast<HopperBlockEntity*>(&container)) {
                        if (!hopper->IsOnCustomCooldown()) {
                            int skipTickCount = 0;
                            if (auto* fromHopper = dynamic_cast<HopperBlockEntity*>(from)) {
                                if (hopper->m_tickedGameTime >= fromHopper->m_tickedGameTime) skipTickCount = 1;
                            }
                            hopper->SetCooldown(HopperBlockEntity::kMoveItemSpeed - skipTickCount);
                        }
                    }
                }
                container.SetChanged();
            }
            return stack;
        }
    };

    ItemStack HopperBlockEntity::AddItem(IContainer* from, IContainer& container, ItemStack stack,
                                         Direction direction, bool hasDirection) {
        if (hasDirection) {
            if (const auto* worldly = dynamic_cast<const IWorldlyContainer*>(&container)) {
                std::vector<int> slots;
                worldly->GetSlotsForFace(direction, slots);
                for (size_t i = 0; i < slots.size() && !stack.IsEmpty(); ++i) {
                    stack = HopperTransfer::TryMoveInItem(from, container, stack, slots[i], direction, true);
                }
                return stack;
            }
        }
        const int size = container.GetContainerSize();
        for (int i = 0; i < size && !stack.IsEmpty(); ++i) {
            stack = HopperTransfer::TryMoveInItem(from, container, stack, i, direction, hasDirection);
        }
        return stack;
    }

    // MC getBlockContainer: a block entity that is a Container; a chest
    // joined to its partner as a CompoundContainer (ChestBlock.getContainer
    // with ignoreBlocked = true — the hopper does not care about a lid held
    // shut). Entity containers (chest minecarts) do not exist here.
    IContainer* HopperBlockEntity::GetContainerAt(ILevelWrite& level, const glm::ivec3& pos,
                                                  std::unique_ptr<IContainer>& owned) {
        BlockEntity* be = level.GetBlockEntity(pos);
        auto* container = dynamic_cast<IContainer*>(be);
        if (!container) return nullptr;
        const BlockID id = level.GetBlock(pos.x, pos.y, pos.z);
        if (id == BlockID::Chest || id == BlockID::TrappedChest) {
            if (auto pairing = FindChestPartner(level, pos)) {
                if (auto* partner = dynamic_cast<IContainer*>(level.GetBlockEntity(pairing->partnerPos))) {
                    owned = pairing->selfIsFirst
                        ? std::make_unique<CompoundContainer>(container, partner)
                        : std::make_unique<CompoundContainer>(partner, container);
                    return owned.get();
                }
            }
        }
        return container;
    }

    void HopperBlockEntity::SetItem(int index, const ItemStack& stack) {
        ItemStack limited = stack;
        const int max = GetMaxStackSize(stack);
        if (limited.count > max) limited.count = max;
        BaseContainerBlockEntity::SetItem(index, limited);
    }

    bool HopperBlockEntity::InventoryFull() const {
        for (int i = 0; i < GetContainerSize(); ++i) {
            const ItemStack& stack = GetItem(i);
            if (stack.IsEmpty() || stack.count != ItemRegistry::Get(stack.itemId).maxStackSize) return false;
        }
        return true;
    }

    namespace {

        // MC ejectItems.
        bool EjectItems(ILevelWrite& level, const glm::ivec3& blockPos, HopperBlockEntity& self, Direction facing) {
            std::unique_ptr<IContainer> owned;
            IContainer* container = HopperBlockEntity::GetContainerAt(level, Relative(blockPos, facing), owned);
            if (!container) return false;
            const Direction direction = Opposite(facing);
            if (IsFullContainer(*container, direction)) return false;
            for (int slot = 0; slot < self.GetContainerSize(); ++slot) {
                const ItemStack stack = self.GetItem(slot);
                if (stack.IsEmpty()) continue;
                const int originalCount = stack.count;
                const ItemStack result = HopperBlockEntity::AddItem(&self, *container, self.RemoveItem(slot, 1), direction, true);
                if (result.IsEmpty()) {
                    container->SetChanged();
                    return true;
                }
                // Put the one back.
                ItemStack restored = stack;
                restored.count = originalCount;
                self.SetItem(slot, restored);
            }
            return false;
        }

        bool TryTakeInItemFromSlot(HopperBlockEntity& hopper, IContainer& container, int slot, Direction direction) {
            const ItemStack stack = container.GetItem(slot);
            if (stack.IsEmpty() || !CanTakeItemFromContainer(hopper, container, stack, slot, direction)) return false;
            const int originalCount = stack.count;
            const ItemStack result = HopperBlockEntity::AddItem(&container, hopper, container.RemoveItem(slot, 1),
                                                                Direction::Down, false);
            if (result.IsEmpty()) {
                container.SetChanged();
                return true;
            }
            ItemStack restored = stack;
            restored.count = originalCount;
            container.SetItem(slot, restored);
            return false;
        }

        // MC addItem(container, ItemEntity): take a dropped item into the hopper.
        bool AddItemEntity(ILevelWrite& level, HopperBlockEntity& hopper, int32_t itemEntityId) {
            EntityLevel* entities = level.Entities();
            if (!entities) return false;
            const ItemStack* stack = entities->GetItemEntityStack(itemEntityId);
            if (!stack || stack->IsEmpty()) return false;
            const ItemStack result = HopperBlockEntity::AddItem(nullptr, hopper, *stack, Direction::Down, false);
            if (result.IsEmpty()) {
                entities->SetItemEntityStack(itemEntityId, ItemStack{});
                return true;
            }
            entities->SetItemEntityStack(itemEntityId, result);
            return false;
        }

        // MC Hopper.SUCK_AABB = column(16, 11, 32), moved to the cell.
        AABBd SuckBox(const glm::ivec3& pos) {
            return AABBd::FromMinMax(glm::dvec3(pos) + glm::dvec3(0.0, 11.0 / 16.0, 0.0),
                                     glm::dvec3(pos) + glm::dvec3(1.0, 2.0, 1.0));
        }

        // MC suckInItems.
        bool SuckInItems(ILevelWrite& level, const glm::ivec3& pos, HopperBlockEntity& hopper) {
            const glm::ivec3 above = Above(pos);
            std::unique_ptr<IContainer> owned;
            IContainer* container = HopperBlockEntity::GetContainerAt(level, above, owned);
            if (container) {
                std::vector<int> slots;
                GetSlots(*container, Direction::Down, true, slots);
                for (int slot : slots) {
                    if (TryTakeInItemFromSlot(hopper, *container, slot, Direction::Down)) return true;
                }
                return false;
            }
            // isBlocked: a full collision cube above stops items dropping in.
            const BlockState aboveState = level.GetBlockState(above.x, above.y, above.z);
            const bool isBlocked = aboveState.Block() != BlockID::Air &&
                                   BlockRegistry::HasCollision(aboveState.Block()) &&
                                   BlockRegistry::GetBlockCollisionShapeSet(aboveState).IsFullCube();
            if (isBlocked) return false;
            EntityLevel* entities = level.Entities();
            if (!entities) return false;
            std::vector<EntityLevel::NearbyItemEntity> items;
            entities->GetItemEntitiesInBox(SuckBox(pos), items);
            for (const auto& item : items) {
                if (AddItemEntity(level, hopper, item.id)) return true;
            }
            return false;
        }

    } // namespace

    // MC tryMoveItems, with the action inlined as a flag.
    static bool TryMoveItems(ILevelWrite& level, const glm::ivec3& pos, BlockState state, HopperBlockEntity& entity,
                             bool (*action)(ILevelWrite&, const glm::ivec3&, HopperBlockEntity&, int32_t),
                             int32_t actionArg) {
        if (level.IsClientSide()) return false;
        if (entity.IsOnCooldown() || !BoolOf(state, PropertyId::ENABLED)) return false;
        bool changed = false;
        if (!entity.IsEmpty()) changed = EjectItems(level, pos, entity, HopperFacingOf(state));
        if (!entity.InventoryFull()) changed |= action(level, pos, entity, actionArg);
        if (changed) {
            entity.SetCooldown(HopperBlockEntity::kMoveItemSpeed);
            entity.SetChanged();
            return true;
        }
        return false;
    }

    void HopperBlockEntity::Tick(World* world, float /*deltaTime*/) {
        if (!world) return;
        --m_cooldownTime;
        m_tickedGameTime = world->GetGameTime();
        if (IsOnCooldown()) return;
        SetCooldown(0);
        const glm::ivec3 pos = GetWorldPos();
        const BlockState state = world->GetBlockState(pos.x, pos.y, pos.z);
        if (!state.Is(BlockID::Hopper)) return;
        TryMoveItems(*world, pos, state, *this,
                     [](ILevelWrite& l, const glm::ivec3& p, HopperBlockEntity& h, int32_t) {
                         return SuckInItems(l, p, h);
                     }, 0);
    }

    void HopperBlockEntity::ItemInside(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
        EntityLevel* entities = level.Entities();
        if (!entities) return;
        std::vector<EntityLevel::NearbyItemEntity> items;
        entities->GetItemEntitiesInBox(SuckBox(pos), items);
        for (const auto& item : items) {
            TryMoveItems(level, pos, state, *this,
                         [](ILevelWrite& l, const glm::ivec3&, HopperBlockEntity& h, int32_t id) {
                             return AddItemEntity(l, h, id);
                         }, item.id);
        }
    }

    void HopperBlockEntity::Save(Network::PacketBuffer& out) const {
        BaseContainerBlockEntity::Save(out);
        out.WriteVarInt(static_cast<uint32_t>(m_cooldownTime + 1));   // -1 → 0
    }

    void HopperBlockEntity::Load(Network::PacketReader& in) {
        BaseContainerBlockEntity::Load(in);
        if (in.HasMore()) m_cooldownTime = static_cast<int>(in.ReadVarInt()) - 1;
    }

} // namespace Game
