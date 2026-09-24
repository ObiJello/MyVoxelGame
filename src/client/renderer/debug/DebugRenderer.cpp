// File: src/client/renderer/debug/DebugRenderer.cpp
#include "DebugRenderer.hpp"
#include "Gizmos.hpp"
#include "../gui/debug/DebugScreenEntries.hpp"
#include "../core/Camera.hpp"
#include "../core/Frustum.hpp"
#include "../mesh/ChunkRenderer.hpp"
#include "../mesh/SectionMesh.hpp"
#include "../culling/SectionOcclusionGraph.hpp"
#include "../culling/VisibilitySet.hpp"
#include "client/entity/Player.hpp"
#include "client/entity/ClientMobManager.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include "client/entity/ItemEntityManager.hpp"
#include "client/entity/XpOrbManager.hpp"
#include "client/entity/ClientFallingBlocks.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "client/world/ClientChunkManager.hpp"
#include "client/world/ClientLevel.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/Mob.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/chunk/Heightmap.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include "server/IntegratedServer.hpp"
#include "server/world/status/ChunkStatusManager.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

namespace Render::DebugRenderer {

    namespace {
        namespace Ids = DebugScreen::Ids;
        namespace DS = DebugScreen;

        constexpr int kWorldMinY = -64;
        constexpr int kWorldMaxY = 319;

        uint32_t ColorFromFloat(float a, float r, float g, float b) {
            auto c = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
            return (c(a) << 24) | (c(r) << 16) | (c(g) << 8) | c(b);
        }
        uint32_t Opaque(uint32_t argb) { return argb | 0xFF000000u; }
        uint32_t WithAlpha(float a, uint32_t rgb) {
            return (static_cast<uint32_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f)) << 24) | (rgb & 0xFFFFFFu);
        }
        // MC Mth.hsvToRgb.
        uint32_t HsvToRgb(float h, float s, float v) {
            const int i = static_cast<int>(h * 6.0f) % 6;
            const float f = h * 6.0f - static_cast<float>(static_cast<int>(h * 6.0f));
            const float p = v * (1.0f - s), q = v * (1.0f - f * s), t = v * (1.0f - (1.0f - f) * s);
            float r, g, b;
            switch (i) {
                case 0: r = v; g = t; b = p; break;
                case 1: r = q; g = v; b = p; break;
                case 2: r = p; g = v; b = t; break;
                case 3: r = p; g = q; b = v; break;
                case 4: r = t; g = p; b = v; break;
                default: r = v; g = p; b = q; break;
            }
            return (static_cast<uint32_t>(r * 255) << 16) | (static_cast<uint32_t>(g * 255) << 8) | static_cast<uint32_t>(b * 255);
        }
        // MC ARGB.srgbLerp.
        uint32_t Lerp(float t, uint32_t a, uint32_t b) {
            auto ch = [&](int shift) {
                const float ca = static_cast<float>((a >> shift) & 0xFF), cb = static_cast<float>((b >> shift) & 0xFF);
                return static_cast<uint32_t>(std::lround(ca + (cb - ca) * t)) & 0xFF;
            };
            return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
        }

        glm::ivec3 BlockOf(const glm::vec3& p) {
            return { static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), static_cast<int>(std::floor(p.z)) };
        }
        double Now() {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // ── Captured frustum ────────────────────────────────────────────
        bool g_hasFrustum = false;
        // WORLD-space corners of the captured frustum (double: they are
        // re-submitted to Gizmos as world points every frame).
        glm::dvec3 g_frustumPoints[8];

        // The six face-index → step vectors of VisibilitySet's ordering.
        const glm::ivec3 kDirStep[6] = { {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1}, {-1,0,0}, {1,0,0} };

        // ── ChunkBorderRenderer ─────────────────────────────────────────
        void RenderChunkBorders(const FrameArgs& a) {
            const uint32_t CELL_BORDER = 0xFF009B9B;
            const uint32_t YELLOW = 0xFFFFFF00;
            const uint32_t MAJOR = ColorFromFloat(1.0f, 0.25f, 0.25f, 1.0f);
            const glm::ivec3 feet = BlockOf(a.player->physics.position);
            const auto cpos = Game::Math::WorldCoordinates::WorldToChunkPos(feet.x, feet.z);
            const double xstart = cpos.x * 16.0, zstart = cpos.z * 16.0;
            const double ymin = kWorldMinY, ymax = kWorldMaxY + 1;
            const uint32_t red50 = ColorFromFloat(0.5f, 1.0f, 0.0f, 0.0f);
            for (int x = -16; x <= 32; x += 16)
                for (int z = -16; z <= 32; z += 16)
                    Gizmos::Line({xstart + x, ymin, zstart + z}, {xstart + x, ymax, zstart + z}, red50, 4.0f);
            for (int x = 2; x < 16; x += 2) {
                const uint32_t c = x % 4 == 0 ? CELL_BORDER : YELLOW;
                Gizmos::Line({xstart + x, ymin, zstart}, {xstart + x, ymax, zstart}, c, 1.0f);
                Gizmos::Line({xstart + x, ymin, zstart + 16.0}, {xstart + x, ymax, zstart + 16.0}, c, 1.0f);
            }
            for (int z = 2; z < 16; z += 2) {
                const uint32_t c = z % 4 == 0 ? CELL_BORDER : YELLOW;
                Gizmos::Line({xstart, ymin, zstart + z}, {xstart, ymax, zstart + z}, c, 1.0f);
                Gizmos::Line({xstart + 16.0, ymin, zstart + z}, {xstart + 16.0, ymax, zstart + z}, c, 1.0f);
            }
            for (int y = kWorldMinY; y <= kWorldMaxY + 1; y += 2) {
                const uint32_t c = y % 8 == 0 ? CELL_BORDER : YELLOW;
                const double yl = y;
                Gizmos::Line({xstart, yl, zstart}, {xstart, yl, zstart + 16.0}, c, 1.0f);
                Gizmos::Line({xstart, yl, zstart + 16.0}, {xstart + 16.0, yl, zstart + 16.0}, c, 1.0f);
                Gizmos::Line({xstart + 16.0, yl, zstart + 16.0}, {xstart + 16.0, yl, zstart}, c, 1.0f);
                Gizmos::Line({xstart + 16.0, yl, zstart}, {xstart, yl, zstart}, c, 1.0f);
            }
            for (int x = 0; x <= 16; x += 16)
                for (int z = 0; z <= 16; z += 16)
                    Gizmos::Line({xstart + x, ymin, zstart + z}, {xstart + x, ymax, zstart + z}, MAJOR, 4.0f);
            const int sy = static_cast<int>(std::floor(feet.y / 16.0f));
            Gizmos::Cuboid({xstart, sy * 16.0, zstart}, {xstart + 16.0, sy * 16.0 + 16.0, zstart + 16.0}, MAJOR, 1.0f, true);
            for (int y = kWorldMinY; y <= kWorldMaxY + 1; y += 16) {
                const double yl = y;
                Gizmos::Line({xstart, yl, zstart}, {xstart, yl, zstart + 16.0}, MAJOR, 4.0f);
                Gizmos::Line({xstart, yl, zstart + 16.0}, {xstart + 16.0, yl, zstart + 16.0}, MAJOR, 4.0f);
                Gizmos::Line({xstart + 16.0, yl, zstart + 16.0}, {xstart + 16.0, yl, zstart}, MAJOR, 4.0f);
                Gizmos::Line({xstart + 16.0, yl, zstart}, {xstart, yl, zstart}, MAJOR, 4.0f);
            }
        }

        // ── EntityHitboxDebugRenderer ───────────────────────────────────
        // MC showHitboxes: the box, a point at the feet, the eye-height slab
        // (living entities), the view-vector arrow.
        void ShowHitbox(const glm::dvec3& pos, float width, float height, float eyeHeight,
                        const glm::vec3* viewVector, bool living) {
            const double hw = width * 0.5;
            const glm::dvec3 lo(pos.x - hw, pos.y, pos.z - hw), hi(pos.x + hw, pos.y + height, pos.z + hw);
            Gizmos::Cuboid(lo, hi, 0xFFFFFFFF);
            Gizmos::Point(pos, 0xFFFFFFFF, 2.0f);
            if (living) {
                Gizmos::Cuboid({lo.x, pos.y + eyeHeight - 0.01, lo.z}, {hi.x, pos.y + eyeHeight + 0.01, hi.z}, 0xFFFF0000);
            }
            if (viewVector) {
                const glm::dvec3 eye = pos + glm::dvec3(0.0, eyeHeight, 0.0);
                Gizmos::Arrow(eye, eye + glm::dvec3(*viewVector) * 2.0, 0xFF0000FF);
            }
        }

        void RenderHitboxes(const FrameArgs& a) {
            // The local player, when the camera is not inside them.
            if (!a.firstPerson && a.player) {
                const auto& ph = a.player->physics;
                const glm::vec3 view = a.camera->GetForward();
                ShowHitbox(glm::dvec3(ph.position), ph.GetWidth(), ph.GetCurrentHeight(), ph.GetEyeHeight(), &view, true);
            }
            if (Client::g_remotePlayerManager) {
                for (const auto& [id, rp] : Client::g_remotePlayerManager->GetPlayers()) {
                    if (!Client::IsRemotePlayerInBoundLevel(rp) || !rp.positionInitialized) continue;
                    const float h = (rp.isCrouching ? 1.5f : 1.8f) * rp.scale;
                    const float eye = (rp.isCrouching ? 1.27f : 1.62f) * rp.scale;
                    const glm::vec3 view = Game::Mth::ViewVector(rp.rotation.y, rp.rotation.x);
                    const glm::dvec3 pos = glm::mix(rp.renderPrevPosition, rp.position, static_cast<double>(a.partialTick));
                    ShowHitbox(pos, 0.6f * rp.scale, h, eye, &view, true);
                }
            }
            if (!Client::ClientLevels::HasSession()) return;
            const Client::ClientLevel& level = Client::ClientLevels::Active();
            if (Client::ClientMobManager* mobs = level.Mobs()) {
                for (const Client::ClientMob* cm : mobs->MobList()) {
                    if (!cm || !cm->mob) continue;
                    const Game::Mob& mob = *cm->mob;
                    const glm::vec3 view = Game::Mth::ViewVector(mob.xRot, mob.yRot);
                    // MC: entity.getPosition(partialTicks), the box moved by
                    // the same offset — a hitbox that slides with the model
                    // instead of stepping at 20 Hz.
                    const glm::dvec3 pos = glm::mix(cm->renderPrevPosition, mob.position, static_cast<double>(a.partialTick));
                    const glm::dvec3 offset = pos - mob.position;
                    ShowHitbox(pos, mob.GetBbWidth(), mob.GetBbHeight(), mob.GetEyeHeight(), &view, true);
                    if (const Game::Entity* vehicle = mob.GetVehicle()) {
                        const float w = std::min(vehicle->GetBbWidth(), mob.GetBbWidth()) * 0.5f;
                        const glm::dvec3 seat = vehicle->GetPassengerRidingPosition(mob) + offset;
                        Gizmos::Cuboid({seat.x - w, seat.y, seat.z - w}, {seat.x + w, seat.y + 0.0625, seat.z + w}, 0xFFFFFF00);
                    }
                }
            }
            if (Client::ItemEntityManager* items = level.Items()) {
                for (const auto& [id, it] : items->GetEntities()) {
                    ShowHitbox(glm::mix(it.renderPrevPosition, it.sim.pos, static_cast<double>(a.partialTick)),
                               Game::ItemEntity::kWidth, Game::ItemEntity::kHeight, 0.0f, nullptr, false);
                }
            }
            if (Client::XpOrbManager* orbs = level.Orbs()) {
                for (const auto& [id, orb] : orbs->GetEntities()) {
                    ShowHitbox(glm::mix(orb.renderPrevPosition, orb.sim.pos, static_cast<double>(a.partialTick)), 0.5f, 0.5f, 0.0f, nullptr, false);
                }
            }
            if (Client::ClientFallingBlocks* falling = level.FallingBlocks()) {
                for (const Client::BlockEntityProxy& p : falling->Proxies()) {
                    if (!p.drawable) continue;
                    ShowHitbox(glm::mix(p.prevPos, p.pos, static_cast<double>(a.partialTick)), p.half.x * 2.0f, p.half.y * 2.0f, 0.0f, nullptr, false);
                }
            }
        }

        // ── DebugCrosshairRenderer ──────────────────────────────────────
        // MC draws the three world axes 1 unit in front of the camera at
        // 0.01 * guiScale per unit: black 4 px underlays, then red X, green Y
        // and blue Z at 2 px.
        void RenderCrosshair3D(const FrameArgs& a) {
            const glm::dvec3 origin = glm::dvec3(a.cameraPos) + glm::dvec3(a.camera->GetForward());
            const double s = 0.01 * a.guiScale;
            const glm::dvec3 ax[3] = { {s, 0, 0}, {0, s, 0}, {0, 0, s} };
            // MC DebugCrosshairRenderer: underlays through LINES, the axes
            // through LINES_DEPTH_BIAS so they win at the same depth.
            for (const glm::dvec3& d : ax) Gizmos::Line(origin, origin + d, 0xFF000000, 4.0f);
            Gizmos::Line(origin, origin + ax[0], 0xFFFF0000, 2.0f, false, true);
            Gizmos::Line(origin, origin + ax[1], 0xFF00FF00, 2.0f, false, true);
            Gizmos::Line(origin, origin + ax[2], 0xFF7F7FFF, 2.0f, false, true);
        }

        // ── WaterDebugRenderer ──────────────────────────────────────────
        int WaterAmount(const glm::ivec3& p) {
            const Game::BlockState st = Client::g_clientBlockAccess->GetBlockState(p.x, p.y, p.z);
            if (st.Block() != Game::BlockID::Water) return 8;   // waterlogged = source
            const std::string_view lv = st.GetValueByName("level");
            const int level = lv.empty() ? 0 : std::atoi(std::string(lv).c_str());
            if (level == 0 || level >= 8) return 8;
            return 8 - level;
        }

        void RenderWaterLevels(const FrameArgs& a) {
            if (!Client::g_clientBlockAccess) return;
            const glm::ivec3 c = BlockOf(a.player->physics.position);
            const uint32_t fill = ColorFromFloat(0.15f, 0.0f, 1.0f, 0.0f);
            for (int x = c.x - 10; x <= c.x + 10; ++x)
                for (int y = c.y - 10; y <= c.y + 10; ++y)
                    for (int z = c.z - 10; z <= c.z + 10; ++z) {
                        if (!Client::g_clientBlockAccess->ContainsWater(x, y, z)) continue;
                        const int amount = WaterAmount({x, y, z});
                        // MC FlowingFluid.getHeight: amount / 9, a source under
                        // another water block fills the cell.
                        const bool waterAbove = Client::g_clientBlockAccess->ContainsWater(x, y + 1, z);
                        const float h = waterAbove ? 1.0f : static_cast<float>(amount) / 9.0f;
                        Gizmos::CuboidFill({x + 0.01, y + 0.01, z + 0.01}, {x + 0.99, y + h, z + 0.99}, fill);
                        Gizmos::BillboardText(std::to_string(amount), {x + 0.5, y + h, z + 0.5}, 0xFF000000, 1.0f, true, false);
                    }
        }

        // ── HeightMapRenderer ───────────────────────────────────────────
        struct HeightCache {
            double time = -1.0;
            glm::ivec2 centerChunk{INT32_MIN, INT32_MIN};
            struct Box { glm::dvec3 lo, hi; uint32_t color; };
            std::vector<Box> boxes;
        } g_heights;

        void RenderHeightmap(const FrameArgs& a) {
            if (!Client::ClientLevels::HasSession()) return;
            Client::ClientChunkManager* chunks = Client::ClientLevels::Active().Chunks();
            if (!chunks) return;
            const glm::ivec3 c = BlockOf(a.cameraPos);
            const auto cpos = Game::Math::WorldCoordinates::WorldToChunkPos(c.x, c.z);
            const double now = Now();
            if (now - g_heights.time > 1.0 || g_heights.centerChunk != glm::ivec2(cpos.x, cpos.z)) {
                g_heights.time = now;
                g_heights.centerChunk = {cpos.x, cpos.z};
                g_heights.boxes.clear();
                // MC's colour per type, in this engine's enum order.
                auto colorOf = [](Game::HeightmapType t) {
                    switch (t) {
                        case Game::HeightmapType::WorldSurface:           return ColorFromFloat(1, 0.0f, 0.7f, 0.0f);
                        case Game::HeightmapType::OceanFloor:             return ColorFromFloat(1, 0.0f, 0.0f, 0.5f);
                        case Game::HeightmapType::MotionBlocking:         return ColorFromFloat(1, 0.0f, 0.3f, 0.3f);
                        default:                                          return ColorFromFloat(1, 0.0f, 0.5f, 0.5f);
                    }
                };
                for (int cx = -2; cx <= 2; ++cx)
                    for (int cz = -2; cz <= 2; ++cz) {
                        const Client::ClientChunk* chunk = chunks->GetChunk({cpos.x + cx, cpos.z + cz});
                        if (!chunk || !chunk->chunkData) continue;
                        for (int t = 0; t < static_cast<int>(Game::HeightmapType::Count); ++t) {
                            const auto type = static_cast<Game::HeightmapType>(t);
                            const uint32_t color = colorOf(type);
                            for (int lx = 0; lx < 16; ++lx)
                                for (int lz = 0; lz < 16; ++lz) {
                                    int h = kWorldMinY - 1;
                                    for (int y = kWorldMaxY; y >= kWorldMinY; --y) {
                                        if (Game::HeightmapIsOpaque(type, chunk->chunkData->GetBlock(lx, y, lz))) { h = y; break; }
                                    }
                                    // MC getHeight is the first EMPTY y above (+1 here) plus a per-type stagger.
                                    const double xx = (cpos.x + cx) * 16.0 + lx, zz = (cpos.z + cz) * 16.0 + lz;
                                    const double hh = (h + 1) + t * 0.09375;
                                    g_heights.boxes.push_back({{xx + 0.25, hh, zz + 0.25}, {xx + 0.75, hh + 0.09375, zz + 0.75}, color});
                                }
                        }
                    }
            }
            for (const auto& b : g_heights.boxes) Gizmos::CuboidFill(b.lo, b.hi, b.color);
        }

        // ── CollisionBoxRenderer ────────────────────────────────────────
        struct CollisionCache { double time = -1.0; std::vector<std::pair<glm::dvec3, glm::dvec3>> boxes; } g_collision;

        void RenderCollisionBoxes(const FrameArgs& a) {
            if (!Client::g_clientBlockAccess) return;
            const double now = Now();
            if (now - g_collision.time > 0.1) {
                g_collision.time = now;
                g_collision.boxes.clear();
                const auto& ph = a.player->physics;
                const glm::vec3 lo = glm::vec3(ph.position) - glm::vec3(ph.GetWidth() * 0.5f + 6.0f, 6.0f, ph.GetWidth() * 0.5f + 6.0f);
                const glm::vec3 hi = glm::vec3(ph.position) + glm::vec3(ph.GetWidth() * 0.5f + 6.0f, ph.GetCurrentHeight() + 6.0f, ph.GetWidth() * 0.5f + 6.0f);
                const glm::ivec3 bl = BlockOf(lo), bh = BlockOf(hi);
                for (int x = bl.x; x <= bh.x; ++x)
                    for (int y = bl.y; y <= bh.y; ++y)
                        for (int z = bl.z; z <= bh.z; ++z) {
                            const Game::BlockState st = Client::g_clientBlockAccess->GetBlockState(x, y, z);
                            if (st.Block() == Game::BlockID::Air) continue;
                            const auto shapes = Game::BlockRegistry::GetBlockCollisionShapeSet(st);
                            for (const auto& s : shapes) {
                                g_collision.boxes.emplace_back(glm::dvec3(x, y, z) + glm::dvec3(s.min), glm::dvec3(x, y, z) + glm::dvec3(s.max));
                            }
                        }
            }
            for (const auto& b : g_collision.boxes) Gizmos::Cuboid(b.first, b.second, 0xFFFFFFFF);
        }

        // ── SupportBlockRenderer ────────────────────────────────────────
        // MC highlights entity.mainSupportingBlockPos (getOnPos) and, when
        // it differs, getOnPosLegacy. The engine keeps neither; both are
        // derived the way Entity.getOnPos does: the block 0.500001 / 0.2
        // below the feet.
        void HighlightPosition(const glm::ivec3& pos, double offset, uint32_t color) {
            const double fx = pos.x - 2.0 * offset, fy = pos.y - 2.0 * offset, fz = pos.z - 2.0 * offset;
            const double tx = fx + 1.0 + 4.0 * offset, ty = fy + 1.0 + 4.0 * offset, tz = fz + 1.0 + 4.0 * offset;
            Gizmos::Cuboid({fx, fy, fz}, {tx, ty, tz}, WithAlpha(0.4f, color));
            if (Client::g_clientBlockAccess) {
                const Game::BlockState st = Client::g_clientBlockAccess->GetBlockState(pos.x, pos.y, pos.z);
                for (const auto& s : Game::BlockRegistry::GetBlockCollisionShapeSet(st)) {
                    Gizmos::Cuboid(glm::dvec3(pos) + glm::dvec3(s.min), glm::dvec3(pos) + glm::dvec3(s.max), Opaque(color));
                }
            }
        }

        void DrawSupport(const glm::dvec3& feet, bool onGround, double bias, uint32_t color) {
            if (!onGround) return;
            const glm::ivec3 on(static_cast<int>(std::floor(feet.x)), static_cast<int>(std::floor(feet.y - 0.500001)), static_cast<int>(std::floor(feet.z)));
            HighlightPosition(on, 0.02 + bias, color);
            const glm::ivec3 legacy(on.x, static_cast<int>(std::floor(feet.y - 0.2)), on.z);
            if (legacy != on) HighlightPosition(legacy, 0.04 + bias, 0xFF00FFFF);
        }

        void RenderSupportBlocks(const FrameArgs& a) {
            const auto& ph = a.player->physics;
            DrawSupport(glm::dvec3(ph.position), ph.isOnGround, 0.0, 0xFFFF0000);
            if (!Client::ClientLevels::HasSession()) return;
            if (Client::ClientMobManager* mobs = Client::ClientLevels::Active().Mobs()) {
                const glm::dvec3 cam(a.cameraPos);
                for (const Client::ClientMob* cm : mobs->MobList()) {
                    if (!cm || !cm->mob) continue;
                    const Game::Mob& mob = *cm->mob;
                    if (glm::length(mob.position - cam) > 16.0 + 8.0) continue;
                    // MC getBias: a per-entity hash so overlapping highlights separate.
                    const double bias = 0.02 * static_cast<double>((cm->selfId * 2654435761u) % 1000) / 1000.0;
                    DrawSupport(mob.position, mob.onGround, bias, 0xFF00FF00);
                }
            }
            if (Client::g_remotePlayerManager) {
                for (const auto& [id, rp] : Client::g_remotePlayerManager->GetPlayers()) {
                    if (!Client::IsRemotePlayerInBoundLevel(rp) || !rp.positionInitialized) continue;
                    DrawSupport(rp.position, true, 0.02 * static_cast<double>((id * 2654435761u) % 1000) / 1000.0, 0xFF00FF00);
                }
            }
        }

        // ── LightDebugRenderer ──────────────────────────────────────────
        void RenderLightLevels(const FrameArgs& a, bool showBlock, bool showSky) {
            if (!Client::g_clientBlockAccess) return;
            const glm::ivec3 c = BlockOf(a.cameraPos);
            std::unordered_set<int64_t> sections;
            for (int x = c.x - 10; x <= c.x + 10; ++x)
                for (int y = c.y - 10; y <= c.y + 10; ++y)
                    for (int z = c.z - 10; z <= c.z + 10; ++z) {
                        const int sky = Client::g_clientBlockAccess->GetRawBrightness(x, y, z);
                        const int sx = x >> 4, sy = y >> 4, sz = z >> 4;
                        const int64_t key = (static_cast<int64_t>(sx) << 42) ^ (static_cast<int64_t>(sy & 0xFFFFF) << 21) ^ static_cast<int64_t>(sz & 0x1FFFFF);
                        if (sections.insert(key).second) {
                            // MC prints the light engine's per-section debug data; there is none.
                            Gizmos::BillboardText("n/a", {sx * 16.0 + 8.0, sy * 16.0 + 8.0, sz * 16.0 + 8.0}, 0xFFFF0000, 4.8f, true, false);
                        }
                        if (showSky && sky != 15) {
                            const uint32_t color = Lerp(static_cast<float>(sky) / 15.0f, 0xFF0000FF, 0xFF00FFFF);
                            Gizmos::BillboardText(std::to_string(sky), {x + 0.5, y + 0.25, z + 0.5}, color, 1.0f, true, false);
                        }
                        (void)showBlock;   // no block light exists to show
                    }
        }

        // ── SolidFaceRenderer ───────────────────────────────────────────
        void RenderSolidFaces(const FrameArgs& a) {
            if (!Client::g_clientBlockAccess) return;
            const glm::ivec3 c = BlockOf(a.cameraPos);
            const uint32_t color = 0x80FF0000;   // -2130771968
            static const Game::Direction dirs[6] = { Game::Direction::West, Game::Direction::South, Game::Direction::East,
                                                     Game::Direction::North, Game::Direction::Down, Game::Direction::Up };
            for (int x = c.x - 6; x <= c.x + 6; ++x)
                for (int y = c.y - 6; y <= c.y + 6; ++y)
                    for (int z = c.z - 6; z <= c.z + 6; ++z) {
                        const Game::BlockState st = Client::g_clientBlockAccess->GetBlockState(x, y, z);
                        if (st.Block() == Game::BlockID::Air) continue;
                        const auto shapes = Game::BlockRegistry::GetBlockShapeSet(st);
                        for (const auto& s : shapes) {
                            const glm::dvec3 lo = glm::dvec3(x, y, z) + glm::dvec3(s.min) - 0.002;
                            const glm::dvec3 hi = glm::dvec3(x, y, z) + glm::dvec3(s.max) + 0.002;
                            for (Game::Direction d : dirs) {
                                if (Game::IsStateFaceSturdy(st, d)) Gizmos::Rect(lo, hi, d, color);
                            }
                        }
                    }
        }

        // ── ChunkCullingDebugRenderer ───────────────────────────────────
        void RenderChunkCulling(const FrameArgs& a, bool sectionPath, bool sectionVisibility) {
            if (Render::g_chunkRenderer && (sectionPath || sectionVisibility) && Client::ClientLevels::HasSession()) {
                const SectionOcclusionGraph& graph = Render::g_chunkRenderer->OcclusionGraph();
                Client::ClientChunkManager* chunks = Client::ClientLevels::Active().Chunks();
                for (const SectionRenderData& s : Render::g_chunkRenderer->GetVisibleSections()) {
                    SectionOcclusionGraph::DebugNode node;
                    if (!graph.DebugNodeAt(s.chunkPos.x, s.chunkPos.z, s.sectionY, node)) continue;
                    const glm::dvec3 origin(s.chunkPos.x * 16.0, s.sectionY * 16.0 + kWorldMinY, s.chunkPos.z * 16.0);
                    if (sectionPath) {
                        const uint32_t c = node.step == 0 ? 0 : HsvToRgb(static_cast<float>(node.step) / 50.0f, 0.9f, 0.9f);
                        for (int i = 0; i < 6; ++i) {
                            if (!(node.sourceDirections & (1 << i))) continue;
                            const glm::ivec3 d = kDirStep[i];
                            Gizmos::Line(origin + glm::dvec3(8.0, 8.0, 8.0),
                                         origin + glm::dvec3(8.0 - 16.0 * d.x, 8.0 - 16.0 * d.y, 8.0 - 16.0 * d.z), Opaque(c));
                        }
                    }
                    if (!sectionVisibility || !chunks) continue;
                    const Client::ClientChunk* chunk = chunks->GetChunk(s.chunkPos);
                    if (!chunk || s.sectionY < 0 || s.sectionY >= 24) continue;
                    const GPUSectionData* gpu = chunk->sectionInfos[static_cast<size_t>(s.sectionY)].gpuData.load(std::memory_order_acquire);
                    if (!gpu || !gpu->HasGeometry()) continue;
                    int blocked = 0;
                    for (int d1 = 0; d1 < 6; ++d1)
                        for (int d2 = 0; d2 < 6; ++d2) {
                            if (gpu->visibilitySet.canSeeThrough(d1, d2)) continue;
                            ++blocked;
                            const glm::ivec3 s1 = kDirStep[d1], s2 = kDirStep[d2];
                            Gizmos::Line(origin + glm::dvec3(8.0 + 8.0 * s1.x, 8.0 + 8.0 * s1.y, 8.0 + 8.0 * s1.z),
                                         origin + glm::dvec3(8.0 + 8.0 * s2.x, 8.0 + 8.0 * s2.y, 8.0 + 8.0 * s2.z), 0xFFFF0000);
                        }
                    if (blocked > 0) {
                        Gizmos::CuboidFill(origin + 0.5, origin + 15.5, ColorFromFloat(0.2f, 0.9f, 0.9f, 0.0f));
                    }
                }
            }
            if (g_hasFrustum) {
                auto quad = [](int i0, int i1, int i2, int i3, float r, float g, float b) {
                    Gizmos::Quad(g_frustumPoints[i0], g_frustumPoints[i1],
                                 g_frustumPoints[i2], g_frustumPoints[i3], ColorFromFloat(0.25f, r, g, b));
                };
                quad(0, 1, 2, 3, 0, 1, 1);
                quad(4, 5, 6, 7, 1, 0, 0);
                quad(0, 1, 5, 4, 1, 1, 0);
                quad(2, 3, 7, 6, 0, 0, 1);
                quad(0, 4, 7, 3, 0, 1, 0);
                quad(1, 5, 6, 2, 1, 0, 1);
                static const int edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
                for (const auto& e : edges) Gizmos::Line(g_frustumPoints[e[0]], g_frustumPoints[e[1]], 0xFF000000);
            }
            (void)a;
        }

        // ── OctreeDebugRenderer ─────────────────────────────────────────
        // No octree here: the frustum grid's visible sections are drawn as
        // the leaves MC's would show — numbered, green when "close".
        void RenderSectionOctree(const FrameArgs&) {
            if (!Render::g_chunkRenderer) return;
            int count = 0;
            for (const SectionRenderData& s : Render::g_chunkRenderer->GetVisibleSections()) {
                ++count;
                const glm::dvec3 lo(s.chunkPos.x * 16.0, s.sectionY * 16.0 + kWorldMinY, s.chunkPos.z * 16.0);
                const glm::dvec3 hi = lo + 16.0;
                const uint32_t color = s.nearby ? 0xFF00FF00 : 0xFFFFFFFF;
                Gizmos::BillboardText(std::to_string(count), (lo + hi) * 0.5, color, 4.8f, true, false);
                // colorNum = size + 5 with size 1; MC's getColorComponent(6, m) = frac(m*6)*0.9+0.1.
                auto comp = [](float m) { const float v = m * 6.0f; return (v - std::floor(v)) * 0.9f + 0.1f; };
                Gizmos::Cuboid(lo + 0.1, hi - 0.1, ColorFromFloat(1.0f, comp(0.3f), comp(0.8f), comp(0.5f)));
            }
        }

        // ── ChunkDebugRenderer ──────────────────────────────────────────
        // Per-chunk labels: the client's loaded/empty state and the server's
        // status (from the per-tick sample), at 0.85 of the camera height.
        struct ChunkLabelCache { double time = -1.0; std::vector<std::pair<glm::dvec3, std::string>> labels; } g_chunkLabels;

        const char* StatusName(uint8_t raw) {
            switch (static_cast<Server::ChunkStatus>(raw)) {
                case Server::ChunkStatus::EMPTY: return "empty";
                case Server::ChunkStatus::LOADING: return "loading";
                case Server::ChunkStatus::GENERATING: return "generating";
                case Server::ChunkStatus::STRUCTURE_GEN: return "structure_gen";
                case Server::ChunkStatus::FEATURES: return "features";
                case Server::ChunkStatus::LIGHT_GEN: return "light_gen";
                case Server::ChunkStatus::LIGHT_READY: return "light_ready";
                case Server::ChunkStatus::FULL: return "full";
                default: return "invalid";
            }
        }

        void RenderChunkLabels(const FrameArgs& a) {
            const double now = Now();
            if (now - g_chunkLabels.time > 3.0) {
                g_chunkLabels.time = now;
                g_chunkLabels.labels.clear();
                if (!Client::ClientLevels::HasSession()) return;
                Client::ClientChunkManager* chunks = Client::ClientLevels::Active().Chunks();
                const auto cpos = Game::Math::WorldCoordinates::WorldToChunkPos(
                    static_cast<int>(std::floor(a.cameraPos.x)), static_cast<int>(std::floor(a.cameraPos.z)));
                Server::IntegratedServer::DebugSample sample;
                if (Server::g_integratedServer) sample = Server::g_integratedServer->GetDebugSample();
                const double y = a.cameraPos.y * 0.85;
                for (int x = cpos.x - 12; x <= cpos.x + 12; ++x)
                    for (int z = cpos.z - 12; z <= cpos.z + 12; ++z) {
                        std::string text = "Client: ";
                        const Client::ClientChunk* chunk = chunks ? chunks->GetChunk({x, z}) : nullptr;
                        if (!chunk || chunk->state != Client::ChunkState::LOADED) text += "0n/a";
                        else {
                            bool empty = true;
                            for (const auto& si : chunk->sectionInfos) if (!si.isAllAir) { empty = false; break; }
                            if (empty) text += " E";
                        }
                        if (sample.valid && sample.statusRadius > 0) {
                            const int rx = x - cpos.x + sample.statusRadius, rz = z - cpos.z + sample.statusRadius;
                            const int diameter = sample.statusRadius * 2 + 1;
                            if (rx >= 0 && rz >= 0 && rx < diameter && rz < diameter) {
                                const size_t idx = static_cast<size_t>(rz) * diameter + rx;
                                if (idx < sample.chunkStatus.size()) text += std::string("\nServer: St: ") + StatusName(sample.chunkStatus[idx]);
                            }
                        }
                        g_chunkLabels.labels.emplace_back(glm::dvec3(x * 16.0 + 8.0, y, z * 16.0 + 8.0), text);
                    }
            }
            for (const auto& [pos, text] : g_chunkLabels.labels) {
                int yOffset = 0;
                size_t start = 0;
                while (start <= text.size()) {
                    const size_t nl = text.find('\n', start);
                    const std::string part = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
                    Gizmos::BillboardText(part, {pos.x, pos.y + yOffset, pos.z}, 0xFFFFFFFF, 2.4f, true, true);
                    yOffset -= 2;
                    if (nl == std::string::npos) break;
                    start = nl + 1;
                }
            }
        }
    }

    void CaptureFrustum(const glm::mat4& proj, const glm::mat4& view, const glm::vec3& cameraPos) {
        (void)cameraPos;
        // `view` is the render-space (camera-relative) view, so the corners
        // come out in render space and are moved back to world here — the
        // capture has to survive the camera walking away from this origin.
        const glm::mat4 inv = glm::inverse(proj * view);
        // MC Frustum.getFrustumPoints order: (-1,-1,-1),(1,-1,-1),(1,1,-1),(-1,1,-1) near, then far.
        const glm::vec3 ndc[8] = { {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1}, {-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1} };
        for (int i = 0; i < 8; ++i) {
            glm::vec4 p = inv * glm::vec4(ndc[i], 1.0f);
            g_frustumPoints[i] = Render::ToWorld(glm::vec3(p) / p.w);
        }
        g_hasFrustum = true;
    }

    void KillFrustum() { g_hasFrustum = false; }
    bool HasCapturedFrustum() { return g_hasFrustum; }

    bool AnyEnabled() {
        const DS::EntryList& e = DS::Entries();
        static const char* ids[] = {
            Ids::ChunkBorders, Ids::EntityHitboxes, Ids::ThreeDimensionalCrosshair, Ids::ChunkSectionPaths,
            Ids::ChunkSectionVisibility, Ids::ChunkSectionOctree, Ids::VisualizeWaterLevels, Ids::VisualizeHeightmap,
            Ids::VisualizeCollisionBoxes, Ids::VisualizeEntitySupportingBlocks, Ids::VisualizeBlockLightLevels,
            Ids::VisualizeSkyLightLevels, Ids::VisualizeSolidFaces, Ids::VisualizeChunksOnServer, Ids::VisualizeSkyLightSections };
        for (const char* id : ids) if (e.IsCurrentlyEnabled(id)) return true;
        return g_hasFrustum;
    }

    void Reset() {
        g_hasFrustum = false;
        g_heights = HeightCache{};
        g_collision = CollisionCache{};
        g_chunkLabels = ChunkLabelCache{};
        Gizmos::Clear();
    }

    void RenderWorld(const FrameArgs& a) {
        if (!a.player || !a.camera) return;
        const DS::EntryList& e = DS::Entries();
        if (!AnyEnabled()) { Gizmos::Clear(); return; }
        if (e.IsCurrentlyEnabled(Ids::ChunkBorders))              RenderChunkBorders(a);
        if (e.IsCurrentlyEnabled(Ids::ChunkSectionOctree))        RenderSectionOctree(a);
        if (e.IsCurrentlyEnabled(Ids::VisualizeWaterLevels))      RenderWaterLevels(a);
        if (e.IsCurrentlyEnabled(Ids::VisualizeHeightmap))        RenderHeightmap(a);
        if (e.IsCurrentlyEnabled(Ids::VisualizeCollisionBoxes))   RenderCollisionBoxes(a);
        if (e.IsCurrentlyEnabled(Ids::VisualizeEntitySupportingBlocks)) RenderSupportBlocks(a);
        {
            const bool block = e.IsCurrentlyEnabled(Ids::VisualizeBlockLightLevels);
            const bool sky = e.IsCurrentlyEnabled(Ids::VisualizeSkyLightLevels);
            if (block || sky) RenderLightLevels(a, block, sky);
        }
        if (e.IsCurrentlyEnabled(Ids::VisualizeSolidFaces))       RenderSolidFaces(a);
        if (e.IsCurrentlyEnabled(Ids::VisualizeChunksOnServer))   RenderChunkLabels(a);
        if (e.IsCurrentlyEnabled(Ids::EntityHitboxes))            RenderHitboxes(a);
        RenderChunkCulling(a, e.IsCurrentlyEnabled(Ids::ChunkSectionPaths), e.IsCurrentlyEnabled(Ids::ChunkSectionVisibility));
        if (e.IsCurrentlyEnabled(Ids::ThreeDimensionalCrosshair) && a.firstPerson) RenderCrosshair3D(a);
        Gizmos::Flush(a.proj, a.view, a.cameraPos, a.fovDeg, a.fbWidth, a.fbHeight);
    }

} // namespace Render::DebugRenderer
