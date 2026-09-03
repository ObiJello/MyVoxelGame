// File: src/server/level/PortalTravel.cpp
//
// Line references are to minecraft_code/decompiled_net/minecraft/world/level/
// block/NetherPortalBlock.java and EndPortalBlock.java.

#include "PortalTravel.hpp"

#include "PortalForcer.hpp"
#include "ServerLevel.hpp"
#include "server/IntegratedServer.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"
#include "server/world/ticketing/ChunkTicketManager.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/entity/Entity.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/entity/BlockEntityType.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/block/entity/EndGatewayBlockEntity.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/level/World.hpp"
#include "server/level/EndDragonFight.hpp"
#include "common/world/portal/BlockUtil.hpp"
#include "common/world/portal/PortalShape.hpp"

#include <algorithm>
#include <cmath>

namespace Server {
    namespace PortalTravel {

        namespace {

            // MC ServerLevel.END_SPAWN_POINT (ServerLevel.java:188).
            constexpr glm::ivec3 kEndSpawnPoint{100, 50, 0};

            // MC Entity.placePortalTicket (Entity.java:3146) — TicketType.PORTAL
            // at radius 3. Holds the exit chunks loaded for the moment between
            // arriving and the player's own watch set catching up; without it
            // you land in a void and fall.
            constexpr int kPortalTicketTicks = 300;   // MC's PORTAL ticket lifetime

            // MC EndPlatformFeature.createEndPlatform (EndPlatformFeature.java:21).
            // A 5x5 obsidian pad with three blocks of air above it, cleared of
            // whatever was there. Built EVERY arrival, which is what makes the
            // End entrance survivable no matter what the terrain did.
            void CreateEndPlatform(Game::World& world, const glm::ivec3& origin) {
                for (int dz = -2; dz <= 2; ++dz) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        for (int dy = -1; dy < 3; ++dy) {
                            const glm::ivec3 p{ origin.x + dx, origin.y + dy, origin.z + dz };
                            const Game::BlockID want = (dy == -1) ? Game::BlockID::Obsidian
                                                                  : Game::BlockID::Air;
                            if (world.GetBlock(p.x, p.y, p.z) == want) continue;
                            world.SetBlock(p.x, p.y, p.z, want,
                                           Game::World::UpdateFlags::All);
                        }
                    }
                }
            }

            // The horizontal axis of a nether_portal state, defaulting to X for
            // anything that is not one (MC's `getOptionalValue(...).orElse(X)`).
            Game::Axis PortalAxisOf(Game::BlockState state) {
                if (!state.Is(Game::BlockID::NetherPortal)) return Game::Axis::X;
                return state.GetName(Game::PropertyId::HORIZONTAL_AXIS) == "z"
                           ? Game::Axis::Z : Game::Axis::X;
            }

            // MC BlockUtil.getLargestRectangleAround(pos, axis, 21, Y, 21,
            // p -> level.getBlockState(p) == portalState) — the opening this
            // cell belongs to. Exact STATE equality, not just "is a portal":
            // two portals of different axes meeting at a corner are two
            // openings, and merging them would misplace the exit.
            Game::FoundRectangle OpeningAround(const Game::World& world,
                                               const glm::ivec3& pos, Game::Axis axis) {
                const Game::BlockState want = world.GetBlockState(pos.x, pos.y, pos.z);
                return Game::GetLargestRectangleAround(
                    pos, axis, Game::PortalShape::kMaxWidth,
                    Game::Axis::Y, Game::PortalShape::kMaxHeight,
                    [&world, want](const glm::ivec3& p) {
                        return world.GetBlockState(p.x, p.y, p.z) == want;
                    });
            }

            struct Landing {
                glm::dvec3 position{0.0};
                float      yRotDelta = 0.0f;   // ADDED to the current yaw (MC Relative.ROTATION)
                bool       absoluteRotation = false;
                float      yRotAbsolute = 0.0f;
                float      xRotAbsolute = 0.0f;
            };

            // MC NetherPortalBlock.createDimensionTransition (:161).
            Landing AlignToExit(const Game::World& toWorld,
                                const Game::FoundRectangle& exit,
                                Game::Axis entryAxis, const glm::dvec3& relative,
                                float entityWidth, float entityHeight) {
                const glm::ivec3 bottomLeft = exit.minCorner;
                const Game::Axis exitAxis = PortalAxisOf(
                    toWorld.GetBlockState(bottomLeft.x, bottomLeft.y, bottomLeft.z));

                const double width  = static_cast<double>(exit.axis1Size);
                const double height = static_cast<double>(exit.axis2Size);

                // Walking into an X portal and out of a Z one turns you 90°.
                // Without this you arrive facing into the obsidian.
                const float outputRotation = (entryAxis == exitAxis) ? 0.0f : 90.0f;

                const double offsetRight   = entityWidth / 2.0
                                           + (width - entityWidth) * relative.x;
                const double offsetUp      = (height - entityHeight) * relative.y;
                const double offsetForward = 0.5 + relative.z;

                const bool xAligned = (exitAxis == Game::Axis::X);
                const glm::dvec3 target{
                    static_cast<double>(bottomLeft.x) + (xAligned ? offsetRight : offsetForward),
                    static_cast<double>(bottomLeft.y) + offsetUp,
                    static_cast<double>(bottomLeft.z) + (xAligned ? offsetForward : offsetRight),
                };

                Landing landing;
                landing.position = Game::PortalShape::FindCollisionFreePosition(
                    toWorld, target, entityWidth, entityHeight);
                landing.yRotDelta = outputRotation;
                return landing;
            }

            // Hold the destination chunks loaded across the hand-off.
            //
            // ONE ticket, at FULL - radius, exactly as MC writes it — the
            // ticket manager's distance propagation covers the radius. This
            // used to fill the square explicitly because the manager had no
            // propagation; it does now, and filling a square by hand is the
            // pattern that produced the frozen-world bug, so it does not come
            // back even where it would have been harmless.
            void PlacePortalTicket(ServerLevel& level, const glm::dvec3& pos) {
                if (!level.Tickets()) return;
                const auto centre = Game::Math::WorldCoordinates::WorldToChunkPos(
                    static_cast<int>(std::floor(pos.x)),
                    static_cast<int>(std::floor(pos.z)));

                level.Tickets()->AddTemporaryTicket(
                    Game::Math::ChunkPos(centre.x, centre.z),
                    ChunkLevel::FULL - PortalForcer::kTicketRadius,
                    kPortalTicketTicks);
            }

            // MC PoiManager.ensureLoadedAndValid — bring the destination area
            // into memory BEFORE looking for or building a portal there.
            //
            // This is not an optimisation, it is the difference between working
            // and not. A dimension nobody has visited has no resident chunks at
            // all, so every block read answers air and every heightmap answers
            // the world floor: the spiral finds no column that can host a
            // frame, falls through to the carve-a-platform branch, and then
            // writes obsidian into chunks that do not exist. The portal would
            // simply never appear.
            //
            // `World::GetChunk` is the blocking form — it goes to disk, then to
            // the generator, and waits. That is a real stall (terrain
            // generation is ~66 ms/chunk here), and it is the same stall
            // vanilla has when you first step into the Nether. It happens once
            // per new portal, not per crossing.
            //
            // Radius 2 chunks covers PortalForcer's 16-block spiral plus the
            // frame it may build at the edge of it.
            void EnsureExitAreaLoaded(ServerLevel& level, const glm::ivec3& around) {
                constexpr int kChunkRadius = 2;
                const auto centre = Game::Math::WorldCoordinates::WorldToChunkPos(
                    around.x, around.z);

                // Tickets first, so nothing unloads between the generation
                // below and the player actually arriving.
                if (level.Tickets()) {
                    for (int dx = -kChunkRadius; dx <= kChunkRadius; ++dx) {
                        for (int dz = -kChunkRadius; dz <= kChunkRadius; ++dz) {
                            level.Tickets()->AddTemporaryTicket(
                                Game::Math::ChunkPos(centre.x + dx, centre.z + dz),
                                ChunkTicketManager::ENTITY_TICKING_LEVEL,
                                kPortalTicketTicks);
                        }
                    }
                }

                for (int dx = -kChunkRadius; dx <= kChunkRadius; ++dx) {
                    for (int dz = -kChunkRadius; dz <= kChunkRadius; ++dz) {
                        level.World()->GetChunk(centre.x + dx, centre.z + dz);
                    }
                }
            }

            // Move a PLAYER. Everything else is logged and left where it is —
            // see the note at the bottom of Traverse.
            void MovePlayer(IntegratedServer& server, ServerLevel& from, ServerLevel& to,
                            Server::ServerPlayer& player, const Landing& landing) {
                auto* sessions = server.GetSessionManager();
                if (!sessions) return;
                auto session = sessions->GetSession(player.getPlayerId());
                if (!session) {
                    Log::Warning("[PortalTravel] Player %u has no session; not travelling",
                                 player.getPlayerId());
                    return;
                }

                const float yaw   = landing.absoluteRotation
                                        ? landing.yRotAbsolute
                                        : player.getYaw() + landing.yRotDelta;
                const float pitch = landing.absoluteRotation ? landing.xRotAbsolute
                                                             : player.getPitch();

                // Drop the tickets held in the level being LEFT before the
                // session's dimension flips. UpdatePlayerTickets removes
                // against whichever level the session claims, so doing this
                // after the flip would aim the removal at the wrong manager and
                // pin the old dimension's chunks for the rest of the session.
                if (from.Tickets()) {
                    from.Tickets()->RemoveAllPlayerTickets(player.getPlayerId());
                }

                PlacePortalTicket(to, landing.position);

                session->ChangeDimension(Game::DimensionToRaw(to.Dimension()),
                                         glm::vec3(landing.position));

                player.setRotation(yaw, pitch);
                if (auto* connection = session->GetConnection()) {
                    // Velocity is deliberately zeroed. MC carries momentum
                    // through with Relative.DELTA, but the wire's dx/dy/dz is
                    // read into the client's per-SECOND physics field while the
                    // server's is per TICK, and a silent 20x is worse than
                    // arriving still.
                    connection->Teleport(landing.position.x, landing.position.y,
                                         landing.position.z, yaw, pitch,
                                         0.0, 0.0, 0.0);
                }

                Log::Info("[PortalTravel] Player %u: '%s' -> '%s' at (%.1f, %.1f, %.1f)",
                          player.getPlayerId(),
                          std::string(Game::DimensionName(from.Dimension())).c_str(),
                          std::string(Game::DimensionName(to.Dimension())).c_str(),
                          landing.position.x, landing.position.y, landing.position.z);
            }

            // ── End gateways (MC TheEndGatewayBlockEntity) ─────────────────

            // The gateway's block entity, created lazily for worldgen-placed
            // gateways (worldgen writes blocks, never block entities — see
            // EndGatewayBlockEntity.hpp).
            Game::EndGatewayBlockEntity* GetOrCreateGatewayEntity(
                Game::World& world, const glm::ivec3& pos) {
                if (world.GetBlock(pos.x, pos.y, pos.z) != Game::BlockID::EndGateway) {
                    return nullptr;
                }
                const auto chunkPos =
                    Game::Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
                auto chunk = world.GetChunk(chunkPos.x, chunkPos.z);
                if (!chunk) return nullptr;
                const int localX = pos.x - chunkPos.x * 16;
                const int localZ = pos.z - chunkPos.z * 16;
                if (auto* be = chunk->GetBlockEntity(localX, pos.y, localZ)) {
                    return dynamic_cast<Game::EndGatewayBlockEntity*>(be);
                }
                const auto* type = Game::BlockEntityTypes::ForId(
                    Game::BlockEntityTypeIds::END_GATEWAY);
                if (!type) return nullptr;
                auto be = type->Create(pos, Game::BlockID::EndGateway);
                auto* raw = dynamic_cast<Game::EndGatewayBlockEntity*>(be.get());
                chunk->SetBlockEntity(localX, pos.y, localZ, std::move(be));
                return raw;
            }

            // MC TheEndGatewayBlockEntity.findTallestBlock: the highest
            // collision-shape-full block within `dist` of `around` in X/Z
            // (optionally skipping bedrock), or `around` when there is none.
            glm::ivec3 FindTallestBlock(Game::World& world, const glm::ivec3& around,
                                        int dist, bool allowBedrock) {
                const int maxY = Game::DimensionMinY(Game::DimensionId::End) +
                                 Game::DimensionLogicalHeight(Game::DimensionId::End) - 1;
                glm::ivec3 tallest = around;
                bool found = false;
                for (int xd = -dist; xd <= dist; ++xd) {
                    for (int zd = -dist; zd <= dist; ++zd) {
                        if (xd == 0 && zd == 0 && !allowBedrock) continue;
                        for (int y = maxY; y > (found ? tallest.y : 0); --y) {
                            const Game::BlockID id = world.GetBlock(
                                around.x + xd, y, around.z + zd);
                            if (id == Game::BlockID::Air) continue;
                            if (!Game::BlockRegistry::HasCollision(id)) continue;
                            if (!allowBedrock && id == Game::BlockID::Bedrock) continue;
                            tallest = glm::ivec3(around.x + xd, y, around.z + zd);
                            found = true;
                            break;
                        }
                    }
                }
                return tallest;
            }

            // MC findValidSpawnInChunk: the END_STONE block with two clear
            // cells above it, closest to the WORLD origin (vanilla's quirk).
            bool FindValidSpawnInChunk(Game::World& world, int chunkX, int chunkZ,
                                       glm::ivec3& out) {
                bool found = false;
                double bestDist = 0.0;
                for (int x = chunkX * 16; x < chunkX * 16 + 16; ++x) {
                    for (int z = chunkZ * 16; z < chunkZ * 16 + 16; ++z) {
                        for (int y = 30; y < 255; ++y) {
                            if (world.GetBlock(x, y, z) != Game::BlockID::EndStone) {
                                continue;
                            }
                            if (Game::BlockRegistry::HasCollision(
                                    world.GetBlock(x, y + 1, z)) ||
                                Game::BlockRegistry::HasCollision(
                                    world.GetBlock(x, y + 2, z))) {
                                continue;
                            }
                            const double dist = static_cast<double>(x) * x +
                                                static_cast<double>(y) * y +
                                                static_cast<double>(z) * z;
                            if (!found || dist < bestDist) {
                                found = true;
                                bestDist = dist;
                                out = glm::ivec3(x, y, z);
                            }
                        }
                    }
                }
                return found;
            }

            // MC EndIslandFeature.place, transcribed: start at a random size
            // (nextInt(3) + 4), lay one end-stone disc per layer downward,
            // shrinking by nextInt(2) + 0.5 each layer until the disc is
            // smaller than half a block — the island a gateway conjures when
            // its ray lands in open void.
            void PlaceSmallEndIsland(Game::World& world, Game::JavaRandom& rng,
                                     const glm::ivec3& origin) {
                float size = static_cast<float>(rng.NextInt(3)) + 4.0f;
                for (int y = 0; size > 0.5f; --y) {
                    const int lo = static_cast<int>(std::floor(-size));
                    const int hi = static_cast<int>(std::ceil(size));
                    for (int x = lo; x <= hi; ++x) {
                        for (int z = lo; z <= hi; ++z) {
                            if (static_cast<float>(x * x + z * z) >
                                (size + 1.0f) * (size + 1.0f)) {
                                continue;
                            }
                            world.SetBlock(origin.x + x, origin.y + y,
                                           origin.z + z,
                                           Game::BlockID::EndStone,
                                           Game::World::UpdateFlags::All);
                        }
                    }
                    size -= static_cast<float>(rng.NextInt(2)) + 0.5f;
                }
            }

            // MC findOrCreateValidTeleportPos + findExitPortalXZPosTentative:
            // walk ~1024 blocks outward along the gateway's bearing, step
            // back over empty chunks and forward over non-empty ones, then
            // pick (or build) somewhere to stand. Loads chunks synchronously
            // — the same freeze vanilla has on a first gateway use.
            glm::ivec3 FindOrCreateGatewayExit(ServerLevel& level,
                                               const glm::ivec3& entryPos) {
                Game::World& world = *level.World();
                glm::dvec3 dir(entryPos.x, 0.0, entryPos.z);
                const double len = std::sqrt(dir.x * dir.x + dir.z * dir.z);
                dir = len > 1.0e-6 ? dir / len : glm::dvec3(1.0, 0.0, 0.0);
                glm::dvec3 tentative = dir * 1024.0;

                const auto chunkEmpty = [&](const glm::dvec3& p) {
                    const auto cp = Game::Math::WorldCoordinates::WorldToChunkPos(
                        static_cast<int>(std::floor(p.x)),
                        static_cast<int>(std::floor(p.z)));
                    auto chunk = world.GetChunk(cp.x, cp.z);
                    return !chunk || chunk->HighestFilledSectionIndex() ==
                                         Game::Chunk::kNoFilledSection;
                };

                for (int i = 16; !chunkEmpty(tentative) && i-- > 0;) {
                    tentative += dir * -16.0;
                }
                for (int i = 16; chunkEmpty(tentative) && i-- > 0;) {
                    tentative += dir * 16.0;
                }

                const auto cp = Game::Math::WorldCoordinates::WorldToChunkPos(
                    static_cast<int>(std::floor(tentative.x)),
                    static_cast<int>(std::floor(tentative.z)));
                glm::ivec3 spawn;
                if (!FindValidSpawnInChunk(world, cp.x, cp.z, spawn)) {
                    spawn = glm::ivec3(
                        static_cast<int>(std::floor(tentative.x + 0.5)), 75,
                        static_cast<int>(std::floor(tentative.z + 0.5)));
                    Log::Info("[PortalTravel] Gateway ray hit open void — "
                              "building an island at (%d, %d, %d)",
                              spawn.x, spawn.y, spawn.z);
                    PlaceSmallEndIsland(world, level.MobLevel()->Random(),
                                        spawn);
                }
                return FindTallestBlock(world, spawn, 16, /*allowBedrock=*/true);
            }

            // MC EndGatewayBlock.getPortalDestination for a player, fused
            // with the teleport itself — a SAME-dimension hop, so the session
            // keeps its dimension and only the position moves.
            void TraverseEndGateway(IntegratedServer& server, ServerLevel& level,
                                    Game::Entity& entity, const glm::ivec3& entryPos) {
                if (level.Dimension() != Game::DimensionId::End) return;
                if (!level.World()) return;
                Game::World& world = *level.World();

                // MC EndGatewayBlock.entityInside teleports ANY entity that
                // canUsePortal — a mob shoved in comes out on the far
                // islands. (Players move by packet below; everything else
                // moves in place.)
                auto* view = dynamic_cast<Server::PlayerEntityView*>(&entity);
                Server::ServerPlayer* player = view ? view->GetPlayer() : nullptr;

                Game::EndGatewayBlockEntity* gateway =
                    GetOrCreateGatewayEntity(world, entryPos);
                if (!gateway) return;
                if (gateway->IsCoolingDown()) return;
                gateway->TriggerCooldown();

                if (!gateway->HasExitPosition()) {
                    // First use: resolve the far end and build the return
                    // gateway 10 above it, aimed back at THIS gateway.
                    const glm::ivec3 ground = FindOrCreateGatewayExit(level, entryPos);
                    const glm::ivec3 exitPortal = ground + glm::ivec3(0, 10, 0);
                    EndDragonFight::PlaceGatewayFrame(world, exitPortal);
                    if (Game::EndGatewayBlockEntity* returnGateway =
                            GetOrCreateGatewayEntity(world, exitPortal)) {
                        // MC EndGatewayConfiguration.knownExit(entry, false).
                        returnGateway->SetExitPosition(entryPos, false);
                    }
                    gateway->SetExitPosition(exitPortal, false);
                }

                const glm::ivec3 exit = gateway->ExitPosition();
                EnsureExitAreaLoaded(level, exit);
                glm::ivec3 landingBlock;
                if (gateway->ExactTeleport()) {
                    landingBlock = exit;
                } else {
                    // MC findExitPosition: tallest non-bedrock block within 5
                    // of exit+2, then one above it.
                    landingBlock = FindTallestBlock(world, exit + glm::ivec3(0, 2, 0),
                                                    5, /*allowBedrock=*/false) +
                                   glm::ivec3(0, 1, 0);
                }

                const glm::dvec3 landing(landingBlock.x + 0.5, landingBlock.y,
                                         landingBlock.z + 0.5);
                PlacePortalTicket(level, landing);

                if (player) {
                    player->portalState().CopyFrom(entity.portal);
                    if (auto* sessions = server.GetSessionManager()) {
                        if (auto session =
                                sessions->GetSession(player->getPlayerId())) {
                            if (auto* connection = session->GetConnection()) {
                                connection->Teleport(landing.x, landing.y,
                                                     landing.z,
                                                     player->getYaw(),
                                                     player->getPitch(),
                                                     0.0, 0.0, 0.0);
                            }
                        }
                    }
                    Log::Info("[PortalTravel] Player %u took a gateway to "
                              "(%.1f, %.1f, %.1f)",
                              player->getPlayerId(), landing.x, landing.y,
                              landing.z);
                } else {
                    // MC's non-pearl TeleportTransition keeps DELTA and
                    // ROTATION relative — the entity arrives moving the way
                    // it entered, fall distance reset by the transition.
                    entity.position = landing;
                    entity.fallDistance = 0.0f;
                    entity.needsSync = true;
                    Log::Info("[PortalTravel] Entity %d took a gateway to "
                              "(%.1f, %.1f, %.1f)",
                              entity.GetId(), landing.x, landing.y, landing.z);
                }
            }

        } // namespace

        void Traverse(IntegratedServer& server, ServerLevel& from, Game::Entity& entity,
                      Game::BlockID portal, const glm::ivec3& entryPos) {
            // End gateways are a hop WITHIN the End, not a dimension change.
            if (portal == Game::BlockID::EndGateway) {
                TraverseEndGateway(server, from, entity, entryPos);
                return;
            }

            const Game::DimensionId fromDim = from.Dimension();

            // ── Which dimension ────────────────────────────────────────────
            // MC NetherPortalBlock.java:108 and EndPortalBlock.java:75.
            Game::DimensionId toDim;
            if (portal == Game::BlockID::NetherPortal) {
                toDim = (fromDim == Game::DimensionId::Nether) ? Game::DimensionId::Overworld
                                                               : Game::DimensionId::Nether;
            } else {
                toDim = (fromDim == Game::DimensionId::End) ? Game::DimensionId::Overworld
                                                            : Game::DimensionId::End;
            }

            ServerLevel* toLevel = server.GetOrCreateLevel(toDim);
            if (!toLevel) {
                Log::Error("[PortalTravel] '%s' could not be created; entity %d stays put",
                           std::string(Game::DimensionName(toDim)).c_str(), entity.GetId());
                return;
            }

            const float entityWidth  = entity.GetBbWidth();
            const float entityHeight = entity.GetBbHeight();
            Landing landing;

            if (portal == Game::BlockID::EndPortal) {
                if (toDim == Game::DimensionId::End) {
                    // Same reason as the nether branch: the platform is written
                    // into chunks that will not exist on a first visit.
                    EnsureExitAreaLoaded(*toLevel, kEndSpawnPoint);
                    // MC EndPortalBlock.java:86 — the platform is built BEFORE
                    // the arrival, every time, so the pad exists even if the
                    // last visitor mined it out.
                    CreateEndPlatform(*toLevel->World(),
                                      glm::ivec3(kEndSpawnPoint.x, kEndSpawnPoint.y - 1,
                                                 kEndSpawnPoint.z));
                    // MC puts a PLAYER one block lower than the block centre
                    // (:91), which lands them standing ON the pad rather than
                    // hovering above it.
                    landing.position = glm::dvec3(kEndSpawnPoint.x + 0.5,
                                                  static_cast<double>(kEndSpawnPoint.y)
                                                      - (entity.IsPlayer() ? 1.0 : 0.0),
                                                  kEndSpawnPoint.z + 0.5);
                    landing.absoluteRotation = true;
                    landing.yRotAbsolute = Game::ToYRot(Game::Direction::West);  // MC :87
                    landing.xRotAbsolute = 0.0f;
                } else {
                    // MC sends the player to their respawn point. This engine
                    // has no bed/anchor respawn, so the world spawn IS the
                    // respawn point — the same fallback MC uses when a bed is
                    // missing.
                    landing.position = glm::dvec3(toLevel->worldSpawn);
                    landing.absoluteRotation = true;
                    landing.yRotAbsolute = entity.yRot;
                    landing.xRotAbsolute = entity.xRot;
                }
            } else {
                // ── Nether portal ──────────────────────────────────────────
                // MC NetherPortalBlock.java:115-117. Only X and Z are scaled;
                // Y is carried across untouched, which is why a portal at
                // y=200 in the Overworld puts you at y=200 in the Nether — on
                // top of the bedrock roof, exactly as in vanilla.
                const double scale = Game::TeleportationScale(fromDim, toDim);
                const glm::ivec3 approximateExit{
                    static_cast<int>(std::floor(entity.position.x * scale)),
                    static_cast<int>(std::floor(entity.position.y)),
                    static_cast<int>(std::floor(entity.position.z * scale)),
                };

                // Where in the ENTRY portal the entity is, proportionally.
                const Game::BlockState entryState = from.World()->GetBlockState(
                    entryPos.x, entryPos.y, entryPos.z);
                Game::Axis  entryAxis = Game::Axis::X;
                glm::dvec3  relative{0.5, 0.0, 0.0};
                if (entryState.Is(Game::BlockID::NetherPortal)) {
                    entryAxis = PortalAxisOf(entryState);
                    const Game::FoundRectangle entryRect =
                        OpeningAround(*from.World(), entryPos, entryAxis);
                    relative = Game::PortalShape::GetRelativePosition(
                        entryRect, entryAxis, entity.position, entityWidth, entityHeight);
                }

                // Must happen before either branch below: both read the
                // destination's blocks, and an unloaded chunk reads as air.
                EnsureExitAreaLoaded(*toLevel, approximateExit);

                const bool toNether = (toDim == Game::DimensionId::Nether);
                Game::FoundRectangle exitRect;

                if (auto existing = PortalForcer::FindClosestPortalPosition(
                        *toLevel, approximateExit, toNether)) {
                    const Game::Axis exitAxis = PortalAxisOf(
                        toLevel->World()->GetBlockState(existing->x, existing->y, existing->z));
                    exitRect = OpeningAround(*toLevel->World(), *existing, exitAxis);
                } else {
                    // MC keeps the SOURCE portal's axis for a portal it builds,
                    // so a linked pair faces the same way.
                    auto created = PortalForcer::CreatePortal(*toLevel, approximateExit,
                                                              entryAxis);
                    if (!created) {
                        Log::Error("[PortalTravel] Could not place an exit portal in '%s'",
                                   std::string(Game::DimensionName(toDim)).c_str());
                        return;
                    }
                    exitRect = *created;
                }

                landing = AlignToExit(*toLevel->World(), exitRect, entryAxis, relative,
                                      entityWidth, entityHeight);
            }

            // ── Execute ────────────────────────────────────────────────────
            if (auto* view = dynamic_cast<Server::PlayerEntityView*>(&entity)) {
                if (Server::ServerPlayer* player = view->GetPlayer()) {
                    // MC Entity.restoreFrom(:3008-3009). This view belongs to
                    // the level being LEFT and SyncPlayerViews will delete it
                    // as soon as the session's dimension flips, taking the
                    // portal cooldown HandleTick armed a moment ago with it.
                    // Park it on the ServerPlayer, which survives the
                    // crossing; the destination's view reads it back in its
                    // constructor.
                    //
                    // Without this the arriving player has cooldown 0 while
                    // standing inside the exit portal, so the exit portal
                    // fires on the very next tick and bounces them home — most
                    // visibly in creative, where the transition time is 0 and
                    // there is no 4-second grace to fly out in.
                    player->portalState().CopyFrom(entity.portal);
                    MovePlayer(server, from, *toLevel, *player, landing);
                    return;
                }
            }

            // Non-player entities do not travel yet. When they do, primed TNT
            // needs `tnt->SetUsedPortal(true)` on arrival (MC PrimedTnt
            // .teleport) or the first charge through a portal blows the portal
            // out behind it. See PrimedTnt::UsedPortal.
            //
            // MC destroys and recreates
            // the entity in the destination level (Entity.java:3050); here that
            // means moving ownership between two MobManagers and re-keying the
            // entity tracker, and getting it half-right leaves a mob ticking in
            // one dimension while its client copy stands in another. The
            // cooldown is already armed by the caller, so the entity simply
            // sits in the portal rather than retrying every tick.
            Log::Debug("[PortalTravel] Entity %d reached a portal but only players travel",
                       entity.GetId());
        }

    } // namespace PortalTravel
} // namespace Server
