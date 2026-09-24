// File: src/server/commands/TeleportCommand.cpp
#include "TeleportCommand.hpp"
#include "EntitySelector.hpp"
#include "CommandCoords.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "../entity/ItemEntityManager.hpp"
#include "../entity/MobManager.hpp"
#include "../entity/ServerEntityTracker.hpp"
#include "../entity/ServerLevelBridge.hpp"   // PlayerEntityView
#include "../level/PortalTravel.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/ItemEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
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

        // ParseCoord / LooksLikeCoord / ParseVec3 moved to CommandCoords.{hpp,cpp}
        // when /summon needed coordinates too — see that header for why a
        // coordinate triple is an argument type rather than one command's
        // private helper.
        using Rotation = CommandRotation;

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

        // What became of one victim — the feedback line distinguishes MC's
        // commands.teleport.invalidPosition from a crossing that could not
        // be carried out.
        enum class TeleportResult : uint8_t { Moved, OutOfBounds, Failed };

        // A non-player arriving in a level it was not in: make sure the
        // column it lands in exists before it is inserted there. A mob
        // placed in a chunk that was never loaded is simulated against air
        // and saved nowhere. The destination of `/tp <mob> <player>` is
        // already loaded (a player stands there); coordinates in a level
        // nobody has visited are not. Same hold-and-generate as a portal
        // exit (PortalTravel::EnsureExitAreaLoaded — a one-off stall).
        void EnsureLandingLoaded(ServerLevel& level, const glm::dvec3& pos) {
            Game::World* world = level.World();
            if (!world) return;
            const glm::ivec3 block{ static_cast<int>(std::floor(pos.x)),
                                    static_cast<int>(std::floor(pos.y)),
                                    static_cast<int>(std::floor(pos.z)) };
            if (world->IsPositionLoaded(block.x, block.y, block.z)) return;
            PortalTravel::EnsureExitAreaLoaded(level, block);
        }

        // Send tracker output scoped to the level it describes.
        void SendTrackerPackets(const CommandSource& source, Game::DimensionId dimension,
                                const std::vector<EntityPacketOut>& packets) {
            if (!source.sessions) return;
            for (const EntityPacketOut& packet : packets) {
                auto session = source.sessions->GetSession(packet.connectionId);
                if (!session || !session->GetConnection()) continue;
                session->GetConnection()->SendPacketIn(dimension,
                                                       static_cast<uint8_t>(packet.packetId),
                                                       packet.payload);
            }
        }

        // MC Entity.teleportTo(level, …) when `level` is not the victim's:
        // Entity.teleport(TeleportTransition) → teleportCrossDimension (or
        // ServerPlayer.teleport's respawn path for a player). The transition
        // is absolute — position, rotation, deltaMovement ZERO — so the
        // victim arrives still, facing (yRot, xRot).
        TeleportResult TeleportAcross(const CommandSource& source, const SelectedEntity& victim,
                                      Game::DimensionId toDim, const glm::dvec3& pos,
                                      float yRot, float xRot) {
            if (!g_integratedServer) return TeleportResult::Failed;
            ServerLevel* from = g_integratedServer->GetLevel(victim.dimension);
            ServerLevel* to   = g_integratedServer->GetOrCreateLevel(toDim);
            if (!from || !to) {
                Log::Warning("[TeleportCommand] %s: '%s' -> '%s' has no level to cross between",
                             victim.name.c_str(),
                             std::string(Game::DimensionName(victim.dimension)).c_str(),
                             std::string(Game::DimensionName(toDim)).c_str());
                return TeleportResult::Failed;
            }

            switch (victim.kind) {
                case SelectedEntity::Kind::Player: {
                    ServerPlayer* player = victim.session ? victim.session->GetPlayer() : victim.player;
                    if (!player) return TeleportResult::Failed;
                    PlayerEntityView* view = g_integratedServer->GetPlayerEntityView(player->getPlayerId());
                    if (!view) return TeleportResult::Failed;

                    // The dimension change every portal and /dim make: the
                    // old level's tickets dropped, a ticket at the landing,
                    // ChangeDimensionS2C (the client swaps its level and
                    // shows the loading screen), then the position packet.
                    // ArriveAt lands keeping the player's OWN facing (MC
                    // Relative rotation), so the command's absolute
                    // rotation is written onto the player first — MC
                    // ServerPlayer.teleportTo(level, x, y, z, relatives=∅,
                    // yRot, xRot).
                    player->setRotation(yRot, xRot);
                    PortalTravel::ArriveAt(*g_integratedServer, *from, *to, *view, pos);
                    return player->getDimensionId() == Game::DimensionToRaw(toDim)
                        ? TeleportResult::Moved : TeleportResult::Failed;
                }

                case SelectedEntity::Kind::Mob: {
                    Game::Mob* mob = victim.mob;
                    if (!mob || mob->IsRemoved()) return TeleportResult::Failed;
                    MobManager* fromMobs = from->Mobs();
                    if (!fromMobs || !to->Mobs() || !to->MobLevel() || !from->MobLevel())
                        return TeleportResult::Failed;
                    const int32_t id = mob->GetId();
                    if (fromMobs->Find(id) != mob) return TeleportResult::Failed;

                    EnsureLandingLoaded(*to, pos);

                    // MC removeAfterChangingDimensions: the old level's
                    // watchers are told it is gone before the new level's
                    // tracker announces it on its next pass.
                    std::vector<EntityPacketOut> outgoing;
                    if (from->MobTracker()) from->MobTracker()->RemoveEntity(id, outgoing);
                    SendTrackerPackets(source, from->Dimension(), outgoing);

                    // Extract breaks riding links (the vehicle and riders
                    // stay behind) and clears every reference TO the mob.
                    std::unique_ptr<Game::Mob> owned = fromMobs->Extract(id);
                    if (!owned) return TeleportResult::Failed;
                    // …and the mob's own references to that level's
                    // entities, which would dangle once it ticks elsewhere
                    // (the same sweep EntityPortalTravel::MoveMob makes).
                    for (PlayerEntityView* view : from->MobLevel()->PlayerViews()) mob->ClearReferenceTo(view);
                    for (const auto& [otherId, other] : fromMobs->All()) mob->ClearReferenceTo(other.get());

                    // MC teleportSetPosition(PositionMoveRotation.of(transition)):
                    // placed before AddExisting, which files the mob under
                    // the chunk it stands in.
                    mob->position    = pos;
                    mob->oldPosition = pos;
                    mob->velocity    = glm::dvec3(0.0);
                    mob->yRot = mob->yRotO = yRot;
                    mob->xRot = mob->xRotO = xRot;
                    mob->yHeadRot = mob->yHeadRotO = yRot;
                    mob->yBodyRot = mob->yBodyRotO = yRot;
                    mob->onGround = true;

                    mob->SetLevel(to->MobLevel());
                    if (!to->Mobs()->AddExisting(std::move(owned))) {
                        // Process-wide ids make this unreachable; if it
                        // happens the mob is gone rather than duplicated.
                        Log::Warning("[TeleportCommand] Mob #%d lost crossing to %s: id taken",
                                     id, std::string(Game::DimensionName(toDim)).c_str());
                        return TeleportResult::Failed;
                    }
                    // performTeleport: `if (victim instanceof PathfinderMob)
                    // mob.getNavigation().stop()` — after SetLevel, which
                    // re-points the navigation at the new level.
                    if (mob->HasAiControls()) mob->GetNavigation().Stop();
                    return TeleportResult::Moved;
                }

                case SelectedEntity::Kind::Item: {
                    ItemEntityManager* fromItems = from->Items();
                    ItemEntityManager* toItems   = to->Items();
                    if (!fromItems || !toItems) return TeleportResult::Failed;
                    Game::ItemEntity* item = fromItems->Find(victim.id);
                    if (!item || item->stack.IsEmpty()) return TeleportResult::Failed;

                    EnsureLandingLoaded(*to, pos);

                    // MC teleportCrossDimension: a new entity in the new
                    // level restored from the old one (same uuid, stack,
                    // age, pickup delay), and the old one removed.
                    Game::ItemEntity arrival = *item;
                    arrival.pos          = pos;
                    arrival.vel          = glm::dvec3(0.0);
                    arrival.onGround     = true;
                    arrival.pendingSpawn = true;   // a fresh spawn to the new level's watchers
                    if (toItems->Adopt(std::move(arrival)) == 0) return TeleportResult::Failed;
                    // Emptied -> reaped on the old level's next tick, which
                    // is what broadcasts its removal there (as /kill does).
                    item->stack.Clear();
                    return TeleportResult::Moved;
                }
            }
            return TeleportResult::Failed;
        }

        // MC TeleportCommand.performTeleport. `level` is the destination
        // level (the destination entity's, or the source's for coordinates);
        // a victim standing in another one crosses (TeleportAcross).
        TeleportResult PerformTeleport(const CommandSource& source,
                                       const SelectedEntity& victim, Game::DimensionId level,
                                       const glm::dvec3& pos, const Rotation& rot) {
            if (!InSpawnableBounds(pos)) return TeleportResult::OutOfBounds;

            const float yRot = Game::Mth::WrapDegrees(rot.yRot);
            const float xRot = Game::Mth::WrapDegrees(rot.xRot);

            if (victim.dimension != level) {
                return TeleportAcross(source, victim, level, pos, yRot, xRot);
            }

            switch (victim.kind) {
                case SelectedEntity::Kind::Player: {
                    if (!victim.session) return TeleportResult::Moved;
                    ServerConnection* conn = victim.session->GetConnection();
                    if (!conn) return TeleportResult::Moved;
                    conn->Teleport(pos.x, pos.y, pos.z, yRot, xRot);
                    return TeleportResult::Moved;
                }

                case SelectedEntity::Kind::Mob: {
                    Game::Mob* mob = victim.mob;
                    if (!mob) return TeleportResult::Moved;

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
                    //
                    // HasAiControls is MC's `instanceof PathfinderMob` guard by
                    // another name: /tp accepts any entity the selector can
                    // name, and primed TNT and falling blocks have no navigator
                    // to stop (Mob::NoAiTag).
                    if (mob->HasAiControls()) mob->GetNavigation().Stop();
                    return TeleportResult::Moved;
                }

                case SelectedEntity::Kind::Item: {
                    if (!g_integratedServer) return TeleportResult::Moved;
                    // The level the item was enumerated from (the sender's —
                    // CollectItems' scoping), which is also the destination here.
                    ServerLevel* itemLevel = g_integratedServer->GetLevel(victim.dimension);
                    ItemEntityManager* items = itemLevel ? itemLevel->Items() : nullptr;
                    if (!items) return TeleportResult::Moved;
                    Game::ItemEntity* item = items->Find(victim.id);
                    if (!item) return TeleportResult::Moved;

                    item->pos       = pos;
                    item->vel.y     = 0.0;
                    item->onGround  = true;
                    // There is no per-client tracked set for items; the flag is
                    // what puts this entity in the next tick's sync batch.
                    item->needsSync = true;
                    return TeleportResult::Moved;
                }
            }
            return TeleportResult::Moved;
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

    void TeleportCommand::Execute(const CommandSourceStack& source,
                                  const std::vector<std::string>& args,
                                  ServerConnection& connection,
                                  PlayerSessionManager& sessionManager) {
        ServerPlayer& sender = *source.sender;
        // The stack carries the origin: the sender's feet, rotation and
        // level for a typed command, or whatever `/execute` rewrote them to.
        // The source's own rotation, in MC's convention, for `~` in a rotation
        // argument and as the fallback when no rotation is given.
        const Rotation sourceRot = source.rotation;

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

            // ...and the destination's LEVEL: MC teleportToEntity passes
            // `(ServerLevel)destination.level()`, so `/tp <player> @s` from
            // the Overworld fetches a player out of the Nether, and
            // `/tp @s <player>` follows them into theirs.
            int  moved = 0;
            bool anyOutOfBounds = false;
            for (const SelectedEntity& victim : targets) {
                const TeleportResult r =
                    PerformTeleport(source, victim, dest.dimension, dest.position, rot);
                if (r == TeleportResult::Moved) ++moved;
                else if (r == TeleportResult::OutOfBounds) anyOutOfBounds = true;
            }
            if (moved == 0) {
                connection.SendChatMessage(
                    anyOutOfBounds ? "Invalid position for teleport" : "No entity was teleported", 1);
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
            // MC teleportToPos passes `source.getLevel()`: coordinates are
            // in the SOURCE's level (`/execute in <dimension>` changes it), and a
            // victim standing in another level is brought across.
            const TeleportResult r = PerformTeleport(source, victim, source.dimension, pos, rot);
            if (r == TeleportResult::Moved) ++moved;
            else if (r == TeleportResult::OutOfBounds) anyOutOfBounds = true;
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
