// File: src/common/network/packets/game/InventoryCloseC2SPacket.hpp
//
// Mirrors MC ServerboundContainerClosePacket — empty marker telling the
// server the player closed the inventory screen (so the server drops any
// carried/cursor item).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <vector>

namespace Network {

    struct InventoryCloseC2SPacket {};

    namespace Serialization {

        // Empty payload — both directions are no-ops.
        inline std::vector<uint8_t> Serialize(const InventoryCloseC2SPacket&) { return {}; }
        inline InventoryCloseC2SPacket DeserializeInventoryCloseC2S(const std::vector<uint8_t>&) { return {}; }

    } // namespace Serialization

} // namespace Network
