// File: src/client/renderer/gui/screens/AdvancementsScreen.hpp
//
// MC AdvancementsScreen / AdvancementTab / AdvancementTabType /
// AdvancementWidget / AdvancementWidgetType: the Advancements screen opened
// from the pause menu or with the advancements key (L).
//
//   • The 252 x 140 window (textures/gui/advancements/window.png), its title
//     the selected tab's root title.
//   • One tab per root with a background, up to 8 above, 8 below, 5 left and
//     5 right (AdvancementTabType), each with its root's icon; clicking one
//     selects it (and tells the server, which remembers the last tab).
//   • The tab's inside (234 x 113): its background tiled, the connecting
//     lines (a black outline under white), and one widget per visible
//     advancement — the frame for its type (task / goal / challenge),
//     obtained or not, with its icon. Drag with the left button or use the
//     wheel to scroll a tree larger than the window.
//   • Hovering a widget fades the inside and shows the title bar — filled in
//     proportion to the progress — with the progress count and the
//     description in the type's colour, flipped left or up near the edges.
//
// Mirrors Client::ClientAdvancements as its Listener while open; closing
// tells the server (ServerboundSeenAdvancementsPacket.closedScreen).
#pragma once

#include "Screen.hpp"
#include "client/advancements/ClientAdvancements.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Render {

    class AdvancementTab;

    // MC AdvancementWidget.
    class AdvancementWidget {
    public:
        static constexpr int kBoxWidth = 200;

        AdvancementWidget(GuiGraphics& g, const Game::Advancements::Node& node);

        // Draw passes, `xo`/`yo` the absolute screen position of the tab's
        // scrolled origin.
        void RenderConnectivity(GuiGraphics& g, int xo, int yo, bool background) const;
        void Render(GuiGraphics& g, int xo, int yo) const;
        // `ox`/`oy`: the inside's absolute origin; `scrollX`/`scrollY`: the
        // tab's scroll; `screenXo`: the window's left (MC's leftPos).
        void RenderHover(GuiGraphics& g, int ox, int oy, int scrollX, int scrollY, float fade,
                         int screenXo, int screenWidth) const;

        // Relative to the inside's origin, as MC tests it.
        bool IsMouseOver(int scrollX, int scrollY, int mouseX, int mouseY) const;
        void AttachToParent(AdvancementTab& tab);
        void AddChild(AdvancementWidget* child) { m_children.push_back(child); }
        void SetProgress(const Game::Advancements::AdvancementProgress& progress) { m_progress = progress; }

        const Game::Advancements::Definition& GetAdvancement() const { return *m_node.def; }
        const Game::Advancements::DisplayInfo& GetDisplay() const { return *m_node.def->display; }
        int GetX() const { return m_x; }
        int GetY() const { return m_y; }

    private:
        bool IsVisible() const;
        int MaxProgressWidth(GuiGraphics& g) const;

        const Game::Advancements::Node& m_node;
        Game::ItemStack m_icon;
        std::vector<std::string> m_titleLines;
        std::vector<std::string> m_description;
        int m_width = 0;
        int m_x = 0;
        int m_y = 0;
        AdvancementWidget* m_parent = nullptr;
        std::vector<AdvancementWidget*> m_children;
        std::optional<Game::Advancements::AdvancementProgress> m_progress;
    };

    // MC AdvancementTabType.
    enum class AdvancementTabType : uint8_t { Above, Below, Left, Right };

    // MC AdvancementTab.
    class AdvancementTab {
    public:
        // MC AdvancementTab.create: null for a root without a display or a
        // background, or past the last tab slot.
        static std::unique_ptr<AdvancementTab> Create(GuiGraphics& g, int index, const Game::Advancements::Node& root);

        void CopyPosition(const AdvancementTab& source);
        void Tick(int relativeMouseX, int relativeMouseY);
        void RenderTab(GuiGraphics& g, int xo, int yo, bool selected) const;
        void RenderIcon(GuiGraphics& g, int xo, int yo) const;
        void RenderContents(GuiGraphics& g, int windowLeft, int windowTop);
        void RenderTooltips(GuiGraphics& g, int windowLeft, int windowTop, int screenWidth) const;
        bool IsMouseOver(int xo, int yo, double mx, double my) const;
        void Scroll(double dx, double dy);
        void AddAdvancement(GuiGraphics& g, const Game::Advancements::Node& node);
        AdvancementWidget* GetWidget(const std::string& id);

        const Game::Advancements::Definition& GetRootAdvancement() const { return m_root->GetAdvancement(); }
        // The root's id, held by value: a retired tab (the tree changed
        // under it) still answers it after its nodes are gone.
        const std::string& RootId() const { return m_rootId; }
        const std::string& GetTitle() const { return m_title; }

    private:
        AdvancementTab(AdvancementTabType type, int index, std::unique_ptr<AdvancementWidget> root,
                       Game::ItemStack icon, std::string title, std::string background);
        void AddWidget(std::unique_ptr<AdvancementWidget> widget);
        bool CanScrollHorizontally() const { return m_maxX - m_minX > 234; }
        bool CanScrollVertically() const { return m_maxY - m_minY > 113; }

        AdvancementTabType m_type;
        int m_index;
        std::string m_rootId;
        std::string m_background;
        Game::ItemStack m_icon;
        std::string m_title;
        AdvancementWidget* m_root = nullptr;
        std::vector<std::unique_ptr<AdvancementWidget>> m_widgetOrder;
        std::map<std::string, AdvancementWidget*> m_widgets;
        double m_scrollX = 0.0;
        double m_scrollY = 0.0;
        int m_minX = 2147483647;
        int m_minY = 2147483647;
        int m_maxX = -2147483647 - 1;
        int m_maxY = -2147483647 - 1;
        float m_fade = 0.0f;
        bool m_centered = false;
        AdvancementWidget* m_hovered = nullptr;
    };

    class AdvancementsScreen : public Screen, public Client::ClientAdvancements::Listener {
    public:
        static constexpr int kWindowWidth = 252;
        static constexpr int kWindowHeight = 140;
        static constexpr int kInsideX = 9;
        static constexpr int kInsideY = 18;
        static constexpr int kInsideWidth = 234;
        static constexpr int kInsideHeight = 113;

        AdvancementsScreen();
        ~AdvancementsScreen() override;

        void Init() override;
        void Tick() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        bool MouseClicked(double mx, double my, int button) override;
        bool MouseReleased(double mx, double my, int button) override;
        void MouseDragged(double mx, double my) override;
        bool MouseScrolled(double mx, double my, double deltaY) override;
        bool KeyPressed(int glfwKey, int glfwMods) override;

        // ClientAdvancements.Listener.
        void OnAdvancementsUpdated() override;
        void OnAdvancementsCleared() override;
        void OnSelectedTabChanged(const Game::Advancements::Definition* tab) override;

    private:
        void Rebuild(GuiGraphics& g);
        void RenderInside(GuiGraphics& g);
        void RenderWindow(GuiGraphics& g, int mouseX, int mouseY);
        void RenderTooltips(GuiGraphics& g, int mouseX, int mouseY);
        AdvancementTab* TabFor(const std::string& rootId);
        AdvancementTab* TabOf(const Game::Advancements::Node& node);

        int m_leftPos = 0;
        int m_topPos = 0;
        // Tabs in creation order, keyed by their root's id.
        std::vector<std::unique_ptr<AdvancementTab>> m_tabs;
        // The tabs of the tree before its last change. Their widgets point at
        // nodes that may be gone, so they are never drawn or ticked — kept
        // only so the rebuilt tabs inherit their scroll (copyPosition).
        std::vector<std::unique_ptr<AdvancementTab>> m_retiredTabs;
        AdvancementTab* m_selectedTab = nullptr;
        std::string m_selectedRootId;
        bool m_isScrolling = false;
        bool m_leftDown = false;
        double m_lastDragX = 0.0;
        double m_lastDragY = 0.0;
        int m_lastMouseX = 0;
        int m_lastMouseY = 0;
        // The tree changed while no GuiGraphics was at hand (the widgets
        // measure text): rebuilt at the next frame.
        bool m_dirty = true;
        bool m_listening = false;
    };

} // namespace Render
