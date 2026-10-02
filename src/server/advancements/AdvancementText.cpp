// File: src/server/advancements/AdvancementText.cpp
#include "AdvancementText.hpp"

#include "server/IntegratedServer.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include "common/advancements/Advancement.hpp"
#include "common/core/Log.hpp"

namespace Server::Advancements {

    std::vector<Network::ChatSegmentData> ToChatSegments(const Game::Text::Component& component) {
        std::vector<Network::ChatSegmentData> out;
        Game::Text::Visit(component, Game::Text::Style{}, [&out](const Game::Text::Style& style, std::string_view text) {
            if (text.empty()) return true;
            Network::ChatSegmentData segment;
            segment.text = std::string(text);
            segment.color = style.color ? (0xFF000000u | style.color->rgb) : 0xFFFFFFFFu;
            if (style.hoverText) segment.hoverText = Game::Text::GetString(*style.hoverText);
            // Merge runs that look the same (a translation's literal pieces).
            if (!out.empty() && out.back().color == segment.color && out.back().hoverText == segment.hoverText &&
                out.back().click == Network::ChatClickAction::None) {
                out.back().text += segment.text;
            } else {
                out.push_back(std::move(segment));
            }
            return true;
        });
        if (out.empty()) out.push_back(Network::ChatSegmentData{"", 0xFFFFFFFF, Network::ChatClickAction::None, "", ""});
        return out;
    }

    Network::ChatMessageS2CPacket SystemMessage(const Game::Text::Component& component) {
        Network::ChatMessageS2CPacket packet;
        packet.senderId = 0;
        packet.position = 1;   // system chat, as broadcastSystemMessage(…, false)
        packet.segments = ToChatSegments(component);
        return packet;
    }

    void BroadcastAnnouncement(const ServerPlayer& player, const Game::Advancements::Definition& advancement) {
        if (!advancement.display || !g_integratedServer) return;
        // AdvancementType.createAnnouncement: chat.type.advancement.<name>
        // with the player's display name and Advancement.name(holder).
        const Game::Text::Component message = Game::Text::Component::Translatable(
            std::string("chat.type.advancement.") + Game::Advancements::FrameName(advancement.display->type),
            {Game::Text::Component::Literal(player.getName()), advancement.Name()});
        const Network::ChatMessageS2CPacket packet = SystemMessage(message);
        if (auto* sessions = g_integratedServer->GetSessionManager()) {
            for (const auto& session : sessions->GetAllSessions()) {
                if (session && session->GetConnection()) session->GetConnection()->SendChatMessage(packet);
            }
        }
        Log::Info("[CHAT] %s", Game::Text::GetString(message).c_str());
    }

} // namespace Server::Advancements
