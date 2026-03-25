// File: src/client/renderer/gui/FontRenderer.hpp
// Bitmap font renderer matching MC's Font.java.
// Loads ascii.png glyph atlas, measures text, generates textured quads.
#pragma once

#include "../backend/RenderTypes.hpp"
#include "GuiRenderState.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

namespace Render {

    class FontRenderer {
    public:
        static constexpr int LINE_HEIGHT = 9;       // MC's Font.lineHeight
        static constexpr int GLYPH_GRID = 16;       // 16x16 grid in ascii.png
        static constexpr int GLYPH_CELL_SIZE = 8;   // Each cell is 8x8 pixels

        FontRenderer() = default;
        ~FontRenderer();

        bool Initialize(const std::string& fontAtlasPath);
        void Shutdown();

        // Measure text width in GUI pixels
        int GetStringWidth(const std::string& text) const;

        // Get width of a single character glyph (no spacing)
        int GetCharWidth(unsigned char c) const { return m_glyphWidths[c]; }

        // Get font texture handle (for batching)
        TextureHandle GetFontTexture() const { return m_fontTexture; }

        // Generate textured quads for a text command.
        // Templated on vertex type so GuiRenderer can pass its own vertex struct.
        template<typename VertexT>
        void GenerateQuadsTyped(const TextCommand& cmd,
                               std::vector<VertexT>& vertices,
                               std::vector<uint32_t>& indices) const;

        // MC formatting code colors (§0-§f)
        static uint32_t GetFormattingColor(char code);

    private:
        TextureHandle m_fontTexture = INVALID_TEXTURE;
        int m_fontTexWidth = 0, m_fontTexHeight = 0;
        int m_glyphWidths[256] = {};  // Per-character pixel widths

        // Get UV rect for an ASCII character
        void GetGlyphUV(unsigned char c, float& u0, float& v0, float& u1, float& v1) const;

        // Detect glyph widths by scanning pixel data
        void DetectGlyphWidths(const unsigned char* pixels, int width, int height);
    };

    // Template implementation — generates quads matching any vertex type with x,y,z,u,v,r,g,b,a layout
    template<typename VertexT>
    void FontRenderer::GenerateQuadsTyped(const TextCommand& cmd,
                                          std::vector<VertexT>& vertices,
                                          std::vector<uint32_t>& indices) const {
        uint32_t color = cmd.color;
        uint8_t a = (color >> 24) & 0xFF;
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;

        auto addGlyph = [&](float px, float py, unsigned char c, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) {
            float u0, v0, u1, v1;
            GetGlyphUV(c, u0, v0, u1, v1);

            int glyphW = m_glyphWidths[c];
            float w = static_cast<float>(glyphW);
            float h = static_cast<float>(GLYPH_CELL_SIZE);

            glm::vec2 tl = { px, py };
            glm::vec2 tr = { px + w, py };
            glm::vec2 br = { px + w, py + h };
            glm::vec2 bl = { px, py + h };

            // Apply 2D transform
            tl = glm::vec2(cmd.transform[0][0] * tl.x + cmd.transform[1][0] * tl.y + cmd.transform[2][0],
                           cmd.transform[0][1] * tl.x + cmd.transform[1][1] * tl.y + cmd.transform[2][1]);
            tr = glm::vec2(cmd.transform[0][0] * tr.x + cmd.transform[1][0] * tr.y + cmd.transform[2][0],
                           cmd.transform[0][1] * tr.x + cmd.transform[1][1] * tr.y + cmd.transform[2][1]);
            br = glm::vec2(cmd.transform[0][0] * br.x + cmd.transform[1][0] * br.y + cmd.transform[2][0],
                           cmd.transform[0][1] * br.x + cmd.transform[1][1] * br.y + cmd.transform[2][1]);
            bl = glm::vec2(cmd.transform[0][0] * bl.x + cmd.transform[1][0] * bl.y + cmd.transform[2][0],
                           cmd.transform[0][1] * bl.x + cmd.transform[1][1] * bl.y + cmd.transform[2][1]);

            // Adjust u1 to match actual glyph width (not full cell)
            float uWidth = (u1 - u0) * (static_cast<float>(glyphW) / GLYPH_CELL_SIZE);
            u1 = u0 + uWidth;

            uint32_t base = static_cast<uint32_t>(vertices.size());
            vertices.push_back({ tl.x, tl.y, 0.0f, u0, v0, cr, cg, cb, ca });
            vertices.push_back({ tr.x, tr.y, 0.0f, u1, v0, cr, cg, cb, ca });
            vertices.push_back({ br.x, br.y, 0.0f, u1, v1, cr, cg, cb, ca });
            vertices.push_back({ bl.x, bl.y, 0.0f, u0, v1, cr, cg, cb, ca });

            indices.push_back(base + 0);
            indices.push_back(base + 1);
            indices.push_back(base + 2);
            indices.push_back(base + 0);
            indices.push_back(base + 2);
            indices.push_back(base + 3);
        };

        float cursorX = cmd.x;
        float cursorY = cmd.y;
        bool bold = false;
        bool italic = false;

        // Drop shadow pass first (if enabled)
        if (cmd.dropShadow) {
            float sx = cursorX;
            for (size_t i = 0; i < cmd.text.size(); i++) {
                unsigned char c = static_cast<unsigned char>(cmd.text[i]);

                // Handle MC formatting codes (§)
                if (c == 0xC2 && i + 1 < cmd.text.size() && static_cast<unsigned char>(cmd.text[i + 1]) == 0xA7) {
                    i += 2; // Skip § (UTF-8: C2 A7)
                    if (i < cmd.text.size()) i++; // Skip format code char
                    continue;
                }

                if (c < 32 || c > 126) { sx += 4; continue; }

                // Shadow color: 25% of original brightness
                uint8_t sr = r / 4, sg = g / 4, sb = b / 4;
                addGlyph(sx + 1.0f, cursorY + 1.0f, c, sr, sg, sb, a);
                sx += m_glyphWidths[c] + 1;
            }
        }

        // Main text pass
        cursorX = cmd.x;
        uint8_t cr = r, cg = g, cb = b, ca = a;

        for (size_t i = 0; i < cmd.text.size(); i++) {
            unsigned char c = static_cast<unsigned char>(cmd.text[i]);

            // Handle MC formatting codes (§ = UTF-8 C2 A7)
            if (c == 0xC2 && i + 1 < cmd.text.size() && static_cast<unsigned char>(cmd.text[i + 1]) == 0xA7) {
                i += 1; // Skip 0xA7
                if (i + 1 < cmd.text.size()) {
                    i++;
                    char code = cmd.text[i];
                    if (code == 'r' || code == 'R') {
                        // Reset
                        cr = r; cg = g; cb = b; bold = false; italic = false;
                    } else if (code == 'l' || code == 'L') {
                        bold = true;
                    } else if (code == 'o' || code == 'O') {
                        italic = true;
                    } else {
                        uint32_t fmtColor = GetFormattingColor(code);
                        if (fmtColor != 0) {
                            ca = (fmtColor >> 24) & 0xFF;
                            cr = (fmtColor >> 16) & 0xFF;
                            cg = (fmtColor >> 8) & 0xFF;
                            cb = fmtColor & 0xFF;
                        }
                    }
                }
                continue;
            }

            if (c < 32 || c > 126) { cursorX += 4; continue; }

            addGlyph(cursorX, cursorY, c, cr, cg, cb, ca);
            cursorX += m_glyphWidths[c] + 1;
            if (bold) cursorX += 1; // Bold adds 1px extra
        }
    }

} // namespace Render
