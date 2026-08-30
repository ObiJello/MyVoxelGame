// File: src/server/commands/SummonCommand.cpp
#include "SummonCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../IntegratedServer.hpp"
#include "common/entity/EntityType.hpp"
#include "common/core/Log.hpp"
#include "CommandCoords.hpp"
#include "EntitySelector.hpp"

#include <stdexcept>
#include <vector>

#include <algorithm>
#include <cctype>
#include <string>

namespace Server {

    namespace {
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

    void SummonCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("summon", SummonCommand::Execute);
    }

    void SummonCommand::Execute(ServerPlayer& sender,
                                const std::vector<std::string>& rawArgs,
                                ServerConnection& connection,
                                PlayerSessionManager& /*sessionManager*/) {
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
        std::vector<std::string> args;
        args.reserve(rawArgs.size());
        for (const std::string& token : rawArgs) {
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
                "Usage: /summon <entity> [count] [<x> <y> <z>] [fuse=n] [delay=n]", 1);
            return;
        }



        // Accept the vanilla "minecraft:" prefix so a command copied from the
        // wiki works unchanged.
        std::string slug = args[0];
        if (slug.rfind("minecraft:", 0) == 0) slug = slug.substr(10);
        std::transform(slug.begin(), slug.end(), slug.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        Game::EntityTypeId type{};
        bool found = false;
        for (uint16_t i = 0; i < static_cast<uint16_t>(Game::EntityTypeId::Count); ++i) {
            const auto candidate = static_cast<Game::EntityTypeId>(i);
            if (Game::GetEntityTypeInfo(candidate).slug == slug) {
                type = candidate;
                found = true;
                break;
            }
        }
        if (!found) {
            connection.SendChatMessage("Unknown entity type: " + args[0], 1);
            return;
        }

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
                "Usage: /summon <entity> [count] [<x> <y> <z>]", 1);
            return;
        }

        glm::dvec3 pos = sender.getPosition();
        if (coordAt != 0) {
            CommandSource source;
            source.sender   = &sender;
            source.position = sender.getPosition();
            CommandRotation rot{ sender.getYaw(), sender.getPitch() };
            std::string error;
            if (!ParseVec3(args[coordAt], args[coordAt + 1], args[coordAt + 2],
                           source, rot, pos, error)) {
                connection.SendChatMessage(error, 1);
                return;
            }
        }

        if (!g_integratedServer) {
            connection.SendChatMessage("No server", 1);
            return;
        }

        const int spawned = g_integratedServer->SummonMobs(type, pos, count, options);
        if (spawned == 0) {
            connection.SendChatMessage("Failed to summon " + slug, 1);
            return;
        }

        connection.SendChatMessage("Summoned " + std::to_string(spawned) + " " + slug, 1);
        Log::Info("[SummonCommand] %s summoned %d %s",
                  sender.getName().c_str(), spawned, slug.c_str());
    }

} // namespace Server
