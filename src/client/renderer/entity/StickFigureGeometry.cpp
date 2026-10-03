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

    // ── The drawn figure ────────────────────────────────────────────────────

    namespace {

        using Drawing = Game::StickFigureDrawing;

        float PointSegmentDistance(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b) {
            const glm::vec2 ab = b - a;
            const float len2 = glm::dot(ab, ab);
            const float t = len2 > 1e-12f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
            return glm::length(p - (a + ab * t));
        }

        float Cross2(const glm::vec2& a, const glm::vec2& b) { return a.x * b.y - a.y * b.x; }

        // The distance between segments [a, b] and [c, d] (0 where they cross).
        float SegmentDistance(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c, const glm::vec2& d) {
            const float d1 = Cross2(b - a, c - a), d2 = Cross2(b - a, d - a);
            const float d3 = Cross2(d - c, a - c), d4 = Cross2(d - c, b - c);
            if (((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f)) &&
                ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f))) {
                return 0.0f;
            }
            return std::min(std::min(PointSegmentDistance(a, c, d), PointSegmentDistance(b, c, d)),
                            std::min(PointSegmentDistance(c, a, b), PointSegmentDistance(d, a, b)));
        }

        // One stroke in figure space, with its ink's bounds.
        struct FigureStroke {
            std::vector<glm::vec2> path;
            float radius = 0.0f;
            glm::vec2 lo{0.0f}, hi{0.0f};
        };

        // Whether two strokes' ink touches anywhere.
        bool StrokesTouch(const FigureStroke& s, const FigureStroke& t) {
            const float reach = s.radius + t.radius;
            const size_t ns = std::max<size_t>(1, s.path.size() - 1), nt = std::max<size_t>(1, t.path.size() - 1);
            for (size_t i = 0; i < ns; ++i) {
                const glm::vec2& a = s.path[i];
                const glm::vec2& b = s.path[std::min(i + 1, s.path.size() - 1)];
                // This segment against the other stroke's bounds first.
                const glm::vec2 lo = glm::min(a, b) - glm::vec2(reach), hi = glm::max(a, b) + glm::vec2(reach);
                if (hi.x < t.lo.x + t.radius || lo.x > t.hi.x - t.radius ||
                    hi.y < t.lo.y + t.radius || lo.y > t.hi.y - t.radius) {
                    continue;
                }
                for (size_t j = 0; j < nt; ++j) {
                    const glm::vec2& c = t.path[j];
                    const glm::vec2& d = t.path[std::min(j + 1, t.path.size() - 1)];
                    if (SegmentDistance(a, b, c, d) < reach) return true;
                }
            }
            return false;
        }

        // Liang–Barsky: the part of [p, q] inside [lo, hi], as t0 .. t1.
        bool ClipSegment(const glm::vec2& p, const glm::vec2& q, const glm::vec2& lo, const glm::vec2& hi,
                         float& t0, float& t1) {
            t0 = 0.0f;
            t1 = 1.0f;
            const glm::vec2 d = q - p;
            const float pk[4] = { -d.x, d.x, -d.y, d.y };
            const float qk[4] = { p.x - lo.x, hi.x - p.x, p.y - lo.y, hi.y - p.y };
            for (int k = 0; k < 4; ++k) {
                if (std::abs(pk[k]) < 1e-12f) {
                    if (qk[k] < 0.0f) return false;
                    continue;
                }
                const float r = qk[k] / pk[k];
                if (pk[k] < 0.0f) t0 = std::max(t0, r);
                else              t1 = std::min(t1, r);
                if (t0 > t1) return false;
            }
            return true;
        }

        // Calls run(points) for each stretch of `path` inside [lo, hi] (two
        // or more points; a single point inside is a dot, one point).
        template <class Run>
        void ForEachInside(const std::vector<glm::vec2>& path, const glm::vec2& lo, const glm::vec2& hi, Run&& run) {
            const auto inside = [&](const glm::vec2& p) {
                return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y;
            };
            if (path.size() == 1) {
                if (inside(path[0])) run(path);
                return;
            }
            std::vector<glm::vec2> current;
            for (size_t i = 0; i + 1 < path.size(); ++i) {
                float t0 = 0.0f, t1 = 1.0f;
                if (!ClipSegment(path[i], path[i + 1], lo, hi, t0, t1)) {
                    if (current.size() >= 2) run(current);
                    current.clear();
                    continue;
                }
                const glm::vec2 a = glm::mix(path[i], path[i + 1], t0);
                const glm::vec2 b = glm::mix(path[i], path[i + 1], t1);
                // A cut start begins a new stretch.
                if (t0 > 0.0f || current.empty()) {
                    if (current.size() >= 2) run(current);
                    current.assign(1, a);
                }
                current.push_back(b);
                if (t1 < 1.0f) {
                    run(current);
                    current.clear();
                }
            }
            if (current.size() >= 2) run(current);
        }

    } // namespace

    void BuildDrawingMesh(const Game::StickFigureDrawing& drawing, DrawingMesh& out, float fromHeight) {
        out.segments.clear();
        out.top = 0.0f;
        if (drawing.Empty()) return;
        const size_t n = drawing.strokes.size();

        // Every stroke in figure space.
        std::vector<FigureStroke> strokes(n);
        for (size_t i = 0; i < n; ++i) {
            const Drawing::Stroke& src = drawing.strokes[i];
            FigureStroke& s = strokes[i];
            s.radius = Drawing::RadiusBlocks(src.radius);
            s.path.reserve(src.points.size());
            for (const Drawing::Point& p : src.points) {
                s.path.emplace_back(Drawing::FigureX(static_cast<float>(p.x)), Drawing::FigureY(static_cast<float>(p.y)));
            }
            if (s.path.empty()) continue;
            s.lo = s.hi = s.path.front();
            for (const glm::vec2& p : s.path) {
                s.lo = glm::min(s.lo, p);
                s.hi = glm::max(s.hi, p);
            }
            s.lo -= glm::vec2(s.radius);
            s.hi += glm::vec2(s.radius);
        }

        // The layers: one above every earlier stroke of another colour this
        // one touches (a later stroke of the same colour may share a layer —
        // its overlap is one colour either way).
        std::vector<uint8_t> layers(n, 0);
        for (size_t i = 0; i < n; ++i) {
            const FigureStroke& s = strokes[i];
            if (s.path.empty()) continue;
            int layer = 0;
            for (size_t j = 0; j < i; ++j) {
                const FigureStroke& t = strokes[j];
                if (t.path.empty() || layers[j] + 1 <= layer) continue;
                if (drawing.strokes[j].color == drawing.strokes[i].color) continue;
                if (s.hi.x < t.lo.x || s.lo.x > t.hi.x || s.hi.y < t.lo.y || s.lo.y > t.hi.y) continue;
                if (StrokesTouch(s, t)) layer = layers[j] + 1;
            }
            layers[i] = static_cast<uint8_t>(std::min(layer, kDrawingMaxLayer));
        }

        // The segments, from `fromHeight` up.
        const glm::vec2 lo(-1.0e6f, fromHeight), hi(1.0e6f, 1.0e6f);
        for (size_t i = 0; i < n; ++i) {
            const FigureStroke& s = strokes[i];
            if (s.path.empty()) continue;
            const uint8_t color = static_cast<uint8_t>(drawing.strokes[i].color);
            ForEachInside(s.path, lo, hi, [&](const std::vector<glm::vec2>& run) {
                const size_t segments = std::max<size_t>(1, run.size() - 1);
                for (size_t k = 0; k < segments; ++k) {
                    DrawingSegment seg;
                    seg.a = run[k];
                    seg.b = run[std::min(k + 1, run.size() - 1)];
                    seg.radius = s.radius;
                    seg.color = color;
                    seg.layer = layers[i];
                    seg.capEnd = run.size() > 1 && k + 1 == segments;
                    out.top = std::max(out.top, std::max(seg.a.y, seg.b.y) + s.radius);
                    out.segments.push_back(seg);
                }
            });
        }
    }

    void AppendDrawingLines(std::vector<StickVertex>& out, const DrawingMesh& mesh,
                            const glm::vec3& feetPos, float bodyYawDeg, bool isCrouching,
                            const PlayerColor* palette) {
        constexpr int kColors = static_cast<int>(Game::PlayerColorId::Count);
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        // The figure frame at this body yaw — ComputeStickFigureSkeleton's.
        const glm::vec3 fwd = Game::Mth::HorizontalViewVector(bodyYawDeg);
        const glm::vec3 right = glm::normalize(glm::cross(fwd, up));
        const float sy = isCrouching ? kDrawingCrouchScaleY : 1.0f;
        const auto place = [&](const glm::vec2& f) { return feetPos + right * f.x + up * (f.y * sy); };
        out.reserve(out.size() + mesh.segments.size() * 2);
        for (const DrawingSegment& seg : mesh.segments) {
            const PlayerColor& c = palette[seg.color < kColors ? seg.color : 0];
            const float tag = DrawingStrokeTag(seg.layer, seg.capEnd);
            const glm::vec3 a = place(seg.a), b = place(seg.b);
            out.push_back(StickVertex{ a.x, a.y, a.z, seg.radius, tag, c.r, c.g, c.b, c.a });
            out.push_back(StickVertex{ b.x, b.y, b.z, seg.radius, tag, c.r, c.g, c.b, c.a });
        }
    }

    void AppendDrawingStrokeTriangles(const StickVertex& va, const StickVertex& vb, const glm::vec3& eye,
                                      std::vector<StickVertex>& out) {
        const float r = va.u;
        if (!(r > 0.0f)) return;
        const int code = std::max(0, static_cast<int>(-va.v + 0.5f) - 1);
        const float pull = static_cast<float>(code >> 1) * kDrawingLayerStep;
        const bool capEnd = (code & 1) != 0;
        const auto put = [&](glm::vec3 p) {
            p += (eye - p) * pull;
            out.push_back(StickVertex{ p.x, p.y, p.z, 0.0f, 0.0f, va.r, va.g, va.b, va.a });
        };
        // A camera-facing disc: the round joint or end.
        const auto disc = [&](const glm::vec3& c) {
            glm::vec3 n = eye - c;
            if (glm::dot(n, n) < 1e-12f) return;
            n = glm::normalize(n);
            const glm::vec3 helper = std::abs(n.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 ax = glm::normalize(glm::cross(helper, n)) * r;
            const glm::vec3 ay = glm::cross(n, ax);
            // More sides for a bigger brush (a giant's too): 9 at the limbs'
            // width, 20 at the largest — under 2 mm from the true circle.
            // A fan from the first corner: sides − 2 triangles.
            const int sides = std::clamp(8 + static_cast<int>(r * 80.0f), 8, 20);
            const float step = 2.0f * 3.14159265f / static_cast<float>(sides);
            const float cs = std::cos(step), sn = std::sin(step);
            const glm::vec3 first = c + ax;
            float x0 = cs, y0 = sn;
            for (int i = 1; i + 1 < sides; ++i) {
                const float x1 = x0 * cs - y0 * sn, y1 = x0 * sn + y0 * cs;
                put(first);
                put(c + ax * x0 + ay * y0);
                put(c + ax * x1 + ay * y1);
                x0 = x1;
                y0 = y1;
            }
        };
        const glm::vec3 a(va.x, va.y, va.z), b(vb.x, vb.y, vb.z);
        const glm::vec3 d = b - a;
        if (glm::dot(d, d) > 1e-12f) {
            // The ribbon: the limbs' strip, the stroke's width, no miter
            // (the discs close the joints).
            glm::vec3 perp = glm::cross(d, eye - (a + b) * 0.5f);
            if (glm::dot(perp, perp) > 1e-12f) {
                perp = glm::normalize(perp) * r;
                put(a + perp); put(a - perp); put(b - perp);
                put(a + perp); put(b - perp); put(b + perp);
            }
        }
        disc(a);
        if (capEnd) disc(b);
    }

    void StickFigureStrokes(const Game::StickFigurePaint& paint, Game::StickFigureDrawing& out) {
        using Part = Paint::Part;
        constexpr float PI = 3.14159265f;
        out.Clear();

        // The figure standing at body yaw 0, in figure space: X the player's
        // right (the skeleton's bodyRight), Y up.
        const StickFigureSkeleton sk = ComputeStickFigureSkeleton(glm::vec3(0.0f), 0.0f, 0.0f,
                                                                  /*isCrouching=*/false, /*isSitting=*/false);
        const auto fig = [&](const glm::vec3& p) { return glm::vec2(glm::dot(p, sk.bodyRight), p.y); };

        // A path in figure space as strokes, cut to the canvas less the
        // brush's radius so the whole stroke stays on it.
        const auto addPath = [&](const std::vector<glm::vec2>& path, Game::PlayerColorId color, int radius) {
            const float r = Drawing::RadiusBlocks(radius);
            const glm::vec2 lo(-Drawing::kWidthBlocks * 0.5f + r, r);
            const glm::vec2 hi(Drawing::kWidthBlocks * 0.5f - r, Drawing::kHeightBlocks - r);
            ForEachInside(path, lo, hi, [&](const std::vector<glm::vec2>& run) {
                if (out.StrokeCount() >= Drawing::kMaxStrokes) return;
                Drawing::Stroke s;
                s.color = color;
                s.radius = static_cast<uint8_t>(radius);
                for (const glm::vec2& p : run) {
                    const Drawing::Point q = Drawing::ToPoint(
                        (Drawing::kWidthBlocks * 0.5f - p.x) * static_cast<float>(Drawing::kGridPerBlock),
                        p.y * static_cast<float>(Drawing::kGridPerBlock));
                    if (s.points.empty() || s.points.back() != q || run.size() == 1) s.points.push_back(q);
                }
                out.strokes.push_back(std::move(s));
            });
        };

        // A limb: one stroke per run of its cells in one colour (cell 0 at `a`).
        const auto limb = [&](const glm::vec3& a, const glm::vec3& b, Part part) {
            const int first = Paint::FirstCell(part), n = Paint::CellCount(part);
            const glm::vec2 fa = fig(a), fb = fig(b);
            for (int i = 0; i < n;) {
                int j = i + 1;
                while (j < n && paint.At(first + j) == paint.At(first + i)) ++j;
                addPath({ glm::mix(fa, fb, static_cast<float>(i) / static_cast<float>(n)),
                          glm::mix(fa, fb, static_cast<float>(j) / static_cast<float>(n)) },
                        paint.At(first + i), Drawing::kLineRadius);
                i = j;
            }
        };
        // An arc from angle a0 to a1 (0 = the player's right, growing toward
        // up — BuildStickFigure's), `segmentsPerCell` straight pieces per
        // cell; a whole circle starts its runs at a colour change so a run
        // across the start stays one stroke.
        const auto arc = [&](const glm::vec2& c, float radius, float a0, float a1, Part part, int segmentsPerCell) {
            const int first = Paint::FirstCell(part), n = Paint::CellCount(part);
            const bool closed = std::abs((a1 - a0) - 2.0f * PI) < 1e-4f;
            const int total = n * segmentsPerCell;
            const auto at = [&](int k) {
                const float angle = a0 + (a1 - a0) * static_cast<float>(k) / static_cast<float>(total);
                return c + glm::vec2(std::cos(angle), std::sin(angle)) * radius;
            };
            const auto colorOf = [&](int cell) { return paint.At(first + ((cell % n) + n) % n); };
            int start = 0;
            bool uniform = true;
            for (int i = 1; i < n; ++i) uniform = uniform && colorOf(i) == colorOf(0);
            if (uniform) {
                std::vector<glm::vec2> path;
                for (int k = 0; k <= total; ++k) path.push_back(at(k));
                addPath(path, colorOf(0), Drawing::kRingRadius);
                return;
            }
            if (closed) {
                while (colorOf(start) == colorOf(start - 1)) ++start;
            }
            const int end = closed ? start + n : n;
            for (int i = start; i < end;) {
                int j = i + 1;
                while (j < end && colorOf(j) == colorOf(i)) ++j;
                std::vector<glm::vec2> path;
                for (int k = i * segmentsPerCell; k <= j * segmentsPerCell; ++k) path.push_back(at(k));
                addPath(path, colorOf(i), Drawing::kRingRadius);
                i = j;
            }
        };

        // The limbs, then the head over them — BuildStickFigure's order.
        limb(sk.neck, sk.hip, Part::Torso);
        limb(sk.legTopL, sk.footL, Part::LeftLeg);
        limb(sk.legTopR, sk.footR, Part::RightLeg);
        limb(sk.shoulderL, sk.handL, Part::LeftArm);
        limb(sk.shoulderR, sk.handR, Part::RightArm);
        const glm::vec2 head = fig(sk.headC);
        arc(head, 0.18f, 0.0f, 2.0f * PI, Part::HeadRing, 3);
        arc(head - glm::vec2(0.0f, 0.04f), 0.07f, PI, 2.0f * PI, Part::Smile, 4);
        // The eyes: BuildStickFigure's solid rings (0.025 ± 0.025), as dots.
        const int eyeCell = Paint::FirstCell(Part::Eyes);
        constexpr int kEyeRadius = 50;
        addPath({ head + glm::vec2(-0.06f, 0.04f) }, paint.At(eyeCell), kEyeRadius);
        addPath({ head + glm::vec2(0.06f, 0.04f) }, paint.At(eyeCell + 1), kEyeRadius);
    }

} // namespace Render
