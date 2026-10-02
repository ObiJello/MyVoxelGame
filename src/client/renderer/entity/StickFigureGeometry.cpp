// File: src/client/renderer/entity/StickFigureGeometry.cpp
#include "StickFigureGeometry.hpp"
#include "common/core/Mth.hpp"
#include <algorithm>
#include <cmath>

namespace Render {

    namespace {

        using Paint = Game::StickFigurePaint;

        // One vertex. `cell` rides the vertex's unused `v` (the launcher's
        // painter reads it back to pick a cell); `u` stays 0 — the world
        // renderer writes the body scale there for the strip width.
        StickVertex Vert(const glm::vec3& p, const PlayerColor& c, int cell) {
            return StickVertex{ p.x, p.y, p.z, 0.0f, static_cast<float>(cell), c.r, c.g, c.b, c.a };
        }

        void PushLine(std::vector<StickVertex>& out,
                      const glm::vec3& a, const glm::vec3& b,
                      const PlayerColor& c, int cell) {
            out.push_back(Vert(a, c, cell));
            out.push_back(Vert(b, c, cell));
        }

        // A limb from `a` to `b`, as the part's cells: one line per cell for
        // a painted figure (cell 0 at `a`), one whole line for a uniform one.
        void PushPartLine(std::vector<StickVertex>& out,
                          const glm::vec3& a, const glm::vec3& b,
                          const StickFigureColors& colors, Paint::Part part) {
            const int first = Paint::FirstCell(part);
            if (colors.uniform) {
                PushLine(out, a, b, colors.cell[first], first);
                return;
            }
            const int n = Paint::CellCount(part);
            for (int i = 0; i < n; ++i) {
                const glm::vec3 p0 = glm::mix(a, b, static_cast<float>(i) / static_cast<float>(n));
                const glm::vec3 p1 = glm::mix(a, b, static_cast<float>(i + 1) / static_cast<float>(n));
                PushLine(out, p0, p1, colors.cell[first + i], first + i);
            }
        }

        // The cell of segment `i` of `segments` when a part's cells divide
        // the arc evenly (a uniform figure: always the part's first cell).
        int ArcCell(const StickFigureColors& colors, Paint::Part part, int i, int segments) {
            const int first = Paint::FirstCell(part);
            if (colors.uniform) return first;
            const int n = Paint::CellCount(part);
            return first + std::min(n - 1, (i * n) / segments);
        }

        // Push a filled annular ring (or arc) as triangle pairs. Two triangles
        // per segment, all in the plane spanned by `right` × `up`, between an
        // inner and outer radius. Wound CCW when viewed from `right × up`'s
        // positive-normal side — that way back-face culling hides the ring
        // when the camera is on the opposite side of the head.
        //
        // This replaces the old N-line-segment "stroke" approach for circles.
        // Because there are no separate quads, there are no per-segment joins
        // and no possible gaps regardless of view angle.
        //
        // `cellOf(i)` names segment i's paint cell.
        template <class CellOf>
        void PushArcRing(std::vector<StickVertex>& out,
                         const glm::vec3& center, const glm::vec3& right,
                         const glm::vec3& up, float radius, float halfWidth,
                         int segments, float startAngle, float endAngle,
                         const StickFigureColors& colors, CellOf&& cellOf) {
            if (segments < 1) return;
            const float rIn  = radius - halfWidth;
            const float rOut = radius + halfWidth;
            const float step = (endAngle - startAngle) / static_cast<float>(segments);
            auto vertAt = [&](float angle, float rad) -> glm::vec3 {
                return center + right * (std::cos(angle) * rad) + up * (std::sin(angle) * rad);
            };
            for (int i = 0; i < segments; ++i) {
                const int cell = cellOf(i);
                const PlayerColor& c = colors.cell[cell];
                float a0 = startAngle + step * static_cast<float>(i);
                float a1 = startAngle + step * static_cast<float>(i + 1);
                glm::vec3 i0 = vertAt(a0, rIn);
                glm::vec3 o0 = vertAt(a0, rOut);
                glm::vec3 i1 = vertAt(a1, rIn);
                glm::vec3 o1 = vertAt(a1, rOut);
                // For the head outline / smile, callers pass right=faceRight,
                // up=worldUp. The natural CCW normal from cross(right, up) is
                // -lookDir (behind the player), but we want the ring visible
                // from in FRONT of the player, so wind the triangles the other
                // way: CCW when viewed from +lookDir, which means each triangle
                // gets reversed compared to the natural cross(right, up) side.
                out.push_back(Vert(i0, c, cell)); out.push_back(Vert(o1, c, cell)); out.push_back(Vert(o0, c, cell));
                out.push_back(Vert(i0, c, cell)); out.push_back(Vert(i1, c, cell)); out.push_back(Vert(o1, c, cell));
            }
        }

        // Push a filled disc as a triangle fan. The disc normal faces along `normal`.
        // With CullMode::Back, the disc is only visible from the side `normal` points at.
        template <class CellOf>
        void PushDisc(std::vector<StickVertex>& out,
                      const glm::vec3& center, const glm::vec3& right,
                      const glm::vec3& up, const glm::vec3& /*normal*/,
                      float radius, int segments,
                      const StickFigureColors& colors, CellOf&& cellOf) {
            constexpr float TWO_PI = 2.0f * 3.14159265f;
            float step = TWO_PI / static_cast<float>(segments);
            for (int i = 0; i < segments; ++i) {
                const int cell = cellOf(i);
                const PlayerColor& c = colors.cell[cell];
                float a0 = step * static_cast<float>(i);
                float a1 = step * static_cast<float>(i + 1);
                glm::vec3 p0 = center + right * (std::cos(a0) * radius) + up * (std::sin(a0) * radius);
                glm::vec3 p1 = center + right * (std::cos(a1) * radius) + up * (std::sin(a1) * radius);
                // Triangle: center, p0, p1 (CCW when viewed from normal direction)
                out.push_back(Vert(center, c, cell));
                out.push_back(Vert(p0, c, cell));
                out.push_back(Vert(p1, c, cell));
            }
        }

    } // namespace

    StickFigureSkeleton ComputeStickFigureSkeleton(const glm::vec3& entityFeetPos,
                                                   float headYawDeg, float bodyYawDeg,
                                                   bool isCrouching, bool isSitting) {
        StickFigureSkeleton sk;
        const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};
        // Body orientation. Yaws are MC's (0 = +Z, clockwise) — the same
        // numbers that arrive on the wire and that the camera stores.
        glm::vec3 bodyFwd = Game::Mth::HorizontalViewVector(bodyYawDeg);
        glm::vec3 bodyRight = glm::normalize(glm::cross(bodyFwd, worldUp));

        // Head orientation
        glm::vec3 lookDir = Game::Mth::HorizontalViewVector(headYawDeg);
        glm::vec3 faceRight = glm::normalize(glm::cross(lookDir, worldUp));

        // Sitting (MC HumanoidModel.setupAnim, isPassenger) never crouches
        // (LocalPlayer.aiStep: crouching needs !isPassenger). The body keeps
        // its standing height — MC moves no part but the limbs — and the
        // feet stay the entity's, which the vehicle attachment put 0.6 below
        // the seat.
        if (isSitting) isCrouching = false;
        const glm::vec3& feetPos = entityFeetPos;

        // Crouching (Minecraft's HumanoidModel.java)
        float crouchTilt     = isCrouching ? 0.5f : 0.0f;
        float crouchHeadDrop = isCrouching ? (4.2f / 16.0f) : 0.0f;
        float crouchBodyDrop = isCrouching ? (3.2f / 16.0f) : 0.0f;
        float crouchLegBack  = isCrouching ? (4.0f / 16.0f) : 0.0f;

        float neckY  = 1.44f - crouchBodyDrop;
        float hipY   = 0.90f;
        float headCY = 1.62f - crouchHeadDrop;
        float handY  = 1.10f - crouchBodyDrop;

        glm::vec3 neck  = feetPos + worldUp * neckY  + bodyFwd * std::sin(crouchTilt) * 0.3f;
        glm::vec3 hip   = feetPos + worldUp * hipY;
        glm::vec3 headC = feetPos + worldUp * headCY + bodyFwd * std::sin(crouchTilt) * 0.35f;

        glm::vec3 footL = feetPos + bodyRight * (-0.20f) - bodyFwd * crouchLegBack;
        glm::vec3 footR = feetPos + bodyRight * ( 0.20f) - bodyFwd * crouchLegBack;
        glm::vec3 shoulderPos = neck - worldUp * 0.14f; // slightly below neck
        glm::vec3 shoulderL = shoulderPos + bodyRight * (-0.05f);
        glm::vec3 shoulderR = shoulderPos + bodyRight * ( 0.05f);
        glm::vec3 handL = feetPos + worldUp * handY + bodyRight * (-0.35f) + bodyFwd * std::sin(crouchTilt) * 0.2f;
        glm::vec3 handR = feetPos + worldUp * handY + bodyRight * ( 0.35f) + bodyFwd * std::sin(crouchTilt) * 0.2f;

        // The two legs' tops; standing, both hang from the one hip point.
        glm::vec3 legTopL = hip, legTopR = hip;
        if (isSitting) {
            // MC's pose, applied the way ModelPart.translateAndRotate does:
            // a part vector v in model space (pixels, +X = the body's left,
            // +Y = down, +Z = back) turns by Rz · Ry · Rx. The model frame is
            // bodyRight/worldUp/bodyFwd with every axis negated
            // (LivingEntityRenderer's rotate(180 − yBodyRot) + scale(−1,−1,1)).
            const auto toWorld = [&](const glm::vec3& m) {
                return -bodyRight * m.x - worldUp * m.y - bodyFwd * m.z;
            };
            const auto toModel = [&](const glm::vec3& w) {
                return glm::vec3(-glm::dot(w, bodyRight), -glm::dot(w, worldUp), -glm::dot(w, bodyFwd));
            };
            const auto rotZYX = [](glm::vec3 v, float xRot, float yRot, float zRot) {
                // Rx
                v = glm::vec3(v.x, v.y * std::cos(xRot) - v.z * std::sin(xRot),
                              v.y * std::sin(xRot) + v.z * std::cos(xRot));
                // Ry
                v = glm::vec3(v.x * std::cos(yRot) + v.z * std::sin(yRot), v.y,
                              -v.x * std::sin(yRot) + v.z * std::cos(yRot));
                // Rz
                return glm::vec3(v.x * std::cos(zRot) - v.y * std::sin(zRot),
                                 v.x * std::sin(zRot) + v.y * std::cos(zRot), v.z);
            };
            // HumanoidModel.setupAnim's isPassenger branch:
            //   rightLeg.xRot = −1.4137167, yRot = +π/10, zRot = +π/40
            //   leftLeg.xRot  = −1.4137167, yRot = −π/10, zRot = −π/40
            //   rightArm.xRot, leftArm.xRot += −π/5
            constexpr float kLegX = -1.4137167f, kLegY = 0.31415927f, kLegZ = 0.07853982f;
            constexpr float kArmX = -0.62831855f;
            // The legs hang from their pivots (PlayerModel: right_leg at
            // (−1.9, 12, 0), left_leg at (1.9, 12, 0)) — 12 px up, the hip —
            // and are 12 px long.
            constexpr float kPx = 1.0f / 16.0f;
            hip     = feetPos + worldUp * (12.0f * kPx);
            legTopR = hip + toWorld(glm::vec3(-1.9f * kPx, 0.0f, 0.0f));
            legTopL = hip + toWorld(glm::vec3( 1.9f * kPx, 0.0f, 0.0f));
            const glm::vec3 leg(0.0f, 12.0f * kPx, 0.0f);
            footR = legTopR + toWorld(rotZYX(leg, kLegX,  kLegY,  kLegZ));
            footL = legTopL + toWorld(rotZYX(leg, kLegX, -kLegY, -kLegZ));
            // The arms keep their standing hang, turned forward about the
            // shoulder by the added xRot.
            handR = shoulderR + toWorld(rotZYX(toModel(handR - shoulderR), kArmX, 0.0f, 0.0f));
            handL = shoulderL + toWorld(rotZYX(toModel(handL - shoulderL), kArmX, 0.0f, 0.0f));
        }

        sk.bodyFwd = bodyFwd; sk.bodyRight = bodyRight;
        sk.lookDir = lookDir; sk.faceRight = faceRight;
        sk.feetPos = feetPos;
        sk.neck = neck; sk.hip = hip; sk.headC = headC;
        sk.footL = footL; sk.footR = footR;
        sk.shoulderL = shoulderL; sk.shoulderR = shoulderR;
        sk.handL = handL; sk.handR = handR;
        sk.legTopL = legTopL; sk.legTopR = legTopR;
        sk.hipY = hipY;
        sk.isCrouching = isCrouching;
        return sk;
    }

    void BuildStickFigure(std::vector<StickVertex>& lineVerts,
                          std::vector<StickVertex>& ringTris,
                          std::vector<StickVertex>& discTris,
                          const glm::vec3& entityFeetPos,
                          float headYawDeg, float bodyYawDeg,
                          float pitchDeg, bool isCrouching,
                          PlayerColor color, bool isSitting) {
        BuildStickFigure(lineVerts, ringTris, discTris, entityFeetPos, headYawDeg, bodyYawDeg,
                         pitchDeg, isCrouching, StickFigureColors::Uniform(color), isSitting);
    }

    void BuildStickFigure(std::vector<StickVertex>& lineVerts,
                          std::vector<StickVertex>& ringTris,
                          std::vector<StickVertex>& discTris,
                          const glm::vec3& entityFeetPos,
                          float headYawDeg, float bodyYawDeg,
                          float /*pitchDeg*/, bool isCrouching,
                          const StickFigureColors& colors, bool isSitting) {
        using Part = Paint::Part;
        constexpr float PI = 3.14159265f;
        const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};

        // The joints — shared with StickFigureHand (a lead held in the hand).
        const StickFigureSkeleton sk =
            ComputeStickFigureSkeleton(entityFeetPos, headYawDeg, bodyYawDeg, isCrouching, isSitting);
        const glm::vec3 lookDir = sk.lookDir, faceRight = sk.faceRight;
        const glm::vec3 neck = sk.neck, hip = sk.hip, headC = sk.headC;
        const glm::vec3 footL = sk.footL, footR = sk.footR;
        const glm::vec3 shoulderL = sk.shoulderL, shoulderR = sk.shoulderR;
        const glm::vec3 handL = sk.handL, handR = sk.handR;
        const glm::vec3 legTopL = sk.legTopL, legTopR = sk.legTopR;

        // --- LINES: Body, legs, arms ---
        PushPartLine(lineVerts, neck, hip, colors, Part::Torso);
        if (isSitting) {
            // The pelvis between the two leg pivots, then each leg. It is the
            // torso's continuation, so it takes the torso's last cell.
            const int pelvis = Paint::FirstCell(Part::Torso) +
                               (colors.uniform ? 0 : Paint::CellCount(Part::Torso) - 1);
            PushLine(lineVerts, legTopL, legTopR, colors.cell[pelvis], pelvis);
        }
        PushPartLine(lineVerts, legTopL, footL, colors, Part::LeftLeg);
        PushPartLine(lineVerts, legTopR, footR, colors, Part::RightLeg);
        PushPartLine(lineVerts, shoulderL, handL, colors, Part::LeftArm);
        PushPartLine(lineVerts, shoulderR, handR, colors, Part::RightArm);

        // --- RING TRIANGLES: head outline + smile arc ---
        // Built as flat annular rings in the head's local plane, NOT as N
        // separate thick-line quads. There are no per-segment joins so there
        // are no possible gaps regardless of camera angle. The ring half-width
        // matches the body-line thickness used in the world renderer.
        constexpr int   kHeadCircleSegments = 64;
        constexpr int   kSmileSegments      = 32;
        // 0.025 m ≈ 1 px in the inventory preview (size=20 px/m). Smaller would
        // sub-pixel-rasterize as nothing in the GUI. In the world this gives a
        // ~5 cm full-width ring, slightly chunkier than the 3.6 cm body limbs
        // but visually consistent.
        constexpr float kRingHalfWidth      = 0.025f;
        const float headRadius = 0.18f;
        const glm::vec3 frontC = headC;

        // Angle 0 is +faceRight — the player's right — and the angle grows
        // toward worldUp: up over the crown, the HeadRing cells' order.
        PushArcRing(ringTris, frontC, faceRight, worldUp,
                    headRadius, kRingHalfWidth, kHeadCircleSegments,
                    0.0f, 2.0f * PI, colors,
                    [&](int i) { return ArcCell(colors, Part::HeadRing, i, kHeadCircleSegments); });

        // Smile (lower half of a small circle): π (the player's left) down
        // through the chin to 2π (their right).
        const glm::vec3 mouthC = frontC - worldUp * 0.04f;
        PushArcRing(ringTris, mouthC, faceRight, worldUp,
                    0.07f, kRingHalfWidth, kSmileSegments,
                    PI, 2.0f * PI, colors,
                    [&](int i) { return ArcCell(colors, Part::Smile, i, kSmileSegments); });

        // --- RING TRIANGLES: Eyes as tiny flat rings in the head's local plane ---
        // Eyes were originally 3D line segments along faceRight, but in the world
        // renderer that turns into a camera-facing thick strip whose perpendicular
        // depends on cross(lineDir, toCamera). When the camera moves to the side
        // of the player, that perpendicular rotates and the eye visibly "tilts"
        // from horizontal toward vertical. Drawing each eye as a tiny solid ring
        // in the same plane as the head outline keeps it locked to the face — it
        // simply foreshortens to a thin strip from the side, never tilts.
        const float eyeOffY = 0.04f, eyeOffX = 0.06f, eyeRad = 0.025f;
        glm::vec3 eyeL = frontC + worldUp * eyeOffY + faceRight * (-eyeOffX);
        glm::vec3 eyeR = frontC + worldUp * eyeOffY + faceRight * ( eyeOffX);
        const int eyeCellL = Paint::FirstCell(Part::Eyes);
        const int eyeCellR = eyeCellL + (colors.uniform ? 0 : 1);
        PushArcRing(ringTris, eyeL, faceRight, worldUp,
                    eyeRad, eyeRad, /*segments*/12, 0.0f, 2.0f * PI, colors,
                    [&](int) { return eyeCellL; });
        PushArcRing(ringTris, eyeR, faceRight, worldUp,
                    eyeRad, eyeRad, /*segments*/12, 0.0f, 2.0f * PI, colors,
                    [&](int) { return eyeCellR; });

        // --- TRIANGLES: Back-of-head filled disc (GPU face-culled) ---
        // Placed at headC (no offset) so it lines up with the neck/body connection.
        // Front features are offset forward, so they still render in front of this disc.
        // Match the front ring's 64-segment smoothness — the old 16-segment disc
        // showed visible polygonal sides next to the smooth front circle.
        constexpr int kDiscSegments = 64;
        PushDisc(discTris, headC, faceRight, worldUp, -lookDir,
                 headRadius, kDiscSegments, colors,
                 [&](int i) { return ArcCell(colors, Part::BackOfHead, i, kDiscSegments); });
    }

    glm::vec3 StickFigureHand(const glm::vec3& feetPos, float bodyYawDeg,
                              bool isCrouching, bool isSitting, bool rightHand) {
        const StickFigureSkeleton sk =
            ComputeStickFigureSkeleton(feetPos, bodyYawDeg, bodyYawDeg, isCrouching, isSitting);
        return rightHand ? sk.handR : sk.handL;
    }

} // namespace Render
