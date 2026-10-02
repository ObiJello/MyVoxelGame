// File: src/server/advancements/AdvancementText.hpp
//
// The chat side of advancements: a Component turned into the styled runs
// ChatMessageS2C carries (colour and hover per run), the completion
// announcement (MC AdvancementType.createAnnouncement through
// PlayerList.broadcastSystemMessage) and /advancement's feedback lines.
#pragma once

#include "common/network/packets/game/ChatMessageS2CPacket.hpp"
#include "common/text/TextComponent.hpp"

#include <vector>

namespace Game::Advancements { struct Definition; }

namespace Server {
    class ServerPlayer;
    class ServerConnection;
}

namespace Server::Advancements {

    // Component.visit → one segment per styled run: the run's colour
    // (white when unstyled) and its show_text hover as plain text.
    std::vector<Network::ChatSegmentData> ToChatSegments(const Game::Text::Component& component);

    // The system-chat packet for a component (position 1, sender 0).
    Network::ChatMessageS2CPacket SystemMessage(const Game::Text::Component& component);

    // "chat.type.advancement.<frame>" with the player's name and the
    // advancement's bracketed name, to every connected player.
    void BroadcastAnnouncement(const ServerPlayer& player, const Game::Advancements::Definition& advancement);

} // namespace Server::Advancements
