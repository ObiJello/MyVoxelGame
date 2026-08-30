// File: src/server/level/PortalForcer.cpp
//
// Line references are to minecraft_code/decompiled_net/minecraft/world/level/
// portal/PortalForcer.java.

#include "PortalForcer.hpp"

#include "NetherPortalIndex.hpp"
#include "ServerLevel.hpp"

#include "common/core/Log.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/chunk/Heightmap.hpp"
#include "common/world/level/World.hpp"

#include <algorithm>
#include <functional>

namespace Server {
    namespace PortalForcer {

        namespace {

            // MC's frame footprint, spelled out as vanilla names them
            // (PortalForcer.java:27-35) so the loop bounds below can be read
            // straight against the Java.
            constexpr int kFrameWidthStart  = -1;
            constexpr int kFrameWidthEnd    =  3;
            constexpr int kFrameHeightStart = -1;
            constexpr int kFrameHeightEnd   =  4;
            constexpr int kFrameBoxStart    = -1;
            constexpr int kFrameBoxEnd      =  2;
            constexpr int kSpiralRadius     = 16;

            inline glm::ivec3 Offset(const glm::ivec3& p, Game::Direction d, int n) {
                return { p.x + Game::StepX(d) * n,
                         p.y + Game::StepY(d) * n,
                         p.z + Game::StepZ(d) * n };
            }

            // MC Direction.get(AxisDirection.POSITIVE, axis).
            constexpr Game::Direction PositiveOn(Game::Axis a) {
                return a == Game::Axis::X ? Game::Direction::East : Game::Direction::South;
            }

            // PortalForcer.java:146 canPortalReplaceBlock — replaceable AND no
            // fluid. The fluid clause matters: lava is replaceable in this
            // engine's table, and without the second test every nether portal
            // would be built inside the lava sea.
            bool CanPortalReplaceBlock(const Game::World& world, const glm::ivec3& p) {
                if (!world.IsValidPosition(p.x, p.y, p.z)) return false;
                const Game::BlockID id = world.GetBlock(p.x, p.y, p.z);
                if (!Game::BlockRegistry::Get(id).replaceable) return false;
                return !world.IsBlockFluid(p.x, p.y, p.z);
            }

            // PortalForcer.java:151 canHostFrame. `offset` steps sideways out
            // of the portal plane: 0 is the plane itself, ±1 are the slices
            // beside it, and a candidate that can host all three is what MC
            // calls a "full" placement — a portal with a block of breathing
            // room on each side rather than one flush against a wall.
            bool CanHostFrame(const Game::World& world, const glm::ivec3& origin,
                              Game::Direction direction, int offset) {
                const Game::Direction clockWise = Game::ClockWise(direction);
                for (int width = kFrameWidthStart; width < kFrameWidthEnd; ++width) {
                    for (int height = kFrameHeightStart; height < kFrameHeightEnd; ++height) {
                        const glm::ivec3 p{
                            origin.x + Game::StepX(direction) * width
                                     + Game::StepX(clockWise) * offset,
                            origin.y + height,
                            origin.z + Game::StepZ(direction) * width
                                     + Game::StepZ(clockWise) * offset,
                        };
                        if (height < 0) {
                            // The floor the frame stands on.
                            if (!world.IsValidPosition(p.x, p.y, p.z)) return false;
                            if (!Game::BlockRegistry::HasCollision(
                                    world.GetBlock(p.x, p.y, p.z))) {
                                return false;
                            }
                        } else if (!CanPortalReplaceBlock(world, p)) {
                            return false;
                        }
                    }
                }
                return true;
            }

            // MC BlockPos.spiralAround(center, radius, EAST, SOUTH)
            // (BlockPos.java:386). A square spiral walking outward one ring at
            // a time, which is what makes the search find the CLOSEST usable
            // column rather than the first one in some scan order.
            //
            // Reproduced exactly, including the initial step along the second
            // direction and the `leg / 2 + 1` leg-length growth — a hand-rolled
            // "rings of increasing radius" loop visits the same cells in a
            // different order and picks a different column.
            void SpiralAround(const glm::ivec3& center, int radius,
                              Game::Direction firstDirection,
                              Game::Direction secondDirection,
                              const std::function<void(const glm::ivec3&)>& visit) {
                const Game::Direction directions[4] = {
                    firstDirection, secondDirection,
                    Game::Opposite(firstDirection), Game::Opposite(secondDirection),
                };
                glm::ivec3 cursor = Offset(center, secondDirection, 1);
                const int legs = 4 * radius;
                int leg      = -1;
                int legSize  = 0;
                int legIndex = 0;
                glm::ivec3 last = cursor;

                for (;;) {
                    cursor = Offset(last, directions[((leg % 4) + 4) % 4], 1);
                    last = cursor;
                    if (legIndex >= legSize) {
                        if (leg >= legs) return;
                        ++leg;
                        legIndex = 0;
                        legSize = leg / 2 + 1;
                    }
                    ++legIndex;
                    visit(cursor);
                }
            }

        } // namespace

        // PortalForcer.java:43
        std::optional<glm::ivec3> FindClosestPortalPosition(
            ServerLevel& level, const glm::ivec3& approximateExitPos, bool toNether)
        {
            const int radius = toNether ? kNetherSearchRadius : kOverworldSearchRadius;
            return level.Portals().FindClosest(*level.World(), approximateExitPos, radius);
        }

        // PortalForcer.java:52
        std::optional<Game::FoundRectangle> CreatePortal(
            ServerLevel& level, const glm::ivec3& origin, Game::Axis portalAxis)
        {
            Game::World& world = *level.World();
            const Game::Direction direction = PositiveOn(portalAxis);

            // MC: min(level.getMaxY(), level.getMinY() + logicalHeight - 1).
            // For the Nether that is 127 — the bedrock roof rule, and the
            // whole reason you cannot build a portal on top of the Nether.
            const int levelMinY = Game::DimensionMinY(level.Dimension());
            const int maxPlaceableY = std::min(
                Game::Math::WorldCoordinates::MAX_WORLD_Y,
                levelMinY + Game::DimensionLogicalHeight(level.Dimension()) - 1);

            double     closestFullDistSqr    = -1.0;
            glm::ivec3 closestFullPosition{0, 0, 0};
            double     closestPartialDistSqr = -1.0;
            glm::ivec3 closestPartialPosition{0, 0, 0};

            SpiralAround(origin, kSpiralRadius, Game::Direction::East,
                         Game::Direction::South,
                         [&](const glm::ivec3& spiralPos) {
                // MC moves one step along `direction` for the border test and
                // then straight back. With no world border both tests pass, so
                // the move is a no-op here — but the column the loop actually
                // examines is the UNMOVED one, which is what the `.move(
                // direction.getOpposite(), 1)` restores.
                const int surface = world.GetSurfaceHeight(
                    spiralPos.x, spiralPos.z, Game::HeightmapType::MotionBlockingNoLeaves);
                int y = std::min(maxPlaceableY, surface);

                for (; y >= levelMinY; --y) {
                    glm::ivec3 columnPos{ spiralPos.x, y, spiralPos.z };
                    if (!CanPortalReplaceBlock(world, columnPos)) continue;

                    // Walk down through the whole air pocket. `firstEmptyY` is
                    // its ceiling and `y` ends at its floor.
                    const int firstEmptyY = y;
                    while (y > levelMinY &&
                           CanPortalReplaceBlock(world, { spiralPos.x, y - 1, spiralPos.z })) {
                        --y;
                    }

                    if (y + 4 > maxPlaceableY) continue;

                    // MC: `deltaY <= 0 || deltaY >= 3`. A pocket one or two
                    // blocks tall is a crawlspace — too short to stand a
                    // portal in and too tall to treat as solid ground — so it
                    // is skipped rather than half-filled.
                    const int deltaY = firstEmptyY - y;
                    if (deltaY > 0 && deltaY < 3) continue;

                    columnPos.y = y;
                    if (!CanHostFrame(world, columnPos, direction, 0)) continue;

                    const double dx = static_cast<double>(columnPos.x - origin.x);
                    const double dy = static_cast<double>(columnPos.y - origin.y);
                    const double dz = static_cast<double>(columnPos.z - origin.z);
                    const double distance = dx * dx + dy * dy + dz * dz;

                    if (CanHostFrame(world, columnPos, direction, -1) &&
                        CanHostFrame(world, columnPos, direction, 1) &&
                        (closestFullDistSqr == -1.0 || closestFullDistSqr > distance)) {
                        closestFullDistSqr = distance;
                        closestFullPosition = columnPos;
                    }

                    // A partial candidate is only remembered while no full one
                    // has been found — MC's own short-circuit, and the reason
                    // a cramped spot never beats a roomy one that is further
                    // away.
                    if (closestFullDistSqr == -1.0 &&
                        (closestPartialDistSqr == -1.0 || closestPartialDistSqr > distance)) {
                        closestPartialDistSqr = distance;
                        closestPartialPosition = columnPos;
                    }
                }
            });

            if (closestFullDistSqr == -1.0 && closestPartialDistSqr != -1.0) {
                closestFullPosition = closestPartialPosition;
                closestFullDistSqr  = closestPartialDistSqr;
            }

            // PortalForcer.java:103 — nothing in the spiral could host a frame,
            // so carve a platform. This is the branch that fires over an ocean
            // or in open Nether air, and MC's y>=70 floor is what stops it
            // being carved at bedrock level.
            if (closestFullDistSqr == -1.0) {
                const int minStartY = std::max(levelMinY + 1, 70);
                const int maxStartY = maxPlaceableY - 9;
                if (maxStartY < minStartY) {
                    Log::Error("[PortalForcer] No legal Y band for a portal in '%s' "
                               "(min %d > max %d)",
                               std::string(Game::DimensionName(level.Dimension())).c_str(),
                               minStartY, maxStartY);
                    return std::nullopt;
                }

                closestFullPosition = glm::ivec3(
                    origin.x - Game::StepX(direction),
                    std::clamp(origin.y, minStartY, maxStartY),
                    origin.z - Game::StepZ(direction));

                const Game::Direction clockWise = Game::ClockWise(direction);
                for (int box = kFrameBoxStart; box < kFrameBoxEnd; ++box) {
                    for (int width = 0; width < 2; ++width) {
                        for (int height = kFrameHeightStart; height < 3; ++height) {
                            const Game::BlockID fill = (height < 0) ? Game::BlockID::Obsidian
                                                                    : Game::BlockID::Air;
                            const glm::ivec3 p{
                                closestFullPosition.x + width * Game::StepX(direction)
                                                      + box * Game::StepX(clockWise),
                                closestFullPosition.y + height,
                                closestFullPosition.z + width * Game::StepZ(direction)
                                                      + box * Game::StepZ(clockWise),
                            };
                            world.SetBlock(p.x, p.y, p.z, fill,
                                           Game::World::UpdateFlags::All);
                        }
                    }
                }
            }

            // The frame. Only the RING — MC's `width == -1 || width == 2 ||
            // height == -1 || height == 3` — one block thick, in the portal
            // plane. Writing the interior too would fill the portal with
            // obsidian.
            for (int width = kFrameWidthStart; width < kFrameWidthEnd; ++width) {
                for (int height = kFrameHeightStart; height < kFrameHeightEnd; ++height) {
                    if (width != kFrameWidthStart && width != 2 &&
                        height != kFrameHeightStart && height != 3) {
                        continue;
                    }
                    const glm::ivec3 p{
                        closestFullPosition.x + width * Game::StepX(direction),
                        closestFullPosition.y + height,
                        closestFullPosition.z + width * Game::StepZ(direction),
                    };
                    world.SetBlock(p.x, p.y, p.z, Game::BlockID::Obsidian,
                                   Game::World::UpdateFlags::All);
                }
            }

            // The opening. MC passes flag 18 — clients told, neighbours NOT
            // re-shaped — for the same reason PortalShape::CreatePortalBlocks
            // does: each portal block written would otherwise look at a
            // half-built portal and delete itself.
            const Game::BlockState portalState =
                Game::BlockStates::Default(Game::BlockID::NetherPortal)
                    .SetName(Game::PropertyId::HORIZONTAL_AXIS, Game::NameOf(portalAxis));

            for (int width = 0; width < 2; ++width) {
                for (int height = 0; height < 3; ++height) {
                    const glm::ivec3 p{
                        closestFullPosition.x + width * Game::StepX(direction),
                        closestFullPosition.y + height,
                        closestFullPosition.z + width * Game::StepZ(direction),
                    };
                    world.SetBlock(p.x, p.y, p.z, portalState,
                                   Game::World::UpdateFlags::MarkDirty);
                    // Index it immediately rather than waiting for a chunk
                    // scan: the return trip will look for this portal, and the
                    // chunk it sits in may never be re-scanned.
                    level.Portals().Add(p);
                }
            }

            Log::Info("[PortalForcer] Built a portal in '%s' at (%d, %d, %d) axis %s",
                      std::string(Game::DimensionName(level.Dimension())).c_str(),
                      closestFullPosition.x, closestFullPosition.y, closestFullPosition.z,
                      std::string(Game::NameOf(portalAxis)).c_str());

            return Game::FoundRectangle{ closestFullPosition, 2, 3 };
        }

    } // namespace PortalForcer
} // namespace Server
