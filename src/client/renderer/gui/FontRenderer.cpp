// File: src/client/renderer/gui/FontRenderer.cpp
#include "FontRenderer.hpp"
#include "GuiRenderer.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include <filesystem>

#include "../ext/stb_image/stb_image.h"

namespace Render {

    FontRenderer::~FontRenderer() {
        Shutdown();
    }

    bool FontRenderer::Initialize(const std::string& fontAtlasPath) {
        if (!g_renderBackend) return false;

        if (!std::filesystem::exists(fontAtlasPath)) {
            Log::Error("[FontRenderer] Font atlas not found: %s", fontAtlasPath.c_str());
            return false;
        }

        // Load font atlas PNG — no vertical flip for GUI (ortho has Y top-to-bottom)
        stbi_set_flip_vertically_on_load(0);

        int width = 0, height = 0, channels = 0;
        unsigned char* pixels = stbi_load(fontAtlasPath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (!pixels) {
            Log::Error("[FontRenderer] Failed to load font atlas: %s", stbi_failure_reason());
            return false;
        }

        m_fontTexWidth = width;
        m_fontTexHeight = height;

        // Detect glyph widths from pixel data
        DetectGlyphWidths(pixels, width, height);

        // Upload to GPU
        m_fontTexture = g_renderBackend->CreateTexture2D(width, height, TextureFormat::RGBA8, pixels);
        stbi_image_free(pixels);

        if (m_fontTexture == INVALID_TEXTURE) {
            Log::Error("[FontRenderer] Failed to create font texture");
            return false;
        }

        g_renderBackend->SetTextureFilter(m_fontTexture, TextureFilter::Nearest, TextureFilter::Nearest);
        g_renderBackend->SetTextureWrap(m_fontTexture, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        Log::Info("[FontRenderer] Initialized with %dx%d font atlas", width, height);
        return true;
    }

    void FontRenderer::Shutdown() {
        if (g_renderBackend && m_fontTexture != INVALID_TEXTURE) {
            g_renderBackend->DestroyTexture(m_fontTexture);
            m_fontTexture = INVALID_TEXTURE;
        }
    }

    void FontRenderer::DetectGlyphWidths(const unsigned char* pixels, int width, int height) {
        // MC's BitmapProvider: scan each glyph cell right-to-left for first non-transparent column
        int cellW = width / GLYPH_GRID;
        int cellH = height / GLYPH_GRID;

        for (int charIndex = 0; charIndex < 256; charIndex++) {
            int gridX = charIndex % GLYPH_GRID;
            int gridY = charIndex / GLYPH_GRID;
            int cellStartX = gridX * cellW;
            int cellStartY = gridY * cellH;

            // Default width
            m_glyphWidths[charIndex] = 0;

            // Space character
            if (charIndex == 32) {
                m_glyphWidths[charIndex] = 4;
                continue;
            }

            // Scan right-to-left for rightmost non-transparent pixel
            int maxCol = 0;
            bool hasPixels = false;
            for (int y = 0; y < cellH; y++) {
                for (int x = cellW - 1; x >= 0; x--) {
                    int px = cellStartX + x;
                    int py = cellStartY + y;
                    int idx = (py * width + px) * 4;
                    if (pixels[idx + 3] > 0) { // Alpha > 0
                        if (x + 1 > maxCol) maxCol = x + 1;
                        hasPixels = true;
                        break; // Found rightmost in this row
                    }
                }
            }

            m_glyphWidths[charIndex] = hasPixels ? maxCol : 0; // maxCol is already rightmost+1 = width
        }
    }

    int FontRenderer::GetStringWidth(const std::string& text) const {
        int width = 0;
        for (size_t i = 0; i < text.size(); i++) {
            unsigned char c = static_cast<unsigned char>(text[i]);

            // Skip MC formatting codes (§ = UTF-8 C2 A7)
            if (c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xA7) {
                i += 2; // Skip § + format code
                continue;
            }

            if (c < 32 || c > 126) {
                width += 4; // Unknown chars get space width
                continue;
            }
            width += m_glyphWidths[c] + 1; // +1 for inter-character spacing
        }
        if (width > 0) width -= 1; // Remove trailing spacing
        return width;
    }

    void FontRenderer::GetGlyphUV(unsigned char c, float& u0, float& v0, float& u1, float& v1) const {
        int gridX = c % GLYPH_GRID;
        int gridY = c / GLYPH_GRID;
        float cellW = 1.0f / GLYPH_GRID;
        float cellH = 1.0f / GLYPH_GRID;

        u0 = gridX * cellW;
        v0 = gridY * cellH;
        u1 = (gridX + 1) * cellW;
        v1 = (gridY + 1) * cellH;
    }

    // Note: GuiRenderer calls GenerateQuadsTyped<GuiVertex>() directly (template in header)

    uint32_t FontRenderer::GetFormattingColor(char code) {
        // MC's 16 formatting colors (§0 through §f)
        switch (code) {
            case '0': return 0xFF000000; // Black
            case '1': return 0xFF0000AA; // Dark Blue
            case '2': return 0xFF00AA00; // Dark Green
            case '3': return 0xFF00AAAA; // Dark Aqua
            case '4': return 0xFFAA0000; // Dark Red
            case '5': return 0xFFAA00AA; // Dark Purple
            case '6': return 0xFFFFAA00; // Gold
            case '7': return 0xFFAAAAAA; // Gray
            case '8': return 0xFF555555; // Dark Gray
            case '9': return 0xFF5555FF; // Blue
            case 'a': case 'A': return 0xFF55FF55; // Green
            case 'b': case 'B': return 0xFF55FFFF; // Aqua
            case 'c': case 'C': return 0xFFFF5555; // Red
            case 'd': case 'D': return 0xFFFF55FF; // Light Purple
            case 'e': case 'E': return 0xFFFFFF55; // Yellow
            case 'f': case 'F': return 0xFFFFFFFF; // White
            default: return 0;
        }
    }

} // namespace Render
