// File: src/server/commands/RideCommand.cpp
#include "RideCommand.hpp"
#include "EntityCommandUtil.hpp"
#include "../IntegratedServer.hpp"
#include "../entity/PlayerRiding.hpp"
#include "../entity/ServerLevelBridge.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"

#include "common/entity/Mob.hpp"

namespace Server {

    namespace {

        // The live Entity behind a selection (a mob, the player's view).
        Game::Entity* EntityOf(const SelectedEntity& e) {
            if (e.kind == SelectedEntity::Kind::Mob) return e.mob;
            if (e.kind == SelectedEntity::Kind::Player && e.player) return e.player->effectEntity();
            return nullptr;
        }

        // Entity.getDisplayName of an entity reached through a link (a
        // vehicle): a player view's player, else the mob's name.
        std::string NameOf(Game::Entity* entity, const CommandSourceStack& source, Game::DimensionId dimension) {
            if (auto* view = dynamic_cast<PlayerEntityView*>(entity)) {
                return view->GetPlayer() ? view->GetPlayer()->getName() : std::string("Player");
            }
            SelectedEntity described;
            if (DescribeEntity(entity, source.WithLevel(dimension), described)) return described.name;
            if (auto* mob = dynamic_cast<Game::Mob*>(entity)) {
                if (mob->HasCustomName()) return *mob->GetCustomName();
                return EntityCmd::Tr(("entity.minecraft." + std::string(mob->TypeInfo().slug)).c_str());
            }
            return "?";
        }

        // Entity.getSelfAndPassengers().anyMatch(e -> e == vehicle).
        bool IsSelfOrPassenger(const Game::Entity& root, const Game::Entity* candidate) {
            if (&root == candidate) return true;
            for (const Game::Entity* passenger : root.GetPassengers()) {
                if (passenger && IsSelfOrPassenger(*passenger, candidate)) return true;
            }
            return false;
        }

    } // namespace

    void RideCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        dispatcher.RegisterCommand("ride", RideCommand::Execute,
            Cmd::Root()
                .Then(Cmd::Argument("target", Cmd::Arg::Entity)
                    .Then(Cmd::Literal("mount")
                        .Then(Cmd::Argument("vehicle", Cmd::Arg::Entity).Executes()))
                    .Then(Cmd::Literal("dismount").Executes())));
    }

    void RideCommand::Execute(const CommandSourceStack& source,
                              const std::vector<std::string>& args,
                              ServerConnection& connection,
                              PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        using namespace EntityCmd;
        const bool mount = args.size() == 3 && args[1] == "mount";
        const bool dismount = args.size() == 2 && args[1] == "dismount";
        if (!mount && !dismount) {
            Failure(connection, "Unknown or incomplete command, see below for error");
            for (const std::string& line : g_integratedServer->GetCommandDispatcher().GetUsageLines("ride")) {
                Failure(connection, line);
            }
            return;
        }
        std::vector<SelectedEntity> targets;
        std::string error;
        if (!ResolveSelector(args[0], SelectorKind::Entity, source, targets, error)) {
            Failure(connection, error);
            return;
        }
        const SelectedEntity& target = targets.front();
        Game::Entity* targetEntity = EntityOf(target);
        const std::string targetName = DisplayName(target);

        if (dismount) {
            Game::Entity* vehicle = targetEntity ? targetEntity->GetVehicle() : nullptr;
            if (!vehicle) {
                Failure(connection, Tr("commands.ride.not_riding", {targetName}));
                return;
            }
            const std::string vehicleName = NameOf(vehicle, source, target.dimension);
            // Entity.stopRiding: a player through PlayerRiding (put down at
            // the dismount location, the clients told), a mob directly.
            if (target.kind == SelectedEntity::Kind::Player && target.session) {
                PlayerRiding::StopRiding(*target.session);
            } else {
                targetEntity->StopRiding();
            }
            Success(source, connection, /*broadcast=*/true, Tr("commands.ride.dismount.success", {targetName, vehicleName}));
            return;
        }

        std::vector<SelectedEntity> vehicles;
        if (!ResolveSelector(args[2], SelectorKind::Entity, source, vehicles, error)) {
            Failure(connection, error);
            return;
        }
        const SelectedEntity& vehicleSel = vehicles.front();
        Game::Entity* vehicle = EntityOf(vehicleSel);
        const std::string vehicleName = DisplayName(vehicleSel);

        if (targetEntity && targetEntity->GetVehicle()) {
            Failure(connection, Tr("commands.ride.already_riding",
                                   {targetName, NameOf(targetEntity->GetVehicle(), source, target.dimension)}));
            return;
        }
        if (vehicleSel.kind == SelectedEntity::Kind::Player) {
            Failure(connection, Tr("commands.ride.mount.failure.cant_ride_players"));
            return;
        }
        if (targetEntity && vehicle && IsSelfOrPassenger(*targetEntity, vehicle)) {
            Failure(connection, Tr("commands.ride.mount.failure.loop"));
            return;
        }
        if (target.dimension != vehicleSel.dimension) {
            Failure(connection, Tr("commands.ride.mount.failure.wrong_dimension"));
            return;
        }
        // startRiding(vehicle, force = true, sendEventAndTriggers = true).
        // Dropped items and orbs are not Entities here and ride nothing.
        bool mounted = false;
        if (targetEntity && vehicle) {
            if (target.kind == SelectedEntity::Kind::Player && target.session) {
                mounted = PlayerRiding::StartRiding(*target.session, *vehicle, /*force=*/true);
            } else {
                mounted = targetEntity->StartRiding(*vehicle, /*force=*/true);
            }
        }
        if (!mounted) {
            Failure(connection, Tr("commands.ride.mount.failure.generic", {targetName, vehicleName}));
            return;
        }
        Success(source, connection, /*broadcast=*/true, Tr("commands.ride.mount.success", {targetName, vehicleName}));
    }

} // namespace Server
