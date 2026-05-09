// File: src/client/renderer/gui/GuiRenderState.hpp
// Render command accumulator for the GUI system.
// Matches MC's GuiRenderState: hierarchical node tree with strata for z-ordering.
#pragma once

#include "../backend/RenderTypes.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>

// `InventorySlot` is now an alias for `ItemStack` (see common/entity/Item.hpp).
// We forward-declare ItemStack so headers that include this don't pay for the full Item.hpp.
namespace Game { struct ItemStack; }

namespace Render {

    struct ScissorRect {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };

    // A single textured/colored quad command
    struct BlitCommand {
        TextureHandle texture = INVALID_TEXTURE;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        uint32_t color = 0xFFFFFFFF;
        glm::mat3x2 transform{1.0f};
        ScissorRect scissor;
        bool hasScissor = false;
        int zOrder = 0;
    };

    // A solid or gradient fill (no texture)
    struct FillCommand {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        uint32_t color0 = 0xFFFFFFFF;   // Top color (or solid)
        uint32_t color1 = 0xFFFFFFFF;   // Bottom color (same = solid)
        glm::mat3x2 transform{1.0f};
        ScissorRect scissor;
        bool hasScissor = false;
        int zOrder = 0;
    };

    // A text draw command
    struct TextCommand {
        std::string text;
        float x = 0, y = 0;
        uint32_t color = 0xFFFFFFFF;
        bool dropShadow = true;
        glm::mat3x2 transform{1.0f};
        ScissorRect scissor;
        bool hasScissor = false;
        int zOrder = 0;
    };

    // An arbitrary textured quad with 4 explicit corner positions (for isometric faces)
    struct QuadCommand {
        TextureHandle texture = INVALID_TEXTURE;
        // 4 corners: top-left, top-right, bottom-right, bottom-left
        float px[4] = {}, py[4] = {};
        float u[4] = {}, v[4] = {};
        uint32_t color = 0xFFFFFFFF;
        int zOrder = 0;
        ScissorRect scissor;
        bool hasScissor = false;
    };

    // MC-style hierarchical render state node
    struct RenderNode {
        RenderNode* parent = nullptr;
        std::vector<std::unique_ptr<RenderNode>> children;

        std::vector<BlitCommand> blits;
        std::vector<FillCommand> fills;
        std::vector<TextCommand> texts;
        std::vector<QuadCommand> quads;
    };

    class GuiRenderState {
    public:
        GuiRenderState();

        void Reset();

        // Z-ordering layers (MC: nextStratum)
        void NextStratum();
        int GetCurrentZOrder() const { return m_currentZOrder; }

        // Submit render commands
        void SubmitBlit(const BlitCommand& cmd);
        void SubmitFill(const FillCommand& cmd);
        void SubmitText(const TextCommand& cmd);
        void SubmitQuad(const QuadCommand& cmd);

        // Flatten all commands for rendering (sorted by z-order)
        void GetAllBlits(std::vector<BlitCommand>& out) const;
        void GetAllFills(std::vector<FillCommand>& out) const;
        void GetAllTexts(std::vector<TextCommand>& out) const;
        void GetAllQuads(std::vector<QuadCommand>& out) const;

    private:
        std::unique_ptr<RenderNode> m_root;
        RenderNode* m_currentNode = nullptr;
        int m_currentZOrder = 0;

        void CollectBlits(const RenderNode* node, std::vector<BlitCommand>& out) const;
        void CollectFills(const RenderNode* node, std::vector<FillCommand>& out) const;
        void CollectTexts(const RenderNode* node, std::vector<TextCommand>& out) const;
        void CollectQuads(const RenderNode* node, std::vector<QuadCommand>& out) const;
    };

} // namespace Render
