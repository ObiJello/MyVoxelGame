// File: src/client/renderer/environment/CloudRenderer.cpp
//
// Constants and cell/face logic verbatim from the vendored decompile
// (minecraft_code/.../client/renderer/CloudRenderer.java) and the vanilla
// cloud shader (assets/shaders/core/rendertype_clouds.vsh):
//   CELL_SIZE_IN_BLOCKS 12, thickness 4, drift 0.03 blocks/tick (+X),
//   fixed Z offset 3.96, cell empty when alpha < 10, face colors
//   top (1,1,1) bottom (0.7) N/S (0.8) E/W (0.9) × CLOUD_COLOR attribute,
//   fog fades ALPHA toward 0 at FogCloudsEnd (spherical distance).
// Cloud bottom height = DimensionTypes overworld CLOUD_HEIGHT 192.33.
#include "CloudRenderer.hpp"
#include "EnvironmentState.hpp"
#include "../backend/RenderBackend.hpp"
#ifdef HAS_VULKAN
#include "../backend/vulkan/VKBackend.hpp"
#endif
#include "platform/GameDirectory.hpp"
#include "common/core/Log.hpp"
#include "stb_image.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <filesystem>
#include <string>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    CloudRenderer g_cloudRenderer;

    namespace {
        constexpr float kCellSize = 12.0f;
        constexpr float kCellHeight = 4.0f;
        constexpr float kCloudBottomY = 192.33f;   // DimensionTypes CLOUD_HEIGHT
        constexpr float kDriftPerTick = 0.03f;     // BLOCKS_PER_SECOND 0.6 / 20
        constexpr float kFixedZOffset = 3.96f;
        constexpr int kTicksPerCell = 400;

        // Face direction ids match MC Direction.get3DDataValue():
        // DOWN=0 UP=1 NORTH=2 SOUTH=3 WEST=4 EAST=5.
        // Vertex tables from rendertype_clouds.vsh (cell-local 0..1).
        const glm::vec3 kFaceVerts[6][4] = {
            {{1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {0, 0, 0}},   // DOWN
            {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}},   // UP
            {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}},   // NORTH (-z)
            {{1, 0, 1}, {1, 1, 1}, {0, 1, 1}, {0, 0, 1}},   // SOUTH (+z)
            {{0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {0, 0, 0}},   // WEST (-x)
            {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}},   // EAST (+x)
        };
        const glm::vec3 kFaceColors[6] = {
            {0.7f, 0.7f, 0.7f},   // DOWN
            {1.0f, 1.0f, 1.0f},   // UP
            {0.8f, 0.8f, 0.8f},   // NORTH
            {0.8f, 0.8f, 0.8f},   // SOUTH
            {0.9f, 0.9f, 0.9f},   // WEST
            {0.9f, 0.9f, 0.9f},   // EAST
        };

        int FloorModI(int a, int b) {
            int r = a % b;
            if (r < 0) r += b;
            return r;
        }
    } // namespace

    // Positions are camera-relative via uModel; fog distance comes from the
    // transformed position (spherical, MC fog_spherical_distance).
    const char* CloudRenderer::vertexShaderSource = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uMVP;
uniform mat4 uModel;

out vec4 vColor;
out float vDist;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vColor = aColor;
    vDist = length((uModel * vec4(aPos, 1.0)).xyz);
}
)";

    const char* CloudRenderer::fragmentShaderSource = R"(
#version 330 core
in vec4 vColor;
in float vDist;
out vec4 FragColor;

uniform vec4 uColor;     // CLOUD_COLOR attribute (rgba, alpha carries 0.8 base)
uniform vec4 uFogEnv;    // (0, cloudsEnd, unused, unused)

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec4 color = vColor * uColor;
    color.a *= 1.0 - linearFog(vDist, uFogEnv.x, uFogEnv.y);
    if (color.a <= 0.0) discard;
    FragColor = color;
}
)";

    CloudRenderer::~CloudRenderer() {
        Shutdown();
    }

    CloudRenderer::Mode CloudRenderer::CurrentMode() {
        const std::string mode = Platform::g_gameSettings.GetRenderClouds();
        if (mode == "false") return Mode::Off;
        if (mode == "fast") return Mode::Fast;
        return Mode::Fancy;
    }

    bool CloudRenderer::Initialize() {
        if (m_initialized) return true;
        if (!g_renderBackend) return false;

        // Vulkan needs the UBO-aware (portal) pipeline layout for
        // uModel/uFogColor/uFogEnv — same pattern as SkyRenderer.
        if (g_renderBackend->GetType() == BackendType::Vulkan) {
#ifdef HAS_VULKAN
            auto* vk = static_cast<VKBackend*>(g_renderBackend.get());
            m_shader = vk->CreateShaderFromFilesPortal("shaders/clouds.vert", "shaders/clouds.frag");
#endif
        } else {
            m_shader = g_renderBackend->CreateShaderFromFiles("shaders/clouds.vert", "shaders/clouds.frag");
            if (m_shader == INVALID_SHADER) {
                m_shader = g_renderBackend->CreateShader(vertexShaderSource, fragmentShaderSource);
            }
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("CloudRenderer: failed to create shader — clouds disabled");
            return false;
        }

        unsigned char white[] = {255, 255, 255, 255};
        m_whiteTexture = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, white);

        // Decode clouds.png into the occupancy grid (CloudRenderer.prepare:
        // a cell is empty when its alpha < 10).
        const std::string path = PlatformMain::GetAssetPath("assets/textures/environment/clouds.png");
        if (!std::filesystem::exists(path)) {
            Log::Warning("CloudRenderer: missing clouds.png — clouds disabled");
            return false;
        }
        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load(0);
        unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
        if (!pixels) {
            Log::Warning("CloudRenderer: failed to decode clouds.png");
            return false;
        }
        m_texWidth = w;
        m_texHeight = h;
        m_cells.assign(static_cast<size_t>(w) * h, 0);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const unsigned char alpha = pixels[(static_cast<size_t>(y) * w + x) * 4 + 3];
                m_cells[static_cast<size_t>(y) * w + x] = alpha >= 10 ? 1 : 0;
            }
        }
        stbi_image_free(pixels);

        m_initialized = true;
        Log::Info("CloudRenderer initialized (%dx%d cell grid)", w, h);
        return true;
    }

    void CloudRenderer::DestroyMeshBuffers(bool deferred) {
        if (!g_renderBackend) return;
        if (m_mesh != INVALID_MESH) { g_renderBackend->DestroyMesh(m_mesh); m_mesh = INVALID_MESH; }
        if (m_vb != INVALID_BUFFER) {
            if (deferred) g_renderBackend->DeferredDestroyBuffer(m_vb);
            else g_renderBackend->DestroyBuffer(m_vb);
            m_vb = INVALID_BUFFER;
        }
        if (m_ib != INVALID_BUFFER) {
            if (deferred) g_renderBackend->DeferredDestroyBuffer(m_ib);
            else g_renderBackend->DestroyBuffer(m_ib);
            m_ib = INVALID_BUFFER;
        }
        m_indexCount = 0;
    }

    void CloudRenderer::Shutdown() {
        if (!g_renderBackend) return;
        DestroyMeshBuffers(false);
        if (m_whiteTexture != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_whiteTexture); m_whiteTexture = INVALID_TEXTURE; }
        if (m_shader != INVALID_SHADER)        { g_renderBackend->DestroyShader(m_shader);        m_shader = INVALID_SHADER; }
        m_initialized = false;
        m_lastCellX = INT32_MIN;
        m_lastCellZ = INT32_MIN;
        m_lastMode = Mode::Off;
        m_lastRadiusCells = -1;
    }

    bool CloudRenderer::CellOccupied(int cx, int cz) const {
        const int x = FloorModI(cx, m_texWidth);
        const int y = FloorModI(cz, m_texHeight);
        return m_cells[static_cast<size_t>(y) * m_texWidth + x] != 0;
    }

    void CloudRenderer::EmitFace(std::vector<Vertex>& verts, std::vector<uint32_t>& indices,
                                 int rx, int rz, int dir, bool inside, bool useTopColor) const {
        const glm::vec3 color = kFaceColors[useTopColor ? 1 : dir];
        const uint8_t r = static_cast<uint8_t>(color.r * 255.0f);
        const uint8_t g = static_cast<uint8_t>(color.g * 255.0f);
        const uint8_t b = static_cast<uint8_t>(color.b * 255.0f);
        const uint32_t base = static_cast<uint32_t>(verts.size());
        for (int i = 0; i < 4; ++i) {
            // Inside faces reverse the vertex order (rendertype_clouds.vsh:
            // 3 - quadVertex) so their winding faces the camera.
            const glm::vec3& fv = kFaceVerts[dir][inside ? 3 - i : i];
            const glm::vec3 p{
                (static_cast<float>(rx) + fv.x) * kCellSize,
                fv.y * kCellHeight,
                (static_cast<float>(rz) + fv.z) * kCellSize,
            };
            verts.push_back({p.x, p.y, p.z, 0.0f, 0.0f, r, g, b, 255});
        }
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }

    void CloudRenderer::BuildCell(std::vector<Vertex>& verts, std::vector<uint32_t>& indices,
                                  int rx, int rz, int cellX, int cellZ,
                                  RelativePos rel, Mode mode) const {
        if (!CellOccupied(cellX + rx, cellZ + rz)) return;

        if (mode == Mode::Fast) {
            // buildFlatCell: one downward face using the TOP color.
            EmitFace(verts, indices, rx, rz, /*DOWN*/ 0, false, /*useTopColor*/ true);
            return;
        }

        // buildFancyCell.
        const bool northEmpty = !CellOccupied(cellX + rx, cellZ + rz - 1);
        const bool southEmpty = !CellOccupied(cellX + rx, cellZ + rz + 1);
        const bool westEmpty  = !CellOccupied(cellX + rx - 1, cellZ + rz);
        const bool eastEmpty  = !CellOccupied(cellX + rx + 1, cellZ + rz);

        if (rel != RelativePos::BelowClouds) EmitFace(verts, indices, rx, rz, 1, false, false); // UP
        if (rel != RelativePos::AboveClouds) EmitFace(verts, indices, rx, rz, 0, false, false); // DOWN
        if (northEmpty && rz > 0) EmitFace(verts, indices, rx, rz, 2, false, false);
        if (southEmpty && rz < 0) EmitFace(verts, indices, rx, rz, 3, false, false);
        if (westEmpty && rx > 0)  EmitFace(verts, indices, rx, rz, 4, false, false);
        if (eastEmpty && rx < 0)  EmitFace(verts, indices, rx, rz, 5, false, false);
        // Cells around/under the camera get all faces with reversed winding
        // so the interior is visible while flying through.
        if (std::abs(rx) <= 1 && std::abs(rz) <= 1) {
            for (int dir = 0; dir < 6; ++dir) {
                EmitFace(verts, indices, rx, rz, dir, true, false);
            }
        }
    }

    void CloudRenderer::RebuildMesh(int cellX, int cellZ, RelativePos rel, Mode mode,
                                    int radiusCells) {
        std::vector<Vertex> verts;
        std::vector<uint32_t> indices;

        // MC's ring iteration: manhattan rings outward, circle-clamped.
        for (int ring = 0; ring <= 2 * radiusCells; ++ring) {
            for (int rx = -ring; rx <= ring; ++rx) {
                const int rz = ring - std::abs(rx);
                if (rz >= 0 && rz <= radiusCells &&
                    rx * rx + rz * rz <= radiusCells * radiusCells) {
                    if (rz != 0) BuildCell(verts, indices, rx, -rz, cellX, cellZ, rel, mode);
                    BuildCell(verts, indices, rx, rz, cellX, cellZ, rel, mode);
                }
            }
        }

        DestroyMeshBuffers(true);
        if (!indices.empty()) {
            m_vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
                                                 verts.size() * sizeof(Vertex), verts.data());
            m_ib = g_renderBackend->CreateBuffer(BufferUsage::Index,
                                                 indices.size() * sizeof(uint32_t), indices.data());
            m_mesh = g_renderBackend->CreateMesh(m_vb, m_ib, GetBlockVertexLayout());
            m_indexCount = static_cast<uint32_t>(indices.size());
        }
    }

    void CloudRenderer::Render(const glm::mat4& proj, const glm::mat4& view,
                               const glm::vec3& cameraPos, int renderDistChunks,
                               float partialTick) {
        if (!m_initialized || !g_renderBackend || m_texWidth == 0) return;

        const Mode mode = CurrentMode();
        if (mode == Mode::Off) return;

        const EnvironmentFrame& env = EnvironmentState::Get().Frame();
        // Skip the pass entirely when the cloud color is fully transparent
        // (LevelRenderer.addCloudsPass).
        if (env.cloudColor.a <= 0.0f) return;

        // Placement (CloudRenderer.render), double precision like MC.
        const double cloudOffset =
            static_cast<double>(EnvironmentState::Get().GameTime() %
                                (static_cast<int64_t>(m_texWidth) * kTicksPerCell)) +
            static_cast<double>(partialTick);
        double cloudX = static_cast<double>(cameraPos.x) + cloudOffset * 0.030000001;
        double cloudZ = static_cast<double>(cameraPos.z) + kFixedZOffset;
        const double texW = static_cast<double>(m_texWidth) * kCellSize;
        const double texH = static_cast<double>(m_texHeight) * kCellSize;
        cloudX -= std::floor(cloudX / texW) * texW;
        cloudZ -= std::floor(cloudZ / texH) * texH;
        const int cellX = static_cast<int>(std::floor(cloudX / kCellSize));
        const int cellZ = static_cast<int>(std::floor(cloudZ / kCellSize));
        const float xInCell = static_cast<float>(cloudX - cellX * static_cast<double>(kCellSize));
        const float zInCell = static_cast<float>(cloudZ - cellZ * static_cast<double>(kCellSize));

        const float relativeBottomY = kCloudBottomY - cameraPos.y;
        const float relativeTopY = relativeBottomY + kCellHeight;
        const RelativePos rel = relativeTopY < 0.0f ? RelativePos::AboveClouds
                              : (relativeBottomY > 0.0f ? RelativePos::BelowClouds
                                                        : RelativePos::InsideClouds);

        // Video Settings "Cloud Range" slider (blocks), clamped so clouds
        // never cut off closer than the render distance allows seeing.
        const int radiusBlocks = std::max(Platform::g_gameSettings.GetCloudRange(),
                                          std::min(renderDistChunks * 16, 512));
        const int radiusCells = static_cast<int>(std::ceil(radiusBlocks / kCellSize));

        if (cellX != m_lastCellX || cellZ != m_lastCellZ || rel != m_lastRel ||
            mode != m_lastMode || radiusCells != m_lastRadiusCells) {
            RebuildMesh(cellX, cellZ, rel, mode, radiusCells);
            m_lastCellX = cellX;
            m_lastCellZ = cellZ;
            m_lastRel = rel;
            m_lastMode = mode;
            m_lastRadiusCells = radiusCells;
        }
        if (m_mesh == INVALID_MESH || m_indexCount == 0) return;

        // Camera-relative model: cell-local mesh → world offset around camera.
        const glm::mat4 model = glm::translate(
            glm::mat4(1.0f), glm::vec3(-xInCell, relativeBottomY, -zInCell));
        const glm::mat4 viewRotation = glm::mat4(glm::mat3(view));

        PipelineState state;
        state.depthTestEnabled = true;
        // Depth write ON, like vanilla's CLOUDS pipeline (the builder default —
        // it never calls withDepthWrite(false)). This is load-bearing: the
        // reversed-winding interior faces of the 3×3 cells around the camera
        // sit behind the cloud's outer faces, and the outer faces must write
        // depth so the interior ones fail the depth test instead of
        // double-blending through (visible as a bright 3×3 patch overhead).
        state.depthWriteEnabled = true;
        state.blendEnabled = true;
        state.srcBlendFactor = BlendFactor::SrcAlpha;
        state.dstBlendFactor = BlendFactor::OneMinusSrcAlpha;
        // FLAT_CLOUDS disables culling; fancy CLOUDS keeps back-face culling.
        state.cullMode = (mode == Mode::Fast) ? CullMode::None : CullMode::Back;
        state.primitiveType = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(state);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(m_whiteTexture, 0);

        g_renderBackend->SetUniformMat4(m_shader, "uMVP", proj * viewRotation * model);
        g_renderBackend->SetUniformMat4(m_shader, "uModel", model);
        g_renderBackend->SetUniformVec4(m_shader, "uColor", env.cloudColor);
        // MC FogData: cloud fog end = min(cloud range, CLOUD_FOG_END 2048).
        const float cloudFogEnd = std::min(static_cast<float>(radiusBlocks), 2048.0f);
        g_renderBackend->SetUniformVec4(m_shader, "uFogEnv",
                                        glm::vec4(0.0f, cloudFogEnd, 1e9f, 1e9f));
        g_renderBackend->DrawIndexed(m_mesh, m_indexCount);
        g_renderBackend->UnbindMesh();

        PipelineState defaultState;
        defaultState.depthTestEnabled = true;
        defaultState.depthWriteEnabled = true;
        defaultState.blendEnabled = false;
        defaultState.cullMode = CullMode::Back;
        g_renderBackend->SetPipelineState(defaultState);
    }

} // namespace Render
