// File: src/server/commands/SummonCommand.cpp
#include "SummonCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../IntegratedServer.hpp"
#include "common/entity/EntityType.hpp"
#include "common/core/Log.hpp"
#include "CommandCoords.hpp"
#include "EntitySelector.hpp"
#include "SnbtParser.hpp"

#include <stdexcept>
#include <vector>

#include <algorithm>
#include <cctype>
#include <string>

namespace Server {

    namespace {
        // Whether every '{' / '[' in `text` outside a quoted string is closed.
        bool BracketsBalanced(const std::string& text) {
            int depth = 0;
            char quote = 0;
            for (size_t i = 0; i < text.size(); ++i) {
                const char c = text[i];
                if (quote) {
                    if (c == '\\') ++i;
                    else if (c == quote) quote = 0;
                    continue;
                }
                if (c == '"' || c == '\'') quote = c;
                else if (c == '{' || c == '[') ++depth;
                else if (c == '}' || c == ']') --depth;
            }
            return depth <= 0 && quote == 0;
        }

        // Shared by the two count-bearing arities. Returns -1 after reporting.
        int ParseCount(const std::string& token, ServerConnection& connection) {
            try {
                size_t used = 0;
                const int n = std::stoi(token, &used);
                if (used != token.size()) throw std::invalid_argument("trailing");

                // NO upper bound, by request. There was a clamp to 512 here.
                // It was an arbitrary number — vanilla /summon has no count
                // argument at all, so there was nothing to match — and worse,
                // it truncated SILENTLY: `/summon tnt 2000` spawned 512 and
                // said nothing, which reads as a bug rather than a limit.
                //
                // It was also guarding the wrong thing. The cap was justified
                // as "each one ticks and is tracked", but what actually fell
                // over at a few hundred entities was the packet path, and that
                // was a stuck needsSync flag in ServerEntityTracker (an entity
                // pushed once sent a velocity packet every tick forever), not
                // entity count. That is fixed; the count is the caller's
                // problem now.
                //
                // The lower bound is a REJECTION rather than a clamp, for the
                // same reason the old upper clamp was wrong: quietly turning
                // `-5` into `1` hides the mistake instead of reporting it.
                if (n < 1) {
                    connection.SendChatMessage(
                        "Count must be at least 1: " + token, 1);
                    return -1;
                }
                return n;
            } catch (...) {
                connection.SendChatMessage("Invalid count: " + token, 1);
                return -1;
            }
        }
    } // namespace

    bool SummonCommand::ParseEntityType(const std::string& text, Game::EntityTypeId& out) {
        // Accept the vanilla "minecraft:" prefix so a command copied from the
        // wiki works unchanged.
        std::string slug = text;
        if (slug.rfind("minecraft:", 0) == 0) slug = slug.substr(10);
        std::transform(slug.begin(), slug.end(), slug.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (uint16_t i = 0; i < static_cast<uint16_t>(Game::EntityTypeId::Count); ++i) {
            const auto candidate = static_cast<Game::EntityTypeId>(i);
            if (Game::GetEntityTypeInfo(candidate).slug == slug) {
                // MC EntityType.canSummon: the fishing bobber is noSummon.
                if (candidate == Game::EntityTypeId::FishingBobber) return false;
                out = candidate;
                return true;
            }
        }
        return false;
    }

    void SummonCommand::Register(CommandDispatcher& dispatcher) {
        using namespace Game::Cmd;
        // MC: summon <entity> [<pos> [<nbt>]]. The engine adds a count before
        // the position and the TNT options fuse=/delay= after it.
        const auto option = [] {
            return Argument("option", Arg::Word).Suggests({"fuse=80", "delay=1"}).Executes();
        };
        const auto posThenNbt = [&] {
            return Argument("pos", Arg::Vec3).Executes()
                .Then(Argument("nbt", Arg::EntityNbt).Executes().Then(option()))
                .Then(option());
        };
        dispatcher.RegisterCommand("summon", SummonCommand::Execute,
            Root().Then(Argument("entity", Arg::EntityType).Executes()
                .Then(posThenNbt())
                .Then(Argument("count", Arg::Integer).Suggests({"1", "10", "100"}).Executes()
                    .Then(posThenNbt())
                    .Then(Argument("nbt", Arg::EntityNbt).Executes().Then(option())))
                .Then(Argument("nbt", Arg::EntityNbt).Executes().Then(option()))));
    }

    void SummonCommand::Execute(const CommandSourceStack& source,
                                const std::vector<std::string>& rawArgs,
                                ServerConnection& connection,
                                PlayerSessionManager& /*sessionManager*/) {
        ServerPlayer& sender = *source.sender;

        // ── <nbt> (MC CompoundTagArgument) ─────────────────────────────────
        //
        // The dispatcher splits on whitespace, and a compound may hold spaces
        // (`{CustomName:"Two Words", NoAI:1b}`), so the argument runs from the
        // first token that opens a '{' until its braces balance, quotes
        // respected. It is pulled out before anything else so a '=' inside it
        // is never mistaken for a fuse=/delay= option.
        std::vector<std::string> tokens;
        std::shared_ptr<const ::World::NBTTagCompound> nbt;
        for (size_t i = 0; i < rawArgs.size(); ++i) {
            if (nbt || rawArgs[i].empty() || rawArgs[i][0] != '{' || i == 0) {
                tokens.push_back(rawArgs[i]);
                continue;
            }
            std::string text = rawArgs[i];
            while (!BracketsBalanced(text) && i + 1 < rawArgs.size()) text += " " + rawArgs[++i];
            std::string error;
            auto compound = Snbt::ParseCompound(text, error);
            if (!compound) {
                connection.SendChatMessage(error, 1);
                return;
            }
            nbt = std::move(compound);
        }

        // ── Keyword overrides (MC's NBT compound, without an NBT parser) ───
        //
        // Vanilla spells per-entity overrides as `/summon tnt ~ ~ ~ {Fuse:40}`.
        // There is no NBT parser here, so the two worth having are `key=value`
        // tokens, pulled out BEFORE the positional arity is worked out so they
        // can be written anywhere after the entity:
        //
        //   fuse=<ticks>    the first TNT's fuse            (MC's {Fuse:n}, default 80)
        //   delay=<ticks>   added per successive TNT        (no MC equivalent)
        //
        // `delay` is the one with no vanilla counterpart: MC would need a
        // separate command per fuse value to stagger a stack.
        SummonOptions options;
        options.nbt = nbt;
        std::vector<std::string> args;
        args.reserve(tokens.size());
        for (const std::string& token : tokens) {
            const size_t eq = token.find('=');
            if (eq == std::string::npos || eq == 0) { args.push_back(token); continue; }

            const std::string key = token.substr(0, eq);
            const std::string value = token.substr(eq + 1);
            int parsed = 0;
            try {
                size_t used = 0;
                parsed = std::stoi(value, &used);
                if (used != value.size()) throw std::invalid_argument("trailing");
            } catch (...) {
                connection.SendChatMessage("Invalid value for " + key + ": " + value, 1);
                return;
            }

            if (key == "fuse") {
                if (parsed < 1) {
                    connection.SendChatMessage("fuse must be at least 1 tick", 1);
                    return;
                }
                options.tntFuse = parsed;
            } else if (key == "delay") {
                options.tntFuseStep = parsed;
            } else {
                connection.SendChatMessage("Unknown option: " + key +
                                           " (expected fuse= or delay=)", 1);
                return;
            }
        }
        if (args.empty()) {
            connection.SendChatMessage(
                "Usage: /summon <entity> [count] [<x> <y> <z>] [<nbt>] [fuse=n] [delay=n]", 1);
            return;
        }



        Game::EntityTypeId type{};
        if (!ParseEntityType(args[0], type)) {
            connection.SendChatMessage("Unknown entity type: " + args[0], 1);
            return;
        }
        const std::string slug(Game::GetEntityTypeInfo(type).slug);

        // ── Argument shapes ────────────────────────────────────────────────
        //
        // Vanilla is `/summon <entity> [<x> <y> <z>] [nbt]` — ONE entity, no
        // count. This engine already had `/summon <entity> [count]`, which is
        // not vanilla but is far more useful for testing, so both are kept and
        // the arity tells them apart:
        //
        //   /summon zombie                 -> 1, at the sender
        //   /summon zombie 5               -> 5, at the sender
        //   /summon zombie 10 64 -20       -> 1, at that position   (VANILLA)
        //   /summon zombie ~ ~5 ~          -> 1, five blocks up     (VANILLA)
        //   /summon zombie 5 10 64 -20     -> 5, at that position
        //
        // Three trailing tokens are always a position, never a count plus two
        // strays — so a command copied off the wiki does the vanilla thing.
        const size_t rest = args.size() - 1;
        int    count    = 1;
        size_t coordAt  = 0;              // 0 = no coordinates given

        if (rest == 1) {
            count = ParseCount(args[1], connection);
            if (count < 0) return;
        } else if (rest == 3) {
            coordAt = 1;
        } else if (rest == 4) {
            count = ParseCount(args[1], connection);
            if (count < 0) return;
            coordAt = 2;
        } else if (rest != 0) {
            connection.SendChatMessage(
                "Usage: /summon <entity> [count] [<x> <y> <z>] [<nbt>]", 1);
            return;
        }

        // MC: the source's position (which `/execute positioned` may have
        // moved) and the source's LEVEL — a summon from the Nether lands in
        // the Nether.
        glm::dvec3 pos = source.position;
        if (coordAt != 0) {
            std::string error;
            if (!ParseVec3(args[coordAt], args[coordAt + 1], args[coordAt + 2],
                           source, source.rotation, pos, error)) {
                connection.SendChatMessage(error, 1);
                return;
            }
        }

        if (!g_integratedServer) {
            connection.SendChatMessage("No server", 1);
            return;
        }

        const int spawned = g_integratedServer->SummonMobs(type, pos, count, options, source.dimension);
        if (spawned == 0) {
            // commands.summon.failed
            connection.SendChatMessage("Unable to summon entity", 1);
            return;
        }

        source.SendSuccess(connection, "Summoned " + std::to_string(spawned) + " " + slug, true);
        Log::Info("[SummonCommand] %s summoned %d %s",
                  sender.getName().c_str(), spawned, slug.c_str());
    }

} // namespace Server
