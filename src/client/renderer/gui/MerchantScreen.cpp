// File: src/client/renderer/gui/MerchantScreen.cpp
#include "MerchantScreen.hpp"
#include "GuiGraphics.hpp"
#include "screens/Screen.hpp"          // LoadStandaloneGuiTexture
#include "screens/BookScreens.hpp"     // QueueContainerButtonClick
#include "client/sound/ClientSounds.hpp"
#include "common/entity/npc/VillagerData.hpp"
#include "common/inventory/MerchantMenu.hpp"
#include "common/text/Language.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <string>

namespace Render {

    namespace {
        std::deque<Network::SelectTradeC2SPacket> s_selectTrades;
        // A reroll was pressed; the next offers for the open menu are its
        // answer (the selection goes back to the first trade).
        bool s_rerollPending = false;

        // MC Language "merchant.title" is "%s - %s".
        std::string MerchantTitle(const std::string& name, int level) {
            std::string pattern = Game::Language::GetOrDefault("merchant.title", "%s - %s");
            const std::string levelName = Game::MerchantLevelName(level);
            std::string out;
            int arg = 0;
            for (size_t i = 0; i < pattern.size(); ++i) {
                if (pattern[i] == '%' && i + 1 < pattern.size() && pattern[i + 1] == 's') {
                    out += (arg++ == 0) ? name : levelName;
                    ++i;
                } else {
                    out += pattern[i];
                }
            }
            return out;
        }
    }

    MerchantScreen& GetMerchantScreen() {
        static MerchantScreen s;
        return s;
    }

    bool ConsumeSelectTrade(Network::SelectTradeC2SPacket& out) {
        if (s_selectTrades.empty()) return false;
        out = s_selectTrades.front();
        s_selectTrades.pop_front();
        return true;
    }

    void ApplyMerchantOffers(const Network::MerchantOffersS2CPacket& packet) {
        // MC handleMerchantOffers: only into the open merchant menu the
        // packet names.
        auto* menu = dynamic_cast<Game::MerchantMenu*>(PlayerContainerMenu());
        if (!menu || menu->containerId != packet.containerId) return;
        menu->SetOffers(packet.offers);
        menu->SetXp(packet.villagerXp);
        menu->SetMerchantLevel(packet.villagerLevel);
        menu->SetShowProgressBar(packet.showProgress);
        menu->SetCanRestock(packet.canRestock);
        if (s_rerollPending) {
            s_rerollPending = false;
            GetMerchantScreen().ResetTradeSelection();
        }
    }

    void MerchantScreen::Configure(const std::string& title) {
        m_title = title.empty() ? Game::Language::GetOrDefault("entity.minecraft.villager", "Villager") : title;
    }

    void MerchantScreen::OnOpen() {
        m_shopItem = 0;
        m_scrollOff = 0;
        m_isDragging = false;
        s_rerollPending = false;
    }

    void MerchantScreen::ResetTradeSelection() {
        m_shopItem = 0;
        m_scrollOff = 0;
        m_isDragging = false;
        // Mirror the server's MerchantMenu::ClickMenuButton, which re-selects
        // trade 0 over the new list (the payments stay where they are).
        if (Game::MerchantMenu* menu = Merchant()) menu->SetSelectionHint(0);
    }

    MerchantScreen::RerollState MerchantScreen::GetRerollState() const {
        // Villagers only: the wandering trader has no XP bar and no level
        // (MC showProgressBar false, merchant level 0); nitwits and the
        // unemployed never open the screen. The lock is MC ResetProfession's
        // test — getVillagerXp() == 0 && level <= 1 — read from the offers
        // packet (and the client's predicted XP, so it locks the moment a
        // trade is taken).
        Game::MerchantMenu* menu = Merchant();
        if (!menu || !menu->ShowProgressBar() || menu->GetTraderLevel() <= 0 || menu->GetOffers().empty()) {
            return RerollState::Hidden;
        }
        if (menu->GetTraderXp() > 0 || menu->GetTraderLevel() > 1) return RerollState::Locked;
        return RerollState::Available;
    }

    bool MerchantScreen::RerollHovered(int leftPos, int topPos) const {
        const int mx = static_cast<int>(std::floor(MouseGui().x)) - leftPos;
        const int my = static_cast<int>(std::floor(MouseGui().y)) - topPos;
        return mx >= REROLL_BUTTON_X && mx < REROLL_BUTTON_X + REROLL_BUTTON_SIZE &&
               my >= REROLL_BUTTON_Y && my < REROLL_BUTTON_Y + REROLL_BUTTON_SIZE;
    }

    Game::MerchantMenu* MerchantScreen::Merchant() const {
        return dynamic_cast<Game::MerchantMenu*>(PlayerContainerMenu());
    }

    TextureHandle MerchantScreen::EnsureBackground() {
        if (m_backgroundTried) return m_background;
        m_backgroundTried = true;
        int w = 0, h = 0;
        m_background = LoadStandaloneGuiTexture("assets/textures/gui/container/villager.png", w, h);
        return m_background;
    }

    int MerchantScreen::VisibleButtonCount() const {
        // MC: `button.visible = button.index < offers.size()`.
        Game::MerchantMenu* menu = Merchant();
        if (!menu) return 0;
        return std::min(NUMBER_OF_OFFER_BUTTONS, static_cast<int>(menu->GetOffers().size()));
    }

    // ─── Drawing ─────────────────────────────────────────────────

    void MerchantScreen::RenderBg(GuiGraphics& g, int leftPos, int topPos) {
        const TextureHandle bg = EnsureBackground();
        if (bg == INVALID_TEXTURE) {
            g.Fill(leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H, 0xC0202020);
        } else {
            // villager.png is a 512×256 sheet.
            g.Blit(bg, leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H,
                   0.0f, 0.0f, static_cast<float>(IMAGE_W) / 512.0f, static_cast<float>(IMAGE_H) / 256.0f);
        }
        Game::MerchantMenu* menu = Merchant();
        if (!menu) return;
        Game::MerchantOffers& offers = menu->GetOffers();
        if (!offers.empty() && m_shopItem >= 0 && m_shopItem < static_cast<int>(offers.size()) &&
            offers[static_cast<size_t>(m_shopItem)].IsOutOfStock()) {
            g.BlitSprite("container/villager/out_of_stock", leftPos + 83 + 99, topPos + 35, 28, 21);
        }

        // The trade buttons (MC TradeOfferButton, a Button.Plain with no
        // text): drawn over the panel, under the offer items.
        g.NextStratum();
        const int mx = static_cast<int>(std::floor(MouseGui().x));
        const int my = static_cast<int>(std::floor(MouseGui().y));
        const int visible = VisibleButtonCount();
        for (int i = 0; i < visible; ++i) {
            const int bx = leftPos + TRADE_BUTTON_X;
            const int by = topPos + 16 + 2 + i * TRADE_BUTTON_H;
            const bool hovered = mx >= bx && mx < bx + TRADE_BUTTON_W && my >= by && my < by + TRADE_BUTTON_H;
            g.BlitSprite(hovered ? "widget/button_highlighted" : "widget/button",
                         bx, by, TRADE_BUTTON_W, TRADE_BUTTON_H);
        }

        // The reroll button's frame (MC Button sprites: normal / highlighted
        // / disabled); its icon goes on in RenderExtras, a stratum above.
        const RerollState reroll = GetRerollState();
        if (reroll != RerollState::Hidden) {
            const char* frame = reroll == RerollState::Locked ? "widget/button_disabled"
                              : RerollHovered(leftPos, topPos) ? "widget/button_highlighted"
                              : "widget/button";
            g.BlitSprite(frame, leftPos + REROLL_BUTTON_X, topPos + REROLL_BUTTON_Y,
                         REROLL_BUTTON_SIZE, REROLL_BUTTON_SIZE);
        }
    }

    void MerchantScreen::RenderExtraSlots(GuiGraphics& g, int leftPos, int topPos) {
        // MC extractContents: the offers' items (fakeItem).
        Game::MerchantMenu* menu = Merchant();
        if (!menu) return;
        Game::MerchantOffers& offers = menu->GetOffers();
        if (offers.empty()) return;
        int offerY = topPos + 16 + 1;
        const int sellItem1X = leftPos + 5 + 5;
        const bool scroll = CanScroll(static_cast<int>(offers.size()));
        for (int index = 0; index < static_cast<int>(offers.size()); ++index) {
            if (scroll && (index < m_scrollOff || index >= NUMBER_OF_OFFER_BUTTONS + m_scrollOff)) continue;
            const Game::MerchantOffer& offer = offers[static_cast<size_t>(index)];
            const int decorHeight = offerY + 2;
            g.RenderItem(offer.GetCostA(), sellItem1X, decorHeight);
            const Game::ItemStack costB = offer.GetCostB();
            if (!costB.IsEmpty()) g.RenderItem(costB, leftPos + 5 + 35, decorHeight);
            g.RenderItem(offer.GetResult(), leftPos + 5 + 68, decorHeight);
            offerY += 20;
        }
    }

    void MerchantScreen::RenderLabels(GuiGraphics& g, int leftPos, int topPos) {
        Game::MerchantMenu* menu = Merchant();
        const int traderLevel = menu ? menu->GetTraderLevel() : 0;
        std::string title = m_title;
        if (menu && traderLevel > 0 && traderLevel <= 5 && menu->ShowProgressBar()) {
            title = MerchantTitle(m_title, traderLevel);
        }
        const int titleW = g.GetStringWidth(title);
        g.DrawString(title, leftPos + 49 + IMAGE_W / 2 - titleW / 2, topPos + 6, LABEL_COLOR, false);
        g.DrawString(Game::Language::GetOrDefault("container.inventory", "Inventory"),
                     leftPos + INVENTORY_LABEL_X, topPos + IMAGE_H - 94, LABEL_COLOR, false);
        const std::string trades = Game::Language::GetOrDefault("merchant.trades", "Trades");
        const int tradesW = g.GetStringWidth(trades);
        g.DrawString(trades, leftPos + 5 - tradesW / 2 + 48, topPos + 6, LABEL_COLOR, false);
    }

    void MerchantScreen::RenderExtras(GuiGraphics& g, int leftPos, int topPos) {
        if (!Merchant()) return;
        RenderRerollIcon(g, leftPos, topPos);
        RenderOfferExtras(g, leftPos, topPos);
        RenderRerollTooltip(g, leftPos, topPos);
    }

    void MerchantScreen::RenderRerollIcon(GuiGraphics& g, int leftPos, int topPos) {
        // container/villager/reroll*.png (tools/gen_villager_reroll_icons.py):
        // 16×16, centred on the 20×20 frame RenderBg drew.
        const RerollState reroll = GetRerollState();
        if (reroll == RerollState::Hidden) return;
        const char* icon = reroll == RerollState::Locked ? "container/villager/reroll_locked"
                         : RerollHovered(leftPos, topPos) ? "container/villager/reroll_highlighted"
                         : "container/villager/reroll";
        g.BlitSprite(icon, leftPos + REROLL_BUTTON_X + 2, topPos + REROLL_BUTTON_Y + 2, 16, 16);
    }

    void MerchantScreen::RenderRerollTooltip(GuiGraphics& g, int leftPos, int topPos) {
        const RerollState reroll = GetRerollState();
        if (reroll == RerollState::Hidden || !RerollHovered(leftPos, topPos) || !Carried().IsEmpty()) return;
        const int mx = static_cast<int>(std::floor(MouseGui().x));
        const int my = static_cast<int>(std::floor(MouseGui().y));
        g.NextStratum();
        if (reroll == RerollState::Available) {
            RenderTextTooltip(g, Game::Language::GetOrDefault("merchant.reroll", "Reroll trades"), mx, my);
        } else {
            // ChatFormatting.GRAY (0xAAAAAA) for the reason line.
            RenderTextTooltip(g, {
                { Game::Language::GetOrDefault("merchant.reroll.locked", "Trades are locked"), 0xFFFFFFFF },
                { Game::Language::GetOrDefault("merchant.reroll.locked.reason", "This villager has traded"),
                  0xFFAAAAAA },
            }, mx, my);
        }
    }

    void MerchantScreen::RenderOfferExtras(GuiGraphics& g, int leftPos, int topPos) {
        Game::MerchantMenu* menu = Merchant();
        if (!menu) return;
        Game::MerchantOffers& offers = menu->GetOffers();
        if (offers.empty()) return;

        RenderScroller(g, leftPos, topPos);

        // Per-offer decorations: counts, the discount strike-through, the
        // arrow (MC extractAndDecorateCostA / extractButtonArrows).
        int offerY = topPos + 16 + 1;
        const int sellItem1X = leftPos + 5 + 5;
        const bool scroll = CanScroll(static_cast<int>(offers.size()));
        for (int index = 0; index < static_cast<int>(offers.size()); ++index) {
            if (scroll && (index < m_scrollOff || index >= NUMBER_OF_OFFER_BUTTONS + m_scrollOff)) continue;
            const Game::MerchantOffer& offer = offers[static_cast<size_t>(index)];
            const int decorHeight = offerY + 2;
            const Game::ItemStack baseCostA = offer.GetBaseCostA();
            const Game::ItemStack costA = offer.GetCostA();
            if (baseCostA.count == costA.count) {
                g.RenderItemDecorations(costA, sellItem1X, decorHeight);
            } else {
                // itemDecorations(font, stack, x, y, text) with "1" forced for
                // a single item, so both prices always show.
                const auto countAt = [&g](int count, int x, int y) {
                    const std::string text = std::to_string(count);
                    g.DrawString(text, x + 17 - g.GetStringWidth(text), y + 9, 0xFFFFFFFF, true);
                };
                countAt(baseCostA.count, sellItem1X, decorHeight);
                countAt(costA.count, sellItem1X + 14, decorHeight);
                g.BlitSprite("container/villager/discount_strikethrough", sellItem1X + 7, decorHeight + 12, 9, 2);
            }
            const Game::ItemStack costB = offer.GetCostB();
            if (!costB.IsEmpty()) g.RenderItemDecorations(costB, leftPos + 5 + 35, decorHeight);
            g.BlitSprite(offer.IsOutOfStock() ? "container/villager/trade_arrow_out_of_stock"
                                              : "container/villager/trade_arrow",
                         leftPos + 5 + 35 + 20, decorHeight + 3, 10, 9);
            g.RenderItemDecorations(offer.GetResult(), leftPos + 5 + 68, decorHeight);
            offerY += 20;
        }

        if (m_shopItem < 0 || m_shopItem >= static_cast<int>(offers.size())) return;
        const Game::MerchantOffer& selected = offers[static_cast<size_t>(m_shopItem)];
        if (menu->ShowProgressBar()) RenderProgressBar(g, leftPos, topPos);

        // MC: the out-of-stock tooltip over the red cross.
        const int mx = static_cast<int>(std::floor(MouseGui().x));
        const int my = static_cast<int>(std::floor(MouseGui().y));
        const int lx = mx - leftPos, ly = my - topPos;
        if (selected.IsOutOfStock() && menu->CanRestock() &&
            lx >= 186 - 1 && lx < 186 + 22 + 1 && ly >= 35 - 1 && ly < 35 + 21 + 1 &&
            Carried().IsEmpty()) {
            g.NextStratum();
            RenderTextTooltip(g, Game::Language::GetOrDefault("merchant.deprecated",
                                                              "Villagers restock up to two times per day."),
                              mx, my);
        }
    }

    void MerchantScreen::RenderProgressBar(GuiGraphics& g, int xo, int yo) {
        // MC extractProgressBar.
        Game::MerchantMenu* menu = Merchant();
        if (!menu) return;
        const int traderLevel = menu->GetTraderLevel();
        const int traderXp = menu->GetTraderXp();
        if (traderLevel >= 5) return;
        g.BlitSprite("container/villager/experience_bar_background", xo + 136, yo + 16, 102, 5);
        const int minXp = Game::VillagerData::GetMinXpPerLevel(traderLevel);
        if (traderXp < minXp || !Game::VillagerData::CanLevelUp(traderLevel)) return;
        const float multiplier = 102.0f / static_cast<float>(Game::VillagerData::GetMaxXpPerLevel(traderLevel) - minXp);
        const int w = std::min(static_cast<int>(std::floor(multiplier * static_cast<float>(traderXp - minXp))), 102);
        if (w > 0) {
            g.BlitSprite("container/villager/experience_bar_current", 102, 5, 0, 0, xo + 136, yo + 16, w, 5);
        }
        const int futureXp = menu->GetFutureTraderXp();
        if (futureXp > 0) {
            const int futureW = std::min(static_cast<int>(std::floor(static_cast<float>(futureXp) * multiplier)), 102 - w);
            if (futureW > 0) {
                g.BlitSprite("container/villager/experience_bar_result", 102, 5, w, 0, xo + 136 + w, yo + 16, futureW, 5);
            }
        }
    }

    void MerchantScreen::RenderScroller(GuiGraphics& g, int xo, int yo) {
        // MC extractScroller.
        Game::MerchantMenu* menu = Merchant();
        if (!menu) return;
        const int steps = static_cast<int>(menu->GetOffers().size()) + 1 - NUMBER_OF_OFFER_BUTTONS;
        if (steps > 1) {
            const int leftOver = SCROLL_BAR_H - (SCROLLER_H + (steps - 1) * SCROLL_BAR_H / steps);
            const int stepHeight = 1 + leftOver / steps + SCROLL_BAR_H / steps;
            int scrollerYOff = std::min(113, m_scrollOff * stepHeight);
            if (m_scrollOff == steps - 1) scrollerYOff = 113;
            g.BlitSprite("container/villager/scroller", xo + SCROLL_BAR_START_X,
                         yo + SCROLL_BAR_TOP_Y + scrollerYOff, SCROLLER_W, SCROLLER_H);
        } else {
            g.BlitSprite("container/villager/scroller_disabled", xo + SCROLL_BAR_START_X,
                         yo + SCROLL_BAR_TOP_Y, SCROLLER_W, SCROLLER_H);
        }
    }

    void MerchantScreen::RenderTextTooltip(GuiGraphics& g, const std::string& text, int mx, int my) {
        RenderTextTooltip(g, { { text, 0xFFFFFFFF } }, mx, my);
    }

    void MerchantScreen::RenderTextTooltip(GuiGraphics& g,
                                           const std::vector<std::pair<std::string, uint32_t>>& lines,
                                           int mx, int my) {
        // Same box as AbstractContainerScreen::RenderTooltip.
        if (lines.empty()) return;
        int textW = 0;
        for (const auto& line : lines) textW = std::max(textW, g.GetStringWidth(line.first));
        // MC ClientTextTooltip: 10 px a line, 2 more under the first.
        const int n = static_cast<int>(lines.size());
        const int totalH = 8 + (n - 1) * 10 + (n > 1 ? 2 : 0);
        // MC DefaultTooltipPositioner: right of the mouse, or left of it
        // when that would leave the screen.
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
            g.DrawString(lines[static_cast<size_t>(i)].first, x, lineY, lines[static_cast<size_t>(i)].second, true);
            lineY += 10 + (i == 0 ? 2 : 0);
        }
    }

    // ─── Input ───────────────────────────────────────────────────

    int MerchantScreen::HitTestExtras(int lx, int ly) {
        Game::MerchantMenu* menu = Merchant();
        if (!menu) return HIT_NONE;
        if (GetRerollState() != RerollState::Hidden &&
            lx >= REROLL_BUTTON_X && lx < REROLL_BUTTON_X + REROLL_BUTTON_SIZE &&
            ly >= REROLL_BUTTON_Y && ly < REROLL_BUTTON_Y + REROLL_BUTTON_SIZE) {
            return HIT_REROLL;
        }
        const int offerCount = static_cast<int>(menu->GetOffers().size());
        // The scroller track (MC mouseClicked's bounds).
        if (CanScroll(offerCount) && lx > SCROLL_BAR_START_X && lx < SCROLL_BAR_START_X + SCROLLER_W &&
            ly > SCROLL_BAR_TOP_Y && ly <= SCROLL_BAR_TOP_Y + SCROLL_BAR_H + 1) {
            return HIT_SCROLLER;
        }
        const int visible = VisibleButtonCount();
        if (lx >= TRADE_BUTTON_X && lx < TRADE_BUTTON_X + TRADE_BUTTON_W) {
            for (int i = 0; i < visible; ++i) {
                const int by = 16 + 2 + i * TRADE_BUTTON_H;
                if (ly >= by && ly < by + TRADE_BUTTON_H) return HIT_TRADE_BUTTON_0 - i;
            }
        }
        return HIT_NONE;
    }

    bool MerchantScreen::HandleExtraClick(int hit, int glfwButton, bool shift) {
        (void)shift;
        if (hit == HIT_SCROLLER) {
            m_isDragging = true;
            return true;
        }
        if (hit == HIT_REROLL) {
            // An AbstractButton: left button only, and an inactive (locked)
            // one neither clicks nor sounds.
            if (glfwButton != 0 || GetRerollState() != RerollState::Available) return true;
            Game::MerchantMenu* menu = Merchant();
            if (!menu) return true;
            Client::Sounds::PlayButtonClick();   // AbstractWidget.playDownSound
            s_rerollPending = true;
            QueueContainerButtonClick(menu->containerId,
                                      static_cast<uint32_t>(Game::MerchantMenu::BUTTON_REROLL_TRADES));
            return true;
        }
        if (hit <= HIT_TRADE_BUTTON_0 && hit > HIT_TRADE_BUTTON_0 - NUMBER_OF_OFFER_BUTTONS) {
            // A Button fires on the left button only.
            if (glfwButton != 0) return true;
            m_shopItem = (HIT_TRADE_BUTTON_0 - hit) + m_scrollOff;
            PostButtonClick();
            return true;
        }
        return false;
    }

    void MerchantScreen::PostButtonClick() {
        // MC postButtonClick: predict, then tell the server.
        Game::MerchantMenu* menu = Merchant();
        if (!menu) return;
        menu->SetSelectionHint(m_shopItem);
        Game::ContainerClickResult ignored;
        menu->TryMoveItems(m_shopItem, ignored);
        Network::SelectTradeC2SPacket packet;
        packet.item = m_shopItem;
        s_selectTrades.push_back(packet);
    }

    bool MerchantScreen::HandleExtraScroll(double dy) {
        Game::MerchantMenu* menu = Merchant();
        if (!menu) return true;
        const int numberOfOffers = static_cast<int>(menu->GetOffers().size());
        if (CanScroll(numberOfOffers)) {
            const int maxScrollOff = numberOfOffers - NUMBER_OF_OFFER_BUTTONS;
            m_scrollOff = std::clamp(static_cast<int>(static_cast<double>(m_scrollOff) - dy), 0, maxScrollOff);
        }
        return true;
    }

    void MerchantScreen::OnExtraMouseMove(int leftPos, int topPos) {
        m_lastMouseLx = static_cast<int>(std::floor(MouseGui().x)) - leftPos;
        if (!m_isDragging) return;
        Game::MerchantMenu* menu = Merchant();
        if (!menu) return;
        // MC mouseDragged.
        const int numberOfOffers = static_cast<int>(menu->GetOffers().size());
        const int fullScrollTopPos = topPos + SCROLL_BAR_TOP_Y;
        const int fullScrollBottomPos = fullScrollTopPos + SCROLL_BAR_H;
        const int maxScrollOff = numberOfOffers - NUMBER_OF_OFFER_BUTTONS;
        if (maxScrollOff <= 0) return;
        float scrolling = (MouseGui().y - static_cast<float>(fullScrollTopPos) - 13.5f) /
                          (static_cast<float>(fullScrollBottomPos - fullScrollTopPos) - 27.0f);
        scrolling = scrolling * static_cast<float>(maxScrollOff) + 0.5f;
        m_scrollOff = std::clamp(static_cast<int>(scrolling), 0, maxScrollOff);
    }

    const Game::ItemStack* MerchantScreen::HoveredExtraStack() const {
        // MC TradeOfferButton.extractToolTip: cost A over the first 20
        // pixels, cost B between 30 and 50, the result past 65.
        Game::MerchantMenu* menu = Merchant();
        if (!menu) return nullptr;
        const int hit = HoveredSlot();
        if (!(hit <= HIT_TRADE_BUTTON_0 && hit > HIT_TRADE_BUTTON_0 - NUMBER_OF_OFFER_BUTTONS)) return nullptr;
        const int index = (HIT_TRADE_BUTTON_0 - hit) + m_scrollOff;
        const Game::MerchantOffers& offers = menu->GetOffers();
        if (index < 0 || index >= static_cast<int>(offers.size())) return nullptr;
        const Game::MerchantOffer& offer = offers[static_cast<size_t>(index)];
        // Panel-relative, like the button's x (5).
        const int xm = m_lastMouseLx;
        const int bx = TRADE_BUTTON_X;
        if (xm < bx + 20) {
            m_tooltipStack = offer.GetCostA();
        } else if (xm < bx + 50 && xm > bx + 30) {
            m_tooltipStack = offer.GetCostB();
            if (m_tooltipStack.IsEmpty()) return nullptr;
        } else if (xm > bx + 65) {
            m_tooltipStack = offer.GetResult();
        } else {
            return nullptr;
        }
        return &m_tooltipStack;
    }

} // namespace Render
