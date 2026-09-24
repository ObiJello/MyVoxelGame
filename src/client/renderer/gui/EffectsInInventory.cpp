// File: src/client/renderer/gui/EffectsInInventory.cpp
#include "EffectsInInventory.hpp"
#include "GuiGraphics.hpp"
#include "InventoryScreen.hpp"
#include "CreativeModeInventoryScreen.hpp"

#include <algorithm>

namespace Render {

    namespace EffectsInInventory {

        namespace {
            constexpr const char* kBackground        = "container/inventory/effect_background";
            constexpr const char* kBackgroundAmbient = "container/inventory/effect_background_ambient";
            constexpr int kFontHeight = 9;   // Font.lineHeight

            // MC ComponentRenderUtils.clipText: the longest prefix that fits
            // with the "..." ellipsis appended.
            std::string ClipText(GuiGraphics& g, const std::string& text, int maxWidth) {
                static const std::string kEllipsis = "...";
                const int ellipsisWidth = g.GetStringWidth(kEllipsis);
                std::string out = text;
                while (!out.empty() && g.GetStringWidth(out) + ellipsisWidth > maxWidth) {
                    // Drop a whole UTF-8 code point.
                    size_t cut = out.size() - 1;
                    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
                    out.erase(cut);
                }
                return out + kEllipsis;
            }

            // The screen tooltip (Screen.renderTooltip's colours, as
            // AbstractContainerScreen::RenderTooltip draws them).
            void DrawTooltip(GuiGraphics& g, const std::string& line0, const std::string& line1,
                             int mx, int my) {
                constexpr int kLineH = 10;
                const int textW = std::max(g.GetStringWidth(line0), g.GetStringWidth(line1));
                const int totalH = 2 * kLineH - 2;
                const int x = mx + 12;
                const int y = my - 12;
                const uint32_t bg     = 0xF0100010;
                const uint32_t border = 0x505000FF;
                g.NextStratum();
                g.Fill(x - 3, y - 4,          x + textW + 3, y - 3,          bg);
                g.Fill(x - 3, y + totalH + 3, x + textW + 3, y + totalH + 4, bg);
                g.Fill(x - 3, y - 3,          x + textW + 3, y + totalH + 3, bg);
                g.Fill(x - 4, y - 3,          x - 3,         y + totalH + 3, bg);
                g.Fill(x + textW + 3, y - 3,  x + textW + 4, y + totalH + 3, bg);
                g.Fill(x - 3,         y - 2, x - 2,         y + totalH + 2, border);
                g.Fill(x + textW + 2, y - 2, x + textW + 3, y + totalH + 2, border);
                g.NextStratum();
                g.DrawString(line0, x, y, 0xFFFFFFFF, true);
                g.DrawString(line1, x, y + kLineH, 0xFFFFFFFF, true);
            }
        } // namespace

        std::string EffectSprite(Game::MobEffectId id) {
            return std::string("mob_effect/") + Game::GetEffectName(id);
        }

        bool CanSeeEffects(int guiWidth, int leftPos, int imageWidth) {
            const int xo = leftPos + imageWidth + 2;
            const int availableWidth = guiWidth - xo;
            return availableWidth >= kSpriteSquareSize;
        }

        void Render(GuiGraphics& g, const std::vector<Game::MobEffectInstance>& effects,
                    int guiWidth, int leftPos, int topPos, int imageWidth,
                    int mouseX, int mouseY) {
            const int xo = leftPos + imageWidth + 2;
            const int availableWidth = guiWidth - xo;
            if (effects.empty() || availableWidth < kSpriteSquareSize) return;
            const int maxWidth = availableWidth >= 120 ? availableWidth - kSpacing : kSpriteSquareSize;
            int yStep = 33;
            if (effects.size() > 5) yStep = 132 / (static_cast<int>(effects.size()) - 1);

            // Ordering.natural().sortedCopy — ascending compareTo.
            std::vector<const Game::MobEffectInstance*> sorted;
            sorted.reserve(effects.size());
            for (const auto& e : effects) sorted.push_back(&e);
            std::stable_sort(sorted.begin(), sorted.end(),
                             [](const Game::MobEffectInstance* a, const Game::MobEffectInstance* b) {
                                 return a->CompareTo(*b) < 0;
                             });

            std::string tooltip0, tooltip1;
            int y0 = topPos;
            for (const Game::MobEffectInstance* effect : sorted) {
                const std::string name = Game::GetEffectInstanceDisplayName(*effect);
                const std::string duration = Game::FormatEffectDuration(*effect, 20.0f);

                // extractBackground.
                const int nameWidth = kTextXOffset + g.GetStringWidth(name) + kSpacing;
                const int durationWidth = kTextXOffset + g.GetStringWidth(duration) + kSpacing;
                const int textureWidth = std::min(maxWidth, std::max(nameWidth, durationWidth));
                g.BlitSprite(effect->ambient ? kBackgroundAmbient : kBackground,
                             xo, y0, textureWidth, 32);

                // extractText.
                const int textX = xo + kTextXOffset;
                const int textY = y0 + 7;
                const int maxTextWidth = textureWidth - kTextXOffset - kSpacing;
                bool isCompact;
                if (maxTextWidth > 0) {
                    const bool shouldClip = g.GetStringWidth(name) > maxTextWidth;
                    g.DrawString(shouldClip ? ClipText(g, name, maxTextWidth) : name,
                                 textX, textY, 0xFFFFFFFF, true);
                    g.DrawString(duration, textX, textY + kFontHeight, 0xFF808080, true);   // -8355712
                    isCompact = shouldClip;
                } else {
                    isCompact = true;
                }
                if (isCompact && mouseX >= xo && mouseX <= xo + textureWidth &&
                    mouseY >= y0 && mouseY <= y0 + yStep) {
                    tooltip0 = name;
                    tooltip1 = duration;
                }

                g.BlitSprite(EffectSprite(effect->effect), xo + 7, y0 + 7, 18, 18);
                y0 += yStep;
            }
            if (!tooltip0.empty()) DrawTooltip(g, tooltip0, tooltip1, mouseX, mouseY);
        }

    } // namespace EffectsInInventory

    bool InventoryShowsActiveEffects(int guiWidth) {
        // MC InventoryScreen / CreativeModeInventoryScreen.showsActiveEffects
        // → effects.canSeeEffects(). No other container screen has the column.
        InventoryScreen& survival = GetSurvivalInventoryScreen();
        if (survival.IsOpen()) {
            return EffectsInInventory::CanSeeEffects(guiWidth, survival.LeftPos(guiWidth),
                                                     survival.PanelWidth());
        }
        CreativeModeInventoryScreen& creative = GetCreativeInventoryScreen();
        if (creative.IsOpen()) {
            return EffectsInInventory::CanSeeEffects(guiWidth, creative.LeftPos(guiWidth),
                                                     creative.PanelWidth());
        }
        return false;
    }

} // namespace Render
