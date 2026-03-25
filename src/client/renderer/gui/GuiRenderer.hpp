// File: src/client/renderer/gui/GuiRenderer.hpp
// GPU batching and draw execution for the GUI system.
// Matches MC's GuiRenderer: sorts commands, batches by texture, executes draws.
#pragma once

#include "../backend/RenderTypes.hpp"
#include "GuiRenderState.hpp"
#include <vector>
#include <glm/glm.hpp>

namespace Render {

    class FontRenderer;

    class GuiRenderer {
    public:
        GuiRenderer() = default;
        ~GuiRenderer();

        bool Initialize();
        void Shutdown();

        // Render all accumulated GUI commands
        // guiScale: MC-style GUI scaling factor (1, 2, 3, or auto)
        void Render(const GuiRenderState& state,
                    int windowWidth, int windowHeight,
                    int framebufferWidth, int framebufferHeight,
                    float guiScale,
                    FontRenderer* fontRenderer);

    private:
        // 24-byte vertex matching GetBlockVertexLayout()
        struct GuiVertex {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        static_assert(sizeof(GuiVertex) == 24, "GuiVertex must be 24 bytes");

        ShaderHandle m_texturedShader = INVALID_SHADER;
        ShaderHandle m_colorShader = INVALID_SHADER;
        TextureHandle m_dummyTexture = INVALID_TEXTURE; // 1x1 white pixel for color-only draws on Vulkan

        // Dynamic vertex buffer — rebuilt each frame
        BufferHandle m_vertexBuffer = INVALID_BUFFER;
        BufferHandle m_indexBuffer = INVALID_BUFFER;
        MeshHandle m_mesh = INVALID_MESH;
        size_t m_vertexBufferCapacity = 0;
        size_t m_indexBufferCapacity = 0;

        struct DrawBatch {
            TextureHandle texture;    // INVALID_TEXTURE for fills
            bool useColorShader;      // true for fills, false for textured
            int firstIndex;
            int indexCount;
            bool hasScissor;
            ScissorRect scissor;
        };

        std::vector<GuiVertex> m_vertices;
        std::vector<uint32_t> m_indices;
        std::vector<DrawBatch> m_batches;

        void BuildBatches(const GuiRenderState& state, float guiScale, FontRenderer* fontRenderer);
        void AddBlitQuad(TextureHandle texture, const BlitCommand& cmd);
        void AddFillQuad(const FillCommand& cmd);
        void AddTextQuads(const TextCommand& cmd, FontRenderer* fontRenderer);
        void FlushBatch(TextureHandle currentTex, bool isColor, bool hasScissor,
                       const ScissorRect& scissor, int startIndex, int indexCount);
        void ExecuteBatches(int fbWidth, int fbHeight, float guiScale);

        // Apply 2D transform to a point
        static glm::vec2 TransformPoint(const glm::mat3x2& m, float x, float y);
    };

} // namespace Render
