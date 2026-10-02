// File: src/server/commands/StopwatchCommand.cpp
#include "StopwatchCommand.hpp"
#include "CommandSavedData.hpp"
#include "CommandStorage.hpp"
#include "CommandText.hpp"
#include "../IntegratedServer.hpp"
#include "../level/NamedEntityIndex.hpp"
#include "../network/ServerConnection.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

namespace Server {

    namespace {

        // Java's Double.toString for a millisecond-resolution value:
        // "5.0", "5.25", "61.123".
        std::string JavaDouble(double v) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.3f", v);
            std::string s = buf;
            while (s.size() > 1 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
            return s;
        }

        void Refresh() {
            if (!g_integratedServer) return;
            g_integratedServer->GetCommandDispatcher().UpdateSyntax("stopwatch", StopwatchCommand::Syntax());
            NamedEntities::ResendCommands();
        }

    } // namespace

    void StopwatchCommand::Execute(const CommandSourceStack& source, const std::vector<std::string>& args,
                                   ServerConnection& connection, PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        if (args.size() < 2) {
            CommandText::SendFailure(connection, "Usage: /stopwatch (create|query|restart|remove) <id>");
            return;
        }
        std::string mode = args[0];
        for (char& c : mode) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string id;
        if (!CommandStorage::NormalizeId(args[1], id)) {
            CommandText::SendFailure(connection, "Invalid ID: " + args[1]);
            return;
        }

        if (mode == "create") {
            if (!CommandSavedData::AddStopwatch(id)) {
                CommandText::SendFailure(connection, "Stopwatch '" + id + "' already exists");
                return;
            }
            source.SendSuccess(connection, "Created stopwatch '" + id + "'", true);
            Refresh();
            return;
        }
        if (mode == "query") {
            const std::optional<double> elapsed = CommandSavedData::StopwatchElapsedSeconds(id);
            if (!elapsed) {
                CommandText::SendFailure(connection, "Stopwatch '" + id + "' does not exist");
                return;
            }
            // <scale> only scales the command's result value (what /execute
            // store would read); the message is the same either way.
            if (args.size() >= 3) {
                char* end = nullptr;
                std::strtod(args[2].c_str(), &end);
                if (end == args[2].c_str() || *end != '\0') {
                    CommandText::SendFailure(connection, "Invalid double '" + args[2] + "'");
                    return;
                }
            }
            source.SendSuccess(connection, "Stopwatch '" + id + "' has run for " + JavaDouble(*elapsed) + "s", true);
            return;
        }
        if (mode == "restart") {
            if (!CommandSavedData::RestartStopwatch(id)) {
                CommandText::SendFailure(connection, "Stopwatch '" + id + "' does not exist");
                return;
            }
            source.SendSuccess(connection, "Restarted stopwatch '" + id + "'", true);
            return;
        }
        if (mode == "remove") {
            if (!CommandSavedData::RemoveStopwatch(id)) {
                CommandText::SendFailure(connection, "Stopwatch '" + id + "' does not exist");
                return;
            }
            source.SendSuccess(connection, "Removed stopwatch '" + id + "'", true);
            Refresh();
            return;
        }
        CommandText::SendFailure(connection, "Usage: /stopwatch (create|query|restart|remove) <id>");
    }

    Game::Cmd::Node StopwatchCommand::Syntax() {
        namespace Cmd = Game::Cmd;
        const std::vector<std::string> ids = CommandSavedData::StopwatchIds();
        Cmd::Node root = Cmd::Root();
        // create takes a NEW id: no suggestions (MC IdentifierArgument).
        root.Then(Cmd::Literal("create").Then(Cmd::Argument("id", Cmd::Arg::Word).Executes()));
        root.Then(Cmd::Literal("query").Then(Cmd::Argument("id", Cmd::Arg::Word).Suggests(ids).Executes()
            .Then(Cmd::Argument("scale", Cmd::Arg::Float).Suggests({"1", "20", "1000"}).Executes())));
        root.Then(Cmd::Literal("restart").Then(Cmd::Argument("id", Cmd::Arg::Word).Suggests(ids).Executes()));
        root.Then(Cmd::Literal("remove").Then(Cmd::Argument("id", Cmd::Arg::Word).Suggests(ids).Executes()));
        return root;
    }

    void StopwatchCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("stopwatch", StopwatchCommand::Execute, Syntax());
    }

} // namespace Server
