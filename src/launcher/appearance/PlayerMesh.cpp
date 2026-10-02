// File: src/launcher/appearance/PlayerMesh.cpp
#include "PlayerMesh.hpp"
#include "client/renderer/entity/StickFigureGeometry.hpp"
#include "common/entity/PlayerModelLayout.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <limits>

namespace Launcher::Appearance {

    namespace {

        namespace PL = Game::PlayerLayout;

        // Model space (MC pixels, Y down, facing −Z) → preview space (blocks,
        // Y up, facing +Z, feet at 0): LivingEntityRenderer's rotateY(180)
        // and scale(−1, −1, 1) fold into a flip of Y and Z, and the model's
        // feet sit 24 px below its origin.
        const glm::mat4& ModelToPreview() {
            static const glm::mat4 m =
                glm::scale(glm::mat4(1.0f), glm::vec3(1.0f / 16.0f, -1.0f / 16.0f, -1.0f / 16.0f)) *
                glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -24.0f, 0.0f));
            return m;
        }

        // MC ModelPart.translateAndRotate: translate, then Z, Y, X.
        glm::mat4 PartMatrix(const glm::vec3& offset, float xRot, float yRot, float zRot) {
            glm::mat4 m = glm::translate(glm::mat4(1.0f), offset);
            if (zRot != 0.0f) m = glm::rotate(m, zRot, glm::vec3(0.0f, 0.0f, 1.0f));
            if (yRot != 0.0f) m = glm::rotate(m, yRot, glm::vec3(0.0f, 1.0f, 0.0f));
            if (xRot != 0.0f) m = glm::rotate(m, xRot, glm::vec3(1.0f, 0.0f, 0.0f));
            return m;
        }

        // One box, as ModelPart.cpp's BuildCube emits it (the non-flat path):
        // the same corner order and UV assignment per face.
        void EmitBox(const PL::Box& box, const glm::mat4& transform, bool outer,
                     std::vector<MeshTri>& out) {
            const float g = box.grow;
            const float minX = box.ox - g, minY = box.oy - g, minZ = box.oz - g;
            const float maxX = box.ox + box.sx + g, maxY = box.oy + box.sy + g, maxZ = box.oz + box.sz + g;

            const glm::vec3 t0(minX, minY, minZ), t1(maxX, minY, minZ);
            const glm::vec3 t2(maxX, maxY, minZ), t3(minX, maxY, minZ);
            const glm::vec3 l0(minX, minY, maxZ), l1(maxX, minY, maxZ);
            const glm::vec3 l2(maxX, maxY, maxZ), l3(minX, maxY, maxZ);

            // UV columns from the UNINFLATED size.
            const float w = box.sx, h = box.sy, d = box.sz;
            const float u0 = static_cast<float>(box.texU);
            const float u1 = u0 + d, u2 = u0 + d + w, u22 = u0 + d + w + w;
            const float u3 = u0 + d + w + d, u4 = u0 + d + w + d + w;
            const float v0 = static_cast<float>(box.texV);
            const float v1 = v0 + d, v2 = v0 + d + h;

            const glm::mat4 world = ModelToPreview() * transform;
            const glm::mat3 normalMat = glm::mat3(world);

            const auto emit = [&](const glm::vec3 (&q)[4], float U0, float V0, float U1, float V1,
                                  const glm::vec3& modelNormal) {
                // Vertex 0 takes the HIGH u (BuildCube's note).
                const glm::vec2 uv[4] = { {U1, V0}, {U0, V0}, {U0, V1}, {U1, V1} };
                glm::vec3 p[4];
                for (int i = 0; i < 4; ++i) p[i] = glm::vec3(world * glm::vec4(q[i], 1.0f));
                const glm::vec3 n = glm::normalize(normalMat * modelNormal);
                MeshTri a, b;
                a.p[0] = p[0]; a.p[1] = p[1]; a.p[2] = p[2];
                a.uv[0] = uv[0]; a.uv[1] = uv[1]; a.uv[2] = uv[2];
                b.p[0] = p[0]; b.p[1] = p[2]; b.p[2] = p[3];
                b.uv[0] = uv[0]; b.uv[1] = uv[2]; b.uv[2] = uv[3];
                const int rx0 = static_cast<int>(std::min(U0, U1)), rx1 = static_cast<int>(std::max(U0, U1));
                const int ry0 = static_cast<int>(std::min(V0, V1)), ry1 = static_cast<int>(std::max(V0, V1));
                for (MeshTri* t : { &a, &b }) {
                    t->n = n;
                    t->rect[0] = rx0; t->rect[1] = ry0; t->rect[2] = rx1; t->rect[3] = ry1;
                    t->outer = outer;
                    out.push_back(*t);
                }
            };

            { const glm::vec3 q[4] = { l1, l0, t0, t1 }; emit(q, u1, v0, u2,  v1, glm::vec3(0, -1, 0)); }
            { const glm::vec3 q[4] = { t2, t3, l3, l2 }; emit(q, u2, v1, u22, v0, glm::vec3(0,  1, 0)); }
            { const glm::vec3 q[4] = { t0, l0, l3, t3 }; emit(q, u0, v1, u1,  v2, glm::vec3(-1, 0, 0)); }
            { const glm::vec3 q[4] = { l1, t1, t2, l2 }; emit(q, u2, v1, u3,  v2, glm::vec3(1,  0, 0)); }
            { const glm::vec3 q[4] = { t1, t0, t3, t2 }; emit(q, u1, v1, u2,  v2, glm::vec3(0,  0, -1)); }
            { const glm::vec3 q[4] = { l0, l1, l2, l3 }; emit(q, u3, v1, u4,  v2, glm::vec3(0,  0, 1)); }
        }

        // The pose of each root part (MC HumanoidModel's idle, with the
        // head turned by the preview).
        glm::mat4 PosedPart(PL::Part part, const PlayerPose& pose) {
            const PL::Pivot& pv = PL::PivotOf(part);
            float xRot = 0.0f, yRot = 0.0f, zRot = 0.0f;
            switch (part) {
                case PL::Part::Head:
                    xRot = glm::radians(pose.headPitchDeg);
                    yRot = glm::radians(pose.headYawDeg);
                    break;
                case PL::Part::RightArm:
                case PL::Part::LeftArm:
                    if (pose.idleSway) {
                        // AnimationUtils.bobModelPart(arm, ageInTicks, ±1).
                        const float s = part == PL::Part::RightArm ? 1.0f : -1.0f;
                        zRot += s * (std::cos(pose.ageTicks * 0.09f) * 0.05f + 0.05f);
                        xRot += s * std::sin(pose.ageTicks * 0.067f) * 0.05f;
                    }
                    break;
                case PL::Part::RightLeg: yRot = 0.005f; zRot = 0.005f; break;
                case PL::Part::LeftLeg:  yRot = -0.005f; zRot = -0.005f; break;
                default: break;
            }
            return PartMatrix(glm::vec3(pv.x, pv.y, pv.z), xRot, yRot, zRot);
        }

        PreviewVertex Vertex(const glm::vec3& p, const glm::vec2& uv, const glm::vec3& n, uint32_t rgba) {
            PreviewVertex v{};
            v.x = p.x; v.y = p.y; v.z = p.z;
            v.u = uv.x; v.v = uv.y;
            v.nx = n.x; v.ny = n.y; v.nz = n.z;
            v.r = static_cast<uint8_t>(rgba >> 24);
            v.g = static_cast<uint8_t>(rgba >> 16);
            v.b = static_cast<uint8_t>(rgba >> 8);
            v.a = static_cast<uint8_t>(rgba);
            return v;
        }

    } // namespace

    void BuildPlayerMesh(Game::SkinModel model, bool showOuter, uint8_t modelParts,
                         const PlayerPose& pose, std::vector<MeshTri>& out) {
        const PL::Box* boxes = PL::Boxes(model);
        for (size_t i = 0; i < PL::kBoxCount; ++i) {
            const PL::Box& box = boxes[i];
            if (box.outer && (!showOuter || (box.partBit != 0 && (modelParts & box.partBit) == 0))) continue;
            EmitBox(box, PosedPart(box.part, pose), box.outer, out);
        }
    }

    void BuildCapeMesh(const PlayerPose& pose, std::vector<MeshTri>& out) {
        // The body part, then PlayerCapeModel's cape: offset (0, 0, 2), yRot π
        // composed with setupAnim's rotateBy — at rest Rx(6°)·Ry(π).
        const glm::mat4 body = PosedPart(PL::Part::Body, pose);
        glm::mat4 cape = glm::translate(body, glm::vec3(PL::kCapeOffset.x, PL::kCapeOffset.y, PL::kCapeOffset.z));
        cape = glm::rotate(cape, glm::radians(6.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        cape = glm::rotate(cape, PL::kCapeYRot, glm::vec3(0.0f, 1.0f, 0.0f));
        EmitBox(PL::kCape, cape, false, out);
    }

    void AppendMeshVertices(const std::vector<MeshTri>& tris, float texW, float texH,
                            std::vector<PreviewVertex>& out, uint32_t rgba) {
        out.reserve(out.size() + tris.size() * 3);
        for (const MeshTri& t : tris) {
            for (int i = 0; i < 3; ++i) {
                out.push_back(Vertex(t.p[i], glm::vec2(t.uv[i].x / texW, t.uv[i].y / texH), t.n, rgba));
            }
        }
    }

    bool RayTriangle(const glm::vec3& o, const glm::vec3& d, const glm::vec3& a, const glm::vec3& b,
                     const glm::vec3& c, bool frontOnly, float& t, float& u, float& v) {
        const glm::vec3 e1 = b - a, e2 = c - a;
        const glm::vec3 p = glm::cross(d, e2);
        const float det = glm::dot(e1, p);
        if (frontOnly) {
            if (det < 1e-9f) return false;
        } else if (std::fabs(det) < 1e-9f) {
            return false;
        }
        const float inv = 1.0f / det;
        const glm::vec3 s = o - a;
        u = glm::dot(s, p) * inv;
        if (u < 0.0f || u > 1.0f) return false;
        const glm::vec3 q = glm::cross(s, e1);
        v = glm::dot(d, q) * inv;
        if (v < 0.0f || u + v > 1.0f) return false;
        t = glm::dot(e2, q) * inv;
        return t > 0.0f;
    }

    TexelHit PickTexel(const std::vector<MeshTri>& tris, const glm::vec3& origin, const glm::vec3& dir,
                       const Game::SkinImage* skipClearOuterOf) {
        TexelHit best;
        best.t = std::numeric_limits<float>::max();
        for (size_t i = 0; i < tris.size(); ++i) {
            const MeshTri& tri = tris[i];
            float t, u, v;
            if (!RayTriangle(origin, dir, tri.p[0], tri.p[1], tri.p[2], false, t, u, v)) continue;
            if (t >= best.t) continue;
            const glm::vec2 uv = tri.uv[0] * (1.0f - u - v) + tri.uv[1] * u + tri.uv[2] * v;
            // The texel, clamped into the face (an edge hit can round out).
            const int x = std::clamp(static_cast<int>(std::floor(uv.x)), tri.rect[0], std::max(tri.rect[0], tri.rect[2] - 1));
            const int y = std::clamp(static_cast<int>(std::floor(uv.y)), tri.rect[1], std::max(tri.rect[1], tri.rect[3] - 1));
            if (skipClearOuterOf && tri.outer && skipClearOuterOf->Valid() &&
                x < skipClearOuterOf->width && y < skipClearOuterOf->height &&
                skipClearOuterOf->PixelPtr(x, y)[3] == 0) {
                continue;
            }
            best.hit = true;
            best.t = t;
            best.x = x;
            best.y = y;
            best.tri = static_cast<int>(i);
        }
        return best;
    }

    std::vector<FaceRect> SkinFaceRects(Game::SkinModel model) {
        std::vector<FaceRect> rects;
        const PL::Box* boxes = PL::Boxes(model);
        for (size_t i = 0; i < PL::kBoxCount; ++i) {
            const PL::Box& b = boxes[i];
            const int u = b.texU, v = b.texV;
            const int w = static_cast<int>(b.sx), h = static_cast<int>(b.sy), d = static_cast<int>(b.sz);
            const FaceRect faces[6] = {
                { u + d,         v,     u + d + w,         v + d,     b.outer, b.name },   // top
                { u + d + w,     v,     u + d + w + w,     v + d,     b.outer, b.name },   // bottom
                { u,             v + d, u + d,             v + d + h, b.outer, b.name },   // right
                { u + d,         v + d, u + d + w,         v + d + h, b.outer, b.name },   // front
                { u + d + w,     v + d, u + d + w + d,     v + d + h, b.outer, b.name },   // left
                { u + d + w + d, v + d, u + d + w + d + w, v + d + h, b.outer, b.name },   // back
            };
            rects.insert(rects.end(), std::begin(faces), std::end(faces));
        }
        return rects;
    }

    const FaceRect* FaceRectAt(const std::vector<FaceRect>& rects, int x, int y) {
        for (const FaceRect& r : rects) {
            if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) return &r;
        }
        return nullptr;
    }

    // ── Stick figure ────────────────────────────────────────────────────────

    namespace {

        Render::StickFigureColors ColorsFor(const Game::StickFigurePaint& paint, bool uniformLook,
                                            int highlightCell) {
            Render::StickFigureColors colors = Render::StickFigureColors::FromPaint(
                paint, [](Render::PlayerColor c) { return c; });
            // The painter always shows the cells split, uniform or not.
            colors.uniform = uniformLook;
            if (highlightCell >= 0 && highlightCell < Game::StickFigurePaint::kCellCount) {
                Render::PlayerColor& h = colors.cell[highlightCell];
                const auto lift = [](uint8_t c) { return static_cast<uint8_t>(c + (255 - c) * 0.45f); };
                h.r = lift(h.r);
                h.g = lift(h.g);
                h.b = lift(h.b);
            }
            return colors;
        }

        void BuildRaw(const Game::StickFigurePaint& paint, bool uniformLook, int highlightCell,
                      std::vector<Render::StickVertex>& lines, std::vector<Render::StickVertex>& faces) {
            std::vector<Render::StickVertex> disc;
            const Render::StickFigureColors colors = ColorsFor(paint, uniformLook, highlightCell);
            Render::BuildStickFigure(lines, faces, disc, glm::vec3(0.0f), 0.0f, 0.0f, 0.0f,
                                     /*isCrouching=*/false, colors, /*isSitting=*/false);
            faces.insert(faces.end(), disc.begin(), disc.end());
        }

        // PlayerRenderer's EmitThickWorldStripFromLines: each line pair a
        // camera-facing quad of fixed world width, ends pushed out by the
        // 64-segment arcs' miter factor so chained segments close.
        template <class Emit>
        void ForEachStrip(const std::vector<Render::StickVertex>& lines, const glm::vec3& eye,
                          float halfWidth, Emit&& emit) {
            for (size_t i = 0; i + 1 < lines.size(); i += 2) {
                const Render::StickVertex& va = lines[i];
                const Render::StickVertex& vb = lines[i + 1];
                const glm::vec3 a(va.x, va.y, va.z), b(vb.x, vb.y, vb.z);
                const glm::vec3 d = b - a;
                if (glm::dot(d, d) < 1e-12f) continue;
                glm::vec3 perp = glm::cross(d, eye - (a + b) * 0.5f);
                if (glm::dot(perp, perp) < 1e-12f) continue;
                perp = glm::normalize(perp) * halfWidth;
                const glm::vec3 along = glm::normalize(d) * (halfWidth * 0.049f);
                const glm::vec3 ae = a - along, be = b + along;
                const glm::vec3 q[6] = { ae + perp, ae - perp, be - perp, ae + perp, be - perp, be + perp };
                emit(q, va, static_cast<int>(va.v + 0.5f));
            }
        }

    } // namespace

    void BuildStickMesh(const Game::StickFigurePaint& paint, bool uniformLook,
                        const glm::vec3& eye, float halfWidth, int highlightCell, StickMesh& out) {
        out.lines.clear();
        out.faces.clear();
        out.lineCells.clear();
        out.faceCells.clear();
        std::vector<Render::StickVertex> lines, faces;
        BuildRaw(paint, uniformLook, highlightCell, lines, faces);

        const auto toVertex = [](const glm::vec3& p, const Render::StickVertex& src) {
            PreviewVertex v{};
            v.x = p.x; v.y = p.y; v.z = p.z;
            v.r = src.r; v.g = src.g; v.b = src.b; v.a = src.a;
            return v;
        };
        ForEachStrip(lines, eye, halfWidth, [&](const glm::vec3 (&q)[6], const Render::StickVertex& src, int cell) {
            for (const glm::vec3& p : q) out.lines.push_back(toVertex(p, src));
            out.lineCells.push_back(cell);
            out.lineCells.push_back(cell);
        });
        for (size_t i = 0; i + 2 < faces.size(); i += 3) {
            for (int k = 0; k < 3; ++k) {
                const Render::StickVertex& s = faces[i + k];
                out.faces.push_back(toVertex(glm::vec3(s.x, s.y, s.z), s));
            }
            out.faceCells.push_back(static_cast<int>(faces[i].v + 0.5f));
        }
    }

    int PickStickCell(const Game::StickFigurePaint& paint, bool uniformLook,
                      const glm::vec3& eye, const glm::vec3& origin, const glm::vec3& dir) {
        std::vector<Render::StickVertex> lines, faces;
        BuildRaw(paint, uniformLook, -1, lines, faces);
        float bestT = std::numeric_limits<float>::max();
        int best = -1;
        float t, u, v;
        // The limbs, picked as strips a little wider than drawn.
        ForEachStrip(lines, eye, 0.045f, [&](const glm::vec3 (&q)[6], const Render::StickVertex&, int cell) {
            for (int k = 0; k < 6; k += 3) {
                if (RayTriangle(origin, dir, q[k], q[k + 1], q[k + 2], false, t, u, v) && t < bestT) {
                    bestT = t;
                    best = cell;
                }
            }
        });
        // The head's rings and the back disc, only from the side they show
        // (StickFigureGeometry winds them CCW toward their viewer).
        for (size_t i = 0; i + 2 < faces.size(); i += 3) {
            const glm::vec3 a(faces[i].x, faces[i].y, faces[i].z);
            const glm::vec3 b(faces[i + 1].x, faces[i + 1].y, faces[i + 1].z);
            const glm::vec3 c(faces[i + 2].x, faces[i + 2].y, faces[i + 2].z);
            // Rings lie a hair in front of the disc in nothing but draw order;
            // a slight bias lets the front features win a tie.
            if (RayTriangle(origin, dir, a, b, c, true, t, u, v) && t < bestT + 1e-4f) {
                bestT = t;
                best = static_cast<int>(faces[i].v + 0.5f);
            }
        }
        return best;
    }

} // namespace Launcher::Appearance
