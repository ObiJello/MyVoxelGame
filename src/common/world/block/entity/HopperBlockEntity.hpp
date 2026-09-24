// File: src/common/world/block/entity/HopperBlockEntity.hpp
//
// MC HopperBlockEntity — five slots, an 8-tick transfer cooldown, and the
// per-tick pushItemsTick that ejects one item into the container it faces
// and pulls one from the container (or the dropped items) above.
#pragma once

#include "BaseContainerBlockEntity.hpp"
#include "common/world/block/Direction.hpp"

namespace Game {

    class IContainer;

    class HopperBlockEntity : public BaseContainerBlockEntity {
    public:
        static constexpr int kMoveItemSpeed     = 8;
        static constexpr int kContainerSize     = 5;
        static constexpr int kNoCooldownTime    = -1;

        HopperBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BaseContainerBlockEntity(type, worldPos, blockId, kContainerSize) {}

        bool NeedsTicking() const override { return true; }
        void Tick(World* world, float deltaTime) override;

        // MC HopperBlockEntity.setItem: clamp to the slot's ceiling.
        void SetItem(int index, const ItemStack& stack) override;

        // MC HopperBlockEntity.entityInside → tryMoveItems(addItem(entity)):
        // a dropped item that lands in the funnel is taken at once.
        void ItemInside(ILevelWrite& level, const glm::ivec3& pos, BlockState state);

        int  CooldownTime() const { return m_cooldownTime; }
        void SetCooldownTime(int t) { m_cooldownTime = t; }

        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

        // MC HopperBlockEntity.addItem(from, container, stack, direction) —
        // the shared insert every container-to-container move uses; public
        // because the dropper's insert goes through it too. Returns what
        // could not be placed.
        static ItemStack AddItem(IContainer* from, IContainer& container, ItemStack stack,
                                 Direction direction, bool hasDirection);

        // MC HopperBlockEntity.getContainerAt(level, pos): the container at
        // a cell — a block entity that is one (a double chest as its joined
        // pair). `owned` keeps a compound alive for the caller's scope.
        static IContainer* GetContainerAt(ILevelWrite& level, const glm::ivec3& pos,
                                          std::unique_ptr<IContainer>& owned);

        bool IsOnCooldown() const       { return m_cooldownTime > 0; }
        bool IsOnCustomCooldown() const { return m_cooldownTime > kMoveItemSpeed; }
        void SetCooldown(int time)      { m_cooldownTime = time; }
        bool InventoryFull() const;

    private:
        friend struct HopperTransfer;

        int     m_cooldownTime   = kNoCooldownTime;   // MC "TransferCooldown"
        int64_t m_tickedGameTime = 0;
    };

} // namespace Game
