// File: src/client/renderer/gui/GuiRenderer.cpp
#include "GuiRenderer.hpp"
#include "FontRenderer.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace Render {

    // Shader source for textured GUI quads (position + UV + color tint)
    static const char* guiTexturedVert = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aColor;
uniform mat4 uProjection;
out vec2 vTexCoord;
out vec4 vColor;
void main() {
    gl_Position = uProjection * vec4(aPos, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)";

    static const char* guiTexturedFrag = R"(
#version 330 core
in vec2 vTexCoord;
in vec4 vColor;
out vec4 FragColor;
uniform sampler2D uTexture;
void main() {
    vec4 texColor = texture(uTexture, vTexCoord);
    FragColor = texColor * vColor;
}
)";

    // Shader source for solid/gradient fills (position + color, no texture)
    static const char* guiColorVert = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aColor;
uniform mat4 uProjection;
out vec4 vColor;
void main() {
    gl_Position = uProjection * vec4(aPos, 1.0);
    vColor = aColor;
}
)";

    static const char* guiColorFrag = R"(
#version 330 core
in vec4 vColor;
out vec4 FragColor;
void main() {
    FragColor = vColor;
}
)";

    GuiRenderer::~GuiRenderer() {
        Shutdown();
    }

    bool GuiRenderer::Initialize() {
        if (!g_renderBackend) return false;

        // Create shaders — try file-based first (Vulkan SPIR-V), fall back to embedded source
        m_texturedShader = g_renderBackend->CreateShaderFromFiles(
            "shaders/gui_textured.vert", "shaders/gui_textured.frag");
        if (m_texturedShader == INVALID_SHADER) {
            m_texturedShader = g_renderBackend->CreateShader(guiTexturedVert, guiTexturedFrag);
        }

        m_colorShader = g_renderBackend->CreateShaderFromFiles(
            "shaders/gui_color.vert", "shaders/gui_color.frag");
        if (m_colorShader == INVALID_SHADER) {
            m_colorShader = g_renderBackend->CreateShader(guiColorVert, guiColorFrag);
        }

        if (m_texturedShader == INVALID_SHADER || m_colorShader == INVALID_SHADER) {
            Log::Error("[GuiRenderer] Failed to create GUI shaders");
            return false;
        }

        Log::Info("[GuiRenderer] Initialized successfully");
        return true;
    }

    void GuiRenderer::Shutdown() {
        if (g_renderBackend) {
            if (m_mesh != INVALID_MESH) { g_renderBackend->DestroyMesh(m_mesh); m_mesh = INVALID_MESH; }
            if (m_vertexBuffer != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(m_vertexBuffer); m_vertexBuffer = INVALID_BUFFER; }
            if (m_indexBuffer != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(m_indexBuffer); m_indexBuffer = INVALID_BUFFER; }
            if (m_texturedShader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_texturedShader); m_texturedShader = INVALID_SHADER; }
            if (m_colorShader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_colorShader); m_colorShader = INVALID_SHADER; }
        }
        m_vertexBufferCapacity = 0;
        m_indexBufferCapacity = 0;
    }

    glm::vec2 GuiRenderer::TransformPoint(const glm::mat3x2& m, float x, float y) {
        return glm::vec2(
            m[0][0] * x + m[1][0] * y + m[2][0],
            m[0][1] * x + m[1][1] * y + m[2][1]
        );
    }

    void GuiRenderer::Render(const GuiRenderState& state,
                             int windowWidth, int windowHeight,
                             int framebufferWidth, int framebufferHeight,
                             float guiScale,
                             FontRenderer* fontRenderer) {
        if (!g_renderBackend) return;

        m_vertices.clear();
        m_indices.clear();
        m_batches.clear();

        BuildBatches(state, guiScale, fontRenderer);

        if (m_vertices.empty()) return;

        ExecuteBatches(framebufferWidth, framebufferHeight, guiScale);
    }

    void GuiRenderer::AddBlitQuad(TextureHandle texture, const BlitCommand& cmd) {
        // Decompose color (ARGB format)
        uint8_t a = (cmd.color >> 24) & 0xFF;
        uint8_t r = (cmd.color >> 16) & 0xFF;
        uint8_t g = (cmd.color >> 8) & 0xFF;
        uint8_t b = cmd.color & 0xFF;

        glm::vec2 tl = TransformPoint(cmd.transform, cmd.x0, cmd.y0);
        glm::vec2 tr = TransformPoint(cmd.transform, cmd.x1, cmd.y0);
        glm::vec2 br = TransformPoint(cmd.transform, cmd.x1, cmd.y1);
        glm::vec2 bl = TransformPoint(cmd.transform, cmd.x0, cmd.y1);

        uint32_t base = static_cast<uint32_t>(m_vertices.size());

        m_vertices.push_back({ tl.x, tl.y, 0.0f, cmd.u0, cmd.v0, r, g, b, a });
        m_vertices.push_back({ tr.x, tr.y, 0.0f, cmd.u1, cmd.v0, r, g, b, a });
        m_vertices.push_back({ br.x, br.y, 0.0f, cmd.u1, cmd.v1, r, g, b, a });
        m_vertices.push_back({ bl.x, bl.y, 0.0f, cmd.u0, cmd.v1, r, g, b, a });

        m_indices.push_back(base + 0);
        m_indices.push_back(base + 1);
        m_indices.push_back(base + 2);
        m_indices.push_back(base + 0);
        m_indices.push_back(base + 2);
        m_indices.push_back(base + 3);
    }

    void GuiRenderer::AddFillQuad(const FillCommand& cmd) {
        uint8_t a0 = (cmd.color0 >> 24) & 0xFF;
        uint8_t r0 = (cmd.color0 >> 16) & 0xFF;
        uint8_t g0 = (cmd.color0 >> 8) & 0xFF;
        uint8_t b0 = cmd.color0 & 0xFF;

        uint8_t a1 = (cmd.color1 >> 24) & 0xFF;
        uint8_t r1 = (cmd.color1 >> 16) & 0xFF;
        uint8_t g1 = (cmd.color1 >> 8) & 0xFF;
        uint8_t b1 = cmd.color1 & 0xFF;

        glm::vec2 tl = TransformPoint(cmd.transform, cmd.x0, cmd.y0);
        glm::vec2 tr = TransformPoint(cmd.transform, cmd.x1, cmd.y0);
        glm::vec2 br = TransformPoint(cmd.transform, cmd.x1, cmd.y1);
        glm::vec2 bl = TransformPoint(cmd.transform, cmd.x0, cmd.y1);

        uint32_t base = static_cast<uint32_t>(m_vertices.size());

        m_vertices.push_back({ tl.x, tl.y, 0.0f, 0, 0, r0, g0, b0, a0 });
        m_vertices.push_back({ tr.x, tr.y, 0.0f, 0, 0, r0, g0, b0, a0 });
        m_vertices.push_back({ br.x, br.y, 0.0f, 0, 0, r1, g1, b1, a1 });
        m_vertices.push_back({ bl.x, bl.y, 0.0f, 0, 0, r1, g1, b1, a1 });

        m_indices.push_back(base + 0);
        m_indices.push_back(base + 1);
        m_indices.push_back(base + 2);
        m_indices.push_back(base + 0);
        m_indices.push_back(base + 2);
        m_indices.push_back(base + 3);
    }

    void GuiRenderer::AddTextQuads(const TextCommand& cmd, FontRenderer* fontRenderer) {
        if (!fontRenderer) return;
        fontRenderer->GenerateQuadsTyped(cmd, m_vertices, m_indices);
    }

    void GuiRenderer::BuildBatches(const GuiRenderState& state, float guiScale,
                                    FontRenderer* fontRenderer) {
        // Merge all commands into a single sorted list by z-order
        // Each command type is tagged so we can batch by texture

        struct SortedCommand {
            int zOrder;
            int type;  // 0=fill, 1=blit, 2=text, 3=quad
            int index;
        };

        std::vector<FillCommand> fills;
        std::vector<BlitCommand> blits;
        std::vector<TextCommand> texts;
        std::vector<QuadCommand> quads;

        state.GetAllFills(fills);
        state.GetAllBlits(blits);
        state.GetAllTexts(texts);
        state.GetAllQuads(quads);

        std::vector<SortedCommand> sorted;
        sorted.reserve(fills.size() + blits.size() + texts.size() + quads.size());

        for (int i = 0; i < static_cast<int>(fills.size()); i++)
            sorted.push_back({ fills[i].zOrder, 0, i });
        for (int i = 0; i < static_cast<int>(blits.size()); i++)
            sorted.push_back({ blits[i].zOrder, 1, i });
        for (int i = 0; i < static_cast<int>(texts.size()); i++)
            sorted.push_back({ texts[i].zOrder, 2, i });
        for (int i = 0; i < static_cast<int>(quads.size()); i++)
            sorted.push_back({ quads[i].zOrder, 3, i });

        std::stable_sort(sorted.begin(), sorted.end(), [](const SortedCommand& a, const SortedCommand& b) {
            return a.zOrder < b.zOrder;
        });

        // Process each command and build batches
        TextureHandle currentTex = INVALID_TEXTURE;
        bool currentIsColor = true;
        bool currentHasScissor = false;
        ScissorRect currentScissor;
        int batchStartIndex = 0;

        for (const auto& sc : sorted) {
            TextureHandle thisTex = INVALID_TEXTURE;
            bool thisIsColor = false;
            bool thisHasScissor = false;
            ScissorRect thisScissor;

            int indexCountBefore = static_cast<int>(m_indices.size());

            if (sc.type == 0) {
                // Fill
                thisIsColor = true;
                thisHasScissor = fills[sc.index].hasScissor;
                thisScissor = fills[sc.index].scissor;
                AddFillQuad(fills[sc.index]);
            } else if (sc.type == 1) {
                // Blit
                thisTex = blits[sc.index].texture;
                thisIsColor = (thisTex == INVALID_TEXTURE);
                thisHasScissor = blits[sc.index].hasScissor;
                thisScissor = blits[sc.index].scissor;
                AddBlitQuad(thisTex, blits[sc.index]);
            } else if (sc.type == 2) {
                // Text — uses font texture
                if (fontRenderer) {
                    thisTex = fontRenderer->GetFontTexture();
                }
                thisIsColor = false;
                thisHasScissor = texts[sc.index].hasScissor;
                thisScissor = texts[sc.index].scissor;
                AddTextQuads(texts[sc.index], fontRenderer);
            } else if (sc.type == 3) {
                // Quad — arbitrary textured quad (isometric block faces)
                const auto& q = quads[sc.index];
                thisTex = q.texture;
                thisIsColor = (thisTex == INVALID_TEXTURE);

                uint8_t a = (q.color >> 24) & 0xFF;
                uint8_t r = (q.color >> 16) & 0xFF;
                uint8_t g = (q.color >> 8) & 0xFF;
                uint8_t b = q.color & 0xFF;

                uint32_t base = static_cast<uint32_t>(m_vertices.size());
                for (int vi = 0; vi < 4; vi++) {
                    m_vertices.push_back({ q.px[vi], q.py[vi], 0.0f, q.u[vi], q.v[vi], r, g, b, a });
                }
                m_indices.push_back(base + 0);
                m_indices.push_back(base + 1);
                m_indices.push_back(base + 2);
                m_indices.push_back(base + 0);
                m_indices.push_back(base + 2);
                m_indices.push_back(base + 3);
            }

            int newIndices = static_cast<int>(m_indices.size()) - indexCountBefore;

            // Check if we need a new batch
            bool needNewBatch = m_batches.empty() ||
                                thisTex != currentTex ||
                                thisIsColor != currentIsColor ||
                                thisHasScissor != currentHasScissor;

            if (!needNewBatch && thisHasScissor && currentHasScissor) {
                if (thisScissor.x0 != currentScissor.x0 || thisScissor.y0 != currentScissor.y0 ||
                    thisScissor.x1 != currentScissor.x1 || thisScissor.y1 != currentScissor.y1) {
                    needNewBatch = true;
                }
            }

            if (needNewBatch && newIndices > 0) {
                DrawBatch batch;
                batch.texture = thisTex;
                batch.useColorShader = thisIsColor;
                batch.firstIndex = indexCountBefore;
                batch.indexCount = newIndices;
                batch.hasScissor = thisHasScissor;
                batch.scissor = thisScissor;
                m_batches.push_back(batch);

                currentTex = thisTex;
                currentIsColor = thisIsColor;
                currentHasScissor = thisHasScissor;
                currentScissor = thisScissor;
            } else if (!m_batches.empty() && newIndices > 0) {
                // Extend current batch
                m_batches.back().indexCount += newIndices;
            }
        }
    }

    void GuiRenderer::ExecuteBatches(int fbWidth, int fbHeight, float guiScale) {
        if (m_batches.empty() || m_vertices.empty()) return;

        // Recreate GPU buffers if needed
        size_t vertexDataSize = m_vertices.size() * sizeof(GuiVertex);
        size_t indexDataSize = m_indices.size() * sizeof(uint32_t);

        // Destroy old mesh/buffers if they exist
        if (m_mesh != INVALID_MESH) {
            g_renderBackend->DestroyMesh(m_mesh);
            m_mesh = INVALID_MESH;
        }
        if (m_vertexBuffer != INVALID_BUFFER) {
            g_renderBackend->DestroyBuffer(m_vertexBuffer);
            m_vertexBuffer = INVALID_BUFFER;
        }
        if (m_indexBuffer != INVALID_BUFFER) {
            g_renderBackend->DestroyBuffer(m_indexBuffer);
            m_indexBuffer = INVALID_BUFFER;
        }

        m_vertexBuffer = g_renderBackend->CreateBuffer(
            BufferUsage::Vertex, vertexDataSize, m_vertices.data());
        m_indexBuffer = g_renderBackend->CreateBuffer(
            BufferUsage::Index, indexDataSize, m_indices.data());
        m_mesh = g_renderBackend->CreateMesh(m_vertexBuffer, m_indexBuffer, GetBlockVertexLayout());

        if (m_mesh == INVALID_MESH) return;

        // Orthographic projection in GUI-scaled coordinates
        float guiWidth = static_cast<float>(fbWidth) / guiScale;
        float guiHeight = static_cast<float>(fbHeight) / guiScale;
        glm::mat4 projection = glm::ortho(0.0f, guiWidth, guiHeight, 0.0f, -1000.0f, 1000.0f);

        // GUI pipeline state: no depth, alpha blend
        PipelineState guiState;
        guiState.depthTestEnabled = false;
        guiState.depthWriteEnabled = false;
        guiState.blendEnabled = true;
        guiState.srcBlendFactor = BlendFactor::SrcAlpha;
        guiState.dstBlendFactor = BlendFactor::OneMinusSrcAlpha;
        guiState.cullMode = CullMode::None;
        g_renderBackend->SetPipelineState(guiState);

        for (const auto& batch : m_batches) {
            ShaderHandle shader = batch.useColorShader ? m_colorShader : m_texturedShader;
            g_renderBackend->BindShader(shader);
            g_renderBackend->SetUniformMat4(shader, "uProjection", projection);

            if (!batch.useColorShader && batch.texture != INVALID_TEXTURE) {
                g_renderBackend->BindTexture(batch.texture, 0);
                g_renderBackend->SetUniformInt(shader, "uTexture", 0);
            }

            // TODO: Scissor rect support via glScissor/vkCmdSetScissor

            g_renderBackend->DrawIndexed(m_mesh, batch.indexCount, batch.firstIndex);
        }

        g_renderBackend->UnbindMesh();

        // Restore default pipeline state
        PipelineState defaultState;
        defaultState.depthTestEnabled = true;
        defaultState.depthWriteEnabled = true;
        defaultState.blendEnabled = false;
        defaultState.cullMode = CullMode::Back;
        g_renderBackend->SetPipelineState(defaultState);
    }

} // namespace Render
