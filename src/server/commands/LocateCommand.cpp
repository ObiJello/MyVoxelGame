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
#include "common/world/level/World.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/core/Log.hpp"

#include "levelgen/structure/StructureSet.h"

#include <chrono>
#include <cmath>
#include <exception>
#include <optional>
#include <string>

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
            // Like /seed's layout: white brackets around the green, clickable
            // coordinates (MC colours the brackets too; this game keeps them
            // white so the clickable part stands out).
            packet.segments.push_back(Network::ChatSegmentData{"The nearest " + foundName + " is at ", 0xFFFFFFFF, Network::ChatClickAction::None, "", ""});
            packet.segments.push_back(Network::ChatSegmentData{"[", 0xFFFFFFFF, Network::ChatClickAction::None, "", ""});
            packet.segments.push_back(Network::ChatSegmentData{coords, 0xFF55FF55,   // ChatFormatting.GREEN
                                                               Network::ChatClickAction::RunCommand, tp, "Click to teleport"});
            packet.segments.push_back(Network::ChatSegmentData{"]", 0xFFFFFFFF, Network::ChatClickAction::None, "", ""});
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
    }

    void LocateCommand::Execute(ServerPlayer& sender,
                                const std::vector<std::string>& args,
                                ServerConnection& connection,
                                PlayerSessionManager& sessionManager) {
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
        auto session = sessionManager.GetSession(sender.getPlayerId());
        ServerLevel* level = session ? g_integratedServer->GetLevel(Game::DimensionFromRaw(session->GetDimensionId())) : nullptr;
        if (!level) {
            connection.SendChatMessage("Locate is unavailable (your level is not ready)", 1);
            return;
        }
        const glm::dvec3 p = sender.getPosition();
        const glm::ivec3 from(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), static_cast<int>(std::floor(p.z)));

        if (kind == "structure") {
            // ResourceOrTagKeyArgument: an unknown id is ERROR_STRUCTURE_INVALID
            // at parse time; an empty tag is "not found".
            const bool isTag = IsWorldgenTag("structure", asked);
            const std::vector<std::string> ids = ResolveStructureIdOrTag(asked);
            if (!isTag) {
                try { (void)minecraft::levelgen::structure::StructureSets::structureByName(ids.front()); }
                catch (const std::exception&) {
                    connection.SendChatMessage("There is no structure with type " + Quoted(asked), 1);
                    return;
                }
            }
            const auto t0 = std::chrono::steady_clock::now();
            const auto found = FindNearestStructure(*level, ids, from, 100);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (!found) {
                connection.SendChatMessage("Could not find a structure of type " + Quoted(asked) + " within reasonable distance", 1);
                return;
            }
            const std::string name = FoundName(asked, isTag, found->id);
            // The finder's Y is the placement's locate offset (0); the
            // distance stays horizontal as MC's is.
            const glm::ivec3 pos(found->pos.x, LocateSurfaceY(*level, found->pos.x, found->pos.z, from.y), found->pos.z);
            SendLocateResult(connection, name, pos, true, Distance(from, pos, false));
            Log::Info("Locating element %s took %lld ms", name.c_str(), static_cast<long long>(ms));
            return;
        }

        if (kind == "biome") {
            const bool isTag = IsWorldgenTag("biome", asked);
            const auto ids = ResolveBiomeIdOrTag(asked);
            const auto t0 = std::chrono::steady_clock::now();
            std::optional<LocateResult> found;
            if (!ids.empty()) found = FindClosestBiome(*level, ids, from, 6400, 32, 64);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (!found) {
                connection.SendChatMessage("Could not find a biome of type " + Quoted(asked) + " within reasonable distance", 1);
                return;
            }
            const std::string name = FoundName(asked, isTag, found->id);
            SendLocateResult(connection, name, found->pos, true, Distance(from, found->pos, true));
            Log::Info("Locating element %s took %lld ms", name.c_str(), static_cast<long long>(ms));
            return;
        }

        if (kind == "poi") {
            // MC asks the POI manager (workstations, beds, bells, portals,
            // lodestones, hives…) within 256 blocks. This engine indexes one
            // point of interest: nether portals (NetherPortalIndex, the
            // portal-linking search), so that is the one that can be found.
            std::string id = asked;
            if (id.rfind("minecraft:", 0) == 0) id.erase(0, 10);
            if (id == "nether_portal") {
                const auto t0 = std::chrono::steady_clock::now();
                const std::optional<glm::ivec3> portal = level->Portals().FindClosest(*level->World(), from, 256);
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                if (!portal) {
                    connection.SendChatMessage("Could not find a point of interest of type " + Quoted(asked) + " within reasonable distance", 1);
                    return;
                }
                SendLocateResult(connection, "minecraft:nether_portal", *portal, false, Distance(from, *portal, false));
                Log::Info("Locating element minecraft:nether_portal took %lld ms", static_cast<long long>(ms));
                return;
            }
            connection.SendChatMessage("Could not find a point of interest of type " + Quoted(asked) +
                                       " within reasonable distance (only nether_portal is tracked in this game)", 1);
            return;
        }

        connection.SendChatMessage("Usage: /locate <structure|biome|poi> <id>", 1);
    }

} // namespace Server
