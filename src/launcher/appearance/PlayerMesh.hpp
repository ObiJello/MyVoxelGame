// File: src/launcher/appearance/PlayerMesh.hpp
//
// CPU geometry for the launcher's 3D previews, and picking against it.
//
// The player model is built from Game::PlayerLayout — the same boxes the
// game's PlayerModel uses — with every face's corners given the UVs the
// game's ModelPart BuildCube gives them (vertex for vertex, including MC's
// "vertex 0 takes the high u" order). A ray-picked point's interpolated UV
// therefore lands on exactly the texel the game samples there, which is what
// lets the skin editor paint straight onto the 3D model.
//
// The stick figure comes from the game's own StickFigureGeometry builder;
// its line pairs are widened into camera-facing strips here (the world
// renderer does the same), and every triangle keeps its paint cell.
//
// Space: blocks, Y up, feet at the origin, the body facing +Z — the world
// pose of a player with body yaw 0 (MC LivingEntityRenderer's rotateY(180) ·
// scale(-1, -1, 1) chain folded into one flip of Y and Z).
#pragma once

#include "common/entity/PlayerAppearance.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Render { struct DrawingMesh; }

namespace Launcher::Appearance {

    // What the GL preview draws: one vertex of a triangle list.
    struct PreviewVertex {
        float x, y, z;
        float u, v;          // normalised texture coordinates
        float nx, ny, nz;    // for the preview's two-light shading
        uint8_t r, g, b, a;
    };

    // One face triangle of the skinned model.
    struct MeshTri {
        glm::vec3 p[3];
        glm::vec2 uv[3];     // texture PIXELS (not normalised)
        glm::vec3 n;
        // The face's texel rectangle, [x0, x1) × [y0, y1) — the fill tool's bound.
        int rect[4] = {0, 0, 0, 0};
        bool outer = false;  // an outer-layer box
    };

    // The pose the preview holds the model in.
    struct PlayerPose {
        float ageTicks = 0.0f;   // MC AnimationUtils.bobModelPart's clock (the idle arm sway)
        bool  idleSway = true;   // off: the arms hang straight (the editor)
        float headYawDeg = 0.0f;
        float headPitchDeg = 0.0f;
    };

    // The skin model's triangles. `showOuter` draws the second layer, each
    // outer box further gated by `modelParts` (Game::ModelPartBits).
    void BuildPlayerMesh(Game::SkinModel model, bool showOuter, uint8_t modelParts,
                         const PlayerPose& pose, std::vector<MeshTri>& out);
    // MC PlayerCapeModel's cape on the body, in its resting lean
    // (setupAnim's 6° with no movement), UVs in 64x32-sheet pixels.
    void BuildCapeMesh(const PlayerPose& pose, std::vector<MeshTri>& out);

    // Triangles to preview vertices, UVs normalised by the sheet size.
    void AppendMeshVertices(const std::vector<MeshTri>& tris, float texW, float texH,
                            std::vector<PreviewVertex>& out, uint32_t rgba = 0xFFFFFFFFu);

    // A ray against the skinned model: the nearest face hit and its texel.
    // With `skipClearOuterOf` set, an outer-layer face whose texel is
    // transparent in that image is passed through (the eyedropper reads the
    // colour you see).
    struct TexelHit {
        bool hit = false;
        int x = 0, y = 0;
        int tri = -1;
        float t = 0.0f;
    };
    TexelHit PickTexel(const std::vector<MeshTri>& tris, const glm::vec3& origin, const glm::vec3& dir,
                       const Game::SkinImage* skipClearOuterOf = nullptr);

    // Every face rectangle of the skin sheet for a model (both layers), for
    // the 2D view's region outlines and the fill tool's bounds there.
    struct FaceRect {
        int x0, y0, x1, y1;
        bool outer;
        const char* part;
    };
    std::vector<FaceRect> SkinFaceRects(Game::SkinModel model);
    // The face rectangle containing a texel, or null for unused sheet space.
    const FaceRect* FaceRectAt(const std::vector<FaceRect>& rects, int x, int y);

    // ── The stick figure ────────────────────────────────────────────────────

    // Triangles of the stick figure as the preview draws them: `lines` (the
    // limbs as camera-facing strips — no culling), `faces` (head outline,
    // eyes and smile — front only) and `back` (the back-of-head disc — back
    // only). Each triangle's cell rides alongside for picking.
    struct StickMesh {
        std::vector<PreviewVertex> lines, faces;
        std::vector<int> lineCells, faceCells;   // one per triangle
    };
    // `colors` per Game::StickFigurePaint cell; `eye` is the camera position
    // (the strips face it); `halfWidth` the strip half-width in blocks.
    // `highlightCell` (>= 0) is drawn brightened — the painter's hover.
    void BuildStickMesh(const Game::StickFigurePaint& paint, bool uniformLook,
                        const glm::vec3& eye, float halfWidth, int highlightCell,
                        StickMesh& out);

    // The cell under a ray: strips (picked a little wider than drawn so a
    // thin limb is easy to hit) and the faces, front-facing only. -1 for none.
    int PickStickCell(const Game::StickFigurePaint& paint, bool uniformLook,
                      const glm::vec3& eye, const glm::vec3& origin, const glm::vec3& dir);

    // ── The drawn figure ────────────────────────────────────────────────────

    // A drawing's strokes (StickFigureGeometry's DrawingMesh) standing at the
    // origin facing +Z — exactly as a player with body yaw 0 wears it —
    // widened toward `eye` the way the game widens them (camera-facing
    // ribbons with round joints, AppendDrawingStrokeTriangles), in the
    // palette's own flat colours: unlit, drawn without culling, like the
    // stick figure's limbs here.
    void AppendDrawingPreview(const Render::DrawingMesh& mesh, const glm::vec3& eye,
                              std::vector<PreviewVertex>& out);

    // Möller–Trumbore; t along `dir` (any length), false for a miss. With
    // `frontOnly`, a triangle seen from its back (CW from the ray) misses.
    bool RayTriangle(const glm::vec3& o, const glm::vec3& d, const glm::vec3& a, const glm::vec3& b,
                     const glm::vec3& c, bool frontOnly, float& t, float& u, float& v);

} // namespace Launcher::Appearance
