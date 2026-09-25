// File: src/client/renderer/gui/EnchantmentScreen.cpp
#include "EnchantmentScreen.hpp"
#include "GuiGraphics.hpp"
#include "screens/Screen.hpp"          // LoadStandaloneGuiTexture
#include "screens/BookScreens.hpp"     // QueueContainerButtonClick
#include "client/entity/Player.hpp"
#include "common/inventory/SystemMenus.hpp"
#include "common/world/enchantment/Enchantment.hpp"
#include "common/text/Language.hpp"
#include "common/text/TextComponent.hpp"

#include <algorithm>
#include <cmath>
#include <string>

// Forward decl: lives in PlatformMain.cpp (the same pattern SignRenderer uses
// for its own copy of the font).
namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    namespace {
        // MC EnchantmentNames.words, in order (the index is what the random
        // picks).
        constexpr const char* kWords[] = {
            "the", "elder", "scrolls", "klaatu", "berata", "niktu", "xyzzy", "bless", "curse",
            "light", "darkness", "fire", "air", "earth", "water", "hot", "dry", "cold", "wet",
            "ignite", "snuff", "embiggen", "twist", "shorten", "stretch", "fiddle", "destroy",
            "imbue", "galvanize", "enchant", "free", "limited", "range", "of", "towards",
            "inside", "sphere", "cube", "self", "other", "ball", "mental", "physical", "grow",
            "shrink", "demon", "elemental", "spirit", "animal", "creature", "beast", "humanoid",
            "undead", "fresh", "stale", "phnglui", "mglwnafh", "cthulhu", "rlyeh", "wgahnagl",
            "fhtagn", "baguette",
        };
        constexpr int kWordCount = static_cast<int>(sizeof(kWords) / sizeof(kWords[0]));

        // MC EnchantmentScreen's colours (ARGB ints in the decompile).
        constexpr uint32_t kNameColor          = 0xFF685E4Au;   // -9937334
        constexpr uint32_t kNameHoverColor     = 0xFFFFFF80u;   // -128
        constexpr uint32_t kCostColor          = 0xFF80FF20u;   // -8323296
        constexpr uint32_t kCostDisabledColor  = 0xFF407F10u;   // -12550384
        // ChatFormatting colours for the tooltip.
        constexpr uint32_t kWhite = 0xFFFFFFFFu;
        constexpr uint32_t kGray  = 0xFFAAAAAAu;
        constexpr uint32_t kRed   = 0xFFFF5555u;

        const char* kEnabledLevelSprites[3] = {
            "container/enchanting_table/level_1", "container/enchanting_table/level_2",
            "container/enchanting_table/level_3" };
        const char* kDisabledLevelSprites[3] = {
            "container/enchanting_table/level_1_disabled", "container/enchanting_table/level_2_disabled",
            "container/enchanting_table/level_3_disabled" };
        constexpr const char* kSlotSprite            = "container/enchanting_table/enchantment_slot";
        constexpr const char* kSlotDisabledSprite    = "container/enchanting_table/enchantment_slot_disabled";
        constexpr const char* kSlotHighlightedSprite = "container/enchanting_table/enchantment_slot_highlighted";

        // A translation with one %s argument, split around it so the
        // argument can keep its own colour.
        void SplitAroundArgument(const std::string& pattern, std::string& before, std::string& after) {
            size_t at = pattern.find("%s");
            size_t len = 2;
            if (at == std::string::npos) {
                at = pattern.find("%1$s");
                len = 4;
            }
            if (at == std::string::npos) {
                before = pattern;
                after.clear();
                return;
            }
            before = pattern.substr(0, at);
            after = pattern.substr(at + len);
        }
    }

    EnchantmentScreen& GetEnchantmentScreen() {
        static EnchantmentScreen s;
        return s;
    }

    void EnchantmentScreen::Configure(const std::string& /*title*/) {
        // EnchantingTableBlock's menu title: the table's custom name, else
        // "container.enchant" — the block's registry name (what the server
        // sends for every block menu) is not what the table shows.
        m_title = Game::Language::GetOrDefault("container.enchant", "Enchant");
    }

    Game::EnchantmentMenu* EnchantmentScreen::Enchanting() const {
        return dynamic_cast<Game::EnchantmentMenu*>(Menu());
    }

    TextureHandle EnchantmentScreen::EnsureBackground() {
        if (m_backgroundTried) return m_background;
        m_backgroundTried = true;
        int w = 0, h = 0;
        m_background = LoadStandaloneGuiTexture("assets/textures/gui/container/enchanting_table.png", w, h);
        return m_background;
    }

    bool EnchantmentScreen::EnsureGalacticFont() {
        // MC's minecraft:alt font — ascii_sga.png on the ascii grid.
        if (!m_galacticTried) {
            m_galacticTried = true;
            m_galacticReady = m_galactic.Initialize(
                PlatformMain::GetAssetPath("assets/textures/font/ascii_sga.png"));
        }
        return m_galacticReady;
    }

    bool EnchantmentScreen::HasInfiniteMaterials() const {
        const Game::ClientPlayer* player = Player();
        return player && player->IsCreative();
    }

    int EnchantmentScreen::PlayerLevel() const {
        const Game::ClientPlayer* player = Player();
        return player ? player->xpLevel : 0;
    }

    void EnchantmentScreen::ContainerTick() {
        // The client's clickMenuButton answers with the local player's level
        // and hasInfiniteMaterials.
        if (Game::EnchantmentMenu* menu = Enchanting()) {
            menu->SetPlayerState(PlayerLevel(), HasInfiniteMaterials());
        }
    }

    int EnchantmentScreen::GalacticAdvance(unsigned char c) const {
        if (c == ' ') return 4;
        return m_galactic.GetCharWidth(c) + 1;
    }

    std::string EnchantmentScreen::RandomName(int maxWidth) {
        // getRandomName: nextInt(2) + 3 words, each Util.getRandom(words).
        std::string name;
        const int wordCount = m_nameRandom.NextInt(2) + 3;
        for (int i = 0; i < wordCount; ++i) {
            if (i != 0) name += ' ';
            name += kWords[m_nameRandom.NextInt(kWordCount)];
        }
        // StringSplitter.headByWidth: the longest prefix that fits.
        int width = 0;
        size_t n = 0;
        while (n < name.size()) {
            const int advance = GalacticAdvance(static_cast<unsigned char>(name[n]));
            if (width + advance > maxWidth) break;
            width += advance;
            ++n;
        }
        return name.substr(0, n);
    }

    void EnchantmentScreen::DrawGalactic(GuiGraphics& g, const std::string& text, int x, int y,
                                         uint32_t color) {
        // One quad per glyph off the ascii_sga grid (8 px cells on a 128 px
        // sheet), no shadow — textWithWordWrap(..., false).
        const TextureHandle tex = m_galactic.GetFontTexture();
        if (tex == INVALID_TEXTURE) return;
        constexpr float kSheet = static_cast<float>(FontRenderer::GLYPH_GRID * FontRenderer::GLYPH_CELL_SIZE);
        int cursor = x;
        for (const char ch : text) {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (c != ' ') {
                const int w = m_galactic.GetCharWidth(c);
                if (w > 0) {
                    const float u0 = static_cast<float>((c % FontRenderer::GLYPH_GRID) * FontRenderer::GLYPH_CELL_SIZE) / kSheet;
                    const float v0 = static_cast<float>((c / FontRenderer::GLYPH_GRID) * FontRenderer::GLYPH_CELL_SIZE) / kSheet;
                    g.Blit(tex, cursor, y, cursor + w, y + FontRenderer::GLYPH_CELL_SIZE,
                           u0, v0, u0 + static_cast<float>(w) / kSheet,
                           v0 + static_cast<float>(FontRenderer::GLYPH_CELL_SIZE) / kSheet, color);
                }
            }
            cursor += GalacticAdvance(c);
        }
    }

    bool EnchantmentScreen::RowHovered(int row, int leftPos, int topPos, int height) const {
        const glm::vec2 mouse = MouseGui();
        const int xx = static_cast<int>(std::floor(mouse.x)) - (leftPos + ROW_X);
        const int yy = static_cast<int>(std::floor(mouse.y)) - (topPos + ROW_Y + ROW_STEP * row);
        return xx >= 0 && yy >= 0 && xx < ROW_W && yy < height;
    }

    void EnchantmentScreen::RenderBg(GuiGraphics& g, int leftPos, int topPos) {
        // MC EnchantmentScreen.extractBackground.
        const TextureHandle bg = EnsureBackground();
        if (bg == INVALID_TEXTURE) {
            g.Fill(leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H, 0xC0202020);
        } else {
            g.Blit(bg, leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H,
                   0.0f, 0.0f, static_cast<float>(IMAGE_W) / 256.0f, static_cast<float>(IMAGE_H) / 256.0f);
        }

        Game::EnchantmentMenu* menu = Enchanting();
        if (!menu) return;
        const bool galactic = EnsureGalacticFont();
        // EnchantmentNames.initSeed(menu.getEnchantmentSeed()) — every frame,
        // so the same offers always read the same words.
        m_nameRandom.SetSeed(static_cast<int64_t>(menu->EnchantmentSeed()));
        const Game::ItemStack& lapis = menu->GetSlot(Game::EnchantmentMenu::SLOT_LAPIS).GetItem();
        const int goldCount = lapis.IsEmpty() ? 0 : lapis.count;
        const bool infinite = HasInfiniteMaterials();
        const int level = PlayerLevel();

        for (int i = 0; i < 3; ++i) {
            const int rowX = leftPos + ROW_X;
            const int rowY = topPos + ROW_Y + ROW_STEP * i;
            const int textX = rowX + 20;
            const int cost = menu->GetData(Game::EnchantmentMenu::DATA_COST_0 + i);
            if (cost == 0) {
                g.BlitSprite(kSlotDisabledSprite, rowX, rowY, ROW_W, ROW_H);
                continue;
            }
            const std::string costText = std::to_string(cost);
            const int textWidth = 86 - g.GetStringWidth(costText);
            const std::string message = RandomName(textWidth);
            uint32_t color = kNameColor;
            if ((goldCount < i + 1 || level < cost) && !infinite) {
                g.BlitSprite(kSlotDisabledSprite, rowX, rowY, ROW_W, ROW_H);
                g.BlitSprite(kDisabledLevelSprites[i], rowX + 1, rowY + 1, 16, 16);
                // ARGB.opaque((col & 0xFEFEFE) >> 1): the name at half brightness.
                if (galactic) DrawGalactic(g, message, textX, rowY + 2, 0xFF000000u | ((kNameColor & 0xFEFEFEu) >> 1));
                color = kCostDisabledColor;
            } else {
                if (RowHovered(i, leftPos, topPos, ROW_H)) {
                    g.BlitSprite(kSlotHighlightedSprite, rowX, rowY, ROW_W, ROW_H);
                    color = kNameHoverColor;
                } else {
                    g.BlitSprite(kSlotSprite, rowX, rowY, ROW_W, ROW_H);
                }
                g.BlitSprite(kEnabledLevelSprites[i], rowX + 1, rowY + 1, 16, 16);
                if (galactic) DrawGalactic(g, message, textX, rowY + 2, color);
                color = kCostColor;
            }
            g.DrawString(costText, textX + 86 - g.GetStringWidth(costText), rowY + 2 + 7, color, true);
        }
    }

    void EnchantmentScreen::RenderLabels(GuiGraphics& g, int leftPos, int topPos) {
        g.DrawString(m_title, leftPos + TITLE_X, topPos + TITLE_Y, LABEL_COLOR, false);
        g.DrawString(Game::Language::GetOrDefault("container.inventory", "Inventory"),
                     leftPos + INV_LABEL_X, topPos + INV_LABEL_Y, LABEL_COLOR, false);
    }

    void EnchantmentScreen::RenderExtras(GuiGraphics& g, int leftPos, int topPos) {
        // MC EnchantmentScreen.extractRenderState's tooltip: the clue, then
        // (outside creative) what the offer needs.
        Game::EnchantmentMenu* menu = Enchanting();
        if (!menu) return;
        const bool infinite = HasInfiniteMaterials();
        const Game::ItemStack& lapis = menu->GetSlot(Game::EnchantmentMenu::SLOT_LAPIS).GetItem();
        const int gold = lapis.IsEmpty() ? 0 : lapis.count;
        for (int i = 0; i < 3; ++i) {
            const int minLevel = menu->GetData(Game::EnchantmentMenu::DATA_COST_0 + i);
            const int clueId = menu->GetData(Game::EnchantmentMenu::DATA_CLUE_ID_0 + i);
            const int enchantLevel = menu->GetData(Game::EnchantmentMenu::DATA_CLUE_LVL_0 + i);
            if (clueId < 0 || clueId >= static_cast<int>(Game::EnchantmentRegistry::All().size())) continue;
            if (!RowHovered(i, leftPos, topPos, ROW_TOOLTIP_H) || minLevel <= 0 || enchantLevel < 0) continue;

            const int cost = i + 1;
            std::vector<Line> lines;
            // "container.enchant.clue" (WHITE) around Enchantment.getFullname,
            // which keeps its own grey / red.
            {
                const Game::Enchantment::FormattedLine name = Game::Enchantment::GetFullname(
                    Game::EnchantmentRegistry::Get(static_cast<Game::EnchantmentId>(clueId)), enchantLevel);
                std::string before, after;
                SplitAroundArgument(Game::Language::GetOrDefault("container.enchant.clue", "%s . . . ?"),
                                    before, after);
                Line clue;
                if (!before.empty()) clue.push_back({before, kWhite});
                clue.push_back({name.text, name.colorARGB});
                if (!after.empty()) clue.push_back({after, kWhite});
                lines.push_back(std::move(clue));
            }
            if (!infinite) {
                lines.push_back({});
                if (PlayerLevel() < minLevel) {
                    lines.push_back({{Game::Text::GetString(Game::Text::Component::Translatable(
                                          "container.enchant.level.requirement",
                                          {Game::Text::Component::Literal(std::to_string(minLevel))})),
                                      kRed}});
                } else {
                    const std::string lapisCost = cost == 1
                        ? Game::Language::Get("container.enchant.lapis.one")
                        : Game::Text::GetString(Game::Text::Component::Translatable(
                              "container.enchant.lapis.many",
                              {Game::Text::Component::Literal(std::to_string(cost))}));
                    lines.push_back({{lapisCost, gold >= cost ? kGray : kRed}});
                    const std::string levelCost = cost == 1
                        ? Game::Language::Get("container.enchant.level.one")
                        : Game::Text::GetString(Game::Text::Component::Translatable(
                              "container.enchant.level.many",
                              {Game::Text::Component::Literal(std::to_string(cost))}));
                    lines.push_back({{levelCost, kGray}});
                }
            }
            const glm::vec2 mouse = MouseGui();
            RenderSegmentTooltip(g, lines, static_cast<int>(mouse.x), static_cast<int>(mouse.y));
            break;
        }
    }

    void EnchantmentScreen::RenderSegmentTooltip(GuiGraphics& g, const std::vector<Line>& lines,
                                                 int mx, int my) {
        // The box every tooltip here draws (AbstractContainerScreen::
        // RenderTooltip): 10 px a line, 2 more under the first, placed right
        // of the mouse or flipped left when it would leave the screen.
        if (lines.empty()) return;
        auto lineWidth = [&g](const Line& line) {
            int w = 0;
            for (const Segment& s : line) w += g.GetStringWidth(s.first);
            return w;
        };
        int textW = 0;
        for (const Line& line : lines) textW = std::max(textW, lineWidth(line));
        const int n = static_cast<int>(lines.size());
        const int totalH = 8 + (n - 1) * 10 + (n > 1 ? 2 : 0);
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
        int lineY = y;
        for (int i = 0; i < n; ++i) {
            int cursor = x;
            for (const Segment& s : lines[static_cast<size_t>(i)]) {
                g.DrawString(s.first, cursor, lineY, s.second, true);
                // The engine's width drops the trailing one-pixel gap, which
                // the next segment's first glyph needs back.
                cursor += g.GetStringWidth(s.first) + (s.first.empty() ? 0 : 1);
            }
            lineY += 10 + (i == 0 ? 2 : 0);
        }
    }

    int EnchantmentScreen::HitTestExtras(int lx, int ly) {
        for (int i = 0; i < 3; ++i) {
            const int xx = lx - ROW_X;
            const int yy = ly - (ROW_Y + ROW_STEP * i);
            if (xx >= 0 && yy >= 0 && xx < ROW_W && yy < ROW_H) return HIT_ROW_0 - i;
        }
        return HIT_NONE;
    }

    bool EnchantmentScreen::HandleExtraClick(int hit, int glfwButton, bool /*shift*/) {
        if (hit > HIT_ROW_0 || hit < HIT_ROW_0 - 2) return false;
        // MC mouseClicked: any button over a row; the client's
        // clickMenuButton decides whether it is worth sending (it never
        // enchants here), then handleInventoryButtonClick.
        (void)glfwButton;
        Game::EnchantmentMenu* menu = Enchanting();
        if (!menu) return true;
        const int row = HIT_ROW_0 - hit;
        menu->SetPlayerState(PlayerLevel(), HasInfiniteMaterials());
        Game::ContainerClickResult ignored;
        if (menu->ClickMenuButton(row, true, ignored)) {
            QueueContainerButtonClick(menu->containerId, static_cast<uint32_t>(row));
        }
        return true;
    }

} // namespace Render
