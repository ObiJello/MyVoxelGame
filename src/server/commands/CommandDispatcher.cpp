// File: src/server/commands/CommandDispatcher.cpp
#include "CommandDispatcher.hpp"
#include "../player/ServerPlayer.hpp"
#include "../network/ServerConnection.hpp"
#include "common/core/Log.hpp"
#include <sstream>
#include <algorithm>
#include <cctype>

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
        return ExecuteCommand(commandLine, CommandSourceStack::ForPlayer(sender, sessionManager),
                              connection, sessionManager);
    }

    bool CommandDispatcher::ExecuteCommand(const std::string& commandLine,
                                           const CommandSourceStack& source,
                                           ServerConnection& connection,
                                           PlayerSessionManager& sessionManager) {
        ServerPlayer& sender = *source.sender;
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
            it->second(source, args, connection, sessionManager);
        } catch (const std::exception& e) {
            connection.SendChatMessage("Error executing command: " + std::string(e.what()), 1);
            Log::Error("[CommandDispatcher] Exception in command '/%s': %s",
                      tokens[0].c_str(), e.what());
        }

        return true;
    }

    std::vector<std::string> CommandDispatcher::GetCommandNames() const {
        std::vector<std::string> names;
        names.reserve(m_commands.size());
        // Keys are already lowercase (RegisterCommand normalises them), which
        // is the form tab-completion wants.
        for (const auto& [name, handler] : m_commands) names.push_back(name);
        // The map is unordered, so sort for a stable popup order.
        std::sort(names.begin(), names.end());
        return names;
    }

    std::vector<std::string> CommandDispatcher::Tokenize(const std::string& input) {
        // Whitespace splits tokens, except inside a selector's [...] (MC's
        // EntitySelectorParser reads options across spaces — `@e[type=cow,
        // name="Two Words"]`), and inside a quoted string within one.
        std::vector<std::string> tokens;
        std::string token;
        int  depth = 0;
        char quote = 0;
        for (size_t i = 0; i < input.size(); ++i) {
            const char c = input[i];
            if (quote) {
                token += c;
                if (c == '\\' && i + 1 < input.size()) token += input[++i];
                else if (c == quote) quote = 0;
                continue;
            }
            if (depth > 0 && (c == '"' || c == '\'')) {
                quote = c;
                token += c;
                continue;
            }
            if (c == '[') ++depth;
            else if (c == ']' && depth > 0) --depth;
            if (depth == 0 && std::isspace(static_cast<unsigned char>(c))) {
                if (!token.empty()) tokens.push_back(std::move(token));
                token.clear();
                continue;
            }
            token += c;
        }
        if (!token.empty()) tokens.push_back(std::move(token));
        return tokens;
    }

} // namespace Server
