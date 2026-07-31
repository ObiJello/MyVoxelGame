// File: src/client/renderer/gui/screens/Screen.cpp
#include "Screen.hpp"
#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include "../../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include "../../ext/stb_image/stb_image.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <filesystem>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    // ── Shared menu textures (loaded once, kept for process lifetime) ──────
    // menu_background.png + the header/footer separator strips. These live
    // outside the sprites atlas so they get their own backend textures,
    // loaded with the same stb pattern as InventoryScreen::EnsureBackground.
    TextureHandle LoadStandaloneGuiTexture(const char* relPath, int& outW, int& outH) {
            if (!g_renderBackend) return INVALID_TEXTURE;
            const std::string full = PlatformMain::GetAssetPath(relPath);
            if (!std::filesystem::exists(full)) {
                Log::Warning("[Screen] Menu texture not found: %s", full.c_str());
                return INVALID_TEXTURE;
            }
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("[Screen] stbi_load failed for %s: %s", relPath, stbi_failure_reason());
                return INVALID_TEXTURE;
            }
            TextureHandle t = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (t != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(t, TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap(t, TextureWrap::Repeat, TextureWrap::Repeat);
                outW = w; outH = h;
            }
            return t;
    }

    namespace {
        struct MenuTextures {
            TextureHandle background = INVALID_TEXTURE;
            TextureHandle headerSep  = INVALID_TEXTURE;
            TextureHandle footerSep  = INVALID_TEXTURE;
            bool loaded = false;

            void EnsureLoaded() {
                if (loaded) return;
                loaded = true;
                int w, h;
                background = LoadStandaloneGuiTexture("assets/textures/gui/menu_background.png", w, h);
                headerSep  = LoadStandaloneGuiTexture("assets/textures/gui/header_separator.png", w, h);
                footerSep  = LoadStandaloneGuiTexture("assets/textures/gui/footer_separator.png", w, h);
            }
        };
        MenuTextures s_menuTextures;
    } // namespace

    void RenderMenuBackgroundTexture(GuiGraphics& g, int x0, int y0, int x1, int y1) {
        s_menuTextures.EnsureLoaded();
        if (s_menuTextures.background == INVALID_TEXTURE) {
            g.Fill(x0, y0, x1, y1, 0xFF202020);
            return;
        }
        // MC Screen.renderMenuBackgroundTexture: the 16×16 texture is drawn
        // tiled at 32×32 GUI pixels. Repeat-wrap lets one blit cover the
        // whole area with fractional UVs.
        const float tile = 32.0f;
        g.Blit(s_menuTextures.background, x0, y0, x1, y1,
               0.0f, 0.0f,
               static_cast<float>(x1 - x0) / tile,
               static_cast<float>(y1 - y0) / tile);
    }

    void RenderMenuSeparators(GuiGraphics& g, int width, int headerY, int footerY) {
        s_menuTextures.EnsureLoaded();
        // 2px-tall strips tiled horizontally (32px period).
        if (s_menuTextures.headerSep != INVALID_TEXTURE) {
            g.Blit(s_menuTextures.headerSep, 0, headerY, width, headerY + 2,
                   0.0f, 0.0f, static_cast<float>(width) / 32.0f, 1.0f);
        } else {
            g.Fill(0, headerY, width, headerY + 2, 0xFF000000);
        }
        if (s_menuTextures.footerSep != INVALID_TEXTURE) {
            g.Blit(s_menuTextures.footerSep, 0, footerY, width, footerY + 2,
                   0.0f, 0.0f, static_cast<float>(width) / 32.0f, 1.0f);
        } else {
            g.Fill(0, footerY, width, footerY + 2, 0xFF000000);
        }
    }

    // ── Screen ──────────────────────────────────────────────────────────────

    void Screen::Resize(int guiWidth, int guiHeight) {
        m_width  = guiWidth;
        m_height = guiHeight;
        ClearWidgets();
        Init();
    }

    void Screen::ClearWidgets() {
        m_widgets.clear();
        m_focusIndex   = -1;
        m_activeWidget = nullptr;
    }

    AbstractWidget* Screen::FocusedWidget() {
        if (m_focusIndex < 0 || m_focusIndex >= static_cast<int>(m_widgets.size()))
            return nullptr;
        return m_widgets[m_focusIndex].get();
    }

    void Screen::SetFocus(AbstractWidget* w) {
        if (AbstractWidget* prev = FocusedWidget()) prev->SetFocused(false);
        m_focusIndex = -1;
        for (size_t i = 0; i < m_widgets.size(); ++i) {
            if (m_widgets[i].get() == w) {
                m_focusIndex = static_cast<int>(i);
                w->SetFocused(true);
                break;
            }
        }
    }

    void Screen::CycleFocus(int direction) {
        const int n = static_cast<int>(m_widgets.size());
        if (n == 0) return;
        if (AbstractWidget* prev = FocusedWidget()) prev->SetFocused(false);
        int idx = m_focusIndex;
        for (int step = 0; step < n; ++step) {
            idx = ((idx + direction) % n + n) % n;
            AbstractWidget* w = m_widgets[idx].get();
            if (w->active && w->visible) {
                m_focusIndex = idx;
                w->SetFocused(true);
                return;
            }
        }
        m_focusIndex = -1;
    }

    void Screen::RenderBackground(GuiGraphics& g, int, int, float) {
        // In a live world (pause-menu flow): MC renderTransparentBackground —
        // a dark gradient over the still-rendering world. Otherwise (title
        // flow): menu_background.png tiled at 32×32 GUI pixels.
        if (m_manager && m_manager->IsInWorld()) {
            g.FillGradient(0, 0, m_width, m_height, 0xC0101010, 0xD0101010);
        } else {
            RenderMenuBackgroundTexture(g, 0, 0, m_width, m_height);
        }
    }

    void Screen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        RenderBackground(g, mouseX, mouseY, partialTick);
        for (auto& w : m_widgets) {
            w->Render(g, mouseX, mouseY, partialTick);
        }

        // Hover tooltip (drawn last, on top). TooltipAt lets containers
        // (OptionsList) delegate to the child under the cursor.
        for (auto& w : m_widgets) {
            const std::vector<std::string>* tip = w->TooltipAt(mouseX, mouseY);
            if (!tip) continue;
            // The renderer batches within a stratum (fills vs sprites vs text
            // each sort independently), so submission order alone doesn't put
            // the tooltip above widget sprites — hop to the next stratum,
            // like MC's tooltip render pass.
            g.NextStratum();
            const auto& lines = *tip;
            int maxW = 0;
            for (const auto& line : lines)
                maxW = std::max(maxW, g.GetStringWidth(line));
            int th = static_cast<int>(lines.size()) * (FontRenderer::LINE_HEIGHT + 1);
            int tx = mouseX + 12;
            int ty = mouseY - 12;
            if (tx + maxW + 8 > m_width) tx = m_width - maxW - 8;
            if (ty + th + 8 > m_height)  ty = m_height - th - 8;
            if (tx < 0) tx = 0;
            if (ty < 0) ty = 0;
            // Vanilla tooltip chrome: dark purple-bordered panel.
            g.Fill(tx - 3, ty - 3, tx + maxW + 3, ty + th + 3, 0xF0100010);
            g.RenderOutline(tx - 3, ty - 3, maxW + 6, th + 6, 0xFF250559);
            int ly = ty;
            for (const auto& line : lines) {
                g.DrawString(line, tx, ly, 0xFFFFFFFF);
                ly += FontRenderer::LINE_HEIGHT + 1;
            }
            break;
        }
    }

    bool Screen::MouseClicked(double mx, double my, int button) {
        if (button != GLFW_MOUSE_BUTTON_LEFT) return false;
        for (auto& w : m_widgets) {
            if (w->IsMouseOver(mx, my)) {
                SetFocus(w.get());
                m_activeWidget = w.get();
                w->OnClick(mx, my);
                return true;
            }
        }
        // Click on empty space: clear focus (unfocuses EditBoxes).
        if (AbstractWidget* prev = FocusedWidget()) prev->SetFocused(false);
        m_focusIndex = -1;
        return false;
    }

    bool Screen::MouseReleased(double mx, double my, int button) {
        if (button != GLFW_MOUSE_BUTTON_LEFT) return false;
        if (m_activeWidget) {
            m_activeWidget->OnRelease(mx, my);
            m_activeWidget = nullptr;
            return true;
        }
        return false;
    }

    void Screen::MouseDragged(double mx, double my) {
        if (m_activeWidget) m_activeWidget->OnDrag(mx, my);
    }

    bool Screen::MouseScrolled(double mx, double my, double deltaY) {
        for (auto& w : m_widgets) {
            if (w->ContainsPoint(mx, my) && w->OnScroll(deltaY)) return true;
        }
        return false;
    }

    bool Screen::KeyPressed(int glfwKey, int glfwMods) {
        if (glfwKey == GLFW_KEY_ESCAPE && ShouldCloseOnEsc()) {
            OnClose();
            return true;
        }
        if (glfwKey == GLFW_KEY_TAB) {
            CycleFocus((glfwMods & GLFW_MOD_SHIFT) ? -1 : +1);
            return true;
        }
        if (AbstractWidget* w = FocusedWidget()) {
            if (w->KeyPressed(glfwKey, glfwMods)) return true;
        }
        return false;
    }

    bool Screen::CharTyped(unsigned int codepoint) {
        if (AbstractWidget* w = FocusedWidget()) {
            return w->CharTyped(codepoint);
        }
        return false;
    }

    void Screen::OnClose() {
        if (m_manager) m_manager->Pop();
    }

    // ── ScreenManager ───────────────────────────────────────────────────────

    ScreenManager& GetScreenManager() {
        static ScreenManager s_instance;
        return s_instance;
    }

    void ScreenManager::Push(std::unique_ptr<Screen> screen) {
        PendingOp op;
        op.kind = PendingOp::Kind::Push;
        op.screen = std::move(screen);
        m_pending.push_back(std::move(op));
    }

    void ScreenManager::Pop() {
        PendingOp op;
        op.kind = PendingOp::Kind::Pop;
        m_pending.push_back(std::move(op));
    }

    void ScreenManager::Set(std::unique_ptr<Screen> screen) {
        PendingOp op;
        op.kind = PendingOp::Kind::Set;
        op.screen = std::move(screen);
        m_pending.push_back(std::move(op));
    }

    void ScreenManager::Clear() {
        PendingOp op;
        op.kind = PendingOp::Kind::Clear;
        m_pending.push_back(std::move(op));
    }

    void ScreenManager::Update(int guiWidth, int guiHeight) {
        bool stackChanged = !m_pending.empty();
        for (auto& op : m_pending) {
            switch (op.kind) {
                case PendingOp::Kind::Push:
                    op.screen->SetManager(this);
                    m_stack.push_back(std::move(op.screen));
                    break;
                case PendingOp::Kind::Pop:
                    if (!m_stack.empty()) m_stack.pop_back();
                    break;
                case PendingOp::Kind::Set:
                    m_stack.clear();
                    op.screen->SetManager(this);
                    m_stack.push_back(std::move(op.screen));
                    break;
                case PendingOp::Kind::Clear:
                    m_stack.clear();
                    break;
            }
        }
        m_pending.clear();

        Screen* top = Current();
        if (!top) return;

        if (stackChanged || guiWidth != m_lastWidth || guiHeight != m_lastHeight) {
            top->Resize(guiWidth, guiHeight);
        }
        m_lastWidth  = guiWidth;
        m_lastHeight = guiHeight;
    }

    void ScreenManager::Tick() {
        if (Screen* top = Current()) top->Tick();
    }

    void ScreenManager::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        if (Screen* top = Current()) top->Render(g, mouseX, mouseY, partialTick);
    }

    void ScreenManager::MouseClicked(double mx, double my, int button) {
        if (Screen* top = Current()) top->MouseClicked(mx, my, button);
    }
    void ScreenManager::MouseReleased(double mx, double my, int button) {
        if (Screen* top = Current()) top->MouseReleased(mx, my, button);
    }
    void ScreenManager::MouseDragged(double mx, double my) {
        if (Screen* top = Current()) top->MouseDragged(mx, my);
    }
    void ScreenManager::MouseScrolled(double mx, double my, double deltaY) {
        if (Screen* top = Current()) top->MouseScrolled(mx, my, deltaY);
    }
    void ScreenManager::KeyPressed(int glfwKey, int glfwMods) {
        if (Screen* top = Current()) top->KeyPressed(glfwKey, glfwMods);
    }
    void ScreenManager::CharTyped(unsigned int codepoint) {
        if (Screen* top = Current()) top->CharTyped(codepoint);
    }

} // namespace Render
