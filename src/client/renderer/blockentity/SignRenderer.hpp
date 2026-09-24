// File: src/client/renderer/blockentity/SignRenderer.hpp
//
// The text on a sign — MC AbstractSignRenderer / StandingSignRenderer /
// HangingSignRenderer. The board, post, chains and bar are ordinary chunk
// geometry from the 26.3 block models (template_sign_rot_N,
// template_wall_sign, the hanging templates); vanilla's block-entity
// renderer draws only the four lines on each face, and so does this.
//
// Glyphs come from the same 8×8 font sheet the GUI uses
// (FontRenderer::GenerateQuadsTyped), posed with vanilla's text
// transformation — translate to the board, turn to the sign's yaw, scale by
// 1/96 (0.9/64 for a hanging sign) with Y flipped — and drawn through the
// shared block-entity shader. The mesh is built once per sign and rebuilt
// when its text, colour or block state changes.
#pragma once

#include "BlockEntityRenderer.hpp"
#include "../backend/RenderTypes.hpp"
#include "../gui/FontRenderer.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace Render {

    class SignRenderer : public BlockEntityRenderer {
    public:
        SignRenderer();
        ~SignRenderer() override;

        bool Initialize();
        void Shutdown();

        void Render(const Game::BlockEntity& be,
                    float partialTick,
                    const glm::mat4& proj,
                    const glm::mat4& view,
                    const glm::vec3& cameraPos) override;

    private:
        // One GPU mesh. A sign holds up to three: the plain glyphs, the
        // glowing glyphs (lit differently) and, for glowing text, their
        // 8-way outline. They are drawn with different depth
        // modes (MC Font.DisplayMode NORMAL vs POLYGON_OFFSET), which is
        // why they cannot share a buffer.
        struct Layer {
            BufferHandle vb = INVALID_BUFFER;
            BufferHandle ib = INVALID_BUFFER;
            MeshHandle   mesh = INVALID_MESH;
            uint32_t     indexCount = 0;
        };
        struct Cached {
            Layer    text;       // plain glyphs, at the sign's light
            Layer    glowText;   // glowing glyphs, at full brightness
            Layer    outline;
            bool     built = false;
            uint64_t hash = 0;
            std::chrono::steady_clock::time_point lastUsed;
        };

        bool BuildLayer(Layer& layer, const void* vertexData, size_t vertexBytes,
                        const uint32_t* indices, size_t indexCount);
        void DestroyLayer(Layer& layer);
        void DestroyCached(Cached& c);
        void SweepStale(std::chrono::steady_clock::time_point now);

        ShaderHandle m_shader = INVALID_SHADER;
        FontRenderer m_font;
        bool m_initialized = false;
        std::unordered_map<int64_t, Cached> m_cache;
        std::chrono::steady_clock::time_point m_lastSweep;
    };

} // namespace Render
