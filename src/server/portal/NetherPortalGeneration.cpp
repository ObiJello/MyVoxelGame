// File: src/server/portal/NetherPortalGeneration.cpp
#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "NetherPortalGeneration.hpp"

#include "ImmersivePortalRegistry.hpp"
#include "server/IntegratedServer.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/world/ticketing/ChunkTicketManager.hpp"
#include "common/world/level/World.hpp"
#include "common/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <unordered_set>

namespace Server {

    using Game::Immersive::FrameShape;
    using Game::Immersive::Portal;
    using Game::Immersive::PortalKind;

    namespace {

        // Chunks requested and held around the far position: a 5×5 block
        // (±40 blocks), which is what the search and the placement walk.
        constexpr int     kFarRadiusChunks     = 2;
        constexpr int     kTicketLifespanTicks = 20 * 60;
        // Give generation this long before building blind.
        constexpr int64_t kLoadTimeoutTicks    = 20 * 30;
        // The mod's frame-search radii: 128 toward the overworld, 16 toward
        // the nether — bounded by what is resident.
        constexpr int     kSearchRadiusOverworld = 128;
        constexpr int     kSearchRadiusNether    = 16;
        constexpr int     kSearchRadiusMax       = kFarRadiusChunks * 16 + 8;
        constexpr int     kSearchVertical        = 24;
        // Integrity sweep period (the mod: gameTime % 233 == id % 233).
        constexpr int64_t kIntegrityPeriod       = 233;

        Game::Math::ChunkPos ChunkOf(const glm::ivec3& p) {
            return Game::Math::ChunkPos{ p.x >> 4, p.z >> 4 };
        }

        // Cells of a shape relative to its min cell, sorted — the identity
        // of a shape up to translation.
        std::vector<glm::ivec3> NormalizedCells(const FrameShape& s) {
            std::vector<glm::ivec3> out;
            out.reserve(s.area.size());
            for (const auto& c : s.area) out.push_back(c - s.minCell);
            std::sort(out.begin(), out.end(), [](const glm::ivec3& a, const glm::ivec3& b) {
                return a.y != b.y ? a.y < b.y : (a.z != b.z ? a.z < b.z : a.x < b.x);
            });
            return out;
        }

        // Does `found` match `templ` exactly, or as a uniformly scaled
        // rectangle? Returns the scale (found / templ), 0 when no match.
        double MatchScale(const FrameShape& templ, const FrameShape& found) {
            if (templ.axis != found.axis) return 0.0;
            if (templ.area.size() == found.area.size() && NormalizedCells(templ) == NormalizedCells(found)) {
                return 1.0;
            }
            if (templ.IsRectangle() && found.IsRectangle()) {
                const glm::ivec2 a = templ.Size(), b = found.Size();
                if (b.x % a.x == 0 && b.y % a.y == 0 && b.x / a.x == b.y / a.y) return double(b.x) / a.x;
                if (a.x % b.x == 0 && a.y % b.y == 0 && a.x / b.x == a.y / b.y) return 1.0 / (double(a.x) / b.x);
            }
            return 0.0;
        }

    } // namespace

    NetherPortalGeneration::NetherPortalGeneration(IntegratedServer& server) : m_server(server) {}

    // ── Ignition ───────────────────────────────────────────────────────────

    bool NetherPortalGeneration::OnFrameLit(Game::DimensionId dimension, const FrameShape& shape) {
        if (!Game::DimensionAllowsNetherPortal(dimension) || shape.Empty()) return false;
        if (FrameHostsPortal(dimension, shape)) return false;

        ServerLevel* fromLevel = m_server.GetLevel(dimension);
        if (!fromLevel || !fromLevel->World()) return false;

        // The fire that lit it goes; the interior stays air.
        for (const glm::ivec3& c : shape.area) {
            if (fromLevel->World()->GetBlock(c.x, c.y, c.z) != Game::BlockID::Air) {
                fromLevel->World()->SetBlock(c.x, c.y, c.z, Game::BlockID::Air);
            }
        }

        Pending p;
        p.from      = dimension;
        p.to        = (dimension == Game::DimensionId::Overworld) ? Game::DimensionId::Nether
                                                                   : Game::DimensionId::Overworld;
        p.fromShape = shape;

        // MC/mod position mapping: the frame's centre through the 8:1 scale,
        // Y unscaled, clamped into the far dimension's build range so the
        // frame fits.
        const double scale = Game::TeleportationScale(dimension, p.to);
        const glm::dvec3 centre = shape.Center();
        const glm::ivec2 size = shape.Size();
        const int minY = Game::DimensionMinY(p.to) + 1;
        const int maxY = Game::DimensionMinY(p.to) + Game::DimensionLogicalHeight(p.to) - std::max(size.y, 2) - 2;
        p.toPos = glm::ivec3(static_cast<int>(std::floor(centre.x * scale)),
                             std::clamp(static_cast<int>(std::floor(centre.y)), minY, std::max(minY, maxY)),
                             static_cast<int>(std::floor(centre.z * scale)));

        ServerLevel* toLevel = m_server.GetOrCreateLevel(p.to);
        if (!toLevel || !toLevel->World()) {
            Log::Warning("[NetherPortal] %s is unavailable; frame at (%d,%d,%d) stays unlit",
                         std::string(Game::DimensionName(p.to)).c_str(),
                         shape.minCell.x, shape.minCell.y, shape.minCell.z);
            return false;
        }
        const Game::Math::ChunkPos centreChunk = ChunkOf(p.toPos);
        for (int dz = -kFarRadiusChunks; dz <= kFarRadiusChunks; ++dz) {
            for (int dx = -kFarRadiusChunks; dx <= kFarRadiusChunks; ++dx) {
                const Game::Math::ChunkPos c{ centreChunk.x + dx, centreChunk.z + dz };
                p.wanted.push_back(c);
                if (toLevel->Tickets()) {
                    toLevel->Tickets()->AddTemporaryTicket(c, ChunkTicketManager::ENTITY_TICKING_LEVEL,
                                                           kTicketLifespanTicks);
                }
                m_server.RequestChunkLoad(p.to, c, 1);
            }
        }
        p.startedTick = -1;
        Log::Info("[NetherPortal] Frame lit in %s (%zu cells, axis %d); searching %s around (%d,%d,%d)",
                  std::string(Game::DimensionName(dimension)).c_str(), shape.area.size(),
                  static_cast<int>(shape.axis), std::string(Game::DimensionName(p.to)).c_str(),
                  p.toPos.x, p.toPos.y, p.toPos.z);
        m_pending.push_back(std::move(p));
        return true;
    }

    bool NetherPortalGeneration::FrameHostsPortal(Game::DimensionId dimension, const FrameShape& shape) const {
        auto* registry = m_server.ImmersivePortals();
        if (!registry) return false;
        bool hosts = false;
        for (const Portal* p : registry->CollectNear(dimension, shape.Center(), 2.0)) {
            if (p->kind != PortalKind::NetherPortal) continue;
            const auto frame = Game::Immersive::FrameFromPortal(*p);
            if (!frame) continue;
            if (frame->axis == shape.axis && frame->minCell == shape.minCell &&
                frame->area.size() == shape.area.size()) { hosts = true; break; }
        }
        return hosts;
    }

    // ── Per tick ───────────────────────────────────────────────────────────

    void NetherPortalGeneration::Tick(int64_t serverTick) {
        for (size_t i = 0; i < m_pending.size();) {
            Pending& p = m_pending[i];
            if (p.startedTick < 0) p.startedTick = serverTick;
            ServerLevel* toLevel = m_server.GetLevel(p.to);
            if (!toLevel || !toLevel->World()) { m_pending.erase(m_pending.begin() + i); continue; }
            const bool ready   = ChunksResident(*toLevel, p.wanted);
            const bool timeout = serverTick - p.startedTick > kLoadTimeoutTicks;
            if (!ready && !timeout) { ++i; continue; }
            if (!ready) {
                Log::Warning("[NetherPortal] Far side did not finish loading; building with what is resident");
            }
            Complete(p, serverTick);
            m_pending.erase(m_pending.begin() + i);
        }
        CheckIntegrity(serverTick);
    }

    bool NetherPortalGeneration::ChunksResident(ServerLevel& level,
                                                const std::vector<Game::Math::ChunkPos>& chunks) const {
        Game::World* world = level.World();
        if (!world) return false;
        for (const auto& c : chunks) {
            if (!world->IsChunkLoaded(c.x, c.z)) return false;
        }
        return true;
    }

    void NetherPortalGeneration::Complete(Pending& p, int64_t /*serverTick*/) {
        ServerLevel* fromLevel = m_server.GetLevel(p.from);
        ServerLevel* toLevel   = m_server.GetLevel(p.to);
        if (!fromLevel || !fromLevel->World() || !toLevel || !toLevel->World()) return;

        // The frame may have been broken while the far side loaded.
        if (!p.fromShape.IsIntact(*fromLevel->World())) {
            Log::Info("[NetherPortal] Frame broken before the far side was ready; nothing built");
            return;
        }

        const int radius = std::min(kSearchRadiusMax, p.to == Game::DimensionId::Nether
                                                          ? kSearchRadiusNether : kSearchRadiusOverworld);
        if (auto match = FindExistingFrame(*toLevel, p.fromShape, p.toPos, radius)) {
            Log::Info("[NetherPortal] Linked to an existing frame at (%d,%d,%d) scale %.2f",
                      match->shape.minCell.x, match->shape.minCell.y, match->shape.minCell.z, match->scale);
            // The step through an existing frame is carved like a built
            // one's: a floor frame found lying on the ground (one this
            // generator built before it reserved a drop, or one laid by
            // hand) is a window onto that ground, and every relight would
            // find and link it again rather than build a new one.
            CarveClearance(*toLevel->World(), match->shape);
            CreateCluster(p.from, p.fromShape, p.to, match->shape, match->scale);
            return;
        }

        std::optional<glm::ivec3> placement = FindPlacement(*toLevel, p.fromShape, p.toPos, kSearchRadiusNether);
        if (!placement) {
            // Nowhere to stand it: float it at the mapped position, like
            // the mod's levitated placement.
            placement = p.toPos - (p.fromShape.maxCell - p.fromShape.minCell) / 2;
            Log::Info("[NetherPortal] No ground for a frame; placing it in the air at (%d,%d,%d)",
                      placement->x, placement->y, placement->z);
        }
        const FrameShape built = p.fromShape.Translated(*placement - p.fromShape.minCell);
        BuildFrame(*toLevel->World(), built);
        Log::Info("[NetherPortal] Built a frame at (%d,%d,%d) in %s",
                  built.minCell.x, built.minCell.y, built.minCell.z,
                  std::string(Game::DimensionName(p.to)).c_str());
        CreateCluster(p.from, p.fromShape, p.to, built, 1.0);
    }

    // ── Search / placement / build ─────────────────────────────────────────

    std::optional<NetherPortalGeneration::Match> NetherPortalGeneration::FindExistingFrame(
            ServerLevel& level, const FrameShape& templ, const glm::ivec3& around, int radius) const {
        Game::World* world = level.World();
        if (!world) return std::nullopt;

        std::optional<Match> best;
        double bestDist = 1e18;
        std::set<std::tuple<int, int, int, int>> seen;   // (axis, min x, y, z)

        const int y0 = std::max(Game::DimensionMinY(level.Dimension()) + 1, around.y - kSearchVertical);
        const int y1 = std::min(Game::DimensionMinY(level.Dimension()) + Game::DimensionLogicalHeight(level.Dimension()) - 2,
                                around.y + kSearchVertical);
        static const glm::ivec3 kNeighbours[6] = {
            {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
        };

        for (int x = around.x - radius; x <= around.x + radius; ++x) {
            for (int z = around.z - radius; z <= around.z + radius; ++z) {
                if (!world->IsChunkLoaded(x >> 4, z >> 4)) continue;
                for (int y = y0; y <= y1; ++y) {
                    if (!FrameShape::IsObsidian(world->GetBlock(x, y, z))) continue;
                    for (const glm::ivec3& n : kNeighbours) {
                        const glm::ivec3 start = glm::ivec3(x, y, z) + n;
                        if (!FrameShape::IsAirLike(world->GetBlock(start.x, start.y, start.z))) continue;
                        // Only the template's plane can match.
                        auto found = FrameShape::FindOnAxis(*world, start, templ.axis,
                                                            FrameShape::kDefaultLengthLimit,
                                                            FrameShape::kDefaultAreaLimit);
                        if (!found) continue;
                        const auto key = std::make_tuple(static_cast<int>(found->axis),
                                                         found->minCell.x, found->minCell.y, found->minCell.z);
                        if (!seen.insert(key).second) continue;
                        const double scale = MatchScale(templ, *found);
                        if (scale <= 0.0) continue;
                        if (FrameHostsPortal(level.Dimension(), *found)) continue;
                        const double dist = glm::length(found->Center() - glm::dvec3(around));
                        if (dist < bestDist) { bestDist = dist; best = Match{ *found, scale }; }
                    }
                }
            }
        }
        return best;
    }

    std::optional<glm::ivec3> NetherPortalGeneration::FindPlacement(
            ServerLevel& level, const FrameShape& templ, const glm::ivec3& around, int radius) const {
        Game::World* world = level.World();
        if (!world) return std::nullopt;
        const glm::ivec3 half = (templ.maxCell - templ.minCell) / 2;
        const int minY = Game::DimensionMinY(level.Dimension()) + 2;
        const int maxY = Game::DimensionMinY(level.Dimension()) + Game::DimensionLogicalHeight(level.Dimension())
                         - (templ.maxCell.y - templ.minCell.y) - 3;

        // Spiral outward in the plane; at each column try from the mapped
        // height downward, then upward — the nearest standing spot wins.
        // Preference order is MC's PortalForcer: a spot with ground under
        // it AND a block of air on both sides of the surface, then one with
        // ground, then anywhere it fits.
        for (int pass = 0; pass < 3; ++pass) {
            const bool requireGround    = (pass < 2);
            const bool requireClearance = (pass == 0);
            for (int r = 0; r <= radius; ++r) {
                for (int dx = -r; dx <= r; ++dx) {
                    for (int dz = -r; dz <= r; ++dz) {
                        if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
                        const int cx = around.x + dx, cz = around.z + dz;
                        if (!world->IsChunkLoaded(cx >> 4, cz >> 4)) continue;
                        for (int dy = 0; dy <= 32; ++dy) {
                            for (int sign : { -1, 1 }) {
                                const int y = around.y + sign * dy;
                                if (y < minY || y > maxY) continue;
                                const glm::ivec3 newMin(cx - half.x, y, cz - half.z);
                                if (templ.CanBuildAt(*world, newMin, requireGround, requireClearance)) return newMin;
                                if (dy == 0) break;
                            }
                        }
                    }
                }
            }
        }
        return std::nullopt;
    }

    void NetherPortalGeneration::BuildFrame(Game::World& world, const FrameShape& shape) const {
        for (const glm::ivec3& c : shape.FrameWithCorners()) {
            if (!FrameShape::IsObsidian(world.GetBlock(c.x, c.y, c.z))) {
                world.SetBlock(c.x, c.y, c.z, Game::BlockID::Obsidian);
            }
        }
        for (const glm::ivec3& c : shape.area) {
            if (world.GetBlock(c.x, c.y, c.z) != Game::BlockID::Air) {
                world.SetBlock(c.x, c.y, c.z, Game::BlockID::Air);
            }
        }
        // One block of air on each side of the surface, whatever spot the
        // search settled for. MC only guarantees this at its preferred
        // spots and leaves you to dig out of the others; an immersive
        // portal flush against netherrack cannot even be entered — the
        // far wall is the near side's collision — so the step through is
        // always carved. For a floor frame that is the drop beneath it
        // (FrameShape::kFallClearance): without it the far side was a
        // window onto the ground and the far floor held the player up in
        // the surface, never entering.
        CarveClearance(world, shape);
    }

    void NetherPortalGeneration::CarveClearance(Game::World& world, const FrameShape& shape) const {
        for (const glm::ivec3& c : shape.Clearance()) {
            if (!FrameShape::IsAirLike(world.GetBlock(c.x, c.y, c.z))) {
                world.SetBlock(c.x, c.y, c.z, Game::BlockID::Air);
            }
        }
    }

    void NetherPortalGeneration::CreateCluster(Game::DimensionId from, const FrameShape& fromShape,
                                               Game::DimensionId to, const FrameShape& toShape, double scale) {
        auto* registry = m_server.ImmersivePortals();
        if (!registry) return;
        Portal front;
        fromShape.FillPortal(front);
        front.kind          = PortalKind::NetherPortal;
        front.tag           = "nether";
        front.dimension     = from;
        front.destDimension = to;
        front.destination   = toShape.Center();
        front.scale         = scale > 0.0 ? scale : 1.0;
        const auto id = registry->AddBiWayBiFaced(front);
        if (id == Game::Immersive::kInvalidPortalId) {
            Log::Warning("[NetherPortal] Could not create the portal cluster");
            return;
        }
        Log::Info("[NetherPortal] Cluster #%u: %s <-> %s", id,
                  std::string(Game::DimensionName(from)).c_str(),
                  std::string(Game::DimensionName(to)).c_str());
    }

    // ── Integrity ──────────────────────────────────────────────────────────

    void NetherPortalGeneration::CheckIntegrity(int64_t serverTick) {
        auto* registry = m_server.ImmersivePortals();
        if (!registry) return;
        std::vector<Game::Immersive::PortalId> broken;
        registry->ForEach([&](const Portal& p) {
            if (p.kind != PortalKind::NetherPortal) return;
            if (((serverTick + p.id) % kIntegrityPeriod) != 0) return;
            ServerLevel* level = m_server.GetLevel(p.dimension);
            if (!level || !level->World()) return;
            const auto frame = Game::Immersive::FrameFromPortal(p);
            if (!frame) return;
            // Unloaded chunks answer "air" for everything; judge only the
            // cells that are resident, and only if at least one is — a
            // frame straddling a chunk border must not be excused by the
            // half that is not loaded.
            Game::World& world = *level->World();
            bool anyLoaded = false;
            bool intact = true;
            for (const glm::ivec3& c : frame->frame) {
                if (!world.IsPositionLoaded(c.x, c.y, c.z)) continue;
                anyLoaded = true;
                if (!Game::Immersive::FrameShape::IsObsidian(world.GetBlock(c.x, c.y, c.z))) { intact = false; break; }
            }
            if (intact) {
                for (const glm::ivec3& c : frame->area) {
                    if (!world.IsPositionLoaded(c.x, c.y, c.z)) continue;
                    anyLoaded = true;
                    if (!Game::Immersive::FrameShape::IsAirLike(world.GetBlock(c.x, c.y, c.z))) { intact = false; break; }
                }
            }
            if (anyLoaded && !intact) broken.push_back(p.id);
        });
        for (auto id : broken) {
            if (!registry->Get(id)) continue;   // removed with an earlier cluster
            Log::Info("[NetherPortal] Frame of #%u is broken; removing its cluster", id);
            registry->RemoveCluster(id);
        }
    }

    void NetherPortalGeneration::OnObsidianRemoved(Game::DimensionId dimension, const glm::ivec3& pos) {
        auto* registry = m_server.ImmersivePortals();
        if (!registry) return;
        std::vector<Game::Immersive::PortalId> broken;
        for (const Portal* p : registry->CollectNear(dimension, glm::dvec3(pos) + glm::dvec3(0.5), 2.0)) {
            if (p->kind != PortalKind::NetherPortal) continue;
            const auto frame = Game::Immersive::FrameFromPortal(*p);
            if (frame && frame->ContainsFrameCell(pos)) broken.push_back(p->id);
        }
        for (auto id : broken) {
            if (!registry->Get(id)) continue;
            Log::Info("[NetherPortal] Frame block of #%u mined at (%d,%d,%d); removing its cluster",
                      id, pos.x, pos.y, pos.z);
            registry->RemoveCluster(id);
        }
    }

} // namespace Server

#endif // ENABLE_IMMERSIVE_PORTALS
