// File: src/client/renderer/gui/screens/AdvancementsScreen.cpp
#include "AdvancementsScreen.hpp"

#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include "../toasts/ToastManager.hpp"
#include "client/input/KeyMapping.hpp"

#include "common/text/Language.hpp"
#include "common/text/TextComponent.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace Render {

    namespace {

        using Game::Advancements::FrameType;

        constexpr int kLineHeight = FontRenderer::LINE_HEIGHT;   // Font.lineHeight (9)

        // The window and the tab backgrounds: loaded once, kept.
        TextureHandle CachedTexture(const std::string& relPath) {
            static std::unordered_map<std::string, TextureHandle> s_cache;
            auto it = s_cache.find(relPath);
            if (it != s_cache.end()) return it->second;
            int w = 0, h = 0;
            const TextureHandle t = LoadStandaloneGuiTexture(relPath.c_str(), w, h);
            s_cache.emplace(relPath, t);
            return t;
        }

        // GuiGraphicsExtractor.horizontalLine / verticalLine — note the
        // vertical one leaves out both end pixels.
        void HorizontalLine(GuiGraphics& g, int x0, int x1, int y, uint32_t color) {
            if (x1 < x0) std::swap(x0, x1);
            g.Fill(x0, y, x1 + 1, y + 1, color);
        }

        void VerticalLine(GuiGraphics& g, int x, int y0, int y1, uint32_t color) {
            if (y1 < y0) std::swap(y0, y1);
            if (y1 > y0 + 1) g.Fill(x, y0 + 1, x + 1, y1, color);
        }

        // AdvancementWidgetType: the box and the frame sprites.
        const char* BoxSprite(bool obtained) {
            return obtained ? "advancements/box_obtained" : "advancements/box_unobtained";
        }

        const char* FrameSprite(bool obtained, FrameType type) {
            switch (type) {
                case FrameType::Challenge:
                    return obtained ? "advancements/challenge_frame_obtained" : "advancements/challenge_frame_unobtained";
                case FrameType::Goal:
                    return obtained ? "advancements/goal_frame_obtained" : "advancements/goal_frame_unobtained";
                case FrameType::Task:
                default:
                    return obtained ? "advancements/task_frame_obtained" : "advancements/task_frame_unobtained";
            }
        }

        // MC blitSprite(sprite, textureWidth, textureHeight, u, v, x, y, w, h)
        // for a nine-slice sprite: the whole sprite laid out at the texture
        // size, clipped to the requested piece.
        void BlitSpritePiece(GuiGraphics& g, const char* sprite, int textureWidth, int textureHeight,
                             int u, int v, int x, int y, int width, int height) {
            if (width <= 0 || height <= 0) return;
            g.EnableScissor(x, y, x + width, y + height);
            g.BlitSprite(sprite, x - u, y - v, textureWidth, textureHeight);
            g.DisableScissor();
        }

        int MaxLineWidth(const GuiGraphics& g, const std::vector<std::string>& lines) {
            int w = 0;
            for (const std::string& line : lines) w = std::max(w, g.GetStringWidth(line));
            return w;
        }

        // AdvancementWidget.findOptimalLines: the split whose widest line
        // lands closest to `preferredWidth`, trying a few margins.
        std::vector<std::string> FindOptimalLines(const GuiGraphics& g, const std::string& text, int preferredWidth) {
            static constexpr int kTestSplitOffsets[] = {0, 10, -10, 25, -25};
            std::vector<std::string> best;
            float bestDistance = 3.4e38f;
            for (int margin : kTestSplitOffsets) {
                std::vector<std::string> split = WrapText(g, text, preferredWidth - margin);
                const float distance = std::abs(static_cast<float>(MaxLineWidth(g, split)) - static_cast<float>(preferredWidth));
                if (distance <= 10.0f) return split;
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = std::move(split);
                }
            }
            return best;
        }

        std::string ProgressText(int done, int total) {
            return Game::Text::GetString(Game::Text::Component::Translatable(
                "advancements.progress",
                {Game::Text::Component::Literal(std::to_string(done)), Game::Text::Component::Literal(std::to_string(total))}));
        }

        // ── AdvancementTabType ───────────────────────────────────────────

        struct TabTypeInfo {
            const char* selected[3];     // first, middle, last
            const char* unselected[3];
            int width, height, max;
        };

        const TabTypeInfo& Info(AdvancementTabType type) {
            static const TabTypeInfo kInfo[] = {
                {{"advancements/tab_above_left_selected", "advancements/tab_above_middle_selected", "advancements/tab_above_right_selected"},
                 {"advancements/tab_above_left", "advancements/tab_above_middle", "advancements/tab_above_right"}, 28, 32, 8},
                {{"advancements/tab_below_left_selected", "advancements/tab_below_middle_selected", "advancements/tab_below_right_selected"},
                 {"advancements/tab_below_left", "advancements/tab_below_middle", "advancements/tab_below_right"}, 28, 32, 8},
                {{"advancements/tab_left_top_selected", "advancements/tab_left_middle_selected", "advancements/tab_left_bottom_selected"},
                 {"advancements/tab_left_top", "advancements/tab_left_middle", "advancements/tab_left_bottom"}, 32, 28, 5},
                {{"advancements/tab_right_top_selected", "advancements/tab_right_middle_selected", "advancements/tab_right_bottom_selected"},
                 {"advancements/tab_right_top", "advancements/tab_right_middle", "advancements/tab_right_bottom"}, 32, 28, 5},
            };
            return kInfo[static_cast<int>(type)];
        }

        int TabX(AdvancementTabType type, int index) {
            const TabTypeInfo& info = Info(type);
            switch (type) {
                case AdvancementTabType::Above:
                case AdvancementTabType::Below: return (info.width + 4) * index;
                case AdvancementTabType::Left:  return -info.width + 4;
                case AdvancementTabType::Right: return 248;
            }
            return 0;
        }

        int TabY(AdvancementTabType type, int index) {
            const TabTypeInfo& info = Info(type);
            switch (type) {
                case AdvancementTabType::Above: return -info.height + 4;
                case AdvancementTabType::Below: return 136;
                case AdvancementTabType::Left:
                case AdvancementTabType::Right: return info.height * index;
            }
            return 0;
        }

        // The vanilla tooltip box (as MerchantScreen draws one), one line.
        void DrawTextTooltip(GuiGraphics& g, const std::string& text, int mx, int my) {
            const int textW = g.GetStringWidth(text);
            const int totalH = 8;
            int x = mx + 12;
            if (x + textW + 4 > g.GuiWidth()) x = std::max(mx - 16 - textW, 4);
            int y = my - 12;
            if (y + totalH + 3 > g.GuiHeight()) y = g.GuiHeight() - totalH - 3;
            y = std::max(y, 4);
            const uint32_t bg = 0xF0100010;
            const uint32_t border = 0x505000FF;
            g.Fill(x - 3, y - 4, x + textW + 3, y - 3, bg);
            g.Fill(x - 3, y + totalH + 3, x + textW + 3, y + totalH + 4, bg);
            g.Fill(x - 3, y - 3, x + textW + 3, y + totalH + 3, bg);
            g.Fill(x - 4, y - 3, x - 3, y + totalH + 3, bg);
            g.Fill(x + textW + 3, y - 3, x + textW + 4, y + totalH + 3, bg);
            g.Fill(x - 3, y - 2, x - 2, y + totalH + 2, border);
            g.Fill(x + textW + 2, y - 2, x + textW + 3, y + totalH + 2, border);
            g.DrawString(text, x, y, 0xFFFFFFFF, true);
        }

    } // namespace

    // ── AdvancementWidget ────────────────────────────────────────────────

    AdvancementWidget::AdvancementWidget(GuiGraphics& g, const Game::Advancements::Node& node) : m_node(node) {
        const Game::Advancements::DisplayInfo& display = *node.def->display;
        m_titleLines = WrapText(g, Game::Text::GetString(display.title), 163);
        m_x = static_cast<int>(std::floor(display.x * 28.0f));
        m_y = static_cast<int>(std::floor(display.y * 27.0f));
        const int titleWidth = std::max(MaxLineWidth(g, m_titleLines), 80);
        int longestDescLine = 29 + titleWidth + MaxProgressWidth(g);
        m_description = FindOptimalLines(g, Game::Text::GetString(display.description), longestDescLine);
        for (const std::string& line : m_description) longestDescLine = std::max(longestDescLine, g.GetStringWidth(line));
        m_width = longestDescLine + 3 + 5;
        m_icon = display.icon;
    }

    int AdvancementWidget::MaxProgressWidth(GuiGraphics& g) const {
        const int maxCriteriaRequired = static_cast<int>(m_node.def->requirements.Size());
        if (maxCriteriaRequired <= 1) return 0;
        return g.GetStringWidth(ProgressText(maxCriteriaRequired, maxCriteriaRequired)) + 8;
    }

    bool AdvancementWidget::IsVisible() const {
        return !GetDisplay().hidden || (m_progress && m_progress->IsDone());
    }

    void AdvancementWidget::AttachToParent(AdvancementTab& tab) {
        if (m_parent) return;
        // findFirstVisibleParent: the nearest ancestor with a display.
        const Game::Advancements::Node* node = m_node.parent;
        while (node && !node->def->display) node = node->parent;
        if (!node) return;
        m_parent = tab.GetWidget(node->def->id);
        if (m_parent) m_parent->AddChild(this);
    }

    void AdvancementWidget::RenderConnectivity(GuiGraphics& g, int xo, int yo, bool background) const {
        if (m_parent) {
            const int depX = xo + m_parent->m_x + 13;
            const int splitX = xo + m_parent->m_x + 26 + 4;
            const int depY = yo + m_parent->m_y + 13;
            const int myX = xo + m_x + 13;
            const int myY = yo + m_y + 13;
            const uint32_t col = background ? 0xFF000000u : 0xFFFFFFFFu;
            if (background) {
                HorizontalLine(g, splitX, depX, depY - 1, col);
                HorizontalLine(g, splitX + 1, depX, depY, col);
                HorizontalLine(g, splitX, depX, depY + 1, col);
                HorizontalLine(g, myX, splitX - 1, myY - 1, col);
                HorizontalLine(g, myX, splitX - 1, myY, col);
                HorizontalLine(g, myX, splitX - 1, myY + 1, col);
                VerticalLine(g, splitX - 1, myY, depY, col);
                VerticalLine(g, splitX + 1, myY, depY, col);
            } else {
                HorizontalLine(g, splitX, depX, depY, col);
                HorizontalLine(g, myX, splitX, myY, col);
                VerticalLine(g, splitX, myY, depY, col);
            }
        }
        for (const AdvancementWidget* child : m_children) child->RenderConnectivity(g, xo, yo, background);
    }

    void AdvancementWidget::Render(GuiGraphics& g, int xo, int yo) const {
        if (IsVisible()) {
            const float amount = m_progress ? m_progress->GetPercent() : 0.0f;
            g.BlitSprite(FrameSprite(amount >= 1.0f, GetDisplay().type), xo + m_x + 3, yo + m_y, 26, 26);
            g.RenderItem(m_icon, xo + m_x + 8, yo + m_y + 5);
        }
        for (const AdvancementWidget* child : m_children) child->Render(g, xo, yo);
    }

    void AdvancementWidget::RenderHover(GuiGraphics& g, int ox, int oy, int scrollX, int scrollY, float fade,
                                        int screenXo, int screenWidth) const {
        (void)fade;
        // MC extractHover runs translated to the inside's origin with
        // xo/yo = the scroll; every coordinate below adds (ox, oy) back.
        const int xo = scrollX, yo = scrollY;
        const int titleBarHeight = kLineHeight * static_cast<int>(m_titleLines.size()) + 9 + 8;
        const int titleTop = yo + m_y + (26 - titleBarHeight) / 2;
        const int titleBarBottom = titleTop + titleBarHeight;
        const int descriptionTextHeight = static_cast<int>(m_description.size()) * kLineHeight;
        const int descriptionHeight = 6 + descriptionTextHeight;
        const bool leftSide = screenXo + xo + m_x + m_width + 26 >= screenWidth;
        std::string progressText;
        if (m_progress) {
            if (auto counts = m_progress->GetProgressCounts()) progressText = ProgressText(counts->first, counts->second);
        }
        const int progressWidth = progressText.empty() ? 0 : g.GetStringWidth(progressText);
        const bool topSide = titleBarBottom + descriptionHeight >= 113;
        const float amount = m_progress ? m_progress->GetPercent() : 0.0f;
        int firstHalfWidth = static_cast<int>(std::floor(amount * static_cast<float>(m_width)));
        bool firstHalf, secondHalf, iconFrame;   // obtained?
        if (amount >= 1.0f) {
            firstHalfWidth = m_width / 2;
            firstHalf = secondHalf = iconFrame = true;
        } else if (firstHalfWidth < 2) {
            firstHalfWidth = m_width / 2;
            firstHalf = secondHalf = iconFrame = false;
        } else if (firstHalfWidth > m_width - 2) {
            firstHalfWidth = m_width / 2;
            firstHalf = secondHalf = true;
            iconFrame = false;
        } else {
            firstHalf = true;
            secondHalf = false;
            iconFrame = false;
        }
        const int secondBarWidth = m_width - firstHalfWidth;
        const int titleLeft = leftSide ? xo + m_x - m_width + 26 + 6 : xo + m_x;
        const int backgroundHeight = titleBarHeight + descriptionHeight;
        if (!m_description.empty()) {
            const int top = topSide ? titleBarBottom - backgroundHeight : titleTop;
            g.BlitSprite("advancements/title_box", ox + titleLeft, oy + top, m_width, backgroundHeight);
        }
        if (firstHalf != secondHalf) {
            BlitSpritePiece(g, BoxSprite(firstHalf), kBoxWidth, titleBarHeight, 0, 0,
                            ox + titleLeft, oy + titleTop, firstHalfWidth, titleBarHeight);
            BlitSpritePiece(g, BoxSprite(secondHalf), kBoxWidth, titleBarHeight, kBoxWidth - secondBarWidth, 0,
                            ox + titleLeft + firstHalfWidth, oy + titleTop, secondBarWidth, titleBarHeight);
        } else {
            g.BlitSprite(BoxSprite(firstHalf), ox + titleLeft, oy + titleTop, m_width, titleBarHeight);
        }
        g.BlitSprite(FrameSprite(iconFrame, GetDisplay().type), ox + xo + m_x + 3, oy + yo + m_y, 26, 26);

        const int descriptionLeft = titleLeft + 5;
        auto drawLines = [&g](const std::vector<std::string>& lines, int x, int y, uint32_t color) {
            for (size_t i = 0; i < lines.size(); ++i) g.DrawString(lines[i], x, y + static_cast<int>(i) * kLineHeight, color, true);
        };
        if (leftSide) {
            drawLines(m_titleLines, ox + descriptionLeft, oy + titleTop + 9, 0xFFFFFFFFu);
            if (!progressText.empty()) g.DrawString(progressText, ox + xo + m_x - progressWidth, oy + titleTop + 9, 0xFFFFFFFFu, true);
        } else {
            drawLines(m_titleLines, ox + xo + m_x + 32, oy + titleTop + 9, 0xFFFFFFFFu);
            if (!progressText.empty()) {
                g.DrawString(progressText, ox + xo + m_x + m_width - progressWidth - 5, oy + titleTop + 9, 0xFFFFFFFFu, true);
            }
        }
        // The description carries the type's chat colour (ComponentUtils.
        // mergeStyles), green for tasks and goals, purple for challenges.
        const uint32_t descriptionColor = 0xFF000000u | Game::Advancements::FrameChatColor(GetDisplay().type);
        if (topSide) {
            drawLines(m_description, ox + descriptionLeft, oy + titleTop - descriptionTextHeight + 1, descriptionColor);
        } else {
            drawLines(m_description, ox + descriptionLeft, oy + titleBarBottom, descriptionColor);
        }
        g.RenderItem(m_icon, ox + xo + m_x + 8, oy + yo + m_y + 5);
    }

    bool AdvancementWidget::IsMouseOver(int scrollX, int scrollY, int mouseX, int mouseY) const {
        if (!IsVisible()) return false;
        const int x0 = scrollX + m_x;
        const int x1 = x0 + 26;
        const int y0 = scrollY + m_y;
        const int y1 = y0 + 26;
        return mouseX >= x0 && mouseX <= x1 && mouseY >= y0 && mouseY <= y1;
    }

    // ── AdvancementTab ───────────────────────────────────────────────────

    AdvancementTab::AdvancementTab(AdvancementTabType type, int index, std::unique_ptr<AdvancementWidget> root,
                                   Game::ItemStack icon, std::string title, std::string background)
        : m_type(type), m_index(index), m_rootId(root->GetAdvancement().id), m_background(std::move(background)),
          m_icon(std::move(icon)), m_title(std::move(title)) {
        m_root = root.get();
        AddWidget(std::move(root));
    }

    std::unique_ptr<AdvancementTab> AdvancementTab::Create(GuiGraphics& g, int index, const Game::Advancements::Node& root) {
        if (!root.def->display) return nullptr;
        const Game::Advancements::DisplayInfo& display = *root.def->display;
        if (!display.background) return nullptr;
        for (AdvancementTabType type : {AdvancementTabType::Above, AdvancementTabType::Below,
                                        AdvancementTabType::Left, AdvancementTabType::Right}) {
            const int max = Info(type).max;
            if (index < max) {
                return std::unique_ptr<AdvancementTab>(new AdvancementTab(
                    type, index, std::make_unique<AdvancementWidget>(g, root), display.icon,
                    Game::Text::GetString(display.title), display.BackgroundTexturePath()));
            }
            index -= max;
        }
        return nullptr;
    }

    void AdvancementTab::CopyPosition(const AdvancementTab& source) {
        m_scrollX = source.m_scrollX;
        m_scrollY = source.m_scrollY;
        m_minX = source.m_minX;
        m_minY = source.m_minY;
        m_maxX = source.m_maxX;
        m_maxY = source.m_maxY;
        m_centered = source.m_centered;
    }

    void AdvancementTab::Tick(int relativeMouseX, int relativeMouseY) {
        bool hovering = false;
        if (relativeMouseX > 0 && relativeMouseX < 234 && relativeMouseY > 0 && relativeMouseY < 113) {
            const int sx = static_cast<int>(std::floor(m_scrollX));
            const int sy = static_cast<int>(std::floor(m_scrollY));
            for (const auto& widget : m_widgetOrder) {
                if (widget->IsMouseOver(sx, sy, relativeMouseX, relativeMouseY)) {
                    hovering = true;
                    m_hovered = widget.get();
                    break;
                }
            }
        }
        if (hovering) {
            m_fade = std::clamp(m_fade + 0.06f, 0.0f, 0.3f);
        } else {
            m_fade = std::clamp(m_fade - 0.12f, 0.0f, 1.0f);
            m_hovered = nullptr;
        }
    }

    void AdvancementTab::RenderTab(GuiGraphics& g, int xo, int yo, bool selected) const {
        const TabTypeInfo& info = Info(m_type);
        const char* const* sprites = selected ? info.selected : info.unselected;
        const char* sprite = m_index == 0 ? sprites[0] : (m_index == info.max - 1 ? sprites[2] : sprites[1]);
        g.BlitSprite(sprite, xo + TabX(m_type, m_index), yo + TabY(m_type, m_index), info.width, info.height);
    }

    void AdvancementTab::RenderIcon(GuiGraphics& g, int xo, int yo) const {
        int x = xo + TabX(m_type, m_index);
        int y = yo + TabY(m_type, m_index);
        switch (m_type) {
            case AdvancementTabType::Above: x += 6;  y += 9; break;
            case AdvancementTabType::Below: x += 6;  y += 6; break;
            case AdvancementTabType::Left:  x += 10; y += 5; break;
            case AdvancementTabType::Right: x += 6;  y += 5; break;
        }
        g.RenderItem(m_icon, x, y);   // fakeItem: no decorations
    }

    void AdvancementTab::RenderContents(GuiGraphics& g, int windowLeft, int windowTop) {
        if (!m_centered) {
            m_scrollX = 117 - (m_maxX + m_minX) / 2;
            m_scrollY = 56 - (m_maxY + m_minY) / 2;
            m_centered = true;
        }
        g.EnableScissor(windowLeft, windowTop, windowLeft + 234, windowTop + 113);
        const int sx = static_cast<int>(std::floor(m_scrollX));
        const int sy = static_cast<int>(std::floor(m_scrollY));
        const int left = sx % 16;
        const int top = sy % 16;
        const TextureHandle background = CachedTexture(m_background);
        if (background != INVALID_TEXTURE) {
            for (int x = -1; x <= 15; ++x) {
                for (int y = -1; y <= 8; ++y) {
                    const int px = windowLeft + left + 16 * x;
                    const int py = windowTop + top + 16 * y;
                    g.Blit(background, px, py, px + 16, py + 16, 0.0f, 0.0f, 1.0f, 1.0f);
                }
            }
        } else {
            g.Fill(windowLeft, windowTop, windowLeft + 234, windowTop + 113, 0xFF000000);
        }
        m_root->RenderConnectivity(g, windowLeft + sx, windowTop + sy, true);
        m_root->RenderConnectivity(g, windowLeft + sx, windowTop + sy, false);
        m_root->Render(g, windowLeft + sx, windowTop + sy);
        g.DisableScissor();
    }

    void AdvancementTab::RenderTooltips(GuiGraphics& g, int windowLeft, int windowTop, int screenWidth) const {
        const int ox = windowLeft + 9;
        const int oy = windowTop + 18;
        g.Fill(ox, oy, ox + 234, oy + 113, static_cast<uint32_t>(static_cast<int>(std::floor(m_fade * 255.0f))) << 24);
        if (m_hovered) {
            const int sx = static_cast<int>(std::floor(m_scrollX));
            const int sy = static_cast<int>(std::floor(m_scrollY));
            m_hovered->RenderHover(g, ox, oy, sx, sy, m_fade, windowLeft, screenWidth);
        }
    }

    bool AdvancementTab::IsMouseOver(int xo, int yo, double mx, double my) const {
        const TabTypeInfo& info = Info(m_type);
        const int x = xo + TabX(m_type, m_index);
        const int y = yo + TabY(m_type, m_index);
        return mx > x && mx < x + info.width && my > y && my < y + info.height;
    }

    void AdvancementTab::Scroll(double dx, double dy) {
        if (CanScrollHorizontally()) m_scrollX = std::clamp(m_scrollX + dx, static_cast<double>(-(m_maxX - 234)), 0.0);
        if (CanScrollVertically())   m_scrollY = std::clamp(m_scrollY + dy, static_cast<double>(-(m_maxY - 113)), 0.0);
    }

    void AdvancementTab::AddAdvancement(GuiGraphics& g, const Game::Advancements::Node& node) {
        if (!node.def->display) return;
        AddWidget(std::make_unique<AdvancementWidget>(g, node));
    }

    void AdvancementTab::AddWidget(std::unique_ptr<AdvancementWidget> widget) {
        AdvancementWidget* raw = widget.get();
        m_widgets[raw->GetAdvancement().id] = raw;
        m_widgetOrder.push_back(std::move(widget));
        const int x0 = raw->GetX();
        const int x1 = x0 + 28;
        const int y0 = raw->GetY();
        const int y1 = y0 + 27;
        m_minX = std::min(m_minX, x0);
        m_maxX = std::max(m_maxX, x1);
        m_minY = std::min(m_minY, y0);
        m_maxY = std::max(m_maxY, y1);
        for (const auto& other : m_widgetOrder) other->AttachToParent(*this);
    }

    AdvancementWidget* AdvancementTab::GetWidget(const std::string& id) {
        auto it = m_widgets.find(id);
        return it == m_widgets.end() ? nullptr : it->second;
    }

    // ── AdvancementsScreen ───────────────────────────────────────────────

    AdvancementsScreen::AdvancementsScreen()
        : Screen(Game::Language::GetOrDefault("gui.advancements", "Advancements")) {}

    AdvancementsScreen::~AdvancementsScreen() {
        // MC removed(): stop listening, tell the server the screen closed.
        if (m_listening) Client::ClientAdvancements::Get().SetListener(nullptr);
        Client::ClientAdvancements::Get().SendClosedScreen();
    }

    void AdvancementsScreen::Init() {
        m_leftPos = (m_width - kWindowWidth) / 2;
        m_topPos = (m_height - kWindowHeight) / 2;
        // MC init: forget the tabs, listen (which replays the tree and the
        // selected tab), then the tab is (re)announced to the server.
        m_tabs.clear();
        m_selectedTab = nullptr;
        m_dirty = true;
        m_listening = true;
        Client::ClientAdvancements::Get().SetListener(this);

        // HeaderAndFooterLayout's footer: Done, 200 wide.
        AddWidget(new Button((m_width - 200) / 2, m_height - 27, 200, WidgetDims::BUTTON_HEIGHT,
                             Game::Language::GetOrDefault("gui.done", "Done"), [this] { OnClose(); }));
    }

    void AdvancementsScreen::OnAdvancementsUpdated() {
        // The widgets measure text, so the rebuild waits for the next frame's
        // GuiGraphics; the current tabs may point at nodes the update just
        // removed, so they retire now (kept for their scroll only).
        if (!m_tabs.empty()) {
            m_retiredTabs = std::move(m_tabs);
            m_tabs.clear();
        }
        m_selectedTab = nullptr;
        m_dirty = true;
    }

    void AdvancementsScreen::OnAdvancementsCleared() {
        m_tabs.clear();
        m_retiredTabs.clear();
        m_selectedTab = nullptr;
        m_dirty = true;
    }

    void AdvancementsScreen::OnSelectedTabChanged(const Game::Advancements::Definition* tab) {
        m_selectedRootId = tab ? tab->id : std::string();
        m_selectedTab = tab ? TabFor(tab->id) : nullptr;
    }

    AdvancementTab* AdvancementsScreen::TabFor(const std::string& rootId) {
        for (const auto& tab : m_tabs) {
            if (tab->RootId() == rootId) return tab.get();
        }
        return nullptr;
    }

    AdvancementTab* AdvancementsScreen::TabOf(const Game::Advancements::Node& node) {
        return TabFor(node.Root()->def->id);
    }

    void AdvancementsScreen::Rebuild(GuiGraphics& g) {
        // MC onAdvancementsUpdated.
        const auto& advancements = Client::ClientAdvancements::Get();
        std::vector<std::unique_ptr<AdvancementTab>> oldTabs = std::move(m_retiredTabs);
        m_retiredTabs.clear();
        for (auto& tab : m_tabs) oldTabs.push_back(std::move(tab));
        m_tabs.clear();
        for (const Game::Advancements::Node* root : advancements.GetTree().Roots()) {
            if (TabFor(root->def->id)) continue;
            std::unique_ptr<AdvancementTab> tab = AdvancementTab::Create(g, static_cast<int>(m_tabs.size()), *root);
            if (!tab) continue;
            for (const auto& old : oldTabs) {
                if (old->RootId() == root->def->id) { tab->CopyPosition(*old); break; }
            }
            m_tabs.push_back(std::move(tab));
        }
        for (const Game::Advancements::Node* task : advancements.GetTree().Tasks()) {
            if (AdvancementTab* tab = TabOf(*task)) tab->AddAdvancement(g, *task);
        }
        for (const auto& [id, progress] : advancements.Progress()) {
            const Game::Advancements::Node* node = advancements.GetTree().Get(id);
            if (!node) continue;
            AdvancementTab* tab = TabOf(*node);
            if (AdvancementWidget* widget = tab ? tab->GetWidget(id) : nullptr) widget->SetProgress(progress);
        }
        m_selectedTab = m_selectedRootId.empty() ? nullptr : TabFor(m_selectedRootId);
        if (!m_selectedTab && !m_tabs.empty()) {
            m_selectedTab = m_tabs.front().get();
            m_selectedRootId = m_selectedTab->RootId();
            const Game::Advancements::Definition* def = advancements.Find(m_selectedRootId);
            Client::ClientAdvancements::Get().SetSelectedTab(def, true);
        }
        m_dirty = false;
    }

    void AdvancementsScreen::Tick() {
        Screen::Tick();
        if (m_selectedTab) {
            m_selectedTab->Tick(m_lastMouseX - m_leftPos - kInsideX, m_lastMouseY - m_topPos - kInsideY);
        }
    }

    bool AdvancementsScreen::MouseClicked(double mx, double my, int button) {
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            m_leftDown = true;
            m_lastDragX = mx;
            m_lastDragY = my;
            for (const auto& tab : m_tabs) {
                if (tab->IsMouseOver(m_leftPos, m_topPos, mx, my)) {
                    const Game::Advancements::Definition* def =
                        Client::ClientAdvancements::Get().Find(tab->RootId());
                    Client::ClientAdvancements::Get().SetSelectedTab(def, true);
                    break;
                }
            }
        }
        return Screen::MouseClicked(mx, my, button);
    }

    bool AdvancementsScreen::MouseReleased(double mx, double my, int button) {
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            m_leftDown = false;
            m_isScrolling = false;
        }
        return Screen::MouseReleased(mx, my, button);
    }

    void AdvancementsScreen::MouseDragged(double mx, double my) {
        // MC mouseDragged: the left button pans the selected tab; the first
        // drag event only arms it.
        if (!m_leftDown) {
            m_isScrolling = false;
            Screen::MouseDragged(mx, my);
            return;
        }
        if (!m_isScrolling) {
            m_isScrolling = true;
        } else if (m_selectedTab) {
            m_selectedTab->Scroll(mx - m_lastDragX, my - m_lastDragY);
        }
        m_lastDragX = mx;
        m_lastDragY = my;
    }

    bool AdvancementsScreen::MouseScrolled(double mx, double my, double deltaY) {
        (void)mx; (void)my;
        if (!m_selectedTab) return false;
        m_selectedTab->Scroll(0.0, deltaY * 16.0);   // SCROLL_SPEED
        return true;
    }

    bool AdvancementsScreen::KeyPressed(int glfwKey, int glfwMods) {
        // MC keyPressed: the advancements key closes the screen again.
        if (Input::Binds::Advancements && Input::Binds::Advancements->key == Input::BoundKey::Keyboard(glfwKey)) {
            OnClose();
            return true;
        }
        return Screen::KeyPressed(glfwKey, glfwMods);
    }

    void AdvancementsScreen::RenderInside(GuiGraphics& g) {
        const int insideLeft = m_leftPos + kInsideX;
        const int insideTop = m_topPos + kInsideY;
        if (!m_selectedTab) {
            g.Fill(insideLeft, insideTop, insideLeft + kInsideWidth, insideTop + kInsideHeight, 0xFF000000);
            const int midX = insideLeft + kInsideWidth / 2;
            g.DrawCenteredString(Game::Language::GetOrDefault("advancements.empty", "There doesn't seem to be anything here..."),
                                 midX, insideTop + 56 - kLineHeight / 2, 0xFFFFFFFF);
            g.DrawCenteredString(Game::Language::GetOrDefault("advancements.sad_label", ":("),
                                 midX, insideTop + kInsideHeight - kLineHeight, 0xFFFFFFFF);
            return;
        }
        m_selectedTab->RenderContents(g, insideLeft, insideTop);
    }

    void AdvancementsScreen::RenderWindow(GuiGraphics& g, int mouseX, int mouseY) {
        (void)mouseX; (void)mouseY;
        const TextureHandle window = CachedTexture("assets/textures/gui/advancements/window.png");
        if (window != INVALID_TEXTURE) {
            g.Blit(window, m_leftPos, m_topPos, m_leftPos + kWindowWidth, m_topPos + kWindowHeight,
                   0.0f, 0.0f, kWindowWidth / 256.0f, kWindowHeight / 256.0f);
        }
        if (m_tabs.size() > 1) {
            for (const auto& tab : m_tabs) tab->RenderTab(g, m_leftPos, m_topPos, tab.get() == m_selectedTab);
            for (const auto& tab : m_tabs) tab->RenderIcon(g, m_leftPos, m_topPos);
        }
        // -12566464: 0xFF404040, no shadow.
        g.DrawString(m_selectedTab ? m_selectedTab->GetTitle() : m_title, m_leftPos + 8, m_topPos + 6, 0xFF404040u, false);
    }

    void AdvancementsScreen::RenderTooltips(GuiGraphics& g, int mouseX, int mouseY) {
        if (m_selectedTab) {
            g.NextStratum();
            m_selectedTab->RenderTooltips(g, m_leftPos, m_topPos, m_width);
        }
        if (m_tabs.size() > 1) {
            for (const auto& tab : m_tabs) {
                if (tab->IsMouseOver(m_leftPos, m_topPos, mouseX, mouseY)) {
                    g.NextStratum();
                    DrawTextTooltip(g, tab->GetTitle(), mouseX, mouseY);
                }
            }
        }
    }

    void AdvancementsScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        if (m_dirty) Rebuild(g);
        m_lastMouseX = mouseX;
        m_lastMouseY = mouseY;
        // MC: the background and the footer's Done, the header's title, then
        // the inside, the window over it, and the hover on top.
        Screen::Render(g, mouseX, mouseY, partialTick);
        g.DrawCenteredString(m_title, m_width / 2, (33 - kLineHeight) / 2, 0xFFFFFFFF);
        g.NextStratum();
        RenderInside(g);
        g.NextStratum();
        RenderWindow(g, mouseX, mouseY);
        RenderTooltips(g, mouseX, mouseY);
    }

} // namespace Render
