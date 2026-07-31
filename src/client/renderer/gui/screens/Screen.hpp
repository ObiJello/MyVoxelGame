// File: src/client/renderer/gui/screens/Screen.hpp
//
// Menu screen base class + the screen stack — the C++ counterpart of MC's
// gui/screens/Screen and Minecraft.setScreen(). Screens own their widgets
// and rebuild them on resize (MC's init() pattern). The ScreenManager holds
// a STACK instead of MC's lastScreen back-pointers: opening a sub-screen
// pushes, "Done"/ESC pops back to the parent. Stack mutations requested
// from inside widget callbacks are deferred to the next Update() so a
// button can safely destroy its own screen.
//
// The manager is render-loop agnostic: PlatformMain (title phase or a
// future in-game pause menu) feeds it input each frame and calls Render
// with a prepared GuiGraphics.
#pragma once

#include "Widgets.hpp"
#include "../../backend/RenderTypes.hpp"   // TextureHandle for the loader helper
#include <memory>
#include <string>
#include <vector>

namespace Render {

    class GuiGraphics;
    class ScreenManager;

    class Screen {
    public:
        explicit Screen(std::string title) : m_title(std::move(title)) {}
        virtual ~Screen() = default;

        // Called by the manager on first show and window resize. Clears all
        // widgets and calls Init() to rebuild layout at the new size.
        void Resize(int guiWidth, int guiHeight);

        // Build widgets for the current width/height. Mirrors MC init().
        virtual void Init() = 0;

        // ~20Hz logic tick (caret blink is time-based; this is for screens
        // that need it — MC Screen.tick()).
        virtual void Tick() {}

        // Render background + widgets + tooltip.
        virtual void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick);

        // Default: dirt-era replacement — tiled menu_background.png (MC
        // Screen.renderMenuBackground). Screens over a live world would use
        // the transparent gradient instead (not needed for the title flow).
        virtual void RenderBackground(GuiGraphics& g, int mouseX, int mouseY, float partialTick);

        // ── Input (returns true when consumed) ─────────────────────────────
        virtual bool MouseClicked(double mx, double my, int button);
        virtual bool MouseReleased(double mx, double my, int button);
        virtual void MouseDragged(double mx, double my);
        virtual bool MouseScrolled(double mx, double my, double deltaY);
        virtual bool KeyPressed(int glfwKey, int glfwMods);
        virtual bool CharTyped(unsigned int codepoint);

        virtual bool ShouldCloseOnEsc() const { return true; }
        // ESC / Done. Default pops this screen off the stack.
        virtual void OnClose();

        const std::string& GetTitle() const { return m_title; }

        void SetManager(ScreenManager* m) { m_manager = m; }

    protected:
        // Ownership stays with the screen; returns a borrowed pointer for
        // further configuration. (MC addRenderableWidget.)
        template <typename T>
        T* AddWidget(T* widget) {
            m_widgets.emplace_back(widget);
            return widget;
        }

        void ClearWidgets();

        // Focus helpers (Tab / Shift+Tab traversal).
        void CycleFocus(int direction);
        AbstractWidget* FocusedWidget();
        void SetFocus(AbstractWidget* w);

        std::string m_title;
        int m_width  = 0;
        int m_height = 0;
        ScreenManager* m_manager = nullptr;

        std::vector<std::unique_ptr<AbstractWidget>> m_widgets;
        int m_focusIndex = -1;
        // Widget the mouse button went down on — receives drag/release.
        AbstractWidget* m_activeWidget = nullptr;
    };

    // ── ScreenManager ───────────────────────────────────────────────────────
    class ScreenManager {
    public:
        // Stack ops are DEFERRED until the next Update() (safe from widget
        // callbacks). Push shows a sub-screen over the current one; Pop
        // returns to the parent; Set replaces the whole stack.
        void Push(std::unique_ptr<Screen> screen);
        void Pop();
        void Set(std::unique_ptr<Screen> screen);
        void Clear();

        Screen* Current() { return m_stack.empty() ? nullptr : m_stack.back().get(); }
        bool Empty() const { return m_stack.empty() && m_pending.empty(); }

        // Apply pending stack ops + propagate resize. Call once per frame
        // BEFORE input/render.
        void Update(int guiWidth, int guiHeight);

        // ~20Hz tick fanout.
        void Tick();

        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick);

        // Input fanout to the top screen.
        void MouseClicked(double mx, double my, int button);
        void MouseReleased(double mx, double my, int button);
        void MouseDragged(double mx, double my);
        void MouseScrolled(double mx, double my, double deltaY);
        void KeyPressed(int glfwKey, int glfwMods);
        void CharTyped(unsigned int codepoint);

        // ── One-shot "apply this setting now" flags ────────────────────────
        // Options screens set these; the host loop (PlatformMain) drains them
        // and pokes the engine (GLFW / render backend / server session).
        enum AppliedSetting : uint32_t {
            APPLY_VSYNC           = 1u << 0,
            APPLY_FULLSCREEN      = 1u << 1,
            APPLY_RENDER_DISTANCE = 1u << 2,
            APPLY_RAW_MOUSE       = 1u << 3,
            APPLY_MAX_FPS         = 1u << 4,
        };
        void MarkSettingApplied(uint32_t bit) { m_appliedBits |= bit; }
        uint32_t ConsumeAppliedSettings() { uint32_t b = m_appliedBits; m_appliedBits = 0; return b; }

        // Version string rendered by the title screen (set from PlatformMain
        // which owns GAME_VERSION).
        void SetVersionString(std::string v) { m_versionString = std::move(v); }
        const std::string& GetVersionString() const { return m_versionString; }

        // True while a live world renders behind the menus (pause-menu flow).
        // Screens then use the transparent gradient background instead of the
        // opaque tiled menu texture (MC renderTransparentBackground).
        void SetInWorld(bool inWorld) { m_inWorld = inWorld; }
        bool IsInWorld() const { return m_inWorld; }

    private:
        struct PendingOp {
            enum class Kind { Push, Pop, Set, Clear } kind;
            std::unique_ptr<Screen> screen;
        };

        std::vector<std::unique_ptr<Screen>> m_stack;
        std::vector<PendingOp> m_pending;
        int m_lastWidth  = -1;
        int m_lastHeight = -1;
        uint32_t m_appliedBits = 0;
        std::string m_versionString;
        bool m_inWorld = false;
    };

    // Global instance (mirrors GetInventoryScreen()).
    ScreenManager& GetScreenManager();

    // Shared vanilla menu chrome, usable by any screen:
    // menu_background.png tiled at 32×32 GUI px over [x0,y0)–(x1,y1).
    void RenderMenuBackgroundTexture(GuiGraphics& g, int x0, int y0, int x1, int y1);
    // The 2px header/footer separator strips at the given Y positions.
    void RenderMenuSeparators(GuiGraphics& g, int width, int headerY, int footerY);

    // Load a standalone GUI texture (outside the sprites atlas) with the
    // standard InventoryScreen stb pattern: nearest filter, un-flipped.
    // Returns INVALID_TEXTURE and logs on failure.
    TextureHandle LoadStandaloneGuiTexture(const char* relPath, int& outW, int& outH);

} // namespace Render
