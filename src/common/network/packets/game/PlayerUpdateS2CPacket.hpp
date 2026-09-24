// File: src/common/network/packets/game/PlayerUpdateS2CPacket.hpp
//
// Position broadcast for OTHER players (multiplayer). Sent by server when a
// remote player moves; the local client uses it to interpolate the remote
// player's position+yaw+pitch and animate accordingly.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/MobEffectPackets.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Network {

    struct PlayerUpdateS2CPacket {
        uint32_t  playerId;
        // Double on the wire, as MC's ClientboundPlayerPositionPacket and
        // entity spawn packets are: a float position at x = 300,000 sits
        // on a 3 cm grid, and this packet is how a teleport places the
        // local player.
        glm::dvec3 position{0.0};
        glm::vec2 rotation;     // yaw, pitch (head look direction)
        bool      isCrouching = false;
        // MC LivingEntity.hurtTime, counting down from 10. Rides the position
        // broadcast rather than an entity event because that broadcast already
        // runs every tick for every player — a separate event would be a second
        // packet carrying one byte.
        uint8_t   hurtTime = 0;
        // MC LivingEntity.deathTime, 0..20 — drives the corpse's topple
        // (LivingEntityRenderer.setupRotations). Rides here for the same reason
        // hurtTime does: it is per-tick state every watcher needs and the
        // position broadcast is already going out.
        uint8_t   deathTime = 0;
        uint32_t  sequenceNumber;
        // The dimension the player stands in. In the packet, not the stream
        // scope, because this broadcast goes to everyone and a scope switch
        // would make every client build a level for a dimension it may never
        // look into.
        int8_t    dimensionId = 0;
        // The player's body size (/scale, scaled portals). Trailing: an
        // older server sends none and the reader takes 1.
        float     scale = 1.0f;
        // /invisible: the receiver draws neither body nor name tag.
        // Trailing and optional on the wire; absent = visible.
        bool      invisible = false;
        // /morph: what the receiver draws instead of the player
        // (Game::Morph code: kind + id), 0xFFFFFFFF = none. Trailing.
        uint32_t  morph = 0xFFFFFFFFu;
        // The morph's animation byte the player reported (creeper swell).
        uint8_t   morphAnim = 0;
        // The player's synched effect visuals (Game::EffectVisuals — MC
        // DATA_EFFECT_PARTICLES + the glowing flag; INVISIBILITY rides
        // `invisible` above as well). Trailing; absent = no effects.
        uint8_t              effectFlags = 0;
        std::vector<uint8_t> effectParticles;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PlayerUpdateS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteVarInt(packet.playerId);
            buffer.WriteDouble(packet.position.x);
            buffer.WriteDouble(packet.position.y);
            buffer.WriteDouble(packet.position.z);
            buffer.WriteFloat(packet.rotation.x);
            buffer.WriteFloat(packet.rotation.y);
            buffer.WriteByte(packet.isCrouching ? 1 : 0);
            buffer.WriteByte(packet.hurtTime);
            buffer.WriteByte(packet.deathTime);
            buffer.WriteVarInt(packet.sequenceNumber);
            buffer.WriteByte(static_cast<uint8_t>(packet.dimensionId));
            buffer.WriteFloat(packet.scale);
            buffer.WriteByte(packet.invisible ? 1 : 0);
            buffer.WriteVarInt(packet.morph);
            buffer.WriteByte(packet.morphAnim);
            WriteEffectVisuals(buffer, packet.effectFlags, packet.effectParticles);
            return buffer.GetData();
        }

        inline PlayerUpdateS2CPacket DeserializePlayerUpdateS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            PlayerUpdateS2CPacket packet;
            packet.playerId = reader.ReadVarInt();
            packet.position.x = reader.ReadDouble();
            packet.position.y = reader.ReadDouble();
            packet.position.z = reader.ReadDouble();
            packet.rotation.x = reader.ReadFloat();
            packet.rotation.y = reader.ReadFloat();
            packet.isCrouching = reader.ReadByte() != 0;
            packet.hurtTime = reader.ReadByte();
            packet.deathTime = reader.ReadByte();
            packet.sequenceNumber = reader.ReadVarInt();
            packet.dimensionId = reader.HasMore() ? static_cast<int8_t>(reader.ReadByte()) : 0;
            packet.scale = reader.HasMore() ? reader.ReadFloat() : 1.0f;
            packet.invisible = reader.HasMore() ? (reader.ReadByte() != 0) : false;
            packet.morph = reader.HasMore() ? reader.ReadVarInt() : 0xFFFFFFFFu;
            packet.morphAnim = reader.HasMore() ? reader.ReadByte() : 0;
            ReadEffectVisuals(reader, packet.effectFlags, packet.effectParticles);
            return packet;
        }

    } // namespace Serialization

} // namespace Network
