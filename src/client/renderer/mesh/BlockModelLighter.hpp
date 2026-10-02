// File: src/client/renderer/mesh/BlockModelLighter.hpp
//
// MC BlockModelLighter (ModelBlockRenderer's AmbientOcclusion / flat light
// preparation), written once against an abstract level so the two places
// that light a block model do it with the SAME arithmetic:
//
//   * the section mesher (Mesher::ComputeFaceLight / ComputeFaceAO), over its
//     snapshot caches;
//   * the moving-block pass (BlockCubeEntityRenderer, MC
//     MovingBlockFeatureRenderer's tesselateBlock), over the live client
//     level — a piston's base while it retracts, the head, a carried block,
//     and a landed block until the section mesh that shows it has uploaded.
//
// A block handed from the section mesh to the moving-block pass and back
// (every piston move does that twice) must not change its look on either
// hand-over, which only holds if both read the same cells the same way.
//
// `Level` is any type with:
//   Game::BlockState StateAt(int x, int y, int z) const;
//   int   LightCoordsWith(Game::BlockState state, int x, int y, int z) const;
//                       // MC LightCoordsUtil.getLightCoords(state, level, pos)
//   bool  LightPermeableAt(int x, int y, int z) const;   // MC isLightPermeable
//   float ShadeAt(int x, int y, int z) const;            // MC getShadeBrightness
//
// `localPos` is a quad's four vertices relative to its block's cell (MC's
// baked quad positions), in any order: every per-vertex value is the
// bilinear blend of the four cell-face corners at that vertex.
#pragma once

#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/lighting/BlockLightProperties.hpp"
#include "common/world/lighting/LightCoords.hpp"

#include <algorithm>
#include <glm/glm.hpp>

namespace Render::BlockModelLighter {

    // MC BlockModelLighter.AdjacencyInfo.corners, by Direction ordinal.
    inline constexpr Game::Direction kCorners[6][4] = {
        /* Down  */ { Game::Direction::West, Game::Direction::East, Game::Direction::North, Game::Direction::South },
        /* Up    */ { Game::Direction::East, Game::Direction::West, Game::Direction::North, Game::Direction::South },
        /* North */ { Game::Direction::Up,   Game::Direction::Down, Game::Direction::East,  Game::Direction::West  },
        /* South */ { Game::Direction::West, Game::Direction::East, Game::Direction::Down,  Game::Direction::Up    },
        /* West  */ { Game::Direction::Up,   Game::Direction::Down, Game::Direction::North, Game::Direction::South },
        /* East  */ { Game::Direction::Down, Game::Direction::Up,   Game::Direction::North, Game::Direction::South },
    };

    // The four cell-face corner values (MC _tc1.._tc4) and the two corner
    // sides each sits between: (c3, c0), (c2, c0), (c2, c1), (c3, c1).
    inline constexpr int kCornerSides[4][2] = { {3, 0}, {2, 0}, {2, 1}, {3, 1} };

    // MC prepareQuadShape's faceCubic: the quad lies flat on the side of its
    // cell it faces (or the block's collision is a full cube), so its light
    // and shade come from the neighbour cell.
    inline bool FaceCubic(Game::Direction dir, const glm::vec3 (&localPos)[4], bool fullCollision) {
        glm::vec3 mn(32.0f), mx(-32.0f);
        for (const glm::vec3& p : localPos) { mn = glm::min(mn, p); mx = glm::max(mx, p); }
        constexpr float kLo = 1.0e-4f, kHi = 0.9999f;
        switch (dir) {
            case Game::Direction::Down:  return mn.y == mx.y && (mn.y < kLo || fullCollision);
            case Game::Direction::Up:    return mn.y == mx.y && (mx.y > kHi || fullCollision);
            case Game::Direction::North: return mn.z == mx.z && (mn.z < kLo || fullCollision);
            case Game::Direction::South: return mn.z == mx.z && (mx.z > kHi || fullCollision);
            case Game::Direction::West:  return mn.x == mx.x && (mn.x < kLo || fullCollision);
            case Game::Direction::East:  return mn.x == mx.x && (mx.x > kHi || fullCollision);
        }
        return false;
    }

    inline bool FaceCubic(Game::BlockState state, Game::Direction dir, const glm::vec3 (&localPos)[4]) {
        return FaceCubic(dir, localPos, Game::Lighting::BlockLightProperties::FullCollision(state));
    }

    // Bilinear weight of a cell-face corner that sits on the `d` side, at a
    // vertex whose block-local coordinate along d's axis is `p`.
    inline float CornerWeight(Game::Direction d, const glm::vec3& p) {
        const int axis = d == Game::Direction::East || d == Game::Direction::West ? 0
                       : d == Game::Direction::Up   || d == Game::Direction::Down ? 1 : 2;
        const bool positive = d == Game::Direction::East || d == Game::Direction::Up || d == Game::Direction::South;
        const float c = std::clamp(p[axis], 0.0f, 1.0f);
        return positive ? c : 1.0f - c;
    }

    // The weight of cell-face corner `k` (kCornerSides) at vertex `p`.
    inline float CornerBlendWeight(const Game::Direction (&corners)[4], int k, const glm::vec3& p) {
        return CornerWeight(corners[kCornerSides[k][0]], p) * CornerWeight(corners[kCornerSides[k][1]], p);
    }

    // MC BlockModelLighter.prepareQuadAmbientOcclusion, shade half: the four
    // vertices' AO (0.2..1.0) for a quad of `state` at cell (x, y, z) facing
    // `dir`. Every sample is MC getShadeBrightness. The four side cells sit
    // around `base` (the neighbour cell for a face on the cell boundary, the
    // block's own cell otherwise); a diagonal is read only when one of its
    // two sides lets light through (26.x: both opaque → shade0 for all four,
    // as the light half takes light0); the centre is the base cell's shade.
    template <class Level>
    void QuadShade(const Level& level, Game::BlockState state, int x, int y, int z,
                   Game::Direction dir, const glm::vec3 (&localPos)[4], float (&outAO)[4]) {
        const int dx = Game::StepX(dir), dy = Game::StepY(dir), dz = Game::StepZ(dir);
        const bool faceCubic = FaceCubic(state, dir, localPos);
        const int bx = faceCubic ? x + dx : x;
        const int by = faceCubic ? y + dy : y;
        const int bz = faceCubic ? z + dz : z;

        const Game::Direction (&corners)[4] = kCorners[static_cast<int>(dir)];
        float shade[4];
        bool permeable[4];
        for (int i = 0; i < 4; ++i) {
            const Game::Direction c = corners[i];
            const int cx = bx + Game::StepX(c), cy = by + Game::StepY(c), cz = bz + Game::StepZ(c);
            shade[i] = level.ShadeAt(cx, cy, cz);
            permeable[i] = level.LightPermeableAt(cx + dx, cy + dy, cz + dz);
        }
        const auto diagonal = [&](int a, int b) {
            const Game::Direction ca = corners[a], cb = corners[b];
            return level.ShadeAt(bx + Game::StepX(ca) + Game::StepX(cb),
                                 by + Game::StepY(ca) + Game::StepY(cb),
                                 bz + Game::StepZ(ca) + Game::StepZ(cb));
        };
        const float shadeCorner02 = (!permeable[2] && !permeable[0]) ? shade[0] : diagonal(0, 2);
        const float shadeCorner03 = (!permeable[3] && !permeable[0]) ? shade[0] : diagonal(0, 3);
        const float shadeCorner12 = (!permeable[2] && !permeable[1]) ? shade[0] : diagonal(1, 2);
        const float shadeCorner13 = (!permeable[3] && !permeable[1]) ? shade[0] : diagonal(1, 3);
        // MC: faceCubic ? shade(basePosition) : shade(centerPosition) — the
        // same cell either way, since base is the centre when not cubic.
        const float shadeCenter = level.ShadeAt(bx, by, bz);

        const float cornerLevel[4] = {
            (shade[3] + shade[0] + shadeCorner03 + shadeCenter) * 0.25f,
            (shade[2] + shade[0] + shadeCorner02 + shadeCenter) * 0.25f,
            (shade[2] + shade[1] + shadeCorner12 + shadeCenter) * 0.25f,
            (shade[3] + shade[1] + shadeCorner13 + shadeCenter) * 0.25f,
        };
        for (int v = 0; v < 4; ++v) {
            float ao = 0.0f;
            for (int k = 0; k < 4; ++k) {
                ao += cornerLevel[k] * CornerWeight(corners[kCornerSides[k][0]], localPos[v]) *
                                       CornerWeight(corners[kCornerSides[k][1]], localPos[v]);
            }
            outAO[v] = std::clamp(ao, 0.0f, 1.0f);
        }
    }

    // MC BlockModelLighter.prepareQuadAmbientOcclusion, light half: the four
    // vertices' light coords (SMOOTH form) for a quad of `state` at cell
    // (x, y, z) facing `dir` — MC's smoothBlend at each cell-face corner and,
    // for a quad that does not fill its cell's face, smoothWeightedBlend
    // with the quad-shape weights.
    template <class Level>
    void QuadLightSmooth(const Level& level, Game::BlockState state, int x, int y, int z,
                         Game::Direction dir, const glm::vec3 (&localPos)[4], int (&outCoords)[4]) {
        namespace LC = Game::Lighting::LightCoords;
        using Game::Lighting::BlockLightProperties;
        const int dx = Game::StepX(dir), dy = Game::StepY(dir), dz = Game::StepZ(dir);
        const bool faceCubic = FaceCubic(state, dir, localPos);
        const int bx = faceCubic ? x + dx : x;
        const int by = faceCubic ? y + dy : y;
        const int bz = faceCubic ? z + dz : z;
        const auto lightAt = [&](int cx, int cy, int cz) {
            return level.LightCoordsWith(level.StateAt(cx, cy, cz), cx, cy, cz);
        };

        const Game::Direction (&corners)[4] = kCorners[static_cast<int>(dir)];
        int light[4];
        bool permeable[4];
        for (int i = 0; i < 4; ++i) {
            const Game::Direction c = corners[i];
            const int cx = bx + Game::StepX(c), cy = by + Game::StepY(c), cz = bz + Game::StepZ(c);
            light[i] = lightAt(cx, cy, cz);
            permeable[i] = level.LightPermeableAt(cx + dx, cy + dy, cz + dz);
        }
        const auto diagonal = [&](int a, int b) {
            const Game::Direction ca = corners[a], cb = corners[b];
            return lightAt(bx + Game::StepX(ca) + Game::StepX(cb),
                           by + Game::StepY(ca) + Game::StepY(cb),
                           bz + Game::StepZ(ca) + Game::StepZ(cb));
        };
        // Where both side cells are opaque the diagonal is hidden: MC takes
        // the first side's light instead (26.x: light0 for all four).
        const int lightCorner02 = (!permeable[2] && !permeable[0]) ? light[0] : diagonal(0, 2);
        const int lightCorner03 = (!permeable[3] && !permeable[0]) ? light[0] : diagonal(0, 3);
        const int lightCorner12 = (!permeable[2] && !permeable[1]) ? light[0] : diagonal(1, 2);
        const int lightCorner13 = (!permeable[3] && !permeable[1]) ? light[0] : diagonal(1, 3);

        int lightCenter = level.LightCoordsWith(state, x, y, z);
        {
            const Game::BlockState next = level.StateAt(x + dx, y + dy, z + dz);
            if (faceCubic || !BlockLightProperties::SolidRender(next)) {
                lightCenter = level.LightCoordsWith(next, x + dx, y + dy, z + dz);
            }
        }

        const int tc[4] = {
            LC::SmoothBlend(light[3], light[0], lightCorner03, lightCenter),
            LC::SmoothBlend(light[2], light[0], lightCorner02, lightCenter),
            LC::SmoothBlend(light[2], light[1], lightCorner12, lightCenter),
            LC::SmoothBlend(light[3], light[1], lightCorner13, lightCenter),
        };
        for (int v = 0; v < 4; ++v) {
            outCoords[v] = LC::SmoothWeightedBlend(tc[0], tc[1], tc[2], tc[3],
                                                   CornerBlendWeight(corners, 0, localPos[v]),
                                                   CornerBlendWeight(corners, 1, localPos[v]),
                                                   CornerBlendWeight(corners, 2, localPos[v]),
                                                   CornerBlendWeight(corners, 3, localPos[v]));
        }
    }

    // MC ModelBlockRenderer.tesselateFlat's light: a quad with a cullface
    // reads the cell that cullface names; one without reads the neighbour
    // when it lies on the face it points at, else its own cell — always
    // through the block's OWN state (emission, emissiveRendering). Packed.
    template <class Level>
    int QuadLightFlat(const Level& level, Game::BlockState state, int x, int y, int z,
                      Game::Direction dir, const Game::Direction* cullface,
                      const glm::vec3 (&localPos)[4]) {
        int lx = x, ly = y, lz = z;
        if (cullface) {
            lx += Game::StepX(*cullface); ly += Game::StepY(*cullface); lz += Game::StepZ(*cullface);
        } else if (FaceCubic(state, dir, localPos)) {
            lx += Game::StepX(dir); ly += Game::StepY(dir); lz += Game::StepZ(dir);
        }
        return level.LightCoordsWith(state, lx, ly, lz);
    }

    // Whether a block model is lit the smooth way (AO + four-corner light) —
    // MC ModelBlockRenderer.tesselateBlock: the model's ambientocclusion
    // flag, the Smooth Lighting option, and no light emission (an emitting
    // or emissiveRendering block is lit flat and carries no AO).
    inline bool UsesSmoothLighting(Game::BlockState state, bool modelAmbientOcclusion, bool smoothLightingOption) {
        using Game::Lighting::BlockLightProperties;
        return modelAmbientOcclusion && smoothLightingOption &&
               BlockLightProperties::Emission(state) == 0 &&
               !BlockLightProperties::EmissiveRendering(state);
    }

} // namespace Render::BlockModelLighter
