// File: src/common/network/packets/game/GameEventS2CPacket.hpp
//
// MC ClientboundGameEventPacket — a one-byte event and a float parameter —
// for the four events this engine sends, all of them weather:
//
//   1  START_RAINING          the client's rain level drops to 0 (the
//                             server's RAIN_LEVEL_CHANGE steps follow)
//   2  STOP_RAINING           the client's rain level goes to 1 (and eases
//                             out with the steps)
//   7  RAIN_LEVEL_CHANGE      param = the level's rain level
//   8  THUNDER_LEVEL_CHANGE   param = the level's thunder level
//   9  PUFFER_FISH_STING      to the stung player only (param 0); its
//                             client plays the sting sound at itself
//
// Sent by Server::ServerWeather: every tick a weather level's eased levels
// move (to the players in that dimension), on each start / stop, and — MC
// PlayerList.sendLevelInfo — to a player joining or arriving in a level
// where it is raining. Rides the id the registry always reserved for weather
// (PacketId::WeatherChange, 0x1A).
//
// Applied on the client main thread in packet order (a typed S2C packet),
// so a dimension change's reset of the client's weather (a new ClientLevel
// starts dry) can never overtake the new level's weather that follows it.
//
// Wire: byte event, float param. New fields go on the end.
#pragma once

#include "common/network/PacketRegistry.hpp"

#include <cstdint>
#include <vector>

namespace Network {

    struct GameEventS2CPacket {
        // MC ClientboundGameEventPacket.Type ids.
        static constexpr uint8_t kStartRaining       = 1;
        static constexpr uint8_t kStopRaining        = 2;
        static constexpr uint8_t kRainLevelChange    = 7;
        static constexpr uint8_t kThunderLevelChange = 8;
        // MC PUFFER_FISH_STING — sent by Pufferfish.playerTouch to the stung
        // player alone; its client plays PUFFER_FISH_STING at itself
        // (ClientPacketListener.handleGameEvent). Nobody else hears it.
        static constexpr uint8_t kPufferFishSting    = 9;

        uint8_t event = 0;
        float   param = 0.0f;

        GameEventS2CPacket() = default;
        GameEventS2CPacket(uint8_t e, float p) : event(e), param(p) {}
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const GameEventS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteByte(packet.event);
            buffer.WriteFloat(packet.param);
            return buffer.GetData();
        }

        inline GameEventS2CPacket DeserializeGameEventS2C(const std::vector<uint8_t>& data) {
            GameEventS2CPacket packet;
            if (data.empty()) return packet;
            PacketReader reader(data);
            packet.event = reader.ReadByte();
            if (reader.HasMore()) packet.param = reader.ReadFloat();
            return packet;
        }

    } // namespace Serialization

} // namespace Network
