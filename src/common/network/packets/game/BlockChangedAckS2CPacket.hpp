// File: src/common/network/packets/game/BlockChangedAckS2CPacket.hpp
//
// Server → client: "every interaction up to and including `sequence` has been
// processed; you may now retire the block predictions you made for them."
//
// Mirrors MC ClientboundBlockChangedAckPacket (a bare VarInt sequence). The
// ORDERING GUARANTEE is the whole point of this packet: the server must have
// already sent every block update caused by those interactions before this ack
// goes out. MC gets that by stashing the sequence during packet handling and
// flushing it at the top of the NEXT tick (ServerGamePacketListenerImpl.tick,
// line 284), by which point the chunk broadcast has run. We do the same —
// PlayerSession::AckBlockChangesUpTo accumulates, and IntegratedServer flushes
// it right after ChunkDeltaBroadcaster::flush().
//
// If that ordering is ever broken the client will retire a prediction before
// the correction arrives and the player sees a one-frame flicker back to the
// stale block.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct BlockChangedAckS2CPacket {
        uint32_t sequence = 0;

        BlockChangedAckS2CPacket() = default;
        explicit BlockChangedAckS2CPacket(uint32_t seq) : sequence(seq) {}
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const BlockChangedAckS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(packet.sequence);
            return buffer.GetData();
        }

        inline BlockChangedAckS2CPacket DeserializeBlockChangedAckS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            BlockChangedAckS2CPacket packet;
            packet.sequence = reader.ReadVarInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network
