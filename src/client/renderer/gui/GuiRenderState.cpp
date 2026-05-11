// File: src/client/renderer/gui/GuiRenderState.cpp
#include "GuiRenderState.hpp"
#include <algorithm>

namespace Render {

    GuiRenderState::GuiRenderState() {
        m_root = std::make_unique<RenderNode>();
        m_currentNode = m_root.get();
    }

    void GuiRenderState::Reset() {
        m_root = std::make_unique<RenderNode>();
        m_currentNode = m_root.get();
        m_currentZOrder = 0;
    }

    void GuiRenderState::NextStratum() {
        m_currentZOrder++;
    }

    void GuiRenderState::SubmitBlit(const BlitCommand& cmd) {
        BlitCommand c = cmd;
        c.zOrder = m_currentZOrder;
        m_currentNode->blits.push_back(c);
    }

    void GuiRenderState::SubmitFill(const FillCommand& cmd) {
        FillCommand c = cmd;
        c.zOrder = m_currentZOrder;
        m_currentNode->fills.push_back(c);
    }

    void GuiRenderState::SubmitText(const TextCommand& cmd) {
        TextCommand c = cmd;
        c.zOrder = m_currentZOrder;
        m_currentNode->texts.push_back(c);
    }

    void GuiRenderState::SubmitQuad(const QuadCommand& cmd) {
        QuadCommand c = cmd;
        c.zOrder = m_currentZOrder;
        m_currentNode->quads.push_back(c);
    }

    void GuiRenderState::GetAllBlits(std::vector<BlitCommand>& out) const {
        CollectBlits(m_root.get(), out);
        std::stable_sort(out.begin(), out.end(), [](const BlitCommand& a, const BlitCommand& b) {
            return a.zOrder < b.zOrder;
        });
    }

    void GuiRenderState::GetAllFills(std::vector<FillCommand>& out) const {
        CollectFills(m_root.get(), out);
        std::stable_sort(out.begin(), out.end(), [](const FillCommand& a, const FillCommand& b) {
            return a.zOrder < b.zOrder;
        });
    }

    void GuiRenderState::GetAllTexts(std::vector<TextCommand>& out) const {
        CollectTexts(m_root.get(), out);
        std::stable_sort(out.begin(), out.end(), [](const TextCommand& a, const TextCommand& b) {
            return a.zOrder < b.zOrder;
        });
    }

    void GuiRenderState::GetAllQuads(std::vector<QuadCommand>& out) const {
        CollectQuads(m_root.get(), out);
        std::stable_sort(out.begin(), out.end(), [](const QuadCommand& a, const QuadCommand& b) {
            return a.zOrder < b.zOrder;
        });
    }

    void GuiRenderState::CollectBlits(const RenderNode* node, std::vector<BlitCommand>& out) const {
        out.insert(out.end(), node->blits.begin(), node->blits.end());
        for (auto& child : node->children) {
            CollectBlits(child.get(), out);
        }
    }

    void GuiRenderState::CollectFills(const RenderNode* node, std::vector<FillCommand>& out) const {
        out.insert(out.end(), node->fills.begin(), node->fills.end());
        for (auto& child : node->children) {
            CollectFills(child.get(), out);
        }
    }

    void GuiRenderState::CollectTexts(const RenderNode* node, std::vector<TextCommand>& out) const {
        out.insert(out.end(), node->texts.begin(), node->texts.end());
        for (auto& child : node->children) {
            CollectTexts(child.get(), out);
        }
    }

    void GuiRenderState::CollectQuads(const RenderNode* node, std::vector<QuadCommand>& out) const {
        out.insert(out.end(), node->quads.begin(), node->quads.end());
        for (auto& child : node->children) {
            CollectQuads(child.get(), out);
        }
    }

} // namespace Render
