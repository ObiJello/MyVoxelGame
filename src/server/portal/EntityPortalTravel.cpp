// File: src/server/portal/EntityPortalTravel.cpp
#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "EntityPortalTravel.hpp"

#include "ImmersivePortalRegistry.hpp"
#include "server/IntegratedServer.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ItemEntityManager.hpp"
#include "server/entity/ExperienceOrbManager.hpp"
#include "server/entity/ServerEntityTracker.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/session/PlayerSessionManager.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/portal/PortalRegistry.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/Animal.hpp"
#include "common/entity/Attributes.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/entity/MobCategory.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/level/World.hpp"
#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <algorithm>
#include <cmath>

namespace Server {

    namespace {
        using Game::Immersive::Portal;
        namespace PortalFlag = Game::Immersive::PortalFlag;

        // The mod's numbers, shared with the player's traveler.
        constexpr double kEdgeLeniency     = 0.05;
        constexpr double kMaxSegmentLength = 40.0;
        constexpr double kNudge            = 0.01;
        // A mob that just arrived must not read its own arrival motion as a
        // crossing back through the reverse surface. Short on purpose: the
        // 300-tick vanilla cooldown is for standing IN a portal block; an
        // immersive crossing is a displacement, and walking back is intended.
        constexpr int     kMobCooldownTicks   = 5;
        // A mob lands at least this far past the far surface (plus half its
        // width), so a goal that immediately turns it around cannot put it
        // straight back through the reverse surface.
        constexpr double  kArrivalMargin      = 0.3;
        constexpr int64_t kLooseCooldownTicks = 10;
        // A dropped item or orb crosses only while it is actually moving.
        // Friction never brings a resting item's velocity to exactly zero,
        // and one lying on the surface would otherwise creep across by a
        // hair, get mapped through, settle on the far floor, creep back —
        // a slow shove nobody threw. One block per second is well below
        // any throw and well above the residue.
        constexpr double  kMinLooseCrossSpeed = 0.05;   // blocks per tick
        // How far from the portal a hunting mob is still sent after its prey,
        // and how long the chase record lives.
        constexpr double  kChaseRadius       = 48.0;
        constexpr int64_t kChaseLifetimeTicks = 400;
        constexpr double  kChaseSpeed        = 1.2;

        glm::dvec3 EyeOf(const Game::Mob& mob, const glm::dvec3& feet) {
            return feet + glm::dvec3(0.0, static_cast<double>(mob.GetEyeHeight()), 0.0);
        }

        bool ChunkLoadedAt(ServerLevel& level, const glm::dvec3& pos) {
            Game::World* world = level.World();
            if (!world) return false;
            return world->IsPositionLoaded(static_cast<int>(std::floor(pos.x)),
                                           static_cast<int>(std::floor(pos.y)),
                                           static_cast<int>(std::floor(pos.z)));
        }
    }

    EntityPortalTravel::EntityPortalTravel(IntegratedServer& server) : m_server(server) {}

    std::vector<PickupSource> PickupSourcesFor(PlayerSessionManager& sessions, Game::DimensionId dimension) {
        // How close to a surface a player must stand for their image beyond
        // it to collect: the pickup box reaches a block past the body.
        constexpr double kPickupReach = 3.0;
        std::vector<PickupSource> out;
        ImmersivePortalRegistry* registry = g_integratedServer ? g_integratedServer->ImmersivePortals() : nullptr;
        for (auto& session : sessions.GetAllSessions()) {
            if (!session) continue;
            ServerPlayer* player = session->GetPlayer();
            if (!player) continue;
            const Game::DimensionId here = Game::DimensionFromRaw(player->getDimensionId());
            const glm::dvec3 pos = player->getPosition();
            if (here == dimension) {
                out.push_back(PickupSource{player, pos});
                continue;
            }
            if (!registry) continue;
            for (const Portal* p : registry->CollectNear(here, pos + glm::dvec3(0.0, 0.9, 0.0), kPickupReach)) {
                if (p->IsMirror() || p->destDimension != dimension) continue;
                if (!p->Has(PortalFlag::Teleportable)) continue;
                out.push_back(PickupSource{player, p->TransformPoint(pos)});
            }
        }
        return out;
    }

    const Portal* EntityPortalTravel::FindCrossing(const std::vector<const Portal*>& portals,
                                                   const glm::dvec3& from, const glm::dvec3& to) {
        const glm::dvec3 delta = to - from;
        const double len2 = glm::dot(delta, delta);
        if (len2 < 1e-12 || len2 > kMaxSegmentLength * kMaxSegmentLength) return nullptr;
        const Portal* best = nullptr;
        double bestT = 2.0;
        for (const Portal* p : portals) {
            const auto hit = p->RaytraceSegment(from, to, std::max(kEdgeLeniency, p->CrossingLeniency()));
            if (!hit || hit->t >= bestT) continue;
            best = p;
            bestT = hit->t;
        }
        return best;
    }

    void EntityPortalTravel::FlushDeferredRemovals(Game::DimensionId dimension) {
        PlayerSessionManager* sessions = m_server.GetSessionManager();
        for (auto it = m_deferredRemovals.begin(); it != m_deferredRemovals.end();) {
            if (it->dimension != dimension) { ++it; continue; }
            if (sessions) {
                if (auto session = sessions->GetSession(it->connectionId); session && session->GetConnection()) {
                    session->GetConnection()->SendPacketIn(it->dimension, it->packetId, it->payload);
                }
            }
            it = m_deferredRemovals.erase(it);
        }
    }

    void EntityPortalTravel::Tick(ServerLevel& level, int64_t serverTick) {
        FlushDeferredRemovals(level.Dimension());
        ImmersivePortalRegistry* registry = m_server.ImmersivePortals();
        if (!registry) return;

        // Every surface in this level that carries entities. Pointers into
        // the registry are stable for the tick: nothing edits it while the
        // levels tick.
        std::vector<const Portal*> portals;
        registry->ForEachInDimension(level.Dimension(), [&](const Portal& p) {
            if (p.Has(PortalFlag::Teleportable)) portals.push_back(&p);
        });

        TickChases(level, serverTick);
        if (portals.empty()) {
            if (!m_looseCooldownUntil.empty() && (serverTick % 200) == 0) {
                for (auto it = m_looseCooldownUntil.begin(); it != m_looseCooldownUntil.end();) {
                    it = it->second <= serverTick ? m_looseCooldownUntil.erase(it) : std::next(it);
                }
            }
            return;
        }

        PROFILE_ZONE_N("ImmersivePortals.EntityTravel");
        TickPlayers(level, portals, serverTick);
        TickNotice(level, portals, serverTick);
        TickMobs(level, portals, serverTick);
        TickItems(level, portals, serverTick);
        TickOrbs(level, portals, serverTick);
    }

    // ── Players ──────────────────────────────────────────────────────────

    void EntityPortalTravel::TickPlayers(ServerLevel& level, const std::vector<const Portal*>& portals,
                                         int64_t serverTick) {
        PlayerSessionManager* sessions = m_server.GetSessionManager();
        if (!sessions) return;

        for (auto& session : sessions->GetAllSessions()) {
            if (!session) continue;
            ServerPlayer* player = session->GetPlayer();
            if (!player || player->isDead()) continue;
            if (Game::DimensionFromRaw(player->getDimensionId()) != level.Dimension()) continue;

            const uint32_t id = session->GetConnectionId();
            const glm::dvec3 eye = player->getPosition() + glm::dvec3(0.0, static_cast<double>(player->getEyeHeight()), 0.0);
            PlayerTrack& track = m_playerTracks[id];
            if (track.dimension != level.Dimension()) {
                // First sight of them in this level: start the watch here.
                track.dimension = level.Dimension();
                track.eye = eye;
                continue;
            }
            const glm::dvec3 lastEye = track.eye;
            track.eye = eye;
            if (track.cooldownUntil > serverTick) continue;

            const Portal* crossed = nullptr;
            {
                const glm::dvec3 delta = eye - lastEye;
                const double len2 = glm::dot(delta, delta);
                if (len2 < 1e-12 || len2 > kMaxSegmentLength * kMaxSegmentLength) continue;
                double bestT = 2.0;
                for (const Portal* p : portals) {
                    if (p->specificPlayerId != 0 && p->specificPlayerId != player->getPlayerId()) continue;
                    const auto hit = p->RaytraceSegment(lastEye, eye, std::max(kEdgeLeniency, p->CrossingLeniency()));
                    if (!hit || hit->t >= bestT) continue;
                    crossed = p;
                    bestT = hit->t;
                }
            }
            if (!crossed) continue;

            // Face the way the view maps through the surface (the client's
            // rule): the full view vector, rotated, then yaw and pitch.
            const glm::vec3 forward = Game::Mth::ViewVector(player->getPitch(), player->getYaw());
            const glm::dvec3 newForward = glm::normalize(crossed->TransformLocalVecNonScale(glm::dvec3(forward)));
            const float newYaw   = Game::Mth::YRotFromVector(glm::vec3(newForward));
            const float newPitch = Game::Mth::XRotFromVector(glm::vec3(newForward));
            m_server.TeleportPlayerThroughPortal(*session, *crossed, eye, newYaw, newPitch,
                                                 /*clientPredicted=*/false);
        }
    }

    void EntityPortalTravel::OnPlayerTeleported(uint32_t connectionId, Game::DimensionId dimension,
                                                const glm::dvec3& newEye, int64_t serverTick) {
        PlayerTrack& track = m_playerTracks[connectionId];
        track.dimension     = dimension;
        track.eye           = newEye;
        track.cooldownUntil = serverTick + 5;
    }

    // ── Noticing through portals ─────────────────────────────────────────
    //
    // A hostile mob with nothing to hunt looks THROUGH every surface near
    // it: each player on the far side has an image on this side (the far
    // position mapped back), and if that image is within the mob's follow
    // range and the mob can see the surface, the mob starts a chase — the
    // same walk-to-the-portal-and-through that OnPlayerCrossed starts when
    // a target escapes, retargeting the real player on arrival. The mod's
    // mobs have no cross-portal sight; this is what "seeing you through a
    // portal" means here: a line of sight to the surface and a target
    // behind it, not a path planned across the seam.

    namespace {
        bool ClearLineToSurface(ServerLevel& level, const glm::dvec3& from, const glm::dvec3& to) {
            Game::World* world = level.World();
            if (!world) return false;
            const glm::dvec3 delta = to - from;
            const double len = glm::length(delta);
            if (len < 1e-6) return true;
            const glm::dvec3 step = delta / len * 0.25;
            glm::dvec3 p = from;
            // Stop a quarter block short of the surface: the cell the
            // surface sits in is its frame's interior, air by construction.
            for (double t = 0.0; t < len - 0.25; t += 0.25, p += step) {
                const Game::BlockID id = world->GetBlock(static_cast<int>(std::floor(p.x)),
                                                         static_cast<int>(std::floor(p.y)),
                                                         static_cast<int>(std::floor(p.z)));
                if (Game::BlockRegistry::HasCollision(id)) return false;
            }
            return true;
        }
    }

    void EntityPortalTravel::TickNotice(ServerLevel& level, const std::vector<const Portal*>& portals,
                                        int64_t serverTick) {
        // Every ten ticks, the cadence of MC's own target reselection.
        if ((serverTick % 10) != 0) return;
        MobManager* mobs = level.Mobs();
        PlayerSessionManager* sessions = m_server.GetSessionManager();
        if (!mobs || !sessions) return;
        constexpr double kNoticeRadius = 16.0;   // portal within this of the mob

        const auto allSessions = sessions->GetAllSessions();
        for (Game::Mob* mob : mobs->List()) {
            if (!mob || !mob->IsAlive() || mob->IsDeadOrDying()) continue;
            if (!mob->HasAiControls() || mob->IsNoAi()) continue;
            if (mob->IsPassenger() || mob->IsVehicle()) continue;
            // Two kinds look through a surface: a hostile mob with no
            // target, hunting; and an animal, tempted by food held on the
            // far side — the wheat a player carries into the Nether keeps
            // its cows coming, as it would across any field.
            const bool hostile = mob->TypeInfo().category == Game::MobCategory::Monster;
            const Game::Animal* animal = hostile ? nullptr : dynamic_cast<const Game::Animal*>(mob);
            if (hostile && mob->GetTarget() != nullptr) continue;
            if (!hostile && !animal) continue;
            bool chasing = false;
            for (const Chase& c : m_chases) if (c.mobId == mob->GetId()) { chasing = true; break; }
            if (chasing) continue;
            // An animal already tempted by someone on THIS side keeps to
            // its own goal.
            if (animal && level.MobLevel()) {
                const double temptRange = mob->GetAttributeValue(Game::Attribute::TemptRange);
                bool temptedHere = false;
                for (const PlayerEntityView* view : level.MobLevel()->PlayerViews()) {
                    if (!view || glm::length(view->position - mob->position) > temptRange) continue;
                    if (animal->IsFood(level.MobLevel()->GetHeldItemId(*view))) { temptedHere = true; break; }
                }
                if (temptedHere) continue;
            }

            const double followRange = animal ? mob->GetAttributeValue(Game::Attribute::TemptRange)
                                              : mob->GetAttributeValue(Game::Attribute::FollowRange);
            const glm::dvec3 eye = EyeOf(*mob, mob->position);

            for (const Portal* portal : portals) {
                if (portal->specificPlayerId != 0) continue;
                if (!portal->IsInFront(eye)) continue;
                glm::dvec3 mn, mx;
                portal->BoundingBox(mn, mx, 0.0);
                if (glm::length(glm::clamp(eye, mn, mx) - eye) > kNoticeRadius) continue;

                const Game::DimensionId dest = portal->IsMirror() ? level.Dimension() : portal->destDimension;
                ServerLevel* far = m_server.GetLevel(dest);
                if (!far || !far->MobLevel()) continue;

                for (const auto& session : allSessions) {
                    if (!session) continue;
                    const PlayerEntityView* view = far->MobLevel()->GetPlayerView(session->GetConnectionId());
                    if (!view) continue;
                    // An animal wants the food in that player's hand.
                    if (animal && !animal->IsFood(far->MobLevel()->GetHeldItemId(*view))) continue;
                    // The player's image on this side.
                    const glm::dvec3 image = portal->InverseTransformPoint(view->position);
                    if (glm::length(image - mob->position) > followRange) continue;
                    // Where the mob's line to the image pierces the surface.
                    const auto hit = portal->RaytraceSegment(eye, image, 0.0);
                    if (!hit) continue;
                    // A hunter needs to see its prey; an animal smells the
                    // food (MC's tempt ignores line of sight).
                    if (hostile && !ClearLineToSurface(level, eye, hit->point)) continue;

                    SendMobToPortal(*mob, *portal);
                    m_chases.push_back(Chase{mob->GetId(), level.Dimension(), portal->id,
                                             session->GetConnectionId(),
                                             serverTick + kChaseLifetimeTicks, false,
                                             /*tempt=*/animal != nullptr});
                    chasing = true;
                    break;
                }
                if (chasing) break;
            }
        }
    }

    // ── Mobs ─────────────────────────────────────────────────────────────

    void EntityPortalTravel::TickMobs(ServerLevel& level, const std::vector<const Portal*>& portals,
                                      int64_t serverTick) {
        MobManager* mobs = level.Mobs();
        if (!mobs) return;

        // A copy: MoveMob edits the manager's list.
        const std::vector<Game::Mob*> list = mobs->List();
        for (Game::Mob* mob : list) {
            if (!mob || !mob->IsAlive() || mob->IsDeadOrDying()) continue;
            if (mob->portal.IsOnCooldown()) continue;
            // Riders and vehicles cross as a unit only in the mod's later
            // versions; here neither crosses. The player's mount is the
            // common case and the player's own crossing dismounts them.
            if (mob->IsPassenger() || mob->IsVehicle()) continue;

            const glm::dvec3 from = EyeOf(*mob, mob->oldPosition);
            const glm::dvec3 to   = EyeOf(*mob, mob->position);
            const Portal* portal = FindCrossing(portals, from, to);
            if (!portal) continue;
            if (portal->specificPlayerId != 0) continue;   // a player's private portal
            MoveMob(level, *mob, *portal, serverTick);
        }
    }

    bool EntityPortalTravel::MoveMob(ServerLevel& from, Game::Mob& mob, const Portal& portal,
                                     int64_t serverTick) {
        const Game::DimensionId dest = portal.IsMirror() ? from.Dimension() : portal.destDimension;
        ServerLevel* to = (dest == from.Dimension()) ? &from : m_server.GetLevel(dest);
        if (!to || !to->Mobs() || !to->MobLevel()) return false;

        const glm::dvec3 eye     = EyeOf(mob, mob.position);
        glm::dvec3 newEye  = portal.TransformPoint(eye);
        // Never land short of, or on, the far surface. The mapped eye can
        // sit a hair past it (the crossing was detected the tick the eye
        // went through), and a body that wide straddles the reverse
        // surface — which then sends it straight back the next tick.
        {
            const glm::dvec3 content = portal.ContentDirection();
            const glm::dvec3 farPoint = portal.IsMirror() ? portal.origin : portal.destination;
            const double margin = kArrivalMargin + 0.5 * static_cast<double>(mob.GetBbWidth());
            const double depth  = glm::dot(newEye - farPoint, content);
            if (depth < margin) newEye += content * (margin - depth);
        }
        const glm::dvec3 newFeet = newEye - glm::dvec3(0.0, static_cast<double>(mob.GetEyeHeight()), 0.0);
        if (!ChunkLoadedAt(*to, newFeet)) return false;

        // Motion and facing through the portal's rotation (and scale for the
        // velocity), exactly as the player's traveler does it.
        const glm::dvec3 newVel = portal.TransformLocalVec(mob.velocity);
        const glm::vec3  look   = Game::Mth::ViewVector(mob.xRot, mob.yRot);
        const glm::dvec3 newLook = glm::normalize(portal.TransformLocalVecNonScale(glm::dvec3(look)));
        const float newYaw   = Game::Mth::YRotFromVector(glm::vec3(newLook));
        const float newPitch = Game::Mth::XRotFromVector(glm::vec3(newLook));
        const glm::dvec3 motion = glm::length(mob.position - mob.oldPosition) > 1e-9
            ? glm::normalize(portal.TransformLocalVecNonScale(mob.position - mob.oldPosition))
            : portal.ContentDirection();

        const int32_t id = mob.GetId();
        const bool crossDimension = (to != &from);

        std::unique_ptr<Game::Mob> owned;
        if (crossDimension) {
            // Out of the old level first: the tracker tells every watcher of
            // the old dimension the entity is gone, before the new level's
            // tracker announces it there.
            std::vector<EntityPacketOut> outgoing;
            if (from.MobTracker()) from.MobTracker()->RemoveEntity(id, outgoing);
            for (auto& packet : outgoing) {
                m_deferredRemovals.push_back(DeferredPacket{from.Dimension(), packet.connectionId,
                                                            static_cast<uint8_t>(packet.packetId),
                                                            std::move(packet.payload)});
            }
            owned = from.Mobs()->Extract(id);
            if (!owned) return false;
        }

        mob.position    = newFeet;
        // The next tick's segment starts a hair PAST the far surface, so the
        // reverse portal standing there is not crossed straight back.
        mob.oldPosition = newFeet + motion * kNudge;
        mob.velocity    = newVel;
        mob.yRot = mob.yRotO = newYaw;
        mob.xRot = mob.xRotO = newPitch;
        mob.yBodyRot = mob.yBodyRotO = newYaw;
        mob.yHeadRot = mob.yHeadRotO = newYaw;
        mob.portal.SetCooldown(kMobCooldownTicks);
        if (mob.HasAiControls()) mob.GetNavigation().Stop();
        // A scaled portal scales what goes through, like the player.
        if (!portal.IsMirror() && std::abs(portal.scale - 1.0) > 1e-9) {
            mob.scale = static_cast<float>(mob.scale * portal.scale);
            mob.needsSync = true;
        }

        if (crossDimension) {
            // Nothing of the old level may be remembered across: the goals,
            // the brain and the target keep raw pointers to the entities
            // they watch, flee or hunt — other mobs, the players' views —
            // and those belong to a level this mob is no longer in. The old
            // level clears such pointers among ITS mobs when an entity goes
            // (a player leaving takes its view with it); a mob that had
            // already moved on was out of that sweep, and held the freed
            // view until its next goal tick cast it. (Dropped items are not
            // Entities and are never held this way.)
            for (PlayerEntityView* view : from.MobLevel()->PlayerViews()) mob.ClearReferenceTo(view);
            for (const auto& [otherId, other] : from.Mobs()->All()) {
                if (other.get() != &mob) mob.ClearReferenceTo(other.get());
            }
            mob.SetLevel(to->MobLevel());
            if (!to->Mobs()->AddExisting(std::move(owned))) {
                // Cannot happen with process-wide ids; if it does, the mob is
                // gone rather than duplicated.
                Log::Warning("[ImmersivePortals] Mob #%d lost crossing #%u: id taken in %s",
                             id, portal.id, std::string(Game::DimensionName(dest)).c_str());
                return false;
            }
        }

        // A chase that reached the portal: its target is now on the far side.
        for (Chase& chase : m_chases) {
            if (chase.mobId == id && !chase.arrived) {
                chase.arrived      = true;
                chase.mobDimension = dest;
                chase.expiresTick  = serverTick + kChaseLifetimeTicks;
            }
        }

        Log::Info("[ImmersivePortals] Mob #%d crossed #%u -> %s (%.1f, %.1f, %.1f)",
                   id, portal.id, std::string(Game::DimensionName(dest)).c_str(),
                   newFeet.x, newFeet.y, newFeet.z);
        Game::Portal::ServerRegistry().OnImmersiveCrossing(portal.tag);
        return true;
    }

    // ── Chasing ──────────────────────────────────────────────────────────

    void EntityPortalTravel::SendMobToPortal(Game::Mob& mob, const Portal& portal) const {
        if (!mob.HasAiControls()) return;
        // Aim at the point of the surface level with the mob's feet, a
        // little INTO it, so the eye segment actually pierces the plane.
        const glm::dvec3 local = portal.WorldToLocal(mob.position);
        const double u = std::clamp(local.x, -portal.HalfWidth()  + 0.3, portal.HalfWidth()  - 0.3);
        const double v = std::clamp(local.y, -portal.HalfHeight() + 0.3, portal.HalfHeight() - 0.3);
        // Into the surface from THIS side: minus the normal, in this level's
        // space. ContentDirection is the far level's vector and only agreed
        // with it for an unrotated portal.
        const glm::dvec3 target = portal.LocalToWorld(u, v) - portal.Normal() * 0.75;
        mob.GetNavigation().MoveTo(target.x, target.y, target.z, kChaseSpeed);
    }

    void EntityPortalTravel::OnPlayerCrossed(ServerLevel& from, const Portal& portal,
                                             uint32_t connectionId, int64_t serverTick) {
        if (!from.Mobs() || !from.MobLevel()) return;
        const PlayerEntityView* view = from.MobLevel()->GetPlayerView(connectionId);
        if (!view) return;

        for (Game::Mob* mob : from.Mobs()->List()) {
            if (!mob || mob->GetTarget() != view) continue;
            if (glm::length(mob->position - portal.origin) > kChaseRadius) continue;
            if (!mob->HasAiControls() || mob->IsNoAi()) continue;
            if (mob->IsPassenger() || mob->IsVehicle()) continue;

            SendMobToPortal(*mob, portal);

            bool known = false;
            for (Chase& chase : m_chases) {
                if (chase.mobId != mob->GetId()) continue;
                chase = Chase{mob->GetId(), from.Dimension(), portal.id, connectionId,
                              serverTick + kChaseLifetimeTicks, false};
                known = true;
                break;
            }
            if (!known) {
                m_chases.push_back(Chase{mob->GetId(), from.Dimension(), portal.id, connectionId,
                                         serverTick + kChaseLifetimeTicks, false});
            }
        }
    }

    void EntityPortalTravel::TickChases(ServerLevel& level, int64_t serverTick) {
        if (m_chases.empty()) return;
        ImmersivePortalRegistry* registry = m_server.ImmersivePortals();

        for (auto it = m_chases.begin(); it != m_chases.end();) {
            Chase& chase = *it;
            if (chase.mobDimension != level.Dimension()) { ++it; continue; }

            Game::Mob* mob = level.Mobs() ? level.Mobs()->Find(chase.mobId) : nullptr;
            if (!mob || !mob->IsAlive() || chase.expiresTick <= serverTick) {
                it = m_chases.erase(it);
                continue;
            }

            if (chase.arrived) {
                // A tempted animal is now in the player's level: its own
                // TemptGoal finds the food from here.
                if (chase.tempt) { it = m_chases.erase(it); continue; }
                // Resume the hunt the moment the player's view exists here.
                if (PlayerEntityView* view = level.MobLevel()
                        ? level.MobLevel()->GetPlayerView(chase.connectionId) : nullptr) {
                    mob->SetTarget(view);
                    it = m_chases.erase(it);
                    continue;
                }
                ++it;
                continue;
            }

            // Still on the near side: keep walking at the portal. A goal that
            // took the navigation over (a wander) ends the walk; re-issue it.
            const Portal* portal = registry ? registry->Get(chase.portalId) : nullptr;
            if (!portal || portal->dimension != level.Dimension()) {
                it = m_chases.erase(it);
                continue;
            }
            if (chase.tempt) {
                // Still worth the walk only while the food is out.
                const Game::DimensionId dest = portal->IsMirror() ? level.Dimension() : portal->destDimension;
                ServerLevel* far = m_server.GetLevel(dest);
                const PlayerEntityView* view = (far && far->MobLevel())
                    ? far->MobLevel()->GetPlayerView(chase.connectionId) : nullptr;
                const Game::Animal* animal = dynamic_cast<const Game::Animal*>(mob);
                if (!view || !animal || !animal->IsFood(far->MobLevel()->GetHeldItemId(*view))) {
                    if (mob->HasAiControls()) mob->GetNavigation().Stop();
                    it = m_chases.erase(it);
                    continue;
                }
            }
            // A hunter that has found something to fight on this side is
            // fighting it, not walking to a portal — before this, a piglin
            // hit beside the frame ran straight back to the portal.
            if (!chase.tempt && mob->GetTarget() != nullptr) {
                it = m_chases.erase(it);
                continue;
            }
            if (mob->HasAiControls() && mob->GetNavigation().IsDone()) {
                constexpr int kMaxReissues = 3;
                if (++chase.reissues > kMaxReissues) {
                    // Arrived at the frame and did not go through: the
                    // way in is not walkable for it. Let it go.
                    mob->GetNavigation().Stop();
                    it = m_chases.erase(it);
                    continue;
                }
                SendMobToPortal(*mob, *portal);
            }
            ++it;
        }
    }

    // ── Dropped items ────────────────────────────────────────────────────

    void EntityPortalTravel::TickItems(ServerLevel& level, const std::vector<const Portal*>& portals,
                                       int64_t serverTick) {
        ItemEntityManager* items = level.Items();
        if (!items) return;

        std::vector<int32_t> ids;
        ids.reserve(items->Count());
        for (const auto& [id, e] : items->All()) ids.push_back(id);

        std::vector<int32_t> removedHere;
        std::vector<std::pair<ServerLevel*, int32_t>> arrivedIds;
        for (int32_t id : ids) {
            Game::ItemEntity* e = items->Find(id);
            if (!e) continue;
            if (auto cd = m_looseCooldownUntil.find(id); cd != m_looseCooldownUntil.end()) {
                if (cd->second > serverTick) continue;
                m_looseCooldownUntil.erase(cd);
            }
            if (glm::length(e->vel) < kMinLooseCrossSpeed) continue;
            const glm::dvec3 centre(e->pos.x, e->pos.y + 0.125, e->pos.z);
            const Portal* portal = FindCrossing(portals, centre - e->vel, centre);
            if (!portal || portal->specificPlayerId != 0) continue;

            const Game::DimensionId dest = portal->IsMirror() ? level.Dimension() : portal->destDimension;
            ServerLevel* to = (dest == level.Dimension()) ? &level : m_server.GetLevel(dest);
            if (!to || !to->Items()) continue;

            const glm::dvec3 newCentre = portal->TransformPoint(centre);
            const glm::dvec3 newPos(newCentre.x, newCentre.y - 0.125, newCentre.z);
            if (!ChunkLoadedAt(*to, newPos)) continue;
            const glm::dvec3 newVel = portal->TransformLocalVec(e->vel);

            m_looseCooldownUntil[id] = serverTick + kLooseCooldownTicks;
            if (to == &level) {
                e->pos = newPos + glm::normalize(glm::length(newVel) > 1e-9 ? newVel : portal->ContentDirection()) * kNudge;
                e->vel = newVel;
                if (!portal->IsMirror()) e->scale = static_cast<float>(e->scale * portal->scale);
                e->pendingSpawn = true;   // a full refresh: the client must snap, not lerp
                e->needsSync    = true;
                continue;
            }

            auto moved = items->Extract(id);
            if (!moved) continue;
            moved->pos = newPos + glm::normalize(glm::length(newVel) > 1e-9 ? newVel : portal->ContentDirection()) * kNudge;
            moved->vel = newVel;
            if (!portal->IsMirror()) moved->scale = static_cast<float>(moved->scale * portal->scale);
            removedHere.push_back(id);
            const glm::dvec3 arrived = moved->pos;
            if (!to->Items()->AdoptWithId(std::move(*moved))) {
                Log::Warning("[ImmersivePortals] Item #%d lost crossing #%u", id, portal->id);
            } else {
                Log::Info("[ImmersivePortals] Item #%d crossed #%u -> %s at (%.1f, %.1f, %.1f)",
                          id, portal->id, std::string(Game::DimensionName(dest)).c_str(),
                          arrived.x, arrived.y, arrived.z);
                Game::Portal::ServerRegistry().OnImmersiveCrossing(portal->tag);
                arrivedIds.emplace_back(to, id);
            }
        }
        // The far spawn goes out NOW rather than with the far level's next
        // sync pass, and BEFORE the removal: the client hands the entity
        // over between its stores when the far spawn arrives (carrying its
        // render state through the portal) and drops the old copy itself;
        // the removal that follows is then a no-op. The other order would
        // leave nothing to hand over.
        for (const auto& [toLevel, id] : arrivedIds) {
            m_server.BroadcastItemEntitySpawn(*toLevel, id);
        }
        if (!removedHere.empty()) {
            m_server.BroadcastItemEntityRemovals(level.Dimension(), removedHere);
        }
    }

    // ── Experience orbs ──────────────────────────────────────────────────

    void EntityPortalTravel::TickOrbs(ServerLevel& level, const std::vector<const Portal*>& portals,
                                      int64_t serverTick) {
        ExperienceOrbManager* orbs = level.Orbs();
        if (!orbs) return;

        std::vector<int32_t> ids;
        ids.reserve(orbs->Count());
        for (const auto& [id, orb] : orbs->All()) ids.push_back(id);

        std::vector<int32_t> removedHere;
        for (int32_t id : ids) {
            auto& all = orbs->AllMutable();
            auto it = all.find(id);
            if (it == all.end()) continue;
            Game::ExperienceOrb& orb = it->second;
            if (orb.pickedUp) continue;
            if (auto cd = m_looseCooldownUntil.find(id); cd != m_looseCooldownUntil.end()) {
                if (cd->second > serverTick) continue;
                m_looseCooldownUntil.erase(cd);
            }
            if (glm::length(orb.vel) < kMinLooseCrossSpeed) continue;
            const glm::dvec3 centre(orb.pos.x, orb.pos.y + 0.25, orb.pos.z);
            const Portal* portal = FindCrossing(portals, centre - orb.vel, centre);
            if (!portal || portal->specificPlayerId != 0) continue;

            const Game::DimensionId dest = portal->IsMirror() ? level.Dimension() : portal->destDimension;
            ServerLevel* to = (dest == level.Dimension()) ? &level : m_server.GetLevel(dest);
            if (!to || !to->Orbs()) continue;

            const glm::dvec3 newCentre = portal->TransformPoint(centre);
            const glm::dvec3 newPos(newCentre.x, newCentre.y - 0.25, newCentre.z);
            if (!ChunkLoadedAt(*to, newPos)) continue;
            const glm::dvec3 newVel = portal->TransformLocalVec(orb.vel);
            const glm::dvec3 nudge = glm::normalize(glm::length(newVel) > 1e-9 ? newVel : portal->ContentDirection()) * kNudge;

            m_looseCooldownUntil[id] = serverTick + kLooseCooldownTicks;
            if (to == &level) {
                orb.pos = newPos + nudge;
                orb.vel = newVel;
                orb.pendingSpawn = true;
                orb.needsSync    = true;
                continue;
            }

            auto moved = orbs->Extract(id);
            if (!moved) continue;
            moved->pos = newPos + nudge;
            moved->vel = newVel;
            removedHere.push_back(id);
            if (!to->Orbs()->AdoptWithId(std::move(*moved))) {
                Log::Warning("[ImmersivePortals] Orb #%d lost crossing #%u", id, portal->id);
            }
        }
        if (!removedHere.empty()) {
            m_server.BroadcastItemEntityRemovals(level.Dimension(), removedHere);
        }
    }

} // namespace Server

#endif // ENABLE_IMMERSIVE_PORTALS
