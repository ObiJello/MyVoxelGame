// File: src/server/commands/LocateCommand.cpp
#include "LocateCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "common/network/PacketTypes.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../session/PlayerSession.hpp"
#include "../player/ServerPlayer.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "../level/LocateFinder.hpp"
#include "../level/NetherPortalIndex.hpp"
#include "common/world/portal/PortalFamily.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/core/Log.hpp"

#include "levelgen/structure/StructureSet.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace Server {

    void LocateCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("locate", LocateCommand::Execute);
    }

    namespace {
        // LocateCommand.showLocateResult: "commands.locate.*.success" =
        // "The nearest %s is at %s (%s blocks away)", the coordinates in
        // square brackets ("chat.coordinates" = "%s, %s, %s"), green, with
        // the hover "Click to teleport". MC prints "~" for a structure's Y
        // (and a POI's), whose distance is horizontal; this game prints the
        // terrain surface for structures and biomes (LocateSurfaceY) so the
        // click lands on the ground — a POI keeps "~", its position being a
        // real block. MC's click SUGGESTS "/tp @s x y z" into the chat box;
        // here it RUNS it, which is what the click reads as.
        void SendLocateResult(ServerConnection& connection, const std::string& foundName,
                              const glm::ivec3& pos, bool includeY, int distance) {
            const std::string y = includeY ? std::to_string(pos.y) : std::string("~");
            const std::string coords = std::to_string(pos.x) + ", " + y + ", " + std::to_string(pos.z);
            const std::string tp = "/tp @s " + std::to_string(pos.x) + " " + y + " " + std::to_string(pos.z);
            Network::ChatMessageS2CPacket packet;
            packet.position = 1;
            // MC ComponentUtils.wrapInSquareBrackets(coordinates).withStyle(
            // GREEN + click + hover): the brackets share the green and the
            // click, so the whole "[x, y, z]" is one link.
            packet.segments.push_back(Network::ChatSegmentData{"The nearest " + foundName + " is at ", 0xFFFFFFFF, Network::ChatClickAction::None, "", ""});
            packet.segments.push_back(Network::ChatSegmentData{"[" + coords + "]", 0xFF55FF55,   // ChatFormatting.GREEN
                                                               Network::ChatClickAction::RunCommand, tp, "Click to teleport"});
            packet.segments.push_back(Network::ChatSegmentData{" (" + std::to_string(distance) + " blocks away)", 0xFFFFFFFF, Network::ChatClickAction::None, "", ""});
            connection.SendChatMessage(packet);
        }
        int Distance(const glm::ivec3& from, const glm::ivec3& to, bool includeY) {
            const double dx = to.x - from.x, dy = includeY ? (to.y - from.y) : 0.0, dz = to.z - from.z;
            return static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy + dz * dz)));
        }
        // The printable name: the id as given, plus the resolved element for
        // a tag — "#minecraft:village (minecraft:village_plains)".
        std::string FoundName(const std::string& asked, bool isTag, const std::string& found) {
            if (!isTag) return found;
            const std::string shown = asked[0] == '#' ? asked : "#" + asked;
            return shown + " (" + found + ")";
        }
        std::string Quoted(const std::string& s) { return "\"" + s + "\""; }

        // Not in MC: when the search misses because the element only
        // generates in another dimension, say which one and how to get
        // there — a gray line after MC's own "not found" message.
        std::string_view DimensionTitle(Game::DimensionId d) {
            switch (d) {
                case Game::DimensionId::Overworld:      return "the Overworld (/dimension overworld)";
                case Game::DimensionId::Nether:         return "the Nether (/dimension nether)";
                case Game::DimensionId::End:            return "the End (/dimension end)";
                case Game::DimensionId::Hush:           return "the Hush (/dimension hush)";
                case Game::DimensionId::TwilightForest: return "the Twilight Forest (/dimension twilight)";
                case Game::DimensionId::Aether:         return "the Aether (/dimension aether)";
            }
            return "another dimension";
        }
        void SendGrayLine(ServerConnection& connection, const std::string& text) {
            Network::ChatMessageS2CPacket packet;
            packet.position = 1;
            packet.segments.push_back(Network::ChatSegmentData{text, 0xFFAAAAAA,   // ChatFormatting.GRAY
                                                               Network::ChatClickAction::None, "", ""});
            connection.SendChatMessage(packet);
        }
        template <typename Has>
        void SendElsewhereHint(ServerConnection& connection, Game::DimensionId here, Has&& has) {
            if (!g_integratedServer) return;
            ServerLevel* current = g_integratedServer->GetLevel(here);
            if (current && has(*current)) return;   // it is here, just too far
            std::string where;
            for (Game::DimensionId d : Game::kAllDimensions) {
                if (d == here) continue;
                ServerLevel* other = g_integratedServer->GetLevel(d);
                if (!other || !has(*other)) continue;
                if (!where.empty()) where += ", ";
                where += DimensionTitle(d);
            }
            if (where.empty()) return;
            SendGrayLine(connection, "It generates in " + where + ", not here.");
        }
    }

    void LocateCommand::Execute(const CommandSourceStack& source,
                                const std::vector<std::string>& args,
                                ServerConnection& connection,
                                PlayerSessionManager& /*sessionManager*/) {
        if (args.size() < 2) {
            connection.SendChatMessage("Usage: /locate <structure|biome|poi> <id>", 1);
            return;
        }
        const std::string& kind = args[0];
        const std::string& asked = args[1];
        if (!g_integratedServer) {
            connection.SendChatMessage("Locate is unavailable (no server)", 1);
            return;
        }
        // The stack's level and position — `/execute in the_nether positioned
        // 0 64 0 run locate structure fortress` searches from there.
        ServerLevel* level = g_integratedServer->GetLevel(source.dimension);
        if (!level) {
            connection.SendChatMessage("Locate is unavailable (your level is not ready)", 1);
            return;
        }
        const glm::dvec3 p = source.position;
        const glm::ivec3 from(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), static_cast<int>(std::floor(p.z)));

        if (kind == "structure") {
            // ResourceOrTagKeyArgument: an unknown id or tag is
            // ERROR_STRUCTURE_INVALID ("There is no structure with type").
            // A bare name finds its namespace (CanonicalWorldgenId), so
            // `lich_tower` is twilightforest:lich_tower.
            const std::string id = CanonicalWorldgenId("structure", asked, level);
            const bool isTag = id[0] == '#';
            const std::vector<std::string> ids = ResolveStructureIdOrTag(id);
            bool known = !ids.empty();
            if (!isTag) {
                try { (void)minecraft::levelgen::structure::StructureSets::structureByName(ids.front()); }
                catch (const std::exception&) { known = false; }
            }
            if (!known) {
                connection.SendChatMessage("There is no structure with type " + Quoted(id), 1);
                return;
            }
            const auto t0 = std::chrono::steady_clock::now();
            const auto found = FindNearestStructure(*level, ids, from, 100);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (!found) {
                connection.SendChatMessage("Could not find a structure of type " + Quoted(id) + " nearby", 1);
                if (std::none_of(ids.begin(), ids.end(), [](const std::string& s) { return StructureIsGenerated(s); })) {
                    SendGrayLine(connection, (isTag ? "None of " + id + "'s structures is" : id + " is") +
                                             std::string(" generated by this version of the game yet."));
                    return;
                }
                SendElsewhereHint(connection, source.dimension, [&](ServerLevel& other) {
                    for (const std::string& s : ids) if (LevelHasStructure(other, s)) return true;
                    return false;
                });
                return;
            }
            const std::string name = FoundName(id, isTag, found->id);
            // The finder's Y is the placement's locate offset (0); the
            // distance stays horizontal as MC's is.
            const glm::ivec3 pos(found->pos.x, LocateSurfaceY(*level, found->pos.x, found->pos.z, from.y), found->pos.z);
            SendLocateResult(connection, name, pos, true, Distance(from, pos, false));
            Log::Info("Locating element %s took %lld ms", name.c_str(), static_cast<long long>(ms));
            return;
        }

        if (kind == "biome") {
            // ResourceOrTagArgument: an unknown element or tag fails at parse
            // time (argument.resource.not_found / resource_tag.not_found).
            const std::string id = CanonicalWorldgenId("biome", asked, level);
            const bool isTag = id[0] == '#';
            if (isTag ? !IsWorldgenTag("biome", id.substr(1))
                      : !std::binary_search(AllBiomeIds().begin(), AllBiomeIds().end(), id)) {
                connection.SendChatMessage(std::string(isTag ? "Can't find tag '" : "Can't find element '") +
                                           (isTag ? id.substr(1) : id) + "' of type 'minecraft:worldgen/biome'", 1);
                return;
            }
            const auto ids = ResolveBiomeIdOrTag(id);
            const auto t0 = std::chrono::steady_clock::now();
            std::optional<LocateResult> found;
            if (!ids.empty()) found = FindClosestBiome(*level, ids, from, 6400, 32, 64);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (!found) {
                connection.SendChatMessage("Could not find a biome of type " + Quoted(id) + " within reasonable distance", 1);
                SendElsewhereHint(connection, source.dimension, [&](ServerLevel& other) {
                    for (const std::string& b : ids) if (LevelHasBiome(other, b)) return true;
                    return false;
                });
                return;
            }
            const std::string name = FoundName(id, isTag, found->id);
            SendLocateResult(connection, name, found->pos, true, Distance(from, found->pos, true));
            Log::Info("Locating element %s took %lld ms", name.c_str(), static_cast<long long>(ms));
            return;
        }

        if (kind == "poi") {
            // MC asks the POI manager (workstations, beds, bells, portals,
            // lodestones, hives…) within 256 blocks. This engine indexes one
            // kind of point of interest: frame portals, one index per
            // family (NetherPortalIndex, the portal-linking search), so
            // those are the ones that can be found.
            std::string id = asked;
            if (id.rfind("minecraft:", 0) == 0) id.erase(0, 10);
            const Game::PortalFamily* family =
                id == "nether_portal" ? &Game::NetherFamily()
              : id == "hush_portal"   ? &Game::HushFamily()
                                      : nullptr;
            if (family) {
                const std::string name = "minecraft:" + id;
                const auto t0 = std::chrono::steady_clock::now();
                const std::optional<glm::ivec3> portal = level->Portals(family->id).FindClosest(*level->World(), from, 256);
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                if (!portal) {
                    connection.SendChatMessage("Could not find a point of interest of type " + Quoted(asked) + " within reasonable distance", 1);
                    return;
                }
                SendLocateResult(connection, name, *portal, false, Distance(from, *portal, false));
                Log::Info("Locating element %s took %lld ms", name.c_str(), static_cast<long long>(ms));
                return;
            }
            connection.SendChatMessage("Could not find a point of interest of type " + Quoted(asked) +
                                       " within reasonable distance (only nether_portal is tracked in this game)", 1);
            return;
        }

        connection.SendChatMessage("Usage: /locate <structure|biome|poi> <id>", 1);
    }

} // namespace Server
