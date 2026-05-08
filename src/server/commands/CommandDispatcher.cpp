// File: src/server/commands/CommandDispatcher.cpp
#include "CommandDispatcher.hpp"
#include "../player/ServerPlayer.hpp"
#include "../network/ServerConnection.hpp"
#include "common/core/Log.hpp"
#include <sstream>
#include <algorithm>

namespace Server {

    void CommandDispatcher::RegisterCommand(const std::string& name, CommandHandler handler) {
        // Store lowercase for case-insensitive lookup
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        m_commands[lower] = std::move(handler);
    }

    bool CommandDispatcher::ExecuteCommand(const std::string& commandLine,
                                           ServerPlayer& sender,
                                           ServerConnection& connection,
                                           PlayerSessionManager& sessionManager) {
        auto tokens = Tokenize(commandLine);
        if (tokens.empty()) return false;

        // Command name is first token (lowercase for lookup)
        std::string cmdName = tokens[0];
        std::transform(cmdName.begin(), cmdName.end(), cmdName.begin(), ::tolower);

        auto it = m_commands.find(cmdName);
        if (it == m_commands.end()) {
            // Unknown command — send error to sender
            connection.SendChatMessage("Unknown command: /" + tokens[0], 1);
            Log::Info("[CommandDispatcher] Unknown command '/%s' from player %s",
                     tokens[0].c_str(), sender.getName().c_str());
            return false;
        }

        // Build args (everything after command name)
        std::vector<std::string> args(tokens.begin() + 1, tokens.end());

        // Execute
        try {
            it->second(sender, args, connection, sessionManager);
        } catch (const std::exception& e) {
            connection.SendChatMessage("Error executing command: " + std::string(e.what()), 1);
            Log::Error("[CommandDispatcher] Exception in command '/%s': %s",
                      tokens[0].c_str(), e.what());
        }

        return true;
    }

    std::vector<std::string> CommandDispatcher::Tokenize(const std::string& input) {
        std::vector<std::string> tokens;
        std::istringstream iss(input);
        std::string token;
        while (iss >> token) {
            tokens.push_back(token);
        }
        return tokens;
    }

} // namespace Server
