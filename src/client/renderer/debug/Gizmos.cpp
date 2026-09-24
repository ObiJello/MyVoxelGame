// File: src/client/renderer/debug/Gizmos.cpp
#include "Gizmos.hpp"
#include "../backend/RenderBackend.hpp"
#include "../core/RenderOrigin.hpp"
#include "../gui/debug/DebugScreenOverlay.hpp"
#include "common/core/Log.hpp"
#include "common/world/block/Direction.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Render::Gizmos {

    namespace {
        struct Vertex { float x, y, z; float u, v; uint8_t r, g, b, a; };
        static_assert(sizeof(Vertex) == 24, "block vertex stride");

        // Points are stored in RENDER space (camera-relative, see
        // RenderOrigin.hpp): every submit subtracts the origin of the view
        // being drawn, in double, and Flush runs in that same view, so the
        // floats here are small however far from the world origin the
        // camera is.
        struct LineRec { glm::vec3 a, b; uint32_t color; float width; bool onTop; bool depthBias; };
        struct Tri { glm::vec3 p[3]; uint32_t color; };

        std::vector<LineRec> g_lines;
        std::vector<Tri>     g_fills;

        ShaderHandle  g_lineShader = INVALID_SHADER;
        ShaderHandle  g_fillShader = INVALID_SHADER;
        TextureHandle g_white = INVALID_TEXTURE;
        constexpr size_t kMaxLines = 1 << 16;           // 65k lines = 262k verts, 393k indices
        constexpr size_t kMaxFillVertices = 1 << 17;
        struct Frame {
            BufferHandle lineVB = INVALID_BUFFER, lineIB = INVALID_BUFFER; MeshHandle lineMesh = INVALID_MESH;
            BufferHandle fillVB = INVALID_BUFFER; MeshHandle fillMesh = INVALID_MESH;
        };
        Frame g_frames[2];
        int   g_frameParity = 0;
        std::vector<Vertex>   g_verts;
        std::vector<uint32_t> g_indices;

        // MC rendertype_lines.vsh on the block vertex layout (see lines_vk.vert
        // for the Vulkan twin and the encoding).
        const char* kLineVert = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform mat4 uMVP;
uniform vec2 uScreenSize;
uniform float uLineWidth;
out vec4 vColor;
vec3 decodeOctahedron(vec2 e) {
    vec3 n = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
void main() {
    vec3 dir = decodeOctahedron(aUV);
    vec4 linePosStart = uMVP * vec4(aPos, 1.0);
    vec4 linePosEnd   = uMVP * vec4(aPos + dir, 1.0);
    vec3 ndc1 = linePosStart.xyz / linePosStart.w;
    vec3 ndc2 = linePosEnd.xyz / linePosEnd.w;
    vec2 lineScreenDirection = normalize((ndc2.xy - ndc1.xy) * uScreenSize);
    vec2 lineOffset = vec2(-lineScreenDirection.y, lineScreenDirection.x) * uLineWidth / uScreenSize;
    if (lineOffset.x < 0.0) lineOffset *= -1.0;
    if (gl_VertexID % 2 == 0) gl_Position = vec4((ndc1 + vec3(lineOffset, 0.0)) * linePosStart.w, linePosStart.w);
    else                      gl_Position = vec4((ndc1 - vec3(lineOffset, 0.0)) * linePosStart.w, linePosStart.w);
    vColor = aColor;
}
)";
        const char* kLineFrag = R"(
#version 330 core
in vec4 vColor;
out vec4 FragColor;
void main() { FragColor = vColor; }
)";
        const char* kFillVert = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vWorldPos;
out vec4 vColor;
void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vWorldPos = worldPos.xyz;
    gl_Position = uMVP * worldPos;
    vColor = aColor;
}
)";
        const char* kFillFrag = R"(
#version 330 core
in vec3 vWorldPos;
in vec4 vColor;
out vec4 FragColor;
uniform vec4 uClipPlane;
void main() {
    if (any(notEqual(uClipPlane.xyz, vec3(0.0))) && dot(vWorldPos, uClipPlane.xyz) + uClipPlane.w < 0.0) discard;
    FragColor = vColor;
}
)";

        void Split(uint32_t argb, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
            a = static_cast<uint8_t>((argb >> 24) & 0xFF);
            r = static_cast<uint8_t>((argb >> 16) & 0xFF);
            g = static_cast<uint8_t>((argb >> 8) & 0xFF);
            b = static_cast<uint8_t>(argb & 0xFF);
        }

        // Unit vector → octahedron map, the inverse of the shader's decode.
        glm::vec2 EncodeOctahedron(glm::vec3 n) {
            const float l1 = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
            if (l1 < 1e-8f) return {0.0f, 0.0f};
            n /= l1;
            glm::vec2 p(n.x, n.y);
            if (n.z < 0.0f) {
                const glm::vec2 q(1.0f - std::abs(p.y), 1.0f - std::abs(p.x));
                p = { p.x >= 0.0f ? q.x : -q.x, p.y >= 0.0f ? q.y : -q.y };
            }
            return p;
        }

        // MC VIEW_SHRINK (the line shader's VIEW_SCALE) and the LINES render
        // type's VIEW_OFFSET_Z_LAYERING, both view-space scales.
        constexpr float kViewShrink = 1.0f - 1.0f / 256.0f;
        constexpr float kLayering   = 1.0f - 1.0f / 4096.0f;

        void SetDefaultPipeline() {
            PipelineState d;
            d.depthTestEnabled = true;
            d.depthWriteEnabled = true;
            d.blendEnabled = false;
            d.cullMode = CullMode::Back;
            g_renderBackend->SetPipelineState(d);
        }
    }

    bool Initialize() {
        if (!g_renderBackend) return false;
        if (g_lineShader != INVALID_SHADER) return true;
        g_lineShader = g_renderBackend->CreateShaderFromFiles("shaders/lines.vert", "shaders/lines.frag");
        if (g_lineShader == INVALID_SHADER) g_lineShader = g_renderBackend->CreateShader(kLineVert, kLineFrag);
        g_fillShader = g_renderBackend->CreateShaderFromFiles("shaders/player_billboard.vert", "shaders/player_billboard.frag");
        if (g_fillShader == INVALID_SHADER) g_fillShader = g_renderBackend->CreateShader(kFillVert, kFillFrag);
        if (g_lineShader == INVALID_SHADER || g_fillShader == INVALID_SHADER) {
            Log::Error("[Gizmos] shader creation failed (lines %u, fills %u)", g_lineShader, g_fillShader);
            return false;
        }
        unsigned char white[] = {255, 255, 255, 255};
        g_white = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, white);
        for (Frame& f : g_frames) {
            f.lineVB = g_renderBackend->CreateBuffer(BufferUsage::Vertex, kMaxLines * 4 * sizeof(Vertex), nullptr, BufferAccess::Streaming);
            f.lineIB = g_renderBackend->CreateBuffer(BufferUsage::Index, kMaxLines * 6 * sizeof(uint32_t), nullptr, BufferAccess::Streaming);
            f.lineMesh = g_renderBackend->CreateMesh(f.lineVB, f.lineIB, GetBlockVertexLayout());
            f.fillVB = g_renderBackend->CreateBuffer(BufferUsage::Vertex, kMaxFillVertices * sizeof(Vertex), nullptr, BufferAccess::Streaming);
            f.fillMesh = g_renderBackend->CreateMesh(f.fillVB, INVALID_BUFFER, GetBlockVertexLayout());
        }
        g_verts.reserve(65536);
        g_indices.reserve(98304);
        return true;
    }

    void Shutdown() {
        if (!g_renderBackend) return;
        for (Frame& f : g_frames) {
            if (f.lineMesh != INVALID_MESH) { g_renderBackend->DestroyMesh(f.lineMesh); f.lineMesh = INVALID_MESH; }
            if (f.fillMesh != INVALID_MESH) { g_renderBackend->DestroyMesh(f.fillMesh); f.fillMesh = INVALID_MESH; }
            if (f.lineVB != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(f.lineVB); f.lineVB = INVALID_BUFFER; }
            if (f.lineIB != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(f.lineIB); f.lineIB = INVALID_BUFFER; }
            if (f.fillVB != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(f.fillVB); f.fillVB = INVALID_BUFFER; }
        }
        if (g_white != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(g_white); g_white = INVALID_TEXTURE; }
        if (g_lineShader != INVALID_SHADER) { g_renderBackend->DestroyShader(g_lineShader); g_lineShader = INVALID_SHADER; }
        if (g_fillShader != INVALID_SHADER) { g_renderBackend->DestroyShader(g_fillShader); g_fillShader = INVALID_SHADER; }
        Clear();
    }

    void Line(const glm::dvec3& a, const glm::dvec3& b, uint32_t argb, float widthPx, bool alwaysOnTop, bool depthBias) {
        // World in, render space stored (see the LineRec note).
        if (g_lines.size() < kMaxLines) g_lines.push_back({ToRender(a), ToRender(b), argb, widthPx, alwaysOnTop, depthBias});
    }

    void Cuboid(const glm::dvec3& min, const glm::dvec3& max, uint32_t stroke, float widthPx, bool alwaysOnTop) {
        const glm::dvec3 c[8] = {
            {min.x, min.y, min.z}, {max.x, min.y, min.z}, {max.x, min.y, max.z}, {min.x, min.y, max.z},
            {min.x, max.y, min.z}, {max.x, max.y, min.z}, {max.x, max.y, max.z}, {min.x, max.y, max.z},
        };
        static const int e[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
        for (const auto& edge : e) Line(c[edge[0]], c[edge[1]], stroke, widthPx, alwaysOnTop);
    }

    void Quad(const glm::dvec3& p0, const glm::dvec3& p1, const glm::dvec3& p2, const glm::dvec3& p3, uint32_t fill) {
        if ((g_fills.size() + 2) * 3 > kMaxFillVertices) return;
        g_fills.push_back({{ToRender(p0), ToRender(p1), ToRender(p2)}, fill});
        g_fills.push_back({{ToRender(p0), ToRender(p2), ToRender(p3)}, fill});
    }

    void Rect(const glm::dvec3& a, const glm::dvec3& b, Game::Direction face, uint32_t fill) {
        const glm::dvec3 lo = glm::min(a, b), hi = glm::max(a, b);
        switch (face) {
            case Game::Direction::Down:  Quad({lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z}, fill); break;
            case Game::Direction::Up:    Quad({lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}, fill); break;
            case Game::Direction::North: Quad({lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z}, fill); break;
            case Game::Direction::South: Quad({lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}, fill); break;
            case Game::Direction::West:  Quad({lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z}, fill); break;
            case Game::Direction::East:  Quad({hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z}, fill); break;
            default: break;
        }
    }

    void CuboidFill(const glm::dvec3& min, const glm::dvec3& max, uint32_t fill) {
        Rect(min, max, Game::Direction::Down, fill);
        Rect(min, max, Game::Direction::Up, fill);
        Rect(min, max, Game::Direction::North, fill);
        Rect(min, max, Game::Direction::South, fill);
        Rect(min, max, Game::Direction::West, fill);
        Rect(min, max, Game::Direction::East, fill);
    }

    void Point(const glm::dvec3& p, uint32_t argb, float sizePx) {
        // A dot the size of the point: three tiny axis lines of that width.
        const double e = 0.005;
        Line(p - glm::dvec3(e, 0, 0), p + glm::dvec3(e, 0, 0), argb, sizePx, false);
        Line(p - glm::dvec3(0, e, 0), p + glm::dvec3(0, e, 0), argb, sizePx, false);
        Line(p - glm::dvec3(0, 0, e), p + glm::dvec3(0, 0, e), argb, sizePx, false);
    }

    void Arrow(const glm::dvec3& from, const glm::dvec3& to, uint32_t argb, float widthPx) {
        Line(from, to, argb, widthPx, false);
        const glm::dvec3 d = to - from;
        const double len = glm::length(d);
        if (len < 1e-6) return;
        const glm::dvec3 dir = d / len;
        const glm::dvec3 up = std::abs(dir.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
        const glm::dvec3 side = glm::normalize(glm::cross(dir, up));
        const glm::dvec3 side2 = glm::normalize(glm::cross(dir, side));
        const double head = std::min(0.25, len * 0.25);
        const glm::dvec3 base = to - dir * head;
        Line(to, base + side * head * 0.5, argb, widthPx, false);
        Line(to, base - side * head * 0.5, argb, widthPx, false);
        Line(to, base + side2 * head * 0.5, argb, widthPx, false);
        Line(to, base - side2 * head * 0.5, argb, widthPx, false);
    }

    void BillboardText(const std::string& text, const glm::dvec3& pos, uint32_t argb, float scale, bool centered, bool alwaysOnTop) {
        // Kept in WORLD double: the HUD pass projects it later, under the
        // main view's origin, and does the render-space subtraction there.
        DebugScreen::QueueBillboardText({pos.x, pos.y, pos.z, text, argb, scale, centered, alwaysOnTop});
    }

    // ── Shape outline: MC DiscreteVoxelShape.forAllAxisEdges ────────────
    void ShapeOutline(const ShapeBox* boxes, size_t count, const glm::dvec3& origin, uint32_t argb, float widthPx) {
        if (count == 0) return;
        // The grid is the boxes' own coordinates (MC's ArrayVoxelShape coords).
        std::vector<float> coords[3];
        auto addCoord = [](std::vector<float>& v, float c) {
            for (float x : v) if (std::abs(x - c) < 1e-4f) return;
            v.push_back(c);
        };
        for (size_t i = 0; i < count; ++i) {
            for (int ax = 0; ax < 3; ++ax) { addCoord(coords[ax], boxes[i].min[ax]); addCoord(coords[ax], boxes[i].max[ax]); }
        }
        for (auto& v : coords) std::sort(v.begin(), v.end());
        const int nx = static_cast<int>(coords[0].size()) - 1, ny = static_cast<int>(coords[1].size()) - 1, nz = static_cast<int>(coords[2].size()) - 1;
        if (nx <= 0 || ny <= 0 || nz <= 0) return;
        // Cell (x,y,z) is solid when its centre is inside any box.
        std::vector<uint8_t> solid(static_cast<size_t>(nx) * ny * nz, 0);
        auto cell = [&](int x, int y, int z) -> uint8_t& { return solid[(static_cast<size_t>(z) * ny + y) * nx + x]; };
        for (int z = 0; z < nz; ++z) for (int y = 0; y < ny; ++y) for (int x = 0; x < nx; ++x) {
            const float cx = (coords[0][x] + coords[0][x + 1]) * 0.5f;
            const float cy = (coords[1][y] + coords[1][y + 1]) * 0.5f;
            const float cz = (coords[2][z] + coords[2][z + 1]) * 0.5f;
            for (size_t i = 0; i < count; ++i) {
                const ShapeBox& b = boxes[i];
                if (cx > b.min.x && cx < b.max.x && cy > b.min.y && cy < b.max.y && cz > b.min.z && cz < b.max.z) { cell(x, y, z) = 1; break; }
            }
        }
        const int size[3] = {nx, ny, nz};
        auto isFullWide = [&](int x, int y, int z) {
            if (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz) return false;
            return cell(x, y, z) != 0;
        };
        // For each line axis `c`, the two cross axes (a, b) — MC's AxisCycle.
        for (int cAxis = 0; cAxis < 3; ++cAxis) {
            const int aAxis = (cAxis + 1) % 3, bAxis = (cAxis + 2) % 3;
            for (int a = 0; a <= size[aAxis]; ++a) {
                for (int b = 0; b <= size[bAxis]; ++b) {
                    int lastStart = -1;
                    for (int c = 0; c <= size[cAxis]; ++c) {
                        int fullSectors = 0, oddSectors = 0;
                        for (int da = 0; da <= 1; ++da) for (int db = 0; db <= 1; ++db) {
                            int p[3];
                            p[aAxis] = a + da - 1; p[bAxis] = b + db - 1; p[cAxis] = c;
                            if (isFullWide(p[0], p[1], p[2])) { ++fullSectors; oddSectors ^= da ^ db; }
                        }
                        const bool edge = fullSectors == 1 || fullSectors == 3 || (fullSectors == 2 && (oddSectors & 1) == 0);
                        if (edge) {
                            if (lastStart == -1) lastStart = c;
                        } else if (lastStart != -1) {
                            glm::dvec3 p0, p1;
                            p0[aAxis] = coords[aAxis][a]; p0[bAxis] = coords[bAxis][b]; p0[cAxis] = coords[cAxis][lastStart];
                            p1 = p0; p1[cAxis] = coords[cAxis][c];
                            Line(origin + p0, origin + p1, argb, widthPx, false);
                            lastStart = -1;
                        }
                    }
                }
            }
        }
    }

    void Clear() {
        g_lines.clear();
        g_fills.clear();
    }

    size_t QueuedVertexCount() { return g_lines.size() * 4 + g_fills.size() * 3; }

    void Flush(const glm::mat4& proj, const glm::mat4& view, const glm::vec3& cameraPos,
               float fovDeg, int fbWidth, int fbHeight) {
        (void)fovDeg; (void)cameraPos;
        if (!g_renderBackend || g_lineShader == INVALID_SHADER || (g_lines.empty() && g_fills.empty())) { Clear(); return; }
        Frame& frame = g_frames[g_frameParity];
        g_frameParity ^= 1;

        // MC: ProjMat * VIEW_SCALE * ModelViewMat, with the LINES render type's
        // VIEW_OFFSET_Z_LAYERING (scale 1 - 1/4096) applied to the model-view.
        const glm::mat4 layered = glm::scale(glm::mat4(1.0f), glm::vec3(kLayering)) * view;
        const glm::mat4 lineMvp = proj * glm::scale(glm::mat4(1.0f), glm::vec3(kViewShrink)) * layered;
        const glm::mat4 fillMvp = proj * layered;

        // ── Lines: one indexed draw per (depth mode, width) ───────────────
        if (!g_lines.empty()) {
            // Group key: on-top last, translucent after opaque; WITHIN a
            // class the lines keep their submission order, because a
            // renderer's later line must win over its earlier one at the
            // same depth (LessEqual): the 3D crosshair's coloured 2 px axes
            // over their black 4 px underlays (MC draws those with
            // LINES_DEPTH_BIAS for the same reason), the chunk border's
            // thick edges over its thin grid. Sorting by width here put the
            // black underlays on top and the crosshair came out black.
            std::stable_sort(g_lines.begin(), g_lines.end(), [](const LineRec& l, const LineRec& r) {
                const bool lt = ((l.color >> 24) & 0xFF) < 255, rt = ((r.color >> 24) & 0xFF) < 255;
                if (l.onTop != r.onTop) return !l.onTop;
                return !lt && rt;
            });
            g_verts.clear();
            g_indices.clear();
            struct Group { size_t firstIndex, indexCount; float width; bool onTop, translucent, depthBias; };
            std::vector<Group> groups;
            for (const LineRec& l : g_lines) {
                // MC GizmoFeatureRenderer.buildLines: a line is clipped to the
                // plane 0.05 in front of the camera. An endpoint behind the
                // eye has a negative clip w, and the shader's perspective
                // divide turns the strip inside out — the corner lines of
                // the chunk you stand in (ymin far below, ymax far above,
                // one of them behind you as you look up or down) flickered
                // and tore for exactly that reason. `view` is the render-
                // space view and the points are render-space, so the eye
                // depth read here is consistent (and exact far from the
                // world origin).
                glm::vec3 a = l.a, b = l.b;
                {
                    const float za = (view * glm::vec4(a, 1.0f)).z;
                    const float zb = (view * glm::vec4(b, 1.0f)).z;
                    const bool aBehind = za > -0.05f, bBehind = zb > -0.05f;
                    if (aBehind && bBehind) continue;
                    if (aBehind || bBehind) {
                        const float denom = zb - za;
                        if (std::abs(denom) < 1e-9f) continue;
                        const float t = std::clamp((-0.05f - za) / denom, 0.0f, 1.0f);
                        const glm::vec3 cut = a + (b - a) * t;
                        if (aBehind) a = cut; else b = cut;
                    }
                }
                const glm::vec3 d = b - a;
                if (glm::dot(d, d) < 1e-12f) continue;
                const bool translucent = ((l.color >> 24) & 0xFF) < 255;
                if (groups.empty() || groups.back().width != l.width || groups.back().onTop != l.onTop ||
                    groups.back().translucent != translucent || groups.back().depthBias != l.depthBias) {
                    groups.push_back({g_indices.size(), 0, l.width, l.onTop, translucent, l.depthBias});
                }
                const glm::vec2 enc = EncodeOctahedron(glm::normalize(d));
                uint8_t r, g, bl, al; Split(l.color, r, g, bl, al);
                const uint32_t base = static_cast<uint32_t>(g_verts.size());
                g_verts.push_back({a.x, a.y, a.z, enc.x, enc.y, r, g, bl, al});
                g_verts.push_back({a.x, a.y, a.z, enc.x, enc.y, r, g, bl, al});
                g_verts.push_back({b.x, b.y, b.z, enc.x, enc.y, r, g, bl, al});
                g_verts.push_back({b.x, b.y, b.z, enc.x, enc.y, r, g, bl, al});
                // Two triangles across the strip; the shader's parity test
                // needs the start vertices at even indices (base is even).
                const uint32_t idx[6] = {base + 0, base + 1, base + 3, base + 0, base + 3, base + 2};
                g_indices.insert(g_indices.end(), idx, idx + 6);
                groups.back().indexCount += 6;
            }
            if (!g_indices.empty()) {
                g_renderBackend->UpdateBufferUnsynchronized(frame.lineVB, 0, g_verts.size() * sizeof(Vertex), g_verts.data());
                g_renderBackend->UpdateBufferUnsynchronized(frame.lineIB, 0, g_indices.size() * sizeof(uint32_t), g_indices.data());
                g_renderBackend->BindShader(g_lineShader);
                g_renderBackend->BindTexture(g_white, 0);
                g_renderBackend->SetUniformMat4(g_lineShader, "uMVP", lineMvp);
                g_renderBackend->SetUniformVec2(g_lineShader, "uScreenSize", glm::vec2(static_cast<float>(fbWidth), static_cast<float>(fbHeight)));
                for (const Group& grp : groups) {
                    PipelineState state;
                    state.depthTestEnabled = !grp.onTop;
                    // RenderTypes.lines writes depth; linesTranslucentNoDepthWrite does not.
                    state.depthWriteEnabled = !grp.onTop && !grp.translucent;
                    // MC LINES_DEPTH_BIAS: DepthStencilState(GEQUAL, write, bias 1, slope 1)
                    // under reversed Z — toward the camera. This engine's depth
                    // runs the normal way, so the same nudge is negative.
                    state.depthBiasEnabled  = grp.depthBias;
                    state.depthBiasConstant = grp.depthBias ? -1.0f : 0.0f;
                    state.depthBiasSlope    = grp.depthBias ? -1.0f : 0.0f;
                    state.blendEnabled = true;
                    state.srcBlendFactor = BlendFactor::SrcAlpha;
                    state.dstBlendFactor = BlendFactor::OneMinusSrcAlpha;
                    state.cullMode = CullMode::None;
                    state.primitiveType = PrimitiveType::Triangles;
                    g_renderBackend->SetPipelineState(state);
                    g_renderBackend->SetUniformFloat(g_lineShader, "uLineWidth", grp.width);
                    g_renderBackend->DrawIndexed(frame.lineMesh, static_cast<uint32_t>(grp.indexCount), static_cast<uint32_t>(grp.firstIndex));
                }
                g_renderBackend->UnbindMesh();
            }
        }

        // ── Fills: blended, two-sided, depth-tested ───────────────────────
        if (!g_fills.empty()) {
            g_verts.clear();
            for (const Tri& t : g_fills) {
                if (g_verts.size() + 3 > kMaxFillVertices) break;
                uint8_t r, g, b, a; Split(t.color, r, g, b, a);
                for (const glm::vec3& p : t.p) g_verts.push_back({p.x, p.y, p.z, 0, 0, r, g, b, a});
            }
            g_renderBackend->UpdateBufferUnsynchronized(frame.fillVB, 0, g_verts.size() * sizeof(Vertex), g_verts.data());
            PipelineState state;
            state.depthTestEnabled = true;
            state.depthWriteEnabled = false;
            state.blendEnabled = true;
            state.srcBlendFactor = BlendFactor::SrcAlpha;
            state.dstBlendFactor = BlendFactor::OneMinusSrcAlpha;
            state.cullMode = CullMode::None;
            state.primitiveType = PrimitiveType::Triangles;
            g_renderBackend->SetPipelineState(state);
            g_renderBackend->BindShader(g_fillShader);
            g_renderBackend->BindTexture(g_white, 0);
            g_renderBackend->SetUniformMat4(g_fillShader, "uMVP", fillMvp);
            g_renderBackend->SetUniformMat4(g_fillShader, "uModel", glm::mat4(1.0f));
            g_renderBackend->SetUniformVec4(g_fillShader, "uClipPlane", glm::vec4(0.0f));
            g_renderBackend->DrawArrays(frame.fillMesh, static_cast<uint32_t>(g_verts.size()), 0);
            g_renderBackend->UnbindMesh();
        }

        SetDefaultPipeline();
        Clear();
    }

} // namespace Render::Gizmos
