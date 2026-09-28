// File: src/common/network/packets/game/SpectatorPackets.hpp
//
// Spectator mode's three packets:
//
//   SetCameraS2C          MC ClientboundSetCameraPacket — the entity whose
//                         eyes the client renders from (ServerPlayer.setCamera).
//                         Wire: VarInt-free int32 entityId. The player's OWN
//                         id means "back to your own body". Player ids are
//                         connection ids, mob ids live above
//                         Game::kMobEntityIdBase, so one number names either.
//   SpectatorActionC2S    MC ServerboundSpectatorActionPacket(OptionalInt): a
//                         spectator's attack click — on an entity, "spectate
//                         it"; on nothing, the empty action the server only
//                         notes as activity (MultiPlayerGameMode.spectate /
//                         spectatorNoAction). Wire: byte hasEntity, int32 id.
//   TeleportToEntityC2S   MC ServerboundTeleportToEntityPacket(uuid): the
//                         spectator menu's "teleport to player". Players have
//                         no UUIDs on this wire; their id is the key, as it is
//                         for every other player packet. Wire: VarInt playerId.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct SetCameraS2CPacket {
        int32_t entityId = 0;
    };

    struct SpectatorActionC2SPacket {
        bool    hasEntity = false;
        int32_t entityId  = 0;
    };

    struct TeleportToEntityC2SPacket {
        uint32_t playerId = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const SetCameraS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteInt(static_cast<uint32_t>(packet.entityId));
            return buffer.GetData();
        }

        inline SetCameraS2CPacket DeserializeSetCameraS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            SetCameraS2CPacket packet;
            packet.entityId = static_cast<int32_t>(reader.ReadInt());
            return packet;
        }

        inline std::vector<uint8_t> Serialize(const SpectatorActionC2SPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteByte(packet.hasEntity ? 1 : 0);
            buffer.WriteInt(static_cast<uint32_t>(packet.entityId));
            return buffer.GetData();
        }

        inline SpectatorActionC2SPacket DeserializeSpectatorActionC2S(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            SpectatorActionC2SPacket packet;
            packet.hasEntity = reader.ReadByte() != 0;
            packet.entityId  = static_cast<int32_t>(reader.ReadInt());
            return packet;
        }

        inline std::vector<uint8_t> Serialize(const TeleportToEntityC2SPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(packet.playerId);
            return buffer.GetData();
        }

        inline TeleportToEntityC2SPacket DeserializeTeleportToEntityC2S(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            TeleportToEntityC2SPacket packet;
            packet.playerId = reader.ReadVarInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network
