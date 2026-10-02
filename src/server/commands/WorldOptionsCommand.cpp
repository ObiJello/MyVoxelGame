// File: src/server/commands/WorldOptionsCommand.cpp
#include "WorldOptionsCommand.hpp"
#include "GameModeCommand.hpp"
#include "../IntegratedServer.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "common/core/Log.hpp"

#include <cctype>
#include <optional>
#include <string>

namespace Server {

    namespace {
        std::string Lower(std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        std::optional<bool> ParseOnOff(const std::string& raw) {
            const std::string v = Lower(raw);
            if (v == "on" || v == "true" || v == "1")   return true;
            if (v == "off" || v == "false" || v == "0") return false;
            return std::nullopt;
        }

        // /gamemode's parser (names, s/c/a/sp, 0-3), as the world's int.
        std::optional<int> ParseGameMode(const std::string& v) {
            const auto mode = GameModeCommand::ParseGameMode(v);
            if (!mode) return std::nullopt;
            return static_cast<int>(*mode);
        }

        const char* GameModeDisplayName(int mode) {
            switch (mode) {
                case 0: return "Survival Mode";
                case 1: return "Creative Mode";
                case 2: return "Adventure Mode";
                case 3: return "Spectator Mode";
            }
            return "Unknown Mode";
        }

        // MC Commands.LEVEL_OWNERS: only the player who opened the world.
        bool RequireOwner(ServerConnection& connection) {
            if (connection.IsSingleplayerOwner()) return true;
            connection.SendChatMessage("You do not have permission to use this command", 1);
            return false;
        }

        std::optional<int> ParsePort(const std::string& raw) {
            if (raw.empty()) return std::nullopt;
            for (char c : raw) if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
            const long v = std::stol(raw);
            if (v < 1024 || v > 65535) return std::nullopt;
            return static_cast<int>(v);
        }
    }

    void WorldOptionsCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        const Cmd::Node onOff = Cmd::Argument("value", Cmd::Arg::Word)
            .Suggests({"on", "off"}).Executes();
        const Cmd::Node port = Cmd::Argument("port", Cmd::Arg::Integer)
            .Suggests({"25565"}).Executes();
        dispatcher.RegisterCommand("worldoptions", WorldOptionsCommand::Execute,
            Cmd::Root()
                .Then(Cmd::Literals({"allow_commands", "guest_command_access", "force_game_mode", "joinable"},
                                    false, &onOff))
                .Then(Cmd::Literal("difficulty_lock").Executes())
                .Then(Cmd::Literal("port").Then(port)));
        dispatcher.RegisterCommand("defaultgamemode", WorldOptionsCommand::ExecuteDefaultGameMode,
            Cmd::Root().Then(Cmd::Argument("gamemode", Cmd::Arg::GameMode).Executes()));
        dispatcher.RegisterCommand("publish", WorldOptionsCommand::ExecutePublish,
            Cmd::Root().Executes().Then(port));
    }

    // MC DefaultGameModeCommand: "The default game mode is now %s"
    // (commands.defaultgamemode.success). Vanilla also moves every player
    // whose mode is not forced… only when the mode is forced (setDefaultGameType
    // → forceGameMode); this engine applies it to future joins.
    void WorldOptionsCommand::ExecuteDefaultGameMode(const CommandSourceStack& source,
                                                     const std::vector<std::string>& args,
                                                     ServerConnection& connection,
                                                     PlayerSessionManager& /*sessionManager*/) {
        if (!g_integratedServer) return;
        if (!RequireOwner(connection)) return;
        if (args.empty()) {
            connection.SendChatMessage("Usage: /defaultgamemode <survival|creative|adventure|spectator>", 1);
            return;
        }
        const auto mode = ParseGameMode(args[0]);
        if (!mode) {
            connection.SendChatMessage("Unknown game mode: " + args[0], 1);
            return;
        }
        g_integratedServer->SetWorldGameType(*mode);
        source.SendSuccess(connection, std::string("The default game mode is now ") + GameModeDisplayName(*mode), true);
        Log::Info("[WorldOptions] default game mode -> %s", GameModeDisplayName(*mode));
    }

    // MC PublishCommand: "/publish [port]" → "Local game hosted on port %s",
    // "Multiplayer game is already hosted on port %s", "Unable to host local game".
    void WorldOptionsCommand::ExecutePublish(const CommandSourceStack& source,
                                             const std::vector<std::string>& args,
                                             ServerConnection& connection,
                                             PlayerSessionManager& /*sessionManager*/) {
        if (!g_integratedServer) return;
        if (!RequireOwner(connection)) return;
        std::optional<int> port;
        if (!args.empty()) {
            port = ParsePort(args[0]);
            if (!port) {
                connection.SendChatMessage("Not a valid port. Enter a number between 1024 and 65535.", 1);
                return;
            }
        }
        if (g_integratedServer->IsJoinable() && (!port || *port == g_integratedServer->GetPort())) {
            connection.SendChatMessage("Multiplayer game is already hosted on port " +
                                       std::to_string(g_integratedServer->GetPort()), 1);
            return;
        }
        if (port && !g_integratedServer->ChangePort(static_cast<uint16_t>(*port))) {
            connection.SendChatMessage("Unable to host local game", 1);
            return;
        }
        g_integratedServer->SetJoinable(true);
        source.SendSuccess(connection, "Local game hosted on port " + std::to_string(g_integratedServer->GetPort()), true);
    }

    void WorldOptionsCommand::Execute(const CommandSourceStack& /*source*/,
                                      const std::vector<std::string>& args,
                                      ServerConnection& connection,
                                      PlayerSessionManager& /*sessionManager*/) {
        static const char* kUsage =
            "Usage: /worldoptions (allow_commands|guest_command_access|force_game_mode|joinable) <on|off> | "
            "/worldoptions difficulty <peaceful|easy|normal|hard> | "
            "/worldoptions difficulty_lock | /worldoptions port <1024-65535>";
        if (!g_integratedServer) return;
        if (!RequireOwner(connection)) return;
        if (args.empty()) { connection.SendChatMessage(kUsage, 1); return; }
        const std::string option = Lower(args[0]);
        auto& server = *g_integratedServer;

        // The World Options screen's difficulty button (MC
        // Minecraft.getSingleplayerServer().setDifficulty(d, false) — a
        // direct call, not the permission-gated /difficulty, so it works with
        // cheats off). Not forced: a locked difficulty stays, and a hardcore
        // world stays HARD.
        if (option == "difficulty") {
            static const char* kNames[] = {"peaceful", "easy", "normal", "hard"};
            if (args.size() < 2) { connection.SendChatMessage(kUsage, 1); return; }
            const std::string wanted = Lower(args[1]);
            int difficulty = -1;
            for (int i = 0; i < 4; ++i) if (wanted == kNames[i]) difficulty = i;
            if (difficulty < 0) { connection.SendChatMessage(kUsage, 1); return; }
            if (server.IsDifficultyLocked()) {
                connection.SendChatMessage("Difficulty is locked.", 1);
                return;
            }
            if (server.IsHardcore()) difficulty = 3;
            if (difficulty == server.GetDifficulty()) return;
            server.SetDifficulty(difficulty);
            connection.SendChatMessage(std::string("The difficulty has been set to ") + kNames[difficulty], 1);
            return;
        }
        if (option == "difficulty_lock") {
            if (server.IsDifficultyLocked()) {
                connection.SendChatMessage("Difficulty is locked.", 1);
                return;
            }
            server.SetDifficultyLocked(true);
            connection.SendChatMessage("The difficulty is now locked", 1);
            return;
        }
        if (option == "port") {
            if (args.size() < 2) { connection.SendChatMessage(kUsage, 1); return; }
            const auto port = ParsePort(args[1]);
            if (!port) {
                connection.SendChatMessage("Not a valid port. Enter a number between 1024 and 65535.", 1);
                return;
            }
            if (!server.ChangePort(static_cast<uint16_t>(*port))) {
                connection.SendChatMessage("Port not available. Enter a different number between 1024 and 65535.", 1);
                return;
            }
            connection.SendChatMessage(server.IsJoinable()
                ? "This world is now open to LAN. The port number is " + std::to_string(server.GetPort())
                : "Port set to " + std::to_string(server.GetPort()), 1);
            return;
        }

        if (args.size() < 2) { connection.SendChatMessage(kUsage, 1); return; }
        const auto value = ParseOnOff(args[1]);
        if (!value) { connection.SendChatMessage(kUsage, 1); return; }

        if (option == "allow_commands") {
            if (server.IsHardcore() && *value) {
                connection.SendChatMessage("Cannot allow commands in a hardcore world.", 1);
                return;
            }
            server.SetWorldAllowCommands(*value);
            // MC has no message for this (the screen applies it silently);
            // one line so the change is visible in chat and the log.
            connection.SendChatMessage(*value ? "Commands are now allowed in this world"
                                              : "Commands are no longer allowed in this world", 1);
            return;
        }
        if (option == "guest_command_access") {
            server.SetGuestCommandAccess(*value);
            connection.SendChatMessage(*value ? "Players joining your world can now use commands"
                                              : "Players joining your world can no longer use commands", 1);
            return;
        }
        if (option == "force_game_mode") {
            server.SetForceGameMode(*value);
            connection.SendChatMessage(*value ? "Other players will be forced to play the world's default game mode"
                                              : "Other players will keep their own game mode", 1);
            return;
        }
        if (option == "joinable") {
            if (server.IsJoinable() == *value) return;
            server.SetJoinable(*value);
            // MC menu.multiplayerOptions.publish.started.lan / .stopped.
            connection.SendChatMessage(*value
                ? "This world is now open to LAN. The port number is " + std::to_string(server.GetPort())
                : "This world is no longer open to multiplayer", 1);
            return;
        }
        connection.SendChatMessage(kUsage, 1);
    }

} // namespace Server
