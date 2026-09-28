// File: src/common/network/packets/game/VehiclePackets.hpp
//
// The riding and vehicle packets — MC's
//
//   SetPassengersS2C  ClientboundSetPassengersPacket: a vehicle's whole
//                     passenger list in seat order. A player appears by its
//                     PLAYER id (players are no client entities), a mob by
//                     its entity id; the client orders its seats by it (a
//                     boat's front and back seat) and learns which vehicle its
//                     own player drives.
//   MoveVehicleS2C    ClientboundMoveVehiclePacket: the server's correction
//                     of the vehicle the receiving client drives.
//   VehicleDataS2C    the vehicles' synched entity data (VehicleEntity's
//                     hurt shake, a boat's paddles and bubble time, a
//                     minecart's displayed block, a furnace cart's fuel) —
//                     what MC's ClientboundSetEntityDataPacket carries for
//                     them; the engine's SetEntityDataS2C is Mob-shaped.
//   PlayerInputC2S    ServerboundPlayerInputPacket: the movement keys, sent
//                     on change (a minecart's move intent, the shift that
//                     dismounts).
//   MoveVehicleC2S    ServerboundMoveVehiclePacket: where the driving client
//                     moved its vehicle this tick.
//   PaddleBoatC2S     ServerboundPaddleBoatPacket: which paddles row.
#pragma once

#include "common/network/PacketRegistry.hpp"

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Network {

    struct SetPassengersS2CPacket {
        int32_t              vehicleId = 0;
        std::vector<int32_t> passengerIds;   // seat order; a player by its player id
    };

    struct MoveVehicleS2CPacket {
        glm::dvec3 position{0.0};
        float      yRot = 0.0f;
        float      xRot = 0.0f;
    };

    struct VehicleDataS2CPacket {
        int32_t  entityId = 0;
        int32_t  hurtTime = 0;
        int32_t  hurtDir  = 1;
        float    damage   = 0.0f;
        bool     paddleLeft  = false;
        bool     paddleRight = false;
        bool     paddleReverse = false;   // back-paddling (flags bit 0x10)
        int32_t  bubbleTime  = 0;
        bool     hasCustomDisplay = false;
        uint32_t customDisplay    = 0;       // BlockState raw id
        int32_t  displayOffset    = 0;
        bool     hasFuel          = false;
    };

    struct PlayerInputC2SPacket {
        uint8_t keys = 0;   // Game::PlayerInput::Pack()
    };

    struct MoveVehicleC2SPacket {
        glm::dvec3 position{0.0};
        float      yRot = 0.0f;
        float      xRot = 0.0f;
        bool       onGround = false;
    };

    struct PaddleBoatC2SPacket {
        bool left  = false;
        bool right = false;
        bool reverse = false;   // back-paddling (bit 2 of the byte)
    };

    // MC ServerboundPlayerCommandPacket's riding actions: the jump bar
    // released on a jumpable mount (START_RIDING_JUMP, data = the charge
    // ×100) and STOP_RIDING_JUMP.
    enum class RidingCommand : uint8_t { StartRidingJump = 0, StopRidingJump = 1 };
    struct RidingCommandC2SPacket {
        RidingCommand action = RidingCommand::StartRidingJump;
        int32_t       data = 0;
    };

    // MC ClientboundMountScreenOpenPacket: open HorseInventoryScreen (and
    // its kin) for the mount `entityId`, as container `containerId` with
    // `inventoryColumns` columns of chest slots (0 for none).
    struct MountScreenOpenS2CPacket {
        uint8_t containerId = 0;
        int32_t inventoryColumns = 0;
        int32_t entityId = 0;
        // Trailing: the menu's full container id. This engine's ids count up
        // without MC's `% 100` wrap, so the byte above alone would stop
        // matching after 255 opens; the client uses this one when present.
        uint32_t fullContainerId = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const SetPassengersS2CPacket& p) {
            PacketBuffer b;
            b.WriteInt(static_cast<uint32_t>(p.vehicleId));
            b.WriteVarInt(static_cast<uint32_t>(p.passengerIds.size()));
            for (int32_t id : p.passengerIds) b.WriteInt(static_cast<uint32_t>(id));
            return b.GetData();
        }
        inline SetPassengersS2CPacket DeserializeSetPassengersS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            SetPassengersS2CPacket p;
            p.vehicleId = static_cast<int32_t>(r.ReadInt());
            const uint32_t n = r.ReadVarInt();
            // A vehicle seats a handful at most; a corrupt count must not
            // allocate the world.
            const uint32_t capped = n > 64u ? 64u : n;
            p.passengerIds.reserve(capped);
            for (uint32_t i = 0; i < n && r.Remaining() >= 4; ++i) {
                const auto id = static_cast<int32_t>(r.ReadInt());
                if (i < capped) p.passengerIds.push_back(id);
            }
            return p;
        }

        inline std::vector<uint8_t> Serialize(const MoveVehicleS2CPacket& p) {
            PacketBuffer b;
            b.WriteDouble(p.position.x);
            b.WriteDouble(p.position.y);
            b.WriteDouble(p.position.z);
            b.WriteFloat(p.yRot);
            b.WriteFloat(p.xRot);
            return b.GetData();
        }
        inline MoveVehicleS2CPacket DeserializeMoveVehicleS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            MoveVehicleS2CPacket p;
            p.position.x = r.ReadDouble();
            p.position.y = r.ReadDouble();
            p.position.z = r.ReadDouble();
            p.yRot = r.ReadFloat();
            p.xRot = r.ReadFloat();
            return p;
        }

        inline std::vector<uint8_t> Serialize(const VehicleDataS2CPacket& p) {
            PacketBuffer b;
            b.WriteInt(static_cast<uint32_t>(p.entityId));
            b.WriteVarInt(static_cast<uint32_t>(p.hurtTime < 0 ? 0 : p.hurtTime));
            b.WriteByte(static_cast<uint8_t>(p.hurtDir < 0 ? 0 : 1));
            b.WriteFloat(p.damage);
            uint8_t flags = 0;
            if (p.paddleLeft)       flags |= 0x01;
            if (p.paddleRight)      flags |= 0x02;
            if (p.hasCustomDisplay) flags |= 0x04;
            if (p.hasFuel)          flags |= 0x08;
            if (p.paddleReverse)    flags |= 0x10;
            b.WriteByte(flags);
            b.WriteVarInt(static_cast<uint32_t>(p.bubbleTime < 0 ? 0 : p.bubbleTime));
            b.WriteInt(p.customDisplay);
            b.WriteInt(static_cast<uint32_t>(p.displayOffset));
            return b.GetData();
        }
        inline VehicleDataS2CPacket DeserializeVehicleDataS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            VehicleDataS2CPacket p;
            p.entityId = static_cast<int32_t>(r.ReadInt());
            p.hurtTime = static_cast<int32_t>(r.ReadVarInt());
            p.hurtDir  = r.ReadByte() != 0 ? 1 : -1;
            p.damage   = r.ReadFloat();
            const uint8_t flags = r.ReadByte();
            p.paddleLeft       = (flags & 0x01) != 0;
            p.paddleRight      = (flags & 0x02) != 0;
            p.hasCustomDisplay = (flags & 0x04) != 0;
            p.hasFuel          = (flags & 0x08) != 0;
            p.paddleReverse    = (flags & 0x10) != 0;
            p.bubbleTime    = static_cast<int32_t>(r.ReadVarInt());
            p.customDisplay = r.ReadInt();
            p.displayOffset = static_cast<int32_t>(r.ReadInt());
            return p;
        }

        inline std::vector<uint8_t> Serialize(const PlayerInputC2SPacket& p) {
            PacketBuffer b;
            b.WriteByte(p.keys);
            return b.GetData();
        }
        inline PlayerInputC2SPacket DeserializePlayerInputC2S(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            PlayerInputC2SPacket p;
            p.keys = r.ReadByte();
            return p;
        }

        inline std::vector<uint8_t> Serialize(const MoveVehicleC2SPacket& p) {
            PacketBuffer b;
            b.WriteDouble(p.position.x);
            b.WriteDouble(p.position.y);
            b.WriteDouble(p.position.z);
            b.WriteFloat(p.yRot);
            b.WriteFloat(p.xRot);
            b.WriteByte(p.onGround ? 1 : 0);
            return b.GetData();
        }
        inline MoveVehicleC2SPacket DeserializeMoveVehicleC2S(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            MoveVehicleC2SPacket p;
            p.position.x = r.ReadDouble();
            p.position.y = r.ReadDouble();
            p.position.z = r.ReadDouble();
            p.yRot = r.ReadFloat();
            p.xRot = r.ReadFloat();
            p.onGround = r.ReadByte() != 0;
            return p;
        }

        inline std::vector<uint8_t> Serialize(const PaddleBoatC2SPacket& p) {
            PacketBuffer b;
            b.WriteByte(static_cast<uint8_t>((p.left ? 1 : 0) | (p.right ? 2 : 0) | (p.reverse ? 4 : 0)));
            return b.GetData();
        }
        inline PaddleBoatC2SPacket DeserializePaddleBoatC2S(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            PaddleBoatC2SPacket p;
            const uint8_t b = r.ReadByte();
            p.left  = (b & 1) != 0;
            p.right = (b & 2) != 0;
            p.reverse = (b & 4) != 0;
            return p;
        }

        inline std::vector<uint8_t> Serialize(const RidingCommandC2SPacket& p) {
            PacketBuffer b;
            b.WriteByte(static_cast<uint8_t>(p.action));
            b.WriteVarInt(static_cast<uint32_t>(p.data < 0 ? 0 : p.data));
            return b.GetData();
        }
        inline RidingCommandC2SPacket DeserializeRidingCommandC2S(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            RidingCommandC2SPacket p;
            const uint8_t action = r.ReadByte();
            p.action = action == 1 ? RidingCommand::StopRidingJump : RidingCommand::StartRidingJump;
            p.data = static_cast<int32_t>(r.ReadVarInt());
            return p;
        }

        inline std::vector<uint8_t> Serialize(const MountScreenOpenS2CPacket& p) {
            PacketBuffer b;
            b.WriteByte(p.containerId);
            b.WriteVarInt(static_cast<uint32_t>(p.inventoryColumns < 0 ? 0 : p.inventoryColumns));
            b.WriteInt(static_cast<uint32_t>(p.entityId));
            b.WriteVarInt(p.fullContainerId);
            return b.GetData();
        }
        inline MountScreenOpenS2CPacket DeserializeMountScreenOpenS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            MountScreenOpenS2CPacket p;
            p.containerId = r.ReadByte();
            p.inventoryColumns = static_cast<int32_t>(r.ReadVarInt());
            p.entityId = static_cast<int32_t>(r.ReadInt());
            p.fullContainerId = r.Remaining() > 0 ? r.ReadVarInt() : p.containerId;
            return p;
        }

    } // namespace Serialization

} // namespace Network
