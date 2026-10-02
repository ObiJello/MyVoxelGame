// File: src/server/commands/InfoCommands.cpp
#include "InfoCommands.hpp"
#include "CommandSavedData.hpp"
#include "CommandStorage.hpp"
#include "CommandText.hpp"
#include "../IntegratedServer.hpp"
#include "../level/NamedEntityIndex.hpp"
#include "../level/ServerLevel.hpp"
#include "../network/NetworkServer.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../world/storage/anvil/PlayerUuid.hpp"
#include "common/core/Config.hpp"
#include "common/core/SaveVersion.hpp"
#include "common/network/packets/HandshakeC2S.hpp"
#include "common/world/level/World.hpp"

#include "random/RandomSupport.h"            // terrain library: MC RandomSupport
#include "random/XoroshiroRandomSource.h"    // terrain library: MC XoroshiroRandomSource

#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace Server {

    namespace {

        namespace Cmd = Game::Cmd;

        bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
                    return false;
                }
            }
            return true;
        }

        std::string Lower(std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // Brigadier IntegerArgumentType.integer(): a whole number, nothing
        // trailing, within int.
        std::optional<int> ParseInt(const std::string& text) {
            if (text.empty()) return std::nullopt;
            char* end = nullptr;
            const long long v = std::strtoll(text.c_str(), &end, 10);
            if (end == text.c_str() || *end != '\0' || v < INT_MIN || v > INT_MAX) return std::nullopt;
            return static_cast<int>(v);
        }

        std::optional<bool> ParseBool(const std::string& text) {
            if (text == "true") return true;
            if (text == "false") return false;
            return std::nullopt;
        }

        // MC RangeArgument.intRange (MinMaxBounds.Ints.fromReader): "5",
        // "1..6", "..6", "1..". Bounds absent stay nullopt.
        bool ParseIntRange(const std::string& text, std::optional<int>& min, std::optional<int>& max,
                           std::string& error) {
            const size_t dots = text.find("..");
            if (dots == std::string::npos) {
                const auto exact = ParseInt(text);
                if (!exact) { error = "Invalid integer '" + text + "'"; return false; }
                min = max = *exact;
                return true;
            }
            const std::string lo = text.substr(0, dots);
            const std::string hi = text.substr(dots + 2);
            if (lo.empty() && hi.empty()) { error = "Expected value or range of values"; return false; }
            if (!lo.empty()) {
                min = ParseInt(lo);
                if (!min) { error = "Invalid integer '" + lo + "'"; return false; }
            }
            if (!hi.empty()) {
                max = ParseInt(hi);
                if (!max) { error = "Invalid integer '" + hi + "'"; return false; }
            }
            if (min && max && *min > *max) { error = "Min cannot be bigger than max"; return false; }
            return true;
        }

        int64_t WorldSeed(const CommandSourceStack& source) {
            ServerLevel* level = g_integratedServer ? g_integratedServer->GetLevel(source.dimension) : nullptr;
            Game::World* world = level ? level->World() : nullptr;
            return world ? world->GetGenerationSeed() : 0;
        }

        // The level's own RandomSource for an unsequenced draw (MC
        // source.getLevel().getRandom()).
        minecraft::XoroshiroRandomSource& LevelRandom() {
            static minecraft::XoroshiroRandomSource random(minecraft::RandomSupport::generateUniqueSeed());
            return random;
        }

        // A resend of /random's tree after its sequence ids changed.
        void RefreshRandomSyntax() {
            if (!g_integratedServer) return;
            g_integratedServer->GetCommandDispatcher().UpdateSyntax("random", InfoCommands::RandomSyntax());
            NamedEntities::ResendCommands();
        }

    } // namespace

    // ── /list ───────────────────────────────────────────────────────────────

    void InfoCommands::ExecuteList(const CommandSourceStack& source, const std::vector<std::string>& args,
                                   ServerConnection& connection, PlayerSessionManager& sessionManager) {
        const bool uuids = !args.empty() && EqualsIgnoreCase(args[0], "uuids");
        std::vector<std::string> names;
        for (const auto& session : sessionManager.GetAllSessions()) {
            const ServerPlayer* player = session ? session->GetPlayer() : nullptr;
            if (!player) continue;
            if (uuids) {
                // commands.list.nameAndId "%s (%s)".
                names.push_back(player->getName() + " (" +
                                Game::Anvil::UuidToString(Game::Anvil::OfflinePlayerUuid(player->getName())) + ")");
            } else {
                names.push_back(player->getName());
            }
        }
        // ComponentUtils.formatList: ", " between.
        std::string list;
        for (size_t i = 0; i < names.size(); ++i) list += (i ? ", " : "") + names[i];
        const NetworkServer* network = g_integratedServer ? g_integratedServer->GetNetworkServer() : nullptr;
        const size_t maxPlayers = network ? network->GetMaxConnections() : names.size();
        // commands.list.players "There are %s of a max of %s players online: %s".
        source.SendSuccess(connection, "There are " + std::to_string(names.size()) + " of a max of " +
                                       std::to_string(maxPlayers) + " players online: " + list, false);
    }

    // ── /help ───────────────────────────────────────────────────────────────

    void InfoCommands::ExecuteHelp(const CommandSourceStack& source, const std::vector<std::string>& args,
                                   ServerConnection& connection, PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        if (!g_integratedServer) return;
        CommandDispatcher& dispatcher = g_integratedServer->GetCommandDispatcher();
        const std::vector<std::string> names = dispatcher.GetCommandNames();
        const std::vector<const Cmd::Node*> trees = dispatcher.GetCommandSyntax();

        if (args.empty()) {
            // getSmartUsage(root): one line per command.
            for (size_t i = 0; i < names.size(); ++i) {
                const Cmd::Node* root = i < trees.size() ? trees[i] : nullptr;
                source.SendSuccess(connection, "/" + (root ? Cmd::Usage(*root, false, *root) : names[i]), false);
            }
            return;
        }

        // dispatcher.parse(<command>): follow the typed words down the tree;
        // the usage of what may follow the last node reached.
        const std::string name = Lower(args[0]);
        const Cmd::Node* root = nullptr;
        bool known = false;
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] != name) continue;
            known = true;
            root = i < trees.size() ? trees[i] : nullptr;
        }
        if (!known) {
            CommandText::SendFailure(connection, "Unknown command or insufficient permissions");
            return;
        }
        if (!root) return;

        const Cmd::Node* node = root;
        size_t i = 1;
        while (i < args.size() && !node->redirectRoot) {
            const Cmd::Node* next = nullptr;
            size_t consumed = 0;
            for (const Cmd::Node& kid : node->children) {
                if (kid.IsLiteral() && EqualsIgnoreCase(kid.name, args[i])) { next = &kid; consumed = 1; break; }
            }
            if (!next) {
                for (const Cmd::Node& kid : node->children) {
                    if (kid.IsLiteral()) continue;
                    const int arity = Cmd::TokenArity(kid.type);
                    if (arity == 0) { next = &kid; consumed = args.size() - i; break; }
                    if (i + static_cast<size_t>(arity) <= args.size()) {
                        next = &kid;
                        consumed = static_cast<size_t>(arity);
                        break;
                    }
                }
            }
            if (!next) break;
            node = next;
            i += consumed;
        }
        const std::string typed = CommandText::Join(args, 0);
        for (const Cmd::Node& child : node->children) {
            source.SendSuccess(connection, "/" + typed + " " + Cmd::Usage(child, node->executable, *root), false);
        }
    }

    // ── /version ────────────────────────────────────────────────────────────

    void InfoCommands::ExecuteVersion(const CommandSourceStack& source, const std::vector<std::string>& args,
                                      ServerConnection& connection, PlayerSessionManager& sessionManager) {
        (void)args;
        (void)sessionManager;
        // VersionCommand.dumpVersion over this build's numbers: the game's
        // own version, the Minecraft version its saves are stamped with
        // (DataVersion / series), its wire protocol, and the pack formats it
        // reads (the resource format the client accepts —
        // client/resource/ResourcePacks kGamePackFormat — and the 26.3 data
        // its vanilla worldgen JSON comes from).
        char protocolHex[16];
        std::snprintf(protocolHex, sizeof(protocolHex), "0x%x", static_cast<unsigned>(Network::kProtocolVersion));
        const std::vector<std::string> lines = {
            "Server version info:",
            std::string("id = obeycraft-") + GAME_VERSION,
            std::string("name = ObeyCraft ") + GAME_VERSION + " (Minecraft " + Game::Save::kVersionName + ")",
            "data = " + std::to_string(Game::Save::DataVersion()),
            std::string("series = ") + Game::Save::kVersionSeries,
            "protocol = " + std::to_string(Network::kProtocolVersion) + " (" + protocolHex + ")",
            std::string("build_time = ") + __DATE__ + " " + __TIME__,
            "pack_resource = 76.0",
            "pack_data = 120.0",
            "stable = no",
        };
        // sendSystemMessage: not gated by send_command_feedback.
        for (const std::string& line : lines) connection.SendChatMessage(line, 1);
    }

    // ── /random ─────────────────────────────────────────────────────────────

    void InfoCommands::ExecuteRandom(const CommandSourceStack& source, const std::vector<std::string>& args,
                                     ServerConnection& connection, PlayerSessionManager& sessionManager) {
        const auto usage = [&] {
            CommandText::SendFailure(connection, "Usage: /random (value|roll) <range> [<sequence>]");
            CommandText::SendFailure(connection,
                "       /random reset (*|<sequence>) [<seed> [<includeWorldSeed> [<includeSequenceId>]]]");
        };
        if (args.empty()) { usage(); return; }
        const std::string mode = Lower(args[0]);
        const bool privileged = CommandText::HasGamemasterPermission(connection);

        if (mode == "value" || mode == "roll") {
            if (args.size() < 2) { usage(); return; }
            std::optional<int> lo, hi;
            std::string error;
            if (!ParseIntRange(args[1], lo, hi, error)) { CommandText::SendFailure(connection, error); return; }

            minecraft::XoroshiroRandomSource* random = &LevelRandom();
            if (args.size() >= 3) {
                if (!privileged) {
                    CommandText::SendFailure(connection, "You do not have permission to use a random sequence");
                    return;
                }
                std::string id;
                if (!CommandStorage::NormalizeId(args[2], id)) {
                    CommandText::SendFailure(connection, "Invalid ID: " + args[2]);
                    return;
                }
                const bool fresh = [&] {
                    for (const std::string& known : CommandSavedData::RandomSequenceIds()) if (known == id) return false;
                    return true;
                }();
                random = &CommandSavedData::RandomSequence(id, WorldSeed(source));
                if (fresh) RefreshRandomSyntax();
            }

            // RandomCommand.randomSample.
            const int min = lo.value_or(INT_MIN);
            const int max = hi.value_or(INT_MAX);
            const long long span = static_cast<long long>(max) - static_cast<long long>(min);
            if (span == 0) {
                CommandText::SendFailure(connection, "The range of the random value must be at least 2");
                return;
            }
            if (span >= 2147483647LL) {
                CommandText::SendFailure(connection, "The range of the random value must be at most 2147483646");
                return;
            }
            // Mth.randomBetweenInclusive.
            const int value = random->nextInt(max - min + 1) + min;
            if (mode == "roll") {
                // broadcastSystemMessage: "%s rolled %s (from %s to %s)".
                const std::string who = source.entity ? CommandText::DisplayNameString(*source.entity)
                                                     : source.DisplayName();
                const std::string text = who + " rolled " + std::to_string(value) + " (from " +
                                         std::to_string(min) + " to " + std::to_string(max) + ")";
                CommandText::BroadcastSystem(sessionManager, Network::ChatMessageS2CPacket(text, 1));
            } else {
                source.SendSuccess(connection, "Randomized value: " + std::to_string(value), false);
            }
            return;
        }

        if (mode == "reset") {
            if (!privileged) {
                CommandText::SendFailure(connection, "You do not have permission to use this command");
                return;
            }
            if (args.size() < 2) { usage(); return; }
            // The optional seed, includeWorldSeed, includeSequenceId.
            std::optional<int> salt;
            bool includeWorldSeed = true, includeSequenceId = true;
            if (args.size() >= 3) {
                salt = ParseInt(args[2]);
                if (!salt) { CommandText::SendFailure(connection, "Invalid integer '" + args[2] + "'"); return; }
            }
            if (args.size() >= 4) {
                const auto b = ParseBool(args[3]);
                if (!b) { CommandText::SendFailure(connection, "Invalid boolean, expected 'true' or 'false' but found '" + args[3] + "'"); return; }
                includeWorldSeed = *b;
            }
            if (args.size() >= 5) {
                const auto b = ParseBool(args[4]);
                if (!b) { CommandText::SendFailure(connection, "Invalid boolean, expected 'true' or 'false' but found '" + args[4] + "'"); return; }
                includeSequenceId = *b;
            }

            if (args[1] == "*") {
                // resetAllSequences / resetAllSequencesAndSetNewDefaults.
                if (salt) CommandSavedData::SetRandomSequenceDefaults(*salt, includeWorldSeed, includeSequenceId);
                const int count = CommandSavedData::ClearRandomSequences();
                source.SendSuccess(connection, "Reset " + std::to_string(count) + " random sequence(s)", false);
                RefreshRandomSyntax();
                return;
            }
            std::string id;
            if (!CommandStorage::NormalizeId(args[1], id)) {
                CommandText::SendFailure(connection, "Invalid ID: " + args[1]);
                return;
            }
            if (salt) CommandSavedData::ResetRandomSequence(id, WorldSeed(source), *salt, includeWorldSeed, includeSequenceId);
            else      CommandSavedData::ResetRandomSequence(id, WorldSeed(source));
            source.SendSuccess(connection, "Reset random sequence " + id, false);
            RefreshRandomSyntax();
            return;
        }

        usage();
    }

    Game::Cmd::Node InfoCommands::RandomSyntax() {
        std::vector<std::string> ids = CommandSavedData::RandomSequenceIds();
        Cmd::Node root = Cmd::Root();
        for (const char* mode : {"value", "roll"}) {
            root.Then(Cmd::Literal(mode)
                .Then(Cmd::Argument("range", Cmd::Arg::Word).Suggests({"1..6", "1..100", "0..1", "1..20"}).Executes()
                    .Then(Cmd::Argument("sequence", Cmd::Arg::Word).Suggests(ids).Executes())));
        }
        // reset (*|<sequence>) [<seed> [<includeWorldSeed> [<includeSequenceId>]]]
        const auto seedTail = [] {
            return Cmd::Argument("seed", Cmd::Arg::Integer).Suggests({"0"}).Executes()
                .Then(Cmd::Argument("includeWorldSeed", Cmd::Arg::Bool).Executes()
                    .Then(Cmd::Argument("includeSequenceId", Cmd::Arg::Bool).Executes()));
        };
        Cmd::Node all = Cmd::Literal("*").Executes();
        all.Then(seedTail());
        Cmd::Node sequence = Cmd::Argument("sequence", Cmd::Arg::Word).Suggests(ids).Executes();
        sequence.Then(seedTail());
        root.Then(Cmd::Literal("reset").Then(std::move(all)).Then(std::move(sequence)));
        return root;
    }

    // ── Registration ────────────────────────────────────────────────────────

    void InfoCommands::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("list", ExecuteList,
            Cmd::Root().Executes().Then(Cmd::Literal("uuids").Executes()));
        dispatcher.RegisterCommand("help", ExecuteHelp,
            Cmd::Root().Executes().Then(Cmd::Argument("command", Cmd::Arg::Command).Executes()));
        dispatcher.RegisterCommand("version", ExecuteVersion, Cmd::Root().Executes());
        dispatcher.RegisterCommand("random", ExecuteRandom, RandomSyntax());
    }

} // namespace Server
