// File: src/server/commands/RotateCommand.cpp
#include "RotateCommand.hpp"
#include "CommandCoords.hpp"
#include "EntityCommandUtil.hpp"
#include "../IntegratedServer.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"

#include "common/core/Mth.hpp"
#include "common/entity/Mob.hpp"

#include <algorithm>
#include <cmath>

namespace Server {

    namespace {

        // Entity.setYRot / setXRot: yRot % 360, xRot % 360 clamped to ±90.
        void Normalize(float& yRot, float& xRot) {
            yRot = std::fmod(yRot, 360.0f);
            xRot = std::clamp(std::fmod(xRot, 360.0f), -90.0f, 90.0f);
        }

        // Entity.forceSetRotation: a player's client is told
        // (ClientboundPlayerRotationPacket — here the position packet at the
        // player's own feet), anything else is turned in place, its head and
        // body with it so the next tracker pass carries the new facing.
        bool Apply(const SelectedEntity& target, float yRot, float xRot) {
            Normalize(yRot, xRot);
            switch (target.kind) {
                case SelectedEntity::Kind::Player: {
                    if (!target.player || !target.session) return false;
                    ServerConnection* conn = target.session->GetConnection();
                    if (!conn) return false;
                    const glm::dvec3 pos = target.player->getPosition();
                    target.player->setRotation(yRot, xRot);
                    conn->Teleport(pos.x, pos.y, pos.z, yRot, xRot);
                    return true;
                }
                case SelectedEntity::Kind::Mob: {
                    Game::Mob* mob = target.mob;
                    if (!mob) return false;
                    mob->yRot = mob->yRotO = yRot;
                    mob->xRot = mob->xRotO = xRot;
                    mob->yHeadRot = mob->yHeadRotO = yRot;
                    mob->yBodyRot = mob->yBodyRotO = yRot;
                    return true;
                }
                default:
                    // Dropped items and orbs keep no facing here.
                    return true;
            }
        }

        // Entity.lookAt(anchor, target): the facing from the rotated entity's
        // feet or eyes (the source's anchor applied to it) to the point.
        bool LookAt(const SelectedEntity& target, bool fromEyes, const glm::dvec3& point) {
            const glm::dvec3 from = EntityAnchorPosition(target, fromEyes);
            const glm::vec3 d(point - from);
            return Apply(target, Game::Mth::YRotFromVector(d), Game::Mth::XRotFromVector(d));
        }

    } // namespace

    void RotateCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        dispatcher.RegisterCommand("rotate", RotateCommand::Execute,
            Cmd::Root()
                .Then(Cmd::Argument("target", Cmd::Arg::Entity)
                    .Then(Cmd::Argument("rotation", Cmd::Arg::Rotation).Executes())
                    .Then(Cmd::Literal("facing")
                        .Then(Cmd::Literal("entity")
                            .Then(Cmd::Argument("facingEntity", Cmd::Arg::Entity).Executes()
                                .Then(Cmd::Literals({"eyes", "feet"}))))
                        .Then(Cmd::Argument("facingLocation", Cmd::Arg::Vec3).Executes()))));
    }

    void RotateCommand::Execute(const CommandSourceStack& source,
                                const std::vector<std::string>& args,
                                ServerConnection& connection,
                                PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        using namespace EntityCmd;
        const auto usage = [&] {
            Failure(connection, "Unknown or incomplete command, see below for error");
            for (const std::string& line : g_integratedServer->GetCommandDispatcher().GetUsageLines("rotate")) {
                Failure(connection, line);
            }
        };
        if (args.size() < 3) { usage(); return; }
        std::vector<SelectedEntity> targets;
        std::string error;
        if (!ResolveSelector(args[0], SelectorKind::Entity, source, targets, error)) {
            Failure(connection, error);
            return;
        }
        const SelectedEntity& target = targets.front();
        bool done = false;

        if (args[1] == "facing") {
            if (args.size() >= 4 && args[2] == "entity" && args.size() <= 5) {
                std::vector<SelectedEntity> facing;
                if (!ResolveSelector(args[3], SelectorKind::Entity, source, facing, error)) {
                    Failure(connection, error);
                    return;
                }
                bool eyes = false;   // EntityAnchorArgument.Anchor.FEET by default
                if (args.size() == 5) {
                    if (args[4] == "eyes") eyes = true;
                    else if (args[4] != "feet") { Failure(connection, "Invalid entity anchor position " + args[4]); return; }
                }
                done = LookAt(target, source.anchorEyes, EntityAnchorPosition(facing.front(), eyes));
            } else if (args.size() == 5) {
                glm::dvec3 point{};
                if (!ParseVec3(args[2], args[3], args[4], source, source.rotation, point, error)) {
                    Failure(connection, error);
                    return;
                }
                done = LookAt(target, source.anchorEyes, point);
            } else {
                usage();
                return;
            }
        } else if (args.size() == 3) {
            // RotationArgument: `~` is relative to the SOURCE's rotation
            // (WorldCoordinates.getRotation), and forceSetRotation's relative
            // flags then land the entity on exactly that value.
            float yRot = 0.0f, xRot = 0.0f;
            bool yRel = false, xRel = false;
            if (!ParseRotation(args[1], args[2], source, yRot, xRot, yRel, xRel, error)) {
                Failure(connection, error);
                return;
            }
            done = Apply(target, yRot, xRot);
        } else {
            usage();
            return;
        }
        if (!done) {
            Failure(connection, "Could not rotate " + DisplayName(target));
            return;
        }
        Success(source, connection, /*broadcast=*/true, Tr("commands.rotate.success", {DisplayName(target)}));
    }

} // namespace Server
