// File: src/server/commands/CommandDispatcher.cpp
#include "CommandDispatcher.hpp"
#include "../player/ServerPlayer.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/core/Log.hpp"
#include "common/world/level/GameRules.hpp"
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

    void CommandDispatcher::RegisterCommand(const std::string& name, CommandHandler handler,
                                            Game::Cmd::Node syntax) {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        syntax.type = Game::Cmd::Arg::Literal;
        syntax.name = lower;
        syntax.redirectRoot = false;
        m_syntax[lower] = std::move(syntax);
        RegisterCommand(lower, std::move(handler));
    }

    void CommandDispatcher::UpdateSyntax(const std::string& name, Game::Cmd::Node syntax) {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (m_commands.find(lower) == m_commands.end()) return;
        syntax.type = Game::Cmd::Arg::Literal;
        syntax.name = lower;
        syntax.redirectRoot = false;
        m_syntax[lower] = std::move(syntax);
    }

    std::vector<const Game::Cmd::Node*> CommandDispatcher::GetCommandSyntax() const {
        std::vector<const Game::Cmd::Node*> out;
        for (const std::string& name : GetCommandNames()) {
            auto it = m_syntax.find(name);
            out.push_back(it != m_syntax.end() ? &it->second : nullptr);
        }
        return out;
    }

    std::vector<std::string> CommandDispatcher::GetUsageLines(const std::string& name) const {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        auto it = m_syntax.find(lower);
        if (it == m_syntax.end()) return {};
        return Game::Cmd::UsageLines(lower, it->second);
    }

    bool CommandDispatcher::ExecuteCommand(const std::string& commandLine,
                                           ServerPlayer& sender,
                                           ServerConnection& connection,
                                           PlayerSessionManager& sessionManager) {
        // A player's own line: `name=` selectors may defer it until the named
        // entities they found in unloaded chunks are in (EntitySelector.hpp,
        // NamedEntityIndex.hpp). Not for a nested run, not for the re-run.
        NamedEntityDeferral& deferral = CurrentNamedEntityDeferral();
        const bool outermost = m_depth == 0;
        if (outermost) {
            deferral.chunks.clear();
            deferral.allowed = !m_rerunning;
        }
        const bool found = ExecuteCommand(commandLine, CommandSourceStack::ForPlayer(sender, sessionManager),
                                          connection, sessionManager);
        if (outermost) {
            if (!deferral.chunks.empty() && !m_rerunning) {
                Deferred wait;
                wait.commandLine = commandLine;
                wait.playerId    = sender.getPlayerId();
                wait.chunks      = std::move(deferral.chunks);
                NamedEntities::Hold(wait.chunks);
                m_deferred.push_back(std::move(wait));
            }
            deferral.chunks.clear();
            deferral.allowed = false;
        }
        return found;
    }

    void CommandDispatcher::ProcessDeferred(PlayerSessionManager& sessionManager) {
        if (m_deferred.empty()) return;
        // Taken out first: the re-run may itself run commands.
        std::vector<Deferred> due;
        for (size_t i = 0; i < m_deferred.size();) {
            Deferred& wait = m_deferred[i];
            ++wait.ticks;
            const auto session = sessionManager.GetSession(wait.playerId);
            const bool gone = !session || !session->GetPlayer() || !session->GetConnection();
            if (gone || NamedEntities::Ready(wait.chunks) || wait.ticks >= kDeferredTimeoutTicks) {
                if (gone) NamedEntities::Release(wait.chunks);
                else due.push_back(std::move(wait));
                m_deferred.erase(m_deferred.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }
        for (Deferred& wait : due) {
            const auto session = sessionManager.GetSession(wait.playerId);
            ServerPlayer* player = session ? session->GetPlayer() : nullptr;
            ServerConnection* connection = session ? session->GetConnection() : nullptr;
            if (player && connection) {
                if (!NamedEntities::Ready(wait.chunks)) {
                    connection->SendChatMessage("Some named entities could not be loaded in time", 1);
                }
                m_rerunning = true;
                ExecuteCommand(wait.commandLine, *player, *connection, sessionManager);
                m_rerunning = false;
            }
            NamedEntities::Release(wait.chunks);
        }
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

        // MC Commands.executeCommandInContext: a top-level command gets an
        // ExecutionContext whose quota is max(1, max_command_sequence_length);
        // runCommandQueue stops (and logs once) when it is spent, and each
        // command executed costs one (ExecuteCommand.execute's incrementCost).
        if (m_depth == 0) {
            m_commandQuota = std::max(1, Game::Rules::GetInt(Game::Rules::Id::MaxCommandSequenceLength));
            m_quotaLogged  = false;
        }
        if (m_commandQuota <= 0) {
            if (!m_quotaLogged) {
                Log::Info("[CommandDispatcher] Command execution stopped due to limit (executed %d commands)",
                          std::max(1, Game::Rules::GetInt(Game::Rules::Id::MaxCommandSequenceLength)));
                m_quotaLogged = true;
            }
            return true;
        }
        --m_commandQuota;

        // Execute
        struct DepthGuard {
            int& depth;
            explicit DepthGuard(int& d) : depth(d) { ++depth; }
            ~DepthGuard() { --depth; }
        } depthGuard(m_depth);
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
        // Whitespace splits tokens, except inside a selector's or an item's
        // [...] (MC's EntitySelectorParser reads options across spaces —
        // `@e[type=cow, name="Two Words"]`), inside SNBT {...} (`/data merge
        // entity @s {Health: 20f, CustomName: "Rex"}`), and inside a quoted
        // string — one within those, or one a token starts with (MC
        // StringReader.readString: `/data modify … value "two words"`). A
        // quote inside a plain word ("don't") is just a character.
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
            if ((depth > 0 || token.empty()) && (c == '"' || c == '\'')) {
                quote = c;
                token += c;
                continue;
            }
            if (c == '[' || c == '{') ++depth;
            else if ((c == ']' || c == '}') && depth > 0) --depth;
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
