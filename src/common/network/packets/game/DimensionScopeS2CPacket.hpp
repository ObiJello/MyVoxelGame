// File: src/common/network/packets/game/DimensionScopeS2CPacket.hpp
//
// Server → client: "everything that follows belongs to dimension D".
//
// WHY A SCOPE PACKET AND NOT A FIELD ON EVERY PACKET
// -------------------------------------------------
// A client that can see through portals holds more than one dimension at a
// time, so chunk, block-change and entity packets need to say which world
// they are for. The Immersive Portals mod wraps each such packet in a
// carrier that names the dimension. This engine does the equivalent with
// one byte of STREAM STATE instead: the connection is an ordered stream and
// the client applies packets in order, so a marker that switches the current
// dimension, sent only when the dimension of the next packet differs from
// the last one, tags every world-scoped packet at almost no cost and without
// touching thirty packet layouts.
//
// The server side is ServerConnection::SendPacketIn(dimension, ...), which
// emits this marker on a change of scope. The client side is
// ClientLevels::SetPacketDimension, read by ClientConnection before every
// apply. Packets that are not world-scoped (chat, inventory, health…) are
// unaffected by the current scope.
//
// Initial scope on both sides is the overworld; ChangeDimensionS2C resets it
// to the dimension the player arrives in.
//
// Wire layout:
//   int8  dimensionId   — Game::DimensionId raw value
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/world/level/DimensionId.hpp"

#include <cstdint>
#include <vector>

namespace Network {

    struct DimensionScopeS2CPacket {
        int8_t dimensionId = 0;

        Game::DimensionId Dimension() const { return Game::DimensionFromRaw(dimensionId); }
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const DimensionScopeS2CPacket& p) {
            PacketBuffer b;
            b.WriteByte(static_cast<uint8_t>(p.dimensionId));
            return b.GetData();
        }

        inline DimensionScopeS2CPacket DeserializeDimensionScopeS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            DimensionScopeS2CPacket p;
            p.dimensionId = static_cast<int8_t>(r.ReadByte());
            return p;
        }

    } // namespace Serialization

} // namespace Network
