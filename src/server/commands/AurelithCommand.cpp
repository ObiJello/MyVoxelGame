// File: src/server/commands/AurelithCommand.cpp
#include "AurelithCommand.hpp"
#include "../IntegratedServer.hpp"
#include "../level/AurelithCities.hpp"
#include "../level/ServerLevel.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/core/Log.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"
#include "common/world/block/AurelithQuestBlocks.hpp"
#include "common/world/level/AurelithQuest.hpp"
#include "common/world/level/DimensionId.hpp"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace Server {

    namespace {

        namespace A = Game::Aurelith;

        std::string ToLower(std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        const char* StateName(A::CityState s) {
            switch (s) {
                case A::CityState::Dormant:   return "dormant";
                case A::CityState::Awakening: return "awakening";
                case A::CityState::Contested: return "contested (the Unsung)";
                case A::CityState::Awakened:  return "awakened";
            }
            return "?";
        }

        // Where /aurelith tp puts you: design offsets from the Heart and a
        // height over the street (tools/gen_aurelith.py build_quest's
        // QUEST_SPOTS and the plaza's landmarks — keep them in step).
        struct Spot { const char* name; int dx, dz, aboveStreet; const char* what; };
        constexpr Spot kSpots[] = {
            { "heart",        0,    7,   2, "the Heart's dais" },
            { "podium",       0,  -11,   2, "the Conductor's Podium (the four sockets)" },
            { "soprano",    -51,  -60,  22, "the Archive's third gallery, before the sealed cell (Soprano)" },
            { "alto",       -16,  -41,   1, "the Hall of Instruments' tuned cabinet (Alto)" },
            { "tenor",       44,  -18,  30, "the Tuners' mast, on the perch (Tenor)" },
            { "bass",        -6,    2,  -9, "the Vault of the Chord, before the spring (Bass)" },
            { "vault",      -22,    1,   1, "the hatch behind the Bass statue" },
            { "gate_soprano", 0,  -92,   1, "inside the Soprano Gate" },
            { "gate_alto",   92,    0,   1, "inside the Alto Gate" },
            { "gate_tenor",   0,   92,   1, "inside the Tenor Gate" },
            { "gate_bass",  -92,    0,   1, "inside the Bass Gate" },
        };

        std::string SpotList() {
            std::string out;
            for (const Spot& s : kSpots) {
                if (!out.empty()) out += ", ";
                out += s.name;
            }
            return out;
        }

    } // namespace

    void AurelithCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("aurelith", AurelithCommand::Execute);
    }

    void AurelithCommand::Execute(const CommandSourceStack& source,
                                  const std::vector<std::string>& args,
                                  ServerConnection& connection,
                                  PlayerSessionManager& sessionManager) {
        const std::string sub = args.empty() ? std::string() : ToLower(args[0]);
        auto session = source.sender ? sessionManager.GetSession(source.sender->getPlayerId()) : nullptr;

        if (sub == "keys" || sub == "heldnote") {
            if (!session) return;
            if (sub == "keys") {
                for (A::Voice v : A::kChordOrder) session->GiveItem(Game::ItemStack(A::KeyOf(v), 1));
                connection.SendChatMessage("The four voice keys (from the floor to the crown: Bass, Tenor, "
                                           "Alto, Soprano)", 1);
            } else {
                session->GiveItem(Game::ItemStack(Game::Items::HeldNote, 1));
                connection.SendChatMessage("The Held Note", 1);
            }
            return;
        }

        ServerLevel* hush = g_integratedServer ? g_integratedServer->GetLevel(Game::DimensionId::Hush) : nullptr;
        AurelithCities* cities = hush ? hush->Aurelith() : nullptr;
        if (!cities || source.dimension != Game::DimensionId::Hush) {
            connection.SendChatMessage("Aurelith is in the Hush: go through a hush portal, then "
                                       "/locate structure minecraft:aurelith", 1);
            return;
        }
        AurelithCities::Summary city;
        if (!cities->Nearest(source.position, AurelithCities::kSyncRange, city)) {
            connection.SendChatMessage("No Aurelith within 640 blocks is known yet (a city is known once "
                                       "the chunk of its Heart has loaded). Try /locate structure minecraft:aurelith", 1);
            return;
        }

        if (sub.empty() || sub == "status") {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "Aurelith: Heart at %d %d %d (%.0f blocks), rotation %d, %s "
                          "(%lld ticks in)%s", city.heart.x, city.heart.y, city.heart.z, city.distance,
                          city.rotation, StateName(city.state), static_cast<long long>(city.stageTicks),
                          city.heldNoteGiven ? ", the Held Note given" : "");
            connection.SendChatMessage(buf, 1);
            return;
        }

        if (sub == "tp") {
            const std::string where = args.size() > 1 ? ToLower(args[1]) : std::string();
            for (const Spot& s : kSpots) {
                if (where != s.name) continue;
                glm::ivec3 p;
                if (!cities->DesignPoint(city.heart, glm::ivec2(s.dx, s.dz), s.aboveStreet, p)) {
                    connection.SendChatMessage("That city's rotation is unknown (generated before the quest)", 1);
                    return;
                }
                connection.Teleport(p.x + 0.5, p.y, p.z + 0.5, 0.0f, 0.0f);
                connection.SendChatMessage(std::string("To ") + s.what, 1);
                return;
            }
            connection.SendChatMessage("Usage: /aurelith tp <" + SpotList() + ">", 1);
            return;
        }

        if (sub == "sing") {
            connection.SendChatMessage(cities->DebugSingTheChord(city.heart), 1);
            return;
        }
        if (sub == "advance") {
            connection.SendChatMessage(cities->DebugAdvance(city.heart), 1);
            return;
        }
        if (sub == "reset") {
            connection.SendChatMessage(cities->DebugReset(city.heart, source.sender), 1);
            return;
        }

        connection.SendChatMessage("Usage: /aurelith [status | tp <spot> | keys | sing | advance | reset | "
                                   "heldnote]", 1);
    }

} // namespace Server
