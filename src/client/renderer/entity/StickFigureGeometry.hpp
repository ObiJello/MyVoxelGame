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
// launcher's painter, docs/player-appearance.md).
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

} // namespace Render
