#include "ShapeCommand.hpp"
#include "CommandCoords.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../IntegratedServer.hpp"
#include "common/core/Log.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace Server {

    namespace {

        std::string Lower(std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // Linear scan of the registry by (case-insensitive) name. ~1000
        // entries once per command — nothing worth an index.
        bool BlockFromName(const std::string& raw, Game::BlockID& out) {
            const std::string want = Lower(raw);
            if (want == "air") { out = Game::BlockID::Air; return true; }
            for (size_t i = 0; i < static_cast<size_t>(Game::BlockID::Count); ++i) {
                const auto id = static_cast<Game::BlockID>(i);
                std::string name = Lower(Game::BlockRegistry::Get(id).name);
                // Display names with spaces ("Infested Stone") are typed with
                // underscores — a chat token cannot contain a space.
                for (char& c : name) if (c == ' ') c = '_';
                if (name == want) {
                    out = id;
                    return true;
                }
            }
            return false;
        }

        bool ParseInt(const std::string& s, int& out) {
            char* end = nullptr;
            const long v = std::strtol(s.c_str(), &end, 10);
            if (!end || *end != '\0') return false;
            out = static_cast<int>(v);
            return true;
        }

        void Reply(ServerConnection& connection, const std::string& msg) {
            connection.SendChatMessage(msg, 1);
        }

        const char* kUsage =
            "/shape <block> <cube s|box sx sy sz|wall w [h]|sphere r|dome r|"
            "cylinder r [h]|pyramid base> [hollow|frame|checker|spaced=N] [at x y z]";

    } // namespace

    void ShapeCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("shape", ShapeCommand::Execute);
    }

    void ShapeCommand::Execute(ServerPlayer& sender,
                               const std::vector<std::string>& args,
                               ServerConnection& connection,
                               PlayerSessionManager& /*sessionManager*/) {
        auto* server = g_integratedServer.get();
        if (!server) return;

        if (args.size() < 3) { Reply(connection, kUsage); return; }

        Game::BlockID block;
        if (!BlockFromName(args[0], block)) {
            Reply(connection, "Unknown block '" + args[0] + "'");
            return;
        }

        ShapeJobRequest job;
        job.block = block;

        const std::string form = Lower(args[1]);
        size_t i = 2;
        std::vector<int> sizes;
        while (i < args.size()) {
            int v;
            if (!ParseInt(args[i], v)) break;
            sizes.push_back(v);
            ++i;
        }

        if      (form == "cube")     job.form = ShapeForm::Box;
        else if (form == "box")      job.form = ShapeForm::Box;
        else if (form == "wall")     job.form = ShapeForm::Wall;
        else if (form == "sphere")   job.form = ShapeForm::Sphere;
        else if (form == "dome")     job.form = ShapeForm::Dome;
        else if (form == "cylinder") job.form = ShapeForm::Cylinder;
        else if (form == "pyramid")  job.form = ShapeForm::Pyramid;
        else { Reply(connection, kUsage); return; }

        for (const int v : sizes) {
            if (v < 1 || v > 512) {
                Reply(connection, "Sizes must be 1..512");
                return;
            }
        }

        // Fill the dimensions per form. `a`,`b`,`c` are form-specific.
        switch (job.form) {
            case ShapeForm::Box:
                if (form == "cube") {
                    if (sizes.size() != 1) { Reply(connection, "cube wants one size"); return; }
                    job.a = job.b = job.c = sizes[0];
                } else {
                    if (sizes.size() != 3) { Reply(connection, "box wants sx sy sz"); return; }
                    job.a = sizes[0]; job.b = sizes[1]; job.c = sizes[2];
                }
                break;
            case ShapeForm::Wall:
                if (sizes.empty() || sizes.size() > 2) { Reply(connection, "wall wants w [h]"); return; }
                job.a = sizes[0];
                job.b = sizes.size() > 1 ? sizes[1] : sizes[0];
                break;
            case ShapeForm::Sphere:
            case ShapeForm::Dome:
                if (sizes.size() != 1) { Reply(connection, "wants one radius"); return; }
                job.a = sizes[0];
                break;
            case ShapeForm::Cylinder:
                if (sizes.empty() || sizes.size() > 2) { Reply(connection, "cylinder wants r [h]"); return; }
                job.a = sizes[0];
                job.b = sizes.size() > 1 ? sizes[1] : sizes[0];
                break;
            case ShapeForm::Pyramid:
                if (sizes.size() != 1) { Reply(connection, "pyramid wants a base size"); return; }
                job.a = sizes[0];
                break;
        }

        // Quirks and the `at` anchor.
        bool haveAt = false;
        glm::dvec3 at(0.0);
        for (; i < args.size(); ++i) {
            const std::string q = Lower(args[i]);
            if (q == "hollow")      job.hollow = true;
            else if (q == "frame")  job.frame = true;
            else if (q == "checker") job.checker = true;
            else if (q.rfind("spaced=", 0) == 0) {
                int n;
                if (!ParseInt(q.substr(7), n) || n < 2 || n > 64) {
                    Reply(connection, "spaced=N wants 2..64");
                    return;
                }
                job.spaced = n;
            } else if (q == "at") {
                if (i + 3 >= args.size()) { Reply(connection, "at wants x y z"); return; }
                const glm::dvec3 origin = sender.getPosition();
                if (!ParseCoord(args[i + 1], origin.x, true, at.x) ||
                    !ParseCoord(args[i + 2], origin.y, false, at.y) ||
                    !ParseCoord(args[i + 3], origin.z, true, at.z)) {
                    Reply(connection, "Bad coordinates after 'at'");
                    return;
                }
                haveAt = true;
                i += 3;
            } else {
                Reply(connection, std::string("Unknown option '") + args[i] + "'");
                return;
            }
        }

        // Facing, snapped to the nearest cardinal so walls stand square to
        // you. MC yaw convention: 0 faces +Z (south).
        const float yaw = sender.getYaw();
        const double rad = static_cast<double>(yaw) * 3.14159265358979323846 / 180.0;
        const glm::dvec3 look(-std::sin(rad), 0.0, std::cos(rad));
        glm::ivec3 fwd = std::abs(look.x) >= std::abs(look.z)
            ? glm::ivec3(look.x >= 0.0 ? 1 : -1, 0, 0)
            : glm::ivec3(0, 0, look.z >= 0.0 ? 1 : -1);
        job.facing = fwd;

        // Anchor: the BASE CENTRE. Default is in front of the player — near
        // face ~4 blocks away — with the base at the player's feet, so a
        // hundred-cubed cube neither buries you nor floats.
        if (haveAt) {
            job.base = glm::ivec3(static_cast<int>(std::floor(at.x)),
                                  static_cast<int>(std::floor(at.y)),
                                  static_cast<int>(std::floor(at.z)));
        } else {
            const glm::dvec3 pos = sender.getPosition();
            const int forward = job.ForwardExtent() / 2 + 4;
            job.base = glm::ivec3(
                static_cast<int>(std::floor(pos.x)) + fwd.x * forward,
                static_cast<int>(std::floor(pos.y)),
                static_cast<int>(std::floor(pos.z)) + fwd.z * forward);
        }

        const int64_t volume = job.BoundingVolume();
        constexpr int64_t kMaxVolume = 140LL * 1000 * 1000;   // ~512^2 walls, 512-cubes refused
        if (volume > kMaxVolume) {
            Reply(connection, "That shape spans too many cells");
            return;
        }

        if (!server->SubmitShapeJob(job)) {
            Reply(connection, "Shape queue is full — let some builds finish");
            return;
        }
        Log::Info("[Shape] %s building %s of '%s' (%d %d %d) at (%d,%d,%d)",
                  sender.getName().c_str(), form.c_str(), args[0].c_str(),
                  job.a, job.b, job.c, job.base.x, job.base.y, job.base.z);
        Reply(connection, "Building " + form + " of " + args[0] + "...");
    }

} // namespace Server
