// File: src/server/commands/ServerAdminCommands.cpp
#include "ServerAdminCommands.hpp"
#include "CommandText.hpp"
#include "../IntegratedServer.hpp"
#include "../network/ServerConnection.hpp"
#include "common/core/Log.hpp"

#include <cctype>
#include <string>

namespace Server {

    void ServerAdminCommands::ExecuteSaveAll(const CommandSourceStack& source, const std::vector<std::string>& args,
                                             ServerConnection& connection, PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        if (!g_integratedServer) return;
        bool flush = false;
        if (!args.empty()) {
            std::string word = args[0];
            for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (word != "flush") {
                CommandText::SendFailure(connection, "Usage: /save-all [flush]");
                return;
            }
            flush = true;
        }
        // SaveAllCommand.saveAll: "Saving the game…", saveEverything(true,
        // flush, true), then "Saved the game" or ERROR_FAILED.
        source.SendSuccess(connection, "Saving the game (this may take a moment!)", false);
        if (!g_integratedServer->SaveEverything(flush)) {
            CommandText::SendFailure(connection, "Unable to save the game (is there enough disk space?)");
            return;
        }
        source.SendSuccess(connection, "Saved the game", true);
    }

    void ServerAdminCommands::ExecuteSaveOff(const CommandSourceStack& source, const std::vector<std::string>& args,
                                             ServerConnection& connection, PlayerSessionManager& sessionManager) {
        (void)args;
        (void)sessionManager;
        if (!g_integratedServer) return;
        if (!g_integratedServer->SetAutoSave(false)) {
            CommandText::SendFailure(connection, "Saving is already turned off");
            return;
        }
        source.SendSuccess(connection, "Automatic saving is now disabled", true);
    }

    void ServerAdminCommands::ExecuteSaveOn(const CommandSourceStack& source, const std::vector<std::string>& args,
                                            ServerConnection& connection, PlayerSessionManager& sessionManager) {
        (void)args;
        (void)sessionManager;
        if (!g_integratedServer) return;
        if (!g_integratedServer->SetAutoSave(true)) {
            CommandText::SendFailure(connection, "Saving is already turned on");
            return;
        }
        source.SendSuccess(connection, "Automatic saving is now enabled", true);
    }

    void ServerAdminCommands::ExecuteStop(const CommandSourceStack& source, const std::vector<std::string>& args,
                                          ServerConnection& connection, PlayerSessionManager& sessionManager) {
        (void)args;
        (void)sessionManager;
        if (!g_integratedServer) return;
        // StopCommand: "Stopping the server", then halt(false) — the process
        // that runs the server notices and shuts it down (saving the world)
        // between ticks, never from inside this one.
        source.SendSuccess(connection, "Stopping the server", true);
        Log::Info("[Server] /stop from %s", source.DisplayName().c_str());
        g_integratedServer->RequestHalt();
    }

    void ServerAdminCommands::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        dispatcher.RegisterCommand("save-all", ExecuteSaveAll,
            Cmd::Root().Executes().Then(Cmd::Literal("flush").Executes()));
        dispatcher.RegisterCommand("save-off", ExecuteSaveOff, Cmd::Root().Executes());
        dispatcher.RegisterCommand("save-on", ExecuteSaveOn, Cmd::Root().Executes());
        dispatcher.RegisterCommand("stop", ExecuteStop, Cmd::Root().Executes());
    }

} // namespace Server
