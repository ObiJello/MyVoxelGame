// File: src/common/network/packets/game/TickingPackets.hpp
//
// MC ClientboundTickingStatePacket + ClientboundTickingStepPacket.
//
// ── Why the client needs these at all ──────────────────────────────────────
//
// Both sides run their own 20 Hz loop. The server's is the authority for the
// world; the client's drives movement prediction, mining progress and — the
// reason these packets exist here — entity ANIMATION. Client-side mobs are
// real Game::Mob instances that tick, so `tickCount`, `attackAnim`, the walk
// animation and the idle arm bob all advance on the client's own clock.
//
// That is fine until `/tick freeze`, which stops the server's game elements
// but said nothing to the client, leaving every mob still swinging its arms
// in a world that had stopped. MC solves it by giving the CLIENT a
// TickRateManager too and mirroring the server's state into it with these two
// packets, then gating entity ticking on `isEntityFrozen`.
//
// Two packets, not one, because MC keeps them separate: the state changes on
// `/tick freeze|unfreeze|rate`, while `/tick step` only ever pushes a count of
// frozen ticks to run and must not disturb the rest of the state.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct TickingStateS2CPacket {
        float tickRate = 20.0f;
        bool  isFrozen = false;

        TickingStateS2CPacket() = default;
        TickingStateS2CPacket(float rate, bool frozen) : tickRate(rate), isFrozen(frozen) {}
    };

    struct TickingStepS2CPacket {
        // MC frozenTicksToRun — how many frozen ticks to run WITH game
        // elements. Counts down on the receiving side, one per tick.
        int32_t tickSteps = 0;

        TickingStepS2CPacket() = default;
        explicit TickingStepS2CPacket(int32_t steps) : tickSteps(steps) {}
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const TickingStateS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteFloat(packet.tickRate);
            buffer.WriteByte(packet.isFrozen ? 0x01 : 0x00);
            return buffer.GetData();
        }

        inline TickingStateS2CPacket DeserializeTickingStateS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            TickingStateS2CPacket packet;
            packet.tickRate = reader.ReadFloat();
            packet.isFrozen = reader.ReadByte() != 0;
            return packet;
        }

        inline std::vector<uint8_t> Serialize(const TickingStepS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteVarInt(packet.tickSteps);
            return buffer.GetData();
        }

        inline TickingStepS2CPacket DeserializeTickingStepS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            TickingStepS2CPacket packet;
            packet.tickSteps = reader.ReadVarInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network
