// File: src/server/commands/CommandDispatcher.hpp
// Server-side command dispatcher — MC's Commands.java pattern.
// Commands are registered with handlers and dispatched by name.
#pragma once

#include "CommandSourceStack.hpp"
#include "common/command/CommandSyntax.hpp"

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

        // Register a command by name, with its argument tree (MC's
        // Brigadier node — common/command/CommandSyntax.hpp). The tree is
        // shipped to every client in CommandsS2C and is the ONLY source of
        // the chat's usage hint and Tab completion for this command, so every
        // command registers one: `Cmd::Root()` alone for a command that takes
        // nothing, `.Executes()` wherever the command may end.
        void RegisterCommand(const std::string& name, CommandHandler handler, Game::Cmd::Node syntax);
        // Without a tree: the name completes, but the client knows nothing
        // of its arguments. Don't — every command should declare its tree.
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

        // The registered tree of each name GetCommandNames returns, in the
        // same order; nullptr for a command registered without one. What
        // CommandsS2C ships. Pointers into the dispatcher: valid until the
        // next RegisterCommand.
        std::vector<const Game::Cmd::Node*> GetCommandSyntax() const;

        // The command's usage lines ("/effect give <targets> <effect> ..."),
        // generated from its tree; empty when the name is unknown.
        std::vector<std::string> GetUsageLines(const std::string& name) const;

        // Server thread, once a tick: re-run the commands a `name=` selector
        // deferred (named entities in unloaded chunks — EntitySelector.hpp)
        // once their chunks' entities are in, or after the timeout.
        void ProcessDeferred(PlayerSessionManager& sessionManager);

    private:
        struct Deferred {
            std::string commandLine;
            uint32_t    playerId = 0;
            std::vector<NamedEntities::ChunkRef> chunks;
            int         ticks = 0;
        };
        static constexpr int kDeferredTimeoutTicks = 200;   // 10 s
        std::vector<Deferred> m_deferred;
        int  m_depth = 0;          // nested ExecuteCommand (/execute run)
        bool m_rerunning = false;  // inside ProcessDeferred's re-run

        std::unordered_map<std::string, CommandHandler> m_commands;
        std::unordered_map<std::string, Game::Cmd::Node> m_syntax;

        // Tokenize a string by spaces
        static std::vector<std::string> Tokenize(const std::string& input);
    };

} // namespace Server
