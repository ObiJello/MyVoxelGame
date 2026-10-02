// File: src/client/renderer/gui/LoomScreen.cpp
#include "LoomScreen.hpp"
#include "GuiGraphics.hpp"
#include "screens/Screen.hpp"          // LoadStandaloneGuiTexture
#include "screens/BookScreens.hpp"     // QueueContainerButtonClick
#include "client/sound/ClientSounds.hpp"
#include "common/inventory/UtilityMenus.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/DyeColorUtil.hpp"
#include "common/world/banner/BannerPatterns.hpp"
#include "common/world/block/entity/BannerBlockEntity.hpp"
#include "common/world/block/entity/SignBlockEntity.hpp"   // DyeColor, DyeColorName
#include "common/text/Language.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace Render {

    namespace {
        constexpr const char* kBannerSlotSprite       = "container/slot/banner";
        constexpr const char* kDyeSlotSprite          = "container/slot/dye";
        constexpr const char* kPatternSlotSprite      = "container/slot/banner_pattern";
        constexpr const char* kScrollerSprite         = "container/loom/scroller";
        constexpr const char* kScrollerDisabledSprite = "container/loom/scroller_disabled";
        constexpr const char* kPatternSelectedSprite  = "container/loom/pattern_selected";
        constexpr const char* kPatternHighlightSprite = "container/loom/pattern_highlighted";
        constexpr const char* kPatternSprite          = "container/loom/pattern";
        constexpr const char* kErrorSprite            = "container/loom/error";

        // BANNER_PATTERN_TEXTURE_SIZE / WIDTH / HEIGHT: the flag's face on
        // its 64 x 64 sheet.
        constexpr float kSheet = 64.0f;
        constexpr float kFlagW = 21.0f;
        constexpr float kFlagH = 40.0f;

        uint32_t Opaque(uint32_t rgb) { return 0xFF000000u | (rgb & 0x00FFFFFFu); }

        int PositiveCeilDiv(int a, int b) { return (a + b - 1) / b; }

        bool HasMaxPatterns(const Game::ItemStack& banner) {
            // BannerPatternLayers.layers().size() >= 6.
            if (banner.IsEmpty()) return false;
            const auto layers = banner.get(Game::DataComponents::BANNER_PATTERNS);
            return layers && static_cast<int>(layers->layers.size()) >= Game::LoomMenu::MAX_PATTERNS;
        }
    }

    LoomScreen& GetLoomScreen() {
        static LoomScreen s;
        return s;
    }

    void LoomScreen::Configure(const std::string& /*title*/) {
        // LoomBlock's menu title is "container.loom" — the block's registry
        // name (what the server sends for every block menu) is not it.
        m_title = Game::Language::GetOrDefault("container.loom", "Loom");
    }

    Game::LoomMenu* LoomScreen::Loom() const {
        return dynamic_cast<Game::LoomMenu*>(Menu());
    }

    TextureHandle LoomScreen::EnsureBackground() {
        if (m_backgroundTried) return m_background;
        m_backgroundTried = true;
        int w = 0, h = 0;
        m_background = LoadStandaloneGuiTexture("assets/textures/gui/container/loom.png", w, h);
        return m_background;
    }

    TextureHandle LoomScreen::PatternTexture(const std::string& asset) {
        auto it = m_patternTextures.find(asset);
        if (it != m_patternTextures.end()) return it->second;
        int w = 0, h = 0;
        const std::string path = "assets/textures/entity/banner/" + asset + ".png";
        const TextureHandle tex = LoadStandaloneGuiTexture(path.c_str(), w, h);
        m_patternTextures.emplace(asset, tex);
        return tex;
    }

    void LoomScreen::OnOpen() {
        m_patterns.clear();
        m_displayPatterns = false;
        m_hasMaxPatterns = false;
        m_scrollOffs = 0.0f;
        m_scrolling = false;
        m_startRow = 0;
    }

    void LoomScreen::ContainerTick() {
        Refresh();
    }

    void LoomScreen::Refresh() {
        // MC containerChanged, run every frame: the flags only ever depend on
        // the slots as they are now.
        Game::LoomMenu* menu = Loom();
        if (!menu) {
            m_patterns.clear();
            m_displayPatterns = false;
            m_hasMaxPatterns = false;
            return;
        }
        m_patterns = menu->PatternsOnOffer();
        m_hasMaxPatterns = HasMaxPatterns(menu->BannerInput());
        m_displayPatterns = !menu->BannerInput().IsEmpty() && !menu->DyeInput().IsEmpty() &&
                            !m_hasMaxPatterns && !m_patterns.empty();
        if (m_startRow >= TotalRowCount()) {
            m_startRow = 0;
            m_scrollOffs = 0.0f;
        }
    }

    int LoomScreen::TotalRowCount() const {
        return PositiveCeilDiv(static_cast<int>(m_patterns.size()), PATTERN_COLUMNS);
    }

    bool LoomScreen::IsScrollBarActive() const {
        return m_displayPatterns && static_cast<int>(m_patterns.size()) > PATTERN_COLUMNS * PATTERN_ROWS;
    }

    const char* LoomScreen::GetNoItemIcon(int menuIndex) const {
        // extractBackground's placeholders for the three empty inputs.
        switch (menuIndex) {
            case Game::LoomMenu::BANNER_SLOT:  return kBannerSlotSprite;
            case Game::LoomMenu::DYE_SLOT:     return kDyeSlotSprite;
            case Game::LoomMenu::PATTERN_SLOT: return kPatternSlotSprite;
            default: return AbstractContainerScreen::GetNoItemIcon(menuIndex);
        }
    }

    void LoomScreen::DrawBannerOnButton(GuiGraphics& g, int posX, int posY, const std::string& pattern) {
        // extractBannerOnButton: at (4, 2) inside the button, a 5 x 10 grey
        // flag with the pattern's sheet on it — u over the first 21 of 64
        // texels, v from 1 over the next 40, untinted.
        const int x = posX + 4;
        const int y = posY + 2;
        g.Fill(x, y, x + 5, y + 10, Opaque(Game::DyeTextureDiffuseColor(static_cast<uint8_t>(Game::DyeColor::Gray))));
        const TextureHandle tex = PatternTexture(Game::BannerPatterns::AssetOf(pattern));
        if (tex == INVALID_TEXTURE) return;
        const float u0 = 0.0f;
        const float u1 = kFlagW / kSheet;
        const float v0 = 1.0f / kSheet;
        const float v1 = v0 + kFlagH / kSheet;
        g.Blit(tex, x, y, x + 5, y + 10, u0, v0, u1, v1);
    }

    void LoomScreen::DrawResultBanner(GuiGraphics& g, int x0, int y0, int x1, int y1) {
        // GuiBannerResultRenderer: the standing flag at 16 px a block, moved
        // 0.25 down, fills the 20 x 40 box face-on — the flag box's front
        // face, texels (1, 1)-(21, 41) of each 64 x 64 sheet. Sheets.
        // BANNER_BASE tinted with the banner's colour, then every layer.
        Game::LoomMenu* menu = Loom();
        if (!menu) return;
        const Game::ItemStack& result = menu->ResultItem();
        if (result.IsEmpty()) return;
        const float u0 = 1.0f / kSheet, v0 = 1.0f / kSheet;
        const float u1 = 21.0f / kSheet, v1 = 41.0f / kSheet;
        auto layer = [&](const std::string& asset, uint8_t color) {
            const TextureHandle tex = PatternTexture(asset);
            if (tex == INVALID_TEXTURE) return;
            g.Blit(tex, x0, y0, x1, y1, u0, v0, u1, v1, Opaque(Game::DyeTextureDiffuseColor(color)));
        };
        // BannerItem.getColor: the block the item places.
        const Game::DyeColor base = Game::BannerBlockEntity::BaseColorOf(static_cast<Game::BlockID>(result.itemId));
        layer("base", static_cast<uint8_t>(base));
        if (const auto layers = result.get(Game::DataComponents::BANNER_PATTERNS)) {
            for (const Game::BannerPatternLayer& l : layers->layers) {
                layer(Game::BannerPatterns::AssetOf(l.pattern), l.color);
            }
        }
    }

    void LoomScreen::RenderBg(GuiGraphics& g, int leftPos, int topPos) {
        // MC LoomScreen.extractBackground.
        Refresh();
        const TextureHandle bg = EnsureBackground();
        if (bg == INVALID_TEXTURE) {
            g.Fill(leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H, 0xC0202020);
        } else {
            g.Blit(bg, leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H,
                   0.0f, 0.0f, static_cast<float>(IMAGE_W) / 256.0f, static_cast<float>(IMAGE_H) / 256.0f);
        }

        Game::LoomMenu* menu = Loom();
        if (!menu) return;

        // The slot placeholders are GetNoItemIcon's (drawn with the slots).

        const int sy = static_cast<int>(41.0f * m_scrollOffs);
        g.BlitSprite(IsScrollBarActive() ? kScrollerSprite : kScrollerDisabledSprite,
                     leftPos + SCROLLER_X, topPos + SCROLLER_Y + sy, SCROLLER_W, SCROLLER_H);

        const Game::Slot& resultSlot = menu->GetSlot(Game::LoomMenu::RESULT_SLOT);
        if (!menu->ResultItem().IsEmpty() && !m_hasMaxPatterns) {
            const int x = leftPos + 141;
            const int y = topPos + 8;
            DrawResultBanner(g, x, y, x + 20, y + 40);
        } else if (m_hasMaxPatterns) {
            g.BlitSprite(kErrorSprite, leftPos + resultSlot.x - 5, topPos + resultSlot.y - 5, 26, 26);
        }

        if (!m_displayPatterns) return;
        const glm::vec2 mouse = MouseGui();
        const int mx = static_cast<int>(std::floor(mouse.x));
        const int my = static_cast<int>(std::floor(mouse.y));
        const int x = leftPos + PATTERNS_X;
        const int y = topPos + PATTERNS_Y;
        const int selected = menu->SelectedPatternIndex();
        for (int row = 0; row < PATTERN_ROWS; ++row) {
            for (int column = 0; column < PATTERN_COLUMNS; ++column) {
                const int index = (row + m_startRow) * PATTERN_COLUMNS + column;
                if (index >= static_cast<int>(m_patterns.size())) return;
                const int posX = x + column * PATTERN_IMAGE_SIZE;
                const int posY = y + row * PATTERN_IMAGE_SIZE;
                const bool highlighted = mx >= posX && my >= posY &&
                                         mx < posX + PATTERN_IMAGE_SIZE && my < posY + PATTERN_IMAGE_SIZE;
                const char* sprite = index == selected ? kPatternSelectedSprite
                                   : highlighted       ? kPatternHighlightSprite
                                                       : kPatternSprite;
                g.BlitSprite(sprite, posX, posY, PATTERN_IMAGE_SIZE, PATTERN_IMAGE_SIZE);
                DrawBannerOnButton(g, posX, posY, m_patterns[static_cast<size_t>(index)]);
            }
        }
    }

    void LoomScreen::RenderLabels(GuiGraphics& g, int leftPos, int topPos) {
        g.DrawString(m_title, leftPos + TITLE_X, topPos + TITLE_Y, LABEL_COLOR, false);
        g.DrawString(Game::Language::GetOrDefault("container.inventory", "Inventory"),
                     leftPos + INV_LABEL_X, topPos + INV_LABEL_Y, LABEL_COLOR, false);
    }

    void LoomScreen::RenderExtras(GuiGraphics& g, int leftPos, int topPos) {
        // The highlighted cell's tooltip (setTooltipForNextFrame): the
        // pattern's name in the dye's colour, "<translationKey>.<dye>". Not
        // on the selected cell, which MC draws without it.
        Game::LoomMenu* menu = Loom();
        if (!menu || !m_displayPatterns) return;
        const glm::vec2 mouse = MouseGui();
        const int mx = static_cast<int>(std::floor(mouse.x));
        const int my = static_cast<int>(std::floor(mouse.y));
        const int column = (mx - (leftPos + PATTERNS_X));
        const int row = (my - (topPos + PATTERNS_Y));
        if (column < 0 || row < 0) return;
        const int c = column / PATTERN_IMAGE_SIZE;
        const int r = row / PATTERN_IMAGE_SIZE;
        if (c >= PATTERN_COLUMNS || r >= PATTERN_ROWS) return;
        const int index = (r + m_startRow) * PATTERN_COLUMNS + c;
        if (index >= static_cast<int>(m_patterns.size()) || index == menu->SelectedPatternIndex()) return;
        // DataComponents.DYE of the dye stack, WHITE without one.
        const int dye = Game::DyeColorOf(menu->DyeInput());
        const Game::DyeColor color = dye >= 0 ? static_cast<Game::DyeColor>(dye) : Game::DyeColor::White;
        const std::string key = Game::BannerPatterns::TranslationKeyOf(m_patterns[static_cast<size_t>(index)]) +
                                "." + Game::DyeColorName(color);
        DrawTextTooltip(g, Game::Language::Get(key), mx, my);
    }

    void LoomScreen::DrawTextTooltip(GuiGraphics& g, const std::string& text, int mx, int my) {
        // The one-line tooltip box every container screen draws (see
        // EnchantmentScreen::RenderSegmentTooltip).
        const int textW = g.GetStringWidth(text);
        const int totalH = 8;
        int x = mx + 12;
        if (x + textW + 4 > g.GuiWidth()) x = std::max(mx - 16 - textW, 4);
        int y = my - 12;
        if (y + totalH + 3 > g.GuiHeight()) y = g.GuiHeight() - totalH - 3;
        y = std::max(y, 4);
        const uint32_t bg     = 0xF0100010;
        const uint32_t border = 0x505000FF;
        g.Fill(x - 3, y - 4,          x + textW + 3, y - 3,          bg);
        g.Fill(x - 3, y + totalH + 3, x + textW + 3, y + totalH + 4, bg);
        g.Fill(x - 3, y - 3,          x + textW + 3, y + totalH + 3, bg);
        g.Fill(x - 4, y - 3,          x - 3,         y + totalH + 3, bg);
        g.Fill(x + textW + 3, y - 3,  x + textW + 4, y + totalH + 3, bg);
        g.Fill(x - 3,         y - 3 + 1, x - 3 + 1,     y + totalH + 3 - 1, border);
        g.Fill(x + textW + 2, y - 3 + 1, x + textW + 3, y + totalH + 3 - 1, border);
        g.DrawString(text, x, y, 0xFFFFFFFF, true);
    }

    int LoomScreen::HitTestExtras(int lx, int ly) {
        if (!m_displayPatterns) return HIT_NONE;
        const int xx = lx - PATTERNS_X;
        const int yy = ly - PATTERNS_Y;
        if (xx >= 0 && yy >= 0 && xx < PATTERN_COLUMNS * PATTERN_IMAGE_SIZE &&
            yy < PATTERN_ROWS * PATTERN_IMAGE_SIZE) {
            return HIT_PATTERN_0 - ((yy / PATTERN_IMAGE_SIZE) * PATTERN_COLUMNS + xx / PATTERN_IMAGE_SIZE);
        }
        if (lx >= SCROLLER_X && lx < SCROLLER_X + SCROLLER_W &&
            ly >= SCROLLER_CLICK_Y && ly < SCROLLER_CLICK_Y + SCROLLER_FULL_HEIGHT) {
            return HIT_SCROLLER;
        }
        return HIT_NONE;
    }

    bool LoomScreen::HandleExtraClick(int hit, int /*glfwButton*/, bool /*shift*/) {
        // MC LoomScreen.mouseClicked: any button.
        if (hit == HIT_SCROLLER) {
            m_scrolling = true;
            return true;
        }
        const int cell = HIT_PATTERN_0 - hit;
        if (cell < 0 || cell >= PATTERN_COLUMNS * PATTERN_ROWS) return false;
        Game::LoomMenu* menu = Loom();
        if (!menu) return true;
        const int index = (cell / PATTERN_COLUMNS + m_startRow) * PATTERN_COLUMNS + cell % PATTERN_COLUMNS;
        // clickMenuButton's check (0 <= id < selectablePatterns.size()),
        // against the list as the slots stand now. The client copy predicts
        // the selection and result when its own list agrees; the server's
        // data slot and result slot settle it either way.
        if (index < 0 || index >= static_cast<int>(m_patterns.size())) return true;
        if (menu->SelectablePatterns() == m_patterns) {
            Game::ContainerClickResult ignored;
            menu->ClickMenuButton(index, true, ignored);
        }
        Client::Sounds::PlayUI("ui.loom.select_pattern", 1.0f);
        QueueContainerButtonClick(menu->containerId, static_cast<uint32_t>(index));
        return true;
    }

    void LoomScreen::HandleExtraRelease() {
        m_scrolling = false;
    }

    void LoomScreen::OnExtraMouseMove(int /*leftPos*/, int topPos) {
        // MC mouseDragged while the scroller is held.
        const int offscreenRows = TotalRowCount() - PATTERN_ROWS;
        if (!m_scrolling || !m_displayPatterns || offscreenRows <= 0) return;
        const float yscr = static_cast<float>(topPos + SCROLLER_Y);
        const float yscr2 = yscr + static_cast<float>(SCROLLER_FULL_HEIGHT);
        m_scrollOffs = (MouseGui().y - yscr - 7.5f) / ((yscr2 - yscr) - 15.0f);
        m_scrollOffs = std::clamp(m_scrollOffs, 0.0f, 1.0f);
        m_startRow = std::max(static_cast<int>(static_cast<double>(m_scrollOffs * static_cast<float>(offscreenRows)) + 0.5), 0);
    }

    bool LoomScreen::HandleExtraScroll(double dy) {
        // MC mouseScrolled.
        const int offscreenRows = TotalRowCount() - PATTERN_ROWS;
        if (m_displayPatterns && offscreenRows > 0) {
            const float scrolledDelta = static_cast<float>(dy) / static_cast<float>(offscreenRows);
            m_scrollOffs = std::clamp(m_scrollOffs - scrolledDelta, 0.0f, 1.0f);
            m_startRow = std::max(static_cast<int>(m_scrollOffs * static_cast<float>(offscreenRows) + 0.5f), 0);
        }
        return true;
    }

} // namespace Render
