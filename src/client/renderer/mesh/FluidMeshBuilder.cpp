// File: src/client/renderer/mesh/FluidMeshBuilder.cpp
#include "FluidMeshBuilder.hpp"

#include "common/world/block/BlockModel.hpp"
#include "MeshCensus.hpp"
#include "Mesher.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "common/core/Log.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/ShapeOcclusion.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include "common/world/lighting/LightCoords.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace Render {

    using Game::BlockID;
    using Game::BlockState;
    using Dir = Game::Direction;
    using Game::FluidState;
    using Game::FluidType;

    namespace {

        // MC CardinalLighting's constants (Game::DirectionalShade — the same
        // table the solid mesher shades with): up 1.0, down 0.5, north/south
        // 0.8, west/east 0.6; up and down 0.9 under the Nether's.
        constexpr float kShadeNorth = 0.8f;   // Z faces
        constexpr float kShadeWest  = 0.6f;   // X faces

        // MC insets every fluid face 0.001 into its own block so it never
        // z-fights the neighbouring block's face (FluidRenderer: `offs`).
        constexpr float kFaceInset = 0.001f;

        // MC FluidRenderer.MAX_FLUID_HEIGHT — the height the bottom face's
        // occlusion test is asked with.
        constexpr float kMaxFluidHeight = 0.8888889f;

        // Aurelith's resonant water: its sprites are authored in colour, so
        // the tint is neutral (the biome's water colour must not reach it).
        // Alpha 1 like water's config tint: the sprite carries translucency.
        const glm::vec4 kResonantWaterTint{1.0f, 1.0f, 1.0f, 1.0f};

        // MC sprite.getU(f) / getV(f): f is a fraction of the SPRITE, not of
        // the atlas, so it interpolates between the sprite's own two edges.
        inline float SpriteU(const glm::vec4& uvRect, float f) {
            return uvRect.x + f * (uvRect.z - uvRect.x);
        }
        inline float SpriteV(const glm::vec4& uvRect, float f) {
            return uvRect.y + f * (uvRect.w - uvRect.y);
        }

        void ApplyShade(Vertex (&verts)[4], float shade) {
            for (Vertex& v : verts) {
                glm::vec4 c = v.GetColor();
                c.r *= shade;
                c.g *= shade;
                c.b *= shade;
                v.SetColor(c);
            }
        }

        glm::ivec3 Relative(const glm::ivec3& p, Dir d) {
            return glm::ivec3(p.x + Game::StepX(d), p.y + Game::StepY(d), p.z + Game::StepZ(d));
        }
    } // namespace

    // ========================================================================
    // STILL-FLUID GREEDY MERGING — per-thread scratch
    // ========================================================================
    //
    // The fluid twin of Mesher.cpp's Greedy namespace, and deliberately the
    // same shape: thread_local grids relying on thread-storage zero-init,
    // stash during the section's block loop, one flush per section. It is a
    // SEPARATE grid because fluid quads live on fractional Y planes (the
    // surface sits at y + 0.888 inside cell y) and merge on fluid-domain keys
    // (surface height, still-sprite rect, tint-baked color) that the solid
    // grid has no slot for.
    //
    // Only three quad shapes ever enter: the flat full-cell TOP surface, its
    // reverse-wound underside copy (backwardUpFace), and the flat full-cell
    // volume BOTTOM. Each gets its own plane family (`kind`) so an up-facing
    // plate can never merge with a down-facing one. Side faces, any top whose
    // four corner heights differ, and any top drawn with the rotated flowing
    // sprite stay per-block, always.
    namespace {
        namespace FluidGreedy {
            constexpr int kKindTopUp   = 0;  // top surface, wound +Y-out
            constexpr int kKindTopDown = 1;  // backwardUpFace copy, wound -Y-out
            constexpr int kKindBottom  = 2;  // volume underside, wound -Y-out
            constexpr int kKindCount   = 3;

            // One stashed quad, kept EXACTLY as EmitFluidQuad would have
            // inserted it so a 1x1 survivor re-emits verbatim (tile rect = 0),
            // bit-identical to the non-greedy build.
            struct PendingQuad {
                std::array<Vertex, 4> verts;
                uint16_t  spriteId;  // still sprite's row in the atlas sprite table
                uint32_t  color;     // shared corner color (tint * shade, all four equal)
                uint32_t  light;     // the quad's light word (one value per fluid face, MC)
                float     y;         // the quad's world-space Y (all four corners equal)
                bool      translucent; // routing: water -> translucent, lava -> opaque
                FluidType fluidType; // census only
            };

            // [kind][plane(local y)][z][x]: index+1 into t_pending, 0 = empty.
            // 3*16*16*16*4 = 48 KB per thread.
            thread_local int32_t t_grid[kKindCount][16][16][16];
            thread_local std::vector<PendingQuad> t_pending;
            thread_local bool t_planeTouched[kKindCount][16];
            struct PlaneRef { uint8_t kind, plane; };
            thread_local std::vector<PlaneRef> t_touched;
        }

        // The canonical still-top layout, exactly as the zero-flow top face is
        // emitted: NW, SW, SE, NE with the sprite's full rect mapped V-along-
        // +Z. One check enforces all of (a) flat — four equal corner heights,
        // (b) full-cell footprint, and (c) the UV orientation the merged
        // plate's tile math reproduces. Float compares are exact: every value
        // is an integer-valued float plus the same constants, computed
        // identically per block. A sloped surface or a flow-rotated one
        // fails the check and stays per-block.
        bool IsCanonicalStillTopQuad(const Vertex (&v)[4], glm::vec3 blockPos,
                                     const glm::vec4& rect) {
            const float y = v[0].pos.y;
            if (v[1].pos.y != y || v[2].pos.y != y || v[3].pos.y != y) return false;
            const float x0 = blockPos.x, x1 = blockPos.x + 1.0f;
            const float z0 = blockPos.z, z1 = blockPos.z + 1.0f;
            return v[0].pos.x == x0 && v[0].pos.z == z0 &&
                   v[1].pos.x == x0 && v[1].pos.z == z1 &&
                   v[2].pos.x == x1 && v[2].pos.z == z1 &&
                   v[3].pos.x == x1 && v[3].pos.z == z0 &&
                   v[0].uv == glm::vec2(rect.x, rect.y) &&
                   v[1].uv == glm::vec2(rect.x, rect.w) &&
                   v[2].uv == glm::vec2(rect.z, rect.w) &&
                   v[3].uv == glm::vec2(rect.z, rect.y);
        }

        // Same idea for the bottom face's walk: SW-top, NW-top corner order in
        // MC's clockwise-from-above winding, V again along +Z.
        bool IsCanonicalStillBottomQuad(const Vertex (&v)[4], glm::vec3 blockPos,
                                        const glm::vec4& rect) {
            const float y = v[0].pos.y;
            if (v[1].pos.y != y || v[2].pos.y != y || v[3].pos.y != y) return false;
            const float x0 = blockPos.x, x1 = blockPos.x + 1.0f;
            const float z0 = blockPos.z, z1 = blockPos.z + 1.0f;
            return v[0].pos.x == x0 && v[0].pos.z == z1 &&
                   v[1].pos.x == x0 && v[1].pos.z == z0 &&
                   v[2].pos.x == x1 && v[2].pos.z == z0 &&
                   v[3].pos.x == x1 && v[3].pos.z == z1 &&
                   v[0].uv == glm::vec2(rect.x, rect.w) &&
                   v[1].uv == glm::vec2(rect.x, rect.y) &&
                   v[2].uv == glm::vec2(rect.z, rect.y) &&
                   v[3].uv == glm::vec2(rect.z, rect.w);
        }
    }

    FluidMeshBuilder::FluidMeshBuilder(const FluidMeshConfig& config) : m_config(config) {
    }

    void FluidMeshBuilder::BeginGreedySection(bool enabled,
                                              int baseWorldX, int baseWorldY, int baseWorldZ) {
        m_greedyEnabled = enabled;
        m_greedyBaseX = baseWorldX;
        m_greedyBaseY = baseWorldY;
        m_greedyBaseZ = baseWorldZ;

        // Defensive: drop any leftovers a previous build on this thread failed
        // to flush, exactly as Greedy::ResetThreadState does for the solid
        // grid — stale quads must never leak into this section's mesh.
        if (FluidGreedy::t_touched.empty() && FluidGreedy::t_pending.empty()) return;
        for (const FluidGreedy::PlaneRef& pr : FluidGreedy::t_touched) {
            std::memset(FluidGreedy::t_grid[pr.kind][pr.plane], 0,
                        sizeof(FluidGreedy::t_grid[0][0]));
            FluidGreedy::t_planeTouched[pr.kind][pr.plane] = false;
        }
        FluidGreedy::t_touched.clear();
        FluidGreedy::t_pending.clear();
    }

    bool FluidMeshBuilder::TryStashGreedyFluidQuad(int kind, FluidType fluidType,
                                                   const std::vector<Vertex>& verts,
                                                   uint16_t spriteId, uint32_t lightWord,
                                                   int worldX, int worldY, int worldZ) {
        if (!m_greedyEnabled || verts.size() != 4) return false;

        // All four corner colors identical — same rule as terrain. The tint is
        // applied uniformly per block today, so this only ever fails if a
        // per-corner gradient (future smooth water tint, per-corner light) is
        // introduced; the block then just stays per-quad, correctly.
        const uint32_t color = verts[0].packedColor;
        if (verts[1].packedColor != color ||
            verts[2].packedColor != color ||
            verts[3].packedColor != color) {
            return false;
        }

        // Section-local cell. BuildFluidBlock only sees interior cells, but a
        // mis-indexed write would corrupt another plane's merge — same cheap
        // guard as TryStashGreedyQuad.
        const int lx = worldX - m_greedyBaseX;
        const int ly = worldY - m_greedyBaseY;
        const int lz = worldZ - m_greedyBaseZ;
        if (static_cast<unsigned>(lx) > 15u || static_cast<unsigned>(ly) > 15u ||
            static_cast<unsigned>(lz) > 15u) {
            return false;
        }

        int32_t& cell = FluidGreedy::t_grid[kind][ly][lz][lx];
        if (cell != 0) return false;  // one fluid quad per cell per kind; defensive

        FluidGreedy::t_pending.push_back({{verts[0], verts[1], verts[2], verts[3]},
                                          spriteId, color, lightWord, verts[0].pos.y,
                                          IsTranslucentFluid(fluidType), fluidType});
        cell = static_cast<int32_t>(FluidGreedy::t_pending.size());
        if (!FluidGreedy::t_planeTouched[kind][ly]) {
            FluidGreedy::t_planeTouched[kind][ly] = true;
            FluidGreedy::t_touched.push_back({static_cast<uint8_t>(kind),
                                              static_cast<uint8_t>(ly)});
        }
        return true;
    }

    void FluidMeshBuilder::FlushGreedyFluidQuads(SectionMesh& mesh, int& mergedQuads,
                                                 int& cellsMerged, int& survivorQuads) {
        using FluidGreedy::PendingQuad;
        if (FluidGreedy::t_touched.empty()) return;

        const bool debugColors = Mesher::GreedyDebugColors();

        // Emit one merged plate: cells [u0, u0+w) x [v0, v0+h) of `plane`,
        // where u = local x and v = local z. Corner positions are the exact
        // values the outermost original quads carried — integer world x/z
        // (exactly representable) and the stashed quad's own float y — so a
        // plate never cracks against an unmerged neighbour. UVs are TILE-space
        // section-local coordinates: for all three kinds the sprite's U axis
        // runs along +X and its V axis along +Z (read off the top and bottom
        // emitters corner by corner), so fract(local) reproduces exactly the
        // per-block UV ramp the original quads had.
        auto emitPlate = [&](int kind, int u0, int v0, int w, int h,
                             const PendingQuad& q) {
            std::vector<TerrainVertex>& outVerts =
                q.translucent ? mesh.translucentVerts : mesh.opaqueVerts;
            std::vector<uint16_t>& outIdxs =
                q.translucent ? mesh.translucentIdxs : mesh.opaqueIdxs;
            // Same 16-bit cap policy as GenerateQuad / EmitFluidQuad.
            if (outVerts.size() + 4 > 65536) return;

            // Section-relative, as TerrainVertex stores it: integer grid
            // corners in x/z, the quad's own height in y.
            const float x0 = static_cast<float>(u0);
            const float x1 = static_cast<float>(u0 + w);
            const float z0 = static_cast<float>(v0);
            const float z1 = static_cast<float>(v0 + h);
            const float tu0 = static_cast<float>(u0);
            const float tu1 = static_cast<float>(u0 + w);
            const float tv0 = static_cast<float>(v0);
            const float tv1 = static_cast<float>(v0 + h);
            const float y = q.y - static_cast<float>(m_greedyBaseY);

            // Winding per kind, matching the single-quad emitters corner by
            // corner (a 1x1 plate here is vertex-identical to the original
            // apart from the tile rect). The facing lives in the VERTEX order,
            // never the index order — the translucent re-sort rebuilds every
            // quad's indices from the one forward template, so a plate whose
            // facing rode in its indices would be re-wound and back-face
            // culled, exactly like the backward-up face it replaces.
            glm::vec3 pos[4];
            glm::vec2 uv[4];
            switch (kind) {
                case FluidGreedy::kKindTopUp:    // NW, SW, SE, NE (CCW from above)
                    pos[0] = {x0, y, z0}; uv[0] = {tu0, tv0};
                    pos[1] = {x0, y, z1}; uv[1] = {tu0, tv1};
                    pos[2] = {x1, y, z1}; uv[2] = {tu1, tv1};
                    pos[3] = {x1, y, z0}; uv[3] = {tu1, tv0};
                    break;
                case FluidGreedy::kKindTopDown:  // the exact reverse walk
                    pos[0] = {x1, y, z0}; uv[0] = {tu1, tv0};
                    pos[1] = {x1, y, z1}; uv[1] = {tu1, tv1};
                    pos[2] = {x0, y, z1}; uv[2] = {tu0, tv1};
                    pos[3] = {x0, y, z0}; uv[3] = {tu0, tv0};
                    break;
                default:                         // kKindBottom: CW from above
                    pos[0] = {x0, y, z1}; uv[0] = {tu0, tv1};
                    pos[1] = {x0, y, z0}; uv[1] = {tu0, tv0};
                    pos[2] = {x1, y, z0}; uv[2] = {tu1, tv0};
                    pos[3] = {x1, y, z1}; uv[3] = {tu1, tv1};
                    break;
            }

            const uint32_t quadColor =
                debugColors ? Mesher::GreedyDebugHeatColor(w * h) : q.color;

            const uint16_t base = static_cast<uint16_t>(outVerts.size());
            for (int k = 0; k < 4; ++k) {
                outVerts.push_back(TerrainVertex::Tiled(pos[k],
                                                        static_cast<int>(uv[k].x), static_cast<int>(uv[k].y),
                                                        q.spriteId, quadColor, q.light));
            }
            MeshCensus::Count(Game::FluidBlockId(q.fluidType), q.translucent ? 2 : 0, true,
                              static_cast<uint32_t>(w * h));
            if (!q.translucent) mesh.opaqueFacing.push_back(QuadFacing(pos[0], pos[1], pos[2]));
            outIdxs.insert(outIdxs.end(), {
                static_cast<uint16_t>(base + 0), static_cast<uint16_t>(base + 1),
                static_cast<uint16_t>(base + 2),
                static_cast<uint16_t>(base + 0), static_cast<uint16_t>(base + 2),
                static_cast<uint16_t>(base + 3)
            });
        };

        // A 1x1 survivor: the ORIGINAL quad verbatim (tile rect stays 0),
        // bit-identical to the non-greedy build. Deliberately not routed
        // through EmitFluidQuad — its debug path paints INELIGIBLE, and a
        // survivor is eligible-but-unmerged, which the debug view shows as
        // pure red (GreedyHeatColor(1)), same as terrain survivors.
        auto emitSurvivor = [&](const PendingQuad& q) {
            std::vector<TerrainVertex>& outVerts =
                q.translucent ? mesh.translucentVerts : mesh.opaqueVerts;
            std::vector<uint16_t>& outIdxs =
                q.translucent ? mesh.translucentIdxs : mesh.opaqueIdxs;
            if (outVerts.size() + 4 > 65536) return;
            const uint16_t base = static_cast<uint16_t>(outVerts.size());
            const glm::ivec3 origin(m_greedyBaseX, m_greedyBaseY, m_greedyBaseZ);
            // The quad's facing rides in the slot's free bits (a shader
            // pack's gl_Normal), as the block mesher's GenerateQuad does.
            const uint8_t facing = QuadFacing(q.verts[0].pos, q.verts[1].pos, q.verts[2].pos);
            for (int k = 0; k < 4; ++k) {
                TerrainVertex tv = TerrainVertex::FromWorld(q.verts[static_cast<size_t>(k)], origin, q.light);
                tv.slot = static_cast<uint16_t>(tv.slot | ((facing & 7u) << TerrainVertex::kNormalShift));
                if (debugColors) tv.packedColor = Mesher::GreedyDebugHeatColor(1);
                outVerts.push_back(tv);
            }
            MeshCensus::Count(Game::FluidBlockId(q.fluidType), q.translucent ? 2 : 0, false);
            if (!q.translucent) mesh.opaqueFacing.push_back(facing);
            outIdxs.insert(outIdxs.end(), {
                static_cast<uint16_t>(base + 0), static_cast<uint16_t>(base + 1),
                static_cast<uint16_t>(base + 2),
                static_cast<uint16_t>(base + 0), static_cast<uint16_t>(base + 2),
                static_cast<uint16_t>(base + 3)
            });
        };

        for (const FluidGreedy::PlaneRef& pr : FluidGreedy::t_touched) {
            auto& grid = FluidGreedy::t_grid[pr.kind][pr.plane];
            FluidGreedy::t_planeTouched[pr.kind][pr.plane] = false;

            // Classic 2D greedy scan, identical structure to the solid flush.
            // Cells match on the quad's full visual identity: surface height
            // (float, computed identically per block so equality is exact),
            // sprite rect, packed color, and output layer.
            for (int v = 0; v < 16; ++v) {
                for (int u = 0; u < 16; ++u) {
                    const int32_t cellIdx = grid[v][u];
                    if (cellIdx == 0) continue;
                    const PendingQuad& q = FluidGreedy::t_pending[cellIdx - 1];

                    auto matches = [&](int32_t other) {
                        if (other == 0) return false;
                        const PendingQuad& o = FluidGreedy::t_pending[other - 1];
                        // Light too: a plate is lit uniformly, so only
                        // cells lit alike join (an ocean under open sky
                        // still merges whole; a torch-lit bay breaks up).
                        return o.color == q.color && o.light == q.light && o.spriteId == q.spriteId &&
                               o.y == q.y && o.translucent == q.translucent;
                    };

                    int w = 1;
                    while (u + w < 16 && matches(grid[v][u + w])) ++w;

                    int h = 1;
                    while (v + h < 16) {
                        bool rowOk = true;
                        for (int du = 0; du < w; ++du) {
                            if (!matches(grid[v + h][u + du])) { rowOk = false; break; }
                        }
                        if (!rowOk) break;
                        ++h;
                    }

                    if (w == 1 && h == 1) {
                        grid[v][u] = 0;
                        ++survivorQuads;
                        emitSurvivor(q);
                        continue;
                    }

                    for (int dv = 0; dv < h; ++dv)
                        for (int du = 0; du < w; ++du)
                            grid[v + dv][u + du] = 0;

                    emitPlate(pr.kind, u, v, w, h, q);
                    ++mergedQuads;
                    cellsMerged += w * h;
                }
            }
        }

        FluidGreedy::t_touched.clear();
        FluidGreedy::t_pending.clear();
    }

    bool FluidMeshBuilder::IsTranslucentFluid(FluidType fluidType) {
        return fluidType == FluidType::Water;
    }

    uint32_t FluidMeshBuilder::FluidLight(int worldX, int worldY, int worldZ) const {
        namespace LC = Game::Lighting::LightCoords;
        if (!lightProvider) return TerrainVertex::LightWord(LC::kFullSky);
        return TerrainVertex::LightWord(LC::Max(lightProvider(worldX, worldY, worldZ),
                                                lightProvider(worldX, worldY + 1, worldZ)));
    }

    bool FluidMeshBuilder::EmitFluidQuad(FluidType fluidType,
                                         const std::vector<Vertex>& verts,
                                         uint32_t lightWord, SectionMesh& mesh) const {
        if (verts.size() != 4) return false;

        const bool translucent = IsTranslucentFluid(fluidType);
        // Terrain buffers hold the 20-byte TerrainVertex; the fluid builder's
        // 24-byte world-space Vertex quads are encoded relative to the section
        // origin on insert (untiled, i.e. plain atlas sampling). Only flat
        // full-cell STILL tops/bottoms are greedy-merged, and those are
        // stashed before reaching here (see BeginGreedySection /
        // FlushGreedyFluidQuads); everything on this direct path — side
        // faces, sloped or flow-rotated tops — stays per-block because its
        // heights and UV sub-rects vary.
        std::vector<TerrainVertex>& outVerts = translucent ? mesh.translucentVerts : mesh.opaqueVerts;
        std::vector<uint16_t>&      outIdxs  = translucent ? mesh.translucentIdxs  : mesh.opaqueIdxs;

        // 16-bit index guard: section layer capped at 65,536 vertices.
        if (outVerts.size() + 4 > 65536) return false;

        const uint16_t base = static_cast<uint16_t>(outVerts.size());
        // m_greedyBase* is the section origin: the Mesher sets it through
        // BeginGreedySection for every section, merging on or off.
        const glm::ivec3 origin(m_greedyBaseX, m_greedyBaseY, m_greedyBaseZ);
        const uint8_t facing = QuadFacing(verts[0].pos, verts[1].pos, verts[2].pos);
        for (const Vertex& v : verts) {
            TerrainVertex tv = TerrainVertex::FromWorld(v, origin, lightWord);
            tv.slot = static_cast<uint16_t>(tv.slot | ((facing & 7u) << TerrainVertex::kNormalShift));
            outVerts.push_back(tv);
        }
        MeshCensus::Count(Game::FluidBlockId(fluidType), translucent ? 2 : 0, false);
        if (!translucent) mesh.opaqueFacing.push_back(facing);

        // Debug view: a fluid quad on this path never entered a merge grid —
        // it is rule-ineligible (or merging is off), so it takes the same dim
        // blue-gray AddBlockFace paints its ineligible quads with. Survivors
        // and merged plates are colored by the flush instead.
        if (Mesher::GreedyDebugColors()) {
            const uint32_t c = Mesher::GreedyDebugIneligibleColor();
            for (size_t k = outVerts.size() - 4; k < outVerts.size(); ++k) {
                outVerts[k].packedColor = c;
            }
        }
        outIdxs.insert(outIdxs.end(), {
            static_cast<uint16_t>(base + 0), static_cast<uint16_t>(base + 1),
            static_cast<uint16_t>(base + 2),
            static_cast<uint16_t>(base + 0), static_cast<uint16_t>(base + 2),
            static_cast<uint16_t>(base + 3)
        });
        return true;
    }

    void FluidMeshBuilder::AddFace(FluidType fluidType, const Vertex (&corners)[4], bool addBackFace,
                                   uint32_t lightWord, SectionMesh& mesh) {
        const std::vector<Vertex> forward(corners, corners + 4);
        if (!EmitFluidQuad(fluidType, forward, lightWord, mesh)) return;
        if (addBackFace) {
            // The same four vertices walked the other way round the
            // perimeter — MC addFace's `x0, x3, x2, x1` re-emission.
            const std::vector<Vertex> back{corners[0], corners[3], corners[2], corners[1]};
            EmitFluidQuad(fluidType, back, lightWord, mesh);
        }
    }

    // ── Sprites ────────────────────────────────────────────────────────────

    FluidMeshBuilder::SpriteRect FluidMeshBuilder::LookupSprite(const std::string& texturePath) const {
        SpriteRect out;
        if (!g_atlasBuilder) {
            Log::Warning("No atlas builder available for texture lookup");
            return out;
        }
        AtlasUVRect atlasUV;
        if (!g_atlasBuilder->GetUVRect(texturePath, atlasUV)) {
            Log::Warning("Failed to find texture '%s' in atlas", texturePath.c_str());
            return out;
        }
        out.rect  = glm::vec4(atlasUV.uvMin.x, atlasUV.uvMin.y, atlasUV.uvMax.x, atlasUV.uvMax.y);
        out.id    = atlasUV.spriteId;
        out.valid = true;
        return out;
    }

    FluidMeshBuilder::SpriteRect FluidMeshBuilder::StillSprite(FluidType fluidType, bool resonant) const {
        if (resonant) {
            // Missing sprite (an old resource pack, a texture not yet
            // generated): fall back to water's so the river still draws.
            SpriteRect r = LookupSprite("block/resonant_water_still");
            if (r.valid) return r;
        }
        return LookupSprite(fluidType == FluidType::Lava ? "block/lava_still" : "block/water_still");
    }

    FluidMeshBuilder::SpriteRect FluidMeshBuilder::FlowingSprite(FluidType fluidType, bool resonant) const {
        if (resonant) {
            SpriteRect r = LookupSprite("block/resonant_water_flow");
            if (r.valid) return r;
        }
        return LookupSprite(fluidType == FluidType::Lava ? "block/lava_flow" : "block/water_flow");
    }

    FluidMeshBuilder::SpriteRect FluidMeshBuilder::OverlaySprite() const {
        // MC's water FluidModel names an overlay material; lava's has none.
        return LookupSprite("block/water_overlay");
    }

    bool FluidMeshBuilder::UsesWaterOverlay(BlockID neighbour) {
        // HalfTransparentBlock: glass (TransparentBlock), stained glass,
        // tinted glass, ice, frosted ice, blue ice, slime, honey. LeavesBlock:
        // every "*_leaves". Panes are IronBarsBlocks and do NOT count.
        switch (neighbour) {
            case BlockID::Glass:
            case BlockID::TintedGlass:
            case BlockID::Nightglass:       // Aurelith's smoked glass: tinted glass's class
            case BlockID::Ice:
            case BlockID::FrostedIce:
            case BlockID::BlueIce:
            case BlockID::SlimeBlock:
            case BlockID::HoneyBlock:
                return true;
            default:
                break;
        }
        const std::string_view slug = Game::BlockRegistry::Get(neighbour).registrySlug;
        auto endsWith = [&](std::string_view suffix) {
            return slug.size() >= suffix.size() &&
                   slug.compare(slug.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        return endsWith("_leaves") || endsWith("_stained_glass");
    }

    // ── Heights ────────────────────────────────────────────────────────────

    float FluidMeshBuilder::GetHeight(const Game::IBlockAccess& blocks, FluidType fluidType,
                                      const glm::ivec3& pos, BlockState state,
                                      const FluidState& fluidState) {
        if (fluidState.IsSame(fluidType)) {
            // Continuous through the top when the same fluid is above.
            if (Game::GetFluidState(blocks, pos.x, pos.y + 1, pos.z).IsSame(fluidType)) return 1.0f;
            return fluidState.OwnHeight();
        }
        // `!state.isSolid() ? 0 : -1` — a solid neighbour is left out of the
        // corner average entirely, an open one pulls it down to zero.
        return Game::BlockRegistry::HasCollision(state.Block()) ? -1.0f : 0.0f;
    }

    float FluidMeshBuilder::GetHeight(const Game::IBlockAccess& blocks, FluidType fluidType,
                                      const glm::ivec3& pos) {
        const BlockState state = blocks.GetBlockState(pos.x, pos.y, pos.z);
        return GetHeight(blocks, fluidType, pos, state, Game::FluidStateOf(state));
    }

    float FluidMeshBuilder::CalculateAverageHeight(const Game::IBlockAccess& blocks, FluidType fluidType,
                                                   float heightSelf, float height2, float height1,
                                                   const glm::ivec3& cornerPos) {
        if (height1 >= 1.0f || height2 >= 1.0f) return 1.0f;

        float weightedSum = 0.0f;
        float weightCount = 0.0f;
        // MC addWeightedHeight: a near-full cell (>= 0.8) counts ten times,
        // an open one once, a solid one (-1) not at all.
        auto addWeighted = [&](float height) {
            if (height >= 0.8f) {
                weightedSum += height * 10.0f;
                weightCount += 10.0f;
            } else if (height >= 0.0f) {
                weightedSum += height;
                weightCount += 1.0f;
            }
        };

        if (height1 > 0.0f || height2 > 0.0f) {
            const float heightCorner = GetHeight(blocks, fluidType, cornerPos);
            if (heightCorner >= 1.0f) return 1.0f;
            addWeighted(heightCorner);
        }
        addWeighted(heightSelf);
        addWeighted(height1);
        addWeighted(height2);
        return weightedSum / weightCount;
    }

    // ── Face culling ───────────────────────────────────────────────────────

    bool FluidMeshBuilder::IsFaceOccludedByState(Dir direction, float height, BlockState state) {
        // MC `state.getFaceOcclusionShape(direction.getOpposite())`: empty for
        // a block that cannot occlude at all (glass, leaves, water, lava —
        // anything off the opaque layer), the block's shape otherwise.
        const BlockID id = state.Block();
        if (id == BlockID::Air) return false;
        if (!Game::BlockRegistry::Get(id).opaque) return false;

        // `occluder == Shapes.block()`: everything but an UP face of a fluid
        // shorter than the cell is hidden by a full cube.
        if (Game::BlockRegistry::IsOcclusionFullCube(state)) {
            return direction != Dir::Up || height == 1.0f;
        }

        // `Shapes.blockOccludes(box(0,0,0,1,height,1), occluder, direction)`:
        // the occluder's face toward the fluid covers the fluid's own face.
        const Game::BlockRegistry::BlockShapeSet shape = Game::BlockRegistry::GetBlockShapeSet(state);
        if (shape.count == 0) return false;
        return Game::Shapes::FaceCovers(shape, Game::Opposite(direction),
                                        Game::Shapes::FluidFaceRect(direction, height));
    }

    bool FluidMeshBuilder::IsFaceOccludedByNeighbor(Dir direction, float height,
                                                    BlockState neighborState) {
        return IsFaceOccludedByState(direction, height, neighborState);
    }

    bool FluidMeshBuilder::IsFaceOccludedBySelf(BlockState state, Dir direction) {
        // A waterlogged block hides the water faces its own solid parts
        // cover: a bottom slab's water has no DOWN face, a full-height
        // waterlogged wall's side has none against its post.
        return IsFaceOccludedByState(Game::Opposite(direction), 1.0f, state);
    }

    bool FluidMeshBuilder::ShouldRenderFace(const FluidState& fluidState, BlockState blockState,
                                            Dir direction, const FluidState& neighborFluid) {
        return !neighborFluid.IsSame(fluidState.type) && !IsFaceOccludedBySelf(blockState, direction);
    }

    bool FluidMeshBuilder::ShouldRenderBackwardUpFace(const Game::IBlockAccess& blocks,
                                                      int worldX, int worldY, int worldZ,
                                                      FluidType fluidType) {
        // MC FluidState.shouldRenderBackwardUpFace: scan the 3x3 centred on
        // the block above. A neighbour that is neither the same fluid nor a
        // solid render block means there is a line of sight to the underside
        // of this surface.
        for (int ox = -1; ox <= 1; ++ox) {
            for (int oz = -1; oz <= 1; ++oz) {
                const int x = worldX + ox, y = worldY + 1, z = worldZ + oz;
                const BlockState state = blocks.GetBlockState(x, y, z);
                if (Game::FluidStateOf(state).IsSame(fluidType)) continue;
                // isSolidRender: an opaque full cube.
                const bool solidRender = state.Block() != BlockID::Air &&
                                         Game::BlockRegistry::Get(state.Block()).opaque &&
                                         Game::BlockRegistry::IsOcclusionFullCube(state);
                if (!solidRender) return true;
            }
        }
        return false;
    }

    glm::vec4 FluidMeshBuilder::GetFluidTint(FluidType fluidType,
                                            int worldX, int worldY, int worldZ) const {
        switch (fluidType) {
            case FluidType::Water: {
                // Biome water colour when the Mesher supplied a resolver. The
                // config's alpha is kept: MC's water colour carries no alpha of
                // its own (BiomeColors returns RGB) and the vertex alpha is 1.
                if (waterTintProvider) {
                    glm::vec4 c = waterTintProvider(worldX, worldY, worldZ);
                    c.a = m_config.waterTint.a;
                    return c;
                }
                return m_config.waterTint;
            }
            case FluidType::Lava:
                return m_config.lavaTint;
            default:
                return glm::vec4(1.0f);
        }
    }

    // ── The cell (MC FluidRenderer.tesselate) ──────────────────────────────

    void FluidMeshBuilder::BuildFluidBlock(const Game::IBlockAccess& blocks,
                                           Game::Math::ChunkPos /*chunkPos*/,
                                           int worldX, int worldY, int worldZ,
                                           SectionMesh& outMesh) {
        const glm::ivec3 pos(worldX, worldY, worldZ);
        const BlockState blockState = blocks.GetBlockState(worldX, worldY, worldZ);
        const FluidState fluidState = Game::FluidStateOf(blockState);
        if (fluidState.IsEmpty()) return;
        const FluidType type = fluidState.type;

        // The six neighbours, block and fluid, read once.
        const glm::ivec3 posDown  = Relative(pos, Dir::Down);
        const glm::ivec3 posUp    = Relative(pos, Dir::Up);
        const glm::ivec3 posNorth = Relative(pos, Dir::North);
        const glm::ivec3 posSouth = Relative(pos, Dir::South);
        const glm::ivec3 posWest  = Relative(pos, Dir::West);
        const glm::ivec3 posEast  = Relative(pos, Dir::East);
        const BlockState stateDown  = blocks.GetBlockState(posDown.x,  posDown.y,  posDown.z);
        const BlockState stateUp    = blocks.GetBlockState(posUp.x,    posUp.y,    posUp.z);
        const BlockState stateNorth = blocks.GetBlockState(posNorth.x, posNorth.y, posNorth.z);
        const BlockState stateSouth = blocks.GetBlockState(posSouth.x, posSouth.y, posSouth.z);
        const BlockState stateWest  = blocks.GetBlockState(posWest.x,  posWest.y,  posWest.z);
        const BlockState stateEast  = blocks.GetBlockState(posEast.x,  posEast.y,  posEast.z);
        const FluidState fluidDown  = Game::FluidStateOf(stateDown);
        const FluidState fluidUp    = Game::FluidStateOf(stateUp);
        const FluidState fluidNorth = Game::FluidStateOf(stateNorth);
        const FluidState fluidSouth = Game::FluidStateOf(stateSouth);
        const FluidState fluidWest  = Game::FluidStateOf(stateWest);
        const FluidState fluidEast  = Game::FluidStateOf(stateEast);

        const bool renderUp    = !fluidUp.IsSame(type);
        const bool renderDown  = ShouldRenderFace(fluidState, blockState, Dir::Down, fluidDown) &&
                                 !IsFaceOccludedByNeighbor(Dir::Down, kMaxFluidHeight, stateDown);
        const bool renderNorth = ShouldRenderFace(fluidState, blockState, Dir::North, fluidNorth);
        const bool renderSouth = ShouldRenderFace(fluidState, blockState, Dir::South, fluidSouth);
        const bool renderWest  = ShouldRenderFace(fluidState, blockState, Dir::West,  fluidWest);
        const bool renderEast  = ShouldRenderFace(fluidState, blockState, Dir::East,  fluidEast);
        if (!renderUp && !renderDown && !renderEast && !renderWest && !renderNorth && !renderSouth) {
            return;
        }

        // Aurelith's river (BlockID::ResonantWater): an always-water block, so
        // its fluid state is a plain water source and every rule above and
        // below is water's — only the look differs. Its sprites carry their
        // own violet-and-cyan colour (authored in colour like lava's, not a
        // greyscale tint carrier), so the tint is white rather than the
        // biome's water colour. It glows: an emissiveRendering block
        // (Block::emissive), so the light below reads FULL_BRIGHT.
        const bool resonant = blockState.Block() == BlockID::ResonantWater;
        const glm::vec4 tint = resonant ? kResonantWaterTint : GetFluidTint(type, worldX, worldY, worldZ);

        // MC FluidRenderer: one light per face — the brighter of the cell and
        // the one above for the top and the sides, of the cell below and this
        // one for the bottom.
        const uint32_t lightTopSides = FluidLight(worldX, worldY, worldZ);
        const uint32_t lightBottom   = FluidLight(worldX, worldY - 1, worldZ);

        // Corner heights: 1.0 everywhere when the column continues above,
        // else each corner is the weighted mean of this cell, its two edge
        // neighbours and the diagonal (calculateAverageHeight).
        const float heightSelf = GetHeight(blocks, type, pos, blockState, fluidState);
        float heightNorthEast, heightNorthWest, heightSouthEast, heightSouthWest;
        if (heightSelf >= 1.0f) {
            heightNorthEast = heightNorthWest = heightSouthEast = heightSouthWest = 1.0f;
        } else {
            const float hN = GetHeight(blocks, type, posNorth, stateNorth, fluidNorth);
            const float hS = GetHeight(blocks, type, posSouth, stateSouth, fluidSouth);
            const float hE = GetHeight(blocks, type, posEast,  stateEast,  fluidEast);
            const float hW = GetHeight(blocks, type, posWest,  stateWest,  fluidWest);
            heightNorthEast = CalculateAverageHeight(blocks, type, heightSelf, hN, hE, Relative(posNorth, Dir::East));
            heightNorthWest = CalculateAverageHeight(blocks, type, heightSelf, hN, hW, Relative(posNorth, Dir::West));
            heightSouthEast = CalculateAverageHeight(blocks, type, heightSelf, hS, hE, Relative(posSouth, Dir::East));
            heightSouthWest = CalculateAverageHeight(blocks, type, heightSelf, hS, hW, Relative(posSouth, Dir::West));
        }

        const float x = static_cast<float>(worldX);
        const float y = static_cast<float>(worldY);
        const float z = static_cast<float>(worldZ);
        const glm::vec3 blockPos(x, y, z);
        const glm::vec3 normal(0.0f, 1.0f, 0.0f);   // ignored by Vertex; MC also writes (0,1,0)

        // MC: `float bottomOffs = renderDown ? 0.001F : 0.0F` — the sides drop
        // to the block floor when nothing is drawn there, and stop 0.001 short
        // of it when the bottom face is, so the two don't co-plane.
        const float bottomOffs = renderDown ? kFaceInset : 0.0f;

        // ── Top ──────────────────────────────────────────────────────────
        if (renderUp && !IsFaceOccludedByNeighbor(Dir::Up,
                                                  std::min(std::min(heightNorthWest, heightSouthWest),
                                                           std::min(heightSouthEast, heightNorthEast)),
                                                  stateUp)) {
            // Dropped 0.001 so the surface never co-planes with a fluid
            // surface in the block above.
            heightNorthWest -= kFaceInset;
            heightSouthWest -= kFaceInset;
            heightSouthEast -= kFaceInset;
            heightNorthEast -= kFaceInset;

            const glm::dvec3 flow = Game::FluidFlow(blocks, pos, fluidState);
            SpriteRect sprite;
            float u00, v00, u01, v01, u10, v10, u11, v11;
            if (flow.x == 0.0 && flow.z == 0.0) {
                // Still: the still sprite, full rect, V along +Z.
                sprite = StillSprite(type, resonant);
                u00 = sprite.rect.x; v00 = sprite.rect.y;
                u01 = sprite.rect.x; v01 = sprite.rect.w;
                u10 = sprite.rect.z; v10 = sprite.rect.w;
                u11 = sprite.rect.z; v11 = sprite.rect.y;
            } else {
                // Flowing: the flowing sprite's centre half-rect, rotated to
                // the flow angle (atan2 - 90°) about the sprite centre.
                sprite = FlowingSprite(type, resonant);
                const float angle = static_cast<float>(std::atan2(flow.z, flow.x)) - 1.5707964f;
                const float s = std::sin(angle) * 0.25f;
                const float c = std::cos(angle) * 0.25f;
                u00 = SpriteU(sprite.rect, 0.5f + (-c - s));
                v00 = SpriteV(sprite.rect, 0.5f + (-c + s));
                u01 = SpriteU(sprite.rect, 0.5f + (-c + s));
                v01 = SpriteV(sprite.rect, 0.5f + (c + s));
                u10 = SpriteU(sprite.rect, 0.5f + (c + s));
                v10 = SpriteV(sprite.rect, 0.5f + (c - s));
                u11 = SpriteU(sprite.rect, 0.5f + (c - s));
                v11 = SpriteV(sprite.rect, 0.5f + (-c - s));
            }
            if (!sprite.valid) return;

            Vertex corners[4] = {
                Vertex(blockPos + glm::vec3(0.0f, heightNorthWest, 0.0f), normal, glm::vec2(u00, v00), tint),
                Vertex(blockPos + glm::vec3(0.0f, heightSouthWest, 1.0f), normal, glm::vec2(u01, v01), tint),
                Vertex(blockPos + glm::vec3(1.0f, heightSouthEast, 1.0f), normal, glm::vec2(u10, v10), tint),
                Vertex(blockPos + glm::vec3(1.0f, heightNorthEast, 0.0f), normal, glm::vec2(u11, v11), tint),
            };
            ApplyShade(corners, Game::DirectionalShade(Game::FaceDir::Up, netherCardinalLight));
            const bool backwardUpFace = ShouldRenderBackwardUpFace(blocks, worldX, worldY, worldZ, type);

            // Still-fluid greedy routing: a flat, full-cell, canonically-mapped,
            // uniformly-tinted top surface parks in the merge grid and is
            // emitted by FlushGreedyFluidQuads — the ocean win. A slope or a
            // flow rotation fails the geometric check and stays per-block.
            // The reverse-wound underside copy merges in its own plane family
            // by the same rules.
            bool stashed = false;
            if (m_greedyEnabled && IsCanonicalStillTopQuad(corners, blockPos, sprite.rect)) {
                const std::vector<Vertex> surface(corners, corners + 4);
                if (TryStashGreedyFluidQuad(FluidGreedy::kKindTopUp, type, surface, sprite.id,
                                            lightTopSides, worldX, worldY, worldZ)) {
                    stashed = true;
                    if (backwardUpFace) {
                        const std::vector<Vertex> back(surface.rbegin(), surface.rend());
                        if (!TryStashGreedyFluidQuad(FluidGreedy::kKindTopDown, type, back, sprite.id,
                                                     lightTopSides, worldX, worldY, worldZ)) {
                            EmitFluidQuad(type, back, lightTopSides, outMesh);
                        }
                    }
                }
            }
            if (!stashed) {
                AddFace(type, corners, backwardUpFace, lightTopSides, outMesh);
            }
        }

        // ── Bottom ───────────────────────────────────────────────────────
        if (renderDown) {
            const SpriteRect sprite = StillSprite(type, resonant);
            if (sprite.valid) {
                // Still sprite, full UV rect, wound clockwise from above so
                // the outward normal is -Y. MC's addFace starts at (x, z);
                // this starts one corner along the same cycle, which is the
                // order the greedy bottom plate reproduces.
                Vertex corners[4] = {
                    Vertex(blockPos + glm::vec3(0.0f, bottomOffs, 1.0f), normal, glm::vec2(sprite.rect.x, sprite.rect.w), tint),
                    Vertex(blockPos + glm::vec3(0.0f, bottomOffs, 0.0f), normal, glm::vec2(sprite.rect.x, sprite.rect.y), tint),
                    Vertex(blockPos + glm::vec3(1.0f, bottomOffs, 0.0f), normal, glm::vec2(sprite.rect.z, sprite.rect.y), tint),
                    Vertex(blockPos + glm::vec3(1.0f, bottomOffs, 1.0f), normal, glm::vec2(sprite.rect.z, sprite.rect.w), tint),
                };
                ApplyShade(corners, Game::DirectionalShade(Game::FaceDir::Down, netherCardinalLight));
                bool stashed = false;
                if (m_greedyEnabled && IsCanonicalStillBottomQuad(corners, blockPos, sprite.rect)) {
                    const std::vector<Vertex> quad(corners, corners + 4);
                    stashed = TryStashGreedyFluidQuad(FluidGreedy::kKindBottom, type, quad, sprite.id,
                                                      lightBottom, worldX, worldY, worldZ);
                }
                if (!stashed) {
                    AddFace(type, corners, /*addBackFace=*/false, lightBottom, outMesh);
                }
            }
        }

        // ── Sides ────────────────────────────────────────────────────────
        const SpriteRect flowing = FlowingSprite(type, resonant);
        // Resonant water seen through glass shows its own flowing sprite (no
        // back face, as the overlay): water_overlay is a greyscale carrier
        // for the biome tint, which the river does not take.
        const SpriteRect overlay = type != FluidType::Water ? SpriteRect{}
                                 : resonant ? flowing : OverlaySprite();
        for (Dir faceDir : {Dir::North, Dir::South, Dir::West, Dir::East}) {
            float hh0, hh1, x0, x1, z0, z1;
            bool renderCondition;
            BlockState faceState;
            // MC's per-direction corner walk: (x0, z0) is the first top
            // vertex and (x1, z1) the second; the order differs per face
            // precisely so all four wind counter-clockwise when seen from
            // outside the block. Each is inset 0.001 into the block.
            switch (faceDir) {
                case Dir::North:
                    hh0 = heightNorthWest; hh1 = heightNorthEast;
                    x0 = x;               x1 = x + 1.0f;
                    z0 = z + kFaceInset;  z1 = z + kFaceInset;
                    renderCondition = renderNorth; faceState = stateNorth;
                    break;
                case Dir::South:
                    hh0 = heightSouthEast; hh1 = heightSouthWest;
                    x0 = x + 1.0f;         x1 = x;
                    z0 = z + 1.0f - kFaceInset; z1 = z + 1.0f - kFaceInset;
                    renderCondition = renderSouth; faceState = stateSouth;
                    break;
                case Dir::West:
                    hh0 = heightSouthWest; hh1 = heightNorthWest;
                    x0 = x + kFaceInset;   x1 = x + kFaceInset;
                    z0 = z + 1.0f;         z1 = z;
                    renderCondition = renderWest; faceState = stateWest;
                    break;
                default: // East
                    hh0 = heightNorthEast; hh1 = heightSouthEast;
                    x0 = x + 1.0f - kFaceInset; x1 = x + 1.0f - kFaceInset;
                    z0 = z;                z1 = z + 1.0f;
                    renderCondition = renderEast; faceState = stateEast;
                    break;
            }
            if (!renderCondition) continue;
            if (IsFaceOccludedByNeighbor(faceDir, std::max(hh0, hh1), faceState)) continue;

            // The flowing sprite, or water's overlay against glass, ice and
            // leaves (which also drops the back face: the overlay is the
            // "inside" of the water seen through the transparent block).
            SpriteRect sprite = flowing;
            bool isOverlay = false;
            if (overlay.valid && UsesWaterOverlay(faceState.Block())) {
                sprite = overlay;
                isOverlay = true;
            }
            if (!sprite.valid) continue;

            // The side samples only the TOP-LEFT QUARTER of the flow sprite —
            // u over [0, 0.5], v over [(1-height)*0.5, 0.5] — which is what
            // puts flowing fluid on a block side at 2x the still texture's
            // scale, cut to each corner's own height.
            const float u0  = SpriteU(sprite.rect, 0.0f);
            const float u1  = SpriteU(sprite.rect, 0.5f);
            const float v01 = SpriteV(sprite.rect, (1.0f - hh0) * 0.5f);
            const float v02 = SpriteV(sprite.rect, (1.0f - hh1) * 0.5f);
            const float v1  = SpriteV(sprite.rect, 0.5f);
            const float shadeSide = Game::AxisOf(faceDir) == Game::Axis::Z ? kShadeNorth : kShadeWest;

            Vertex corners[4] = {
                Vertex(glm::vec3(x0, y + hh0,        z0), normal, glm::vec2(u0, v01), tint),
                Vertex(glm::vec3(x1, y + hh1,        z1), normal, glm::vec2(u1, v02), tint),
                Vertex(glm::vec3(x1, y + bottomOffs, z1), normal, glm::vec2(u1, v1),  tint),
                Vertex(glm::vec3(x0, y + bottomOffs, z0), normal, glm::vec2(u0, v1),  tint),
            };
            ApplyShade(corners, Game::DirectionalShade(Game::FaceDir::Up, netherCardinalLight) * shadeSide);
            AddFace(type, corners, /*addBackFace=*/!isOverlay, lightTopSides, outMesh);
        }
    }

} // namespace Render
