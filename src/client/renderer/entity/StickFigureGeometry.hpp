// File: src/client/renderer/entity/StickFigureGeometry.hpp
//
// Pose-driven stick-figure geometry builder. Used by:
//   - PlayerRenderer (renders remote players in the world via GPU lines/triangles)
//   - PlayerInventoryPreview (renders the local player in the inventory's preview
//     box via CPU-projected QuadCommands)
//   - the launcher's stick-figure painter (src/launcher/appearance — the
//     launcher compiles this file; it depends on glm and common/ only)
//
// A figure is one colour or painted per cell (Game::StickFigurePaint; the
// launcher's painter, docs/player-appearance.md) — or replaced by a drawn
// figure (Game::StickFigureDrawing), round strokes drawn like the figure's
// own limbs: the end of this file.
//
// Vertex layout matches the block vertex layout (pos3 + uv2 + color4 ubyte = 24 B)
// so the world renderer can stream it straight into a GPU buffer without copies.
// The UV slots carry no texture coordinates: the world renderer writes the
// body scale into a line's `u` (the strip width), and the builder writes each
// vertex's paint cell into `v` (the launcher's painter picks by it).
#pragma once

#include "common/entity/PlayerAppearance.hpp"
#include "common/entity/PlayerColors.hpp"

#include <glm/glm.hpp>
#include <algorithm>
#include <vector>
#include <cstdint>

namespace Render {

    struct StickVertex {
        float x, y, z;
        float u, v;
        uint8_t r, g, b, a;
    };
    static_assert(sizeof(StickVertex) == 24, "StickVertex must match block vertex stride");

    // Per-player colour for the stick figure. Defaults to the historical neon
    // green so single-player and any callers that don't pass a colour see the
    // same look they always have.
    struct PlayerColor {
        uint8_t r = 0;
        uint8_t g = 255;
        uint8_t b = 60;
        uint8_t a = 255;
    };
    inline constexpr PlayerColor kDefaultPlayerColor{0, 255, 60, 255};

    // A painted figure's colours, one per Game::StickFigurePaint cell (already
    // lit / tinted by the caller). `uniform` builds the plain one-colour
    // figure from cell[0] with the classic geometry (whole limbs, no
    // per-cell splits) — what every unpainted player draws.
    struct StickFigureColors {
        PlayerColor cell[Game::StickFigurePaint::kCellCount];
        bool uniform = true;

        static StickFigureColors Uniform(PlayerColor c) {
            StickFigureColors s;
            for (PlayerColor& p : s.cell) p = c;
            s.uniform = true;
            return s;
        }
        // `paint`'s palette colours, each passed through `shade` (the
        // caller's light, hurt flash and translucency).
        template <class Shade>
        static StickFigureColors FromPaint(const Game::StickFigurePaint& paint, Shade&& shade) {
            StickFigureColors s;
            PlayerColor byId[static_cast<int>(Game::PlayerColorId::Count)];
            for (int i = 0; i < static_cast<int>(Game::PlayerColorId::Count); ++i) {
                const auto& e = Game::LookupPlayerColor(static_cast<Game::PlayerColorId>(i));
                byId[i] = shade(PlayerColor{ e.r, e.g, e.b, 255 });
            }
            for (int c = 0; c < Game::StickFigurePaint::kCellCount; ++c) {
                s.cell[c] = byId[static_cast<int>(paint.At(c))];
            }
            s.uniform = paint.IsUniform();
            return s;
        }
    };

    // Build geometry for a stick-figure player at the given pose. Output goes
    // into three lists:
    //
    //   lineVerts: body, limbs, eyes — line-pair list. Caller decides thickness
    //              (PlayerRenderer expands to camera-facing thick world strips,
    //              PlayerInventoryPreview projects + emits 1 px screen quads).
    //   ringTris : front-face outline + smile, as filled annular ring triangles
    //              in the head's local plane. Triangle list. Same colour as
    //              lineVerts. Wound CCW from the head's lookDir so back-face
    //              culling hides it when viewing the player from behind.
    //   discTris : filled back-of-head disc. Triangle list. Wound CCW from
    //              -lookDir so back-face culling hides it from in front.
    //
    // Splitting ring vs disc lets the inventory preview render only the ring
    // (it always views the player from the front and shouldn't see the disc),
    // while the world renderer batches both into one draw call.
    void BuildStickFigure(std::vector<StickVertex>& lineVerts,
                          std::vector<StickVertex>& ringTris,
                          std::vector<StickVertex>& discTris,
                          const glm::vec3& feetPos,
                          float headYawDeg, float bodyYawDeg,
                          float pitchDeg, bool isCrouching,
                          PlayerColor color = kDefaultPlayerColor,
                          bool isSitting = false);
    // The painted figure: every vertex takes its cell's colour, and carries
    // the cell index in its (otherwise unused) `v` — the launcher's painter
    // picks cells by it. A non-uniform figure splits each limb and arc into
    // its cells; a uniform one is the classic geometry above exactly.
    void BuildStickFigure(std::vector<StickVertex>& lineVerts,
                          std::vector<StickVertex>& ringTris,
                          std::vector<StickVertex>& discTris,
                          const glm::vec3& feetPos,
                          float headYawDeg, float bodyYawDeg,
                          float pitchDeg, bool isCrouching,
                          const StickFigureColors& colors,
                          bool isSitting = false);
    // `isSitting`: MC HumanoidModel.setupAnim's isPassenger pose — the legs
    // from their pivots (±1.9 px, 12 px up) thrust forward (xRot −1.4137167,
    // yRot ±0.31415927, zRot ±0.07853982) and the arms turned forward by
    // xRot −0.62831855; head, neck and body keep their standing heights.

    // The figure's joints, exactly as BuildStickFigure places them (it builds
    // from this). Positions in the space `feetPos` was given in, unscaled.
    struct StickFigureSkeleton {
        glm::vec3 bodyFwd{0.0f}, bodyRight{0.0f};
        glm::vec3 lookDir{0.0f}, faceRight{0.0f};
        glm::vec3 feetPos{0.0f};
        glm::vec3 neck{0.0f}, hip{0.0f}, headC{0.0f};
        glm::vec3 footL{0.0f}, footR{0.0f};
        glm::vec3 shoulderL{0.0f}, shoulderR{0.0f};
        glm::vec3 handL{0.0f}, handR{0.0f};
        glm::vec3 legTopL{0.0f}, legTopR{0.0f};
        float     hipY = 0.0f;
        bool      isCrouching = false;   // after the sitting override
    };
    StickFigureSkeleton ComputeStickFigureSkeleton(const glm::vec3& feetPos,
                                                   float headYawDeg, float bodyYawDeg,
                                                   bool isCrouching, bool isSitting);

    // Where the figure's hand is — the end of the arm line BuildStickFigure
    // draws (right = the player's right, MC's default main arm). The lead
    // renderer ties a held lead there. Unscaled: a caller drawing the figure
    // scaled about its feet scales this the same way.
    glm::vec3 StickFigureHand(const glm::vec3& feetPos, float bodyYawDeg,
                              bool isCrouching, bool isSitting, bool rightHand);

    // ── The drawn figure (Game::StickFigureDrawing) ─────────────────────────
    //
    // A drawing replaces the stick figure, and its strokes are drawn the way
    // the stick figure draws its limbs: each segment rides the figure's LINE
    // list as a line pair, and the world renderer widens it toward the
    // camera at draw time (PlayerRenderer's EmitThickWorldStripFromLines) —
    // a camera-facing ribbon the stroke's width, with a camera-facing disc at
    // every point for the round joints and ends. Together those are the
    // silhouette of a round tube along the stroke, so the drawing reads as
    // the same drawing from the front, as a figure of tubes from the side,
    // and never vanishes edge-on. The colour is flat like the limbs': the
    // figure's light, hurt flash and translucency, no face shading.
    //
    // BuildDrawingMesh flattens the strokes once per look into FIGURE-space
    // segments — blocks, feet at the origin, +X the player's right, +Y up,
    // the drawing in the plane Z = 0. AppendDrawingLines places them at a
    // body every frame (body yaw, the sneak); the world renderer's glide,
    // swim, spin, death topple, lying in bed, body scale and portal model
    // reach them through the line list like the limbs.
    //
    // Layers. Strokes are coplanar, so where two of different colours
    // overlap their ribbons are at the same depth; a later stroke must win
    // as it did on the canvas. Each segment carries its stroke's layer — one
    // above every earlier stroke of another colour it touches — and the
    // widening pulls the ribbon toward the eye by kDrawingLayerStep of the
    // distance per layer: along the eye ray, so the picture on screen does
    // not move, only its depth.

    struct DrawingSegment {
        glm::vec2 a{0.0f}, b{0.0f};   // figure space (X the player's right, Y up), blocks
        float     radius = 0.0f;      // blocks
        uint8_t   color = 0;          // Game::PlayerColorId
        uint8_t   layer = 0;          // 0 .. kDrawingMaxLayer
        bool      capEnd = false;     // a round cap at `b` too: a stroke's last segment
    };
    struct DrawingMesh {
        // Every stroke's segments in stroke order; a dot is one segment with
        // a == b. Each point's disc is drawn at the start of the segment
        // leaving it, and the last point's by `capEnd`.
        std::vector<DrawingSegment> segments;
        float top = 0.0f;      // the highest ink above the feet (blocks)
        bool Empty() const { return segments.empty(); }
    };
    inline constexpr int   kDrawingMaxLayer = 31;
    inline constexpr float kDrawingLayerStep = 1.0f / 4096.0f;
    // Flattens the parts of the strokes at least `fromHeight` blocks above
    // the feet (0: all of it; kDrawingHeadFrom: the head, for a spectator's
    // floating head) — a cut stroke ends round there like any end.
    void BuildDrawingMesh(const Game::StickFigureDrawing& drawing, DrawingMesh& out, float fromHeight = 0.0f);
    // Where a spectator's floating head starts: the stick figure's neck
    // (ComputeStickFigureSkeleton's standing neckY).
    inline constexpr float kDrawingHeadFrom = 1.44f;
    // The sneak: the drawing is squashed toward its feet by the stick
    // figure's crouched eye height over its standing one (HumanoidModel's
    // 4.2-px head drop on a 1.62 eye), so it lowers as the figure does and
    // never sinks into the ground. The stroke width is kept.
    inline constexpr float kDrawingCrouchScaleY = (1.62f - 4.2f / 16.0f) / 1.62f;

    // A drawn segment's line pair is told from a limb's by its `v`: a limb
    // carries its paint cell (>= 0), a stroke a negative tag holding its
    // layer and whether its end is capped. Its `u` is its radius in blocks
    // (a limb's `u` is the body scale, 0 until the renderer sets it).
    inline float DrawingStrokeTag(int layer, bool capEnd) {
        return -static_cast<float>(1 + 2 * layer + (capEnd ? 1 : 0));
    }
    inline bool IsDrawingStroke(const StickVertex& v) { return v.v < -0.5f; }
    // A body's size on a line's width: a limb's `u` becomes the scale, a
    // stroke's radius is multiplied by it.
    inline void ScaleLineWidth(StickVertex& v, float scale) {
        v.u = IsDrawingStroke(v) ? v.u * scale : scale;
    }

    // Appends `mesh`'s segments as line pairs placed at a body: feet at
    // `feetPos`, facing `bodyYawDeg` (MC yaw), squashed while `isCrouching`,
    // each in `palette`'s colour for its PlayerColorId (the caller's light,
    // hurt flash and translucency already applied).
    void AppendDrawingLines(std::vector<StickVertex>& out, const DrawingMesh& mesh,
                            const glm::vec3& feetPos, float bodyYawDeg, bool isCrouching,
                            const PlayerColor* palette);

    // Widens one drawn segment's line pair (IsDrawingStroke) toward `eye`
    // into triangles: the ribbon, the disc at its start and, for a stroke's
    // last segment, the disc at its end — every vertex pulled toward the eye
    // by its layer. Its own space: the eye must be in the vertices' space.
    // No winding: drawn without culling, like the limbs' strips.
    void AppendDrawingStrokeTriangles(const StickVertex& a, const StickVertex& b, const glm::vec3& eye,
                                      std::vector<StickVertex>& out);

    // The stick figure's front view as strokes, at its true size and place:
    // the torso, legs and arms at the limbs' width, the head outline and
    // smile at the rings', the eyes as dots, each part split where its paint
    // changes colour — `paint` is the figure's paint,
    // StickFigurePaint::Uniform(colour) for a plain one. Everything stays
    // inside the canvas (the hands are cut at the hitbox's sides, the feet
    // lifted by their radius). The drawing editor's "From stick figure" and
    // its guide.
    void StickFigureStrokes(const Game::StickFigurePaint& paint, Game::StickFigureDrawing& out);

} // namespace Render
