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
        // MC Entity.isSprinting (DATA_SHARED_FLAGS bit 3): the receiver kicks
        // up the sprint dust under this player (Entity.spawnSprintParticle).
        // Trailing; absent = not sprinting.
        bool                 sprinting = false;
        // MC AvatarRenderer.getArmPose for both arms (PlayerArmPose.hpp
        // ordinals) and the item-use state HumanoidModel reads (isUsingItem,
        // useItemHand, ticksUsingItem) — a morphed player's humanoid body is
        // posed from them on every other client.
        uint8_t   rightArmPose = 0;
        uint8_t   leftArmPose  = 0;
        bool      usingItem    = false;
        uint8_t   useItemHand  = 0;
        uint32_t  ticksUsingItem = 0;
        // MC LivingEntity.isFallFlying (shared flag 7): the player glides on
        // an elytra — a firework rocket attached to them rides at their hand.
        // Trailing; absent = not gliding.
        bool      fallFlying = false;
        // The chest slot's elytra, for MC's WingsLayer on every other client:
        // bit 0 an elytra is worn (an EQUIPPABLE chest item with the elytra
        // asset), bit 1 it has the enchantment glint (ItemStack.hasFoil).
        // Trailing; absent = none.
        uint8_t   elytraFlags = 0;
        // HumanoidRenderState.maxCrossbowChargeDuration: the used crossbow's
        // charge time in ticks (Quick Charge shortens it). Trailing; absent =
        // 25 (1.25 s).
        uint8_t   maxCrossbowCharge = 25;
        // MC LivingEntity.isAutoSpinAttack (DATA_LIVING_ENTITY_FLAGS bit 4):
        // a riptide in flight — the spinning body and its swirl. Trailing;
        // absent = not spinning.
        bool      autoSpinAttack = false;
        // /morph: the morph's look beyond the code (Game::Morph
        // DefaultVariantOf — a tropical fish's packed variant, a salmon's
        // size). Trailing; absent = the type's default.
        int32_t   morphVariant = 0;
        bool      hasMorphVariant = false;   // read side: the field was on the wire
        // MC Attributes.SCALE (LivingEntity.getScale) — the body's size from
        // the player's attribute (worn items' modifiers), apart from `scale`
        // (the portal size that also scales the motion the receiver
        // extrapolates). The receiver draws / boxes the body at
        // scale × attributeScale. Trailing; absent = 1.
        float     attributeScale = 1.0f;
        // MC Attributes.NAME_TAG_DISTANCE: how far away this player's name
        // tag still shows (LivingEntityRenderer.extractNameTags). Trailing;
        // absent = 64.
        float     nameTagDistance = 64.0f;
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
            buffer.WriteByte(packet.sprinting ? 1 : 0);
            buffer.WriteByte(packet.rightArmPose);
            buffer.WriteByte(packet.leftArmPose);
            buffer.WriteByte(static_cast<uint8_t>((packet.usingItem ? 0x01 : 0) | (packet.useItemHand ? 0x02 : 0)));
            buffer.WriteVarInt(packet.ticksUsingItem);
            buffer.WriteByte(packet.fallFlying ? 1 : 0);
            buffer.WriteByte(packet.elytraFlags);
            buffer.WriteByte(packet.maxCrossbowCharge);
            buffer.WriteByte(packet.autoSpinAttack ? 1 : 0);
            buffer.WriteVarInt(static_cast<uint32_t>(packet.morphVariant));
            buffer.WriteFloat(packet.attributeScale);
            buffer.WriteFloat(packet.nameTagDistance);
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
            packet.sprinting = reader.HasMore() ? (reader.ReadByte() != 0) : false;
            packet.rightArmPose = reader.HasMore() ? reader.ReadByte() : 0;
            packet.leftArmPose  = reader.HasMore() ? reader.ReadByte() : 0;
            if (reader.HasMore()) {
                const uint8_t use = reader.ReadByte();
                packet.usingItem   = (use & 0x01) != 0;
                packet.useItemHand = (use & 0x02) ? 1 : 0;
            }
            packet.ticksUsingItem = reader.HasMore() ? reader.ReadVarInt() : 0;
            packet.fallFlying = reader.HasMore() ? (reader.ReadByte() != 0) : false;
            packet.elytraFlags = reader.HasMore() ? reader.ReadByte() : 0;
            packet.maxCrossbowCharge = reader.HasMore() ? reader.ReadByte() : 25;
            packet.autoSpinAttack = reader.HasMore() ? (reader.ReadByte() != 0) : false;
            packet.hasMorphVariant = reader.HasMore();
            packet.morphVariant = packet.hasMorphVariant
                ? static_cast<int32_t>(reader.ReadVarInt()) : 0;
            packet.attributeScale  = reader.HasMore() ? reader.ReadFloat() : 1.0f;
            packet.nameTagDistance = reader.HasMore() ? reader.ReadFloat() : 64.0f;
            return packet;
        }

    } // namespace Serialization

} // namespace Network
