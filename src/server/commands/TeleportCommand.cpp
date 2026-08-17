// File: src/server/commands/TeleportCommand.cpp
#include "TeleportCommand.hpp"
#include "EntitySelector.hpp"
#include "../IntegratedServer.hpp"
#include "../entity/ItemEntityManager.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace Server {

    void TeleportCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("tp", TeleportCommand::Execute);
        dispatcher.RegisterCommand("teleport", TeleportCommand::Execute);
    }

    namespace {

        // MC Level.isInSpawnableBounds — what TeleportCommand's INVALID_POSITION
        // guards. Without it a typo'd coordinate sends a player somewhere the
        // chunk system cannot represent and the session never recovers.
        constexpr double kMaxHorizontal = 30000000.0;
        constexpr double kMaxVertical   = 20000000.0;

        bool InSpawnableBounds(const glm::dvec3& p) {
            return std::abs(p.x) < kMaxHorizontal && std::abs(p.z) < kMaxHorizontal &&
                   std::abs(p.y) < kMaxVertical;
        }

        // MC WorldCoordinate.parseDouble — "~", "~5", "~-3", "100", "100.5".
        // `centerCorrect` is MC's flag for the block-position form: an integer
        // coordinate in a Vec3Argument means the CENTRE of that block, which is
        // why `/tp 100 64 100` lands you at 100.5 / 100.5 and not on a corner.
        bool ParseCoord(const std::string& arg, double origin, bool centerCorrect,
                        double& out) {
            if (arg.empty()) return false;

            if (arg[0] == '~') {
                if (arg.size() == 1) { out = origin; return true; }
                try {
                    size_t used = 0;
                    const double d = std::stod(arg.substr(1), &used);
                    if (used != arg.size() - 1) return false;
                    out = origin + d;
                    return true;
                } catch (...) { return false; }
            }

            try {
                size_t used = 0;
                const double v = std::stod(arg, &used);
                if (used != arg.size()) return false;
                // Only a coordinate written WITHOUT a decimal point is centred.
                out = (centerCorrect && arg.find('.') == std::string::npos) ? v + 0.5 : v;
                return true;
            } catch (...) { return false; }
        }

        bool LooksLikeCoord(const std::string& s) {
            if (s.empty()) return false;
            if (s[0] == '~' || s[0] == '^') return true;
            if (s[0] == '@') return false;
            size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
            if (i >= s.size()) return false;
            return std::isdigit(static_cast<unsigned char>(s[i])) != 0 || s[i] == '.';
        }

        struct Rotation { float yRot = 0.0f; float xRot = 0.0f; };

        // One `^n` component. Bare `^` is zero, like bare `~`.
        bool ParseLocalComponent(const std::string& arg, double& out) {
            if (arg.empty() || arg[0] != '^') return false;
            if (arg.size() == 1) { out = 0.0; return true; }
            try {
                size_t used = 0;
                out = std::stod(arg.substr(1), &used);
                return used == arg.size() - 1;
            } catch (...) { return false; }
        }

        // MC Vec3Argument: a coordinate triple is EITHER three world
        // coordinates (absolute or `~`) or three LOCAL `^` ones. Mixing them is
        // ERROR_MIXED_TYPE in vanilla, and rightly so — the local frame is a
        // basis, and two thirds of a basis means nothing.
        //
        // `^left ^up ^forward` is measured in the SOURCE's own frame, so
        // `^ ^ ^5` is five blocks along the way they are looking, pitch
        // included. That is the whole point of it over `~`.
        bool ParseVec3(const std::string& ax, const std::string& ay, const std::string& az,
                       const CommandSource& src, const Rotation& srcRot,
                       glm::dvec3& out, std::string& error) {
            const bool lx = !ax.empty() && ax[0] == '^';
            const bool ly = !ay.empty() && ay[0] == '^';
            const bool lz = !az.empty() && az[0] == '^';

            if (lx || ly || lz) {
                if (!(lx && ly && lz)) {
                    error = "Cannot mix world & local coordinates "
                            "(everything must either use ^ or not)";
                    return false;
                }
                double left = 0.0, up = 0.0, forwards = 0.0;
                if (!ParseLocalComponent(ax, left) ||
                    !ParseLocalComponent(ay, up) ||
                    !ParseLocalComponent(az, forwards)) {
                    error = "Invalid local coordinate: " + ax + " " + ay + " " + az;
                    return false;
                }

                // MC Vec3.applyLocalCoordinatesToRotation. `up` is the view
                // vector pitched a further quarter turn up, so the frame tilts
                // with the head instead of staying world-aligned — which is why
                // `^ ^1 ^` while looking down moves you FORWARD-and-down's
                // perpendicular, not straight up.
                const glm::dvec3 forward =
                    glm::dvec3(Game::Mth::ViewVector(srcRot.xRot, srcRot.yRot));
                const glm::dvec3 upVec =
                    glm::dvec3(Game::Mth::ViewVector(srcRot.xRot - 90.0f, srcRot.yRot));
                const glm::dvec3 leftVec = -glm::cross(forward, upVec);

                // MC's anchor here is the source's, which defaults to FEET.
                out = src.position + forward * forwards + upVec * up + leftVec * left;
                return true;
            }

            if (!ParseCoord(ax, src.position.x, true,  out.x) ||
                !ParseCoord(ay, src.position.y, false, out.y) ||
                !ParseCoord(az, src.position.z, true,  out.z)) {
                error = "Invalid position: " + ax + " " + ay + " " + az;
                return false;
            }
            return true;
        }

        // MC Entity.lookAt(anchor, target).
        Rotation LookAtRotation(const glm::dvec3& from, const glm::dvec3& target) {
            const glm::vec3 d(target - from);
            return Rotation{ Game::Mth::YRotFromVector(d), Game::Mth::XRotFromVector(d) };
        }

        // MC EntityAnchorArgument.Anchor.
        glm::dvec3 AnchorPos(const SelectedEntity& e, bool eyes) {
            if (!eyes) return e.position;
            double eyeHeight = 0.0;
            switch (e.kind) {
                case SelectedEntity::Kind::Player: eyeHeight = 1.62; break;
                case SelectedEntity::Kind::Mob:
                    eyeHeight = e.mob ? e.mob->GetEyeHeight() : 0.0;
                    break;
                case SelectedEntity::Kind::Item: eyeHeight = 0.0; break;
            }
            return e.position + glm::dvec3(0.0, eyeHeight, 0.0);
        }

        // MC TeleportCommand.performTeleport, minus the dimension change.
        //
        // Returns false only for a position outside the spawnable bounds, which
        // MC turns into commands.teleport.invalidPosition.
        bool PerformTeleport(const SelectedEntity& victim, const glm::dvec3& pos,
                             const Rotation& rot) {
            if (!InSpawnableBounds(pos)) return false;

            const float yRot = Game::Mth::WrapDegrees(rot.yRot);
            const float xRot = Game::Mth::WrapDegrees(rot.xRot);

            switch (victim.kind) {
                case SelectedEntity::Kind::Player: {
                    if (!victim.session) return true;
                    ServerConnection* conn = victim.session->GetConnection();
                    if (!conn) return true;
                    conn->Teleport(pos.x, pos.y, pos.z, yRot, xRot);
                    return true;
                }

                case SelectedEntity::Kind::Mob: {
                    Game::Mob* mob = victim.mob;
                    if (!mob) return true;

                    mob->position  = pos;
                    mob->yRot      = yRot;
                    mob->xRot      = xRot;
                    mob->yHeadRot  = yRot;
                    mob->yBodyRot  = yRot;

                    // MC zeroes vertical motion and asserts onGround for
                    // anything that is not elytra-flying, so a mob dropped
                    // mid-jump does not keep the arc it was on.
                    mob->velocity.y = 0.0;
                    mob->onGround   = true;

                    // MC: `if (victim instanceof PathfinderMob) mob.getNavigation().stop();`
                    // A live path still points at the old position, so without
                    // this the mob immediately walks back the way it came.
                    mob->GetNavigation().Stop();
                    return true;
                }

                case SelectedEntity::Kind::Item: {
                    if (!g_integratedServer) return true;
                    ItemEntityManager* items = g_integratedServer->GetItemEntities();
                    if (!items) return true;
                    Game::ItemEntity* item = items->Find(victim.id);
                    if (!item) return true;

                    item->pos       = pos;
                    item->vel.y     = 0.0;
                    item->onGround  = true;
                    // There is no per-client tracked set for items; the flag is
                    // what puts this entity in the next tick's sync batch.
                    item->needsSync = true;
                    return true;
                }
            }
            return true;
        }

        std::string FormatPos(const glm::dvec3& p) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%.2f, %.2f, %.2f", p.x, p.y, p.z);
            return buf;
        }

        void Usage(ServerConnection& connection) {
            connection.SendChatMessage(
                "Usage: /tp <destination> | /tp <x> <y> <z> | "
                "/tp <targets> <destination> | /tp <targets> <x> <y> <z> "
                "[<yaw> <pitch> | facing <x> <y> <z> | facing entity <entity> [feet|eyes]]", 1);
        }

    } // namespace

    void TeleportCommand::Execute(ServerPlayer& sender,
                                  const std::vector<std::string>& args,
                                  ServerConnection& connection,
                                  PlayerSessionManager& sessionManager) {
        CommandSource source;
        source.sender   = &sender;
        source.sessions = &sessionManager;
        source.position = sender.getPosition();

        // The source's own rotation, in MC's convention, for `~` in a rotation
        // argument and as the fallback when no rotation is given.
        const Rotation sourceRot{ sender.getYaw(), sender.getPitch() };

        std::string error;

        // ── Which tokens are the targets, and which are the destination? ────
        //
        // MC's Brigadier tries the longer branch (`<targets> <location>`) and
        // falls back to the shorter (`<location>` alone). The token shape is
        // enough to tell them apart here: a leading coordinate can only be the
        // self form, because a selector or a player name never starts with a
        // digit or a tilde.
        size_t at = 0;
        std::vector<SelectedEntity> targets;

        // A single non-coordinate token is MC's `teleport <destination>` branch
        // — the sender moves to that entity. It has to be decided here rather
        // than by the arg count below, because `/tp Steve` would otherwise
        // parse Steve as the TARGET and then find no destination.
        const bool selfForm =
            !args.empty() && (LooksLikeCoord(args[0]) || args.size() == 1);
        if (selfForm) {
            // MC uses source.getEntityOrException() — the sender itself, with
            // no selector involved.
            if (!ResolveSelector("@s", SelectorKind::Entities, source, targets, error)) {
                connection.SendChatMessage(error, 1);
                return;
            }
        } else {
            if (args.empty()) { Usage(connection); return; }
            if (!ResolveSelector(args[0], SelectorKind::Entities, source, targets, error)) {
                connection.SendChatMessage(error, 1);
                return;
            }
            at = 1;
        }

        const size_t remaining = args.size() - at;

        // ── Form 1: teleport to an entity (MC teleportToEntity) ─────────────
        if (remaining == 1) {
            std::vector<SelectedEntity> destination;
            if (!ResolveSelector(args[at], SelectorKind::Entity, source, destination, error)) {
                connection.SendChatMessage(error, 1);
                return;
            }
            const SelectedEntity& dest = destination.front();

            // MC takes the DESTINATION's rotation here, not the victim's — a
            // teleport onto someone leaves you facing the way they face.
            const Rotation rot{ dest.yRot, dest.xRot };

            int moved = 0;
            for (const SelectedEntity& victim : targets) {
                if (PerformTeleport(victim, dest.position, rot)) ++moved;
            }
            if (moved == 0) {
                connection.SendChatMessage("Invalid position for teleport", 1);
                return;
            }

            connection.SendChatMessage(
                moved == 1
                    ? "Teleported " + targets.front().name + " to " + dest.name
                    : "Teleported " + std::to_string(moved) + " entities to " + dest.name, 1);
            Log::Info("[TeleportCommand] %s teleported %d entity(s) to %s",
                      sender.getName().c_str(), moved, dest.name.c_str());
            return;
        }

        // Everything below needs at least a location.
        if (remaining < 3) { Usage(connection); return; }

        glm::dvec3 pos{};
        if (!ParseVec3(args[at + 0], args[at + 1], args[at + 2],
                       source, sourceRot, pos, error)) {
            connection.SendChatMessage(error, 1);
            return;
        }

        const size_t tail = at + 3;
        const size_t tailCount = args.size() - tail;

        // Default: MC passes each victim its OWN rotation when none is given.
        // `hasRotation` distinguishes that from an explicit one.
        bool     hasRotation = false;
        Rotation rotation{};
        // `facing` is resolved per victim, because the angle depends on where
        // that victim ends up looking FROM.
        bool       facing = false;
        glm::dvec3 facingTarget{0.0};
        bool       facingEyes = false;

        if (tailCount == 0) {
            // no rotation
        } else if (tailCount == 2) {
            // <yaw> <pitch>, MC RotationArgument — `~` is the SOURCE's rotation.
            double y = 0.0, x = 0.0;
            if (args[tail + 0][0] == '^' || args[tail + 1][0] == '^') {
                // MC RotationArgument parses world coordinates only.
                connection.SendChatMessage("Local coordinates are not allowed here", 1);
                return;
            }
            if (!ParseCoord(args[tail + 0], sourceRot.yRot, false, y) ||
                !ParseCoord(args[tail + 1], sourceRot.xRot, false, x)) {
                connection.SendChatMessage("Invalid rotation: " + args[tail + 0] + " " +
                                           args[tail + 1], 1);
                return;
            }
            hasRotation = true;
            rotation.yRot = static_cast<float>(y);
            rotation.xRot = static_cast<float>(x);
        } else if (args[tail] == "facing") {
            const size_t f = tail + 1;
            const size_t fCount = args.size() - f;

            if (fCount >= 1 && args[f] == "entity") {
                if (fCount < 2) { Usage(connection); return; }
                std::vector<SelectedEntity> facingEntity;
                if (!ResolveSelector(args[f + 1], SelectorKind::Entity, source,
                                     facingEntity, error)) {
                    connection.SendChatMessage(error, 1);
                    return;
                }
                // MC's default anchor for `facing entity` is FEET.
                bool anchorEyes = false;
                if (fCount >= 3) {
                    if (args[f + 2] == "eyes")      anchorEyes = true;
                    else if (args[f + 2] == "feet") anchorEyes = false;
                    else {
                        connection.SendChatMessage("Invalid entity anchor: " + args[f + 2], 1);
                        return;
                    }
                }
                if (fCount > 3) { Usage(connection); return; }

                facing       = true;
                facingEyes   = anchorEyes;
                facingTarget = AnchorPos(facingEntity.front(), anchorEyes);
            } else if (fCount == 3) {
                glm::dvec3 look{};
                if (!ParseVec3(args[f + 0], args[f + 1], args[f + 2],
                               source, sourceRot, look, error)) {
                    connection.SendChatMessage(error, 1);
                    return;
                }
                facing       = true;
                facingEyes   = false;
                facingTarget = look;
            } else {
                Usage(connection);
                return;
            }
        } else {
            Usage(connection);
            return;
        }

        // ── Move them (MC teleportToPos) ────────────────────────────────────
        int  moved = 0;
        bool anyOutOfBounds = false;
        for (const SelectedEntity& victim : targets) {
            Rotation rot = hasRotation ? rotation : Rotation{ victim.yRot, victim.xRot };
            if (facing) {
                // MC LookAt runs AFTER the move, from the victim's new anchor —
                // which is why facing a point right next to the destination
                // gives a sharp angle rather than the one you had on arrival.
                SelectedEntity moved_ = victim;
                moved_.position = pos;
                rot = LookAtRotation(AnchorPos(moved_, facingEyes), facingTarget);
            }
            if (PerformTeleport(victim, pos, rot)) ++moved;
            else anyOutOfBounds = true;
        }

        if (moved == 0) {
            connection.SendChatMessage(
                anyOutOfBounds ? "Invalid position for teleport" : "No entity was teleported", 1);
            return;
        }

        connection.SendChatMessage(
            moved == 1
                ? "Teleported " + targets.front().name + " to " + FormatPos(pos)
                : "Teleported " + std::to_string(moved) + " entities to " + FormatPos(pos), 1);

        Log::Info("[TeleportCommand] %s teleported %d entity(s) to (%.1f, %.1f, %.1f)",
                  sender.getName().c_str(), moved, pos.x, pos.y, pos.z);
    }

} // namespace Server
