// File: src/client/renderer/gui/items/PlayerInventoryPreview.cpp
#include "PlayerInventoryPreview.hpp"
#include "../GuiRenderState.hpp"
#include "../../entity/StickFigureGeometry.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <vector>

namespace Render {

    namespace {

        // CPU-project a model-space vertex through the model matrix, then map to
        // screen-space (centered at cx,cy with `size` pixels per model meter).
        glm::vec2 ProjectToScreen(const glm::vec3& v, const glm::mat4& model,
                                  float cx, float cy, float size) {
            glm::vec3 m = glm::vec3(model * glm::vec4(v, 1.0f));
            // Negate Y to convert world-Y-up → screen-Y-down.
            return glm::vec2(cx + m.x * size, cy + (-m.y) * size);
        }

        // Build a quad command for a "thick line" between two screen-space points.
        // The quad is the two endpoints offset perpendicular to the line by ±width/2.
        // We also extend each endpoint slightly along the line direction so adjacent
        // chained segments (head circle, smile arc) overlap at their shared point
        // and the outer-edge gap disappears. The exact "perfect miter" extension is
        // halfWidth * tan(angleChange/2). StickFigureGeometry's head circle (64
        // segments) and smile (32 segments) both share a 5.625° per-segment angle,
        // so tan(2.8125°) ≈ 0.049 perfectly fills the gap on both. Tiny enough that
        // standalone limbs/eyes don't get visibly extended.
        void EmitThickLineQuad(GuiRenderState* rs,
                               const glm::vec2& p0, const glm::vec2& p1,
                               float width, uint32_t color,
                               const ScissorRect& scissor) {
            glm::vec2 d = p1 - p0;
            float len = glm::length(d);
            if (len < 1e-4f) return; // degenerate
            glm::vec2 dir = d / len;
            float half = width * 0.5f;
            constexpr float kMiterFactor = 0.049f; // tan(2.8125°), matches the 64/32-segment circle/smile
            glm::vec2 ext = dir * (half * kMiterFactor);
            glm::vec2 perp(-dir.y * half, dir.x * half); // perpendicular offset
            glm::vec2 a = p0 - ext;                // slightly pulled-back start
            glm::vec2 b = p1 + ext;                // slightly pushed-forward end
            QuadCommand q;
            q.texture    = INVALID_TEXTURE;
            q.color      = color;
            q.hasScissor = true;
            q.scissor    = scissor;
            q.px[0] = a.x + perp.x; q.py[0] = a.y + perp.y;
            q.px[1] = b.x + perp.x; q.py[1] = b.y + perp.y;
            q.px[2] = b.x - perp.x; q.py[2] = b.y - perp.y;
            q.px[3] = a.x - perp.x; q.py[3] = a.y - perp.y;
            // UVs unused (no texture) but zero them out.
            q.u[0] = q.u[1] = q.u[2] = q.u[3] = 0.0f;
            q.v[0] = q.v[1] = q.v[2] = q.v[3] = 0.0f;
            rs->SubmitQuad(q);
        }

        // Submit a screen-space triangle as a degenerate QuadCommand (one duplicated
        // corner). The rasterizer treats every quad as two triangles {0,1,2} and
        // {0,2,3}; with corner 3 == corner 2, the second triangle is degenerate and
        // contributes no pixels — we get a single visible triangle.
        void EmitTriangleQuad(GuiRenderState* rs,
                              const glm::vec2& a, const glm::vec2& b, const glm::vec2& c,
                              uint32_t color, const ScissorRect& scissor) {
            QuadCommand q;
            q.texture    = INVALID_TEXTURE;
            q.color      = color;
            q.hasScissor = true;
            q.scissor    = scissor;
            q.px[0] = a.x; q.py[0] = a.y;
            q.px[1] = b.x; q.py[1] = b.y;
            q.px[2] = c.x; q.py[2] = c.y;
            q.px[3] = c.x; q.py[3] = c.y; // degenerate
            q.u[0] = q.u[1] = q.u[2] = q.u[3] = 0.0f;
            q.v[0] = q.v[1] = q.v[2] = q.v[3] = 0.0f;
            rs->SubmitQuad(q);
        }

        uint32_t PackColorRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
            return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
        }

    } // namespace

    void RenderStickFigureInInventory(GuiGraphics& g,
                                      int x0, int y0, int x1, int y1,
                                      int size, float offsetY,
                                      float mouseX, float mouseY,
                                      const StickFigurePose& pose,
                                      Game::PlayerColorId colorId) {
        GuiRenderState* rs = g.GetRenderState();
        if (!rs) return;

        // ─── MC's mouse-tracking math (InventoryScreen.java:83-90) ───────────
        const float centerX = (x0 + x1) * 0.5f;
        const float centerY = (y0 + y1) * 0.5f;
        const float xAngle  = std::atan((centerX - mouseX) / 40.0f);
        const float yAngle  = std::atan((centerY - mouseY) / 40.0f);

        // MC's preview ALWAYS faces the camera regardless of the player's world
        // yaw — InventoryScreen.java:91-92 sets `bodyRot = 180.0F + xAngle*20`
        // and `yRot = xAngle*20`, ignoring `(void)pose`. Our equivalent of MC's
        // 180° face-camera bias: in our stick figure, bodyYaw=90° puts bodyFwd
        // at (0,0,+1) which projects toward the camera (we orthographic-project
        // X→screenX, Y→screenY, ignore Z).
        (void)pose;  // base pose (player's world facing) intentionally ignored
        StickFigurePose effective{};
        effective.bodyYawDeg   = 90.0f + xAngle * 20.0f;
        effective.headYawDeg   = 90.0f + xAngle * 20.0f;
        effective.headPitchDeg = -yAngle * 20.0f;
        effective.isCrouching  = false;

        // ─── Build the stick figure ──────────────────────────────────────────
        // ringTris holds the head outline + smile as flat annular ring triangles
        // (gap-free at any angle); discTris holds the back-of-head disc which we
        // skip in the inventory preview (always a front-facing view, no need).
        const auto& colorEntry = Game::LookupPlayerColor(colorId);
        PlayerColor color{ colorEntry.r, colorEntry.g, colorEntry.b, 255 };
        std::vector<StickVertex> lineVerts;
        std::vector<StickVertex> ringTris;
        std::vector<StickVertex> discTris;
        BuildStickFigure(lineVerts, ringTris, discTris,
                         /*feetPos*/ glm::vec3(0.0f),
                         effective.headYawDeg, effective.bodyYawDeg,
                         effective.headPitchDeg, effective.isCrouching,
                         color);

        // ─── Outer view rotation ─────────────────────────────────────────────
        // We negate Y inside ProjectToScreen (world-Y-up → screen-Y-down) so we
        // do NOT replicate MC's `Rz(180°)` (that's MC's compensation for the
        // GuiRenderer's `scale(size, -size, size)` Y-flip — we already did the
        // flip in projection). Just apply the X-tilt for mouse-Y tracking, then
        // translate the figure down so its mid-height sits at the box center.
        glm::mat4 model(1.0f);
        model = glm::rotate(model, yAngle * glm::radians(20.0f), glm::vec3(1, 0, 0));
        model = glm::translate(model, glm::vec3(0.0f, -0.81f - offsetY, 0.0f));

        // ─── Scissor to the preview box ──────────────────────────────────────
        ScissorRect scissor;
        scissor.x0 = static_cast<float>(x0);
        scissor.y0 = static_cast<float>(y0);
        scissor.x1 = static_cast<float>(x1);
        scissor.y1 = static_cast<float>(y1);

        // Match the world stick-figure exactly: GPU PrimitiveType::Lines always
        // rasterizes at 1 logical pixel, regardless of how far the model is from
        // the camera. So a remote player in the world viewed at the same on-screen
        // size as this preview would have 1 px lines too. Using anything thicker
        // (e.g. 1.5 px) makes the preview feel chunkier than the actual model.
        const float kLineWidth = 1.0f;
        const auto sz = static_cast<float>(size);

        // ─── Project + emit lines ────────────────────────────────────────────
        // BuildStickFigure outputs line vertices as PAIRS (a, b, a, b, ...).
        for (size_t i = 0; i + 1 < lineVerts.size(); i += 2) {
            const auto& v0 = lineVerts[i];
            const auto& v1 = lineVerts[i + 1];
            glm::vec2 p0 = ProjectToScreen(glm::vec3(v0.x, v0.y, v0.z), model, centerX, centerY, sz);
            glm::vec2 p1 = ProjectToScreen(glm::vec3(v1.x, v1.y, v1.z), model, centerX, centerY, sz);
            uint32_t color = PackColorRGBA(v0.r, v0.g, v0.b, v0.a);
            EmitThickLineQuad(rs, p0, p1, kLineWidth, color, scissor);
        }

        // ─── Project + emit ring triangles (head outline + smile) ────────────
        // ringTris is a triangle list (a, b, c, a, b, c, ...). Each triangle is
        // submitted as a degenerate QuadCommand (corner 3 == corner 2). The ring
        // already has consistent perpendiculars in model space, so projection
        // never breaks it — no per-segment gaps possible at any view angle.
        for (size_t i = 0; i + 2 < ringTris.size(); i += 3) {
            const auto& va = ringTris[i];
            const auto& vb = ringTris[i + 1];
            const auto& vc = ringTris[i + 2];
            glm::vec2 pa = ProjectToScreen(glm::vec3(va.x, va.y, va.z), model, centerX, centerY, sz);
            glm::vec2 pb = ProjectToScreen(glm::vec3(vb.x, vb.y, vb.z), model, centerX, centerY, sz);
            glm::vec2 pc = ProjectToScreen(glm::vec3(vc.x, vc.y, vc.z), model, centerX, centerY, sz);
            uint32_t color = PackColorRGBA(va.r, va.g, va.b, va.a);
            EmitTriangleQuad(rs, pa, pb, pc, color, scissor);
        }

        // The back-of-head triangle disc (in `discTris`) is intentionally NOT
        // rendered here. In the world, the disc relies on GPU back-face culling
        // (`CullMode::Back`) to be invisible from the front — but the GUI quad
        // path has no culling. Submitting the disc would draw a green circle
        // over the eyes/mouth line features. The line head outline alone gives
        // an acceptable face read in the preview.
        (void)discTris;
    }

} // namespace Render
