// File: src/common/world/block/entity/BlockEntityType.hpp
//
// One BlockEntityType per kind of BE (Chest, Sign, Banner, …). Holds:
//   - factory  : how to construct an instance (given world pos + block id)
//   - typeId   : the wire-stable uint16 sent over the network and persisted
//   - stringId : the human / save-format id ("chest", "sign", …) for debug + future Anvil compat
//   - validBlocks : which BlockID values this BE attaches to (a single BE class
//                   often backs multiple variants — ChestBlockEntity backs Chest,
//                   TrappedChest, EnderChest, …).
//
// Mirrors MC `BlockEntityType.java`.
#pragma once

#include "BlockEntity.hpp"   // std::function<unique_ptr<BlockEntity>(…)> needs the complete type to instantiate its destructor.
#include "../Blocks.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>

namespace Game {

    class BlockEntityType {
    public:
        using Factory = std::function<std::unique_ptr<BlockEntity>(
            const BlockEntityType*, glm::ivec3 worldPos, BlockID blockId)>;

        BlockEntityType(uint16_t typeId, std::string stringId,
                        Factory factory,
                        std::unordered_set<BlockID> validBlocks)
            : m_typeId(typeId)
            , m_stringId(std::move(stringId))
            , m_factory(std::move(factory))
            , m_validBlocks(std::move(validBlocks)) {}

        uint16_t           TypeId()   const { return m_typeId; }
        const std::string& StringId() const { return m_stringId; }

        bool IsValidFor(BlockID id) const { return m_validBlocks.count(id) > 0; }

        std::unique_ptr<BlockEntity> Create(glm::ivec3 worldPos, BlockID blockId) const {
            return m_factory(this, worldPos, blockId);
        }

    private:
        uint16_t                    m_typeId    = 0;
        std::string                 m_stringId;
        Factory                     m_factory;
        std::unordered_set<BlockID> m_validBlocks;
    };

} // namespace Game
