// File: src/common/network/packets/game/TitlesS2CPacket.hpp
//
// MC's four title packets folded into one: ClientboundSetTitleTextPacket,
// ClientboundSetSubtitleTextPacket, ClientboundSetTitlesAnimationPacket and
// ClientboundClearTitlesPacket(resetTimes). What /title sends; the client
// hands each to its title overlay (Hud.setTitle / setSubtitle / setTimes /
// clearTitles + resetTitleTimes — client/renderer/gui/TitleOverlay).
//
// The action bar is NOT here: it already travels as a ChatMessageS2C at
// position 2 (MC ClientboundSetActionBarTextPacket / the overlay half of
// ClientboundSystemChatPacket).
//
// The text is the full component (common/text/TextComponent's wire codec),
// already resolved on the server against the target player
// (ComponentUtils.resolve with the player as the entity override).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/text/TextComponent.hpp"

#include <cstdint>
#include <vector>

namespace Network {

    struct TitlesS2CPacket {
        enum class Action : uint8_t {
            Title    = 0,   // ClientboundSetTitleTextPacket
            Subtitle = 1,   // ClientboundSetSubtitleTextPacket
            Times    = 2,   // ClientboundSetTitlesAnimationPacket
            Clear    = 3,   // ClientboundClearTitlesPacket(false)
            Reset    = 4,   // ClientboundClearTitlesPacket(true)
        };

        Action                action = Action::Title;
        Game::Text::Component text;          // Title / Subtitle
        // Times, in ticks. MC Hud.setTimes ignores a negative value (keeps
        // the current one); /title always sends three non-negative ones.
        int32_t               fadeIn  = 10;
        int32_t               stay    = 70;
        int32_t               fadeOut = 20;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const TitlesS2CPacket& p) {
            PacketBuffer b;
            b.WriteByte(static_cast<uint8_t>(p.action));
            switch (p.action) {
                case TitlesS2CPacket::Action::Title:
                case TitlesS2CPacket::Action::Subtitle:
                    Game::Text::Write(b, p.text);
                    break;
                case TitlesS2CPacket::Action::Times:
                    b.WriteInt(static_cast<uint32_t>(p.fadeIn));
                    b.WriteInt(static_cast<uint32_t>(p.stay));
                    b.WriteInt(static_cast<uint32_t>(p.fadeOut));
                    break;
                case TitlesS2CPacket::Action::Clear:
                case TitlesS2CPacket::Action::Reset:
                    break;
            }
            return b.GetData();
        }

        // Throws (Text::Read / PacketReader) on a malformed payload, which
        // the client's decoder reports and drops like any other.
        inline TitlesS2CPacket DeserializeTitlesS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            TitlesS2CPacket p;
            const uint8_t action = r.ReadByte();
            p.action = action <= static_cast<uint8_t>(TitlesS2CPacket::Action::Reset)
                ? static_cast<TitlesS2CPacket::Action>(action) : TitlesS2CPacket::Action::Clear;
            switch (p.action) {
                case TitlesS2CPacket::Action::Title:
                case TitlesS2CPacket::Action::Subtitle:
                    p.text = Game::Text::Read(r);
                    break;
                case TitlesS2CPacket::Action::Times:
                    p.fadeIn  = static_cast<int32_t>(r.ReadInt());
                    p.stay    = static_cast<int32_t>(r.ReadInt());
                    p.fadeOut = static_cast<int32_t>(r.ReadInt());
                    break;
                case TitlesS2CPacket::Action::Clear:
                case TitlesS2CPacket::Action::Reset:
                    break;
            }
            return p;
        }

    } // namespace Serialization

} // namespace Network
