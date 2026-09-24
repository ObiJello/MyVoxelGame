// File: src/server/portal/TwilightTeleporter.cpp
//
// Java references (mods_reference/twilightforest, git-ignored):
//   src/main/java/twilightforest/world/TFTeleporter.java
//   src/main/java/twilightforest/block/TFPortalBlock.java
//   src/main/java/twilightforest/events/ProgressionEvents.java

#include "common/world/level/ModDimensions.hpp"
#include "TwilightTeleporter.hpp"

#include "server/IntegratedServer.hpp"
#include "server/entity/ItemEntityManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/LightningSpawn.hpp"
#include "server/level/NetherPortalIndex.hpp"
#include "server/level/PortalTravel.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/LightningBolt.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/ItemEntity.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/chunk/Heightmap.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include "common/world/portal/ModPortalBehaviors.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Server {
    namespace TwilightTeleporter {

        namespace {

            using Predicate = std::function<bool(const glm::ivec3&)>;

            // MC Level.isEmptyBlock — state.isAir().
            bool IsEmpty(const Game::World& world, const glm::ivec3& p) {
                return world.GetBlock(p.x, p.y, p.z) == Game::BlockID::Air;
            }

            // BlockTags.FEATURES_CANNOT_REPLACE (data/minecraft/tags/block/
            // features_cannot_replace.json).
            bool FeaturesCannotReplace(Game::BlockID id) {
                switch (id) {
                    case Game::BlockID::Bedrock:
                    case Game::BlockID::Spawner:
                    case Game::BlockID::Chest:
                    case Game::BlockID::EndPortalFrame:
                    case Game::BlockID::ReinforcedDeepslate:
                    case Game::BlockID::TrialSpawner:
                    case Game::BlockID::Vault:
                        return true;
                    default:
                        return false;
                }
            }

            // BlockTags.SUBSTRATE_OVERWORLD — the portal edge set without
            // TF's two extra members (farmland, dirt path).
            bool IsSubstrate(Game::BlockID id) {
                return id != Game::BlockID::Farmland && id != Game::BlockID::DirtPath &&
                       Game::TwilightPortalBlocks::IsEdge(id);
            }

            // MC BlockState.canBeReplaced() — the replaceable flag (air is
            // replaceable in vanilla).
            bool CanBeReplaced(Game::BlockID id) {
                return id == Game::BlockID::Air || Game::BlockRegistry::Get(id).replaceable;
            }

            bool IsLiquid(Game::BlockID id) {
                return id == Game::BlockID::Water || id == Game::BlockID::Lava;
            }

            // MC `state.getCollisionShape(level, pos).isEmpty()`.
            // GetBlockCollisionShapeSet alone cannot answer it: it hands back
            // the OUTLINE for everything but fences, walls and gates — one box
            // for air (the default cube), a flower, a twilight_portal — and
            // leaves the "does it collide at all" half to HasCollision, as
            // every physics caller gates it.
            bool CollisionShapeIsEmpty(Game::BlockState state) {
                if (!Game::BlockRegistry::HasCollision(state.Block())) return true;
                return Game::BlockRegistry::GetBlockCollisionShapeSet(state).count == 0;
            }

            // MC Level.noCollision(entity.dimensions.makeBoundingBox(pos)) —
            // is the entity's box at bottom-centre `pos` clear of every
            // block collision shape?
            bool BoxIsFree(const Game::World& world, const glm::dvec3& pos,
                           double width, double height) {
                const double half = width * 0.5;
                const glm::dvec3 mn{pos.x - half, pos.y, pos.z - half};
                const glm::dvec3 mx{pos.x + half, pos.y + height, pos.z + half};
                constexpr double kEps = 1.0e-7;
                const int x0 = static_cast<int>(std::floor(mn.x)), x1 = static_cast<int>(std::floor(mx.x - kEps));
                const int y0 = static_cast<int>(std::floor(mn.y)), y1 = static_cast<int>(std::floor(mx.y - kEps));
                const int z0 = static_cast<int>(std::floor(mn.z)), z1 = static_cast<int>(std::floor(mx.z - kEps));
                for (int y = y0; y <= y1; ++y) {
                    for (int z = z0; z <= z1; ++z) {
                        for (int x = x0; x <= x1; ++x) {
                            const Game::BlockState state = world.GetBlockState(x, y, z);
                            // Air, plants, the portal itself: no collision
                            // shape, nothing to test.
                            if (CollisionShapeIsEmpty(state)) continue;
                            const auto shapes = Game::BlockRegistry::GetBlockCollisionShapeSet(state);
                            for (const auto& box : shapes) {
                                const glm::dvec3 bmn = glm::dvec3(x, y, z) + glm::dvec3(box.min);
                                const glm::dvec3 bmx = glm::dvec3(x, y, z) + glm::dvec3(box.max);
                                if (bmn.x < mx.x - kEps && bmx.x > mn.x + kEps &&
                                    bmn.y < mx.y - kEps && bmx.y > mn.y + kEps &&
                                    bmn.z < mx.z - kEps && bmx.z > mn.z + kEps) {
                                    return false;
                                }
                            }
                        }
                    }
                }
                return true;
            }

            // TFTeleporter.safePosInColumn: the position itself when the
            // entity fits there, else the same column at the
            // MOTION_BLOCKING_NO_LEAVES height (MC getHeight = the top
            // block + 1). The column is `(int) Math.round(pos.x)` — Java rounds
            // half UP (-63.5 -> -63), where std::lround rounds half away from
            // zero (-64), so floor(v + 0.5) it is.
            glm::dvec3 SafePosInColumn(const Game::World& world, const Game::Entity& entity,
                                       const glm::dvec3& pos) {
                if (BoxIsFree(world, pos, entity.GetBbWidth(), entity.GetBbHeight())) return pos;
                const int height = world.GetSurfaceHeight(
                    static_cast<int>(std::floor(pos.x + 0.5)), static_cast<int>(std::floor(pos.z + 0.5)),
                    Game::HeightmapType::MotionBlockingNoLeaves) + 1;
                return glm::dvec3(pos.x, static_cast<double>(height), pos.z);
            }

            // TFTeleporter.getScanHeight: the top of the chunk's highest
            // non-empty section, capped at the level's max Y - 1. A chunk
            // that is not resident answers the floor, so its columns are
            // not walked (TF's getChunk would load it; loadSurroundingArea
            // already made the ±2 chunks around the destination resident).
            int GetScanHeight(const Game::World& world, Game::DimensionId dim, int x, int z) {
                const int minY = Game::DimensionMinY(dim);
                const int maxY = minY + Game::DimensionLogicalHeight(dim) - 1;
                const auto chunk = world.GetLoadedChunk(x >> 4, z >> 4);
                if (!chunk) return minY - 1;
                const int sectionIndex = chunk->HighestFilledSectionIndex();
                const int chunkHeight = sectionIndex == Game::Chunk::kNoFilledSection
                    ? minY
                    : Game::Math::WorldCoordinates::SectionCoordsToWorldY(sectionIndex, 0) + 15;
                return std::min(maxY - 1, chunkHeight);
            }

            // TFTeleporter.getYFactor.
            double GetYFactor(Game::DimensionId dim) {
                return dim == Game::DimensionId::Overworld ? 2.0 : 0.5;
            }

            // TFTeleporter.findPortalCoords: over a 33x33 column square
            // around `loc`, every floor cell under an air pocket (or, with
            // `makePortalInAir`, the lowest air cell the predicate still
            // accepts), weighted by distance with Y scaled by getYFactor;
            // the closest one the predicate accepts.
            std::optional<glm::ivec3> FindPortalCoords(const Game::World& world, Game::DimensionId dim,
                                                       const glm::dvec3& loc, const Predicate& predicate,
                                                       bool makePortalInAir) {
                const double yFactor = GetYFactor(dim);
                const int minY = Game::DimensionMinY(dim);
                const int entityX = static_cast<int>(std::floor(loc.x));
                const int entityZ = static_cast<int>(std::floor(loc.z));

                double spotWeight = -1.0;
                std::optional<glm::ivec3> spot;

                constexpr int kRange = 16;
                for (int rx = entityX - kRange; rx <= entityX + kRange; ++rx) {
                    const double xWeight = (rx + 0.5) - loc.x;
                    for (int rz = entityZ - kRange; rz <= entityZ + kRange; ++rz) {
                        const double zWeight = (rz + 0.5) - loc.z;

                        for (int ry = GetScanHeight(world, dim, rx, rz); ry >= minY; --ry) {
                            glm::ivec3 pos{rx, ry, rz};
                            if (!makePortalInAir && !IsEmpty(world, pos)) continue;

                            if (makePortalInAir) {
                                while (ry > minY && IsEmpty(world, {rx, ry - 1, rz}) &&
                                       predicate({rx, ry - 1, rz})) {
                                    --ry;
                                }
                                pos = glm::ivec3(rx, ry, rz);
                            } else {
                                // `while (ry > minY && isEmpty(pos.set(rx, ry - 1, rz))) ry--;`
                                // — pos ends on the floor under the pocket
                                // (or on the level floor itself).
                                while (ry > minY) {
                                    pos = glm::ivec3(rx, ry - 1, rz);
                                    if (!IsEmpty(world, pos)) break;
                                    --ry;
                                }
                            }

                            const double yWeight = (ry + 0.5) - loc.y * yFactor;
                            const double rPosWeight = xWeight * xWeight + yWeight * yWeight +
                                                      zWeight * zWeight;
                            if (spotWeight < 0.0 || rPosWeight < spotWeight) {
                                // Checked from the "in ground" pos.
                                if (predicate(pos)) {
                                    spotWeight = rPosWeight;
                                    spot = pos;
                                }
                            }
                        }
                    }
                }
                return spot;
            }

            // The 4x4x6 box TFTeleporter's three placement predicates walk:
            // the pool's 2x2 and its ring at the floor (y 0), five cells of
            // headroom above.
            template <typename Test>
            bool ForPortalBox(const Game::World& world, const glm::ivec3& pos, Test&& test) {
                for (int potentialZ = 0; potentialZ < 4; ++potentialZ) {
                    for (int potentialX = 0; potentialX < 4; ++potentialX) {
                        for (int potentialY = 0; potentialY < 6; ++potentialY) {
                            const glm::ivec3 t = pos + glm::ivec3(potentialX - 1, potentialY, potentialZ - 1);
                            if (!world.IsValidPosition(t.x, t.y, t.z)) return false;
                            if (!test(world.GetBlock(t.x, t.y, t.z), potentialY)) return false;
                        }
                    }
                }
                return true;
            }

            // TFTeleporter.isIdealForPortal.
            bool IsIdealForPortal(const Game::World& world, const glm::ivec3& pos) {
                return ForPortalBox(world, pos, [](Game::BlockID id, int y) {
                    if (FeaturesCannotReplace(id)) return false;
                    if (y == 0 && !IsSubstrate(id)) return false;
                    if (y >= 1 && !CanBeReplaced(id)) return false;
                    return true;
                });
            }

            // TFTeleporter.isOkayForPortal — the floor need only be solid
            // (MC's legacy isSolid ~ has a collision shape) or liquid.
            bool IsOkayForPortal(const Game::World& world, const glm::ivec3& pos) {
                return ForPortalBox(world, pos, [](Game::BlockID id, int y) {
                    if (FeaturesCannotReplace(id)) return false;
                    if (y == 0 && !Game::BlockRegistry::HasCollision(id) && !IsLiquid(id)) return false;
                    if (y >= 1 && !CanBeReplaced(id)) return false;
                    return true;
                });
            }

            // TFTeleporter.isOkayForFallbackPortal — headroom only.
            bool IsOkayForFallbackPortal(const Game::World& world, const glm::ivec3& pos) {
                return ForPortalBox(world, pos, [](Game::BlockID id, int y) {
                    if (FeaturesCannotReplace(id)) return false;
                    if (y >= 1 && !CanBeReplaced(id)) return false;
                    return true;
                });
            }

            // TFTeleporter.makePortalAt: a ring of twelve grass blocks, dirt
            // under the 2x2, the portal, five layers of air cleared over the
            // 4x4, and a random plant on every ring block. `pos` is the
            // pool's north-west cell.
            void MakePortalAt(Game::World& world, const glm::ivec3& pos, Game::JavaRandom& random,
                              NetherPortalIndex& index) {
                using Flags = Game::World::UpdateFlags;
                // The ring, in TF's order: the north row, the two sides of
                // the pool's two rows, the south row.
                const glm::ivec3 ring[12] = {
                    {-1, 0, -1}, {0, 0, -1}, {1, 0, -1}, {2, 0, -1},
                    {-1, 0,  0}, {2, 0,  0},
                    {-1, 0,  1}, {2, 0,  1},
                    {-1, 0,  2}, {0, 0,  2}, {1, 0,  2}, {2, 0,  2},
                };
                const glm::ivec3 pool[4] = { {0, 0, 0}, {1, 0, 0}, {0, 0, 1}, {1, 0, 1} };

                // "grass all around it" — setBlockAndUpdate.
                for (const glm::ivec3& r : ring) {
                    const glm::ivec3 p = pos + r;
                    world.SetBlock(p.x, p.y, p.z, Game::BlockID::Grass, Flags::All);
                }

                // "dirt under it" — only over substrate, replaceables and air.
                for (const glm::ivec3& c : pool) {
                    const glm::ivec3 below = pos + c + glm::ivec3(0, -1, 0);
                    const Game::BlockID id = world.GetBlock(below.x, below.y, below.z);
                    if (IsSubstrate(id) || CanBeReplaced(id)) {
                        world.SetBlock(below.x, below.y, below.z, Game::BlockID::Dirt, Flags::All);
                    }
                }

                // "portal in it" — UPDATE_CLIENTS, indexed at once so the
                // return trip finds it without waiting for a chunk scan.
                const Game::BlockState portal = Game::BlockStates::Default(Game::BlockID::TwilightPortal);
                for (const glm::ivec3& c : pool) {
                    const glm::ivec3 p = pos + c;
                    world.SetBlock(p.x, p.y, p.z, portal, Flags::UpdateClients);
                    index.Add(p);
                }

                // "a bunch of air over it for 4 squares" — removeBlock.
                for (int dx = -1; dx <= 2; ++dx) {
                    for (int dz = -1; dz <= 2; ++dz) {
                        for (int dy = 1; dy <= 5; ++dy) {
                            const glm::ivec3 p = pos + glm::ivec3(dx, dy, dz);
                            if (world.GetBlock(p.x, p.y, p.z) == Game::BlockID::Air) continue;
                            world.SetBlock(p.x, p.y, p.z, Game::BlockID::Air, Flags::All);
                        }
                    }
                }

                // "nature decorations" on the ring — UPDATE_CLIENTS.
                for (const glm::ivec3& r : ring) {
                    const glm::ivec3 p = pos + r + glm::ivec3(0, 1, 0);
                    const Game::BlockID plant = Game::TwilightPortalBlocks::RandomGeneratedDecoration(random);
                    if (plant == Game::BlockID::Air) continue;
                    world.SetBlock(p.x, p.y, p.z, plant, Flags::UpdateClients);
                }

                Log::Info("[TwilightTeleporter] Built a twilight portal in '%s' at (%d, %d, %d)",
                          std::string(Game::DimensionName(world.GetDimension())).c_str(),
                          pos.x, pos.y, pos.z);
            }

            // TFTeleporter.getBoundaryPositions / checkAdjacent: every
            // horizontal non-portal neighbour of the pool with a full top
            // face and nothing collidable above it — the ring you step out
            // onto.
            //
            // The "nothing collidable above" half is CollisionShapeIsEmpty,
            // not a bare GetBlockCollisionShapeSet(...).count == 0: that set
            // is never empty (air answers the default cube, a ring flower its
            // outline), so every ring block was rejected, the border came
            // back empty, and placeInExistingPortal's fallback put the
            // traveller ON the pool cell — they sank into the portal they
            // had just come out of and were sent straight back.
            std::vector<glm::ivec3> GetBoundaryPositions(const Game::World& world,
                                                         const glm::ivec3& start) {
                struct Hash {
                    size_t operator()(const glm::ivec3& v) const noexcept {
                        return static_cast<size_t>(v.x) * 73856093u ^
                               static_cast<size_t>(v.y) * 19349663u ^
                               static_cast<size_t>(v.z) * 83492791u;
                    }
                };
                std::unordered_set<glm::ivec3, Hash> checked{start};
                std::vector<glm::ivec3> result;
                std::vector<glm::ivec3> stack{start};
                // Direction.Plane.HORIZONTAL: NORTH, EAST, SOUTH, WEST.
                constexpr glm::ivec3 kPlane[4] = { {0, 0, -1}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0} };
                // Iterative form of the recursion; a pool is at most 64
                // cells, so the set it produces is the same.
                while (!stack.empty()) {
                    const glm::ivec3 pos = stack.back();
                    stack.pop_back();
                    for (const glm::ivec3& d : kPlane) {
                        const glm::ivec3 offset = pos + d;
                        if (!checked.insert(offset).second) continue;
                        const Game::BlockState state = world.GetBlockState(offset.x, offset.y, offset.z);
                        if (state.Block() == Game::BlockID::TwilightPortal) {
                            stack.push_back(offset);
                        } else {
                            // `Block.isFaceFull(checkState.getCollisionShape(),
                            // UP) && above.getCollisionShape().isEmpty()`.
                            const Game::BlockState above =
                                world.GetBlockState(offset.x, offset.y + 1, offset.z);
                            if (Game::TwilightPortalBlocks::IsSturdyTop(state) &&
                                CollisionShapeIsEmpty(above)) {
                                result.push_back(offset);
                            }
                        }
                    }
                }
                // TF draws from a HashSet's iteration order; sorting keeps
                // the pick a function of the level random alone.
                std::sort(result.begin(), result.end(), [](const glm::ivec3& a, const glm::ivec3& b) {
                    if (a.y != b.y) return a.y < b.y;
                    if (a.z != b.z) return a.z < b.z;
                    return a.x < b.x;
                });
                return result;
            }

            // TFTeleporter.placeInExistingPortal's landing half: a random
            // ring block beside the pool, one above it, then safePosInColumn.
            glm::dvec3 LandBeside(const Game::World& world, const Game::Entity& entity,
                                  const glm::ivec3& portal, Game::JavaRandom& random) {
                const std::vector<glm::ivec3> border = GetBoundaryPositions(world, portal);
                const glm::ivec3 borderPos = border.empty()
                    ? portal
                    : border[static_cast<size_t>(random.NextInt(static_cast<int>(border.size())))];
                return SafePosInColumn(world, entity,
                                       glm::dvec3(borderPos.x + 0.5, borderPos.y + 1.0, borderPos.z + 0.5));
            }

            // Feed every resident chunk within the search radius to the
            // index. NoteChunkLoaded walks each chunk once per session, so
            // this costs a set probe per chunk after the first crossing; it
            // catches chunks made resident by paths that bypass the async
            // chunk-result scan (EnsureExitAreaLoaded's blocking loads).
            void IndexResidentChunks(const Game::World& world, NetherPortalIndex& index,
                                     const glm::ivec3& around) {
                const int radius = kPortalSearchRadius / 16 + 1;
                const int cx = around.x >> 4, cz = around.z >> 4;
                for (int dz = -radius; dz <= radius; ++dz) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        if (auto chunk = world.GetLoadedChunk(cx + dx, cz + dz)) {
                            index.NoteChunkLoaded(Game::Math::ChunkPos(cx + dx, cz + dz), *chunk);
                        }
                    }
                }
            }

            // TFTeleporter.makePortal: the placement cascade (ideal, okay,
            // fallback-in-air, then the heightmap spot) and the build. The
            // "existing portal within 16" first pass is the index search the
            // caller already made (within 200). TF's moveToSafeCoords biome
            // and landmark rerouting (progression enforcement) is not ported:
            // the start is safePosInColumn of the scaled position.
            glm::ivec3 MakePortal(ServerLevel& level, const Game::Entity& entity,
                                  const glm::ivec3& dest, NetherPortalIndex& index) {
                Game::World& world = *level.World();
                const Game::DimensionId dim = level.Dimension();
                const glm::dvec3 safePos = SafePosInColumn(
                    world, entity, glm::dvec3(dest.x + 0.5, dest.y + 0.5, dest.z + 0.5));

                std::optional<glm::ivec3> spot = FindPortalCoords(
                    world, dim, safePos,
                    [&world](const glm::ivec3& p) { return IsIdealForPortal(world, p); }, false);
                if (!spot) {
                    Log::Debug("[TwilightTeleporter] No ideal spot, trying an okay one");
                    spot = FindPortalCoords(
                        world, dim, safePos,
                        [&world](const glm::ivec3& p) { return IsOkayForPortal(world, p); }, false);
                }
                if (!spot) {
                    Log::Debug("[TwilightTeleporter] No okay spot, trying a fallback in the air");
                    spot = FindPortalCoords(
                        world, dim, safePos,
                        [&world](const glm::ivec3& p) { return IsOkayForFallbackPortal(world, p); }, true);
                }
                if (!spot) {
                    // "well I don't think we can actually just return and fail
                    // here" — the heightmap spot over the scaled position.
                    Log::Debug("[TwilightTeleporter] No fallback spot either; building at the surface");
                    spot = glm::ivec3(dest.x,
                                      world.GetSurfaceHeight(dest.x, dest.z,
                                                             Game::HeightmapType::MotionBlockingNoLeaves) + 1,
                                      dest.z);
                }

                // Keep the build (ring below, five layers above) inside the
                // level.
                const int minY = Game::DimensionMinY(dim) + 1;
                const int maxY = Game::DimensionMinY(dim) + Game::DimensionLogicalHeight(dim) - 7;
                spot->y = std::clamp(spot->y, minY, std::max(minY, maxY));

                MakePortalAt(world, *spot, level.MobLevel()->Random(), index);
                return *spot;
            }

        } // namespace

        // TFPortalBlock.getPortalDestination.
        Game::DimensionId DestinationOf(Game::DimensionId from) {
            return from == Game::DimensionId::TwilightForest ? Game::DimensionId::Overworld
                                                             : Game::DimensionId::TwilightForest;
        }

        // TFPortalBlock.getPortalDestination + TFTeleporter.createTransition /
        // placeInExistingPortal / createPosition.
        void Travel(IntegratedServer& server, ServerLevel& from, Game::Entity& entity,
                    const glm::ivec3& entryPos, Game::DimensionId toDim, bool buildPortal) {
            // /gamerule twilight_forest off: nobody goes IN (leaving still works).
            if (toDim == Game::DimensionId::TwilightForest
                && !Game::ModDimensions::Enabled(Game::DimensionId::TwilightForest)) {
                return;
            }
            // Only players travel (PortalTravel's rule for every portal);
            // stop before the search and the build, so a pig wandering into
            // a pool does not raise a portal on the far side.
            if (!entity.IsPlayer()) {
                Log::Debug("[TwilightTeleporter] Entity %d reached a portal but only players travel",
                           entity.GetId());
                return;
            }
            ServerLevel* toLevel = server.GetOrCreateLevel(toDim);
            if (!toLevel || !toLevel->World() || !toLevel->MobLevel()) {
                Log::Error("[TwilightTeleporter] '%s' could not be created; entity %d stays put",
                           std::string(Game::DimensionName(toDim)).c_str(), entity.GetId());
                return;
            }
            Game::World& world = *toLevel->World();

            // `worldborder.clampToBounds(pos.getX() * d0, pos.getY(),
            // pos.getZ() * d0)` with d0 = getTeleportationScale — X and Z
            // scaled, Y carried, clamped into the destination's range.
            const double scale = Game::TeleportationScale(from.Dimension(), toDim);
            const int minY = Game::DimensionMinY(toDim);
            const int maxY = minY + Game::DimensionLogicalHeight(toDim) - 1;
            const glm::ivec3 dest{
                static_cast<int>(std::floor(static_cast<double>(entryPos.x) * scale)),
                std::clamp(entryPos.y, minY, maxY),
                static_cast<int>(std::floor(static_cast<double>(entryPos.z) * scale)),
            };

            // TFTeleporter.loadSurroundingArea — before any read: an
            // unvisited dimension has no resident chunks and reads as air.
            PortalTravel::EnsureExitAreaLoaded(*toLevel, dest);

            NetherPortalIndex& index = server.TwilightPortalIndex(toDim);
            IndexResidentChunks(world, index, dest);

            // TFTeleporter.getPortalPosition: the nearest twilight_portal
            // within 200 (ties to the lowest cell, as TF walks down to it).
            std::optional<glm::ivec3> portal = index.FindClosest(world, dest, kPortalSearchRadius);
            if (!portal && buildPortal) {
                portal = MakePortal(*toLevel, entity, dest, index);
            }

            glm::dvec3 landing;
            if (portal) {
                landing = LandBeside(world, entity, *portal, toLevel->MobLevel()->Random());
            } else {
                // No pool and none to be built (the command): where TF's
                // search would have started — moveToSafeCoords.
                landing = SafePosInColumn(world, entity, glm::dvec3(dest.x + 0.5, dest.y, dest.z + 0.5));
            }

            Log::Info("[TwilightTeleporter] Entity %d: '%s' -> '%s' at (%.1f, %.1f, %.1f)%s",
                      entity.GetId(), std::string(Game::DimensionName(from.Dimension())).c_str(),
                      std::string(Game::DimensionName(toDim)).c_str(),
                      landing.x, landing.y, landing.z, portal ? "" : " (no portal)");

            // makeTransition: Vec3.ZERO velocity, the entity's own rotation.
            PortalTravel::ArriveAt(server, from, *toLevel, entity, landing);
        }

        namespace {

            // TFConfig.destructivePortalLightning — TFCommonConfig defines it
            // `true`, and that is the default this port keeps.
            constexpr bool kDestructivePortalLightning = true;

            // TFPortalBlock.causeLightning(level, pos, destructive):
            //
            //     bolt.setPos(pos.x + 0.5, pos.y, pos.z + 0.5);
            //     bolt.setVisualOnly(destructive);          // sic
            //     level.addFreshEntity(bolt);
            //     if (destructive) for every Entity in AABB(pos).inflate(3):
            //         victim.thunderHit(level, bolt);
            //
            // Note the inversion, kept as TF has it: a DESTRUCTIVE strike is
            // the visual-only bolt (so it lights no fire and runs no damage
            // pass of its own) plus TF's single thunderHit pass over the
            // 7×7×7 box around the pool; a non-destructive one is a full
            // vanilla bolt. With the default config the pool therefore never
            // catches fire, while everything standing at it — the thrower
            // included — takes the 5 damage once.
            void CauseLightning(Game::World& world, const glm::ivec3& pos, bool destructive) {
                ServerLevel* level = g_integratedServer
                    ? g_integratedServer->GetLevel(world.GetDimension()) : nullptr;
                if (!level) return;

                SpawnLightningBolt(*level,
                                   glm::dvec3(pos.x + 0.5, pos.y, pos.z + 0.5),
                                   /*visualOnly=*/destructive);

                if (!destructive) return;

                // new AABB(pos).inflate(3.0): the cell's unit box grown by 3.
                const glm::dvec3 lo = glm::dvec3(pos) - glm::dvec3(3.0);
                const glm::dvec3 hi = glm::dvec3(pos) + glm::dvec3(4.0);

                // getEntitiesOfClass(Entity.class, box) — NO_SPECTATORS, as
                // the bridge's query already filters. The bolt itself is not
                // among them: it joins the level at the next absorb (in TF
                // it is, and its own thunderHit does nothing).
                if (ServerLevelBridge* bridge = level->MobLevel()) {
                    std::vector<Game::Entity*> victims;
                    bridge->GetEntitiesInBox(Game::AABB::FromMinMax(glm::vec3(lo), glm::vec3(hi)),
                                             nullptr, victims);
                    for (Game::Entity* victim : victims) {
                        if (victim) Game::LightningBolt::ThunderHit(*victim);
                    }
                }

                // Item entities are Entities in MC, so the pass reaches them
                // too: ItemEntity.hurtServer takes 5 off their 5 health, which
                // destroys any stack still lying in the pool — the rest of a
                // thrown stack of diamonds included (a diamond is not
                // damage_resistant). Their fire is not modelled (item fire
                // damage does not exist here).
                if (ItemEntityManager* items = level->Items()) {
                    const double kW = Game::ItemEntity::kWidth;
                    const double kH = Game::ItemEntity::kHeight;
                    for (auto& [id, item] : items->AllMutable()) {
                        if (item.stack.IsEmpty()) continue;
                        const glm::dvec3 bmin = item.pos - glm::dvec3(kW * 0.5, 0.0, kW * 0.5);
                        const glm::dvec3 bmax = bmin + glm::dvec3(kW, kH, kW);
                        if (bmax.x <= lo.x || bmin.x >= hi.x) continue;
                        if (bmax.y <= lo.y || bmin.y >= hi.y) continue;
                        if (bmax.z <= lo.z || bmin.z >= hi.z) continue;
                        item.health -= static_cast<int>(Game::LightningBolt::kThunderDamage);
                        if (item.health <= 0) item.stack.Clear();
                        item.needsSync = true;
                    }
                }
            }

        } // namespace

        // ProgressionEvents.checkForPortalCreation + TFPortalBlock
        // .tryToCreatePortal.
        bool TryCreatePortalFromCatalyst(Game::World& world, PlayerSessionManager* sessions,
                                         Game::ItemEntity& catalyst, bool thrownByPlayer) {
            if (!thrownByPlayer || !sessions) return false;
            // /gamerule twilight_forest off: the pool will not light.
            if (!Game::ModDimensions::Enabled(Game::DimensionId::TwilightForest)) return false;
            // #twilightforest:portal/activator = #c:gems/diamond.
            if (catalyst.stack.IsEmpty() || catalyst.stack.itemId != Game::Items::Diamond) return false;

            // The origin dimension, the Twilight Forest, and nowhere else
            // (TFConfig.allowPortalsInOtherDimensions defaults off).
            const Game::DimensionId dim = world.GetDimension();
            if (dim != Game::DimensionId::Overworld && dim != Game::DimensionId::TwilightForest) {
                return false;
            }

            // canFormPortal(level.getBlockState(entityItem.blockPosition()))
            // — cheap, so first.
            const glm::ivec3 blockPos{
                static_cast<int>(std::floor(catalyst.pos.x)),
                static_cast<int>(std::floor(catalyst.pos.y)),
                static_cast<int>(std::floor(catalyst.pos.z)),
            };
            if (!Game::TwilightPortalBlocks::IsPoolBlock(
                    world.GetBlockState(blockPos.x, blockPos.y, blockPos.z))) {
                return false;
            }

            // `level.getEntitiesOfClass(ItemEntity.class, player.getBoundingBox()
            // .inflate(32))` with `getOwner() == player`: the thrower is a
            // player in this dimension whose box, grown by 32, holds the item.
            bool throwerNear = false;
            const double halfW = Game::PlayerPhysics::WIDTH * 0.5 + kCatalystRange;
            for (const auto& session : sessions->GetAllSessions()) {
                if (!session) continue;
                ServerPlayer* player = session->GetPlayer();
                if (!player || player->isDead()) continue;
                if (Game::DimensionFromRaw(player->getDimensionId()) != dim) continue;
                const glm::dvec3 p = player->getPosition();
                if (std::abs(catalyst.pos.x - p.x) > halfW) continue;
                if (std::abs(catalyst.pos.z - p.z) > halfW) continue;
                if (catalyst.pos.y < p.y - kCatalystRange) continue;
                if (catalyst.pos.y > p.y + Game::PlayerPhysics::HEIGHT_STANDING + kCatalystRange) continue;
                throwerNear = true;
                break;
            }
            if (!throwerNear) return false;

            const auto shape = Game::TwilightPortalShape::Find(world, blockPos);
            if (!shape) return false;

            // catalyst.getItem().shrink(1).
            catalyst.stack.count -= 1;
            if (catalyst.stack.count <= 0) catalyst.stack.Clear();
            catalyst.needsSync = true;

            // causeLightning(level, pos, TFConfig.destructivePortalLightning)
            // — the bolt's thunder and impact play from its own client tick.
            CauseLightning(world, blockPos, kDestructivePortalLightning);

            shape->CreatePortalBlocks(world);

            // Index the new pool now: its chunk was scanned before it existed.
            if (g_integratedServer) {
                NetherPortalIndex& index = g_integratedServer->TwilightPortalIndex(dim);
                for (const glm::ivec3& c : shape->Cells()) index.Add(c);
            }

            Log::Info("[TwilightTeleporter] A diamond lit a %zu-cell twilight portal at (%d, %d, %d) in '%s'",
                      shape->Cells().size(), blockPos.x, blockPos.y, blockPos.z,
                      std::string(Game::DimensionName(dim)).c_str());
            return true;
        }

    } // namespace TwilightTeleporter
} // namespace Server
