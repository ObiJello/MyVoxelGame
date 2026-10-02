// File: src/server/commands/CommandText.hpp
//
// What the chat-facing commands (/say, /msg, /me, /tellraw, /title …) share:
//
//   • MC ComponentArgument.textComponent — a text component typed as SNBT
//     (JSON is a subset: `{"text":"hi","color":"red"}` parses the same as
//     `{text:"hi",color:red}`), read with the world save's component reader.
//   • MC ComponentUtils.resolve — a component's selector / nbt / score
//     contents filled in against a command source, with an entity override
//     for `@s` (/tellraw and /title resolve once per target, as the target).
//   • MC MessageArgument — free chat text whose `@` selectors become the
//     matched entities' names (only for a source allowed selectors).
//   • The engine's chat wire form: a resolved component as ChatSegmentData
//     runs (colour, click, hover; bold / italic / underline / strikethrough /
//     obfuscated as the font's '§' style codes, which the chat font draws),
//     or as one '§'-coded string for the action bar.
#pragma once

#include "CommandSourceStack.hpp"
#include "common/network/packets/game/ChatMessageS2CPacket.hpp"
#include "common/text/TextComponent.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Server {

    class ServerConnection;
    class PlayerSessionManager;

    namespace CommandText {

        // The tokens from `from` on, joined by single spaces — the greedy
        // tail a message or component argument reads.
        std::string Join(const std::vector<std::string>& args, size_t from);

        // MC ComponentArgument.parse. False with MC's message
        // ("Invalid chat component: …") on a bad component.
        bool ParseComponent(const std::string& text, Game::Text::Component& out, std::string& error);

        // MC ComponentUtils.resolve(ResolutionContext{source, entityOverride}).
        // `self` (may be null) replaces the source's entity — what `@s`
        // means inside the component. Score contents resolve to nothing (no
        // scoreboard); entity / block / storage nbt contents read the same
        // data /data does.
        Game::Text::Component Resolve(const Game::Text::Component& component,
                                      const CommandSourceStack& source,
                                      const SelectedEntity* self);

        // The resolved component as chat runs. Text with no colour takes
        // `baseColor`; `baseStyleCodes` ('§'-coded) go in front of every run
        // (the italic of a whisper).
        std::vector<Network::ChatSegmentData> ToSegments(const Game::Text::Component& component,
                                                        uint32_t baseColor = 0xFFFFFFFFu,
                                                        const std::string& baseStyleCodes = {});
        // The resolved component as one string with '§' colour and style
        // codes (the action bar draws a single string). A custom RGB colour
        // becomes the nearest of the sixteen named ones.
        std::string ToLegacyString(const Game::Text::Component& component);

        // MC Entity.getDisplayName: a player's name (clicking it suggests
        // "/tell <name> ", as MC's player display name does), a mob's custom
        // name, else its type's translated name.
        Game::Text::Component DisplayName(const SelectedEntity& entity);
        std::string DisplayNameString(const SelectedEntity& entity);

        // MC MessageArgument.resolveChatMessage: `text` with each `@p` /
        // `@a[...]` selector replaced by the names it matches (", "
        // between them, nothing when none). Only when `allowSelectors` —
        // MC resolves selectors in a message only for a source with the
        // entity-selector permission; otherwise the text is literal.
        std::string ResolveMessage(const std::string& text, const CommandSourceStack& source, bool allowSelectors);

        // MC's permission level 2 (LEVEL_GAMEMASTERS) as this engine has
        // it: cheats on for the world and, for a guest, command access.
        // The commands vanilla gives every player (msg, me, list, help,
        // random value/roll) check this for their privileged parts.
        bool HasGamemasterPermission(const ServerConnection& connection);

        // MC CommandSourceStack.sendFailure (red; no rule gates it) and a
        // system message from runs, to the command's sender. Success lines
        // go through CommandSourceStack::SendSuccess (send_command_feedback).
        void SendFailure(ServerConnection& connection, const std::string& text);
        void SendSystem(ServerConnection& connection, std::vector<Network::ChatSegmentData> segments);

        // MC PlayerList.broadcastSystemMessage: every connected player.
        void BroadcastSystem(PlayerSessionManager& sessions, const Network::ChatMessageS2CPacket& packet);

    } // namespace CommandText

} // namespace Server
