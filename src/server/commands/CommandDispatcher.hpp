// File: src/server/commands/CommandDispatcher.hpp
// Server-side command dispatcher — MC's Commands.java pattern.
// Commands are registered with handlers and dispatched by name.
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace Server {

    class ServerConnection;
    class PlayerSessionManager;
    class ServerPlayer;

    class CommandDispatcher {
    public:
        // Command handler signature: sender player, args (command name excluded), connection for feedback
        using CommandHandler = std::function<void(
            ServerPlayer& sender,
            const std::vector<std::string>& args,
            ServerConnection& connection,
            PlayerSessionManager& sessionManager)>;

        // Register a command by name
        void RegisterCommand(const std::string& name, CommandHandler handler);

        // Execute a command line (without leading '/').
        // Returns true if the command was found, false if unknown.
        bool ExecuteCommand(const std::string& commandLine,
                           ServerPlayer& sender,
                           ServerConnection& connection,
                           PlayerSessionManager& sessionManager);

    private:
        std::unordered_map<std::string, CommandHandler> m_commands;

        // Tokenize a string by spaces
        static std::vector<std::string> Tokenize(const std::string& input);
    };

} // namespace Server
