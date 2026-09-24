// File: src/common/world/block/entity/ComparatorBlockEntity.hpp
//
// MC ComparatorBlockEntity: one int, the comparator's last computed output.
// It lives in a block entity rather than the blockstate because 16 output
// levels × the existing 16 states would be 256 states for a block whose
// output is never rendered — vanilla made the same call.
#pragma once

#include "BlockEntity.hpp"
#include "common/network/PacketRegistry.hpp"

namespace Game {

    class ComparatorBlockEntity : public BlockEntity {
    public:
        ComparatorBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        int  GetOutputSignal() const { return m_output; }
        void SetOutputSignal(int value) {
            if (m_output == value) return;
            m_output = value;
            MarkDirty();
        }

        void Save(Network::PacketBuffer& out) const override {
            out.WriteVarInt(static_cast<uint32_t>(m_output));
        }
        void Load(Network::PacketReader& in) override {
            if (in.HasMore()) m_output = static_cast<int>(in.ReadVarInt());
        }

    private:
        int m_output = 0;   // MC "OutputSignal"
    };

} // namespace Game
