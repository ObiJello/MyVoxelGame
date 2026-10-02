// File: src/server/commands/ChatCommands.cpp
#include "ChatCommands.hpp"
#include "CommandText.hpp"
#include "EntitySelector.hpp"
#include "TimeCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/network/PacketTypes.hpp"

#include <cctype>
#include <string>
#include <vector>

namespace Server {

    namespace {

        using Segments = std::vector<Network::ChatSegmentData>;

        // MC CommandSourceStack.getDisplayName: the executing entity's name
        // ("Zombie" under `/execute as @e[type=zombie]`), the sender's
        // otherwise.
        std::string SourceName(const CommandSourceStack& source) {
            if (source.entity) return CommandText::DisplayNameString(*source.entity);
            return source.DisplayName();
        }

        ServerConnection* ConnectionOf(const SelectedEntity& player) {
            return player.session ? player.session->GetConnection() : nullptr;
        }

        // MC broadcastChatMessage: the line in everyone's chat.
        void BroadcastChat(PlayerSessionManager& sessions, const std::string& text) {
            Network::ChatMessageS2CPacket packet(text, /*position=*/0, /*sender=*/0);
            for (const auto& session : sessions.GetAllSessions()) {
                if (ServerConnection* connection = session ? session->GetConnection() : nullptr) {
                    connection->SendChatMessage(packet);
                }
            }
        }

        // EntityArgument.players() for a source without the selector
        // permission: names only ("argument.entity.selector.not_allowed").
        bool ResolvePlayers(const std::string& token, const CommandSourceStack& source, ServerConnection& connection,
                            std::vector<SelectedEntity>& out) {
            if (!token.empty() && token[0] == '@' && !CommandText::HasGamemasterPermission(connection)) {
                CommandText::SendFailure(connection, "Selector not allowed");
                return false;
            }
            std::string error;
            if (!ResolveSelector(token, SelectorKind::Players, source, out, error)) {
                CommandText::SendFailure(connection, error);
                return false;
            }
            return true;
        }

        // MC CommandResponseTracker.sendFeedback over player targets: the
        // single form names the player, the multiple form counts them.
        void Feedback(const CommandSourceStack& source, ServerConnection& connection,
                      const std::vector<SelectedEntity>& targets,
                      const std::string& single, const std::string& multiple) {
            if (targets.size() == 1) {
                source.SendSuccess(connection, single + CommandText::DisplayNameString(targets.front()), true);
            } else {
                source.SendSuccess(connection, multiple.substr(0, multiple.find("%s")) +
                                               std::to_string(targets.size()) +
                                               multiple.substr(multiple.find("%s") + 2), true);
            }
        }

    } // namespace

    // ── /say ────────────────────────────────────────────────────────────────

    void ChatCommands::ExecuteSay(const CommandSourceStack& source, const std::vector<std::string>& args,
                                  ServerConnection& connection, PlayerSessionManager& sessionManager) {
        if (args.empty()) {
            CommandText::SendFailure(connection, "Usage: /say <message>");
            return;
        }
        // ChatType.SAY_COMMAND: chat.type.announcement "[%s] %s".
        const std::string message = CommandText::ResolveMessage(CommandText::Join(args, 0), source,
                                                                CommandText::HasGamemasterPermission(connection));
        BroadcastChat(sessionManager, "[" + SourceName(source) + "] " + message);
    }

    // ── /me ─────────────────────────────────────────────────────────────────

    void ChatCommands::ExecuteMe(const CommandSourceStack& source, const std::vector<std::string>& args,
                                 ServerConnection& connection, PlayerSessionManager& sessionManager) {
        if (args.empty()) {
            CommandText::SendFailure(connection, "Usage: /me <action>");
            return;
        }
        // ChatType.EMOTE_COMMAND: chat.type.emote "* %s %s".
        const std::string action = CommandText::ResolveMessage(CommandText::Join(args, 0), source,
                                                               CommandText::HasGamemasterPermission(connection));
        BroadcastChat(sessionManager, "* " + SourceName(source) + " " + action);
    }

    // ── /msg /tell /w ───────────────────────────────────────────────────────

    void ChatCommands::ExecuteMsg(const CommandSourceStack& source, const std::vector<std::string>& args,
                                  ServerConnection& connection, PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        if (args.size() < 2) {
            CommandText::SendFailure(connection, "Usage: /msg <targets> <message>");
            return;
        }
        std::vector<SelectedEntity> targets;
        if (!ResolvePlayers(args[0], source, connection, targets)) return;
        const std::string message = CommandText::ResolveMessage(CommandText::Join(args, 1), source,
                                                                CommandText::HasGamemasterPermission(connection));
        // MsgCommand.sendMessage: the source sees "You whisper to <target>:
        // …" (MSG_COMMAND_OUTGOING), each target "<source> whispers to you:
        // …" (MSG_COMMAND_INCOMING) — both grey and italic. The outgoing
        // copy goes to the executing player (CommandSourceStack.
        // sendChatMessage → getPlayer()), else the sender.
        ServerConnection* outgoingTo = &connection;
        if (source.entity && source.entity->kind == SelectedEntity::Kind::Player) {
            if (ServerConnection* executor = ConnectionOf(*source.entity)) outgoingTo = executor;
        }
        const std::string from = SourceName(source);
        constexpr uint32_t kGrey = 0xFFAAAAAAu;
        const std::string italic = "\xC2\xA7o";
        for (const SelectedEntity& target : targets) {
            Network::ChatSegmentData outgoing{italic + "You whisper to " + CommandText::DisplayNameString(target) +
                                                  ": " + message,
                                              kGrey, Network::ChatClickAction::None, "", ""};
            CommandText::SendSystem(*outgoingTo, Segments{outgoing});
            if (ServerConnection* to = ConnectionOf(target)) {
                Network::ChatSegmentData incoming{italic + from + " whispers to you: " + message,
                                                  kGrey, Network::ChatClickAction::None, "", ""};
                CommandText::SendSystem(*to, Segments{incoming});
            }
        }
    }

    // ── /tellraw ────────────────────────────────────────────────────────────

    void ChatCommands::ExecuteTellRaw(const CommandSourceStack& source, const std::vector<std::string>& args,
                                      ServerConnection& connection, PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        if (args.size() < 2) {
            CommandText::SendFailure(connection, "Usage: /tellraw <targets> <message>");
            return;
        }
        std::vector<SelectedEntity> targets;
        if (!ResolvePlayers(args[0], source, connection, targets)) return;
        Game::Text::Component component;
        std::string error;
        if (!CommandText::ParseComponent(CommandText::Join(args, 1), component, error)) {
            CommandText::SendFailure(connection, error);
            return;
        }
        // TellRawCommand: player.sendSystemMessage(getResolvedComponent(
        // c, "message", player)) — resolved once per target, as the target.
        for (const SelectedEntity& target : targets) {
            ServerConnection* to = ConnectionOf(target);
            if (!to) continue;
            const Game::Text::Component resolved = CommandText::Resolve(component, source, &target);
            CommandText::SendSystem(*to, CommandText::ToSegments(resolved));
        }
    }

    // ── /title ──────────────────────────────────────────────────────────────

    void ChatCommands::ExecuteTitle(const CommandSourceStack& source, const std::vector<std::string>& args,
                                    ServerConnection& connection, PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        if (args.size() < 2) {
            CommandText::SendFailure(connection,
                "Usage: /title <targets> (clear|reset|title <title>|subtitle <title>|actionbar <title>|times <fadeIn> <stay> <fadeOut>)");
            return;
        }
        std::vector<SelectedEntity> targets;
        if (!ResolvePlayers(args[0], source, connection, targets)) return;

        std::string mode = args[1];
        for (char& c : mode) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        auto sendTo = [&](const Network::TitlesS2CPacket& packet) {
            const auto payload = Network::Serialization::Serialize(packet);
            for (const SelectedEntity& target : targets) {
                if (ServerConnection* to = ConnectionOf(target)) {
                    to->SendPacket(static_cast<uint8_t>(Network::PacketId::TitlesS2C), payload);
                }
            }
        };

        if (mode == "clear" || mode == "reset") {
            Network::TitlesS2CPacket packet;
            packet.action = mode == "clear" ? Network::TitlesS2CPacket::Action::Clear
                                            : Network::TitlesS2CPacket::Action::Reset;
            sendTo(packet);
            if (mode == "clear") Feedback(source, connection, targets, "Cleared titles for ", "Cleared titles for %s players");
            else                 Feedback(source, connection, targets, "Reset title options for ", "Reset title options for %s players");
            return;
        }

        if (mode == "times") {
            if (args.size() < 5) {
                CommandText::SendFailure(connection, "Usage: /title <targets> times <fadeIn> <stay> <fadeOut>");
                return;
            }
            // TimeArgument.time() (minimum 0) for each.
            int times[3] = {0, 0, 0};
            for (int i = 0; i < 3; ++i) {
                std::string error;
                if (!TimeCommand::ParseTimeArgumentTicks(args[2 + static_cast<size_t>(i)], 0, times[i], error)) {
                    CommandText::SendFailure(connection, error);
                    return;
                }
            }
            Network::TitlesS2CPacket packet;
            packet.action  = Network::TitlesS2CPacket::Action::Times;
            packet.fadeIn  = times[0];
            packet.stay    = times[1];
            packet.fadeOut = times[2];
            sendTo(packet);
            Feedback(source, connection, targets, "Changed title display times for ",
                     "Changed title display times for %s players");
            return;
        }

        if (mode != "title" && mode != "subtitle" && mode != "actionbar") {
            CommandText::SendFailure(connection, "Unknown /title mode: " + args[1]);
            return;
        }
        if (args.size() < 3) {
            CommandText::SendFailure(connection, "Usage: /title <targets> " + mode + " <title>");
            return;
        }
        Game::Text::Component component;
        std::string error;
        if (!CommandText::ParseComponent(CommandText::Join(args, 2), component, error)) {
            CommandText::SendFailure(connection, error);
            return;
        }
        // TitleCommand.showTitle: resolved per target with the target as
        // the entity override.
        for (const SelectedEntity& target : targets) {
            ServerConnection* to = ConnectionOf(target);
            if (!to) continue;
            const Game::Text::Component resolved = CommandText::Resolve(component, source, &target);
            if (mode == "actionbar") {
                // ClientboundSetActionBarTextPacket → the action bar line.
                to->SendChatMessage(CommandText::ToLegacyString(resolved), /*position=*/2);
                continue;
            }
            Network::TitlesS2CPacket packet;
            packet.action = mode == "title" ? Network::TitlesS2CPacket::Action::Title
                                            : Network::TitlesS2CPacket::Action::Subtitle;
            packet.text = resolved;
            to->SendPacket(static_cast<uint8_t>(Network::PacketId::TitlesS2C), Network::Serialization::Serialize(packet));
        }
        if (mode == "title")         Feedback(source, connection, targets, "Showing new title for ", "Showing new title for %s players");
        else if (mode == "subtitle") Feedback(source, connection, targets, "Showing new subtitle for ", "Showing new subtitle for %s players");
        else                         Feedback(source, connection, targets, "Showing new actionbar title for ",
                                              "Showing new actionbar title for %s players");
    }

    // ── Registration ────────────────────────────────────────────────────────

    void ChatCommands::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;

        dispatcher.RegisterCommand("say", ExecuteSay,
            Cmd::Root().Then(Cmd::Argument("message", Cmd::Arg::Message).Executes()));

        dispatcher.RegisterCommand("me", ExecuteMe,
            Cmd::Root().Then(Cmd::Argument("action", Cmd::Arg::Message).Executes()));

        // MsgCommand: /tell and /w redirect to /msg's node — the same tree.
        const Cmd::Node msg = Cmd::Root().Then(Cmd::Argument("targets", Cmd::Arg::Players)
            .Then(Cmd::Argument("message", Cmd::Arg::Message).Executes()));
        dispatcher.RegisterCommand("msg", ExecuteMsg, msg);
        dispatcher.RegisterCommand("tell", ExecuteMsg, msg);
        dispatcher.RegisterCommand("w", ExecuteMsg, msg);

        dispatcher.RegisterCommand("tellraw", ExecuteTellRaw,
            Cmd::Root().Then(Cmd::Argument("targets", Cmd::Arg::Players)
                .Then(Cmd::Argument("message", Cmd::Arg::TextComponent).Executes())));

        Cmd::Node targets = Cmd::Argument("targets", Cmd::Arg::Players);
        targets.Then(Cmd::Literal("clear").Executes());
        targets.Then(Cmd::Literal("reset").Executes());
        for (const char* mode : {"title", "subtitle", "actionbar"}) {
            targets.Then(Cmd::Literal(mode).Then(Cmd::Argument("title", Cmd::Arg::TextComponent).Executes()));
        }
        targets.Then(Cmd::Literal("times")
            .Then(Cmd::Argument("fadeIn", Cmd::Arg::Time).Suggests({"10", "20", "1s"})
                .Then(Cmd::Argument("stay", Cmd::Arg::Time).Suggests({"70", "100", "5s"})
                    .Then(Cmd::Argument("fadeOut", Cmd::Arg::Time).Suggests({"20", "1s"}).Executes()))));
        dispatcher.RegisterCommand("title", ExecuteTitle, Cmd::Root().Then(std::move(targets)));
    }

} // namespace Server
