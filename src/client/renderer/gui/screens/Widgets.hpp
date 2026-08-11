// File: src/client/renderer/gui/screens/Widgets.hpp
//
// Menu widget toolkit — the C++ counterpart of MC's
// gui/components/AbstractWidget family (Button, AbstractSliderButton,
// CycleButton, EditBox, StringWidget). Rendered entirely through
// GuiGraphics sprites ("widget/button", "widget/slider", …) so visuals
// match vanilla pixel-for-pixel, including the nine-slice borders that
// come from the sprites' .mcmeta files (GuiAtlas handles those).
//
// Widgets are owned by a Screen (screens/Screen.hpp) which routes input
// to them. All callbacks are std::function so screens can bind lambdas
// against Platform::g_gameSettings or their own state.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Render {

    class GuiGraphics;

    // ── Shared layout constants (MC AbstractWidget / Button) ────────────────
    namespace WidgetDims {
        constexpr int BUTTON_WIDTH       = 200; // full-width menu button
        constexpr int SMALL_BUTTON_WIDTH = 150; // options-list column button
        constexpr int BUTTON_HEIGHT      = 20;
        constexpr int TEXT_COLOR_ACTIVE   = 0xFFFFFFFF;
        constexpr int TEXT_COLOR_INACTIVE = 0xFFA0A0A0;
    }

    // Multiply an ARGB color's alpha channel by `alpha` (0..1). Used for the
    // title-screen fade-in, mirroring MC's ARGB.white(alpha) tinting.
    uint32_t ApplyAlpha(uint32_t argb, float alpha);

    class AbstractWidget {
    public:
        AbstractWidget(int x, int y, int width, int height, std::string message)
            : m_x(x), m_y(y), m_width(width), m_height(height),
              m_message(std::move(message)) {}
        virtual ~AbstractWidget() = default;

        // ── Geometry / state ───────────────────────────────────────────────
        int  GetX() const { return m_x; }
        int  GetY() const { return m_y; }
        int  GetWidth() const { return m_width; }
        int  GetHeight() const { return m_height; }
        void SetPosition(int x, int y) { m_x = x; m_y = y; }
        void SetWidth(int w) { m_width = w; }

        const std::string& GetMessage() const { return m_message; }
        void SetMessage(std::string msg) { m_message = std::move(msg); }

        bool active  = true;   // false → greyed out, ignores clicks (MC `active`)
        bool visible = true;

        bool IsHovered() const { return m_hovered; }
        bool IsFocused() const { return m_focused; }
        void SetFocused(bool f) { m_focused = f; }

        // Widget-fade alpha (title-screen fade-in). 0..1, multiplied into all
        // sprite/text colors at render time.
        void  SetAlpha(float a) { m_alpha = a; }
        float GetAlpha() const { return m_alpha; }

        // Optional hover tooltip. Multiple lines supported; Screen renders it.
        void SetTooltip(std::vector<std::string> lines) { m_tooltip = std::move(lines); }
        const std::vector<std::string>& GetTooltip() const { return m_tooltip; }

        // Tooltip lookup for the given mouse position. Containers (OptionsList)
        // override to delegate to the child under the cursor. Non-const:
        // containers lay out children on demand.
        virtual const std::vector<std::string>* TooltipAt(double mx, double my) {
            return (ContainsPoint(mx, my) && !m_tooltip.empty()) ? &m_tooltip : nullptr;
        }

        bool IsMouseOver(double mx, double my) const {
            return visible && active &&
                   mx >= m_x && mx < m_x + m_width &&
                   my >= m_y && my < m_y + m_height;
        }
        // Hit test ignoring `active` — inactive widgets still show tooltips.
        bool ContainsPoint(double mx, double my) const {
            return visible &&
                   mx >= m_x && mx < m_x + m_width &&
                   my >= m_y && my < m_y + m_height;
        }

        // ── Per-frame ──────────────────────────────────────────────────────
        // Updates hover state then calls RenderWidget.
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick);

        // ── Input (Screen routes these) ────────────────────────────────────
        virtual void OnClick(double mouseX, double mouseY) { (void)mouseX; (void)mouseY; }
        virtual void OnRelease(double mouseX, double mouseY) { (void)mouseX; (void)mouseY; }
        virtual void OnDrag(double mouseX, double mouseY) { (void)mouseX; (void)mouseY; }
        virtual bool OnScroll(double /*deltaY*/) { return false; }
        // Returns true if consumed. Called only on the focused widget.
        virtual bool KeyPressed(int /*glfwKey*/, int /*glfwMods*/) { return false; }
        virtual bool CharTyped(unsigned int /*codepoint*/) { return false; }

    protected:
        virtual void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) = 0;

        // Standard vanilla button chrome: nine-sliced sprite + centered label.
        // Sprite choice mirrors MC Button.getTextureY(): disabled → _disabled,
        // hovered-or-focused → _highlighted, else base.
        void RenderButtonChrome(GuiGraphics& g, const std::string& label) const;

        int m_x, m_y, m_width, m_height;
        std::string m_message;
        bool  m_hovered = false;
        bool  m_focused = false;
        float m_alpha   = 1.0f;
        std::vector<std::string> m_tooltip;
    };

    // ── Button — MC gui/components/Button ───────────────────────────────────
    class Button : public AbstractWidget {
    public:
        using OnPress = std::function<void()>;

        Button(int x, int y, int width, int height, std::string message, OnPress onPress)
            : AbstractWidget(x, y, width, height, std::move(message)),
              m_onPress(std::move(onPress)) {}

        void OnClick(double, double) override { if (m_onPress) m_onPress(); }
        // Lets a screen attach the handler after construction, when the
        // callback needs to capture the button itself.
        void SetOnPress(OnPress onPress) { m_onPress = std::move(onPress); }
        bool KeyPressed(int glfwKey, int glfwMods) override;

    protected:
        void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

    private:
        OnPress m_onPress;
    };

    // ── SliderButton — MC gui/components/AbstractSliderButton ───────────────
    // Normalized value in [0,1]; the owner supplies label formatting and the
    // apply callback (fired whenever the value changes, MC applyValue()).
    class SliderButton : public AbstractWidget {
    public:
        using Format = std::function<std::string(double normValue)>;
        using Apply  = std::function<void(double normValue)>;

        SliderButton(int x, int y, int width, int height,
                     double initialNorm, Format format, Apply apply,
                     double keyStep = 0.0)
            : AbstractWidget(x, y, width, height, ""),
              m_value(initialNorm < 0.0 ? 0.0 : (initialNorm > 1.0 ? 1.0 : initialNorm)),
              m_format(std::move(format)), m_apply(std::move(apply)),
              m_keyStep(keyStep) {
            if (m_format) m_message = m_format(m_value);
        }

        double GetValue() const { return m_value; }

        void OnClick(double mouseX, double mouseY) override;
        void OnDrag(double mouseX, double mouseY) override;
        void OnRelease(double mouseX, double mouseY) override;
        bool KeyPressed(int glfwKey, int glfwMods) override;

    protected:
        void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

    private:
        void SetValueFromMouse(double mouseX);
        void SetValue(double v);

        double m_value;
        Format m_format;
        Apply  m_apply;
        // Keyboard left/right step in normalized units. 0 → 1/(width-8)
        // per press (pixel-equivalent, MC's default behaviour).
        double m_keyStep;
    };

    // ── CycleButton — MC gui/components/CycleButton (string-valued) ─────────
    // Clicking advances through `values`; shift-click goes backwards (MC).
    class CycleButton : public AbstractWidget {
    public:
        using OnChange = std::function<void(int index)>;

        CycleButton(int x, int y, int width, int height,
                    std::string caption, std::vector<std::string> values,
                    int initialIndex, OnChange onChange)
            : AbstractWidget(x, y, width, height, ""),
              m_caption(std::move(caption)), m_values(std::move(values)),
              m_index(initialIndex), m_onChange(std::move(onChange)) {
            if (m_values.empty()) m_values.push_back("");
            if (m_index < 0 || m_index >= static_cast<int>(m_values.size())) m_index = 0;
            UpdateMessage();
        }

        int GetIndex() const { return m_index; }

        void OnClick(double mouseX, double mouseY) override;
        bool KeyPressed(int glfwKey, int glfwMods) override;

        // Convenience factory for ON/OFF toggles (MC CycleButton.onOffBuilder).
        static CycleButton* MakeOnOff(int x, int y, int width, int height,
                                      const std::string& caption, bool initial,
                                      std::function<void(bool)> onChange);

    protected:
        void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

    private:
        void Cycle(int delta);
        void UpdateMessage() { m_message = m_caption + ": " + m_values[m_index]; }

        std::string m_caption;
        std::vector<std::string> m_values;
        int m_index;
        OnChange m_onChange;
    };

    // ── EditBox — MC gui/components/EditBox (subset) ────────────────────────
    // Single-line text input: insert/delete, cursor movement, focus-gated
    // caret blink. No selection range (not needed by the menu screens yet).
    class EditBox : public AbstractWidget {
    public:
        EditBox(int x, int y, int width, int height, std::string label)
            : AbstractWidget(x, y, width, height, std::move(label)) {}

        const std::string& GetText() const { return m_text; }
        void SetText(const std::string& text);
        void SetMaxLength(int n) { m_maxLength = n; }
        // Grey hint drawn while empty and unfocused (MC setHint).
        void SetHint(std::string hint) { m_hint = std::move(hint); }
        void SetResponder(std::function<void(const std::string&)> fn) { m_responder = std::move(fn); }

        void OnClick(double mouseX, double mouseY) override;
        bool KeyPressed(int glfwKey, int glfwMods) override;
        bool CharTyped(unsigned int codepoint) override;

    protected:
        void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

    private:
        void InsertText(const std::string& s);
        void DeleteChars(int dir); // -1 backspace, +1 delete
        void NotifyChanged() { if (m_responder) m_responder(m_text); }

        std::string m_text;
        std::string m_hint;
        int m_cursorPos  = 0;
        int m_maxLength  = 128;
        std::function<void(const std::string&)> m_responder;
    };

    // ── StringWidget — MC gui/components/StringWidget ───────────────────────
    // Non-interactive centered label (screen titles, list headers).
    class StringWidget : public AbstractWidget {
    public:
        StringWidget(int x, int y, int width, int height, std::string message,
                     uint32_t color = 0xFFFFFFFF)
            : AbstractWidget(x, y, width, height, std::move(message)), m_color(color) {
            active = false; // never focus/click
        }

    protected:
        void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

    private:
        uint32_t m_color;
    };

    // ── PlainTextButton — MC gui/components/PlainTextButton ─────────────────
    // Text-only button (the copyright line on the title screen). Underlines
    // on hover like vanilla.
    class PlainTextButton : public Button {
    public:
        PlainTextButton(int x, int y, int width, int height, std::string message, OnPress onPress)
            : Button(x, y, width, height, std::move(message), std::move(onPress)) {}

    protected:
        void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
    };

    // ── TabButton — MC gui/components/tabs/TabButton ────────────────────────
    // Header tab for tabbed screens (Create World). Uses the widget/tab
    // sprites; `selected` picks the merged-with-content variant.
    class TabButton : public Button {
    public:
        TabButton(int x, int y, int width, int height, std::string label, OnPress onPress)
            : Button(x, y, width, height, std::move(label), std::move(onPress)) {}

        bool selected = false;

    protected:
        void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
    };

    // ── OptionsList — MC gui/components/OptionsList ─────────────────────────
    // The scrolling two-column widget container used by every options
    // sub-screen. Owns its child widgets. Rows are either one full-width
    // (310px, addBig), two half-width (150px each, addSmall), or a header
    // string. Handles wheel scrolling, the right-edge scrollbar, and
    // scissored rendering between header and footer.
    class OptionsList : public AbstractWidget {
    public:
        static constexpr int ROW_HEIGHT   = 25;
        static constexpr int ROW_WIDTH    = 310; // 150 + 10 gap + 150
        static constexpr int SCROLLBAR_W  = 6;

        OptionsList(int x, int y, int width, int height)
            : AbstractWidget(x, y, width, height, "") {}

        // Ownership transfers to the list. Pass nullptr for an empty cell.
        void AddBig(AbstractWidget* w);
        void AddSmall(AbstractWidget* left, AbstractWidget* right);
        void AddHeader(const std::string& text);

        // Input routed from the owning Screen. Coordinates are screen-space.
        AbstractWidget* HitTest(double mx, double my);
        const std::vector<std::string>* TooltipAt(double mx, double my) override;
        void OnClick(double mouseX, double mouseY) override;
        void OnDrag(double mouseX, double mouseY) override;
        void OnRelease(double mouseX, double mouseY) override;
        bool OnScroll(double deltaY) override;

        // The child currently being interacted with (mouse held down on it).
        AbstractWidget* GetDraggedChild() { return m_draggedChild; }

        // Focus traversal support for the owning Screen.
        const std::vector<std::unique_ptr<AbstractWidget>>& Children() const { return m_children; }

    protected:
        void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

    private:
        struct Row {
            AbstractWidget* left  = nullptr;  // also "big" (spans both columns)
            AbstractWidget* right = nullptr;
            std::string header;               // non-empty → header row
            bool big = false;
        };

        double MaxScroll() const;
        void   ClampScroll();
        void   LayoutRow(const Row& row, int rowTop);
        bool   ScrollbarVisible() const { return MaxScroll() > 0.0; }
        int    ScrollbarX() const { return m_x + m_width / 2 + ROW_WIDTH / 2 + 10; }

        std::vector<Row> m_rows;
        std::vector<std::unique_ptr<AbstractWidget>> m_children;
        double m_scroll = 0.0;
        AbstractWidget* m_draggedChild = nullptr;
        bool m_draggingScrollbar = false;
        double m_scrollbarGrabOffset = 0.0;
    };

} // namespace Render
