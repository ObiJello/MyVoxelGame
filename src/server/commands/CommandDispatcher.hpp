// File: src/server/commands/CommandDispatcher.hpp
// Server-side command dispatcher — MC's Commands.java pattern.
// Commands are registered with handlers and dispatched by name.
#pragma once

#include "CommandSourceStack.hpp"

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
        // Command handler signature: the source stack (MC CommandSourceStack —
        // `source.sender` is the player who typed it, never null; position,
        // rotation, dimension and `@s` entity are what `/execute` may have
        // rewritten), args (command name excluded), connection for feedback.
        using CommandHandler = std::function<void(
            const CommandSourceStack& source,
            const std::vector<std::string>& args,
            ServerConnection& connection,
            PlayerSessionManager& sessionManager)>;

        // Register a command by name
        void RegisterCommand(const std::string& name, CommandHandler handler);

        // Execute a command line (without leading '/') as the player typed it:
        // the stack is built from the sender (CommandSourceStack::ForPlayer).
        // Returns true if the command was found, false if unknown.
        bool ExecuteCommand(const std::string& commandLine,
                           ServerPlayer& sender,
                           ServerConnection& connection,
                           PlayerSessionManager& sessionManager);

        // Execute a command line from an already-built stack — what
        // `/execute ... run <command>` does with each of its forked sources.
        bool ExecuteCommand(const std::string& commandLine,
                           const CommandSourceStack& source,
                           ServerConnection& connection,
                           PlayerSessionManager& sessionManager);

        // Every registered command name, sorted. Sent to each client on join
        // (CommandsS2C) so tab-completion reflects what this server actually
        // accepts instead of a list the client hardcodes and forgets to update.
        std::vector<std::string> GetCommandNames() const;

    private:
        std::unordered_map<std::string, CommandHandler> m_commands;

        // Tokenize a string by spaces
        static std::vector<std::string> Tokenize(const std::string& input);
    };

} // namespace Server
